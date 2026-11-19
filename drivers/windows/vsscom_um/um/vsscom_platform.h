//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the user-mode driver includes and what it supplies to the code that the
// kernel-mode and the user-mode driver share (see vsscom/vsscom.h).
//

#pragma once

// windows.h and ntstatus.h both define the STATUS_* codes, with the same values
// and different types; the one included second wins, and a driver wants the
// NTSTATUS ones of ntstatus.h. winioctl.h and ntddser.h (included by vsscom.h)
// likewise both define the SERIAL_LSRMST_* and SERIAL_IOC_* macros with the same
// values. The repeats are harmless, so the warning is off for the whole driver.
#pragma warning(disable : 4005)

#include <windows.h>
#include <ntstatus.h>
#include <winioctl.h>
#include <wdf.h>

// The time in 100 ns units since the system started, for the write deadline.
// Time spent asleep does not count, which is what a timeout wants.
static FORCEINLINE ULONGLONG VssComInterruptTime(VOID)
{
    ULONGLONG now = 0;
    (void)QueryUnbiasedInterruptTime(&now);
    return now;
}

// The performance counter, for the break and write marks. `Frequency` may be NULL.
static FORCEINLINE ULONGLONG VssComQpc(_Out_opt_ PULONGLONG Frequency)
{
    LARGE_INTEGER count;
    LARGE_INTEGER frequency;
    (void)QueryPerformanceCounter(&count);
    (void)QueryPerformanceFrequency(&frequency);
    if (Frequency != NULL)
        *Frequency = (ULONGLONG)frequency.QuadPart;
    return (ULONGLONG)count.QuadPart;
}

// The lock that guards a port's state. A user-mode driver has no spin locks:
// everything runs at passive level, so it is a wait lock.
typedef WDFWAITLOCK VSSCOM_LOCK;

static FORCEINLINE NTSTATUS VssComLockCreate(
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes, _Out_ VSSCOM_LOCK* Lock)
{
    return WdfWaitLockCreate(Attributes, Lock);
}

static FORCEINLINE VOID VssComLockAcquire(_In_ VSSCOM_LOCK Lock)
{
    (void)WdfWaitLockAcquire(Lock, NULL);
}

static FORCEINLINE VOID VssComLockRelease(_In_ VSSCOM_LOCK Lock)
{
    WdfWaitLockRelease(Lock);
}
