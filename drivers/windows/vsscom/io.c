//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vsscom: reads, writes and the serial control requests.
//
// The rules (what a timeout combination means, what an event history keeps)
// are in core/vsscom_core.c, which the unit tests cover on the host. This file
// carries them out against Windows requests.
//
// Without a hub handle the port is a loopback: bytes written come back from the
// next read, and a write that does not fit in the receive ring completes with
// the bytes that did. With a hub handle open (hub.c) the application's writes go
// to the hub and its reads come from the hub, with flow control both ways, and
// the input lines are the ones the hub sets.
//
// Locks and completion: Lock guards the context and is never held while a
// request is completed or while another queue is called.
//

#include "vsscom.h"

// -- Time ------------------------------------------------------------------

#define VSSCOM_100NS_PER_MS 10000ULL
// A timer fires on or after its due time; this much early still counts as due.
#define VSSCOM_TIMER_SLACK (1ULL * VSSCOM_100NS_PER_MS)

static ULONGLONG VssComNow(VOID)
{
    return VssComInterruptTime();
}

static ULONGLONG VssComDeadline(ULONG Milliseconds)
{
    return VssComNow() + (ULONGLONG)Milliseconds * VSSCOM_100NS_PER_MS;
}

static VOID VssComStartTimer(WDFTIMER Timer, ULONGLONG Deadline)
{
    const ULONGLONG now = VssComNow();
    const ULONGLONG left = Deadline > now ? Deadline - now : 1;
    WdfTimerStart(Timer, -(LONGLONG)left);
}

// -- Small request helpers -------------------------------------------------

// True when the request came from an application handle that was revoked.
BOOLEAN VssComRequestRevoked(_In_ WDFREQUEST Request)
{
    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    return file != NULL && VssComFileContext(file)->Revoked ? TRUE : FALSE;
}

VOID VssComCompleteWithData(_In_ WDFREQUEST Request,
                            _In_reads_bytes_(Length) const VOID* Data,
                            _In_ size_t Length)
{
    PVOID output = NULL;
    NTSTATUS status =
        WdfRequestRetrieveOutputBuffer(Request, Length, &output, NULL);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }
    RtlCopyMemory(output, Data, Length);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Length);
}

BOOLEAN VssComFetchInput(_In_ WDFREQUEST Request,
                         _Out_writes_bytes_(Length) PVOID Data,
                         _In_ size_t Length)
{
    PVOID input = NULL;
    if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, Length, &input, NULL)))
        return FALSE;
    RtlCopyMemory(Data, input, Length);
    return TRUE;
}

// -- Line errors -----------------------------------------------------------

vsscom_flow VssComFlowLocked(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    vsscom_flow flow;
    flow.control_handshake = Context->HandFlow.ControlHandShake;
    flow.flow_replace = Context->HandFlow.FlowReplace;
    flow.xon_limit = (vsscom_u32)Context->HandFlow.XonLimit;
    flow.xoff_limit = (vsscom_u32)Context->HandFlow.XoffLimit;
    return flow;
}

BOOLEAN VssComErrorAbort(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    BOOLEAN blocked;

    VssComLockAcquire(Context->Lock);
    {
        const vsscom_flow flow = VssComFlowLocked(Context);
        blocked = vsscom_errors_abort(&flow, Context->Errors) != 0;
    }
    VssComLockRelease(Context->Lock);
    return blocked;
}

// -- Waiting reads ---------------------------------------------------------

static VOID VssComStopReadTimers(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    WdfTimerStop(Context->ReadTotalTimer, FALSE);
    WdfTimerStop(Context->ReadIntervalTimer, FALSE);
}

// Starts or stops the two timers to match the read at the head of the queue.
static VOID VssComArmReadTimers(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                                _In_ PVSSCOM_READ_CONTEXT Read)
{
    ULONGLONG total;
    ULONGLONG interval;

    VssComLockAcquire(Context->Lock);
    total = Read->TotalDeadline;
    interval = Read->IntervalDeadline;
    VssComLockRelease(Context->Lock);

    if (total != 0)
        VssComStartTimer(Context->ReadTotalTimer, total);
    else
        WdfTimerStop(Context->ReadTotalTimer, FALSE);

    if (interval != 0)
        VssComStartTimer(Context->ReadIntervalTimer, interval);
    else
        WdfTimerStop(Context->ReadIntervalTimer, FALSE);
}

static VOID VssComFinishRead(_In_ WDFREQUEST Request, _In_ NTSTATUS Status)
{
    PVSSCOM_READ_CONTEXT read = VssComReadContext(Request);
    WdfRequestCompleteWithInformation(Request, Status, read->Received);
}

// Serves the read at the head of the queue from the receive ring. A read that
// is complete leaves, and the next one gets its turn; one that still waits goes
// back to the head with its timers set.
static VOID VssComServiceOnce(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                              _Inout_ BOOLEAN* Moved)
{
    for (;;)
    {
        WDFREQUEST request;
        PVSSCOM_READ_CONTEXT read;
        BOOLEAN done;
        BOOLEAN abort;
        BOOLEAN timed_out;
        ULONGLONG now;

        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->ReadQueue,
                                                      &request)))
        {
            VssComLockAcquire(Context->Lock);
            Context->AbortHead = FALSE;
            VssComLockRelease(Context->Lock);
            VssComStopReadTimers(Context);
            return;
        }

        read = VssComReadContext(request);
        now = VssComNow();

        // A read of a handle that was revoked ends here, wherever it was when the
        // revoke ran: parked in the queue, or in this function's hands.
        if (VssComRequestRevoked(request))
        {
            VssComStopReadTimers(Context);
            VssComFinishRead(request, STATUS_DEVICE_REMOVED);
            continue;
        }

        VssComLockAcquire(Context->Lock);
        if (!read->Started)
        {
            read->Plan = vsscom_plan_read(&Context->Timeouts, read->Requested);
            read->Started = TRUE;
            if (read->Plan.has_total_timer)
                read->TotalDeadline = VssComDeadline(read->Plan.total_ms);
        }

        abort = Context->AbortHead;
        Context->AbortHead = FALSE;
        done = FALSE;
        timed_out = FALSE;
        {
            // Abort on error ends every read that waits, not only the first,
            // and holds until ClearCommError: each one that reaches the head
            // finds the error still latched.
            const vsscom_flow flow = VssComFlowLocked(Context);
            if (vsscom_errors_abort(&flow, Context->Errors))
                abort = TRUE;
        }

        if (!abort)
        {
            const ULONG got =
                vsscom_ring_read(&Context->Rx, read->Buffer + read->Received,
                                 read->Requested - read->Received);
            read->Received += got;
            if (got != 0)
                *Moved = TRUE;

            done = read->Plan.return_now ||
                   vsscom_read_satisfied(&read->Plan, read->Requested,
                                         read->Received);

            // The gap allowed between bytes restarts with each one that
            // arrives. MAXDWORD here means no gap limit.
            if (!done && got > 0 && read->Plan.interval_ms != 0 &&
                read->Plan.interval_ms != VSSCOM_MAXDWORD)
            {
                read->IntervalDeadline = VssComDeadline(read->Plan.interval_ms);
            }

            // A timer that fires calls this function, and a timer stopped too
            // late can call it early, so the deadlines decide.
            if (!done)
            {
                timed_out =
                    (read->TotalDeadline != 0 &&
                     now + VSSCOM_TIMER_SLACK >= read->TotalDeadline) ||
                    (read->IntervalDeadline != 0 &&
                     now + VSSCOM_TIMER_SLACK >= read->IntervalDeadline);
            }
        }
        VssComLockRelease(Context->Lock);

        if (abort)
        {
            VssComStopReadTimers(Context);
            VssComFinishRead(request, STATUS_CANCELLED);
            continue;
        }
        if (done)
        {
            VssComStopReadTimers(Context);
            VssComFinishRead(request, STATUS_SUCCESS);
            continue;
        }
        if (timed_out)
        {
            // STATUS_TIMEOUT with the bytes read so far: Win32 reports a
            // successful short read.
            VssComStopReadTimers(Context);
            VssComFinishRead(request, STATUS_TIMEOUT);
            continue;
        }

        VssComArmReadTimers(Context, read);
        WdfRequestRequeue(request);
        return;
    }
}

// Runs VssComServiceOnce on one thread at a time, and again if something
// changed while it ran. Two threads at the head of the queue would hand the
// reads out of order.
static BOOLEAN VssComServiceReads(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    BOOLEAN again;
    BOOLEAN moved = FALSE;

    VssComLockAcquire(Context->Lock);
    if (Context->ServiceBusy)
    {
        Context->ServiceAgain = TRUE;
        VssComLockRelease(Context->Lock);
        return FALSE;
    }
    Context->ServiceBusy = TRUE;
    VssComLockRelease(Context->Lock);

    do
    {
        VssComLockAcquire(Context->Lock);
        Context->ServiceAgain = FALSE;
        VssComLockRelease(Context->Lock);

        VssComServiceOnce(Context, &moved);

        VssComLockAcquire(Context->Lock);
        again = Context->ServiceAgain;
        if (!again)
            Context->ServiceBusy = FALSE;
        VssComLockRelease(Context->Lock);
    } while (again);
    return moved;
}

// Moves bytes between the two ends and serves every request that can go on.
// One thread does it at a time, and again if something changed meanwhile.
// Reading from Rx makes room for the hub's writes, and the hub reading Tx makes
// room for the application's, so the four steps repeat until none moves bytes.
VOID VssComPump(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    BOOLEAN again;

    VssComLockAcquire(Context->Lock);
    if (Context->PumpBusy)
    {
        Context->PumpAgain = TRUE;
        VssComLockRelease(Context->Lock);
        return;
    }
    Context->PumpBusy = TRUE;
    VssComLockRelease(Context->Lock);

    do
    {
        BOOLEAN moved;

        VssComLockAcquire(Context->Lock);
        Context->PumpAgain = FALSE;
        VssComLockRelease(Context->Lock);

        do
        {
            moved = FALSE;
            if (VssComServiceHubWrites(Context))
                moved = TRUE;
            if (VssComServiceReads(Context))
                moved = TRUE;
            if (VssComServiceHubReads(Context))
                moved = TRUE;
            if (VssComServiceAppWrites(Context))
                moved = TRUE;
        } while (moved);

        // The buffers changed: a line that follows the receive buffer may move.
        VssComApplyFlow(Context, FALSE);

        VssComLockAcquire(Context->Lock);
        again = Context->PumpAgain;
        if (!again)
            Context->PumpBusy = FALSE;
        VssComLockRelease(Context->Lock);
    } while (again);
}

// A timer fired: the head read may be due.
VOID VssComEvtReadTotalTimer(_In_ WDFTIMER Timer)
{
    VssComPump(VssComGetContext(WdfTimerGetParentObject(Timer)));
}

VOID VssComEvtReadIntervalTimer(_In_ WDFTIMER Timer)
{
    VssComPump(VssComGetContext(WdfTimerGetParentObject(Timer)));
}

// The framework calls this for a read that was cancelled while it waited.
VOID VssComEvtReadCanceled(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request)
{
    PVSSCOM_DEVICE_CONTEXT context = VssComGetContext(WdfIoQueueGetDevice(Queue));

    VssComFinishRead(Request, STATUS_CANCELLED);
    VssComPump(context);
}

// -- Events ----------------------------------------------------------------

// Completes the pending WaitCommEvent request, if there is one, with `Events`.
static VOID VssComFinishWait(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                             _In_ ULONG Events)
{
    WDFREQUEST request;

    if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->WaitQueue, &request)))
        return;

    VssComLockAcquire(Context->Lock);
    Context->WaitQueued = FALSE;
    VssComLockRelease(Context->Lock);

    VssComCompleteWithData(request, &Events, sizeof(Events));
}

// Ends the waiting WaitCommEvent request, if there is one, with `Status`.
VOID VssComFinishWaitWith(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                          _In_ NTSTATUS Status)
{
    WDFREQUEST request;

    if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->WaitQueue, &request)))
        return;

    VssComLockAcquire(Context->Lock);
    Context->WaitQueued = FALSE;
    Context->Events.waiting = 0;
    VssComLockRelease(Context->Lock);

    WdfRequestComplete(request, Status);
}

// Something happened that an application can wait for.
VOID VssComRaise(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ ULONG Events)
{
    vsscom_u32 fired = 0;
    BOOLEAN complete;

    VssComLockAcquire(Context->Lock);
    complete = vsscom_events_raise(&Context->Events, Events, &fired);
    VssComLockRelease(Context->Lock);

    if (complete)
        VssComFinishWait(Context, fired);
}

VOID VssComEvtWaitCanceled(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request)
{
    PVSSCOM_DEVICE_CONTEXT context = VssComGetContext(WdfIoQueueGetDevice(Queue));

    VssComLockAcquire(context->Lock);
    context->Events.waiting = 0;
    context->WaitQueued = FALSE;
    VssComLockRelease(context->Lock);

    WdfRequestComplete(Request, STATUS_CANCELLED);
}

// Takes the input lines from where they come from now: the hub's lines while a
// hub handle is open, otherwise the loopback of DTR and RTS. Raises what
// changed.
VOID VssComApplyLines(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    ULONG events;

    VssComLockAcquire(Context->Lock);
    {
        const vsscom_u32 levels =
            Context->HubOpen
                ? (Context->HubLines & VSSCOM_LINE_MASK)
                : vsscom_loopback_lines(Context->Dtr != FALSE,
                                        Context->Rts != FALSE);
        events = vsscom_modem_events(Context->ModemLevels, levels);
        Context->ModemDeltas |= vsscom_modem_deltas(Context->ModemLevels, levels);
        Context->ModemLevels = levels;
    }
    VssComLockRelease(Context->Lock);

    if (events != 0)
        VssComRaise(Context, events);
}

// The application set DTR and RTS.
static VOID VssComUpdateLines(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                              _In_ BOOLEAN Dtr, _In_ BOOLEAN Rts)
{
    BOOLEAN changed;

    VssComLockAcquire(Context->Lock);
    changed = (Context->Dtr != Dtr) || (Context->Rts != Rts);
    Context->Dtr = Dtr;
    Context->Rts = Rts;
    VssComLockRelease(Context->Lock);

    VssComApplyLines(Context);
    if (changed)
        VssComBumpState(Context);
}

// -- Flow control ----------------------------------------------------------

VOID VssComApplyFlow(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ BOOLEAN Set)
{
    vsscom_flow flow;
    vsscom_flow_lines now;
    vsscom_flow_lines next;
    ULONGLONG time = VssComNow();
    ULONGLONG busy;
    BOOLEAN changed;
    BOOLEAN hold;
    ULONG rx;
    ULONG tx;

    VssComLockAcquire(Context->Lock);
    flow.control_handshake = Context->HandFlow.ControlHandShake;
    flow.flow_replace = Context->HandFlow.FlowReplace;
    flow.xon_limit = (vsscom_u32)Context->HandFlow.XonLimit;
    flow.xoff_limit = (vsscom_u32)Context->HandFlow.XoffLimit;
    now.dtr = Context->Dtr ? 1 : 0;
    now.rts = Context->Rts ? 1 : 0;
    rx = vsscom_ring_count(&Context->Rx);
    tx = vsscom_ring_count(&Context->Tx);
    busy = Context->TxBusyUntil;

    next = vsscom_flow_update(&flow, now, rx, VSSCOM_RX_SIZE,
                              vsscom_tx_sending(tx, time, busy),
                              Set ? 1 : 0);
    changed = next.dtr != now.dtr || next.rts != now.rts;
    Context->Dtr = next.dtr != 0;
    Context->Rts = next.rts != 0;
    // The bytes have all gone to the hub, but the last may not have left yet: RTS
    // stays up until its time is over.
    hold = (flow.flow_replace & VSSCOM_RTS_MASK) == VSSCOM_RTS_TOGGLE &&
           tx == 0 && time < busy;
    VssComLockRelease(Context->Lock);

    if (changed)
    {
        VssComApplyLines(Context);
        VssComBumpState(Context);
    }
    if (hold)
    {
        // Restarting a timer that is queued does not move its due time.
        WdfTimerStop(Context->FlowTimer, FALSE);
        VssComStartTimer(Context->FlowTimer, busy);
    }
}

VOID VssComFlowTransmit(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ ULONG Bytes)
{
    const ULONGLONG time = VssComNow();
    BOOLEAN toggle;

    VssComLockAcquire(Context->Lock);
    toggle = (Context->HandFlow.FlowReplace & VSSCOM_RTS_MASK) ==
             VSSCOM_RTS_TOGGLE;
    if (toggle)
    {
        const vsscom_u64 duration = vsscom_tx_time(
            Context->BaudRate, Context->LineControl.WordLength,
            Context->LineControl.Parity, Context->LineControl.StopBits, Bytes);
        Context->TxBusyUntil =
            vsscom_tx_busy_until(time, Context->TxBusyUntil, duration);
    }
    VssComLockRelease(Context->Lock);

    // RTS goes high before the hub can read the bytes, so a hub that gates on it
    // sees it first.
    if (toggle)
        VssComApplyFlow(Context, FALSE);
}

VOID VssComEvtFlowTimer(_In_ WDFTIMER Timer)
{
    VssComApplyFlow(VssComGetContext(WdfTimerGetParentObject(Timer)), FALSE);
}

// -- Reset on close --------------------------------------------------------

// Like a UART driver, the port goes back to quiet when its last handle closes:
// lines dropped, nothing buffered, no mask.
VOID VssComResetPort(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    BOOLEAN release;

    VssComLockAcquire(Context->Lock);
    vsscom_ring_clear(&Context->Rx);
    Context->Dtr = FALSE;
    Context->Rts = FALSE;
    // Every open starts with the settings of a port nobody has configured, as it
    // does on a UART: no DTR or RTS control, no handshake.
    RtlZeroMemory(&Context->HandFlow, sizeof(Context->HandFlow));
    Context->TxBusyUntil = 0;
    // With a hub attached its lines stay as they are, as a real device's would.
    Context->ModemLevels = Context->HubOpen ? (Context->HubLines & VSSCOM_LINE_MASK) : 0;
    Context->ModemDeltas = 0;
    Context->Errors = 0;
    release = vsscom_events_set_mask(&Context->Events, 0);
    VssComLockRelease(Context->Lock);

    WdfTimerStop(Context->FlowTimer, FALSE);
    if (release)
        VssComFinishWait(Context, 0);
}

// -- Revoke ----------------------------------------------------------------

// Takes the application handle back: its requests fail from now on, the ones that
// wait end, and the port goes back to quiet so that a new program can open it. The
// old handle stays open in its process, and closing it later changes nothing
// (VssComEvtFileCleanup).
ULONG VssComRevokeApplication(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    WDFFILEOBJECT file;
    ULONG process;
    BOOLEAN was_open;

    VssComLockAcquire(Context->Lock);
    was_open = Context->AppOpen;
    file = Context->AppFile;
    process = Context->AppProcessId;
    if (was_open)
    {
        if (file != NULL)
            VssComFileContext(file)->Revoked = TRUE;
        Context->AppFile = NULL;
        Context->AppProcessId = 0;
        Context->AppOpen = FALSE;
        Context->Break = FALSE;
        Context->RevokeCount++;
    }
    VssComLockRelease(Context->Lock);
    if (!was_open)
        return 0;

    // The wait goes first, so that the reset does not complete it as a success.
    VssComFinishWaitWith(Context, STATUS_DEVICE_REMOVED);
    VssComAbortAppWrites(Context, STATUS_DEVICE_REMOVED);
    VssComResetPort(Context);
    VssComBumpState(Context);
    // The reads of the old handle are in the read queue: the pump ends them.
    VssComPump(Context);
    return process;
}

// -- Read and write --------------------------------------------------------

// Which end sent a request: VSSCOM_FILE_APP, VSSCOM_FILE_HUB, or 0 for a
// request with no file object.
static ULONG VssComRequestKind(_In_ WDFREQUEST Request)
{
    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    PVSSCOM_FILE_CONTEXT context;

    if (file == NULL)
        return 0;
    context = VssComFileContext(file);
    // An application handle that was revoked is not an application handle any more.
    if (context->Kind == VSSCOM_FILE_APP && context->Revoked)
        return VssComFileRevoked;
    return context->Kind;
}

static VOID VssComEvtIoRead(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                            _In_ size_t Length)
{
    PVSSCOM_DEVICE_CONTEXT context = VssComGetContext(WdfIoQueueGetDevice(Queue));
    PVSSCOM_READ_CONTEXT read = NULL;
    WDF_OBJECT_ATTRIBUTES attributes;
    PVOID buffer = NULL;
    NTSTATUS status;
    const ULONG kind = VssComRequestKind(Request);

    if (kind == VSSCOM_FILE_HUB)
    {
        VssComHubRead(context, Request, Length);
        return;
    }
    if (kind == VssComFileRevoked)
    {
        WdfRequestComplete(Request, STATUS_DEVICE_REMOVED);
        return;
    }
    if (kind != VSSCOM_FILE_APP)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (VssComErrorAbort(context))
    {
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
    if (Length == 0)
    {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }
    if (Length > VssComMaxUlong)
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(Request, Length, &buffer, NULL);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VSSCOM_READ_CONTEXT);
    status = WdfObjectAllocateContext(Request, &attributes, (PVOID*)&read);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }
    read->Buffer = (PUCHAR)buffer;
    read->Requested = (ULONG)Length;

    status = WdfRequestForwardToIoQueue(Request, context->ReadQueue);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }

    VssComPump(context);
}

static VOID VssComEvtIoWrite(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                             _In_ size_t Length)
{
    PVSSCOM_DEVICE_CONTEXT context = VssComGetContext(WdfIoQueueGetDevice(Queue));
    PVOID buffer = NULL;
    ULONG written;
    NTSTATUS status;
    BOOLEAN hub;
    BOOLEAN flag;
    const ULONG kind = VssComRequestKind(Request);

    if (kind == VSSCOM_FILE_HUB)
    {
        VssComHubWrite(context, Request, Length);
        return;
    }
    if (kind == VssComFileRevoked)
    {
        WdfRequestComplete(Request, STATUS_DEVICE_REMOVED);
        return;
    }
    if (kind != VSSCOM_FILE_APP)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (VssComErrorAbort(context))
    {
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
    if (Length == 0)
    {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }
    if (Length > VssComMaxUlong)
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    // With a hub attached the bytes go to it, and the write waits for room.
    VssComLockAcquire(context->Lock);
    hub = context->HubOpen;
    VssComLockRelease(context->Lock);
    if (hub)
    {
        VssComAppWriteToHub(context, Request, Length);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(Request, Length, &buffer, NULL);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }

    VssComLockAcquire(context->Lock);
    written = vsscom_ring_write(&context->Rx, (const vsscom_u8*)buffer,
                                (vsscom_u32)Length);
    // Looked at before the request completes: the buffer goes with it.
    flag = vsscom_bytes_have((const vsscom_u8*)buffer, written,
                             context->Chars.EventChar) != 0;
    VssComLockRelease(context->Lock);

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, written);

    if (written != 0)
    {
        VssComFlowTransmit(context, written);
        VssComPump(context);
        VssComRaise(context,
                    VSSCOM_EV_RXCHAR | (flag ? VSSCOM_EV_RXFLAG : 0u));
    }
    VssComRaise(context, VSSCOM_EV_TXEMPTY);
}

// -- Control requests ------------------------------------------------------

static VOID VssComProperties(_Out_ PSERIAL_COMMPROP Properties)
{
    RtlZeroMemory(Properties, sizeof(*Properties));
    Properties->PacketLength = (USHORT)sizeof(SERIAL_COMMPROP);
    Properties->PacketVersion = 2;
    Properties->ServiceMask = SERIAL_SP_SERIALCOMM;
    Properties->MaxBaud = SERIAL_BAUD_USER;
    Properties->ProvSubType = SERIAL_SP_RS232;
    Properties->ProvCapabilities =
        SERIAL_PCF_DTRDSR | SERIAL_PCF_RTSCTS | SERIAL_PCF_CD |
        SERIAL_PCF_PARITY_CHECK | SERIAL_PCF_XONXOFF | SERIAL_PCF_SETXCHAR |
        SERIAL_PCF_TOTALTIMEOUTS | SERIAL_PCF_INTTIMEOUTS;
    Properties->SettableParams =
        SERIAL_SP_PARITY | SERIAL_SP_BAUD | SERIAL_SP_DATABITS |
        SERIAL_SP_STOPBITS | SERIAL_SP_HANDSHAKING | SERIAL_SP_PARITY_CHECK |
        SERIAL_SP_CARRIER_DETECT;
    Properties->SettableBaud =
        SERIAL_BAUD_075 | SERIAL_BAUD_110 | SERIAL_BAUD_134_5 |
        SERIAL_BAUD_150 | SERIAL_BAUD_300 | SERIAL_BAUD_600 |
        SERIAL_BAUD_1200 | SERIAL_BAUD_1800 | SERIAL_BAUD_2400 |
        SERIAL_BAUD_4800 | SERIAL_BAUD_7200 | SERIAL_BAUD_9600 |
        SERIAL_BAUD_14400 | SERIAL_BAUD_19200 | SERIAL_BAUD_38400 |
        SERIAL_BAUD_56K | SERIAL_BAUD_57600 | SERIAL_BAUD_115200 |
        SERIAL_BAUD_128K | SERIAL_BAUD_USER;
    Properties->SettableData = SERIAL_DATABITS_5 | SERIAL_DATABITS_6 |
                               SERIAL_DATABITS_7 | SERIAL_DATABITS_8;
    Properties->SettableStopParity =
        SERIAL_STOPBITS_10 | SERIAL_STOPBITS_15 | SERIAL_STOPBITS_20 |
        SERIAL_PARITY_NONE | SERIAL_PARITY_ODD | SERIAL_PARITY_EVEN |
        SERIAL_PARITY_MARK | SERIAL_PARITY_SPACE;
    Properties->MaxTxQueue = VSSCOM_TX_SIZE;
    Properties->MaxRxQueue = VSSCOM_RX_SIZE;
    Properties->CurrentTxQueue = VSSCOM_TX_SIZE;
    Properties->CurrentRxQueue = VSSCOM_RX_SIZE;
}

static VOID VssComEvtIoDeviceControl(_In_ WDFQUEUE Queue,
                                     _In_ WDFREQUEST Request,
                                     _In_ size_t OutputBufferLength,
                                     _In_ size_t InputBufferLength,
                                     _In_ ULONG IoControlCode)
{
    PVSSCOM_DEVICE_CONTEXT context = VssComGetContext(WdfIoQueueGetDevice(Queue));
    const ULONG kind = VssComRequestKind(Request);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode == IOCTL_VSSCOM_GET_VERSION)
    {
        VSSCOM_VERSION_INFO info = {VSSCOM_VERSION, VSSCOM_RX_SIZE};
        VssComCompleteWithData(Request, &info, sizeof(info));
        return;
    }
    if (kind == VSSCOM_FILE_HUB)
    {
        VssComHubIoControl(context, Request, IoControlCode);
        return;
    }
    if (kind == VssComFileRevoked)
    {
        WdfRequestComplete(Request, STATUS_DEVICE_REMOVED);
        return;
    }
    if (kind != VSSCOM_FILE_APP)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    // After an error with abort on error set, only ClearCommError gets through,
    // as in the serial driver sample (SerialCompleteIfError).
    if (IoControlCode != IOCTL_SERIAL_GET_COMMSTATUS && VssComErrorAbort(context))
    {
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }

    switch (IoControlCode)
    {
    case IOCTL_SERIAL_GET_BAUD_RATE:
    {
        SERIAL_BAUD_RATE baud = {0};
        VssComLockAcquire(context->Lock);
        baud.BaudRate = context->BaudRate;
        VssComLockRelease(context->Lock);
        VssComCompleteWithData(Request, &baud, sizeof(baud));
        return;
    }

    case IOCTL_SERIAL_SET_BAUD_RATE:
    {
        SERIAL_BAUD_RATE baud = {0};
        if (!VssComFetchInput(Request, &baud, sizeof(baud)) || baud.BaudRate == 0)
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        VssComLockAcquire(context->Lock);
        context->BaudRate = baud.BaudRate;
        VssComLockRelease(context->Lock);
        VssComBumpState(context);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_GET_LINE_CONTROL:
    {
        SERIAL_LINE_CONTROL control;
        VssComLockAcquire(context->Lock);
        control = context->LineControl;
        VssComLockRelease(context->Lock);
        VssComCompleteWithData(Request, &control, sizeof(control));
        return;
    }

    case IOCTL_SERIAL_SET_LINE_CONTROL:
    {
        SERIAL_LINE_CONTROL control = {0};
        if (!VssComFetchInput(Request, &control, sizeof(control)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        VssComLockAcquire(context->Lock);
        context->LineControl = control;
        VssComLockRelease(context->Lock);
        VssComBumpState(context);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_GET_TIMEOUTS:
    {
        SERIAL_TIMEOUTS timeouts;
        VssComLockAcquire(context->Lock);
        timeouts.ReadIntervalTimeout = context->Timeouts.read_interval;
        timeouts.ReadTotalTimeoutMultiplier =
            context->Timeouts.read_total_multiplier;
        timeouts.ReadTotalTimeoutConstant = context->Timeouts.read_total_constant;
        timeouts.WriteTotalTimeoutMultiplier =
            context->Timeouts.write_total_multiplier;
        timeouts.WriteTotalTimeoutConstant =
            context->Timeouts.write_total_constant;
        VssComLockRelease(context->Lock);
        VssComCompleteWithData(Request, &timeouts, sizeof(timeouts));
        return;
    }

    case IOCTL_SERIAL_SET_TIMEOUTS:
    {
        SERIAL_TIMEOUTS timeouts = {0};
        vsscom_timeouts next;
        if (!VssComFetchInput(Request, &timeouts, sizeof(timeouts)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        next.read_interval = timeouts.ReadIntervalTimeout;
        next.read_total_multiplier = timeouts.ReadTotalTimeoutMultiplier;
        next.read_total_constant = timeouts.ReadTotalTimeoutConstant;
        next.write_total_multiplier = timeouts.WriteTotalTimeoutMultiplier;
        next.write_total_constant = timeouts.WriteTotalTimeoutConstant;
        if (!vsscom_timeouts_valid(&next))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        VssComLockAcquire(context->Lock);
        context->Timeouts = next;
        VssComLockRelease(context->Lock);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_GET_HANDFLOW:
    {
        SERIAL_HANDFLOW handflow;
        VssComLockAcquire(context->Lock);
        handflow = context->HandFlow;
        VssComLockRelease(context->Lock);
        VssComCompleteWithData(Request, &handflow, sizeof(handflow));
        return;
    }

    case IOCTL_SERIAL_SET_HANDFLOW:
    {
        SERIAL_HANDFLOW handflow = {0};
        vsscom_flow flow;
        if (!VssComFetchInput(Request, &handflow, sizeof(handflow)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        flow.control_handshake = handflow.ControlHandShake;
        flow.flow_replace = handflow.FlowReplace;
        flow.xon_limit = (vsscom_u32)handflow.XonLimit;
        flow.xoff_limit = (vsscom_u32)handflow.XoffLimit;
        if (!vsscom_flow_valid(&flow, VSSCOM_RX_SIZE))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        VssComLockAcquire(context->Lock);
        context->HandFlow = handflow;
        VssComLockRelease(context->Lock);
        // Enable and disable move DTR and RTS now; a handshake or a toggle starts to
        // follow the buffers.
        VssComApplyFlow(context, TRUE);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_GET_CHARS:
    {
        SERIAL_CHARS chars;
        VssComLockAcquire(context->Lock);
        chars = context->Chars;
        VssComLockRelease(context->Lock);
        VssComCompleteWithData(Request, &chars, sizeof(chars));
        return;
    }

    case IOCTL_SERIAL_SET_CHARS:
    {
        SERIAL_CHARS chars = {0};
        if (!VssComFetchInput(Request, &chars, sizeof(chars)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        VssComLockAcquire(context->Lock);
        context->Chars = chars;
        VssComLockRelease(context->Lock);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_GET_PROPERTIES:
    {
        SERIAL_COMMPROP properties;
        VssComProperties(&properties);
        VssComCompleteWithData(Request, &properties, sizeof(properties));
        return;
    }

    case IOCTL_SERIAL_SET_QUEUE_SIZE:
    {
        // The ring has a fixed size; accept any request, as a UART would.
        SERIAL_QUEUE_SIZE size = {0};
        if (!VssComFetchInput(Request, &size, sizeof(size)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_GET_COMMSTATUS:
    {
        SERIAL_STATUS status = {0};
        VssComLockAcquire(context->Lock);
        // The error bits are read once: this is what clears them, and what
        // lets reads and writes through again after an abort on error.
        status.Errors = vsscom_errors_take(&context->Errors);
        status.AmountInInQueue = vsscom_ring_count(&context->Rx);
        status.AmountInOutQueue = vsscom_ring_count(&context->Tx);
        VssComLockRelease(context->Lock);
        VssComCompleteWithData(Request, &status, sizeof(status));
        return;
    }

    case IOCTL_SERIAL_SET_DTR:
    case IOCTL_SERIAL_CLR_DTR:
    case IOCTL_SERIAL_SET_RTS:
    case IOCTL_SERIAL_CLR_RTS:
    {
        BOOLEAN dtr;
        BOOLEAN rts;
        BOOLEAN refused;
        const BOOLEAN is_dtr = IoControlCode == IOCTL_SERIAL_SET_DTR ||
                               IoControlCode == IOCTL_SERIAL_CLR_DTR;
        vsscom_flow flow;
        VssComLockAcquire(context->Lock);
        dtr = context->Dtr;
        rts = context->Rts;
        flow.control_handshake = context->HandFlow.ControlHandShake;
        flow.flow_replace = context->HandFlow.FlowReplace;
        flow.xon_limit = 0;
        flow.xoff_limit = 0;
        VssComLockRelease(context->Lock);

        // A line that follows the receive buffer is the driver's to move, as on a
        // real port.
        refused = is_dtr ? !vsscom_flow_dtr_manual(&flow)
                         : !vsscom_flow_rts_manual(&flow);
        if (refused)
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }

        if (IoControlCode == IOCTL_SERIAL_SET_DTR)
            dtr = TRUE;
        else if (IoControlCode == IOCTL_SERIAL_CLR_DTR)
            dtr = FALSE;
        else if (IoControlCode == IOCTL_SERIAL_SET_RTS)
            rts = TRUE;
        else
            rts = FALSE;

        VssComUpdateLines(context, dtr, rts);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_GET_DTRRTS:
    {
        ULONG lines = 0;
        VssComLockAcquire(context->Lock);
        if (context->Dtr)
            lines |= SERIAL_DTR_STATE;
        if (context->Rts)
            lines |= SERIAL_RTS_STATE;
        VssComLockRelease(context->Lock);
        VssComCompleteWithData(Request, &lines, sizeof(lines));
        return;
    }

    case IOCTL_SERIAL_GET_MODEMSTATUS:
    {
        ULONG modem;
        VssComLockAcquire(context->Lock);
        modem = context->ModemLevels | context->ModemDeltas;
        context->ModemDeltas = 0;
        VssComLockRelease(context->Lock);
        VssComCompleteWithData(Request, &modem, sizeof(modem));
        return;
    }

    case IOCTL_SERIAL_SET_BREAK_ON:
    case IOCTL_SERIAL_SET_BREAK_OFF:
    {
        BOOLEAN changed;
        const BOOLEAN on = IoControlCode == IOCTL_SERIAL_SET_BREAK_ON;
        VssComLockAcquire(context->Lock);
        changed = context->Break != on;
        context->Break = on;
        VssComLockRelease(context->Lock);
        if (changed)
            VssComBumpState(context);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_SET_XOFF:
    case IOCTL_SERIAL_SET_XON:
    case IOCTL_SERIAL_RESET_DEVICE:
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    case IOCTL_SERIAL_CONFIG_SIZE:
    {
        ULONG size = 0;
        VssComCompleteWithData(Request, &size, sizeof(size));
        return;
    }

    case IOCTL_SERIAL_SET_WAIT_MASK:
    {
        ULONG mask = 0;
        BOOLEAN release;
        if (!VssComFetchInput(Request, &mask, sizeof(mask)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        VssComLockAcquire(context->Lock);
        release = vsscom_events_set_mask(&context->Events, mask);
        VssComLockRelease(context->Lock);

        // A wait that was pending ends with no events, as a new mask replaces
        // the old one.
        if (release)
            VssComFinishWait(context, 0);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_GET_WAIT_MASK:
    {
        ULONG mask;
        VssComLockAcquire(context->Lock);
        mask = context->Events.mask;
        VssComLockRelease(context->Lock);
        VssComCompleteWithData(Request, &mask, sizeof(mask));
        return;
    }

    case IOCTL_SERIAL_WAIT_ON_MASK:
    {
        vsscom_u32 fired = 0;
        BOOLEAN ready;
        ULONG epoch;
        NTSTATUS status;

        VssComLockAcquire(context->Lock);
        if (context->WaitQueued)
        {
            VssComLockRelease(context->Lock);
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        context->WaitQueued = TRUE;
        epoch = context->RevokeCount;
        VssComLockRelease(context->Lock);

        // The request is parked before the event state says "waiting", so an
        // event that arrives in between finds a request to complete.
        status = WdfRequestForwardToIoQueue(Request, context->WaitQueue);
        if (!NT_SUCCESS(status))
        {
            VssComLockAcquire(context->Lock);
            context->WaitQueued = FALSE;
            VssComLockRelease(context->Lock);
            WdfRequestComplete(Request, status);
            return;
        }

        VssComLockAcquire(context->Lock);
        ready = vsscom_events_wait(&context->Events, &fired);
        VssComLockRelease(context->Lock);

        // Events were already waiting in the history: take the request back
        // and finish it now.
        if (ready)
            VssComFinishWait(context, fired);

        // A revoke that ran between the check at the top and the parking found
        // nothing to end. The request may be gone by now, so the count tells.
        VssComLockAcquire(context->Lock);
        ready = context->RevokeCount != epoch;
        VssComLockRelease(context->Lock);
        if (ready)
            VssComFinishWaitWith(context, STATUS_DEVICE_REMOVED);
        return;
    }

    case IOCTL_SERIAL_PURGE:
    {
        ULONG flags = 0;
        BOOLEAN abort_read;
        if (!VssComFetchInput(Request, &flags, sizeof(flags)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }

        abort_read = (flags & SERIAL_PURGE_RXABORT) != 0;
        if ((flags & (SERIAL_PURGE_RXCLEAR | SERIAL_PURGE_RXABORT)) != 0)
        {
            VssComLockAcquire(context->Lock);
            vsscom_ring_clear(&context->Rx);
            VssComLockRelease(context->Lock);
        }
        if ((flags & (SERIAL_PURGE_TXCLEAR | SERIAL_PURGE_TXABORT)) != 0)
        {
            VssComLockAcquire(context->Lock);
            vsscom_ring_clear(&context->Tx);
            VssComLockRelease(context->Lock);
        }
        // Aborting writes ends those that are waiting for the hub.
        if ((flags & SERIAL_PURGE_TXABORT) != 0)
            VssComAbortAppWrites(context, STATUS_CANCELLED);

        // Aborting reads ends the one that is waiting with what it holds.
        if (abort_read)
        {
            VssComLockAcquire(context->Lock);
            context->AbortHead = TRUE;
            VssComLockRelease(context->Lock);
        }
        // Clearing made room, and an abort needs the read queue served.
        VssComPump(context);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_SERIAL_IMMEDIATE_CHAR:
    {
        UCHAR byte = 0;
        ULONG stored;
        BOOLEAN hub;
        if (!VssComFetchInput(Request, &byte, sizeof(byte)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        // The byte goes to the hub when there is one, and loops back if not.
        VssComLockAcquire(context->Lock);
        hub = context->HubOpen;
        stored = vsscom_ring_write(hub ? &context->Tx : &context->Rx, &byte, 1);
        VssComLockRelease(context->Lock);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        if (stored != 0)
        {
            VssComFlowTransmit(context, 1);
            VssComPump(context);
            if (!hub)
                VssComRaise(context, VSSCOM_EV_RXCHAR);
        }
        return;
    }

    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}

// -- Queues ----------------------------------------------------------------

NTSTATUS VssComCreateQueues(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    WDFDEVICE device = Context->Device;
    WDF_IO_QUEUE_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_TIMER_CONFIG timer;
    WDFQUEUE queue;
    NTSTATUS status;

    // Control requests: the default queue, many at once.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&config, WdfIoQueueDispatchParallel);
    config.EvtIoDeviceControl = VssComEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status))
        return status;

    // Reads arrive here and are parked in ReadQueue; writes are handled at once.
    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchParallel);
    config.EvtIoRead = VssComEvtIoRead;
    config.EvtIoWrite = VssComEvtIoWrite;
    status = WdfIoQueueCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfDeviceConfigureRequestDispatching(device, queue,
                                                  WdfRequestTypeRead);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfDeviceConfigureRequestDispatching(device, queue,
                                                  WdfRequestTypeWrite);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchManual);
    config.EvtIoCanceledOnQueue = VssComEvtReadCanceled;
    status = WdfIoQueueCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES,
                              &Context->ReadQueue);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchManual);
    config.EvtIoCanceledOnQueue = VssComEvtWaitCanceled;
    status = WdfIoQueueCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES,
                              &Context->WaitQueue);
    if (!NT_SUCCESS(status))
        return status;

    status = VssComCreateHubQueues(Context);
    if (!NT_SUCCESS(status))
        return status;

    // The timers take the lock themselves, so the framework does not
    // serialise their callbacks with the device.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    WDF_TIMER_CONFIG_INIT(&timer, VssComEvtReadTotalTimer);
    timer.AutomaticSerialization = FALSE;
    status = WdfTimerCreate(&timer, &attributes, &Context->ReadTotalTimer);
    if (!NT_SUCCESS(status))
        return status;

    WDF_TIMER_CONFIG_INIT(&timer, VssComEvtReadIntervalTimer);
    timer.AutomaticSerialization = FALSE;
    return WdfTimerCreate(&timer, &attributes, &Context->ReadIntervalTimer);
}
