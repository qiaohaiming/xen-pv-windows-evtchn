/*
PV Drivers for Windows Xen HVM Domains
Copyright (C) 2007 James Harper

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "xenpci.h"

#if defined(__MINGW32__)
  #define xchg(p1, p2) InterlockedExchange((xen_long_t * volatile)p1, p2)
  /* rest implemented in mingw_extras.c */
#elif defined(_X86_)
  #define xchg(p1, p2) InterlockedExchange(p1, p2)
  #define synch_clear_bit(p1, p2) InterlockedBitTestAndReset(p2, p1)
  #define synch_set_bit(p1, p2) InterlockedBitTestAndSet(p2, p1)
  #define bit_scan_forward(p1, p2) _BitScanForward(p1, p2)
#else
  #define xchg(p1, p2) InterlockedExchange64(p1, p2)
  #define synch_clear_bit(p1, p2) InterlockedBitTestAndReset64(p2, p1)
  #define synch_set_bit(p1, p2) InterlockedBitTestAndSet64(p2, p1)
  #define bit_scan_forward(p1, p2) _BitScanForward64(p1, p2)
#endif

#define BITS_PER_LONG (sizeof(xen_ulong_t) * 8)
#define BITS_PER_LONG_SHIFT (5 + (sizeof(xen_ulong_t) >> 3))

static DDKAPI VOID
EvtChn_DpcBounce(PRKDPC Dpc, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2)
{
  ev_action_t *action = Context;

  UNREFERENCED_PARAMETER(Dpc);
  UNREFERENCED_PARAMETER(SystemArgument1);
  UNREFERENCED_PARAMETER(SystemArgument2);

  //FUNCTION_ENTER();

  if (action->type != EVT_ACTION_TYPE_EMPTY)
  {
    action->ServiceRoutine(action->ServiceContext);
  }
  //FUNCTION_EXIT();
}

/* Called at DIRQL */
BOOLEAN
EvtChn_AckEvent(PVOID context, evtchn_port_t port, BOOLEAN *last_interrupt)
{
  PXENPCI_DEVICE_DATA xpdd = context;
  ULONG pcpu = KeGetCurrentProcessorNumber() & 0xff;
  ULONG evt_word;
  ULONG evt_bit;
  xen_ulong_t val;
  int i;
  
  evt_bit = port & (BITS_PER_LONG - 1);
  evt_word = port >> BITS_PER_LONG_SHIFT;

  val = synch_clear_bit(evt_bit, (volatile xen_long_t *)&xpdd->evtchn_pending_pvt[pcpu][evt_word]);
  *last_interrupt = TRUE;
  for (i = 0; i < sizeof(xen_ulong_t) * 8; i++)
  {
    if (xpdd->evtchn_pending_pvt[pcpu][i])
    {
      *last_interrupt = FALSE;
      break;
    }
  }
  
  return (BOOLEAN)!!val;
}

volatile ULONG in_inq = 0;

BOOLEAN
EvtChn_EvtInterruptIsr(WDFINTERRUPT interrupt, ULONG message_id)
{
/*
For HVM domains, Xen always triggers the event on CPU0. Because the
interrupt is delivered via the virtual PCI device it might get delivered
to CPU != 0, but we should always use vcpu_info[0]
*/
  int vcpu = 0;
  ULONG pcpu = KeGetCurrentProcessorNumber() & 0xff;
  vcpu_info_t *vcpu_info;
  PXENPCI_DEVICE_DATA xpdd = GetXpdd(WdfInterruptGetDevice(interrupt));
  shared_info_t *shared_info_area = xpdd->shared_info_area;
  xen_ulong_t evt_words;
  unsigned long evt_word;
  unsigned long evt_bit;
  unsigned int port;
  ev_action_t *ev_action;
  BOOLEAN handled = FALSE;
  BOOLEAN deferred = FALSE;
  int i;

  UNREFERENCED_PARAMETER(message_id);

  if (xpdd->interrupts_masked)
  {
    KdPrint((__DRIVER_NAME "     unhandled interrupt\n"));
  }

  if (xpdd->hibernated)
  {
    KdPrint((__DRIVER_NAME "     interrupt while hibernated\n"));
  }

  for (i = 0; i < ARRAY_SIZE(xpdd->evtchn_pending_pvt[pcpu]); i++)
  {
    if (xpdd->evtchn_pending_pvt[pcpu][i])
    {
      KdPrint((__DRIVER_NAME "     Unacknowledged event word = %d, val = %p\n", i, xpdd->evtchn_pending_pvt[pcpu][i]));
      xpdd->evtchn_pending_pvt[pcpu][i] = 0;
    }
  }
  
  vcpu_info = &shared_info_area->vcpu_info[vcpu];

  vcpu_info->evtchn_upcall_pending = 0;

  if (xpdd->interrupts_masked)
  {
    return TRUE;
  }
  
  evt_words = (xen_ulong_t)xchg((volatile xen_long_t *)&vcpu_info->evtchn_pending_sel, 0);

  while (bit_scan_forward(&evt_word, evt_words))
  {
    evt_words &= ~(1 << evt_word);
    while (bit_scan_forward(&evt_bit, shared_info_area->evtchn_pending[evt_word] & ~shared_info_area->evtchn_mask[evt_word]))
    {
      port = (evt_word << BITS_PER_LONG_SHIFT) + evt_bit;
      ev_action = &xpdd->ev_actions[port];
      ev_action->count++;
	  if(ev_action->mask_on_fire) {
		  EvtChn_Mask(xpdd, port);
	  }
      synch_clear_bit(evt_bit, (volatile xen_long_t *)&shared_info_area->evtchn_pending[evt_word]);
      handled = TRUE;
      switch (ev_action->type)
      {
      case EVT_ACTION_TYPE_NORMAL:
        //KdPrint((__DRIVER_NAME "     EVT_ACTION_TYPE_NORMAL\n"));
        ev_action->ServiceRoutine(ev_action->ServiceContext);
        break;
      case EVT_ACTION_TYPE_IRQ:
        //KdPrint((__DRIVER_NAME "     EVT_ACTION_TYPE_IRQ\n"));
        synch_set_bit(evt_bit, (volatile xen_long_t *)&xpdd->evtchn_pending_pvt[pcpu][evt_word]);
        deferred = TRUE;
        break;
      case EVT_ACTION_TYPE_DPC:
        //KdPrint((__DRIVER_NAME "     EVT_ACTION_TYPE_DPC\n"));
        KeInsertQueueDpc(&ev_action->Dpc, NULL, NULL);
        break;
      case EVT_ACTION_TYPE_SUSPEND:
        KdPrint((__DRIVER_NAME "     EVT_ACTION_TYPE_SUSPEND\n"));
        for (i = 0; i < ARRAY_SIZE(xpdd->evtchn_pending_pvt[pcpu]); i++)
        {
          if (xpdd->ev_actions[i].type == EVT_ACTION_TYPE_IRQ)
          {
            int suspend_bit = i & (BITS_PER_LONG - 1);
            int suspend_word = i >> BITS_PER_LONG_SHIFT;
            synch_set_bit(suspend_bit, (volatile xen_long_t *)&xpdd->evtchn_pending_pvt[pcpu][suspend_word]);
          }
          else if (xpdd->ev_actions[i].type == EVT_ACTION_TYPE_NORMAL && xpdd->ev_actions[i].ServiceRoutine)
          {
            xpdd->ev_actions[i].ServiceRoutine(xpdd->ev_actions[i].ServiceContext);
          }
        }
        KeInsertQueueDpc(&ev_action->Dpc, NULL, NULL);
        deferred = TRUE;
        break;
      default:
        KdPrint((__DRIVER_NAME "     Unhandled Event!!!\n"));
        break;
      }
    }
  }

  return handled && !deferred;
}

NTSTATUS
EvtChn_EvtInterruptEnable(WDFINTERRUPT interrupt, WDFDEVICE device)
{
  NTSTATUS status = STATUS_SUCCESS;
  
  UNREFERENCED_PARAMETER(interrupt);
  UNREFERENCED_PARAMETER(device);

  FUNCTION_ENTER();
  FUNCTION_EXIT();

  return status;
}

NTSTATUS
EvtChn_EvtInterruptDisable(WDFINTERRUPT interrupt, WDFDEVICE device)
{
  NTSTATUS status = STATUS_SUCCESS;
  
  UNREFERENCED_PARAMETER(interrupt);
  UNREFERENCED_PARAMETER(device);

  FUNCTION_ENTER();
  FUNCTION_EXIT();

  return status;
}

NTSTATUS
EvtChn_Bind(PVOID Context, evtchn_port_t Port, PXEN_EVTCHN_SERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  FUNCTION_ENTER();
  
  if (xpdd->ev_actions[Port].type != EVT_ACTION_TYPE_EMPTY)
  {
    xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_EMPTY;
    KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
    KdPrint((__DRIVER_NAME " Handler for port %d already registered, replacing\n", Port));
  }

  xpdd->ev_actions[Port].ServiceRoutine = ServiceRoutine;
  xpdd->ev_actions[Port].ServiceContext = ServiceContext;
  xpdd->ev_actions[Port].xpdd = xpdd;
  xpdd->ev_actions[Port].mask_on_fire = FALSE;
  KeMemoryBarrier();
  xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_NORMAL;

  EvtChn_Unmask(Context, Port);

  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_BindDpcReplace(PVOID Context, evtchn_port_t Port, PXEN_EVTCHN_SERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext, BOOLEAN replace, BOOLEAN mask_on_fire)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  FUNCTION_ENTER();

  if (xpdd->ev_actions[Port].type != EVT_ACTION_TYPE_EMPTY)
  {
	  if(replace) {
		xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_EMPTY;
		KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
		KdPrint((__DRIVER_NAME " Handler for port %d already registered, replacing\n", Port));
	  }
	  else {
		  return STATUS_INVALID_PARAMETER;
	  }
  }

  if(mask_on_fire) {
	  KdPrint(("Setting event port %d to mask-on-fire\n", Port));
  }
  xpdd->ev_actions[Port].mask_on_fire = mask_on_fire;
  xpdd->ev_actions[Port].ServiceRoutine = ServiceRoutine;
  xpdd->ev_actions[Port].ServiceContext = ServiceContext;
  xpdd->ev_actions[Port].xpdd = xpdd;
  KeInitializeDpc(&xpdd->ev_actions[Port].Dpc, EvtChn_DpcBounce, &xpdd->ev_actions[Port]);
  KeMemoryBarrier(); // make sure that the new service routine is only called once the context is set up
  xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_DPC;

  EvtChn_Unmask(Context, Port);

  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_BindDpc(PVOID Context, evtchn_port_t Port, PXEN_EVTCHN_SERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext) {

	return EvtChn_BindDpcReplace(Context, Port, ServiceRoutine, ServiceContext, TRUE, FALSE);

}

NTSTATUS
EvtChn_BindIrq(PVOID Context, evtchn_port_t Port, ULONG vector, PCHAR description)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  FUNCTION_ENTER();

  if (xpdd->ev_actions[Port].type != EVT_ACTION_TYPE_EMPTY)
  {
    xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_EMPTY;
    KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
    KdPrint((__DRIVER_NAME " Handler for port %d already registered, replacing\n", Port));
  }

  xpdd->ev_actions[Port].vector = vector;
  xpdd->ev_actions[Port].xpdd = xpdd;
  xpdd->ev_actions[Port].mask_on_fire = FALSE;
  KeMemoryBarrier();
  xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_IRQ;
  strncpy(xpdd->ev_actions[Port].description, description, 128);

  EvtChn_Unmask(Context, Port);

  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Unbind(PVOID Context, evtchn_port_t Port)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  int old_type;
  
  EvtChn_Mask(Context, Port);
  old_type = xpdd->ev_actions[Port].type;
  xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_EMPTY;
  KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
  xpdd->ev_actions[Port].ServiceRoutine = NULL;
  xpdd->ev_actions[Port].ServiceContext = NULL;

  if (old_type == EVT_ACTION_TYPE_DPC)
  {
    KeRemoveQueueDpc(&xpdd->ev_actions[Port].Dpc);
    KeFlushQueuedDpcs();
  }
  
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Mask(PVOID Context, evtchn_port_t port)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  synch_set_bit(port & (BITS_PER_LONG - 1),
    (volatile xen_long_t *)&xpdd->shared_info_area->evtchn_mask[port >> BITS_PER_LONG_SHIFT]);
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Unmask(PVOID context, evtchn_port_t port)
{
  PXENPCI_DEVICE_DATA xpdd = context;

  synch_clear_bit(port & (BITS_PER_LONG - 1),
    (volatile xen_long_t *)&xpdd->shared_info_area->evtchn_mask[port >> BITS_PER_LONG_SHIFT]);
  return STATUS_SUCCESS;
}

/* Un-pends an event channel which was pending.
   Intended to allow userspace evtchn to get more events even if
   the channel fired whilst it was masked.
   Beware race: Must unmask, then reset, which might cause a spurious double-fire. */

NTSTATUS
EvtChn_Reset(PVOID context, evtchn_port_t port) 
{
  PXENPCI_DEVICE_DATA xpdd = context;

  synch_clear_bit(port & (BITS_PER_LONG - 1),
    (volatile xen_long_t *)&xpdd->shared_info_area->evtchn_pending[port >> BITS_PER_LONG_SHIFT]);
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Notify(PVOID Context, evtchn_port_t Port)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  struct evtchn_send send;

  send.port = Port;
  (void)HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_send, &send);
  return STATUS_SUCCESS;
}

evtchn_port_t
EvtChn_AllocIpi(PVOID context, ULONG vcpu)
{
  PXENPCI_DEVICE_DATA xpdd = context;
  evtchn_bind_ipi_t op;
  
  FUNCTION_ENTER();
  op.vcpu = vcpu;
  op.port = 0;
  HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_bind_ipi, &op);
  FUNCTION_EXIT();
  return op.port;
}

evtchn_port_t
EvtChn_AllocUnbound(PVOID Context, domid_t Domain)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  evtchn_alloc_unbound_t op;
  op.dom = DOMID_SELF;
  op.remote_dom = Domain;
  HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_alloc_unbound, &op);
  return op.port;
}

VOID
EvtChn_Close(PVOID Context, evtchn_port_t port )
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  evtchn_close_t op;
  op.port = port;
  HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_close, &op);
  return;
}

VOID
EvtChn_PdoEventChannelDpc(PVOID context)
{
  PXENPCI_DEVICE_DATA xpdd = context;
  
  FUNCTION_ENTER();
  KeSetEvent(&xpdd->pdo_suspend_event, IO_NO_INCREMENT, FALSE);
  FUNCTION_EXIT();
}

NTSTATUS
EvtChn_Init(PXENPCI_DEVICE_DATA xpdd)
{
  ULONGLONG result;
  int i;

  FUNCTION_ENTER();

  for (i = 0; i < NR_EVENTS; i++)
  {
    EvtChn_Mask(xpdd, i);
    xpdd->ev_actions[i].type = EVT_ACTION_TYPE_EMPTY;
    xpdd->ev_actions[i].count = 0;
  }

  for (i = 0; i < 8; i++)
  {
    xpdd->shared_info_area->evtchn_pending[i] = 0;
  }

  for (i = 0; i < MAX_VIRT_CPUS; i++)
  {
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_pending = 0;
    xpdd->shared_info_area->vcpu_info[i].evtchn_pending_sel = 0;
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 1; /* apparantly this doesn't do anything */
  }

  KeMemoryBarrier();

  result = hvm_set_parameter(xpdd, HVM_PARAM_CALLBACK_IRQ, xpdd->irq_number);
  KdPrint((__DRIVER_NAME "     hvm_set_parameter(HVM_PARAM_CALLBACK_IRQ, %d) = %d\n", xpdd->irq_number, (ULONG)result));

  for (i = 0; i < MAX_VIRT_CPUS; i++)
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 0;  
  xpdd->interrupts_masked = FALSE;
  KeMemoryBarrier();

  KeInitializeEvent(&xpdd->pdo_suspend_event, SynchronizationEvent, FALSE);
  xpdd->pdo_event_channel = EvtChn_AllocIpi(xpdd, 0);
  EvtChn_BindDpc(xpdd, xpdd->pdo_event_channel, EvtChn_PdoEventChannelDpc, xpdd);
  xpdd->ev_actions[xpdd->pdo_event_channel].type = EVT_ACTION_TYPE_SUSPEND; /* override dpc type */
  
  KdPrint((__DRIVER_NAME "     pdo_event_channel = %d\n", xpdd->pdo_event_channel));

  FUNCTION_EXIT();
  
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Suspend(PXENPCI_DEVICE_DATA xpdd)
{
  int i;
//  LARGE_INTEGER wait_time;

  xpdd->interrupts_masked = TRUE;
  for (i = 0; i < MAX_VIRT_CPUS; i++)
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 1;
  KeMemoryBarrier();
  hvm_set_parameter(xpdd, HVM_PARAM_CALLBACK_IRQ, 0);

  for (i = 0; i < NR_EVENTS; i++)
  {
    if (xpdd->ev_actions[i].type == EVT_ACTION_TYPE_DPC)
    {
      KeRemoveQueueDpc(&xpdd->ev_actions[i].Dpc);
    }
  }
  KeFlushQueuedDpcs();

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Resume(PXENPCI_DEVICE_DATA xpdd)
{
  return EvtChn_Init(xpdd);
}