//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the kernel-mode driver includes and what it supplies to the code that
// the kernel-mode and the user-mode driver share (see vsscom.h).
//

#pragma once

// ntifs.h for the security routines that tell who opened a handle.
#include <ntifs.h>
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>

// The time in 100 ns units since the system started, for the write deadline.
static FORCEINLINE ULONGLONG VssComInterruptTime(VOID)
{
    return KeQueryInterruptTime();
}

// The performance counter, for the break and write marks. The kernel clock that
// QueryPerformanceCounter reads in user mode: the helper's steady_clock is the
// same counter. `Frequency` may be NULL.
static FORCEINLINE ULONGLONG VssComQpc(_Out_opt_ PULONGLONG Frequency)
{
    LARGE_INTEGER frequency;
    const LARGE_INTEGER count = KeQueryPerformanceCounter(&frequency);
    if (Frequency != NULL)
        *Frequency = (ULONGLONG)frequency.QuadPart;
    return (ULONGLONG)count.QuadPart;
}

// The lock that guards a port's state. The ends run at DISPATCH_LEVEL here, so
// it is a spin lock.
typedef WDFSPINLOCK VSSCOM_LOCK;

static FORCEINLINE NTSTATUS VssComLockCreate(
    _In_opt_ PWDF_OBJECT_ATTRIBUTES Attributes, _Out_ VSSCOM_LOCK* Lock)
{
    return WdfSpinLockCreate(Attributes, Lock);
}

static FORCEINLINE VOID VssComLockAcquire(_In_ VSSCOM_LOCK Lock)
{
    WdfSpinLockAcquire(Lock);
}

static FORCEINLINE VOID VssComLockRelease(_In_ VSSCOM_LOCK Lock)
{
    WdfSpinLockRelease(Lock);
}
