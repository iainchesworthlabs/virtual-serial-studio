//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the helper service shares with the studio and with the user-mode driver.
//
// The kernel-mode driver has a control device that makes ports on demand. A
// user-mode driver cannot be a bus, so it has no such device. Its ports are
// software devices, and Windows lets only an administrator or SYSTEM make one.
// The helper is a service that runs as SYSTEM and makes them for ordinary
// users, with the same rules as the control device:
//
//   - a user holds at most VSSCOM_HELPER_MAX_PORTS_PER_USER ports, and all
//     users together at most VSSCOM_HELPER_MAX_PORTS;
//   - the hub end of a port opens only for the user who asked for the port
//     (the driver reads the owner from the device property below);
//   - a port goes when the pipe that asked for it closes, whether the caller
//     closed it or died;
//   - a request to make a port is answered when the port's driver has started,
//     so its COM name and hub interface exist by then.
//
// The caller opens the named pipe, then sends one request and reads one reply
// per exchange. A request and a reply are one message each. Open the pipe with
// SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION: the helper needs to know the
// user and nothing more.
//
// The constants are enumerators and constant objects and not macros, because the
// preprocessor policy counts the directives of a header. Include <windows.h> and
// <devpropdef.h> before this file.
//

#pragma once

// The pipe. Authenticated users may open it; remote machines may not.
static const WCHAR VssComHelperPipeName[] = L"\\\\.\\pipe\\vsscom_helper";

// The name of the service.
static const WCHAR VssComHelperServiceName[] = L"vsscom_helper";

// The enumerator of the software devices, the hardware ID of a port, and the
// format of its instance id: the slot, as %04lX, so the port that holds a slot
// keeps its COM name from one run to the next. The instance id of the whole
// device is SWD\VSSCOMUM\<slot>.
static const WCHAR VssComUmEnumerator[] = L"VSSCOMUM";
static const WCHAR VssComUmPortHardwareId[] = L"VSSCOMUM\\PORT";
static const WCHAR VssComUmPortInstanceFormat[] = L"%04lX";
static const WCHAR VssComUmPortInstancePrefix[] = L"SWD\\VSSCOMUM\\";

// The device property that holds the owner of a port: the user's SID in its binary
// form. The helper sets it when it makes the device and the driver reads it; a
// device without it is a port made by hand.
// {5e0b8a77-1c3d-4f29-b6a4-8d2f7c91e305}, property 2.
static const DEVPROPKEY VssComUmOwnerKey = {
    {0x5e0b8a77, 0x1c3d, 0x4f29, {0xb6, 0xa4, 0x8d, 0x2f, 0x7c, 0x91, 0xe3, 0x05}},
    2};

enum
{
    // 1 is the first version. A request with another version is refused.
    VSSCOM_HELPER_VERSION = 1,
    // The ports that can exist at once, for all callers together, and the most
    // one user can hold. The same numbers as the control device.
    VSSCOM_HELPER_MAX_PORTS = 32,
    VSSCOM_HELPER_MAX_PORTS_PER_USER = 16,
    // A SID is at most this many bytes (SECURITY_MAX_SID_SIZE).
    VSSCOM_HELPER_MAX_SID_BYTES = 68
};

enum
{
    // Make a port. The reply carries its slot.
    VSSCOM_HELPER_OP_CREATE = 1,
    // Remove a port that this pipe made.
    VSSCOM_HELPER_OP_DESTROY = 2,
    // Change the COM number of a port of the user-mode driver that this pipe made:
    // Slot is its slot and Argument the number. See "COM numbers" below.
    VSSCOM_HELPER_OP_SET_COM_NUMBER = 3,
    // The same for a port of the kernel-mode driver, made through its control device
    // by the caller (the helper asks the control device who made it). The port
    // lives as long as the caller's control handle, not this pipe.
    VSSCOM_HELPER_OP_SET_COM_NUMBER_KM = 4
};

enum
{
    // The numbers a port may be given. The COM name database holds 256 ports by
    // default, and a name above 9 needs the \\.\COMn form to open.
    VSSCOM_HELPER_MIN_COM_NUMBER = 1,
    VSSCOM_HELPER_MAX_COM_NUMBER = 255
};

typedef struct _VSSCOM_HELPER_REQUEST
{
    ULONG Version;  // VSSCOM_HELPER_VERSION
    ULONG Op;       // VSSCOM_HELPER_OP_*
    ULONG Slot;     // for DESTROY and SET_COM_NUMBER; zero for CREATE
    ULONG Argument; // the COM number for SET_COM_NUMBER; zero for the others
} VSSCOM_HELPER_REQUEST, *PVSSCOM_HELPER_REQUEST;

// Status is a Win32 error code:
//   ERROR_SUCCESS             done;
//   ERROR_NOT_ENOUGH_QUOTA    this user holds the most ports they may;
//   ERROR_NO_SYSTEM_RESOURCES every slot is taken, or the system refused;
//   ERROR_NOT_FOUND           DESTROY named a slot this pipe does not hold;
//   ERROR_DEV_NOT_EXIST       the device was made and no driver started on it:
//                             the user-mode driver is not installed;
//   ERROR_TIMEOUT             the driver did not start in 30 seconds;
//   ERROR_NOT_SUPPORTED       the request has a version or a code that the helper
//                             does not know;
//   ERROR_ACCESS_DENIED       the helper could not tell who the caller is, or (for
//                             SET_COM_NUMBER_KM) the port was made by someone else;
//   ERROR_INVALID_PARAMETER   SET_COM_NUMBER: the number is outside the range above;
//   ERROR_ALREADY_ASSIGNED    SET_COM_NUMBER: another device has that COM number: a real
//                             port, com0com, or an unplugged port of something else
//                             that keeps the name; nothing was changed. A Studio port
//                             that was removed, or a number only the COM name database
//                             marks, does not count: the helper clears it and takes
//                             the number;
//   ERROR_BUSY                SET_COM_NUMBER: a program has the port open (nothing was
//                             changed), or its hub end was open, so Windows could not
//                             restart it (the old values are back, but the port cannot
//                             restart again until it is removed and made again);
//   ERROR_NOT_FOUND           (also) SET_COM_NUMBER: no such port, or this pipe does not
//                             hold it;
//   ERROR_GEN_FAILURE         SET_COM_NUMBER: Windows would not take the change; the
//                             old number was put back.
typedef struct _VSSCOM_HELPER_REPLY
{
    ULONG Version; // VSSCOM_HELPER_VERSION
    ULONG Status;
    ULONG Slot;
    ULONG Reserved;
} VSSCOM_HELPER_REPLY, *PVSSCOM_HELPER_REPLY;

// COM numbers
//
// A port gets its COM name from the Ports class installer when Windows builds it:
// the lowest number the COM name database (ComDB) has free. A program that wants
// another number, as Device Manager's Advanced > COM Port Number does, asks the
// helper, which runs as SYSTEM and so may do what Device Manager does:
//
//   1. claim the new number in ComDB (refused if another device holds it, or if
//      \\.\COMn already exists);
//   2. write it as PortName in the device's hardware key and update its friendly
//      name;
//   3. restart the device, so that the driver reads the new PortName;
//   4. release the old number in ComDB.
//
// Writing PortName into the device's registry key before the port is built does
// not work: Windows then fails to install the port. The change is therefore made
// on a port that exists, and the port is restarted: no program may have it open and
// the hub end must be closed, or Windows refuses to restart it (ERROR_BUSY). The
// reply comes when the port is running again under the new name.
