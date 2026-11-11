//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vsscom: the hub end of a port.
//
// A port has two ends and one device object. The application end is opened as
// \\.\COMn. The hub end is opened through the device interface with the
// reference string "hub", so the driver sees the file name \hub. Each end has
// one handle at a time; a second open fails with access denied.
//
//   application write -> Tx ring -> hub ReadFile
//   hub WriteFile     -> Rx ring -> application read (io.c)
//
// A write that does not fit waits until the other end makes room, as it does
// on a real port with a full buffer. While no hub handle is open the port
// loops back (io.c), so a port that nobody has attached still behaves.
//
// The hub also reads what the application set (baud rate, line control, DTR,
// RTS, break) and sets the input lines the application sees (CTS, DSR, DCD,
// RI). The state request waits until something changes, so the hub never polls.
//
// Locks and completion: as in io.c, Lock guards the context and is never held
// while a request is completed or another queue is called.
//

#include "vsscom.h"

// -- Opening and closing ---------------------------------------------------

// Takes the end for the file object, if it is free, and completes the create.
static VOID VssComCompleteOpen(_In_ PVSSCOM_DEVICE_CONTEXT context,
                               _In_ WDFREQUEST Request,
                               _In_ WDFFILEOBJECT FileObject, _In_ ULONG kind)
{
    PVSSCOM_FILE_CONTEXT file = VssComFileContext(FileObject);
    NTSTATUS status = STATUS_SUCCESS;

    VssComLockAcquire(context->Lock);
    if (kind == VSSCOM_FILE_APP)
    {
        if (context->AppOpen)
        {
            status = STATUS_ACCESS_DENIED;
        }
        else
        {
            context->AppOpen = TRUE;
            context->AppFile = FileObject;
            context->AppProcessId = WdfRequestGetRequestorProcessId(Request);
        }
    }
    else
    {
        if (context->HubOpen)
        {
            status = STATUS_ACCESS_DENIED;
        }
        else
        {
            // Whatever the loopback held is not for the hub, and it starts with
            // every input line low until it says otherwise.
            context->HubOpen = TRUE;
            context->HubLines = 0;
            vsscom_ring_clear(&context->Rx);
            vsscom_ring_clear(&context->Tx);
        }
    }
    VssComLockRelease(context->Lock);

    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }

    file->Kind = kind;
    file->Granted = TRUE;

    if (kind == VSSCOM_FILE_HUB)
        VssComApplyLines(context);
    VssComBumpState(context);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID VssComEvtDeviceFileCreate(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                               _In_ WDFFILEOBJECT FileObject)
{
    PVSSCOM_DEVICE_CONTEXT context = VssComGetContext(Device);
    PUNICODE_STRING name = WdfFileObjectGetFileName(FileObject);
    DECLARE_CONST_UNICODE_STRING(hubName, VSSCOM_HUB_FILE_NAME);

    if (name == NULL || name->Length == 0 ||
        (name->Length == sizeof(WCHAR) && name->Buffer[0] == L'\\'))
    {
        VssComCompleteOpen(context, Request, FileObject, VSSCOM_FILE_APP);
    }
    else if (RtlEqualUnicodeString(name, &hubName, TRUE))
    {
        // The hub end is the device side of the port: not everyone who can open
        // the COM port may take it. The platform decides who (access.c, or
        // vsscom_um/um_access.c), and may take its time doing it.
        VssComAuthorizeHubOpen(context, Request, FileObject);
    }
    else
    {
        WdfRequestComplete(Request, STATUS_OBJECT_NAME_NOT_FOUND);
    }
}

VOID VssComHubOpenDecided(_In_ PVSSCOM_DEVICE_CONTEXT context,
                          _In_ WDFREQUEST Request,
                          _In_ WDFFILEOBJECT FileObject, _In_ BOOLEAN allowed)
{
    if (!allowed)
    {
        WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
        return;
    }
    VssComCompleteOpen(context, Request, FileObject, VSSCOM_FILE_HUB);
}

// A port goes back to quiet when its handle closes. The framework has already
// cancelled the requests that handle left waiting.
VOID VssComEvtFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
    PVSSCOM_DEVICE_CONTEXT context =
        VssComGetContext(WdfFileObjectGetDevice(FileObject));
    PVSSCOM_FILE_CONTEXT file = VssComFileContext(FileObject);

    if (!file->Granted)
        return;

    // A handle that was revoked has nothing left to undo: the port is quiet, and
    // may belong to another program by now.
    if (file->Kind == VSSCOM_FILE_APP && file->Revoked)
        return;

    if (file->Kind == VSSCOM_FILE_APP)
    {
        VssComResetPort(context);
        VssComLockAcquire(context->Lock);
        context->AppOpen = FALSE;
        context->AppFile = NULL;
        context->AppProcessId = 0;
        context->Break = FALSE;
        VssComLockRelease(context->Lock);
    }
    else
    {
        VssComLockAcquire(context->Lock);
        file->Closed = TRUE;
        context->HubOpen = FALSE;
        context->HubLines = 0;
        vsscom_ring_clear(&context->Rx);
        vsscom_ring_clear(&context->Tx);
        VssComLockRelease(context->Lock);
        VssComApplyLines(context);
    }

    VssComBumpState(context);
    // Reset and the end of hub mode both change who may go on: waiting writes
    // finish or find room.
    VssComPump(context);
}

// -- The state the hub watches ---------------------------------------------

static VOID VssComFillState(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                            _Out_ PVSSCOM_HUB_STATE State)
{
    RtlZeroMemory(State, sizeof(*State));
    VssComLockAcquire(Context->Lock);
    State->Generation = Context->Generation;
    State->AppOpen = Context->AppOpen ? 1 : 0;
    State->BaudRate = Context->BaudRate;
    State->DataBits = Context->LineControl.WordLength;
    State->Parity = Context->LineControl.Parity;
    State->StopBits = Context->LineControl.StopBits;
    State->Dtr = Context->Dtr ? 1 : 0;
    State->Rts = Context->Rts ? 1 : 0;
    State->Break = Context->Break ? 1 : 0;
    State->Lines = Context->HubLines;
    State->TxQueued = vsscom_ring_count(&Context->Tx);
    State->AppProcessId = Context->AppOpen ? Context->AppProcessId : 0;
    VssComLockRelease(Context->Lock);
}

static VOID VssComCompleteStateWaiters(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    WDFREQUEST request;

    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->StateQueue, &request)))
    {
        VSSCOM_HUB_STATE state;
        VssComFillState(Context, &state);
        VssComCompleteWithData(request, &state, sizeof(state));
    }
}

VOID VssComBumpState(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    VssComLockAcquire(Context->Lock);
    Context->Generation++;
    VssComLockRelease(Context->Lock);
    VssComCompleteStateWaiters(Context);
}

// -- Requests of the hub end -----------------------------------------------

static VOID VssComFinishPipe(_In_ WDFREQUEST Request, _In_ NTSTATUS Status)
{
    PVSSCOM_PIPE_CONTEXT pipe = VssComPipeContext(Request);
    WdfRequestCompleteWithInformation(Request, Status, pipe->Done);
}

// Gives a request a pipe context and parks it in `Queue`.
static NTSTATUS VssComParkPipe(_In_ WDFREQUEST Request, _In_ WDFQUEUE Queue,
                               _In_ PVOID Buffer, _In_ ULONG Length)
{
    PVSSCOM_PIPE_CONTEXT pipe = NULL;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VSSCOM_PIPE_CONTEXT);
    status = WdfObjectAllocateContext(Request, &attributes, (PVOID*)&pipe);
    if (!NT_SUCCESS(status))
        return status;
    pipe->Buffer = (PUCHAR)Buffer;
    pipe->Length = Length;
    pipe->Done = 0;
    return WdfRequestForwardToIoQueue(Request, Queue);
}

VOID VssComHubRead(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request,
                   _In_ size_t Length)
{
    PVOID buffer = NULL;
    NTSTATUS status;

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
    if (NT_SUCCESS(status))
        status = VssComParkPipe(Request, Context->HubReadQueue, buffer,
                                (ULONG)Length);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }
    VssComPump(Context);
}

VOID VssComHubWrite(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request,
                    _In_ size_t Length)
{
    PVOID buffer = NULL;
    NTSTATUS status;

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
    status = WdfRequestRetrieveInputBuffer(Request, Length, &buffer, NULL);
    if (NT_SUCCESS(status))
        status = VssComParkPipe(Request, Context->HubWriteQueue, buffer,
                                (ULONG)Length);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }
    VssComPump(Context);
}

VOID VssComAppWriteToHub(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                         _In_ WDFREQUEST Request, _In_ size_t Length)
{
    PVOID buffer = NULL;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, Length, &buffer, NULL);

    if (NT_SUCCESS(status))
    {
        VssComLockAcquire(Context->Lock);
        Context->AppWritesPending++;
        VssComLockRelease(Context->Lock);

        status = VssComParkPipe(Request, Context->AppWriteQueue, buffer,
                                (ULONG)Length);
        if (!NT_SUCCESS(status))
        {
            VssComLockAcquire(Context->Lock);
            Context->AppWritesPending--;
            VssComLockRelease(Context->Lock);
        }
    }
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }
    VssComPump(Context);
}

VOID VssComHubIoControl(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                        _In_ WDFREQUEST Request, _In_ ULONG IoControlCode)
{
    switch (IoControlCode)
    {
    case IOCTL_VSSCOM_HUB_GET_STATE:
    {
        VSSCOM_HUB_STATE state;
        VssComFillState(Context, &state);
        VssComCompleteWithData(Request, &state, sizeof(state));
        return;
    }

    case IOCTL_VSSCOM_HUB_WAIT_STATE:
    {
        ULONG known = 0;
        ULONG now;
        NTSTATUS status;

        if (!VssComFetchInput(Request, &known, sizeof(known)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }

        VssComLockAcquire(Context->Lock);
        now = Context->Generation;
        VssComLockRelease(Context->Lock);
        if (now != known)
        {
            VSSCOM_HUB_STATE state;
            VssComFillState(Context, &state);
            VssComCompleteWithData(Request, &state, sizeof(state));
            return;
        }

        status = WdfRequestForwardToIoQueue(Request, Context->StateQueue);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return;
        }

        // A change between the check and the parking found no request to
        // answer, so look again.
        VssComLockAcquire(Context->Lock);
        now = Context->Generation;
        VssComLockRelease(Context->Lock);
        if (now != known)
            VssComCompleteStateWaiters(Context);
        return;
    }

    case IOCTL_VSSCOM_HUB_SET_LINES:
    {
        ULONG lines = 0;
        if (!VssComFetchInput(Request, &lines, sizeof(lines)))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        VssComLockAcquire(Context->Lock);
        Context->HubLines = lines & VSSCOM_LINE_MASK;
        VssComLockRelease(Context->Lock);
        VssComApplyLines(Context);
        VssComBumpState(Context);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    case IOCTL_VSSCOM_HUB_REVOKE:
    {
        // Only the user who owns the port has a hub handle (access.c), so only
        // that user gets here. The answer is the process that had the port, or 0.
        PVOID output = NULL;
        ULONG process;

        // The process id is an answer for those who ask for it: a caller with no
        // output buffer revokes just the same.
        const BOOLEAN wants_process = NT_SUCCESS(WdfRequestRetrieveOutputBuffer(
            Request, sizeof(ULONG), &output, NULL));

        process = VssComRevokeApplication(Context);
        if (!wants_process)
        {
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;
        }
        *(PULONG)output = process;
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(ULONG));
        return;
    }

    case IOCTL_VSSCOM_HUB_LINE_ERROR:
    {
        VSSCOM_HUB_LINE_ERROR input = {0};
        PVSSCOM_PIPE_CONTEXT pipe = NULL;
        WDF_OBJECT_ATTRIBUTES attributes;
        vsscom_line_error error;
        NTSTATUS status;

        if (!VssComFetchInput(Request, &input, sizeof(input)) ||
            input.HasByte > 1 || input.Byte > 0xFF)
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        error.errors = input.Errors;
        error.has_byte = input.HasByte != 0;
        error.byte = (vsscom_u8)input.Byte;
        if (!vsscom_line_error_valid(&error))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }

        // The error waits in the queue of the hub's writes, so it keeps its
        // place among the bytes, and is cancelled with them when the hub goes.
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VSSCOM_PIPE_CONTEXT);
        status = WdfObjectAllocateContext(Request, &attributes, (PVOID*)&pipe);
        if (NT_SUCCESS(status))
        {
            pipe->IsLineError = TRUE;
            pipe->LineError = error;
            status = WdfRequestForwardToIoQueue(Request, Context->HubWriteQueue);
        }
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return;
        }
        VssComPump(Context);
        return;
    }

    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}

// -- Moving bytes ----------------------------------------------------------

// What became of a line error that reached the head of the hub's writes.
typedef enum _VSSCOM_ERROR_OUTCOME
{
    VssComErrorWaits,   // no room yet for the bytes it adds
    VssComErrorDropped, // no program has the port open
    VssComErrorRaised
} VSSCOM_ERROR_OUTCOME;

// Raises the line error of `Pipe` at the application end: latches it, puts its
// bytes in Rx and says which events it raises and whether it starts an abort on
// error. Completes nothing.
static VSSCOM_ERROR_OUTCOME VssComApplyLineError(
    _In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ PVSSCOM_PIPE_CONTEXT Pipe,
    _Out_ PULONG Events, _Out_ PBOOLEAN Abort)
{
    VSSCOM_ERROR_OUTCOME outcome = VssComErrorDropped;

    *Events = 0;
    *Abort = FALSE;

    VssComLockAcquire(Context->Lock);
    if (Context->AppOpen)
    {
        const vsscom_flow flow = VssComFlowLocked(Context);
        vsscom_error_chars chars;
        vsscom_error_effect effect;

        chars.error_char = Context->Chars.ErrorChar;
        chars.break_char = Context->Chars.BreakChar;
        chars.event_char = Context->Chars.EventChar;
        effect = vsscom_line_error_effect(&Pipe->LineError, &flow, &chars);
        if (vsscom_line_error_apply(&Context->Errors, &Context->Rx,
                                    &Pipe->LineError, &effect))
        {
            outcome = VssComErrorRaised;
            *Events = effect.events;
            *Abort = vsscom_errors_abort(&flow, Context->Errors) != 0;
        }
        else
        {
            outcome = VssComErrorWaits;
        }
    }
    VssComLockRelease(Context->Lock);
    return outcome;
}

// Hub writes go into Rx while it has room. The request at the head keeps its
// place, so writes stay in order.
BOOLEAN VssComServiceHubWrites(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    BOOLEAN moved = FALSE;

    for (;;)
    {
        WDFREQUEST request;
        PVSSCOM_PIPE_CONTEXT pipe;
        ULONG stored;
        BOOLEAN flag;

        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->HubWriteQueue,
                                                      &request)))
            return moved;

        pipe = VssComPipeContext(request);

        // A request of a hub handle that is closing ends here; see Closed.
        {
            WDFFILEOBJECT file = WdfRequestGetFileObject(request);
            if (file != NULL && VssComFileContext(file)->Closed)
            {
                VssComFinishPipe(request, STATUS_CANCELLED);
                continue;
            }
        }

        if (pipe->IsLineError)
        {
            VSSCOM_ERROR_OUTCOME outcome;
            ULONG events;
            BOOLEAN abort;

            // Bytes went into Rx in this call. A read that waits takes them
            // first (the pump serves the reads next and comes back), so an
            // abort on error does not end a read over bytes that came before
            // the error.
            if (moved)
            {
                WdfRequestRequeue(request);
                return moved;
            }

            outcome = VssComApplyLineError(Context, pipe, &events, &abort);
            if (outcome == VssComErrorWaits)
            {
                WdfRequestRequeue(request);
                return moved;
            }
            if (outcome == VssComErrorRaised)
            {
                if (events != 0)
                    VssComRaise(Context, events);
                // The reads end in the pump, which finds the error latched; the
                // writes that wait for the hub end here.
                if (abort)
                    VssComAbortAppWrites(Context, STATUS_CANCELLED);
                // Something changed for the reads either way: a byte, or the
                // abort.
                moved = TRUE;
            }
            // Completed last, so that the hub knows the error is latched and
            // its events are out when the request returns.
            WdfRequestComplete(request, STATUS_SUCCESS);
            continue;
        }

        VssComLockAcquire(Context->Lock);
        stored = vsscom_ring_write(&Context->Rx, pipe->Buffer + pipe->Done,
                                   pipe->Length - pipe->Done);
        flag = vsscom_bytes_have(pipe->Buffer + pipe->Done, stored,
                                 Context->Chars.EventChar) != 0;
        pipe->Done += stored;
        VssComLockRelease(Context->Lock);

        if (stored != 0)
        {
            moved = TRUE;
            VssComRaise(Context,
                        VSSCOM_EV_RXCHAR | (flag ? VSSCOM_EV_RXFLAG : 0u));
        }
        if (pipe->Done == pipe->Length)
        {
            VssComFinishPipe(request, STATUS_SUCCESS);
            continue;
        }
        WdfRequestRequeue(request);
        return moved;
    }
}

// Hub reads take what the application wrote, as soon as there is any.
BOOLEAN VssComServiceHubReads(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    BOOLEAN moved = FALSE;

    for (;;)
    {
        WDFREQUEST request;
        PVSSCOM_PIPE_CONTEXT pipe;
        ULONG got;
        BOOLEAN empty;
        ULONG pending;

        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->HubReadQueue,
                                                      &request)))
            return moved;

        pipe = VssComPipeContext(request);
        VssComLockAcquire(Context->Lock);
        got = vsscom_ring_read(&Context->Tx, pipe->Buffer, pipe->Length);
        empty = vsscom_ring_count(&Context->Tx) == 0;
        pending = Context->AppWritesPending;
        VssComLockRelease(Context->Lock);

        if (got == 0)
        {
            WdfRequestRequeue(request);
            return moved;
        }

        moved = TRUE;
        pipe->Done = got;
        VssComFinishPipe(request, STATUS_SUCCESS);

        // The application's output queue ran dry with nothing more waiting.
        if (empty && pending == 0)
            VssComRaise(Context, VSSCOM_EV_TXEMPTY);
    }
}

// The application's writes go into Tx while it has room. If the hub end has
// gone, what is left of a write is dropped and the write completes short.
BOOLEAN VssComServiceAppWrites(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    BOOLEAN moved = FALSE;

    for (;;)
    {
        WDFREQUEST request;
        PVSSCOM_PIPE_CONTEXT pipe;
        ULONG stored = 0;
        ULONG timeout_ms;
        BOOLEAN hub;
        BOOLEAN finished;
        BOOLEAN timed_out;

        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->AppWriteQueue,
                                                      &request)))
            return moved;

        // A write of a handle that was revoked ends here, wherever it was when the
        // revoke ran.
        if (VssComRequestRevoked(request))
        {
            VssComLockAcquire(Context->Lock);
            Context->AppWritesPending--;
            VssComLockRelease(Context->Lock);
            VssComFinishPipe(request, STATUS_DEVICE_REMOVED);
            moved = TRUE;
            continue;
        }

        // After an error with abort on error set, the writes that wait end, and
        // so does one that slipped in before the error was latched.
        if (VssComErrorAbort(Context))
        {
            VssComLockAcquire(Context->Lock);
            Context->AppWritesPending--;
            VssComLockRelease(Context->Lock);
            VssComFinishPipe(request, STATUS_CANCELLED);
            moved = TRUE;
            continue;
        }

        pipe = VssComPipeContext(request);
        VssComLockAcquire(Context->Lock);
        hub = Context->HubOpen;
        if (hub)
        {
            stored = vsscom_ring_write(&Context->Tx, pipe->Buffer + pipe->Done,
                                       pipe->Length - pipe->Done);
            pipe->Done += stored;
        }
        finished = !hub || pipe->Done == pipe->Length;
        timeout_ms = 0;
        if (!finished && !pipe->DeadlineSet)
        {
            // The write has stalled at the head of the queue: its timeout
            // starts now, as it does for a write that begins on a real port.
            pipe->DeadlineSet = TRUE;
            timeout_ms = vsscom_write_timeout_ms(&Context->Timeouts, pipe->Length);
            if (timeout_ms != 0)
                pipe->Deadline = VssComInterruptTime() +
                                 (ULONGLONG)timeout_ms * 10000ull;
        }
        timed_out = !finished && pipe->Deadline != 0 &&
                    VssComInterruptTime() >= pipe->Deadline;
        if (finished || timed_out)
            Context->AppWritesPending--;
        VssComLockRelease(Context->Lock);

        if (stored != 0)
        {
            moved = TRUE;
            // An RTS toggle goes up before the hub reads what was stored.
            VssComFlowTransmit(Context, stored);
        }
        if (finished)
        {
            VssComFinishPipe(request, STATUS_SUCCESS);
            continue;
        }
        if (timed_out)
        {
            // The bytes taken so far stay queued for the hub.
            VssComFinishPipe(request, STATUS_TIMEOUT);
            moved = TRUE;
            continue;
        }
        if (timeout_ms != 0)
        {
            // Restarting a timer that is queued does not move its due time, so
            // stop it first: it may be left from a write that has finished.
            WdfTimerStop(Context->WriteTimer, FALSE);
            WdfTimerStart(Context->WriteTimer, WDF_REL_TIMEOUT_IN_MS(timeout_ms));
        }
        WdfRequestRequeue(request);
        return moved;
    }
}

VOID VssComAbortAppWrites(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                          _In_ NTSTATUS Status)
{
    WDFREQUEST request;

    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->AppWriteQueue,
                                                    &request)))
    {
        VssComLockAcquire(Context->Lock);
        Context->AppWritesPending--;
        VssComLockRelease(Context->Lock);
        VssComFinishPipe(request, Status);
    }
}

// The write timeout of the head application write may be up: the pump looks.
VOID VssComEvtWriteTimer(_In_ WDFTIMER Timer)
{
    VssComPump(VssComGetContext(WdfTimerGetParentObject(Timer)));
}

VOID VssComEvtAppWriteCanceled(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request)
{
    PVSSCOM_DEVICE_CONTEXT context = VssComGetContext(WdfIoQueueGetDevice(Queue));

    VssComLockAcquire(context->Lock);
    context->AppWritesPending--;
    VssComLockRelease(context->Lock);
    VssComFinishPipe(Request, STATUS_CANCELLED);
}

// -- Queues ----------------------------------------------------------------

NTSTATUS VssComCreateHubQueues(_In_ PVSSCOM_DEVICE_CONTEXT Context)
{
    WDFDEVICE device = Context->Device;
    WDF_IO_QUEUE_CONFIG config;
    WDF_TIMER_CONFIG timer;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    // Without a cancel callback the framework completes a cancelled request
    // itself, which is all these three need.
    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES,
                              &Context->HubReadQueue);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES,
                              &Context->HubWriteQueue);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES,
                              &Context->StateQueue);
    if (!NT_SUCCESS(status))
        return status;

    // The application's waiting writes must give back the count of bytes they
    // had taken, and the pending count.
    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchManual);
    config.EvtIoCanceledOnQueue = VssComEvtAppWriteCanceled;
    status = WdfIoQueueCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES,
                              &Context->AppWriteQueue);
    if (!NT_SUCCESS(status))
        return status;

    // Like the read timers, it takes the lock itself through the pump, so the
    // framework does not serialise its callback with the device.
    WDF_TIMER_CONFIG_INIT(&timer, VssComEvtWriteTimer);
    timer.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfTimerCreate(&timer, &attributes, &Context->WriteTimer);
    if (!NT_SUCCESS(status))
        return status;

    // The hold of a toggling RTS ends here; it takes the lock through the flow
    // rules, so the framework does not serialise it with the device either.
    WDF_TIMER_CONFIG_INIT(&timer, VssComEvtFlowTimer);
    timer.AutomaticSerialization = FALSE;
    return WdfTimerCreate(&timer, &attributes, &Context->FlowTimer);
}
