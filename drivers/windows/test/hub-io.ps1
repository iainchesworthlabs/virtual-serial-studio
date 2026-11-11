# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The hub end of a vsscom port, the control device, the helper pipe and a few process
# calls, for test-wow64.ps1. Run inside a guest. Dot-source it after common.ps1:
#
#   . $PSScriptRoot\common.ps1
#   . $PSScriptRoot\hub-io.ps1
#
# test-driver.ps1 has its own copies of the hub-end and control classes (HubIo, CtlIo,
# HelperIo), written for its tests. These have other names (Wow*) so the two never meet in
# one session, and they add what the hung-program tests need: waiting for AppOpen to change
# with a stopwatch, sending an arbitrary control code, an echo hub, and suspending a process
# or closing a handle in it from outside. The hub state layout is VSSCOM_HUB_STATE in
# drivers/windows/vsscom/public.h.
#
# Windows PowerShell 5.1 compiles this, so the C# is C# 5.

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

/// The hub end of a port: the second handle, opened by the interface path.
public sealed class WowHub : IDisposable
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
    // VSSCOM_HUB_LINE_ERROR { Errors, HasByte, Byte } (driver version 4), three 32-bit values.
    const uint IOCTL_LINE_ERROR = 0x80002014;
    // VSSCOM_HUB_STATE as of VSSCOM_VERSION 3: twelve words, AppProcessId last.
    const int StateWords = 12;
    public const int TimedOut = 258;

    // Indexes into the state words.
    public const int Generation = 0, AppOpen = 1, BaudRate = 2, DataBits = 3, Parity = 4, StopBits = 5,
                     Dtr = 6, Rts = 7, Break = 8, Lines = 9, TxQueued = 10, AppProcessId = 11;

    IntPtr handle;
    WowHub(IntPtr h) { handle = h; }

    /// Opens the hub end. Returns null and the Win32 error on failure.
    public static WowHub Open(string path, out int error)
    {
        IntPtr h = CreateFileW(path, 0xC0000000, 0, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        error = h == new IntPtr(-1) ? Marshal.GetLastWin32Error() : 0;
        return h == new IntPtr(-1) ? null : new WowHub(h);
    }

    int Finish(IntPtr ov, IntPtr signal, bool started, int ms, out int error)
    {
        error = 0;
        uint n;
        if (!started)
        {
            int e = Marshal.GetLastWin32Error();
            if (e != 997) { error = e; return -1; }
            if (WaitForSingleObject(signal, (uint)ms) != 0)
            {
                CancelIoEx(handle, ov);
                if (GetOverlappedResult(handle, ov, out n, true) && n > 0) return (int)n;
                error = TimedOut;
                return -1;
            }
        }
        if (!GetOverlappedResult(handle, ov, out n, true)) { error = Marshal.GetLastWin32Error(); return -1; }
        return (int)n;
    }

    IntPtr NewOverlapped(out IntPtr signal)
    {
        signal = CreateEventW(IntPtr.Zero, true, false, null);
        IntPtr ov = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));
        Marshal.StructureToPtr(new OVERLAPPED { Event = signal }, ov, false);
        return ov;
    }

    /// Writes all of `data`, waiting for the application to read. The count, or -1.
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

    /// Reads what the application wrote: at least one byte, at most `max`. Null on timeout or error.
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

    /// Sets the input lines the application sees (VSSCOM_LINE_* bits).
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

    /// Raises a line error at the application end (driver version 4). `errors` is one or more
    /// SERIAL_ERROR_* bits (break 1, framing 2, overrun 4, queue overrun 8, parity 0x10); `hasByte`
    /// is 1 when `value` is the character the receiver made of it. Returns the Win32 error, 0 for
    /// success, 258 when it did not finish in `ms`. The request is ordered with the hub's writes:
    /// it takes effect after the bytes written before it, and waits while the receive buffer is
    /// too full for the byte it adds.
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

    /// Sends a control code with no input and no output (a request such as a revoke) and
    /// returns the Win32 error, 0 for success, 258 when it did not finish in `ms`.
    public int Control(uint code, int ms)
    {
        IntPtr signal;
        IntPtr ov = NewOverlapped(out signal);
        try
        {
            int error;
            bool started = DeviceIoControl(handle, code, IntPtr.Zero, 0, IntPtr.Zero, 0, IntPtr.Zero, ov);
            int n = Finish(ov, signal, started, ms, out error);
            return n < 0 ? error : 0;
        }
        finally { CloseHandle(signal); Marshal.FreeHGlobal(ov); }
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

    /// Generation, AppOpen, BaudRate, DataBits, Parity, StopBits, Dtr, Rts, Break, Lines, TxQueued, AppProcessId.
    public uint[] GetState() { return StateCall(IOCTL_GET_STATE, 0, false, 5000); }

    /// Waits until the generation is not `known`; null when `ms` passes first.
    public uint[] WaitState(uint known, int ms) { return StateCall(IOCTL_WAIT_STATE, known, true, ms); }

    /// Waits until AppOpen is `want` and returns the milliseconds it took, or -1 when `ms` ran out
    /// (or the state could not be read). The stopwatch starts when the call starts.
    public int WaitAppOpen(uint want, int ms)
    {
        var watch = Stopwatch.StartNew();
        uint[] state = GetState();
        while (state != null)
        {
            if (state[AppOpen] == want) return (int)watch.ElapsedMilliseconds;
            int left = ms - (int)watch.ElapsedMilliseconds;
            if (left <= 0) return -1;
            uint[] next = WaitState(state[Generation], left);
            if (next != null) state = next;
            else if (watch.ElapsedMilliseconds >= ms) return -1;
        }
        return -1;
    }

    public void Dispose()
    {
        if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }
}

/// An echo hub: what the application writes comes back, DTR shows as DSR and DCD, and RTS as
/// CTS, as in the loopback of the driver. test-wow64.ps1 runs it behind the exercise.
public sealed class WowEcho : IDisposable
{
    readonly WowHub hub;
    Thread bytesThread, linesThread;
    volatile bool stop;
    public long Echoed;
    public string LastProblem = "";

    public WowEcho(WowHub h) { hub = h; }

    public void Start()
    {
        bytesThread = new Thread(Bytes) { IsBackground = true };
        linesThread = new Thread(Lines) { IsBackground = true };
        bytesThread.Start();
        linesThread.Start();
    }

    void Bytes()
    {
        while (!stop)
        {
            int error;
            byte[] got = hub.Read(4096, 100, out error);
            if (got == null)
            {
                if (error != WowHub.TimedOut && error != 0) { LastProblem = "hub read error " + error; Thread.Sleep(50); }
                continue;
            }
            int written = hub.Write(got, 5000, out error);
            if (written < 0) LastProblem = "hub write error " + error;
            else Echoed += written;
        }
    }

    void Lines()
    {
        uint generation = 0;
        uint last = 0xFFFFFFFF;
        uint[] state = hub.GetState();
        while (!stop)
        {
            if (state != null)
            {
                generation = state[WowHub.Generation];
                uint lines = 0;
                if (state[WowHub.Rts] != 0) lines |= 0x10;            // CTS
                if (state[WowHub.Dtr] != 0) lines |= 0x20 | 0x80;     // DSR and DCD
                if (lines != last) { hub.SetLines(lines); last = lines; }
            }
            state = hub.WaitState(generation, 100);
        }
    }

    public void Dispose()
    {
        stop = true;
        if (bytesThread != null) bytesThread.Join(3000);
        if (linesThread != null) linesThread.Join(3000);
    }
}

/// The control device (kernel-mode driver). Every call returns the Win32 error, 0 for success.
public sealed class WowCtl : IDisposable
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "CreateFileW")]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr input, uint inSize, IntPtr output, uint outSize, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    const uint IOCTL_CREATE_PORT = 0x80002044;
    const uint IOCTL_DESTROY_PORT = 0x80002048;

    IntPtr handle;
    WowCtl(IntPtr h) { handle = h; }

    public static WowCtl Open(string path, out int error)
    {
        IntPtr h = CreateFileW(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        error = h == new IntPtr(-1) ? Marshal.GetLastWin32Error() : 0;
        return h == new IntPtr(-1) ? null : new WowCtl(h);
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

    public int Create(uint flags, out uint slot)
    {
        var o = new uint[1];
        int e = Call(IOCTL_CREATE_PORT, new uint[] { flags, 0 }, o);
        slot = o[0];
        return e;
    }

    public int Destroy(uint slot) { return Call(IOCTL_DESTROY_PORT, new uint[] { slot }, null); }

    public void Dispose()
    {
        if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }
}

/// The helper service's pipe (user-mode driver). The same calls as WowCtl; a port lives as
/// long as the pipe that asked for it. Protocol: drivers/windows/vsscom_helper/protocol.h.
public sealed class WowHelper : IDisposable
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "CreateFileW")]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetNamedPipeHandleState(IntPtr h, ref uint mode, IntPtr maxCollect, IntPtr timeout);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TransactNamedPipe(IntPtr h, IntPtr input, uint inSize, IntPtr output, uint outSize, out uint read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    const uint OpCreate = 1;
    const uint OpDestroy = 2;

    IntPtr handle;
    WowHelper(IntPtr h) { handle = h; }

    public static WowHelper Open(string name, out int error)
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
        return new WowHelper(h);
    }

    int Raw(uint op, uint slot, uint reserved, out uint replySlot)
    {
        replySlot = 0;
        IntPtr inp = Marshal.AllocHGlobal(16);
        IntPtr outp = Marshal.AllocHGlobal(16);
        try
        {
            Marshal.WriteInt32(inp, 0, 1);
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

    public int Create(uint flags, out uint slot) { return Raw(OpCreate, 0, flags, out slot); }
    public int Destroy(uint slot) { uint ignored; return Raw(OpDestroy, slot, 0, out ignored); }

    public void Dispose()
    {
        if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }
}

/// What the hung-program tests do to another process.
public static class WowProc
{
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DuplicateHandle(IntPtr sourceProcess, IntPtr sourceHandle, IntPtr targetProcess,
                                       out IntPtr targetHandle, uint access, bool inherit, uint options);
    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CancelIoEx(IntPtr h, IntPtr overlapped);
    [DllImport("ntdll.dll")]
    static extern int NtSuspendProcess(IntPtr process);
    [DllImport("ntdll.dll")]
    static extern int NtResumeProcess(IntPtr process);

    const uint PROCESS_SUSPEND_RESUME = 0x0800;
    const uint PROCESS_DUP_HANDLE = 0x0040;
    const uint DUPLICATE_CLOSE_SOURCE = 1;
    const uint DUPLICATE_SAME_ACCESS = 2;

    static int Nt(int pid, bool suspend)
    {
        IntPtr p = OpenProcess(PROCESS_SUSPEND_RESUME, false, pid);
        if (p == IntPtr.Zero) return Marshal.GetLastWin32Error();
        try { return suspend ? NtSuspendProcess(p) : NtResumeProcess(p); }
        finally { CloseHandle(p); }
    }

    /// Suspends every thread of the process, as a debugger break or a hung UI thread would not,
    /// but with the same effect on its handles. Returns 0 on success, else an error or NTSTATUS.
    public static int Suspend(int pid) { return Nt(pid, true); }
    public static int Resume(int pid) { return Nt(pid, false); }

    /// Closes a handle in another process from outside: duplicates it into this process with
    /// DUPLICATE_CLOSE_SOURCE, cancels every request pending on the copy, then closes the copy.
    /// Returns "ok" or the step and error that failed.
    public static string CloseRemoteHandle(int pid, long handleValue, int ms)
    {
        // On its own thread, so that a close that waits for the driver cannot hang the test.
        string result = "timed out after " + ms + " ms";
        var worker = new Thread(delegate() { result = CloseRemote(pid, handleValue); }) { IsBackground = true };
        worker.Start();
        worker.Join(ms);
        return result;
    }

    static string CloseRemote(int pid, long handleValue)
    {
        IntPtr p = OpenProcess(PROCESS_DUP_HANDLE, false, pid);
        if (p == IntPtr.Zero) return "OpenProcess error " + Marshal.GetLastWin32Error();
        try
        {
            IntPtr copy;
            if (!DuplicateHandle(p, new IntPtr(handleValue), GetCurrentProcess(), out copy, 0, false,
                                 DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS))
                return "DuplicateHandle error " + Marshal.GetLastWin32Error();
            string cancel = CancelIoEx(copy, IntPtr.Zero) ? "cancel ok" : "cancel error " + Marshal.GetLastWin32Error();
            CloseHandle(copy);
            return "ok (" + cancel + ")";
        }
        finally { CloseHandle(p); }
    }
}
'@
