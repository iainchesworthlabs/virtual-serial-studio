# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Drives the installed vsscom port through the Win32 serial API, the way real
# programs do. Run inside a guest, as an administrator, after install-driver.ps1.
#
#   test-driver.ps1 [-ResultPath <json>]
#
# Tests (the driver's own, named D to keep them apart from the platform cells):
#   D1 enumeration     the Ports device, SERIALCOMM and GetPortNames agree
#   D2 configuration   baud, bits, parity and stop bits round-trip; data loops back
#   D3 timeouts        the documented SetCommTimeouts combinations
#   D4 pending I/O     a waiting read, CancelIoEx, PurgeComm, closing the handle
#   D5 events          SetCommMask and WaitCommEvent
#   D6 lines           DTR and RTS come back as DSR, DCD and CTS
#   D7 exclusive       a second CreateFile fails with access denied
#   D8-D13 hub end     the second handle, bytes both ways, flow control, lines, purge
#   D14-D21 control    the control device makes ports on demand and removes them again; write timeouts (D20), hub-end access (D21)
#   H14 helper         the helper service of the user-mode driver answers and refuses what it should
#   D23-D25 flow       SetCommState drives DTR and RTS: enable and disable, handshake, toggle
#   D26-D27 revoke     the hub takes the port back from a program; the program's process id
#   D28-D31 COM number the helper gives a port the COM number asked for, and refuses what it should
#   D32-D41 line errors the hub raises a break or a framing, parity or overrun error (driver version 4): what
#                       ClearCommError reports, EV_ERR and EV_BREAK, the error and break characters, a request
#                       that waits behind the bytes before it, fAbortOnError, EV_RXFLAG, cancel and close races, special characters
#                       reset on close, events kept across a cancelled wait, and the bytes of an aborted write
#
# D1 to D7 use a port made by hand and run as a loopback: what is written comes
# back from the next read. D14 to D19 and D21 need something that makes ports on
# demand, and make their own ports: the control device that install-driver.ps1
# makes (kernel-mode driver), or the helper service that install-driver.ps1 -Helper
# installs (user-mode driver). Without the helper they are skipped.
# Exit code 0 when every test passed.

[CmdletBinding()]
param(
    [string] $ResultPath,
    [ValidateSet('kmdf', 'umdf')] [string] $Flavour
)

$ErrorActionPreference = 'Stop'
if ($Flavour) { $env:VSS_DRIVER = $Flavour }
. (Join-Path $PSScriptRoot 'common.ps1')

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public sealed class PendingIo : IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    struct OVERLAPPED { public IntPtr Internal, InternalHigh; public uint Offset, OffsetHigh; public IntPtr Event; }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(IntPtr h, IntPtr buffer, uint length, IntPtr read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WaitCommEvent(IntPtr h, IntPtr mask, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteFile(IntPtr h, IntPtr buffer, uint length, IntPtr written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetOverlappedResult(IntPtr h, IntPtr overlapped, out uint transferred, bool wait);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CancelIoEx(IntPtr h, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr CreateEvent(IntPtr attributes, bool manual, bool initial, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    readonly IntPtr handle;
    IntPtr overlapped = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));
    IntPtr buffer;
    IntPtr signal;
    public int Length;

    PendingIo(IntPtr h, int length)
    {
        handle = h;
        Length = length;
        buffer = Marshal.AllocHGlobal(Math.Max(length, 4));
        signal = CreateEvent(IntPtr.Zero, true, false, null);
        var ov = new OVERLAPPED { Event = signal };
        Marshal.StructureToPtr(ov, overlapped, false);
    }

    /// Starts a read of `length` bytes. It may already be finished.
    public static PendingIo BeginRead(IntPtr h, int length)
    {
        var io = new PendingIo(h, length);
        if (!ReadFile(h, io.buffer, (uint)length, IntPtr.Zero, io.overlapped) && Marshal.GetLastWin32Error() != 997)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "ReadFile");
        return io;
    }

    /// Starts a write of `data`. It may already be finished; Finish gives the byte count.
    public static PendingIo BeginWrite(IntPtr h, byte[] data)
    {
        var io = new PendingIo(h, data.Length);
        Marshal.Copy(data, 0, io.buffer, data.Length);
        if (!WriteFile(h, io.buffer, (uint)data.Length, IntPtr.Zero, io.overlapped) && Marshal.GetLastWin32Error() != 997)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "WriteFile");
        return io;
    }

    /// Starts WaitCommEvent. It may already be finished.
    public static PendingIo BeginWait(IntPtr h)
    {
        var io = new PendingIo(h, 4);
        if (!WaitCommEvent(h, io.buffer, io.overlapped) && Marshal.GetLastWin32Error() != 997)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "WaitCommEvent");
        return io;
    }

    public bool Completed(int ms) { return WaitForSingleObject(signal, (uint)ms) == 0; }

    /// The byte count; `error` is 0 or the Win32 error the operation ended with.
    public int Finish(bool wait, out int error)
    {
        uint n;
        error = 0;
        if (!GetOverlappedResult(handle, overlapped, out n, wait)) error = Marshal.GetLastWin32Error();
        return (int)n;
    }

    public bool Cancel() { return CancelIoEx(handle, overlapped); }

    public byte[] Data(int n)
    {
        var bytes = new byte[n];
        if (n > 0) Marshal.Copy(buffer, bytes, 0, n);
        return bytes;
    }

    public uint Mask() { return (uint)Marshal.ReadInt32(buffer); }

    public void Dispose()
    {
        if (signal != IntPtr.Zero) { CloseHandle(signal); signal = IntPtr.Zero; }
        if (overlapped != IntPtr.Zero) { Marshal.FreeHGlobal(overlapped); overlapped = IntPtr.Zero; }
        if (buffer != IntPtr.Zero) { Marshal.FreeHGlobal(buffer); buffer = IntPtr.Zero; }
    }
}

public static class VssComm
{
    [StructLayout(LayoutKind.Sequential)]
    struct DCB
    {
        public uint DCBlength, BaudRate, Flags;
        public ushort wReserved, XonLim, XoffLim;
        public byte ByteSize, Parity, StopBits;
        public sbyte XonChar, XoffChar, ErrorChar, EofChar, EvtChar;
        public ushort wReserved1;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct COMMTIMEOUTS { public uint ReadInterval, ReadMultiplier, ReadConstant, WriteMultiplier, WriteConstant; }

    [StructLayout(LayoutKind.Sequential)]
    struct OVERLAPPED { public IntPtr Internal, InternalHigh; public uint Offset, OffsetHigh; public IntPtr Event; }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetCommState(IntPtr h, ref DCB dcb);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetCommState(IntPtr h, ref DCB dcb);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetCommTimeouts(IntPtr h, out COMMTIMEOUTS t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetCommTimeouts(IntPtr h, ref COMMTIMEOUTS t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetCommMask(IntPtr h, uint mask);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetCommModemStatus(IntPtr h, out uint status);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool EscapeCommFunction(IntPtr h, uint function);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool PurgeComm(IntPtr h, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteFile(IntPtr h, byte[] buffer, uint length, out uint written, IntPtr overlapped);

    public const int InvalidHandle = -1;

    /// Opens \\.\COMn for overlapped I/O. Returns IntPtr(-1) and the error on failure.
    public static IntPtr Open(string port, out int error)
    {
        IntPtr h = CreateFile("\\\\.\\" + port, 0xC0000000, 0, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        error = h == new IntPtr(-1) ? Marshal.GetLastWin32Error() : 0;
        return h;
    }

    public static void Close(IntPtr h) { CloseHandle(h); }

    public static bool SetState(IntPtr h, uint baud, byte bits, byte parity, byte stop)
    {
        var dcb = new DCB { DCBlength = (uint)Marshal.SizeOf(typeof(DCB)) };
        if (!GetCommState(h, ref dcb)) return false;
        dcb.BaudRate = baud; dcb.ByteSize = bits; dcb.Parity = parity; dcb.StopBits = stop;
        return SetCommState(h, ref dcb);
    }

    /// baud, bits, parity, stop; null when the call fails.
    public static uint[] GetState(IntPtr h)
    {
        var dcb = new DCB { DCBlength = (uint)Marshal.SizeOf(typeof(DCB)) };
        if (!GetCommState(h, ref dcb)) return null;
        return new uint[] { dcb.BaudRate, dcb.ByteSize, dcb.Parity, dcb.StopBits };
    }

    public static bool SetTimeouts(IntPtr h, uint interval, uint rmul, uint rconst, uint wmul, uint wconst)
    {
        var t = new COMMTIMEOUTS { ReadInterval = interval, ReadMultiplier = rmul, ReadConstant = rconst, WriteMultiplier = wmul, WriteConstant = wconst };
        return SetCommTimeouts(h, ref t);
    }

    public static uint[] GetTimeouts(IntPtr h)
    {
        COMMTIMEOUTS t;
        if (!GetCommTimeouts(h, out t)) return null;
        return new uint[] { t.ReadInterval, t.ReadMultiplier, t.ReadConstant, t.WriteMultiplier, t.WriteConstant };
    }

    public static bool SetMask(IntPtr h, uint mask) { return SetCommMask(h, mask); }
    public static bool Escape(IntPtr h, uint function) { return EscapeCommFunction(h, function); }
    public static bool Purge(IntPtr h, uint flags) { return PurgeComm(h, flags); }

    public static uint ModemStatus(IntPtr h)
    {
        uint status;
        if (!GetCommModemStatus(h, out status)) throw new Win32Exception(Marshal.GetLastWin32Error(), "GetCommModemStatus");
        return status;
    }

    /// Writes with an overlapped handle and waits for the result.
    public static int Write(IntPtr h, byte[] data)
    {
        // Allocated with the OVERLAPPED so the write may complete after the call returns.
        IntPtr ov = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));
        IntPtr ev = CreateEventW(IntPtr.Zero, true, false, null);
        try
        {
            Marshal.StructureToPtr(new OVERLAPPED { Event = ev }, ov, false);
            uint written;
            if (!WriteFile(h, data, (uint)data.Length, out written, ov))
            {
                if (Marshal.GetLastWin32Error() != 997) return -1;
                if (!GetOverlappedResult(h, ov, out written, true)) return -1;
            }
            return (int)written;
        }
        finally { CloseHandle(ev); Marshal.FreeHGlobal(ov); }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetOverlappedResult(IntPtr h, IntPtr overlapped, out uint transferred, bool wait);
    [DllImport("kernel32.dll", SetLastError = true, EntryPoint = "CreateEventW", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateEventW(IntPtr attributes, bool manual, bool initial, string name);
}

/// The hub end of a port: the second handle, opened by the interface path.
public sealed class HubIo : IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    struct OVERLAPPED { public IntPtr Internal, InternalHigh; public uint Offset, OffsetHigh; public IntPtr Event; }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "CreateFileW")]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(IntPtr h, IntPtr buffer, uint length, IntPtr read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteFile(IntPtr h, IntPtr buffer, uint length, IntPtr written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr input, uint inSize, IntPtr output, uint outSize, IntPtr returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetOverlappedResult(IntPtr h, IntPtr overlapped, out uint transferred, bool wait);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CancelIoEx(IntPtr h, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "CreateEventW")]
    static extern IntPtr CreateEventW(IntPtr attributes, bool manual, bool initial, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr h, uint ms);

    const uint IOCTL_GET_STATE = 0x80002004;
    const uint IOCTL_WAIT_STATE = 0x80002008;
    const uint IOCTL_SET_LINES = 0x8000200C;
    const uint IOCTL_REVOKE = 0x80002010;
    // Version 4 of the driver: VSSCOM_HUB_LINE_ERROR { Errors, HasByte, Byte }, three 32-bit values.
    const uint IOCTL_LINE_ERROR = 0x80002014;
    // Version 3 of the driver adds AppProcessId to the state: twelve words.
    const int StateWords = 12;
    public const int TimedOut = 258;

    IntPtr handle;

    HubIo(IntPtr h) { handle = h; }

    /// Opens the hub end. Returns null and the Win32 error on failure.
    public static HubIo Open(string path, out int error)
    {
        IntPtr h = CreateFileW(path, 0xC0000000, 0, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        error = h == new IntPtr(-1) ? Marshal.GetLastWin32Error() : 0;
        return h == new IntPtr(-1) ? null : new HubIo(h);
    }

    /// Finishes an overlapped call. Returns the byte count; -1 with `error` set when it
    /// failed or did not finish in `ms`, in which case it is cancelled first.
    int Finish(IntPtr ov, IntPtr signal, bool started, int ms, out int error)
    {
        return FinishOn(handle, ov, signal, started ? 0 : Marshal.GetLastWin32Error(), ms, out error);
    }

    // `startError` is what the call that started the request left: 0 when it finished at once, 997
    // when it is pending, otherwise the error it failed with. It is a parameter, and `h` is the
    // handle the request was started on, so that a request started on one thread can be finished
    // on another, and after Dispose.
    static int FinishOn(IntPtr h, IntPtr ov, IntPtr signal, int startError, int ms, out int error)
    {
        error = 0;
        uint n;
        if (startError != 0)
        {
            if (startError != 997) { error = startError; return -1; }
            if (WaitForSingleObject(signal, (uint)ms) != 0)
            {
                CancelIoEx(h, ov);
                // The buffers must outlive the cancel, so wait for it to land.
                if (GetOverlappedResult(h, ov, out n, true) && n > 0) return (int)n;
                error = TimedOut;
                return -1;
            }
        }
        if (!GetOverlappedResult(h, ov, out n, true)) { error = Marshal.GetLastWin32Error(); return -1; }
        return (int)n;
    }

    IntPtr NewOverlapped(out IntPtr signal)
    {
        signal = CreateEventW(IntPtr.Zero, true, false, null);
        IntPtr ov = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));
        Marshal.StructureToPtr(new OVERLAPPED { Event = signal }, ov, false);
        return ov;
    }

    /// Writes all of `data`, waiting for the application to read. Returns the count, or -1.
    public int Write(byte[] data, int ms, out int error)
    {
        IntPtr signal;
        IntPtr ov = NewOverlapped(out signal);
        IntPtr buffer = Marshal.AllocHGlobal(Math.Max(data.Length, 1));
        try
        {
            Marshal.Copy(data, 0, buffer, data.Length);
            bool started = WriteFile(handle, buffer, (uint)data.Length, IntPtr.Zero, ov);
            return Finish(ov, signal, started, ms, out error);
        }
        finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); Marshal.FreeHGlobal(buffer); }
    }

    /// Reads what the application wrote: at least one byte and at most `max`. Null on timeout or error.
    public byte[] Read(int max, int ms, out int error)
    {
        IntPtr signal;
        IntPtr ov = NewOverlapped(out signal);
        IntPtr buffer = Marshal.AllocHGlobal(max);
        try
        {
            bool started = ReadFile(handle, buffer, (uint)max, IntPtr.Zero, ov);
            int n = Finish(ov, signal, started, ms, out error);
            if (n < 0) return null;
            var bytes = new byte[n];
            Marshal.Copy(buffer, bytes, 0, n);
            return bytes;
        }
        finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); Marshal.FreeHGlobal(buffer); }
    }

    public bool SetLines(uint lines)
    {
        IntPtr signal;
        IntPtr ov = NewOverlapped(out signal);
        IntPtr input = Marshal.AllocHGlobal(4);
        try
        {
            Marshal.WriteInt32(input, (int)lines);
            int error;
            bool started = DeviceIoControl(handle, IOCTL_SET_LINES, input, 4, IntPtr.Zero, 0, IntPtr.Zero, ov);
            return Finish(ov, signal, started, 5000, out error) >= 0;
        }
        finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); Marshal.FreeHGlobal(input); }
    }

    uint[] StateCall(uint code, uint known, bool hasInput, int ms)
    {
        IntPtr signal;
        IntPtr ov = NewOverlapped(out signal);
        IntPtr input = Marshal.AllocHGlobal(4);
        IntPtr output = Marshal.AllocHGlobal(StateWords * 4);
        try
        {
            Marshal.WriteInt32(input, (int)known);
            int error;
            bool started = DeviceIoControl(handle, code, hasInput ? input : IntPtr.Zero, hasInput ? 4u : 0u,
                                           output, StateWords * 4, IntPtr.Zero, ov);
            int n = Finish(ov, signal, started, ms, out error);
            if (n != StateWords * 4) return null;
            var words = new uint[StateWords];
            for (int i = 0; i < StateWords; i++) words[i] = (uint)Marshal.ReadInt32(output, i * 4);
            return words;
        }
        finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); Marshal.FreeHGlobal(input); Marshal.FreeHGlobal(output); }
    }

    /// Generation, AppOpen, BaudRate, DataBits, Parity, StopBits, Dtr, Rts, Break, Lines, TxQueued,
    /// AppProcessId.
    public uint[] GetState() { return StateCall(IOCTL_GET_STATE, 0, false, 5000); }

    /// Waits until the generation is not `known`; null when `ms` passes first.
    public uint[] WaitState(uint known, int ms) { return StateCall(IOCTL_WAIT_STATE, known, true, ms); }

    public void Dispose()
    {
        if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }

    /// Takes the port back from the program that has the COM end open (version 3). Returns the Win32
    /// error, 0 for success; `process` is the process that had the port, or 0 when none did.
    public int Revoke(out uint process)
    {
        process = 0;
        IntPtr signal;
        IntPtr ov = NewOverlapped(out signal);
        IntPtr output = Marshal.AllocHGlobal(4);
        try
        {
            Marshal.WriteInt32(output, 0);
            int error;
            bool started = DeviceIoControl(handle, IOCTL_REVOKE, IntPtr.Zero, 0, output, 4, IntPtr.Zero, ov);
            int n = Finish(ov, signal, started, 5000, out error);
            if (n != 4) return error != 0 ? error : -1;
            process = (uint)Marshal.ReadInt32(output);
            return 0;
        }
        finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); Marshal.FreeHGlobal(output); }
    }

    /// Raises a line error at the application end (version 4). `errors` is one or more SERIAL_ERROR_*
    /// bits (break 1, framing 2, overrun 4, queue overrun 8, parity 0x10); `hasByte` is 1 when `value`
    /// is the character the receiver made of it. Returns the Win32 error, 0 for success, 258 when it
    /// did not finish in `ms` (it waits behind hub writes that wait, and is cancelled then).
    public int LineError(uint errors, uint hasByte, uint value, int ms)
    {
        IntPtr signal;
        IntPtr ov = NewOverlapped(out signal);
        IntPtr input = Marshal.AllocHGlobal(12);
        try
        {
            Marshal.WriteInt32(input, 0, (int)errors);
            Marshal.WriteInt32(input, 4, (int)hasByte);
            Marshal.WriteInt32(input, 8, (int)value);
            int error;
            bool started = DeviceIoControl(handle, IOCTL_LINE_ERROR, input, 12, IntPtr.Zero, 0, IntPtr.Zero, ov);
            int n = Finish(ov, signal, started, ms, out error);
            return n < 0 ? error : 0;
        }
        finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); Marshal.FreeHGlobal(input); }
    }

    /// LineError on another thread, which starts the request too: two of these, or one and a
    /// WriteAsync, may reach the driver in either order. BeginLineError keeps the order.
    public System.Threading.Tasks.Task<int> LineErrorAsync(uint errors, uint hasByte, uint value, int ms)
    {
        return System.Threading.Tasks.Task.Factory.StartNew<int>(delegate { return LineError(errors, hasByte, value, ms); });
    }

    /// Starts the line error request on this thread and finishes it on another: when this returns,
    /// the driver has the request, so requests started one after the other stay in that order.
    /// The task gives the same value as LineError. It is not cancelled when the hub handle is
    /// disposed; the driver ends the request then, with 995.
    public System.Threading.Tasks.Task<int> BeginLineError(uint errors, uint hasByte, uint value, int ms)
    {
        IntPtr h = handle;
        IntPtr signal;
        IntPtr ov = NewOverlapped(out signal);
        IntPtr input = Marshal.AllocHGlobal(12);
        Marshal.WriteInt32(input, 0, (int)errors);
        Marshal.WriteInt32(input, 4, (int)hasByte);
        Marshal.WriteInt32(input, 8, (int)value);
        bool started = DeviceIoControl(h, IOCTL_LINE_ERROR, input, 12, IntPtr.Zero, 0, IntPtr.Zero, ov);
        int startError = started ? 0 : Marshal.GetLastWin32Error();
        return System.Threading.Tasks.Task.Factory.StartNew<int>(delegate
        {
            try
            {
                int error;
                int n = FinishOn(h, ov, signal, startError, ms, out error);
                return n < 0 ? error : 0;
            }
            finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); Marshal.FreeHGlobal(input); }
        });
    }

    /// Like BeginLineError, for a write: started on this thread, finished on another. The task gives
    /// the byte count, or -1 when the write failed or did not finish in `ms`.
    public System.Threading.Tasks.Task<int> BeginWrite(byte[] data, int ms)
    {
        IntPtr h = handle;
        IntPtr signal;
        IntPtr ov = NewOverlapped(out signal);
        IntPtr buffer = Marshal.AllocHGlobal(Math.Max(data.Length, 1));
        Marshal.Copy(data, 0, buffer, data.Length);
        bool started = WriteFile(h, buffer, (uint)data.Length, IntPtr.Zero, ov);
        int startError = started ? 0 : Marshal.GetLastWin32Error();
        return System.Threading.Tasks.Task.Factory.StartNew<int>(delegate
        {
            try
            {
                int error;
                return FinishOn(h, ov, signal, startError, ms, out error);
            }
            finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); Marshal.FreeHGlobal(buffer); }
        });
    }

    /// Writes to the application on another thread.
    public System.Threading.Tasks.Task<int> WriteAsync(byte[] data, int ms)
    {
        return System.Threading.Tasks.Task.Factory.StartNew<int>(delegate { int error; return Write(data, ms, out error); });
    }

    /// Writes to an application handle on another thread.
    public static System.Threading.Tasks.Task<int> WriteAsync(IntPtr appHandle, byte[] data)
    {
        return System.Threading.Tasks.Task.Factory.StartNew<int>(delegate { return VssComm.Write(appHandle, data); });
    }
}

/// A handle on the control device. Every call returns the Win32 error, 0 for success.
public sealed class CtlIo : IDisposable
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "CreateFileW")]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr input, uint inSize, IntPtr output, uint outSize, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr p);
    [DllImport("advapi32.dll")]
    static extern uint GetSecurityInfo(IntPtr h, int objectType, uint info, IntPtr owner, IntPtr group, out IntPtr dacl, IntPtr sacl, out IntPtr descriptor);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ConvertSecurityDescriptorToStringSecurityDescriptorW(IntPtr descriptor, uint revision, uint info, out IntPtr text, out uint length);

    const uint IOCTL_GET_VERSION = 0x80002040;
    const uint IOCTL_CREATE_PORT = 0x80002044;
    const uint IOCTL_DESTROY_PORT = 0x80002048;
    const uint IOCTL_LIST_PORTS = 0x8000204C;
    public const int MaxPorts = 32;

    IntPtr handle;

    CtlIo(IntPtr h) { handle = h; }

    public static CtlIo Open(string path, out int error)
    {
        IntPtr h = CreateFileW(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        error = h == new IntPtr(-1) ? Marshal.GetLastWin32Error() : 0;
        return h == new IntPtr(-1) ? null : new CtlIo(h);
    }

    int Call(uint code, uint[] input, uint[] output)
    {
        int inBytes = input == null ? 0 : input.Length * 4;
        int outBytes = output == null ? 0 : output.Length * 4;
        IntPtr inp = Marshal.AllocHGlobal(Math.Max(inBytes, 1));
        IntPtr outp = Marshal.AllocHGlobal(Math.Max(outBytes, 1));
        try
        {
            for (int i = 0; i < inBytes / 4; i++) Marshal.WriteInt32(inp, i * 4, (int)input[i]);
            uint returned;
            if (!DeviceIoControl(handle, code, inp, (uint)inBytes, outp, (uint)outBytes, out returned, IntPtr.Zero))
                return Marshal.GetLastWin32Error();
            for (int i = 0; i < Math.Min(returned, (uint)outBytes) / 4; i++) output[i] = (uint)Marshal.ReadInt32(outp, i * 4);
            return 0;
        }
        finally { Marshal.FreeHGlobal(inp); Marshal.FreeHGlobal(outp); }
    }

    public int GetVersion(out uint version, out uint maxPorts)
    {
        var o = new uint[2];
        int e = Call(IOCTL_GET_VERSION, null, o);
        version = o[0]; maxPorts = o[1];
        return e;
    }

    /// Asks for a port. `flags` must be 0; other values test the refusal.
    public int Create(uint flags, out uint slot)
    {
        var o = new uint[1];
        int e = Call(IOCTL_CREATE_PORT, new uint[] { flags, 0 }, o);
        slot = o[0];
        return e;
    }

    public int Destroy(uint slot) { return Call(IOCTL_DESTROY_PORT, new uint[] { slot }, null); }

    /// The ports in use as slot, mine pairs.
    public int List(out uint[] slotAndMine)
    {
        var o = new uint[1 + 2 * MaxPorts];
        int e = Call(IOCTL_LIST_PORTS, null, o);
        var pairs = new uint[e == 0 ? o[0] * 2 : 0];
        for (int i = 0; i < pairs.Length; i++) pairs[i] = o[1 + i];
        slotAndMine = pairs;
        return e;
    }

    /// The access list of the handle's device, as an SDDL string; null when it cannot be read.
    public string Sddl()
    {
        IntPtr dacl, descriptor;
        // SE_KERNEL_OBJECT = 6, DACL_SECURITY_INFORMATION = 4
        if (GetSecurityInfo(handle, 6, 4, IntPtr.Zero, IntPtr.Zero, out dacl, IntPtr.Zero, out descriptor) != 0) return null;
        try
        {
            IntPtr text; uint length;
            if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, 1, 4, out text, out length)) return null;
            string s = Marshal.PtrToStringUni(text);
            LocalFree(text);
            return s;
        }
        finally { LocalFree(descriptor); }
    }

    public void Dispose()
    {
        if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }
}

/// The helper service's pipe, which makes the user-mode driver's ports. It has the
/// calls of CtlIo that the tests share (Create, Destroy, Sddl, Dispose); a port
/// lives as long as the pipe that asked for it. Protocol:
/// drivers/windows/vsscom_helper/protocol.h.
public sealed class HelperIo : IDisposable
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "CreateFileW")]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetNamedPipeHandleState(IntPtr h, ref uint mode, IntPtr maxCollect, IntPtr timeout);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TransactNamedPipe(IntPtr h, IntPtr input, uint inSize, IntPtr output, uint outSize, out uint read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr p);
    [DllImport("advapi32.dll")]
    static extern uint GetSecurityInfo(IntPtr h, int objectType, uint info, IntPtr owner, IntPtr group, out IntPtr dacl, IntPtr sacl, out IntPtr descriptor);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ConvertSecurityDescriptorToStringSecurityDescriptorW(IntPtr descriptor, uint revision, uint info, out IntPtr text, out uint length);

    public const int MaxPorts = 32;
    public const uint OpCreate = 1;
    public const uint OpDestroy = 2;

    IntPtr handle;

    HelperIo(IntPtr h) { handle = h; }

    /// Opens the pipe the way the studio does: identification level only.
    public static HelperIo Open(string name, out int error)
    {
        // OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION
        IntPtr h = CreateFileW(@"\\.\pipe\" + name, 0xC0000000, 0, IntPtr.Zero, 3, 0x00100000 | 0x00010000, IntPtr.Zero);
        error = h == new IntPtr(-1) ? Marshal.GetLastWin32Error() : 0;
        if (h == new IntPtr(-1)) return null;
        uint mode = 2; // PIPE_READMODE_MESSAGE
        if (!SetNamedPipeHandleState(h, ref mode, IntPtr.Zero, IntPtr.Zero))
        {
            error = Marshal.GetLastWin32Error();
            CloseHandle(h);
            return null;
        }
        return new HelperIo(h);
    }

    /// One exchange. Returns the Win32 error of the exchange itself, or the Status of
    /// the reply when the exchange worked.
    public int Raw(uint version, uint op, uint slot, uint reserved, out uint replySlot)
    {
        replySlot = 0;
        IntPtr inp = Marshal.AllocHGlobal(16);
        IntPtr outp = Marshal.AllocHGlobal(16);
        try
        {
            Marshal.WriteInt32(inp, 0, (int)version);
            Marshal.WriteInt32(inp, 4, (int)op);
            Marshal.WriteInt32(inp, 8, (int)slot);
            Marshal.WriteInt32(inp, 12, (int)reserved);
            uint read;
            if (!TransactNamedPipe(handle, inp, 16, outp, 16, out read, IntPtr.Zero)) return Marshal.GetLastWin32Error();
            if (read != 16) return 13; // ERROR_INVALID_DATA
            replySlot = (uint)Marshal.ReadInt32(outp, 8);
            return Marshal.ReadInt32(outp, 4);
        }
        finally { Marshal.FreeHGlobal(inp); Marshal.FreeHGlobal(outp); }
    }

    /// Asks for a port. `flags` must be 0; other values test the refusal.
    public int Create(uint flags, out uint slot) { return Raw(1, OpCreate, 0, flags, out slot); }

    public int Destroy(uint slot) { uint ignored; return Raw(1, OpDestroy, slot, 0, out ignored); }

    /// The access list of the pipe, as an SDDL string; null when it cannot be read.
    public string Sddl()
    {
        IntPtr dacl, descriptor;
        // SE_KERNEL_OBJECT = 6, DACL_SECURITY_INFORMATION = 4
        if (GetSecurityInfo(handle, 6, 4, IntPtr.Zero, IntPtr.Zero, out dacl, IntPtr.Zero, out descriptor) != 0) return null;
        try
        {
            IntPtr text; uint length;
            if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, 1, 4, out text, out length)) return null;
            string s = Marshal.PtrToStringUni(text);
            LocalFree(text);
            return s;
        }
        finally { LocalFree(descriptor); }
    }

    public void Dispose()
    {
        if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }
}
'@

# Win32 constants.
$MS_CTS = 0x10; $MS_DSR = 0x20; $MS_DCD = 0x80
$EV_RXCHAR = 0x1; $EV_TXEMPTY = 0x4; $EV_CTS = 0x8; $EV_DSR = 0x10
$SETDTR = 5; $CLRDTR = 6; $SETRTS = 3; $CLRRTS = 4
$PURGE_RXABORT = 0x2; $PURGE_RXCLEAR = 0x8
$ERROR_ACCESS_DENIED = 5; $ERROR_OPERATION_ABORTED = 995
$MAX = [uint32]::MaxValue

$tests = @()

# Checks that are about the kernel-mode driver's control device itself. The
# user-mode driver has none; H14 checks the helper service that stands in for it.
$KmdfOnly = @{
    D14 = 'the user-mode driver has no control device (H14 checks its helper service)'
}
# Checks that make ports on demand. The kernel-mode driver's control device does
# it; the user-mode driver needs the helper service, installed with
# install-driver.ps1 -Helper, and these are skipped without it.
$NeedsDemand = 'D15', 'D16', 'D17', 'D18', 'D19', 'D21'
$HelperRunning = [bool]([IO.Directory]::GetFiles('\\.\pipe\') -contains "\\.\pipe\$HelperPipeName")
# Checks of the COM number, which the helper service gives for either driver: they
# are skipped when it is not running.
$NeedsHelper = 'D28', 'D29', 'D30', 'D31'
# Checks of the line error request (driver version 4). They are skipped when the driver is older: a
# probe before D32 finds out. Set to $true here so that nothing is skipped before the probe has run.
$NeedsLineErrors = 'D32', 'D33', 'D34', 'D35', 'D36', 'D37', 'D38', 'D39', 'D40', 'D41'
$LineErrorsSupported = $true
# Checks of the helper service alone.
$HelperOnly = @{
    H14 = 'the kernel-mode driver has no helper service'
}

function Add-Test([string] $Id, [string] $Name, [scriptblock] $Body) {
    if ($Flavour -eq 'umdf' -and $KmdfOnly.ContainsKey($Id)) {
        $script:tests += New-TestRecord $Id $Name 'skip' $KmdfOnly[$Id]
        return
    }
    if ($Flavour -ne 'umdf' -and $HelperOnly.ContainsKey($Id)) {
        $script:tests += New-TestRecord $Id $Name 'skip' $HelperOnly[$Id]
        return
    }
    if ($Id -in $NeedsHelper -and -not $HelperRunning) {
        $script:tests += New-TestRecord $Id $Name 'skip' 'the helper service is not running (install-driver.ps1 -Helper)'
        return
    }
    if ($Id -in $NeedsLineErrors -and -not $LineErrorsSupported) {
        $script:tests += New-TestRecord $Id $Name 'skip' 'the installed driver is older than version 4 and has no line error request'
        return
    }
    if ($Flavour -eq 'umdf' -and ($Id -in $NeedsDemand -or $Id -eq 'H14') -and -not $HelperRunning) {
        $script:tests += New-TestRecord $Id $Name 'skip' 'the helper service is not running (install-driver.ps1 -Helper)'
        return
    }
    try {
        $detail = & $Body
        $script:tests += New-TestRecord $Id $Name 'pass' ($detail -join '; ')
    } catch {
        $script:tests += New-TestRecord $Id $Name 'fail' $_.Exception.Message
    }
}
function Assert([bool] $Condition, [string] $Message) { if (-not $Condition) { throw $Message } }

function Open-Port([string] $Name) {
    $error_ = 0
    $h = [VssComm]::Open($Name, [ref]$error_)
    if ($h -eq [IntPtr](-1)) { throw "CreateFile($Name) failed with error $error_" }
    $h
}

function Read-Now([IntPtr] $H, [int] $Length) {
    # Reads once and returns the bytes and how long the call took.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $io = [PendingIo]::BeginRead($H, $Length)
    try {
        if (-not $io.Completed(10000)) { $io.Cancel() | Out-Null; throw 'a read did not finish in 10 seconds' }
        $err = 0
        $n = $io.Finish($true, [ref]$err)
        [pscustomobject]@{ Bytes = $io.Data($n); Count = $n; Error = $err; Ms = $watch.ElapsedMilliseconds }
    } finally { $io.Dispose() }
}

$port = Get-VssComPort
if (-not $port) {
    $tests += New-TestRecord 'D0' 'a vsscom port is installed' 'fail' 'run install-driver.ps1 first'
    Write-Result -Path $ResultPath -Phase 'test' -Tests $tests
    $tests | ForEach-Object { '{0,-6} {1,-4} {2}' -f $_.status, $_.id, $_.detail }
    exit 1
}
$tests += New-TestRecord 'D0' 'a vsscom port is installed' 'pass' $port

Add-Test 'D1' 'the Ports device, SERIALCOMM and GetPortNames agree' {
    $listed = (Get-ItemProperty 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM').PSObject.Properties | ForEach-Object { $_.Value }
    Assert ($listed -contains $port) "$port is not in SERIALCOMM"
    $names = [System.IO.Ports.SerialPort]::GetPortNames()
    Assert ($names -contains $port) "$port is not in GetPortNames: $($names -join ',')"
    # Windows lists a port in Win32_SerialPort only for the inbox serial stack, so
    # a function driver such as this one is not in it (seen on Windows 11 25H2).
    # Programs that find ports through SetupAPI, SERIALCOMM or GetPortNames, as
    # pyserial and Qt do, see it. The test therefore checks the Ports device
    # itself and only reports what the WMI class says.
    $device = Get-PnpDevice -Class Ports -PresentOnly |
        Where-Object { $_.FriendlyName -like "*($port)" }
    Assert ($null -ne $device) "no Ports device is named for $port"
    Assert ($device.Status -eq 'OK') "the Ports device status is $($device.Status)"
    $serial = @(Get-CimInstance Win32_SerialPort | ForEach-Object { $_.DeviceID })
    $wmi = if ($serial -contains $port) { 'listed' } else { 'not listed' }
    "port $port; Win32_SerialPort $wmi"
}

Add-Test 'D2' 'line settings round-trip and bytes loop back' {
    $h = Open-Port $port
    try {
        foreach ($case in @(@(9600, 8, 0, 0), @(57600, 7, 2, 2), @(115200, 8, 1, 0))) {
            Assert ([VssComm]::SetState($h, $case[0], $case[1], $case[2], $case[3])) "SetCommState failed for $($case -join '/')"
            $got = [VssComm]::GetState($h)
            Assert ($null -ne $got) 'GetCommState failed'
            Assert (($got -join '/') -eq ($case -join '/')) "set $($case -join '/') but read $($got -join '/')"
        }
        Assert ([VssComm]::SetTimeouts($h, $MAX, 0, 0, 0, 0)) 'SetCommTimeouts failed'
        $sent = [VssComm]::Write($h, [Text.Encoding]::ASCII.GetBytes('hello'))
        Assert ($sent -eq 5) "wrote $sent bytes"
        $r = Read-Now $h 5
        Assert ($r.Count -eq 5 -and [Text.Encoding]::ASCII.GetString($r.Bytes) -eq 'hello') "read back $($r.Count) bytes"
        'round trips and loopback'
    } finally { [VssComm]::Close($h) }
}

Add-Test 'D3' 'read timeouts follow SetCommTimeouts' {
    $h = Open-Port $port
    $notes = @()
    try {
        # Interval MAXDWORD alone: return at once, even with nothing to read.
        [VssComm]::SetTimeouts($h, $MAX, 0, 0, 0, 0) | Out-Null
        $r = Read-Now $h 8
        Assert ($r.Count -eq 0 -and $r.Ms -lt 300) "immediate read took $($r.Ms) ms and got $($r.Count)"
        $notes += "immediate $($r.Ms) ms"

        # A constant total timeout: wait about that long for nothing.
        [VssComm]::SetTimeouts($h, 0, 0, 400, 0, 0) | Out-Null
        $r = Read-Now $h 8
        Assert ($r.Count -eq 0 -and $r.Ms -ge 300 -and $r.Ms -lt 1500) "400 ms timeout took $($r.Ms) ms and got $($r.Count)"
        $notes += "total 400 ms took $($r.Ms) ms"

        # Per-byte multiplier plus constant, with 5 of 20 bytes available.
        [VssComm]::SetTimeouts($h, 0, 20, 100, 0, 0) | Out-Null
        [VssComm]::Write($h, [Text.Encoding]::ASCII.GetBytes('abcde')) | Out-Null
        $r = Read-Now $h 20
        Assert ($r.Count -eq 5) "multiplier read returned $($r.Count) bytes"
        Assert ($r.Ms -ge 400 -and $r.Ms -lt 2000) "20x20+100 ms timeout took $($r.Ms) ms"
        $notes += "multiplier $($r.Ms) ms"

        # Interval timeout: the gap after the first byte ends the read.
        [VssComm]::SetTimeouts($h, 150, 0, 0, 0, 0) | Out-Null
        [VssComm]::Write($h, [Text.Encoding]::ASCII.GetBytes('xy')) | Out-Null
        $r = Read-Now $h 10
        Assert ($r.Count -eq 2 -and $r.Ms -ge 100 -and $r.Ms -lt 1500) "interval read returned $($r.Count) in $($r.Ms) ms"
        $notes += "interval $($r.Ms) ms"

        # MAXDWORD, MAXDWORD, constant: return as soon as one byte is there.
        [VssComm]::SetTimeouts($h, $MAX, $MAX, 1000, 0, 0) | Out-Null
        $io = [PendingIo]::BeginRead($h, 10)
        try {
            Start-Sleep -Milliseconds 150
            Assert (-not $io.Completed(0)) 'the read finished before any byte arrived'
            [VssComm]::Write($h, [Text.Encoding]::ASCII.GetBytes('z')) | Out-Null
            Assert ($io.Completed(800)) 'the read did not finish when a byte arrived'
            $err = 0
            $n = $io.Finish($true, [ref]$err)
            Assert ($n -eq 1) "first-byte read returned $n bytes"
        } finally { $io.Dispose() }
        $notes += 'first byte'

        # All three at MAXDWORD is refused.
        Assert (-not [VssComm]::SetTimeouts($h, $MAX, $MAX, $MAX, 0, 0)) 'SetCommTimeouts accepted all MAXDWORD'
        $got = [VssComm]::GetTimeouts($h)
        Assert (($got -join ',') -eq "$MAX,$MAX,1000,0,0") "timeouts changed to $($got -join ',')"
        $notes
    } finally { [VssComm]::Close($h) }
}

Add-Test 'D4' 'a waiting read can be cancelled, purged and closed' {
    $notes = @()
    $h = Open-Port $port
    try {
        [VssComm]::SetTimeouts($h, 0, 0, 0, 0, 0) | Out-Null   # wait forever

        $io = [PendingIo]::BeginRead($h, 4)
        try {
            Assert (-not $io.Completed(200)) 'a read with nothing to read finished'
            Assert ($io.Cancel()) 'CancelIoEx failed'
            Assert ($io.Completed(2000)) 'the cancelled read did not finish'
            $err = 0
            $null = $io.Finish($true, [ref]$err)
            Assert ($err -eq $ERROR_OPERATION_ABORTED) "cancelled read ended with error $err"
        } finally { $io.Dispose() }
        $notes += 'CancelIoEx'

        $io = [PendingIo]::BeginRead($h, 4)
        try {
            Assert (-not $io.Completed(200)) 'a read with nothing to read finished'
            Assert ([VssComm]::Purge($h, $PURGE_RXABORT)) 'PurgeComm failed'
            Assert ($io.Completed(2000)) 'PurgeComm(RXABORT) did not end the read'
        } finally { $io.Dispose() }
        $notes += 'PurgeComm'

        # Data left in the buffer is dropped by RXCLEAR.
        [VssComm]::Write($h, [Text.Encoding]::ASCII.GetBytes('stale')) | Out-Null
        Assert ([VssComm]::Purge($h, $PURGE_RXCLEAR)) 'PurgeComm(RXCLEAR) failed'
        [VssComm]::SetTimeouts($h, $MAX, 0, 0, 0, 0) | Out-Null
        $r = Read-Now $h 8
        Assert ($r.Count -eq 0) "RXCLEAR left $($r.Count) bytes"
        $notes += 'RXCLEAR'
    } finally { [VssComm]::Close($h) }

    # Closing a handle that has a read waiting must not hang.
    $h = Open-Port $port
    [VssComm]::SetTimeouts($h, 0, 0, 0, 0, 0) | Out-Null
    $io = [PendingIo]::BeginRead($h, 4)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    [VssComm]::Close($h)
    Assert ($watch.ElapsedMilliseconds -lt 3000) "CloseHandle with a read waiting took $($watch.ElapsedMilliseconds) ms"
    Assert ($io.Completed(3000)) 'the read did not finish when the handle closed'
    $io.Dispose()
    $notes += "close $($watch.ElapsedMilliseconds) ms"

    # The port opens again afterwards.
    [VssComm]::Close((Open-Port $port))
    $notes
}

Add-Test 'D5' 'SetCommMask and WaitCommEvent report events once and block otherwise' {
    $notes = @()
    $h = Open-Port $port
    try {
        [VssComm]::SetTimeouts($h, $MAX, 0, 0, 0, 0) | Out-Null
        Assert ([VssComm]::SetMask($h, $EV_RXCHAR)) 'SetCommMask failed'

        $io = [PendingIo]::BeginWait($h)
        try {
            Assert (-not $io.Completed(200)) 'WaitCommEvent finished with no event'
            [VssComm]::Write($h, [Text.Encoding]::ASCII.GetBytes('e')) | Out-Null
            Assert ($io.Completed(2000)) 'WaitCommEvent did not finish when a byte arrived'
            $err = 0
            $null = $io.Finish($true, [ref]$err)
            Assert ($err -eq 0 -and ($io.Mask() -band $EV_RXCHAR) -ne 0) "event mask was 0x$('{0:x}' -f $io.Mask()), error $err"
        } finally { $io.Dispose() }
        $notes += 'EV_RXCHAR'
        $null = Read-Now $h 8

        # A new mask releases a waiting call with no events.
        $io = [PendingIo]::BeginWait($h)
        try {
            Assert (-not $io.Completed(200)) 'WaitCommEvent finished with no event'
            Assert ([VssComm]::SetMask($h, 0)) 'SetCommMask(0) failed'
            Assert ($io.Completed(2000)) 'SetCommMask(0) did not release the wait'
            $err = 0
            $null = $io.Finish($true, [ref]$err)
            Assert ($io.Mask() -eq 0) "released wait reported 0x$('{0:x}' -f $io.Mask())"
        } finally { $io.Dispose() }
        $notes += 'mask change releases'

        # A modem-line change is an event too.
        Assert ([VssComm]::SetMask($h, $EV_CTS)) 'SetCommMask(EV_CTS) failed'
        $io = [PendingIo]::BeginWait($h)
        try {
            Assert (-not $io.Completed(200)) 'WaitCommEvent finished with no event'
            [VssComm]::Escape($h, $SETRTS) | Out-Null
            Assert ($io.Completed(2000)) 'RTS did not raise EV_CTS'
            $err = 0
            $null = $io.Finish($true, [ref]$err)
            Assert (($io.Mask() -band $EV_CTS) -ne 0) "event mask was 0x$('{0:x}' -f $io.Mask())"
        } finally { $io.Dispose() }
        [VssComm]::Escape($h, $CLRRTS) | Out-Null
        $notes += 'EV_CTS'
        [VssComm]::SetMask($h, 0) | Out-Null
        $notes
    } finally { [VssComm]::Close($h) }
}

Add-Test 'D6' 'DTR and RTS come back as DSR, DCD and CTS' {
    $h = Open-Port $port
    try {
        $status = [VssComm]::ModemStatus($h)
        Assert (($status -band ($MS_CTS -bor $MS_DSR -bor $MS_DCD)) -eq 0) "lines were up at the start: 0x$('{0:x}' -f $status)"

        [VssComm]::Escape($h, $SETRTS) | Out-Null
        Assert (([VssComm]::ModemStatus($h) -band $MS_CTS) -ne 0) 'RTS did not raise CTS'
        [VssComm]::Escape($h, $SETDTR) | Out-Null
        $status = [VssComm]::ModemStatus($h)
        Assert (($status -band $MS_DSR) -ne 0 -and ($status -band $MS_DCD) -ne 0) "DTR did not raise DSR and DCD: 0x$('{0:x}' -f $status)"

        [VssComm]::Escape($h, $CLRRTS) | Out-Null
        [VssComm]::Escape($h, $CLRDTR) | Out-Null
        $status = [VssComm]::ModemStatus($h)
        Assert (($status -band ($MS_CTS -bor $MS_DSR -bor $MS_DCD)) -eq 0) "lines stayed up: 0x$('{0:x}' -f $status)"
        'RTS to CTS, DTR to DSR and DCD'
    } finally { [VssComm]::Close($h) }
}

Add-Test 'D7' 'a second open fails while the port is open' {
    $first = Open-Port $port
    try {
        $err = 0
        $second = [VssComm]::Open($port, [ref]$err)
        if ($second -ne [IntPtr](-1)) { [VssComm]::Close($second); throw 'a second CreateFile succeeded' }
        Assert ($err -eq $ERROR_ACCESS_DENIED) "second CreateFile failed with error $err, not access denied"
        "error $err"
    } finally { [VssComm]::Close($first) }
    # After the first closes, the port opens again.
    [VssComm]::Close((Open-Port $port))
}


# -- The hub end -------------------------------------------------------------

$hubs = Get-VssHubPaths
$hubPath = if ($hubs.ContainsKey($port)) { $hubs[$port] } else { $null }
$MS_RI = 0x40
$LINE_CTS = 0x10; $LINE_DSR = 0x20; $LINE_DCD = 0x80
$PURGE_TXABORT = 0x1; $PURGE_TXCLEAR = 0x4

function Open-Hub([string] $Path) {
    $e = 0
    $hub = [HubIo]::Open($Path, [ref]$e)
    if (-not $hub) { throw "opening the hub end failed with error $e" }
    $hub
}

function New-Pattern([int] $Length) {
    $bytes = New-Object byte[] $Length
    for ($i = 0; $i -lt $Length; $i++) { $bytes[$i] = [byte]($i % 251) }
    , $bytes
}

# Reads from the hub until `$Length` bytes have come, and returns them.
function Read-Hub([HubIo] $Hub, [int] $Length) {
    $all = New-Object byte[] $Length
    $got = 0
    while ($got -lt $Length) {
        $e = 0
        $chunk = $Hub.Read([Math]::Min(65536, $Length - $got), 5000, [ref]$e)
        if (-not $chunk) { throw "the hub read stopped after $got of $Length bytes (error $e)" }
        [Array]::Copy($chunk, 0, $all, $got, $chunk.Length)
        $got += $chunk.Length
    }
    , $all
}

function Read-App([IntPtr] $H, [int] $Length) {
    [VssComm]::SetTimeouts($H, 100, 0, 5000, 0, 0) | Out-Null
    (Read-Now $H $Length)
}

Add-Test 'D8' 'the hub end opens once, beside the application end' {
    Assert ($null -ne $hubPath) "no hub interface for $port"
    $hub = Open-Hub $hubPath
    try {
        $e = 0
        $second = [HubIo]::Open($hubPath, [ref]$e)
        if ($second) { $second.Dispose(); throw 'a second hub open worked' }
        Assert ($e -eq $ERROR_ACCESS_DENIED) "the second hub open failed with $e, not access denied"

        $state = $hub.GetState()
        Assert ($null -ne $state) 'GET_STATE failed'
        Assert ($state[1] -eq 0) 'AppOpen is set with no application'

        $app = Open-Port $port
        try {
            $state = $hub.WaitState($state[0], 5000)
            Assert ($null -ne $state -and $state[1] -eq 1) 'the hub was not told the application opened'
        } finally { [VssComm]::Close($app) }
        $state = $hub.WaitState($state[0], 5000)
        Assert ($null -ne $state -and $state[1] -eq 0) 'the hub was not told the application closed'
        'one handle per end; open and close reported'
    } finally { $hub.Dispose() }
}

Add-Test 'D9' 'bytes cross in both directions' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        $e = 0
        Assert ([VssComm]::Write($app, [Text.Encoding]::ASCII.GetBytes('to the hub')) -eq 10) 'the application write was short'
        $got = [Text.Encoding]::ASCII.GetString((Read-Hub $hub 10))
        Assert ($got -eq 'to the hub') "the hub read '$got'"

        Assert ($hub.Write([Text.Encoding]::ASCII.GetBytes('to the app'), 5000, [ref]$e) -eq 10) "the hub write failed ($e)"
        $r = Read-App $app 10
        Assert ($r.Count -eq 10) "the application read $($r.Count) bytes"
        $text = [Text.Encoding]::ASCII.GetString($r.Bytes)
        Assert ($text -eq 'to the app') "the application read '$text'"
        'both ways'
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D10' 'a write larger than the buffer waits for the hub to read' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        $size = 300000
        $data = New-Pattern $size
        $task = [HubIo]::WriteAsync($app, $data)
        Start-Sleep -Milliseconds 400
        Assert (-not $task.IsCompleted) 'the write finished before the hub read anything'

        $got = Read-Hub $hub $size
        Assert ($task.Wait(10000)) 'the write did not finish once the hub had read it'
        Assert ($task.Result -eq $size) "the write returned $($task.Result)"
        for ($i = 0; $i -lt $size; $i++) { if ($got[$i] -ne $data[$i]) { throw "byte $i differs" } }

        # And the other way: more than Rx holds, read by the application.
        $e = 0
        $out = New-Pattern $size
        $sender = $hub.WriteAsync($out, 20000)
        Start-Sleep -Milliseconds 400
        Assert (-not $sender.IsCompleted) 'the hub write finished before the application read anything'
        [VssComm]::SetTimeouts($app, 100, 0, 5000, 0, 0) | Out-Null
        $total = 0
        while ($total -lt $size) {
            $r = Read-Now $app ([Math]::Min(65536, $size - $total))
            if ($r.Count -eq 0) { throw "the application read stopped after $total bytes" }
            for ($i = 0; $i -lt $r.Count; $i++) { if ($r.Bytes[$i] -ne $out[$total + $i]) { throw "byte $($total + $i) differs" } }
            $total += $r.Count
        }
        Assert ($sender.Wait(10000) -and $sender.Result -eq $size) 'the hub write did not finish'
        "$size bytes each way, in order"
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D11' 'the hub drives the input lines and sees the application set DTR, RTS and the line settings' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        # With a hub, the application's RTS no longer comes back as CTS.
        [VssComm]::Escape($app, $SETRTS) | Out-Null
        $status = [VssComm]::ModemStatus($app)
        Assert (($status -band ($MS_CTS -bor $MS_DSR -bor $MS_DCD)) -eq 0) ("the lines were not low: 0x{0:X}" -f $status)

        $state = $hub.GetState()
        Assert ($state[7] -eq 1) 'the hub did not see RTS'
        Assert ($state[6] -eq 0) 'the hub saw DTR before it was set'

        [VssComm]::SetMask($app, $EV_CTS -bor $EV_DSR) | Out-Null
        $wait = [PendingIo]::BeginWait($app)
        try {
            Assert ($hub.SetLines($LINE_CTS -bor $LINE_DCD)) 'SET_LINES failed'
            Assert ($wait.Completed(5000)) 'EV_CTS did not fire when the hub raised CTS'
            $err = 0
            $wait.Finish($true, [ref]$err) | Out-Null
            Assert (($wait.Mask() -band $EV_CTS) -ne 0) ("the event was 0x{0:X}" -f $wait.Mask())
        } finally { $wait.Dispose() }
        $status = [VssComm]::ModemStatus($app)
        Assert (($status -band $MS_CTS) -ne 0 -and ($status -band $MS_DCD) -ne 0 -and ($status -band $MS_DSR) -eq 0) ("the status was 0x{0:X}" -f $status)

        $known = $hub.GetState()[0]
        [VssComm]::Escape($app, $SETDTR) | Out-Null
        $state = $hub.WaitState($known, 5000)
        Assert ($null -ne $state -and $state[6] -eq 1) 'the hub was not told DTR went high'

        [VssComm]::SetState($app, 57600, 7, 2, 2) | Out-Null
        $state = $hub.GetState()
        Assert ($state[2] -eq 57600 -and $state[3] -eq 7 -and $state[4] -eq 2 -and $state[5] -eq 2) ("the settings were {0}" -f ($state -join ','))
        'CTS, DCD and the events; DTR, RTS and the settings reach the hub'
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D12' 'the port loops back again when the hub closes' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        $size = 300000
        $task = [HubIo]::WriteAsync($app, (New-Pattern $size))
        Start-Sleep -Milliseconds 300
        Assert (-not $task.IsCompleted) 'the write did not wait for the hub'
        $hub.Dispose()
        Assert ($task.Wait(10000)) 'a waiting write did not finish when the hub closed'

        # No hub: the lines and the data loop back.
        [VssComm]::Escape($app, $SETRTS) | Out-Null
        Assert (([VssComm]::ModemStatus($app) -band $MS_CTS) -ne 0) 'RTS did not come back as CTS'
        [VssComm]::Purge($app, $PURGE_RXCLEAR) | Out-Null
        Assert ([VssComm]::Write($app, [Text.Encoding]::ASCII.GetBytes('loop')) -eq 4) 'the loopback write was short'
        $r = Read-App $app 4
        Assert ([Text.Encoding]::ASCII.GetString($r.Bytes) -eq 'loop') 'the loopback returned other bytes'
        'waiting write finished; loopback is back'
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D13' 'PurgeComm ends the writes that wait for the hub' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        $task = [HubIo]::WriteAsync($app, (New-Pattern 300000))
        Start-Sleep -Milliseconds 300
        Assert (-not $task.IsCompleted) 'the write did not wait for the hub'
        Assert ([VssComm]::Purge($app, $PURGE_TXABORT -bor $PURGE_TXCLEAR)) 'PurgeComm failed'
        Assert ($task.Wait(10000)) 'the write was not ended by the purge'
        $e = 0
        $left = $hub.Read(65536, 500, [ref]$e)
        Assert ($null -eq $left -and $e -eq [HubIo]::TimedOut) 'the purge left bytes for the hub'
        'purge ended the write and emptied the queue'
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D20' 'a write that waits for the hub ends when the write timeout is up' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        # No multiplier, 500 ms in all. The hub reads nothing, so Tx fills.
        Assert ([VssComm]::SetTimeouts($app, 0, 0, 0, 0, 500)) 'SetCommTimeouts failed'
        $size = 300000
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $written = [VssComm]::Write($app, (New-Pattern $size))
        $ms = $watch.ElapsedMilliseconds
        Assert ($written -ge 0 -and $written -lt $size) "the write returned $written of $size bytes"
        Assert ($ms -ge 400 -and $ms -lt 5000) "the write took $ms ms for a 500 ms timeout"

        # What was taken before the timeout is still there for the hub, in order.
        $got = Read-Hub $hub $written
        $want = New-Pattern $written
        for ($i = 0; $i -lt $written; $i++) { if ($got[$i] -ne $want[$i]) { throw "byte $i differs" } }

        # With the queue empty a write that fits goes straight through.
        Assert ([VssComm]::Write($app, [Text.Encoding]::ASCII.GetBytes('after')) -eq 5) 'a write after the timeout was short'
        Assert ([Text.Encoding]::ASCII.GetString((Read-Hub $hub 5)) -eq 'after') 'the hub read other bytes'

        # And a timeout of 0 means wait: the write is still waiting after a second.
        Assert ([VssComm]::SetTimeouts($app, 0, 0, 0, 0, 0)) 'SetCommTimeouts failed'
        $task = [HubIo]::WriteAsync($app, (New-Pattern $size))
        Start-Sleep -Milliseconds 1000
        Assert (-not $task.IsCompleted) 'a write with no timeout ended by itself'
        [VssComm]::Purge($app, $PURGE_TXABORT -bor $PURGE_TXCLEAR) | Out-Null
        Assert ($task.Wait(10000)) 'the purge did not end the write'
        "took $written of $size bytes in $ms ms; later writes unaffected; 0 means wait"
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

# -- The control device ------------------------------------------------------

$ERROR_NOT_FOUND = 1168; $ERROR_INVALID_PARAMETER = 87; $ERROR_NO_SYSTEM_RESOURCES = 1450
$ERROR_NOT_ENOUGH_QUOTA = 1816
$VssCtlPath = Get-VssControlPath

# What asks for ports: the kernel-mode driver's control device, or the user-mode
# driver's helper service. Both answer Create and Destroy with a Win32 error, and
# both end the ports they made when the handle is disposed.
function Open-Ctl {
    $e = 0
    if ($Flavour -eq 'umdf') {
        $helper = [HelperIo]::Open($HelperPipeName, [ref]$e)
        if (-not $helper) { throw "opening the helper's pipe failed with error $e (install-driver.ps1 -Helper)" }
        return $helper
    }
    if (-not $VssCtlPath) { throw 'the control device has no interface (run install-driver.ps1)' }
    $ctl = [CtlIo]::Open($VssCtlPath, [ref]$e)
    if (-not $ctl) { throw "opening the control device failed with error $e" }
    $ctl
}

# Asks for a port and waits for it to appear. Returns its slot, COM name and hub path.
function New-DemandPort($Ctl) {
    $slot = [uint32]0
    $e = $Ctl.Create(0, [ref]$slot)
    if ($e -ne 0) { throw "CREATE_PORT failed with error $e" }
    $found = Wait-VssDemandPort $slot 60
    if (-not $found) { throw "the port in slot $slot did not appear within 60 seconds" }
    [pscustomobject]@{ Slot = $slot; Port = $found.Port; Path = $found.Path }
}

Add-Test 'D14' 'the control device answers, refuses bad requests and is open to signed-in users only' {
    $ctl = Open-Ctl
    try {
        $version = [uint32]0; $max = [uint32]0
        $e = $ctl.GetVersion([ref]$version, [ref]$max)
        Assert ($e -eq 0) "GET_VERSION failed with error $e"
        Assert ($version -eq 1 -and $max -eq 32) "version $version with room for $max ports"

        $slot = [uint32]0
        $e = $ctl.Create(1, [ref]$slot)
        Assert ($e -eq $ERROR_INVALID_PARAMETER) "CREATE_PORT with unknown flags gave error $e"
        $e = $ctl.Destroy(99)
        Assert ($e -eq $ERROR_INVALID_PARAMETER) "DESTROY_PORT of slot 99 gave error $e"

        $sddl = $ctl.Sddl()
        Assert ([bool]$sddl) 'the access list could not be read'
        Assert ($sddl -match ';;;AU\)') "no access for signed-in users in $sddl"
        Assert ($sddl -notmatch ';;;(WD|BU|AN|IU)\)') "access for more than signed-in users in $sddl"
        "version $version, $max ports at most; $sddl"
    } finally { $ctl.Dispose() }
}

$ERROR_NOT_SUPPORTED = 50

Add-Test 'H14' 'the helper service answers, refuses bad requests and is open to signed-in users only' {
    $service = Get-CimInstance Win32_Service -Filter "Name='$HelperServiceName'"
    Assert ($service -and $service.State -eq 'Running') 'the helper service is not running'
    Assert ($service.StartName -eq 'LocalSystem') "the helper runs as '$($service.StartName)', not as SYSTEM"

    $helper = Open-Ctl
    try {
        $slot = [uint32]0
        $e = $helper.Raw(2, [HelperIo]::OpCreate, 0, 0, [ref]$slot)
        Assert ($e -eq $ERROR_NOT_SUPPORTED) "a request of version 2 gave error $e"
        $e = $helper.Raw(1, 99, 0, 0, [ref]$slot)
        Assert ($e -eq $ERROR_NOT_SUPPORTED) "an unknown operation gave error $e"
        $e = $helper.Create(1, [ref]$slot)
        Assert ($e -eq $ERROR_NOT_SUPPORTED) "a create request with a reserved field set gave error $e"
        $e = $helper.Raw(1, [HelperIo]::OpCreate, 5, 0, [ref]$slot)
        Assert ($e -eq $ERROR_NOT_SUPPORTED) "a create request that names a slot gave error $e"
        $e = $helper.Destroy(99)
        Assert ($e -eq $ERROR_NOT_FOUND) "destroying a slot this pipe does not hold gave error $e"

        $sddl = $helper.Sddl()
        Assert ([bool]$sddl) 'the access list could not be read'
        Assert ($sddl -match ';;;AU\)') "no access for signed-in users in $sddl"
        Assert ($sddl -notmatch ';;;(WD|BU|AN|IU)\)') "access for more than signed-in users in $sddl"
        "runs as SYSTEM; refuses what it does not know; $sddl"
    } finally { $helper.Dispose() }
}

Add-Test 'D15' 'a port made on demand appears, has both ends and carries bytes' {
    $ctl = Open-Ctl
    $p = $null
    try {
        $p = New-DemandPort $ctl
        Assert ($p.Port -match '^COM\d+$') "the port is called '$($p.Port)'"
        Assert (Test-VssSerialComm $p.Port) "$($p.Port) is not in SERIALCOMM"

        $hub = Open-Hub $p.Path
        $app = Open-Port $p.Port
        try {
            $e = 0
            Assert ([VssComm]::Write($app, [Text.Encoding]::ASCII.GetBytes('on demand')) -eq 9) 'the application write was short'
            $got = [Text.Encoding]::ASCII.GetString((Read-Hub $hub 9))
            Assert ($got -eq 'on demand') "the hub read '$got'"
            Assert ($hub.Write([Text.Encoding]::ASCII.GetBytes('answer'), 5000, [ref]$e) -eq 6) "the hub write failed ($e)"
            $r = Read-App $app 6
            Assert ([Text.Encoding]::ASCII.GetString($r.Bytes) -eq 'answer') 'the application read other bytes'
        } finally { [VssComm]::Close($app); $hub.Dispose() }
        "slot $($p.Slot) is $($p.Port); bytes cross both ways"
    } finally {
        $ctl.Dispose()
        if ($p) { Wait-VssDemandPort $p.Slot 60 -Gone | Out-Null }
    }
}

Add-Test 'D16' 'closing the handle that made a port removes it, and the port comes back under the same name' {
    $ctl = Open-Ctl
    $again = $null
    $q = $null
    try {
        $p = New-DemandPort $ctl
        $ctl.Dispose()
        Assert (Wait-VssDemandPort $p.Slot 60 -Gone) "the port was still there 60 seconds after its handle closed"
        Assert (Wait-VssSerialCommGone $p.Port) "$($p.Port) is still in SERIALCOMM"
        $e = 0
        $h = [VssComm]::Open($p.Port, [ref]$e)
        if ($h -ne [IntPtr](-1)) { [VssComm]::Close($h); throw "$($p.Port) still opens" }

        $again = Open-Ctl
        $q = New-DemandPort $again
        Assert ($q.Slot -eq $p.Slot -and $q.Port -eq $p.Port) "the port came back as slot $($q.Slot), $($q.Port), not slot $($p.Slot), $($p.Port)"
        "slot $($p.Slot) went and came back as $($p.Port)"
    } finally {
        $ctl.Dispose()
        if ($again) { $again.Dispose() }
        if ($q) { Wait-VssDemandPort $q.Slot 60 -Gone | Out-Null }
    }
}

Add-Test 'D17' 'only the handle that made a port can destroy it' {
    $a = Open-Ctl
    $b = Open-Ctl
    $pb = $null
    try {
        $pa = New-DemandPort $a
        $pb = New-DemandPort $b
        Assert ($pa.Slot -ne $pb.Slot) 'two ports share a slot'

        # The control device says the port is not this handle's to destroy; the
        # helper does not tell one pipe that another pipe holds a slot.
        $refused = if ($Flavour -eq 'umdf') { $ERROR_NOT_FOUND } else { $ERROR_ACCESS_DENIED }
        $e = $b.Destroy($pa.Slot)
        Assert ($e -eq $refused) "another handle destroying the port gave error $e"

        if ($Flavour -ne 'umdf') {
            # Only the control device lists the ports and whose they are.
            $pairs = [uint32[]]@()
            $e = $b.List([ref]$pairs)
            Assert ($e -eq 0) "LIST_PORTS failed with error $e"
            $mine = @{}
            for ($i = 0; $i -lt $pairs.Count; $i += 2) { $mine[[int]$pairs[$i]] = [int]$pairs[$i + 1] }
            Assert ($mine[[int]$pa.Slot] -eq 0 -and $mine[[int]$pb.Slot] -eq 1) ("the list was " + (($mine.GetEnumerator() | ForEach-Object { "$($_.Key):$($_.Value)" }) -join ' '))
        }

        $e = $a.Destroy($pa.Slot)
        Assert ($e -eq 0) "destroying the port gave error $e"
        Assert (Wait-VssDemandPort $pa.Slot 60 -Gone) 'the destroyed port stayed'
        Assert ($null -ne (Find-VssDemandPort $pb.Slot)) 'the other port went too'
        $e = $a.Destroy($pa.Slot)
        Assert ($e -eq $ERROR_NOT_FOUND) "destroying a destroyed port gave error $e"
        "slot $($pa.Slot) destroyed by its maker only; slot $($pb.Slot) stayed"
    } finally {
        $a.Dispose(); $b.Dispose()
        if ($pb) { Wait-VssDemandPort $pb.Slot 60 -Gone | Out-Null }
    }
}

Add-Test 'D18' 'a user can hold 16 ports, the next request is refused, and closing frees them all' {
    $ctl = Open-Ctl
    $slots = @()
    try {
        for ($n = 0; $n -lt 16; $n++) {
            $slot = [uint32]0
            $e = $ctl.Create(0, [ref]$slot)
            Assert ($e -eq 0) "CREATE_PORT number $n failed with error $e"
            $slots += $slot
        }
        Assert ((@($slots | Sort-Object -Unique)).Count -eq 16) 'two requests shared a slot'
        # The limit is per user, not per handle: another handle of the same user is refused too.
        $other = Open-Ctl
        try {
            $extra = [uint32]0
            $e = $other.Create(0, [ref]$extra)
            Assert ($e -eq $ERROR_NOT_ENOUGH_QUOTA) "the 17th request, on another handle, gave error $e"
        } finally { $other.Dispose() }
        $extra = [uint32]0
        $e = $ctl.Create(0, [ref]$extra)
        Assert ($e -eq $ERROR_NOT_ENOUGH_QUOTA) "the 17th request gave error $e"
    } finally {
        # Closed at once, while Windows is still building the ports.
        $ctl.Dispose()
    }
    foreach ($slot in $slots) {
        Assert (Wait-VssDemandPort $slot 120 -Gone) "the port in slot $slot did not go"
    }
    # Every slot must be free again, however far each port had come.
    $deadline = (Get-Date).AddSeconds(120)
    $again = $null
    do {
        $ctl = Open-Ctl
        $slot = [uint32]0
        $e = $ctl.Create(0, [ref]$slot)
        if ($e -eq 0) { $again = $slot }
        $ctl.Dispose()
        if ($e -ne 0) { Start-Sleep -Milliseconds 500 }
    } while ($e -ne 0 -and (Get-Date) -lt $deadline)
    Assert ($e -eq 0) "slots did not become free again (error $e)"
    Assert ($again -eq 0) "the lowest free slot was $again, not 0"
    Wait-VssDemandPort $again 60 -Gone | Out-Null
    '16 ports per user; all slots free after the handle closed'
}

Add-Test 'D19' 'a port that is in use when its maker closes ends the use and goes away' {
    $ctl = Open-Ctl
    $p = New-DemandPort $ctl
    $hub = Open-Hub $p.Path
    $app = Open-Port $p.Port
    try {
        $task = [HubIo]::WriteAsync($app, (New-Pattern 300000))
        Start-Sleep -Milliseconds 300
        Assert (-not $task.IsCompleted) 'the write did not wait for the hub'
        $ctl.Dispose()
        Assert ($task.Wait(20000)) 'a waiting write hung after the port was removed'
        Assert (-not (Find-VssDemandPort $p.Slot)) 'the port is still listed while the application holds it'
        $detail = "the waiting write ended with $($task.Result) bytes"
    } finally {
        $ctl.Dispose()
        [VssComm]::Close($app)
        $hub.Dispose()
    }
    Assert (Wait-VssDemandPort $p.Slot 60 -Gone) 'the port was still there after every handle closed'
    Assert (Wait-VssSerialCommGone $p.Port) "$($p.Port) is still in SERIALCOMM"
    $detail
}

# Opens each path as the SYSTEM account, through a scheduled task, and returns
# one line each: "open <path>" or "error<n> <path>". SYSTEM is another user than
# the one running this script, and needs no password.
function Invoke-AsSystem([string[]] $Paths) {
    # The helper is compiled here, by the user, because the SYSTEM account
    # cannot compile in a scheduled task; the probe only loads it.
    $dll = 'C:\vss\probe.dll'
    if (-not (Test-Path $dll)) {
        Add-Type -OutputAssembly $dll -OutputType Library -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ProbeK
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);
    public static string Try(string path)
    {
        IntPtr h = CreateFileW(path, 0xC0000000, 0, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        int e = Marshal.GetLastWin32Error();
        if (h == new IntPtr(-1)) return "error" + e + " " + path;
        CloseHandle(h);
        return "open " + path;
    }
}
'@
    }
    $probe = @'
try {
    Add-Type -Path C:\vss\probe.dll
    (whoami) | Set-Content C:\vss\probe-who.txt
    $lines = foreach ($path in (Get-Content C:\vss\probe-in.txt)) { [ProbeK]::Try($path) }
} catch {
    $lines = "probe failed: $($_.Exception.Message)"
}
$lines | Set-Content C:\vss\probe-out.txt
'@
    Set-Content -Path C:\vss\probe.ps1 -Value $probe
    Set-Content -Path C:\vss\probe-in.txt -Value $Paths
    Remove-Item C:\vss\probe-out.txt, C:\vss\probe-who.txt -ErrorAction SilentlyContinue
    & schtasks.exe /create /tn VssProbe /tr 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\vss\probe.ps1' /sc once /st 23:59 /ru SYSTEM /f | Out-Null
    try {
        & schtasks.exe /run /tn VssProbe | Out-Null
        $until = (Get-Date).AddSeconds(90)
        while (-not (Test-Path C:\vss\probe-out.txt) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 500 }
        if (-not (Test-Path C:\vss\probe-out.txt)) { throw 'the probe did not answer within 90 seconds' }
        Start-Sleep -Milliseconds 300
        @(Get-Content C:\vss\probe-out.txt)
    } finally {
        & schtasks.exe /delete /tn VssProbe /f | Out-Null
    }
}

# Opens each path with a restricted token: this user's, with the Administrators
# group and the powerful privileges taken away (runas /trustlevel:0x20000, the
# "basic user" level). That is a user who is not an administrator, without making
# an account. Returns the lines as Invoke-AsSystem does, with the line "admin=..."
# first. The files are in a folder that every user may write.
function Invoke-AsBasicUser([string[]] $Paths) {
    $dir = 'C:\Users\Public\vssprobe'
    New-Item -ItemType Directory -Force $dir | Out-Null
    & icacls.exe $dir /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
    if (-not (Test-Path 'C:\vss\probe.dll')) { Invoke-AsSystem @('\\.\NUL') | Out-Null }
    Copy-Item C:\vss\probe.dll $dir -Force
    $probe = @'
$dir = 'C:\Users\Public\vssprobe'
try {
    Add-Type -Path "$dir\probe.dll"
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $lines = @("admin=$isAdmin")
    $lines += foreach ($path in (Get-Content "$dir\in.txt")) { [ProbeK]::Try($path) }
} catch {
    $lines = "probe failed: $($_.Exception.Message)"
}
$lines | Set-Content "$dir\out.txt"
'@
    Set-Content -Path "$dir\probe.ps1" -Value $probe
    Set-Content -Path "$dir\in.txt" -Value $Paths
    Remove-Item "$dir\out.txt" -ErrorAction SilentlyContinue
    & runas.exe /trustlevel:0x20000 "powershell.exe -NoProfile -ExecutionPolicy Bypass -File $dir\probe.ps1" | Out-Null
    $until = (Get-Date).AddSeconds(90)
    while (-not (Test-Path "$dir\out.txt") -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path "$dir\out.txt")) { throw 'the probe did not answer within 90 seconds' }
    Start-Sleep -Milliseconds 300
    @(Get-Content "$dir\out.txt")
}

Add-Test 'D22' 'the hub end of a port made by hand opens for an administrator and not for a user who is not one' {
    $com = '\\.\' + $port
    # Nobody holds the hub end, so a refusal is the access rule, not a busy port.
    $answer = Invoke-AsBasicUser @($hubPath, $com)
    Assert ($answer.Count -eq 3) "the probe answered: $($answer -join ' | ')"
    Assert ($answer[0] -eq 'admin=False') "the probe was not a plain user: $($answer[0])"
    Assert ($answer[1] -like "error$ERROR_ACCESS_DENIED *") "a user who is not an administrator opening the hub end: $($answer[1])"
    Assert ($answer[2] -like 'open *') "a user who is not an administrator opening the COM port: $($answer[2])"
    $hub = Open-Hub $hubPath
    $hub.Dispose()
    "refused for a plain user: $($answer[1]); the COM port opens; an administrator opens the hub end"
}

Add-Test 'D21' 'the hub end of a port opens for the user who made it, not for another user' {
    $ctl = Open-Ctl
    $p = $null
    try {
        $p = New-DemandPort $ctl
        $com = '\\.\' + $p.Port
        # Nobody holds the hub end, so a refusal is the access rule, not a busy port.
        $answer = Invoke-AsSystem @($p.Path, $com, $hubPath)
        Assert ($answer.Count -eq 3) "the probe answered: $($answer -join ' | ')"
        $who = (Get-Content C:\vss\probe-who.txt) -join ''
        Assert ($who -eq 'nt authority\system') "the probe ran as '$who'"
        Assert ($answer[0] -like "error$ERROR_ACCESS_DENIED *") "another user opening the hub end of a made port: $($answer[0])"
        Assert ($answer[1] -like 'open *') "another user opening the COM port: $($answer[1])"
        Assert ($answer[2] -like 'open *') "an administrator opening the hub end of a port made by hand: $($answer[2])"

        $hub = Open-Hub $p.Path
        $hub.Dispose()
        "refused for another user: $($answer[0]); COM port and the hand-made hub end open"
    } finally {
        $ctl.Dispose()
        if ($p) { Wait-VssDemandPort $p.Slot 60 -Gone | Out-Null }
    }
}

# ---------------------------------------------------------------------------
# D23 to D31: flow control, revoke, the process id and COM numbers (driver version 3).
# ---------------------------------------------------------------------------

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

/// SetCommState with the DTR and RTS control values of the DCB, and what the lines do.
public static class VssFlow
{
    [StructLayout(LayoutKind.Sequential)]
    struct DCB
    {
        public uint DCBlength, BaudRate, Flags;
        public ushort wReserved, XonLim, XoffLim;
        public byte ByteSize, Parity, StopBits;
        public sbyte XonChar, XoffChar, ErrorChar, EofChar, EvtChar;
        public ushort wReserved1;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetCommState(IntPtr h, ref DCB dcb);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetCommState(IntPtr h, ref DCB dcb);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool EscapeCommFunction(IntPtr h, uint function);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint QueryDosDeviceW(string name, System.Text.StringBuilder target, uint max);

    const int DtrShift = 4;
    const int RtsShift = 12;

    /// fDtrControl and fRtsControl: 0 disable, 1 enable, 2 handshake, 3 toggle (RTS only). A baud rate
    /// of 0 leaves the rate as it is. Returns the Win32 error, 0 for success.
    public static int SetFlow(IntPtr h, uint dtr, uint rts, ushort xon, ushort xoff, uint baud)
    {
        var dcb = new DCB { DCBlength = (uint)Marshal.SizeOf(typeof(DCB)) };
        if (!GetCommState(h, ref dcb)) return Marshal.GetLastWin32Error();
        dcb.Flags = (dcb.Flags & ~(3u << DtrShift) & ~(3u << RtsShift)) | (dtr << DtrShift) | (rts << RtsShift);
        dcb.XonLim = xon;
        dcb.XoffLim = xoff;
        if (baud != 0) dcb.BaudRate = baud;
        return SetCommState(h, ref dcb) ? 0 : Marshal.GetLastWin32Error();
    }

    /// DTR control, RTS control, XonLim, XoffLim; null when the call fails.
    public static uint[] GetFlow(IntPtr h)
    {
        var dcb = new DCB { DCBlength = (uint)Marshal.SizeOf(typeof(DCB)) };
        if (!GetCommState(h, ref dcb)) return null;
        return new uint[] { (dcb.Flags >> DtrShift) & 3u, (dcb.Flags >> RtsShift) & 3u, dcb.XonLim, dcb.XoffLim };
    }

    /// EscapeCommFunction; the Win32 error, 0 for success.
    public static int Escape(IntPtr h, uint function)
    {
        return EscapeCommFunction(h, function) ? 0 : Marshal.GetLastWin32Error();
    }

    /// True when the DOS name (COM5) resolves to something.
    public static bool DosNameExists(string name)
    {
        return QueryDosDeviceW(name, new System.Text.StringBuilder(512), 512) != 0;
    }
}
'@

$ERROR_INVALID_PARAMETER = 87; $ERROR_BUSY = 170; $ERROR_ALREADY_ASSIGNED = 85; $ERROR_NOT_FOUND = 1168

# Reads `$Length` bytes from the application handle, however many reads it takes.
function Read-AppAll([IntPtr] $H, [int] $Length) {
    $got = 0
    while ($got -lt $Length) {
        $r = Read-App $H ([Math]::Min(16384, $Length - $got))
        if ($r.Count -eq 0) { throw "the application read stopped after $got of $Length bytes" }
        $got += $r.Count
    }
    $got
}

Add-Test 'D23' 'SetCommState drives DTR and RTS: enable raises, disable drops, and a fresh open starts clear' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        Assert (([VssFlow]::SetFlow($app, 1, 1, 0, 0, 0)) -eq 0) 'SetCommState with DTR and RTS enabled failed'
        $state = $hub.GetState()
        Assert ($state[6] -eq 1 -and $state[7] -eq 1) ("enabling did not raise DTR and RTS: {0}" -f ($state -join ','))
        $flow = [VssFlow]::GetFlow($app)
        Assert ($flow[0] -eq 1 -and $flow[1] -eq 1) "GetCommState reported DTR control $($flow[0]) and RTS control $($flow[1])"

        Assert (([VssFlow]::SetFlow($app, 0, 0, 0, 0, 0)) -eq 0) 'SetCommState with both disabled failed'
        $state = $hub.GetState()
        Assert ($state[6] -eq 0 -and $state[7] -eq 0) ("disabling did not drop DTR and RTS: {0}" -f ($state -join ','))

        # A disabled line is still the application's to set, as on a real port.
        Assert (([VssFlow]::Escape($app, $SETDTR)) -eq 0) 'SETDTR failed with DTR disabled'
        $state = $hub.GetState()
        Assert ($state[6] -eq 1 -and $state[7] -eq 0) ("SETDTR gave {0}" -f ($state -join ','))

        # One line each way.
        Assert (([VssFlow]::SetFlow($app, 0, 1, 0, 0, 0)) -eq 0) 'SetCommState failed'
        $state = $hub.GetState()
        Assert ($state[6] -eq 0 -and $state[7] -eq 1) ("DTR off and RTS on gave {0}" -f ($state -join ','))
        # The loopback lines are the hub's, so the null-modem wiring is not what is checked here.
    } finally { [VssComm]::Close($app) }

    # The port went back to its defaults when the program closed it.
    $app = Open-Port $port
    try {
        $flow = [VssFlow]::GetFlow($app)
        Assert ($flow[0] -eq 0 -and $flow[1] -eq 0) "a new open started with DTR control $($flow[0]) and RTS control $($flow[1])"
        $state = $hub.GetState()
        Assert ($state[6] -eq 0 -and $state[7] -eq 0) ("a new open started with the lines {0}" -f ($state -join ','))
    } finally { [VssComm]::Close($app); $hub.Dispose() }
    'enable and disable move DTR and RTS; SETDTR still works after a disable; every open starts clear'
}

Add-Test 'D24' 'DTR and RTS handshake follow the receive buffer, and the application may not set them' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        # Low at 4096 bytes or less free, high again at 16384 bytes or less waiting, in a 64 KB buffer.
        Assert (([VssFlow]::SetFlow($app, 2, 2, 16384, 4096, 0)) -eq 0) 'SetCommState with handshake failed'
        $flow = [VssFlow]::GetFlow($app)
        Assert ($flow[0] -eq 2 -and $flow[1] -eq 2) "GetCommState reported DTR control $($flow[0]) and RTS control $($flow[1])"
        $state = $hub.GetState()
        Assert ($state[6] -eq 1 -and $state[7] -eq 1) ("an empty buffer should start both lines high: {0}" -f ($state -join ','))

        foreach ($code in $SETDTR, $CLRDTR, $SETRTS, $CLRRTS) {
            $e = [VssFlow]::Escape($app, $code)
            Assert ($e -eq $ERROR_INVALID_PARAMETER) "EscapeCommFunction($code) in handshake mode gave error $e"
        }

        # Fill the buffer to within 3536 bytes of full.
        $e = 0
        Assert ($hub.Write((New-Pattern 62000), 5000, [ref]$e) -eq 62000) "the hub write failed ($e)"
        $state = $hub.GetState()
        $until = (Get-Date).AddSeconds(3)
        while (($state[6] -ne 0 -or $state[7] -ne 0) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 50; $state = $hub.GetState() }
        Assert ($state[6] -eq 0 -and $state[7] -eq 0) ("a nearly full buffer should drop both lines: {0}" -f ($state -join ','))

        # Between the limits they stay low.
        Assert ((Read-AppAll $app 40000) -ge 40000) 'could not read 40000 bytes'
        $state = $hub.GetState()
        Assert ($state[6] -eq 0 -and $state[7] -eq 0) ("22000 bytes waiting should keep the lines low: {0}" -f ($state -join ','))

        # At 16384 bytes or fewer waiting they come back.
        Assert ((Read-AppAll $app 6000) -ge 6000) 'could not read 6000 bytes'
        $state = $hub.GetState()
        $until = (Get-Date).AddSeconds(3)
        while (($state[6] -ne 1 -or $state[7] -ne 1) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 50; $state = $hub.GetState() }
        Assert ($state[6] -eq 1 -and $state[7] -eq 1) ("16000 bytes waiting should raise the lines: {0}" -f ($state -join ','))

        # Leaving handshake gives the lines back to the application.
        Assert (([VssFlow]::SetFlow($app, 0, 0, 0, 0, 0)) -eq 0) 'SetCommState failed'
        Assert (([VssFlow]::Escape($app, $SETDTR)) -eq 0) 'SETDTR is still refused after leaving handshake'
        'both lines drop at 4096 free and rise at 16384 waiting; SETDTR and SETRTS are refused meanwhile'
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D25' 'RTS toggle is high from the first queued byte until the last has been sent' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        # 1200 baud, 8N1: ten bits a byte, so 60 bytes leave in half a second.
        Assert ([VssComm]::SetState($app, 1200, 8, 0, 0)) 'SetCommState failed'
        Assert (([VssFlow]::SetFlow($app, 0, 3, 0, 0, 0)) -eq 0) 'SetCommState with RTS toggle failed'
        $flow = [VssFlow]::GetFlow($app)
        Assert ($flow[1] -eq 3) "GetCommState reported RTS control $($flow[1])"
        $state = $hub.GetState()
        Assert ($state[7] -eq 0) 'RTS was high with nothing to send'

        # RTS may still be set by hand in toggle mode, as on a real port.
        Assert (([VssFlow]::Escape($app, $SETRTS)) -eq 0) 'SETRTS was refused in toggle mode'
        Assert (([VssFlow]::Escape($app, $CLRRTS)) -eq 0) 'CLRRTS was refused in toggle mode'

        $watch = [Diagnostics.Stopwatch]::StartNew()
        Assert ([VssComm]::Write($app, (New-Pattern 60)) -eq 60) 'the application write was short'
        $state = $hub.GetState()
        Assert ($state[7] -eq 1) ("RTS was not high just after the write: {0}" -f ($state -join ','))
        $got = Read-Hub $hub 60
        Assert ($got.Length -eq 60) 'the hub did not read the 60 bytes'

        # The hub has them all, but they have not finished leaving: RTS stays up for the sending time.
        $state = $hub.GetState()
        Assert ($state[7] -eq 1 -and $watch.ElapsedMilliseconds -lt 300) ("RTS fell early, after $($watch.ElapsedMilliseconds) ms")
        $until = (Get-Date).AddSeconds(3)
        while ($state[7] -ne 0 -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 20; $state = $hub.GetState() }
        $held = $watch.ElapsedMilliseconds
        Assert ($state[7] -eq 0) 'RTS never fell'
        Assert ($held -ge 400 -and $held -le 1500) "RTS was held for $held ms; 60 bytes at 1200 baud take 500"
        "RTS rose with the first byte and fell after $held ms (500 expected)"
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D26' 'revoke takes the port from a program that holds it, and a new program can open it' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    $new = [IntPtr](-1)
    try {
        # Three things wait in the driver: a read, a WaitCommEvent, and a write that does not fit.
        [VssComm]::SetTimeouts($app, 0, 0, 0, 0, 0) | Out-Null
        [VssComm]::SetMask($app, $EV_CTS) | Out-Null
        $read = [PendingIo]::BeginRead($app, 16)
        $wait = [PendingIo]::BeginWait($app)
        $task = [HubIo]::WriteAsync($app, (New-Pattern 300000))
        Start-Sleep -Milliseconds 300
        Assert (-not $read.Completed(0) -and -not $wait.Completed(0) -and -not $task.IsCompleted) 'the requests did not wait'

        $state = $hub.GetState()
        Assert ($state[1] -eq 1 -and $state[11] -eq $PID) ("the hub saw AppOpen $($state[1]) and process $($state[11]), not 1 and $PID")

        $process = [uint32]0
        $e = $hub.Revoke([ref]$process)
        Assert ($e -eq 0) "REVOKE failed with error $e"
        Assert ($process -eq $PID) "REVOKE named process $process, not $PID"

        Assert ($read.Completed(3000)) 'the waiting read did not end'
        $readError = 0; $read.Finish($false, [ref]$readError) | Out-Null
        Assert ($readError -ne 0) 'the waiting read ended as a success'
        Assert ($wait.Completed(3000)) 'the waiting WaitCommEvent did not end'
        $waitError = 0; $wait.Finish($false, [ref]$waitError) | Out-Null
        Assert ($waitError -ne 0) 'the waiting WaitCommEvent ended as a success'
        Assert ($task.Wait(5000)) 'the blocked write did not end'
        Assert ($task.Result -lt 300000) 'the blocked write went through'
        $read.Dispose(); $wait.Dispose()

        $state = $hub.GetState()
        Assert ($state[1] -eq 0 -and $state[11] -eq 0) ("after the revoke the hub saw AppOpen $($state[1]) and process $($state[11])")

        # Everything on the old handle fails from now on.
        Assert ([VssComm]::Write($app, [byte[]](1, 2, 3)) -le 0) 'a write on the revoked handle worked'
        Assert (([VssFlow]::Escape($app, $SETDTR)) -ne 0) 'EscapeCommFunction on the revoked handle worked'
        Assert ($null -eq [VssComm]::GetState($app)) 'GetCommState on the revoked handle worked'
        $again = $null
        try { $again = [PendingIo]::BeginRead($app, 4) } catch { $again = $null }
        if ($again) {
            Assert ($again.Completed(3000)) 'a read on the revoked handle waited'
            $againError = 0; $again.Finish($false, [ref]$againError) | Out-Null
            $again.Dispose()
            Assert ($againError -ne 0) 'a read on the revoked handle was accepted'
        }

        # A new program opens the port at once and it carries bytes.
        $new = Open-Port $port
        $state = $hub.GetState()
        Assert ($state[1] -eq 1) 'the hub was not told the new program opened'
        $e = 0
        Assert ($hub.Write([Text.Encoding]::ASCII.GetBytes('fresh'), 5000, [ref]$e) -eq 5) "the hub write failed ($e)"
        $r = Read-App $new 5
        Assert ([Text.Encoding]::ASCII.GetString($r.Bytes) -eq 'fresh') 'the new program read other bytes'

        # The old handle closing later changes nothing for the new program.
        [VssComm]::Close($app); $app = [IntPtr](-1)
        Start-Sleep -Milliseconds 200
        $state = $hub.GetState()
        Assert ($state[1] -eq 1 -and $state[11] -eq $PID) 'closing the revoked handle took the port from the new program'
        Assert ($hub.Write([Text.Encoding]::ASCII.GetBytes('still'), 5000, [ref]$e) -eq 5) "the hub write failed ($e)"
        $r = Read-App $new 5
        Assert ([Text.Encoding]::ASCII.GetString($r.Bytes) -eq 'still') 'the new program lost its bytes'

        # A revoke with no program on the port is not an error.
        [VssComm]::Close($new); $new = [IntPtr](-1)
        Start-Sleep -Milliseconds 200
        $process = [uint32]99
        $e = $hub.Revoke([ref]$process)
        Assert ($e -eq 0 -and $process -eq 0) "REVOKE with no program gave error $e and process $process"
        "ended a read, a wait and a write (errors $readError, $waitError); a new program opened; the old handle failed"
    } finally {
        if ($app -ne [IntPtr](-1)) { [VssComm]::Close($app) }
        if ($new -ne [IntPtr](-1)) { [VssComm]::Close($new) }
        $hub.Dispose()
    }
}

Add-Test 'D27' 'the hub is told which process has the port, and the settings it chose' {
    $hub = Open-Hub $hubPath
    $child = $null
    try {
        $state = $hub.GetState()
        Assert ($state[1] -eq 0 -and $state[11] -eq 0) 'a process is named with nothing open'

        # A program of its own, so that the process id is not this one's.
        $script = "`$p = New-Object System.IO.Ports.SerialPort '$port', 38400, 'Even', 7, 'Two'; `$p.Open(); Start-Sleep 12; `$p.Close()"
        $child = Start-Process powershell.exe -ArgumentList '-NoProfile', '-Command', $script -PassThru -WindowStyle Hidden
        $until = (Get-Date).AddSeconds(30)
        while ($state[1] -ne 1 -and (Get-Date) -lt $until) {
            $next = $hub.WaitState($state[0], 1000)
            if ($next) { $state = $next }
        }
        Assert ($state[1] -eq 1) 'the hub was not told a program opened the port'
        Assert ($state[11] -eq $child.Id) "the hub saw process $($state[11]), not $($child.Id)"
        Assert ($state[11] -ne $PID) 'the process is this one'
        $detail = "process $($state[11]) at $($state[2]) baud, $($state[3]) bits, parity $($state[4]), stop $($state[5])"

        $until = (Get-Date).AddSeconds(30)
        while ($state[1] -ne 0 -and (Get-Date) -lt $until) {
            $next = $hub.WaitState($state[0], 1000)
            if ($next) { $state = $next }
        }
        Assert ($state[1] -eq 0 -and $state[11] -eq 0) 'the process was still named after the program closed the port'
        $detail
    } finally {
        if ($child -and -not $child.HasExited) { $child.Kill() }
        $hub.Dispose()
    }
}

# What asks the helper to change a number: the helper's own pipe for the user-mode driver (which
# made the port), and a pipe of its own for the kernel-mode driver (whose control device did).
function Set-VssComNumber($Ctl, [uint32] $Slot, [uint32] $Number) {
    $reply = [uint32]0
    if ($Flavour -eq 'umdf') { return $Ctl.Raw(1, 3, $Slot, $Number, [ref]$reply) }
    $e = 0
    $helper = [HelperIo]::Open($HelperPipeName, [ref]$e)
    if (-not $helper) { throw "opening the helper's pipe failed with error $e" }
    try { return $helper.Raw(1, 4, $Slot, $Number, [ref]$reply) } finally { $helper.Dispose() }
}

# A COM number from `$From` up that nothing has: not listed, not resolving.
function Get-FreeComNumber([int] $From) {
    for ($n = $From; $n -le 255; $n++) {
        if (-not [VssFlow]::DosNameExists("COM$n") -and -not (Test-VssSerialComm "COM$n")) { return $n }
    }
    throw 'no free COM number'
}

# Waits until the port in the slot has that COM name.
function Wait-VssPortNamed([uint32] $Slot, [string] $Name, [int] $Seconds = 40) {
    $until = (Get-Date).AddSeconds($Seconds)
    do {
        $found = Find-VssDemandPort $Slot
        if ($found -and $found.Port -eq $Name) { return $found }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $until)
    $null
}

Add-Test 'D28' 'a port is given the COM number asked for, keeps it when made again, and frees the one it left' {
    $first = Get-FreeComNumber 150
    $second = Get-FreeComNumber ($first + 1)
    $ctl = Open-Ctl
    $p = $null
    try {
        $p = New-DemandPort $ctl
        $before = $p.Port
        $e = Set-VssComNumber $ctl $p.Slot $first
        Assert ($e -eq 0) "asking for COM$first gave error $e"

        $found = Wait-VssPortNamed $p.Slot "COM$first"
        Assert ($null -ne $found) "the port in slot $($p.Slot) never became COM$first (it is $((Find-VssDemandPort $p.Slot).Port))"
        Assert (Test-VssSerialComm "COM$first") "COM$first is not in SERIALCOMM"
        Assert (Wait-VssSerialCommGone $before) "$before is still in SERIALCOMM"
        $device = Get-PnpDevice -Class Ports -PresentOnly | Where-Object { $_.FriendlyName -like "*(COM$first)" }
        Assert ($null -ne $device) "no Ports device is called (COM$first)"

        # The new name works as a port: bytes cross.
        $hub = Open-Hub $found.Path
        $app = Open-Port "COM$first"
        try {
            $e = 0
            Assert ([VssComm]::Write($app, [Text.Encoding]::ASCII.GetBytes('numbered')) -eq 8) 'the application write was short'
            $got = [Text.Encoding]::ASCII.GetString((Read-Hub $hub 8))
            Assert ($got -eq 'numbered') "the hub read '$got'"
        } finally { [VssComm]::Close($app); $hub.Dispose() }

        # Again, to another number, with the first one freed.
        $e = Set-VssComNumber $ctl $p.Slot $second
        Assert ($e -eq 0) "asking for COM$second gave error $e"
        Assert ($null -ne (Wait-VssPortNamed $p.Slot "COM$second")) "the port never became COM$second"
        Assert (Wait-VssSerialCommGone "COM$first") "COM$first is still in SERIALCOMM"
        $opens = 0
        $handle = [VssComm]::Open("COM$first", [ref]$opens)
        if ($handle -ne [IntPtr](-1)) { [VssComm]::Close($handle); throw "COM$first still opens" }

        # Asking for the number it has is not an error.
        $e = Set-VssComNumber $ctl $p.Slot $second
        Assert ($e -eq 0) "asking again for COM$second gave error $e"

        # The port goes, and when it is made again in the same slot it has the number.
        $ctl.Dispose()
        Assert (Wait-VssDemandPort $p.Slot 60 -Gone) 'the port was still there after its handle closed'
        $ctl = Open-Ctl
        $q = New-DemandPort $ctl
        Assert ($q.Slot -eq $p.Slot -and $q.Port -eq "COM$second") "the port came back as slot $($q.Slot), $($q.Port), not slot $($p.Slot), COM$second"
        "$before -> COM$first -> COM$second; carried bytes; COM$first freed; kept after being made again"
    } finally {
        $ctl.Dispose()
        if ($p) { Wait-VssDemandPort $p.Slot 60 -Gone | Out-Null }
    }
}

Add-Test 'D29' 'a number another device has, or no number at all, is refused and nothing changes' {
    $ctl = Open-Ctl
    $p = $null
    try {
        $p = New-DemandPort $ctl
        $name = $p.Port
        $taken = [int]($port -replace '\D', '')
        $e = Set-VssComNumber $ctl $p.Slot $taken
        Assert ($e -eq $ERROR_ALREADY_ASSIGNED) "asking for COM$taken, which $port has, gave error $e"
        foreach ($bad in 0, 256, 4096) {
            $e = Set-VssComNumber $ctl $p.Slot $bad
            Assert ($e -eq $ERROR_INVALID_PARAMETER) "asking for COM$bad gave error $e"
        }
        $now = Find-VssDemandPort $p.Slot
        Assert ($now.Port -eq $name) "the port is $($now.Port) after the refusals, not $name"
        Assert (Test-VssSerialComm $name) "$name left SERIALCOMM"
        "COM$taken refused ($ERROR_ALREADY_ASSIGNED), 0, 256 and 4096 refused ($ERROR_INVALID_PARAMETER); still $name"
    } finally {
        $ctl.Dispose()
        if ($p) { Wait-VssDemandPort $p.Slot 60 -Gone | Out-Null }
    }
}

Add-Test 'D30' 'the number of a port a program has open is not changed, and can be once it is closed' {
    $number = Get-FreeComNumber 150
    $ctl = Open-Ctl
    $p = $null
    try {
        $p = New-DemandPort $ctl
        $name = $p.Port
        $app = Open-Port $name
        try {
            $e = Set-VssComNumber $ctl $p.Slot $number
            Assert ($e -eq $ERROR_BUSY) "asking with a program on $name gave error $e, not busy ($ERROR_BUSY)"
            Assert ((Find-VssDemandPort $p.Slot).Port -eq $name) "the port is not $name after the refusal"
        } finally { [VssComm]::Close($app) }

        # The refusal left the port working: the same request goes through once the program is gone.
        $e = Set-VssComNumber $ctl $p.Slot $number
        Assert ($e -eq 0) "asking after the program closed gave error $e"
        Assert ($null -ne (Wait-VssPortNamed $p.Slot "COM$number")) "the port never became COM$number"
        "busy while $name was open ($ERROR_BUSY); COM$number once it was closed"
    } finally {
        $ctl.Dispose()
        if ($p) { Wait-VssDemandPort $p.Slot 60 -Gone | Out-Null }
    }
}

Add-Test 'D31' 'the number of a port that is not the asker''s, or is not there, is not changed' {
    $number = Get-FreeComNumber 150
    $ctl = Open-Ctl
    $p = $null
    try {
        $p = New-DemandPort $ctl
        $name = $p.Port
        # A slot with no port.
        $empty = [uint32]31
        $e = Set-VssComNumber $ctl $empty $number
        Assert ($e -eq $ERROR_NOT_FOUND) "asking for a slot with no port gave error $e, not not-found ($ERROR_NOT_FOUND)"
        # The user-mode driver's ports belong to the pipe that made them: another pipe is refused.
        if ($Flavour -eq 'umdf') {
            $other = Open-Ctl
            try {
                $e = Set-VssComNumber $other $p.Slot $number
                Assert ($e -eq $ERROR_NOT_FOUND) "another pipe changing the port gave error $e, not not-found"
            } finally { $other.Dispose() }
        }
        Assert ((Find-VssDemandPort $p.Slot).Port -eq $name) "the port is not $name after the refusals"
        'a slot with no port, and (user-mode driver) a port of another pipe, are refused'
    } finally {
        $ctl.Dispose()
        if ($p) { Wait-VssDemandPort $p.Slot 60 -Gone | Out-Null }
    }
}

# ---------------------------------------------------------------------------
# D32 to D41: line errors (driver version 4). The hub raises a break or a framing, parity or
# overrun error with IOCTL_VSSCOM_HUB_LINE_ERROR; the application sees it through ClearCommError,
# WaitCommEvent, the error and break characters and fAbortOnError.
# ---------------------------------------------------------------------------

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

/// What a program does about line errors: ClearCommError, the DCB's error character, event
/// character and abort on error, and the raw requests that set the break character (the DCB has
/// no field for it). Every call returns the Win32 error, 0 for success.
public static class VssErr
{
    [StructLayout(LayoutKind.Sequential)]
    struct DCB
    {
        public uint DCBlength, BaudRate, Flags;
        public ushort wReserved, XonLim, XoffLim;
        public byte ByteSize, Parity, StopBits;
        public byte XonChar, XoffChar, ErrorChar, EofChar, EvtChar;
        public ushort wReserved1;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct COMSTAT { public uint Flags, cbInQue, cbOutQue; }

    [StructLayout(LayoutKind.Sequential)]
    struct OVERLAPPED { public IntPtr Internal, InternalHigh; public uint Offset, OffsetHigh; public IntPtr Event; }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetCommState(IntPtr h, ref DCB dcb);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetCommState(IntPtr h, ref DCB dcb);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ClearCommError(IntPtr h, out uint errors, out COMSTAT stat);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetCommMask(IntPtr h, uint mask);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(IntPtr h, IntPtr buffer, uint length, IntPtr read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr input, uint inSize, IntPtr output, uint outSize, IntPtr returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetOverlappedResult(IntPtr h, IntPtr overlapped, out uint transferred, bool wait);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CancelIoEx(IntPtr h, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "CreateEventW")]
    static extern IntPtr CreateEventW(IntPtr attributes, bool manual, bool initial, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr h, uint ms);

    // Bits of DCB.Flags: fParity, fErrorChar, fAbortOnError.
    const uint FlagParity = 0x2;
    const uint FlagErrorChar = 0x400;
    const uint FlagAbortOnError = 0x4000;

    // IOCTL_SERIAL_GET_CHARS, SET_CHARS, GET_HANDFLOW and SET_HANDFLOW from ntddser.h.
    const uint IoctlGetChars = 0x1B0058;
    const uint IoctlSetChars = 0x1B005C;
    const uint IoctlGetHandflow = 0x1B0060;
    const uint IoctlSetHandflow = 0x1B0064;
    // SERIAL_BREAK_CHAR in SERIAL_HANDFLOW.FlowReplace.
    const int FlowBreakChar = 0x10;

    public const int TimedOut = 258;

    /// ClearCommError: the CE_* bits it reported (they are cleared by the call) and the bytes in
    /// the receive queue.
    public static int ClearError(IntPtr h, out uint errors, out uint inQueue)
    {
        COMSTAT stat;
        uint bits;
        errors = 0;
        inQueue = 0;
        if (!ClearCommError(h, out bits, out stat)) return Marshal.GetLastWin32Error();
        errors = bits;
        inQueue = stat.cbInQue;
        return 0;
    }

    /// SetCommState with fAbortOnError, and with fErrorChar (and fParity, which goes with it in
    /// programs that ask for the error character) and ErrorChar, and with EvtChar. Everything else
    /// in the DCB stays as it is. SetCommState rebuilds the handshake and replace flags and the
    /// characters, so it also clears a break character set by SetBreakChar: call this first.
    public static int SetErrorMode(IntPtr h, bool abortOnError, bool errorChar, byte errorCharValue, byte evtChar)
    {
        var dcb = new DCB { DCBlength = (uint)Marshal.SizeOf(typeof(DCB)) };
        if (!GetCommState(h, ref dcb)) return Marshal.GetLastWin32Error();
        uint flags = dcb.Flags & ~(FlagParity | FlagErrorChar | FlagAbortOnError);
        if (abortOnError) flags |= FlagAbortOnError;
        if (errorChar) flags |= FlagErrorChar | FlagParity;
        dcb.Flags = flags;
        dcb.ErrorChar = errorCharValue;
        dcb.EvtChar = evtChar;
        return SetCommState(h, ref dcb) ? 0 : Marshal.GetLastWin32Error();
    }

    /// One overlapped ReadFile of one byte with an event of its own. 0 when it read the byte, the
    /// Win32 error it ended with otherwise (995 after an error with abort on error), 258 when it
    /// did not end in `ms`: it is cancelled then.
    public static int ReadError(IntPtr h, int ms)
    {
        IntPtr signal = CreateEventW(IntPtr.Zero, true, false, null);
        IntPtr ov = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));
        IntPtr buffer = Marshal.AllocHGlobal(4);
        try
        {
            Marshal.StructureToPtr(new OVERLAPPED { Event = signal }, ov, false);
            uint n;
            if (!ReadFile(h, buffer, 1, IntPtr.Zero, ov))
            {
                int e = Marshal.GetLastWin32Error();
                if (e != 997) return e;
                if (WaitForSingleObject(signal, (uint)ms) != 0)
                {
                    CancelIoEx(h, ov);
                    // The buffer must outlive the cancel, so wait for it to land.
                    GetOverlappedResult(h, ov, out n, true);
                    return TimedOut;
                }
            }
            return GetOverlappedResult(h, ov, out n, true) ? 0 : Marshal.GetLastWin32Error();
        }
        finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); Marshal.FreeHGlobal(buffer); }
    }

    /// SetCommMask; the Win32 error, 0 for success.
    public static int MaskError(IntPtr h, uint mask)
    {
        return SetCommMask(h, mask) ? 0 : Marshal.GetLastWin32Error();
    }

    // One overlapped DeviceIoControl, finished before it returns. The handle is overlapped, so
    // every call needs an OVERLAPPED with an event, even one that completes at once.
    static int Control(IntPtr h, uint code, IntPtr input, uint inSize, IntPtr output, uint outSize)
    {
        IntPtr signal = CreateEventW(IntPtr.Zero, true, false, null);
        IntPtr ov = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));
        try
        {
            Marshal.StructureToPtr(new OVERLAPPED { Event = signal }, ov, false);
            if (!DeviceIoControl(h, code, input, inSize, output, outSize, IntPtr.Zero, ov))
            {
                int e = Marshal.GetLastWin32Error();
                if (e != 997) return e;
            }
            uint n;
            return GetOverlappedResult(h, ov, out n, true) ? 0 : Marshal.GetLastWin32Error();
        }
        finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); }
    }

    /// The special characters and flags the port holds now: ErrorChar, BreakChar and EventChar of
    /// GET_CHARS, and the FlowReplace of GET_HANDFLOW (it carries the error and break character
    /// switches). Returns the Win32 error, 0 for success.
    public static int GetSpecial(IntPtr h, out byte errorChar, out byte breakChar, out byte eventChar, out int flowReplace)
    {
        IntPtr flow = Marshal.AllocHGlobal(16);
        IntPtr chars = Marshal.AllocHGlobal(8);
        errorChar = breakChar = eventChar = 0;
        flowReplace = 0;
        try
        {
            int e = Control(h, IoctlGetHandflow, IntPtr.Zero, 0, flow, 16);
            if (e != 0) return e;
            e = Control(h, IoctlGetChars, IntPtr.Zero, 0, chars, 6);
            if (e != 0) return e;
            flowReplace = Marshal.ReadInt32(flow, 4);
            errorChar = Marshal.ReadByte(chars, 1);
            breakChar = Marshal.ReadByte(chars, 2);
            eventChar = Marshal.ReadByte(chars, 3);
            return 0;
        }
        finally { Marshal.FreeHGlobal(flow); Marshal.FreeHGlobal(chars); }
    }

    /// Turns SERIAL_BREAK_CHAR on or off in the flow settings and sets BreakChar, with the raw
    /// requests: GET_HANDFLOW and SET_HANDFLOW (four 32-bit values, ControlHandShake, FlowReplace,
    /// XonLimit, XoffLimit) and GET_CHARS and SET_CHARS (six bytes, EofChar, ErrorChar, BreakChar,
    /// EventChar, XonChar, XoffChar). Everything else stays as it is.
    public static int SetBreakChar(IntPtr h, bool enable, byte value)
    {
        IntPtr flow = Marshal.AllocHGlobal(16);
        IntPtr chars = Marshal.AllocHGlobal(8);
        try
        {
            int e = Control(h, IoctlGetHandflow, IntPtr.Zero, 0, flow, 16);
            if (e != 0) return e;
            int replace = Marshal.ReadInt32(flow, 4);
            replace = enable ? (replace | FlowBreakChar) : (replace & ~FlowBreakChar);
            Marshal.WriteInt32(flow, 4, replace);

            e = Control(h, IoctlGetChars, IntPtr.Zero, 0, chars, 6);
            if (e != 0) return e;
            Marshal.WriteByte(chars, 2, value);
            e = Control(h, IoctlSetChars, chars, 6, IntPtr.Zero, 0);
            if (e != 0) return e;

            return Control(h, IoctlSetHandflow, flow, 16, IntPtr.Zero, 0);
        }
        finally { Marshal.FreeHGlobal(flow); Marshal.FreeHGlobal(chars); }
    }
}
'@

# SERIAL_ERROR_* bits, which the hub sends, and the CE_* bits that ClearCommError turns them into.
$LE_BREAK = 0x01; $LE_FRAMING = 0x02; $LE_OVERRUN = 0x04; $LE_QUEUEOVERRUN = 0x08; $LE_PARITY = 0x10
$CE_RXOVER = 0x1; $CE_OVERRUN = 0x2; $CE_RXPARITY = 0x4; $CE_FRAME = 0x8; $CE_BREAK = 0x10
$EV_RXFLAG = 0x2; $EV_BREAK = 0x40; $EV_ERR = 0x80
$PURGE_ALL = 0xF
$ERROR_INVALID_FUNCTION = 1

# Does the installed driver have the request? An empty error is refused with 87 by a driver that
# has it and with 1 by one that has not, and changes nothing either way.
if ($hubPath) {
    $probe = $null
    try {
        $probe = Open-Hub $hubPath
        $LineErrorsSupported = $probe.LineError(0, 0, 0, 5000) -ne $ERROR_INVALID_FUNCTION
    } catch {
        $LineErrorsSupported = $true
    } finally {
        if ($probe) { $probe.Dispose() }
    }
}

# Raises one line error from the hub. The request is done, and the error latched, when this returns.
# The events it raises are not necessarily raised yet (the driver completes the request first); see
# Sync-Events.
function Send-LineError($Hub, [uint32] $Errors, [uint32] $HasByte = 0, [uint32] $Value = 0) {
    $e = $Hub.LineError($Errors, $HasByte, $Value, 5000)
    if ($e -ne 0) { throw ("the line error request (errors 0x{0:X}, byte {1}:{2}) failed with error {3}" -f $Errors, $HasByte, $Value, $e) }
}

# ClearCommError: the CE_* bits and the bytes waiting in the receive queue.
function Clear-Errors([IntPtr] $H) {
    $errors = [uint32]0
    $queue = [uint32]0
    $e = [VssErr]::ClearError($H, [ref]$errors, [ref]$queue)
    if ($e -ne 0) { throw "ClearCommError failed with error $e" }
    [pscustomobject]@{ Errors = [int]$errors; Queue = [int]$queue }
}

# Returns once the driver has raised the events of the line errors sent so far. A request to the
# hub completes before VssComServiceHubWrites raises its events, and a later hub write completes
# after them, because one thread moves the bytes and takes the hub's requests in order. So a byte
# is written, and taken out again, which leaves the receive queue as it was.
function Sync-Events($Hub, [IntPtr] $App) {
    $e = 0
    Assert ($Hub.Write([byte[]]@(66), 5000, [ref]$e) -eq 1) "the hub write that waits for the events failed ($e)"
    Assert ([VssComm]::Purge($App, $PURGE_RXCLEAR)) 'PurgeComm(RXCLEAR) failed'
}

Add-Test 'D32' 'a line error is latched for ClearCommError and read once' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        # Each kind alone: the SERIAL_ERROR_* bit the hub sends, the CE_* bit that comes back, once.
        $kinds = @(
            @('break', $LE_BREAK, $CE_BREAK),
            @('framing', $LE_FRAMING, $CE_FRAME),
            @('overrun', $LE_OVERRUN, $CE_OVERRUN),
            @('queue overrun', $LE_QUEUEOVERRUN, $CE_RXOVER),
            @('parity', $LE_PARITY, $CE_RXPARITY)
        )
        foreach ($kind in $kinds) {
            Send-LineError $hub $kind[1]
            $got = Clear-Errors $app
            Assert ($got.Errors -eq $kind[2]) ("{0}: ClearCommError reported 0x{1:X}, not 0x{2:X}" -f $kind[0], $got.Errors, $kind[2])
            Assert ($got.Queue -eq 0) ("{0}: the error put {1} bytes in the queue" -f $kind[0], $got.Queue)
            $got = Clear-Errors $app
            Assert ($got.Errors -eq 0) ("{0}: a second ClearCommError reported 0x{1:X}" -f $kind[0], $got.Errors)
        }

        # Errors that arrive one after the other, or in one request, are one report. The same error
        # twice is one bit.
        Send-LineError $hub $LE_BREAK
        Send-LineError $hub $LE_PARITY
        Send-LineError $hub $LE_PARITY
        $got = Clear-Errors $app
        $want = $CE_BREAK -bor $CE_RXPARITY
        Assert ($got.Errors -eq $want) ("two errors were reported as 0x{0:X}, not 0x{1:X}" -f $got.Errors, $want)
        Send-LineError $hub ($LE_FRAMING -bor $LE_OVERRUN)
        $got = Clear-Errors $app
        $want = $CE_FRAME -bor $CE_OVERRUN
        Assert ($got.Errors -eq $want) ("one request with two bits was reported as 0x{0:X}, not 0x{1:X}" -f $got.Errors, $want)

        # What the driver refuses: no bit, a bit that is no error, a HasByte that is not 0 or 1, a byte above 255.
        $bad = @(
            @(0, 0, 0, 'no error bit'),
            @(0x20, 0, 0, 'a bit that is no error'),
            @(0x111, 0, 0, 'a bit above the five errors'),
            @($LE_PARITY, 2, 0, 'a HasByte of 2'),
            @($LE_PARITY, 1, 256, 'a Byte of 256')
        )
        foreach ($b in $bad) {
            $e = $hub.LineError($b[0], $b[1], $b[2], 5000)
            Assert ($e -eq $ERROR_INVALID_PARAMETER) ("{0} gave error {1}, not {2}" -f $b[3], $e, $ERROR_INVALID_PARAMETER)
        }
        $got = Clear-Errors $app
        Assert ($got.Errors -eq 0) ("a refused request latched 0x{0:X}" -f $got.Errors)

        # An error raised while no program has the port open is dropped, and so is its byte.
        [VssComm]::Close($app); $app = [IntPtr](-1)
        Send-LineError $hub $LE_PARITY 1 0x78
        Send-LineError $hub $LE_BREAK
        $app = Open-Port $port
        $got = Clear-Errors $app
        Assert ($got.Errors -eq 0 -and $got.Queue -eq 0) ("errors raised with no program open arrived: 0x{0:X}, {1} bytes" -f $got.Errors, $got.Queue)

        # Closing the port clears an error that nobody read.
        Send-LineError $hub $LE_FRAMING 1 0x79
        [VssComm]::Close($app); $app = [IntPtr](-1)
        $app = Open-Port $port
        $got = Clear-Errors $app
        Assert ($got.Errors -eq 0 -and $got.Queue -eq 0) ("a new open found 0x{0:X} and {1} bytes left by the last one" -f $got.Errors, $got.Queue)
        'five kinds read once; accumulate; five refused with 87; dropped with no program open; cleared when the port closes'
    } finally {
        if ($app -ne [IntPtr](-1)) { [VssComm]::Close($app) }
        $hub.Dispose()
    }
}

Add-Test 'D33' 'EV_ERR and EV_BREAK complete a pending WaitCommEvent and are kept when none is pending' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    $notes = @()
    try {
        Assert ([VssComm]::SetMask($app, $EV_ERR -bor $EV_BREAK)) 'SetCommMask failed'

        # A parity error completes a pending wait with EV_ERR.
        $io = [PendingIo]::BeginWait($app)
        try {
            Assert (-not $io.Completed(200)) 'WaitCommEvent finished with no event'
            Send-LineError $hub $LE_PARITY
            Assert ($io.Completed(5000)) 'a parity error did not complete the wait'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq 0 -and $io.Mask() -eq $EV_ERR) ("the wait ended with mask 0x{0:X}, error {1}" -f $io.Mask(), $err)
        } finally { $io.Dispose() }
        Assert ((Clear-Errors $app).Errors -eq $CE_RXPARITY) 'ClearCommError did not report the parity error'
        $notes += 'parity completes a wait'

        # A break with no wait pending is kept, and the next wait returns at once.
        Send-LineError $hub $LE_BREAK
        Sync-Events $hub $app
        $io = [PendingIo]::BeginWait($app)
        try {
            Assert ($io.Completed(2000)) 'the kept EV_BREAK did not complete the next wait'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq 0 -and $io.Mask() -eq $EV_BREAK) ("the kept event came back as mask 0x{0:X}, error {1}" -f $io.Mask(), $err)
        } finally { $io.Dispose() }
        Assert ((Clear-Errors $app).Errors -eq $CE_BREAK) 'ClearCommError did not report the break'
        $notes += 'a break is kept for the next wait'

        # With EV_BREAK alone in the mask a framing error is no event, and the break is, by itself.
        Assert ([VssComm]::SetMask($app, $EV_BREAK)) 'SetCommMask failed'
        $io = [PendingIo]::BeginWait($app)
        try {
            Send-LineError $hub $LE_FRAMING
            Sync-Events $hub $app
            Assert (-not $io.Completed(200)) 'a framing error completed a wait for EV_BREAK'
            Send-LineError $hub $LE_BREAK
            Assert ($io.Completed(5000)) 'the break did not complete the wait'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq 0 -and $io.Mask() -eq $EV_BREAK) ("the wait ended with mask 0x{0:X}, error {1}" -f $io.Mask(), $err)
        } finally { $io.Dispose() }
        $want = $CE_FRAME -bor $CE_BREAK
        Assert ((Clear-Errors $app).Errors -eq $want) 'ClearCommError did not report the framing error and the break'
        $notes += 'the mask picks the events'

        # A queue overrun is only latched: no event, so the wait stays, and it can still be cancelled.
        Assert ([VssComm]::SetMask($app, $EV_ERR)) 'SetCommMask failed'
        $io = [PendingIo]::BeginWait($app)
        try {
            Send-LineError $hub $LE_QUEUEOVERRUN
            Sync-Events $hub $app
            Assert (-not $io.Completed(200)) 'a queue overrun completed a wait for EV_ERR'
            Assert ($io.Cancel()) 'CancelIoEx failed'
            Assert ($io.Completed(2000)) 'the cancelled wait did not end'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq $ERROR_OPERATION_ABORTED) "the cancelled wait ended with error $err"
        } finally { $io.Dispose() }
        Assert ((Clear-Errors $app).Errors -eq $CE_RXOVER) 'ClearCommError did not report the queue overrun'
        $notes += 'a queue overrun raises no event; a cancelled wait ends with 995'

        # A new wait can start after the cancel, and a new mask releases it with no events.
        $io = [PendingIo]::BeginWait($app)
        try {
            Assert (-not $io.Completed(200)) 'a new wait finished with no event'
            Assert ([VssComm]::SetMask($app, 0)) 'SetCommMask(0) failed'
            Assert ($io.Completed(2000)) 'SetCommMask(0) did not release the wait'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq 0 -and $io.Mask() -eq 0) ("the released wait ended with mask 0x{0:X}, error {1}" -f $io.Mask(), $err)
        } finally { $io.Dispose() }
        $notes += 'SetCommMask(0) releases a wait'

        # SetCommMask drops a kept event, and leaves the latched error alone.
        Assert ([VssComm]::SetMask($app, $EV_ERR)) 'SetCommMask failed'
        Send-LineError $hub $LE_PARITY
        Sync-Events $hub $app
        Assert ([VssComm]::SetMask($app, $EV_ERR)) 'SetCommMask failed'
        $io = [PendingIo]::BeginWait($app)
        try {
            Assert (-not $io.Completed(300)) 'SetCommMask did not drop the kept event'
            Assert ((Clear-Errors $app).Errors -eq $CE_RXPARITY) 'SetCommMask took the latched error away'
            Assert ([VssComm]::SetMask($app, 0)) 'SetCommMask(0) failed'
            Assert ($io.Completed(2000)) 'SetCommMask(0) did not release the wait'
        } finally { $io.Dispose() }
        $notes += 'a new mask drops a kept event, not the error'
        $notes
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

# Sends 'ab', one line error and 'cd' from the hub, then reads what the application gets and
# what ClearCommError reports.
function Invoke-ErrorStream($Hub, [IntPtr] $App, [uint32] $Errors, [uint32] $HasByte, [uint32] $Value) {
    $e = 0
    Assert ($Hub.Write([Text.Encoding]::ASCII.GetBytes('ab'), 5000, [ref]$e) -eq 2) "the hub write failed ($e)"
    Send-LineError $Hub $Errors $HasByte $Value
    Assert ($Hub.Write([Text.Encoding]::ASCII.GetBytes('cd'), 5000, [ref]$e) -eq 2) "the hub write failed ($e)"
    $r = Read-App $App 16
    $got = Clear-Errors $App
    [pscustomobject]@{ Text = [Text.Encoding]::ASCII.GetString($r.Bytes); Errors = $got.Errors; Queue = $got.Queue }
}

Add-Test 'D34' 'a line error keeps its place among the bytes, and the error and break characters replace the bad byte when asked' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    $notes = @()
    try {
        # Nothing asked for: the byte of a parity error is in the stream, in its place.
        $x = [int][char]'x'
        $r = Invoke-ErrorStream $hub $app $LE_PARITY 1 $x
        Assert ($r.Text -ceq 'abxcd') "a parity error with a byte: the application read '$($r.Text)'"
        Assert ($r.Errors -eq $CE_RXPARITY) ("ClearCommError reported 0x{0:X}" -f $r.Errors)
        $notes += 'abxcd'

        # A break and a framing error with no byte put nothing in the stream when no character is set.
        $r = Invoke-ErrorStream $hub $app $LE_BREAK 0 0
        Assert ($r.Text -ceq 'abcd') "a break with no break character: the application read '$($r.Text)'"
        Assert ($r.Errors -eq $CE_BREAK) ("ClearCommError reported 0x{0:X}" -f $r.Errors)
        $r = Invoke-ErrorStream $hub $app $LE_FRAMING 0 0
        Assert ($r.Text -ceq 'abcd') "a framing error with no error character: the application read '$($r.Text)'"
        Assert ($r.Errors -eq $CE_FRAME) ("ClearCommError reported 0x{0:X}" -f $r.Errors)
        $notes += 'no replacement: abcd'

        # fErrorChar: the error character stands for the byte of a parity or framing error, with or
        # without one, and goes in front of the byte of an overrun.
        Assert (([VssErr]::SetErrorMode($app, $false, $true, 0x3F, 0)) -eq 0) 'SetCommState with fErrorChar failed'
        $r = Invoke-ErrorStream $hub $app $LE_PARITY 1 $x
        Assert ($r.Text -ceq 'ab?cd') "parity with a byte and fErrorChar: the application read '$($r.Text)'"
        Assert ($r.Errors -eq $CE_RXPARITY) ("ClearCommError reported 0x{0:X}" -f $r.Errors)
        $r = Invoke-ErrorStream $hub $app $LE_FRAMING 0 0
        Assert ($r.Text -ceq 'ab?cd') "framing with no byte and fErrorChar: the application read '$($r.Text)'"
        Assert ($r.Errors -eq $CE_FRAME) ("ClearCommError reported 0x{0:X}" -f $r.Errors)
        $r = Invoke-ErrorStream $hub $app $LE_OVERRUN 1 $x
        Assert ($r.Text -ceq 'ab?xcd') "overrun with a byte and fErrorChar: the application read '$($r.Text)'"
        Assert ($r.Errors -eq $CE_OVERRUN) ("ClearCommError reported 0x{0:X}" -f $r.Errors)
        # A break is not a parity or framing error: the error character does not stand for it.
        $r = Invoke-ErrorStream $hub $app $LE_BREAK 0 0
        Assert ($r.Text -ceq 'abcd') "a break with only fErrorChar: the application read '$($r.Text)'"
        $notes += 'fErrorChar: ab?cd, ab?cd, ab?xcd, abcd'

        # The break character, which only the raw requests can set (after SetCommState, which resets it).
        Assert (([VssErr]::SetBreakChar($app, $true, 0x21)) -eq 0) 'setting the break character failed'
        $r = Invoke-ErrorStream $hub $app $LE_BREAK 0 0
        Assert ($r.Text -ceq 'ab!cd') "a break with a break character: the application read '$($r.Text)'"
        Assert ($r.Errors -eq $CE_BREAK) ("ClearCommError reported 0x{0:X}" -f $r.Errors)
        $r = Invoke-ErrorStream $hub $app $LE_BREAK 1 $x
        Assert ($r.Text -ceq 'ab!cd') "a break with a byte and a break character: the application read '$($r.Text)'"
        $notes += 'break character: ab!cd'

        # A new open starts without any of these.
        [VssComm]::Close($app); $app = [IntPtr](-1)
        $app = Open-Port $port
        $r = Invoke-ErrorStream $hub $app $LE_BREAK 0 0
        Assert ($r.Text -ceq 'abcd') "after a new open a break put '$($r.Text)' in the stream"
        $notes += 'a new open starts clean'
        $notes
    } finally {
        if ($app -ne [IntPtr](-1)) { [VssComm]::Close($app) }
        $hub.Dispose()
    }
}

Add-Test 'D35' 'a line error waits for room behind the bytes before it, and ends when the hub closes' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        $e = 0
        $fill = New-Pattern 65536
        Assert ($hub.Write($fill, 5000, [ref]$e) -eq 65536) "the hub write of a full buffer failed ($e)"

        # No room for the byte: the request waits, and the error is not latched yet.
        $task = $hub.BeginLineError($LE_PARITY, 1, 0x78, 20000)
        Assert (-not $task.Wait(300)) 'the line error finished although the receive buffer was full'
        $got = Clear-Errors $app
        Assert ($got.Errors -eq 0 -and $got.Queue -eq 65536) ("while it waited ClearCommError reported 0x{0:X} and {1} bytes" -f $got.Errors, $got.Queue)

        # Reading makes room: the request finishes, with the byte behind the bytes that came first.
        $r = Read-App $app 100
        Assert ($r.Count -eq 100) "the application read $($r.Count) bytes, not 100"
        Assert ($task.Wait(5000)) 'the line error did not finish when there was room'
        Assert ($task.Result -eq 0) "the line error ended with error $($task.Result)"
        $got = Clear-Errors $app
        Assert ($got.Errors -eq $CE_RXPARITY) ("after it finished ClearCommError reported 0x{0:X}" -f $got.Errors)
        Assert ($got.Queue -eq 65437) "the queue held $($got.Queue) bytes, not 65437"
        $r = Read-App $app 65536
        Assert ($r.Count -eq 65437) "the application read $($r.Count) bytes, not 65437"
        Assert ($r.Bytes[0] -eq $fill[100] -and $r.Bytes[65435] -eq $fill[65535] -and $r.Bytes[65436] -eq 0x78) 'the byte of the error is not behind the bytes that came before it'

        # A request that waits ends when the hub handle closes, and latches nothing.
        Assert ($hub.Write($fill, 5000, [ref]$e) -eq 65536) "the second hub write of a full buffer failed ($e)"
        $task = $hub.BeginLineError($LE_PARITY, 1, 0x78, 20000)
        Assert (-not $task.Wait(300)) 'the second line error finished although the receive buffer was full'
        $hub.Dispose()
        Assert ($task.Wait(5000)) 'the waiting line error did not end when the hub closed'
        Assert ($task.Result -eq $ERROR_OPERATION_ABORTED) "the waiting line error ended with error $($task.Result), not $ERROR_OPERATION_ABORTED"
        $got = Clear-Errors $app
        Assert ($got.Errors -eq 0) ("the request that was cancelled latched 0x{0:X}" -f $got.Errors)
        'waited at 65536 bytes, finished behind the bytes when 100 were read; cancelled (995) with nothing latched when the hub closed'
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D36' 'abort on error ends the reads and writes until ClearCommError' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    $notes = @()
    try {
        Assert (([VssErr]::SetErrorMode($app, $true, $false, 0, 0)) -eq 0) 'SetCommState with fAbortOnError failed'
        [VssComm]::SetTimeouts($app, 0, 0, 0, 0, 0) | Out-Null

        # A read and a write that wait, then an error.
        $read = [PendingIo]::BeginRead($app, 10)
        try {
            $task = [HubIo]::WriteAsync($app, (New-Pattern 300000))
            $until = (Get-Date).AddSeconds(5)
            while ($hub.GetState()[10] -eq 0 -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 20 }
            Assert (-not $read.Completed(200)) 'the read finished with nothing to read'
            Assert (-not $task.Wait(300)) 'the write finished before the hub read anything'

            Send-LineError $hub $LE_FRAMING
            Assert ($read.Completed(5000)) 'the error did not end the waiting read'
            $err = 0
            $read.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq $ERROR_OPERATION_ABORTED) "the waiting read ended with error $err, not $ERROR_OPERATION_ABORTED"
            Assert ($task.Wait(5000)) 'the error did not end the waiting write'
            Assert ($task.Result -eq -1) "the waiting write returned $($task.Result), not -1"
        } finally { $read.Dispose() }
        $notes += 'a waiting read and a waiting write end with 995'

        # Until ClearCommError, every new request but that one fails.
        $e = [VssErr]::ReadError($app, 5000)
        Assert ($e -eq $ERROR_OPERATION_ABORTED) "a read after the error gave error $e, not $ERROR_OPERATION_ABORTED"
        $e = [VssErr]::MaskError($app, $EV_RXCHAR)
        Assert ($e -eq $ERROR_OPERATION_ABORTED) "SetCommMask after the error gave error $e, not $ERROR_OPERATION_ABORTED"
        Assert ([VssComm]::Write($app, [Text.Encoding]::ASCII.GetBytes('x')) -eq -1) 'a write after the error was accepted'
        Assert ((Clear-Errors $app).Errors -eq $CE_FRAME) 'ClearCommError did not report the framing error'
        $notes += 'new requests fail with 995 until ClearCommError'

        # And then the port works again. The write that was ended left its bytes in Tx.
        Assert ([VssComm]::Purge($app, $PURGE_ALL)) 'PurgeComm failed after ClearCommError'
        $left = $hub.Read(65536, 500, [ref]$e)
        Assert ($null -eq $left -and $e -eq [HubIo]::TimedOut) 'the purge left bytes for the hub'
        Assert ($hub.Write([Text.Encoding]::ASCII.GetBytes('ok'), 5000, [ref]$e) -eq 2) "the hub write failed ($e)"
        $r = Read-App $app 2
        Assert ($r.Count -eq 2 -and [Text.Encoding]::ASCII.GetString($r.Bytes) -ceq 'ok') "the application read '$([Text.Encoding]::ASCII.GetString($r.Bytes))' after the error was cleared"
        $notes += 'the port works after ClearCommError'

        # The bytes written before the error reach a read that waits, which is not ended. The buffer is
        # full, so the four bytes and the error queue up behind it, in that order.
        Assert ([VssComm]::SetTimeouts($app, 0, 0, 0, 0, 0)) 'SetCommTimeouts failed'
        $fill = New-Pattern 65536
        Assert ($hub.Write($fill, 5000, [ref]$e) -eq 65536) "the hub write of a full buffer failed ($e)"
        $abcd = [Text.Encoding]::ASCII.GetBytes('abcd')
        $bytes = $hub.BeginWrite($abcd, 20000)
        $error_ = $hub.BeginLineError($LE_FRAMING, 0, 0, 20000)
        Assert (-not $bytes.Wait(300)) 'the four bytes went in although the buffer was full'
        Assert (-not $error_.Wait(0)) 'the error did not wait behind the bytes'
        $read = [PendingIo]::BeginRead($app, 65540)
        try {
            Assert ($read.Completed(10000)) 'the read of 65540 bytes did not finish'
            $err = 0
            $n = $read.Finish($true, [ref]$err)
            Assert ($err -eq 0 -and $n -eq 65540) "the read ended with error $err and $n bytes, not 65540"
            $data = $read.Data($n)
            for ($i = 0; $i -lt 65536; $i++) { if ($data[$i] -ne $fill[$i]) { throw "byte $i differs" } }
            Assert ([Text.Encoding]::ASCII.GetString($data, 65536, 4) -ceq 'abcd') 'the four bytes before the error were not in the read'
        } finally { $read.Dispose() }
        Assert ($bytes.Wait(5000) -and $bytes.Result -eq 4) 'the hub write of the four bytes did not finish'
        Assert ($error_.Wait(5000) -and $error_.Result -eq 0) 'the line error did not finish'
        Assert ((Clear-Errors $app).Errors -eq $CE_FRAME) 'ClearCommError did not report the framing error after the read'
        $notes += 'a read that waits takes the bytes before the error first'

        # Without fAbortOnError an error ends nothing.
        Assert (([VssErr]::SetErrorMode($app, $false, $false, 0, 0)) -eq 0) 'SetCommState without fAbortOnError failed'
        Assert ([VssComm]::SetTimeouts($app, 0, 0, 0, 0, 0)) 'SetCommTimeouts failed'
        $read = [PendingIo]::BeginRead($app, 10)
        try {
            Send-LineError $hub $LE_PARITY
            Assert (-not $read.Completed(300)) 'a parity error ended a read without fAbortOnError'
            Assert ([VssErr]::MaskError($app, 0) -eq 0) 'SetCommMask failed after an error without fAbortOnError'
            Assert ([VssComm]::Write($app, [Text.Encoding]::ASCII.GetBytes('z')) -eq 1) 'a write failed after an error without fAbortOnError'
            Assert ([Text.Encoding]::ASCII.GetString((Read-Hub $hub 1)) -ceq 'z') 'the hub read another byte'
            Assert ($read.Cancel()) 'CancelIoEx failed'
            Assert ($read.Completed(2000)) 'the cancelled read did not end'
            $err = 0
            $read.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq $ERROR_OPERATION_ABORTED) "the cancelled read ended with error $err"
        } finally { $read.Dispose() }
        Assert ((Clear-Errors $app).Errors -eq $CE_RXPARITY) 'ClearCommError did not report the parity error'
        $notes += 'without fAbortOnError an error ends nothing'
        $notes
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D37' 'EV_RXFLAG reports the event character' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        $e = 0
        Assert (([VssErr]::SetErrorMode($app, $false, $false, 0, 13)) -eq 0) 'SetCommState with an event character failed'
        Assert ([VssComm]::SetMask($app, $EV_RXFLAG)) 'SetCommMask failed'

        $io = [PendingIo]::BeginWait($app)
        try {
            Assert ($hub.Write([Text.Encoding]::ASCII.GetBytes('abc'), 5000, [ref]$e) -eq 3) "the hub write failed ($e)"
            Assert (-not $io.Completed(300)) 'bytes without the event character completed the wait'
            Assert ($hub.Write([byte[]]@(100, 13), 5000, [ref]$e) -eq 2) "the hub write failed ($e)"
            Assert ($io.Completed(5000)) 'the event character did not complete the wait'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq 0 -and $io.Mask() -eq $EV_RXFLAG) ("the wait ended with mask 0x{0:X}, error {1}" -f $io.Mask(), $err)
        } finally { $io.Dispose() }

        # The character that comes with a line error counts too, and another one does not.
        $io = [PendingIo]::BeginWait($app)
        try {
            Send-LineError $hub $LE_PARITY 1 0x78
            Sync-Events $hub $app
            Assert (-not $io.Completed(200)) 'a parity error with another byte completed the wait'
            Send-LineError $hub $LE_PARITY 1 13
            Assert ($io.Completed(5000)) 'a parity error with the event character did not complete the wait'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq 0 -and $io.Mask() -eq $EV_RXFLAG) ("the wait ended with mask 0x{0:X}, error {1}" -f $io.Mask(), $err)
        } finally { $io.Dispose() }
        'EV_RXFLAG when the event character arrives, with a hub write or a line error; not for other bytes'
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D38' 'cancelling and closing with a wait pending never loses or doubles a completion' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    $completed = 0
    $cancelled = 0
    try {
        # A wait for EV_BREAK and a break race with the cancel of the wait. The break starts on another
        # thread, and the cancel comes a little later each round, so that it lands before, during
        # and after the driver handles the break. Either the wait gets the event or it ends with
        # 995; the break request always succeeds and the error is always latched.
        for ($i = 0; $i -lt 200; $i++) {
            Assert ([VssComm]::SetMask($app, $EV_BREAK)) "round ${i}: SetCommMask failed"
            $io = [PendingIo]::BeginWait($app)
            try {
                $task = $hub.LineErrorAsync($LE_BREAK, 0, 0, 5000)
                [Threading.Thread]::SpinWait(($i % 40) * 25)
                $io.Cancel() | Out-Null
                Assert ($io.Completed(5000)) "round ${i}: the wait did not end"
                $err = 0
                $io.Finish($true, [ref]$err) | Out-Null
                if ($err -eq 0) {
                    Assert ($io.Mask() -eq $EV_BREAK) ("round ${i}: the wait ended with mask 0x{0:X}" -f $io.Mask())
                    $completed++
                } elseif ($err -eq $ERROR_OPERATION_ABORTED) {
                    $cancelled++
                } else {
                    throw "round ${i}: the wait ended with error $err"
                }
                Assert ($task.Wait(5000)) "round ${i}: the break request did not finish"
                Assert ($task.Result -eq 0) "round ${i}: the break request ended with error $($task.Result)"
            } finally { $io.Dispose() }
            $got = Clear-Errors $app
            Assert ($got.Errors -eq $CE_BREAK) ("round ${i}: ClearCommError reported 0x{0:X}" -f $got.Errors)
        }

        # Closing the handle with a wait pending and an error latched: the wait ends, and the next
        # open starts with no error and works.
        [VssComm]::Close($app); $app = [IntPtr](-1)
        $app = Open-Port $port
        Send-LineError $hub $LE_PARITY
        Assert ([VssComm]::SetMask($app, $EV_BREAK)) 'SetCommMask failed'
        $io = [PendingIo]::BeginWait($app)
        try {
            Assert (-not $io.Completed(200)) 'the wait finished with no event'
            [VssComm]::Close($app); $app = [IntPtr](-1)
            Assert ($io.Completed(5000)) 'the wait did not end when the handle closed'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            $closeEnd = "error $err, mask 0x$('{0:X}' -f $io.Mask())"
        } finally { $io.Dispose() }
        $app = Open-Port $port
        $got = Clear-Errors $app
        Assert ($got.Errors -eq 0) ("a new open found 0x{0:X} left by the last one" -f $got.Errors)
        $e = 0
        Assert ($hub.Write([Text.Encoding]::ASCII.GetBytes('z'), 5000, [ref]$e) -eq 1) "the hub write failed ($e)"
        $r = Read-App $app 1
        Assert ($r.Count -eq 1 -and [Text.Encoding]::ASCII.GetString($r.Bytes) -ceq 'z') 'the port did not carry a byte after the close'
        "200 rounds: $completed waits got EV_BREAK, $cancelled were cancelled, every break was latched; the handle closed with a wait pending ($closeEnd), and the port opens clean"
    } finally {
        if ($app -ne [IntPtr](-1)) { [VssComm]::Close($app) }
        $hub.Dispose()
    }
}

Add-Test 'D39' 'the error, break and event characters do not survive the close' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    $FLOW_ERROR_CHAR = 0x04; $FLOW_BREAK_CHAR = 0x10
    try {
        $e = 0
        Assert (([VssErr]::SetErrorMode($app, $false, $true, 0x3F, 13)) -eq 0) 'SetCommState with an error and an event character failed'
        Assert (([VssErr]::SetBreakChar($app, $true, 0x7E)) -eq 0) 'SetBreakChar failed'
        $ec = [byte]0; $bc = [byte]0; $vc = [byte]0; $flow = 0
        Assert (([VssErr]::GetSpecial($app, [ref]$ec, [ref]$bc, [ref]$vc, [ref]$flow)) -eq 0) 'reading the characters failed'
        Assert ($ec -eq 0x3F -and $bc -eq 0x7E -and $vc -eq 13) "the characters read back as $ec, $bc, $vc"
        Assert (($flow -band ($FLOW_ERROR_CHAR -bor $FLOW_BREAK_CHAR)) -eq ($FLOW_ERROR_CHAR -bor $FLOW_BREAK_CHAR)) ("the switches read back as 0x{0:X}" -f $flow)

        [VssComm]::Close($app); $app = [IntPtr](-1)
        $app = Open-Port $port

        Assert (([VssErr]::GetSpecial($app, [ref]$ec, [ref]$bc, [ref]$vc, [ref]$flow)) -eq 0) 'reading the characters after the reopen failed'
        Assert ($ec -eq 0 -and $bc -eq 0 -and $vc -eq 0) "the new open found the characters $ec, $bc, $vc"
        Assert (($flow -band ($FLOW_ERROR_CHAR -bor $FLOW_BREAK_CHAR)) -eq 0) ("the new open found the switches 0x{0:X}" -f $flow)

        # A break puts no break character into the stream, and a parity error leaves the byte.
        Send-LineError $hub $LE_BREAK
        $got = Clear-Errors $app
        Assert ($got.Errors -eq $CE_BREAK -and $got.Queue -eq 0) ("a break left 0x{0:X} and {1} bytes" -f $got.Errors, $got.Queue)
        Send-LineError $hub $LE_PARITY 1 0x41
        $r = Read-App $app 1
        Assert ($r.Count -eq 1 -and $r.Bytes[0] -eq 0x41) 'a parity error did not leave its own byte'
        Clear-Errors $app | Out-Null

        # The old event character, 13, raises nothing; the default one, 0, does.
        Assert ([VssComm]::SetMask($app, $EV_RXFLAG)) 'SetCommMask failed'
        $io = [PendingIo]::BeginWait($app)
        try {
            Assert ($hub.Write([byte[]]@(13), 5000, [ref]$e) -eq 1) "the hub write failed ($e)"
            Assert (-not $io.Completed(300)) 'the event character of the last program completed the wait'
            Assert ($hub.Write([byte[]]@(0), 5000, [ref]$e) -eq 1) "the hub write failed ($e)"
            Assert ($io.Completed(5000)) 'the default event character did not complete the wait'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq 0 -and $io.Mask() -eq $EV_RXFLAG) ("the wait ended with mask 0x{0:X}, error {1}" -f $io.Mask(), $err)
        } finally { $io.Dispose() }
        'the error, break and event characters and their switches are back to 0 after a close; no old error, break or event character is used'
    } finally {
        if ($app -ne [IntPtr](-1)) { [VssComm]::Close($app) }
        $hub.Dispose()
    }
}

Add-Test 'D40' 'an event raised after a wait was cancelled is kept for the next wait' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    $seen = 0
    $kept = 0
    try {
        $e = 0
        # Without a race: cancel the wait, then raise the events, then wait again.
        Assert ([VssComm]::SetMask($app, $EV_RXCHAR -bor $EV_BREAK)) 'SetCommMask failed'
        $io = [PendingIo]::BeginWait($app)
        try {
            Assert (-not $io.Completed(100)) 'the wait finished with no event'
            Assert ($io.Cancel()) 'CancelIoEx failed'
            Assert ($io.Completed(5000)) 'the cancelled wait did not end'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq $ERROR_OPERATION_ABORTED) "the cancelled wait ended with error $err"
        } finally { $io.Dispose() }
        Assert ($hub.Write([Text.Encoding]::ASCII.GetBytes('a'), 5000, [ref]$e) -eq 1) "the hub write failed ($e)"
        Send-LineError $hub $LE_BREAK
        $io = [PendingIo]::BeginWait($app)
        try {
            Assert ($io.Completed(2000)) 'the next wait did not return the events raised after the cancel'
            $err = 0
            $io.Finish($true, [ref]$err) | Out-Null
            Assert ($err -eq 0 -and $io.Mask() -eq ($EV_RXCHAR -bor $EV_BREAK)) ("the next wait ended with mask 0x{0:X}, error {1}" -f $io.Mask(), $err)
        } finally { $io.Dispose() }
        Clear-Errors $app | Out-Null
        Read-App $app 1 | Out-Null

        # With the race of D38: in every round the break is seen by the cancelled wait or, if that
        # ended with 995, by the next one at once. It is never lost.
        for ($i = 0; $i -lt 300; $i++) {
            Assert ([VssComm]::SetMask($app, $EV_BREAK)) "round ${i}: SetCommMask failed"
            $io = [PendingIo]::BeginWait($app)
            try {
                $task = $hub.LineErrorAsync($LE_BREAK, 0, 0, 5000)
                [Threading.Thread]::SpinWait(($i % 40) * 25)
                $io.Cancel() | Out-Null
                Assert ($io.Completed(5000)) "round ${i}: the wait did not end"
                $err = 0
                $io.Finish($true, [ref]$err) | Out-Null
                Assert ($task.Wait(5000)) "round ${i}: the break request did not finish"
                Assert ($task.Result -eq 0) "round ${i}: the break request ended with error $($task.Result)"
                if ($err -eq 0) {
                    Assert ($io.Mask() -eq $EV_BREAK) ("round ${i}: the wait ended with mask 0x{0:X}" -f $io.Mask())
                    $seen++
                    Clear-Errors $app | Out-Null
                    continue
                }
                Assert ($err -eq $ERROR_OPERATION_ABORTED) "round ${i}: the wait ended with error $err"
            } finally { $io.Dispose() }
            $next = [PendingIo]::BeginWait($app)
            try {
                Assert ($next.Completed(2000)) "round ${i}: the break was lost: the cancelled wait did not get it and the next one did not either"
                $err = 0
                $next.Finish($true, [ref]$err) | Out-Null
                Assert ($err -eq 0 -and $next.Mask() -eq $EV_BREAK) ("round ${i}: the next wait ended with mask 0x{0:X}, error {1}" -f $next.Mask(), $err)
                $kept++
            } finally { $next.Dispose() }
            $got = Clear-Errors $app
            Assert ($got.Errors -eq $CE_BREAK) ("round ${i}: ClearCommError reported 0x{0:X}" -f $got.Errors)
        }
        "300 rounds: $seen breaks seen by the wait that was cancelled, $kept kept for the next wait, none lost; events raised after a cancel are kept"
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Add-Test 'D41' 'abort on error takes back the bytes a waiting write had put in the transmit queue' {
    $hub = Open-Hub $hubPath
    $app = Open-Port $port
    try {
        $e = 0
        Assert (([VssErr]::SetErrorMode($app, $true, $false, 0, 0)) -eq 0) 'SetCommState with fAbortOnError failed'
        Assert ([VssComm]::SetTimeouts($app, 0, 0, 0, 0, 0)) 'SetCommTimeouts failed'

        # The hub reads some of the write, then the error: what the hub read is what was sent, and the
        # write reports that.
        $io = [PendingIo]::BeginWrite($app, (New-Pattern 300000))
        try {
            $until = (Get-Date).AddSeconds(5)
            while ($hub.GetState()[10] -eq 0 -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 20 }
            Assert (-not $io.Completed(200)) 'the write finished before the hub read anything'
            $first = Read-Hub $hub 1000
            Send-LineError $hub $LE_FRAMING
            Assert ($io.Completed(5000)) 'the error did not end the waiting write'
            $err = 0
            $n = $io.Finish($true, [ref]$err)
            Assert ($err -eq $ERROR_OPERATION_ABORTED) "the aborted write ended with error $err"
            Assert ($n -eq 1000) "the aborted write reported $n bytes, and the hub had read 1000"
        } finally { $io.Dispose() }
        Assert ((Clear-Errors $app).Errors -eq $CE_FRAME) 'ClearCommError did not report the framing error'
        $left = $hub.Read(65536, 500, [ref]$e)
        Assert ($null -eq $left -and $e -eq [HubIo]::TimedOut) 'the aborted write left bytes for the hub'

        # Bytes of an earlier write that was complete stay, and the write reports none of its own.
        Assert ([VssComm]::Write($app, [Text.Encoding]::ASCII.GetBytes('old')) -eq 3) 'the write before the error failed'
        $io = [PendingIo]::BeginWrite($app, (New-Pattern 300000))
        try {
            $until = (Get-Date).AddSeconds(5)
            while ($hub.GetState()[10] -eq 0 -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 20 }
            Assert (-not $io.Completed(200)) 'the second write finished before the hub read anything'
            Send-LineError $hub $LE_FRAMING
            Assert ($io.Completed(5000)) 'the error did not end the second waiting write'
            $err = 0
            $n = $io.Finish($true, [ref]$err)
            Assert ($err -eq $ERROR_OPERATION_ABORTED -and $n -eq 0) "the second aborted write ended with error $err and $n bytes, not 995 and 0"
        } finally { $io.Dispose() }
        Assert ((Clear-Errors $app).Errors -eq $CE_FRAME) 'ClearCommError did not report the second framing error'
        $rest = Read-Hub $hub 3
        Assert ([Text.Encoding]::ASCII.GetString($rest) -ceq 'old') 'the bytes of the earlier write were not kept'
        $left = $hub.Read(65536, 500, [ref]$e)
        Assert ($null -eq $left -and $e -eq [HubIo]::TimedOut) 'the second aborted write left bytes for the hub'
        'an aborted write takes back its bytes that the hub has not read, reports the ones it has, and leaves the earlier writes alone'
    } finally { [VssComm]::Close($app); $hub.Dispose() }
}

Write-Result -Path $ResultPath -Phase 'test' -Tests $tests
$tests | ForEach-Object { '{0,-6} {1,-4} {2}' -f $_.status, $_.id, $_.detail }
if ($tests | Where-Object { $_.status -eq 'fail' }) { exit 1 }
exit 0
