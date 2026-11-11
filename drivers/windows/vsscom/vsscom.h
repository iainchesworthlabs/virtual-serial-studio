//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vsscom: the device context and the entry points shared by driver.c and io.c.
//

#ifndef VSSCOM_H
#define VSSCOM_H

// The operating system and framework headers differ between the kernel-mode and
// the user-mode driver. Each project puts its own vsscom_platform.h first on the
// include path: it includes what the framework needs and defines
// VssComInterruptTime.
#include <vsscom_platform.h>

// initguid.h first, so that driver.c defines the GUIDs of ntddser.h and
// public.h and the linker finds them. io.c only declares them.
#ifdef VSSCOM_DEFINE_GUIDS
#include <initguid.h>
#endif
#include <ntddser.h>

#include "public.h"
#include "core/vsscom_core.h"

#define VSSCOM_POOL_TAG 'mssv'

// The largest ULONG, and the slot of a port that has none. A constant and not
// ULONG_MAX or MAXULONG, because the kernel-mode and user-mode headers define
// different ones.
static const ULONG VssComMaxUlong = 0xFFFFFFFFul;

// The bytes a port holds in each direction. Rx is what the application reads;
// Tx is what it has written and the hub has not read yet.
#define VSSCOM_RX_SIZE (64 * 1024)
#define VSSCOM_TX_SIZE (64 * 1024)

// Who opened a handle, taken when the handle is created: the user of the
// token that opens it, which is the thread's token when it impersonates.
typedef struct _VSSCOM_OPENER
{
    BOOLEAN Kernel; // opened from kernel mode
    BOOLEAN Admin;  // the token is in the Administrators group
    BOOLEAN Valid;  // Sid holds the user
    UCHAR Sid[SECURITY_MAX_SID_SIZE];
} VSSCOM_OPENER, *PVSSCOM_OPENER;

// Which end a file object is.
#define VSSCOM_FILE_APP 1
#define VSSCOM_FILE_HUB 2
// What VssComRequestKind reports for the application end after a revoke: every
// request on it fails. A constant and not a macro, because the preprocessor policy
// counts the directives of a header.
static const ULONG VssComFileRevoked = 3;

typedef struct _VSSCOM_FILE_CONTEXT
{
    ULONG Kind;
    // Set once the open was accepted, so cleanup counts only handles that count.
    BOOLEAN Granted;
    // The application handle was taken back by IOCTL_VSSCOM_HUB_REVOKE. Its requests
    // fail, and closing it changes nothing: the port may belong to another program
    // by then. Written under the device lock, read without it.
    volatile BOOLEAN Revoked;
    // The hub handle is closing. The framework cancels the requests it left in the
    // queues only after the cleanup callback, and that callback runs the pump, so
    // the pump must not serve them: a write or a line error of a hub that has gone
    // would reach the application. Written under the device lock, read without it.
    volatile BOOLEAN Closed;
} VSSCOM_FILE_CONTEXT, *PVSSCOM_FILE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VSSCOM_FILE_CONTEXT, VssComFileContext)

// A read or write that waits for the other end to make room or send bytes.
typedef struct _VSSCOM_PIPE_CONTEXT
{
    PUCHAR Buffer;
    ULONG Length;
    ULONG Done;
    // For an application write that waits for the hub: when the write timeout
    // ends it, in interrupt time (100 ns units); 0 until the write stalls at
    // the head of the queue, and 0 again if it has no timeout.
    ULONGLONG Deadline;
    BOOLEAN DeadlineSet;
    // A line error that waits its turn among the hub's writes
    // (IOCTL_VSSCOM_HUB_LINE_ERROR): no buffer, and LineError says what to raise.
    BOOLEAN IsLineError;
    vsscom_line_error LineError;
} VSSCOM_PIPE_CONTEXT, *PVSSCOM_PIPE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VSSCOM_PIPE_CONTEXT, VssComPipeContext)

// What one read request remembers while it waits in the read queue.
typedef struct _VSSCOM_READ_CONTEXT
{
    PUCHAR Buffer;
    ULONG Requested;
    ULONG Received;
    BOOLEAN Started;
    vsscom_read_plan Plan;
    ULONGLONG TotalDeadline;    // interrupt time, 100 ns units; 0 for none
    ULONGLONG IntervalDeadline; // 0 until the first byte has arrived
} VSSCOM_READ_CONTEXT, *PVSSCOM_READ_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VSSCOM_READ_CONTEXT, VssComReadContext)

typedef struct _VSSCOM_DEVICE_CONTEXT
{
    WDFDEVICE Device;
    PWSTR PdoName;
    BOOLEAN CreatedSerialCommValue;
    // Made by the control device (and then Slot is its slot), or made by hand.
    BOOLEAN OnDemand;
    ULONG Slot;

    // Reads wait here in arrival order; only the first is served. Manual, so
    // the framework cancels a request that is waiting here without our help.
    WDFQUEUE ReadQueue;
    // At most one WaitCommEvent request waits here.
    WDFQUEUE WaitQueue;
    // The hub end: reads wait for bytes the application wrote, writes wait for
    // room in Rx, and state requests wait for the application to change
    // something. The application's writes wait here when Tx is full.
    WDFQUEUE HubReadQueue;
    WDFQUEUE HubWriteQueue;
    WDFQUEUE StateQueue;
    WDFQUEUE AppWriteQueue;
    WDFTIMER ReadTotalTimer;
    WDFTIMER ReadIntervalTimer;
    // Ends the head application write that waits for the hub, when its write
    // timeout is up. It only runs the pump, which looks at the deadline.
    WDFTIMER WriteTimer;
    // Ends the hold of a toggling RTS: RTS stays high until the last byte has had
    // time to leave, then this runs the flow rules again (VssComApplyFlow).
    WDFTIMER FlowTimer;

    // Everything below is guarded by Lock.
    VSSCOM_LOCK Lock;
    vsscom_u8 RxStorage[VSSCOM_RX_SIZE];
    vsscom_ring Rx;
    vsscom_u8 TxStorage[VSSCOM_TX_SIZE];
    vsscom_ring Tx;
    BOOLEAN AppOpen;
    // One more for each revoke.
    ULONG RevokeCount;
    // The file object and the process of the application that has the port open,
    // for the hub state and for revoke. Valid while AppOpen.
    WDFFILEOBJECT AppFile;
    ULONG AppProcessId;
    BOOLEAN HubOpen;
    BOOLEAN Break;
    ULONG Generation;      // one more for each change the hub can see
    ULONG HubLines;        // the input lines the hub set
    ULONG AppWritesPending; // writes waiting in AppWriteQueue
    BOOLEAN PumpBusy;      // one thread is moving bytes between the ends
    BOOLEAN PumpAgain;     // something changed while it was
    vsscom_timeouts Timeouts;
    vsscom_events Events;
    ULONG BaudRate;
    SERIAL_LINE_CONTROL LineControl;
    SERIAL_HANDFLOW HandFlow;
    SERIAL_CHARS Chars;
    BOOLEAN Dtr;
    BOOLEAN Rts;
    ULONG ModemLevels; // the input lines as last reported
    ULONG ModemDeltas; // changes since IOCTL_SERIAL_GET_MODEMSTATUS last ran
    BOOLEAN WaitQueued;   // a WaitCommEvent request is parked in WaitQueue
    BOOLEAN ServiceBusy;  // one thread is serving the read queue
    BOOLEAN ServiceAgain; // something changed while it was
    BOOLEAN AbortHead;    // PurgeComm asked for the waiting read to end
    // The line errors the hub raised that ClearCommError has not read yet
    // (VSSCOM_ERROR_* bits). Cleared when the application closes the port.
    vsscom_u32 Errors;
    // When the bytes the application wrote will all have left, in interrupt time
    // (100 ns units); 0 for none. Only RTS toggle looks at it.
    ULONGLONG TxBusyUntil;
} VSSCOM_DEVICE_CONTEXT, *PVSSCOM_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VSSCOM_DEVICE_CONTEXT, VssComGetContext)

// port.c
NTSTATUS VssComRegisterPort(_In_ PVSSCOM_DEVICE_CONTEXT Context);
// Finishes a port: the file object rules, the device, its context, the two
// interfaces, the COM name and the queues. The caller has set what only its
// framework has. `OnDemand` and `Slot` say where the port came from (access.c).
NTSTATUS VssComCreatePort(_Inout_ PWDFDEVICE_INIT DeviceInit,
                          _In_ BOOLEAN OnDemand, _In_ ULONG Slot);

// control.c: the device that makes ports. A device is the control device when
// its hardware key holds VssRole = 1, which vsscomctl.inf sets; every other
// device is a port.
#define VSSCOM_ROLE_VALUE L"VssRole"
#define VSSCOM_ROLE_CONTROL 1
NTSTATUS VssComControlAdd(_Inout_ PWDFDEVICE_INIT DeviceInit);
// The user who made the port in `Slot`. FALSE when there is no such port.
BOOLEAN VssComControlOwnerOf(_In_ ULONG Slot, _Out_ PVSSCOM_OPENER Owner);

// Who may open the hub end of a port. The kernel-mode driver decides at once
// from the token of the create request (access.c); the user-mode driver learns
// the user from the framework, which may take its time (vsscom_um/um_access.c).
// Either way it ends by calling VssComHubOpenDecided, which hub.c defines.
VOID VssComAuthorizeHubOpen(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                            _In_ WDFREQUEST Request,
                            _In_ WDFFILEOBJECT FileObject);
VOID VssComHubOpenDecided(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                          _In_ WDFREQUEST Request,
                          _In_ WDFFILEOBJECT FileObject, _In_ BOOLEAN Allowed);

// access.c (kernel-mode driver only): who opened a handle.
// Reads who is opening the file object a create request is for.
NTSTATUS VssComCaptureOpener(_In_ WDFREQUEST Request,
                             _Out_ PVSSCOM_OPENER Opener);
// The hub end of a port made on demand opens only for the user who made it. The
// hub end of a port made by hand opens only for administrators. Kernel mode
// may open either.
BOOLEAN VssComHubOpenAllowed(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                             _In_ const VSSCOM_OPENER* Opener);

// io.c
NTSTATUS VssComCreateQueues(_In_ PVSSCOM_DEVICE_CONTEXT Context);
VOID VssComResetPort(_In_ PVSSCOM_DEVICE_CONTEXT Context);
VOID VssComCompleteWithData(_In_ WDFREQUEST Request,
                            _In_reads_bytes_(Length) const VOID* Data,
                            _In_ size_t Length);
BOOLEAN VssComFetchInput(_In_ WDFREQUEST Request,
                         _Out_writes_bytes_(Length) PVOID Data,
                         _In_ size_t Length);
// Moves bytes between the ends and serves whatever is waiting. Call it after
// anything that could let a waiting request go on.
VOID VssComPump(_In_ PVSSCOM_DEVICE_CONTEXT Context);
VOID VssComRaise(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ ULONG Events);
// Ends the waiting WaitCommEvent request, if there is one, with `Status`.
VOID VssComFinishWaitWith(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ NTSTATUS Status);
// True when the request is from an application handle that IOCTL_VSSCOM_HUB_REVOKE took
// back; a request that was waiting then ends with STATUS_DEVICE_REMOVED.
BOOLEAN VssComRequestRevoked(_In_ WDFREQUEST Request);
// Runs the DTR and RTS rules of the flow control settings. `Set` is TRUE when the
// application has just given the settings (enable and disable then move the lines);
// otherwise only the lines that follow the receive buffer or the sending change.
// Call with the lock free.
VOID VssComApplyFlow(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ BOOLEAN Set);
// The application queued `Bytes` bytes for sending: an RTS toggle goes high now and
// stays high until they have left. Call with the lock free.
VOID VssComFlowTransmit(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ ULONG Bytes);
// The flow timer: the hold of a toggling RTS may be over.
VOID VssComEvtFlowTimer(_In_ WDFTIMER Timer);
// The flow control settings as the core takes them. Call with the lock held.
vsscom_flow VssComFlowLocked(_In_ PVSSCOM_DEVICE_CONTEXT Context);
// True while the application asked for abort on error and an error is latched:
// its requests then end with STATUS_CANCELLED. Call with the lock free.
BOOLEAN VssComErrorAbort(_In_ PVSSCOM_DEVICE_CONTEXT Context);
// Takes the application handle back (IOCTL_VSSCOM_HUB_REVOKE). Returns the process
// that had it, or 0.
ULONG VssComRevokeApplication(_In_ PVSSCOM_DEVICE_CONTEXT Context);
// Recomputes the input lines from the mode (hub or loopback) and raises what
// changed. Call with the lock free.
VOID VssComApplyLines(_In_ PVSSCOM_DEVICE_CONTEXT Context);

// hub.c
NTSTATUS VssComCreateHubQueues(_In_ PVSSCOM_DEVICE_CONTEXT Context);
// The file object callbacks and the requests of the hub end.
VOID VssComEvtDeviceFileCreate(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                               _In_ WDFFILEOBJECT FileObject);
VOID VssComEvtFileCleanup(_In_ WDFFILEOBJECT FileObject);
VOID VssComHubRead(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request,
                   _In_ size_t Length);
VOID VssComHubWrite(_In_ PVSSCOM_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request,
                    _In_ size_t Length);
VOID VssComHubIoControl(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                        _In_ WDFREQUEST Request, _In_ ULONG IoControlCode);
// An application write in hub mode: waits in AppWriteQueue until Tx has room.
VOID VssComAppWriteToHub(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                         _In_ WDFREQUEST Request, _In_ size_t Length);
// Each returns TRUE when it moved bytes.
BOOLEAN VssComServiceHubWrites(_In_ PVSSCOM_DEVICE_CONTEXT Context);
BOOLEAN VssComServiceHubReads(_In_ PVSSCOM_DEVICE_CONTEXT Context);
BOOLEAN VssComServiceAppWrites(_In_ PVSSCOM_DEVICE_CONTEXT Context);
// Completes the application's waiting writes with `Status`.
VOID VssComAbortAppWrites(_In_ PVSSCOM_DEVICE_CONTEXT Context,
                          _In_ NTSTATUS Status);
// Adds one to Generation and answers the hub's waiting state requests.
VOID VssComBumpState(_In_ PVSSCOM_DEVICE_CONTEXT Context);
// The write timeout timer: runs the pump, which ends a write that is past due.
VOID VssComEvtWriteTimer(_In_ WDFTIMER Timer);
// The queue callbacks for writes that were cancelled while they waited.
VOID VssComEvtAppWriteCanceled(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request);

#endif // VSSCOM_H
