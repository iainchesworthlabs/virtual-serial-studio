/*
 * Copyright (c) 2026 Virtual Serial Studio contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A small serial client that behaves like an old Windows program, to run in the VMware
 * guests against a vsscom port. It is built three ways from this one file: Win32 (the
 * 32-bit programs this driver has to serve, running under WOW64), x64 and ARM64. Every
 * result goes to standard output (and to --out) as one JSON document.
 *
 *   vss_com_client probe    [--first 1] [--last 16] [--out file]
 *   vss_com_client open     --port COM5 [--out file]
 *   vss_com_client exercise --port COM5 [--out file]
 *   vss_com_client hold     --port COM5 --ready file [--log file] [--scenario overlapped|syncread]
 *   vss_com_client errors   --port COM5 --ready file [--out file]
 *
 * probe     opens COM1 to COM16 with and without the \\.\ prefix, one after the other, as a
 *           program does at start-up to fill its port list.
 * open      one open of one port, to learn what a second program gets.
 * exercise  the calls an old program makes, in order, against a port whose hub end answers
 *           (drivers/windows/test/test-wow64.ps1 runs an echo hub that sends back what is
 *           written and mirrors DTR to DSR and RTS to CTS).
 * hold      leaves requests pending on the port and stays alive, for the hung-program tests:
 *           it writes the --ready file when the requests are in, then waits to be killed,
 *           suspended or left alone. The ready file holds the process id and the handle value.
 * errors    what a program does about line errors, against a port whose hub end raises them
 *           (drivers/windows/test/test-wow64.ps1, test W5): it asks for the error character and
 *           waits for EV_ERR and EV_BREAK, reads ClearCommError and the bytes around the errors,
 *           then sets abort on error and checks that a waiting read ends. It writes the --ready
 *           file when it is set up. The hub end cannot see what the program does, so the program
 *           tells it where it is by raising RTS (the first error has been read) and DTR (the read
 *           that the third error must end is waiting).
 */

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------ */
/* A growing string, for the JSON.                                                       */
/* ------------------------------------------------------------------------------------ */

typedef struct text_buffer
{
    char *data;
    size_t length;
    size_t capacity;
} text_buffer;

static void text_reserve(text_buffer *t, size_t more)
{
    if (t->length + more + 1 <= t->capacity)
    {
        return;
    }
    size_t want = t->capacity ? t->capacity : 1024;
    while (want < t->length + more + 1)
    {
        want *= 2;
    }
    char *grown = (char *)realloc(t->data, want);
    if (grown == NULL)
    {
        fputs("out of memory\n", stderr);
        exit(3);
    }
    t->data = grown;
    t->capacity = want;
}

static void text_put(text_buffer *t, const char *s)
{
    size_t n = strlen(s);
    text_reserve(t, n);
    memcpy(t->data + t->length, s, n + 1);
    t->length += n;
}

static void text_fmt(text_buffer *t, const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof line, format, args);
    va_end(args);
    text_put(t, line);
}

/* A JSON string, quoted and escaped. */
static void text_string(text_buffer *t, const char *s)
{
    text_put(t, "\"");
    for (; *s != '\0'; ++s)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
        {
            char pair[3] = {'\\', (char)c, '\0'};
            text_put(t, pair);
        }
        else if (c < 0x20)
        {
            text_fmt(t, "\\u%04x", (unsigned)c);
        }
        else
        {
            char one[2] = {(char)c, '\0'};
            text_put(t, one);
        }
    }
    text_put(t, "\"");
}

/* ------------------------------------------------------------------------------------ */
/* The report: the steps of an exercise.                                                 */
/* ------------------------------------------------------------------------------------ */

typedef struct report
{
    text_buffer steps;
    int count;
    int failed;
    int required_failed;
} report;

static const char *error_name(DWORD error)
{
    switch (error)
    {
    case 0: return "OK";
    case 1: return "INCORRECT_FUNCTION";
    case 2: return "FILE_NOT_FOUND";
    case 3: return "PATH_NOT_FOUND";
    case 5: return "ACCESS_DENIED";
    case 6: return "INVALID_HANDLE";
    case 21: return "NOT_READY";
    case 31: return "GEN_FAILURE";
    case 32: return "SHARING_VIOLATION";
    case 50: return "NOT_SUPPORTED";
    case 55: return "DEV_NOT_EXIST";
    case 87: return "INVALID_PARAMETER";
    case 121: return "SEM_TIMEOUT";
    case 122: return "INSUFFICIENT_BUFFER";
    case 258: return "WAIT_TIMEOUT";
    case 995: return "OPERATION_ABORTED";
    case 997: return "IO_PENDING";
    case 1117: return "IO_DEVICE";
    case 1167: return "DEVICE_NOT_CONNECTED";
    default: return "";
    }
}

/*
 * One step. `required` says that a failure fails the exercise; a step that only records
 * what the driver does (GetCommConfig, a break) is not required. `ms` is a time in
 * milliseconds, or -1 when the step has none; the comparison between two builds ignores it.
 */
static void add_step(report *r, const char *name, int ok, DWORD error, int required, long ms, const char *format, ...)
{
    char detail[768];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(detail, sizeof detail, format, args);
    va_end(args);

    if (r->count > 0)
    {
        text_put(&r->steps, ",\n    ");
    }
    text_put(&r->steps, "{\"name\":");
    text_string(&r->steps, name);
    text_fmt(&r->steps, ",\"ok\":%s,\"required\":%s,\"error\":%lu,\"error_name\":", ok ? "true" : "false",
             required ? "true" : "false", (unsigned long)error);
    text_string(&r->steps, error_name(error));
    text_fmt(&r->steps, ",\"ms\":%ld,\"detail\":", ms);
    text_string(&r->steps, detail);
    text_put(&r->steps, "}");
    r->count++;
    if (!ok)
    {
        r->failed++;
        if (required)
        {
            r->required_failed++;
        }
    }
}

/* ------------------------------------------------------------------------------------ */
/* What this process is.                                                                 */
/* ------------------------------------------------------------------------------------ */

static const char *machine_name(USHORT machine)
{
    switch (machine)
    {
    case 0x014c: return "x86";
    case 0x8664: return "x64";
    case 0xAA64: return "arm64";
    case 0: return "none";
    default: return "other";
    }
}

typedef BOOL(WINAPI *is_wow64_process2_fn)(HANDLE, USHORT *, USHORT *);
typedef LONG(WINAPI *rtl_get_version_fn)(OSVERSIONINFOW *);

static void put_identity(text_buffer *t, const char *mode)
{
    USHORT process_machine = 0;
    USHORT native_machine = 0;
    int wow64 = 0;
    int known = 0;
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    is_wow64_process2_fn wow2 = kernel ? (is_wow64_process2_fn)(void *)GetProcAddress(kernel, "IsWow64Process2") : NULL;
    if (wow2 != NULL && wow2(GetCurrentProcess(), &process_machine, &native_machine))
    {
        wow64 = process_machine != 0;
        known = 1;
    }
    else
    {
        BOOL flag = FALSE;
        if (IsWow64Process(GetCurrentProcess(), &flag))
        {
            wow64 = flag ? 1 : 0;
        }
    }

    char version[64] = "";
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    rtl_get_version_fn rtl_version = ntdll ? (rtl_get_version_fn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (rtl_version != NULL)
    {
        OSVERSIONINFOW info;
        memset(&info, 0, sizeof info);
        info.dwOSVersionInfoSize = sizeof info;
        if (rtl_version(&info) == 0)
        {
            (void)_snprintf_s(version, sizeof version, _TRUNCATE, "%lu.%lu.%lu", (unsigned long)info.dwMajorVersion,
                              (unsigned long)info.dwMinorVersion, (unsigned long)info.dwBuildNumber);
        }
    }

    text_put(t, "\"program\":\"vss_com_client\",\"arch\":\"" VSS_CLIENT_ARCH "\"");
    text_fmt(t, ",\"wow64\":%s,\"native_machine\":", wow64 ? "true" : "false");
    text_string(t, known ? machine_name(native_machine) : "unknown");
    text_put(t, ",\"windows\":");
    text_string(t, version);
    text_put(t, ",\"mode\":");
    text_string(t, mode);
    text_fmt(t, ",\"pid\":%lu", (unsigned long)GetCurrentProcessId());
}

/* ------------------------------------------------------------------------------------ */
/* Small helpers for the port.                                                           */
/* ------------------------------------------------------------------------------------ */

static long since(ULONGLONG start)
{
    return (long)(GetTickCount64() - start);
}

static HANDLE open_path(const char *path, DWORD flags, DWORD *error)
{
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, flags, NULL);
    *error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    return h;
}

/* "COM5" -> "\\.\COM5". A name that already starts with the prefix is kept. */
static void device_path(const char *port, char *path, size_t size)
{
    if (strncmp(port, "\\\\.\\", 4) == 0)
    {
        (void)_snprintf_s(path, size, _TRUNCATE, "%s", port);
    }
    else
    {
        (void)_snprintf_s(path, size, _TRUNCATE, "\\\\.\\%s", port);
    }
}

static void init_overlapped(OVERLAPPED *ov)
{
    memset(ov, 0, sizeof *ov);
    ov->hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
}

static void free_overlapped(OVERLAPPED *ov)
{
    if (ov->hEvent != NULL)
    {
        CloseHandle(ov->hEvent);
        ov->hEvent = NULL;
    }
}

enum
{
    guard_timeout = 258 /* what a step reports when an operation was cancelled after the guard time */
};

/*
 * Waits for an overlapped operation that has started. Returns 1 and the byte count when it
 * finished in `guard_ms`; otherwise cancels it, waits for the cancel and returns 0.
 */
static int finish_overlapped(HANDLE h, OVERLAPPED *ov, BOOL started, DWORD guard_ms, DWORD *count, DWORD *error)
{
    *count = 0;
    *error = 0;
    if (!started)
    {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING)
        {
            *error = e;
            return 1;
        }
        if (WaitForSingleObject(ov->hEvent, guard_ms) != WAIT_OBJECT_0)
        {
            CancelIoEx(h, ov);
            GetOverlappedResult(h, ov, count, TRUE);
            *error = guard_timeout;
            return 0;
        }
    }
    if (!GetOverlappedResult(h, ov, count, TRUE))
    {
        *error = GetLastError();
    }
    return 1;
}

static int timed_read(HANDLE h, void *buffer, DWORD length, DWORD guard_ms, DWORD *count, DWORD *error, long *ms)
{
    OVERLAPPED ov;
    init_overlapped(&ov);
    ULONGLONG start = GetTickCount64();
    BOOL started = ReadFile(h, buffer, length, NULL, &ov);
    int finished = finish_overlapped(h, &ov, started, guard_ms, count, error);
    *ms = since(start);
    free_overlapped(&ov);
    return finished;
}

static int timed_write(HANDLE h, const void *buffer, DWORD length, DWORD guard_ms, DWORD *count, DWORD *error, long *ms)
{
    OVERLAPPED ov;
    init_overlapped(&ov);
    ULONGLONG start = GetTickCount64();
    BOOL started = WriteFile(h, buffer, length, NULL, &ov);
    int finished = finish_overlapped(h, &ov, started, guard_ms, count, error);
    *ms = since(start);
    free_overlapped(&ov);
    return finished;
}

static int set_timeouts(HANDLE h, DWORD interval, DWORD multiplier, DWORD constant, DWORD write_multiplier,
                        DWORD write_constant, DWORD *error)
{
    COMMTIMEOUTS to;
    to.ReadIntervalTimeout = interval;
    to.ReadTotalTimeoutMultiplier = multiplier;
    to.ReadTotalTimeoutConstant = constant;
    to.WriteTotalTimeoutMultiplier = write_multiplier;
    to.WriteTotalTimeoutConstant = write_constant;
    BOOL ok = SetCommTimeouts(h, &to);
    *error = ok ? 0 : GetLastError();
    return ok ? 1 : 0;
}

/* ------------------------------------------------------------------------------------ */
/* probe                                                                                  */
/* ------------------------------------------------------------------------------------ */

static void put_open_result(text_buffer *t, const char *key, const char *path)
{
    DWORD error = 0;
    HANDLE h = open_path(path, 0, &error);
    int usable = 0;
    if (h != INVALID_HANDLE_VALUE)
    {
        DCB dcb;
        memset(&dcb, 0, sizeof dcb);
        dcb.DCBlength = sizeof dcb;
        usable = GetCommState(h, &dcb) ? 1 : 0;
        CloseHandle(h);
    }
    text_fmt(t, "\"%s\":{\"ok\":%s,\"error\":%lu,\"error_name\":", key, h != INVALID_HANDLE_VALUE ? "true" : "false",
             (unsigned long)error);
    text_string(t, error_name(error));
    text_fmt(t, ",\"comm_state\":%s}", usable ? "true" : "false");
}

static int run_probe(int first, int last, text_buffer *json)
{
    text_put(json, "{");
    put_identity(json, "probe");
    text_put(json, ",\"ports\":[");
    for (int n = first; n <= last; ++n)
    {
        char plain[32];
        char prefixed[32];
        (void)_snprintf_s(plain, sizeof plain, _TRUNCATE, "COM%d", n);
        (void)_snprintf_s(prefixed, sizeof prefixed, _TRUNCATE, "\\\\.\\COM%d", n);
        text_fmt(json, "%s\n    {\"n\":%d,", n == first ? "" : ",", n);
        put_open_result(json, "plain", plain);
        text_put(json, ",");
        put_open_result(json, "prefixed", prefixed);
        text_put(json, "}");
    }
    text_put(json, "]}\n");
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* open                                                                                   */
/* ------------------------------------------------------------------------------------ */

static int run_open(const char *port, text_buffer *json)
{
    char path[64];
    device_path(port, path, sizeof path);
    DWORD error = 0;
    ULONGLONG start = GetTickCount64();
    HANDLE h = open_path(path, FILE_FLAG_OVERLAPPED, &error);
    long ms = since(start);
    text_put(json, "{");
    put_identity(json, "open");
    text_put(json, ",\"port\":");
    text_string(json, port);
    text_fmt(json, ",\"ok\":%s,\"error\":%lu,\"error_name\":", h != INVALID_HANDLE_VALUE ? "true" : "false",
             (unsigned long)error);
    text_string(json, error_name(error));
    text_fmt(json, ",\"ms\":%ld}\n", ms);
    if (h != INVALID_HANDLE_VALUE)
    {
        CloseHandle(h);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* exercise                                                                               */
/* ------------------------------------------------------------------------------------ */

static void step_state(report *r, HANDLE h, const char *name, DWORD baud, BYTE bits, BYTE parity, BYTE stop,
                       DWORD dtr, DWORD rts)
{
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb))
    {
        add_step(r, name, 0, GetLastError(), 1, -1, "GetCommState before the change failed");
        return;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = bits;
    dcb.Parity = parity;
    dcb.StopBits = stop;
    dcb.fBinary = TRUE;
    dcb.fParity = parity != NOPARITY;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fDtrControl = dtr & 3u;
    dcb.fRtsControl = rts & 3u;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb))
    {
        add_step(r, name, 0, GetLastError(), 1, -1,
                 "SetCommState failed (baud %lu, %u bits, parity %u, stop %u, dtr %lu, rts %lu)", (unsigned long)baud,
                 (unsigned)bits, (unsigned)parity, (unsigned)stop, (unsigned long)dtr, (unsigned long)rts);
        return;
    }
    DCB back;
    memset(&back, 0, sizeof back);
    back.DCBlength = sizeof back;
    if (!GetCommState(h, &back))
    {
        add_step(r, name, 0, GetLastError(), 1, -1, "GetCommState after the change failed");
        return;
    }
    int same = back.BaudRate == baud && back.ByteSize == bits && back.Parity == parity && back.StopBits == stop &&
               back.fDtrControl == dtr && back.fRtsControl == rts;
    DWORD modem = 0;
    Sleep(150);
    GetCommModemStatus(h, &modem);
    add_step(r, name, same, 0, 1, -1,
             "set baud %lu, %u bits, parity %u, stop %u, dtr control %lu, rts control %lu; read back baud %lu, %u bits, "
             "parity %u, stop %u, dtr control %lu, rts control %lu; modem status after 150 ms 0x%02lx",
             (unsigned long)baud, (unsigned)bits, (unsigned)parity, (unsigned)stop, (unsigned long)dtr,
             (unsigned long)rts, (unsigned long)back.BaudRate, (unsigned)back.ByteSize, (unsigned)back.Parity,
             (unsigned)back.StopBits, (unsigned long)back.fDtrControl, (unsigned long)back.fRtsControl,
             (unsigned long)(modem & 0xF0));
}

static void step_timeouts(report *r, HANDLE h, const char *name, DWORD interval, DWORD multiplier, DWORD constant,
                          DWORD write_multiplier, DWORD write_constant)
{
    DWORD error = 0;
    if (!set_timeouts(h, interval, multiplier, constant, write_multiplier, write_constant, &error))
    {
        add_step(r, name, 0, error, 1, -1, "SetCommTimeouts failed");
        return;
    }
    COMMTIMEOUTS back;
    memset(&back, 0, sizeof back);
    if (!GetCommTimeouts(h, &back))
    {
        add_step(r, name, 0, GetLastError(), 1, -1, "GetCommTimeouts failed");
        return;
    }
    int same = back.ReadIntervalTimeout == interval && back.ReadTotalTimeoutMultiplier == multiplier &&
               back.ReadTotalTimeoutConstant == constant && back.WriteTotalTimeoutMultiplier == write_multiplier &&
               back.WriteTotalTimeoutConstant == write_constant;
    add_step(r, name, same, 0, 1, -1, "set %lu/%lu/%lu write %lu/%lu; read back %lu/%lu/%lu write %lu/%lu",
             (unsigned long)interval, (unsigned long)multiplier, (unsigned long)constant,
             (unsigned long)write_multiplier, (unsigned long)write_constant, (unsigned long)back.ReadIntervalTimeout,
             (unsigned long)back.ReadTotalTimeoutMultiplier, (unsigned long)back.ReadTotalTimeoutConstant,
             (unsigned long)back.WriteTotalTimeoutMultiplier, (unsigned long)back.WriteTotalTimeoutConstant);
}

/* A read of ten bytes on an empty queue under the timeouts already set: 0 bytes after `low` to `high` ms. */
static void step_empty_read(report *r, HANDLE h, const char *name, long low, long high)
{
    unsigned char buffer[16];
    DWORD count = 0;
    DWORD error = 0;
    long ms = 0;
    int finished = timed_read(h, buffer, 10, 5000, &count, &error, &ms);
    int ok = finished && error == 0 && count == 0 && ms >= low && ms <= high;
    add_step(r, name, ok, error, 1, ms,
             "read of 10 bytes on an empty queue returned %lu bytes (%s); expected 0 bytes between %ld and %ld ms",
             (unsigned long)count, finished ? "finished" : "guard time ran out", low, high);
}

/* Reads until `want` bytes have come or `limit_ms` has passed. Returns the count. */
static DWORD read_all(HANDLE h, unsigned char *buffer, DWORD want, long limit_ms)
{
    DWORD got = 0;
    ULONGLONG start = GetTickCount64();
    while (got < want && since(start) < limit_ms)
    {
        DWORD count = 0;
        DWORD error = 0;
        long ms = 0;
        if (!timed_read(h, buffer + got, want - got, 1000, &count, &error, &ms) || error != 0)
        {
            break;
        }
        got += count;
    }
    return got;
}

static DWORD poll_modem(HANDLE h, DWORD mask, int want_set, long limit_ms, long *ms)
{
    ULONGLONG start = GetTickCount64();
    DWORD status = 0;
    for (;;)
    {
        status = 0;
        if (!GetCommModemStatus(h, &status))
        {
            *ms = since(start);
            return 0xFFFFFFFFu;
        }
        int is_set = (status & mask) == mask;
        if (is_set == want_set || since(start) >= limit_ms)
        {
            break;
        }
        Sleep(10);
    }
    *ms = since(start);
    return status;
}

static void step_escape(report *r, HANDLE h, const char *name, DWORD function, DWORD watch, int want_set, int required)
{
    if (!EscapeCommFunction(h, function))
    {
        add_step(r, name, 0, GetLastError(), required, -1, "EscapeCommFunction(%lu) failed", (unsigned long)function);
        return;
    }
    if (watch == 0)
    {
        add_step(r, name, 1, 0, required, -1, "EscapeCommFunction(%lu) worked", (unsigned long)function);
        return;
    }
    long ms = 0;
    DWORD status = poll_modem(h, watch, want_set, 1000, &ms);
    int seen = status != 0xFFFFFFFFu && ((status & watch) == watch) == (want_set != 0);
    add_step(r, name, seen, 0, required, ms,
             "EscapeCommFunction(%lu) worked; modem status 0x%02lx, line %s within 1 s: %s", (unsigned long)function,
             (unsigned long)(status & 0xF0), want_set ? "rose" : "fell", seen ? "yes" : "no");
}

static void step_event_wait(report *r, HANDLE h)
{
    DWORD error = 0;
    if (!SetCommMask(h, EV_RXCHAR | EV_TXEMPTY | EV_CTS | EV_DSR | EV_RLSD | EV_ERR | EV_BREAK))
    {
        add_step(r, "SetCommMask", 0, GetLastError(), 1, -1, "SetCommMask failed");
        return;
    }
    DWORD mask = 0;
    GetCommMask(h, &mask);
    add_step(r, "SetCommMask", 1, 0, 1, -1, "mask read back 0x%04lx", (unsigned long)mask);

    /* Drain what is left from earlier steps, so that the next event is the one this step causes. */
    set_timeouts(h, MAXDWORD, 0, 0, 0, 0, &error);
    {
        unsigned char scratch[256];
        DWORD n = 0;
        DWORD e = 0;
        long ms = 0;
        Sleep(100);
        while (timed_read(h, scratch, sizeof scratch, 1000, &n, &e, &ms) && e == 0 && n > 0)
        {
        }
    }
    set_timeouts(h, MAXDWORD, MAXDWORD, 2000, 0, 0, &error);

    /* The write below may end the wait with EV_TXEMPTY before the echo arrives with EV_RXCHAR, as it
       does on a real port, so the step collects events until it has seen EV_RXCHAR or 2 s are up. */
    OVERLAPPED ov;
    DWORD event_mask = 0;
    DWORD seen_mask = 0;
    DWORD count = 0;
    DWORD wait_error = 0;
    int finished = 1;
    BOOL started = FALSE;
    int waits = 0;
    DWORD pending_error = 0;
    long ms = 0;
    ULONGLONG start = GetTickCount64();
    for (; waits < 4 && finished && wait_error == 0 && (seen_mask & EV_RXCHAR) == 0; ++waits)
    {
        init_overlapped(&ov);
        event_mask = 0;
        started = WaitCommEvent(h, &event_mask, &ov);
        pending_error = started ? 0 : GetLastError();
        if (!started && pending_error != ERROR_IO_PENDING)
        {
            add_step(r, "WaitCommEvent overlapped", 0, pending_error, 1, -1, "WaitCommEvent did not start");
            free_overlapped(&ov);
            return;
        }
        if (waits == 0)
        {
            DWORD written = 0;
            long wms = 0;
            unsigned char byte = 0x55;
            timed_write(h, &byte, 1, 2000, &written, &wait_error, &wms);
            wait_error = 0;
        }
        if (!started)
        {
            /* finish_overlapped asks for the error of the call that started the wait. */
            SetLastError(pending_error);
        }
        finished = finish_overlapped(h, &ov, started, 2000, &count, &wait_error);
        seen_mask |= event_mask;
        free_overlapped(&ov);
    }
    ms = since(start);
    int ok = finished && wait_error == 0 && (seen_mask & EV_RXCHAR) != 0;
    add_step(r, "WaitCommEvent overlapped", ok, wait_error, 1, ms,
             "%s, then completed with event mask 0x%04lx after a one-byte write; EV_RXCHAR expected",
             started ? "completed at once" : "pending", (unsigned long)seen_mask);
    {
        unsigned char scratch[16];
        DWORD n = 0;
        DWORD e = 0;
        long m = 0;
        timed_read(h, scratch, sizeof scratch, 2000, &n, &e, &m);
    }

    /* The pattern of a program that stops its monitor thread: SetCommMask(0) wakes the wait. */
    init_overlapped(&ov);
    event_mask = 0xFFFF;
    started = WaitCommEvent(h, &event_mask, &ov);
    pending_error = started ? 0 : GetLastError();
    if (!started && pending_error != ERROR_IO_PENDING)
    {
        add_step(r, "SetCommMask(0) ends a pending WaitCommEvent", 0, pending_error, 1, -1,
                 "WaitCommEvent did not start");
        free_overlapped(&ov);
        return;
    }
    Sleep(100);
    SetCommMask(h, 0);
    start = GetTickCount64();
    if (!started)
    {
        SetLastError(pending_error);
    }
    finished = finish_overlapped(h, &ov, started, 2000, &count, &wait_error);
    ms = since(start);
    add_step(r, "SetCommMask(0) ends a pending WaitCommEvent", finished && wait_error == 0 && event_mask == 0,
             wait_error, 1, ms, "%s; event mask 0x%04lx",
             finished ? "the wait ended" : "the wait was still pending after 2 s", (unsigned long)event_mask);
    free_overlapped(&ov);
}

static void step_roundtrip(report *r, HANDLE h)
{
    unsigned char out[1000];
    unsigned char in[1000];
    for (size_t i = 0; i < sizeof out; ++i)
    {
        out[i] = (unsigned char)(i % 251);
    }
    memset(in, 0, sizeof in);
    DWORD error = 0;
    set_timeouts(h, MAXDWORD, MAXDWORD, 2000, 0, 0, &error);

    DWORD written = 0;
    long wms = 0;
    int finished = timed_write(h, out, sizeof out, 5000, &written, &error, &wms);
    add_step(r, "WriteFile overlapped 1000 bytes", finished && error == 0 && written == sizeof out, error, 1, wms,
             "wrote %lu of 1000 bytes", (unsigned long)written);

    ULONGLONG start = GetTickCount64();
    DWORD got = read_all(h, in, sizeof in, 4000);
    int same = got == sizeof in && memcmp(in, out, sizeof out) == 0;
    add_step(r, "ReadFile overlapped 1000 bytes echoed back", same, 0, 1, since(start),
             "read %lu of 1000 bytes; bytes %s", (unsigned long)got, same ? "match" : "differ");

    COMSTAT stat;
    DWORD errors = 0;
    memset(&stat, 0, sizeof stat);
    BOOL ok = ClearCommError(h, &errors, &stat);
    add_step(r, "ClearCommError", ok, ok ? 0 : GetLastError(), 1, -1, "errors 0x%lx, in queue %lu, out queue %lu",
             (unsigned long)errors, (unsigned long)stat.cbInQue, (unsigned long)stat.cbOutQue);

    /* Purge: bytes that came back are dropped, and the next read finds nothing. */
    DWORD w2 = 0;
    long m2 = 0;
    timed_write(h, out, 100, 2000, &w2, &error, &m2);
    Sleep(200);
    ok = PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    DWORD purge_error = ok ? 0 : GetLastError();
    set_timeouts(h, MAXDWORD, 0, 0, 0, 0, &error);
    DWORD after = 0;
    long m3 = 0;
    DWORD e3 = 0;
    timed_read(h, in, sizeof in, 2000, &after, &e3, &m3);
    add_step(r, "PurgeComm clears the received bytes", ok && after == 0, purge_error, 1, -1,
             "purge %s; %lu bytes were left to read after it", ok ? "worked" : "failed", (unsigned long)after);
    ok = PurgeComm(h, PURGE_RXABORT | PURGE_TXABORT);
    add_step(r, "PurgeComm aborts", ok, ok ? 0 : GetLastError(), 1, -1, "purge of RXABORT and TXABORT %s",
             ok ? "worked" : "failed");
}

static void step_properties(report *r, HANDLE h)
{
    COMMPROP prop;
    memset(&prop, 0, sizeof prop);
    if (!GetCommProperties(h, &prop))
    {
        add_step(r, "GetCommProperties", 0, GetLastError(), 1, -1, "GetCommProperties failed");
        return;
    }
    add_step(r, "GetCommProperties", 1, 0, 1, -1,
             "packet length %u, version %u, service mask 0x%lx, max tx queue %lu, max rx queue %lu, max baud 0x%lx, "
             "provider subtype 0x%lx, capabilities 0x%lx, settable params 0x%lx, settable baud 0x%lx, settable data "
             "0x%x, settable stop/parity 0x%x, current tx queue %lu, current rx queue %lu, provider specific 0x%lx",
             (unsigned)prop.wPacketLength, (unsigned)prop.wPacketVersion, (unsigned long)prop.dwServiceMask,
             (unsigned long)prop.dwMaxTxQueue, (unsigned long)prop.dwMaxRxQueue, (unsigned long)prop.dwMaxBaud,
             (unsigned long)prop.dwProvSubType, (unsigned long)prop.dwProvCapabilities,
             (unsigned long)prop.dwSettableParams, (unsigned long)prop.dwSettableBaud, (unsigned)prop.wSettableData,
             (unsigned)prop.wSettableStopParity, (unsigned long)prop.dwCurrentTxQueue,
             (unsigned long)prop.dwCurrentRxQueue, (unsigned long)prop.dwProvSpec1);
}

static void step_config(report *r, HANDLE h)
{
    /* The size query first, as programs do, then the call with a buffer. Not required: the
       driver reports no provider settings, and what a real serial port does is the reference. */
    DWORD size = 0;
    BOOL ok = GetCommConfig(h, NULL, &size);
    DWORD error = ok ? 0 : GetLastError();
    add_step(r, "GetCommConfig size query", ok || error == ERROR_INSUFFICIENT_BUFFER, error, 0, -1, "size %lu, %s",
             (unsigned long)size, ok ? "succeeded" : "failed");

    union
    {
        COMMCONFIG config;
        unsigned char bytes[512];
    } buffer;
    memset(&buffer, 0, sizeof buffer);
    buffer.config.dwSize = sizeof(COMMCONFIG);
    DWORD length = sizeof buffer;
    ok = GetCommConfig(h, &buffer.config, &length);
    error = ok ? 0 : GetLastError();
    add_step(r, "GetCommConfig", ok, error, 0, -1, "size %lu, version %u, provider subtype %lu, baud %lu",
             (unsigned long)length, (unsigned)buffer.config.wVersion, (unsigned long)buffer.config.dwProviderSubType,
             (unsigned long)buffer.config.dcb.BaudRate);
}

static int run_exercise(const char *port, text_buffer *json)
{
    report r;
    memset(&r, 0, sizeof r);
    char path[64];
    device_path(port, path, sizeof path);
    DWORD error = 0;

    HANDLE h = open_path(path, FILE_FLAG_OVERLAPPED, &error);
    add_step(&r, "CreateFile overlapped", h != INVALID_HANDLE_VALUE, error, 1, -1, "path %s", path);
    if (h != INVALID_HANDLE_VALUE)
    {
        BOOL ok = SetupComm(h, 4096, 4096);
        add_step(&r, "SetupComm", ok, ok ? 0 : GetLastError(), 1, -1, "asked for 4096 bytes each way");

        step_properties(&r, h);

        step_state(&r, h, "SetCommState 9600 8N1, DTR and RTS enabled", 9600, 8, NOPARITY, ONESTOPBIT,
                   DTR_CONTROL_ENABLE, RTS_CONTROL_ENABLE);
        /* After a SetCommState, so that the baud it reports is the one just set. A port keeps its
           settings from one open to the next, so before the first call it is whatever the program
           before this one left. */
        step_config(&r, h);
        step_state(&r, h, "SetCommState 19200 8N1, DTR and RTS disabled", 19200, 8, NOPARITY, ONESTOPBIT,
                   DTR_CONTROL_DISABLE, RTS_CONTROL_DISABLE);
        step_state(&r, h, "SetCommState 4800 7E2, RTS handshake", 4800, 7, EVENPARITY, TWOSTOPBITS,
                   DTR_CONTROL_DISABLE, RTS_CONTROL_HANDSHAKE);
        step_state(&r, h, "SetCommState 115200 8N1, RTS toggle", 115200, 8, NOPARITY, ONESTOPBIT, DTR_CONTROL_ENABLE,
                   RTS_CONTROL_TOGGLE);
        step_state(&r, h, "SetCommState 9600 8N1 to finish", 9600, 8, NOPARITY, ONESTOPBIT, DTR_CONTROL_DISABLE,
                   RTS_CONTROL_DISABLE);

        /* The timeout combinations old programs use. */
        step_timeouts(&r, h, "SetCommTimeouts read immediately", MAXDWORD, 0, 0, 0, 0);
        step_empty_read(&r, h, "read returns at once under MAXDWORD/0/0", 0, 500);
        step_timeouts(&r, h, "SetCommTimeouts wait for the first byte", MAXDWORD, MAXDWORD, 300, 0, 0);
        step_empty_read(&r, h, "read waits 300 ms under MAXDWORD/MAXDWORD/300", 200, 1500);
        step_timeouts(&r, h, "SetCommTimeouts total timeout", 0, 10, 200, 10, 200);
        step_empty_read(&r, h, "read of 10 bytes ends after 300 ms under 0/10/200", 200, 1500);
        step_timeouts(&r, h, "SetCommTimeouts interval timeout", 50, 0, 0, 0, 0);
        step_timeouts(&r, h, "SetCommTimeouts none (block)", 0, 0, 0, 0, 0);

        step_escape(&r, h, "EscapeCommFunction SETDTR raises DSR", SETDTR, MS_DSR_ON, 1, 1);
        step_escape(&r, h, "EscapeCommFunction CLRDTR drops DSR", CLRDTR, MS_DSR_ON, 0, 1);
        step_escape(&r, h, "EscapeCommFunction SETRTS raises CTS", SETRTS, MS_CTS_ON, 1, 1);
        step_escape(&r, h, "EscapeCommFunction CLRRTS drops CTS", CLRRTS, MS_CTS_ON, 0, 1);
        step_escape(&r, h, "EscapeCommFunction SETBREAK", SETBREAK, 0, 1, 0);
        step_escape(&r, h, "EscapeCommFunction CLRBREAK", CLRBREAK, 0, 1, 0);
        step_escape(&r, h, "EscapeCommFunction SETXOFF", SETXOFF, 0, 1, 0);
        step_escape(&r, h, "EscapeCommFunction SETXON", SETXON, 0, 1, 0);

        step_event_wait(&r, h);
        step_roundtrip(&r, h);

        DWORD modem = 0;
        ok = GetCommModemStatus(h, &modem);
        add_step(&r, "GetCommModemStatus", ok, ok ? 0 : GetLastError(), 1, -1, "status 0x%02lx",
                 (unsigned long)(modem & 0xF0));

        ok = CloseHandle(h);
        add_step(&r, "CloseHandle", ok, ok ? 0 : GetLastError(), 1, -1, "closed");
    }

    text_put(json, "{");
    put_identity(json, "exercise");
    text_put(json, ",\"port\":");
    text_string(json, port);
    text_fmt(json, ",\"summary\":{\"steps\":%d,\"failed\":%d,\"required_failed\":%d},\n  \"steps\":[\n    ", r.count,
             r.failed, r.required_failed);
    text_put(json, r.steps.data ? r.steps.data : "");
    text_put(json, "\n  ]}\n");
    free(r.steps.data);
    return r.required_failed == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------------------ */
/* hold                                                                                   */
/* ------------------------------------------------------------------------------------ */

static const char *g_log_path = NULL;

/* Appends one line to the log: what happened to a pending request, and when. */
static void log_line(const char *format, ...)
{
    if (g_log_path == NULL)
    {
        return;
    }
    char line[512];
    int n = _snprintf_s(line, sizeof line - 2, _TRUNCATE, "[%llu ms] ", (unsigned long long)GetTickCount64());
    if (n < 0)
    {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)_vsnprintf_s(line + n, sizeof line - 2 - (size_t)n, _TRUNCATE, format, args);
    va_end(args);
    strcat_s(line, sizeof line, "\r\n");
    HANDLE f = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(f, line, (DWORD)strlen(line), &written, NULL);
        CloseHandle(f);
    }
}

/* Writes the ready file whole, so the test never sees half of it. */
static int write_ready(const char *path, const char *content)
{
    char temp[MAX_PATH + 8];
    (void)_snprintf_s(temp, sizeof temp, _TRUNCATE, "%s.tmp", path);
    HANDLE f = CreateFileA(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(f, content, (DWORD)strlen(content), &written, NULL);
    CloseHandle(f);
    return ok && MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING);
}

typedef struct sync_read_job
{
    HANDLE handle;
} sync_read_job;

static DWORD WINAPI sync_read_thread(LPVOID parameter)
{
    sync_read_job *job = (sync_read_job *)parameter;
    unsigned char byte = 0;
    DWORD count = 0;
    log_line("synchronous read started");
    BOOL ok = ReadFile(job->handle, &byte, 1, &count, NULL);
    log_line("synchronous read ended: %s, error %lu, %lu bytes", ok ? "ok" : "failed",
             ok ? 0ul : (unsigned long)GetLastError(), (unsigned long)count);
    return 0;
}

static int run_hold(const char *port, const char *ready_path, const char *scenario)
{
    char path[64];
    device_path(port, path, sizeof path);
    int synchronous = strcmp(scenario, "syncread") == 0;
    DWORD error = 0;
    HANDLE h = open_path(path, synchronous ? 0 : FILE_FLAG_OVERLAPPED, &error);
    if (h == INVALID_HANDLE_VALUE)
    {
        char content[256];
        (void)_snprintf_s(content, sizeof content, _TRUNCATE,
                          "{\"ready\":false,\"pid\":%lu,\"open_error\":%lu,\"open_error_name\":\"%s\"}\n",
                          (unsigned long)GetCurrentProcessId(), (unsigned long)error, error_name(error));
        write_ready(ready_path, content);
        return 1;
    }

    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = 9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    SetCommState(h, &dcb);
    /* No timeouts at all: a read waits for its full length, a write for the hub. */
    set_timeouts(h, 0, 0, 0, 0, 0, &error);

    int read_pending = 0;
    int wait_pending = 0;
    int write_pending = 0;
    OVERLAPPED read_ov;
    OVERLAPPED wait_ov;
    OVERLAPPED write_ov;
    memset(&read_ov, 0, sizeof read_ov);
    memset(&wait_ov, 0, sizeof wait_ov);
    memset(&write_ov, 0, sizeof write_ov);
    unsigned char read_byte = 0;
    DWORD event_mask = 0;
    unsigned char *big = NULL;
    const DWORD big_size = 300000;

    if (synchronous)
    {
        static sync_read_job job;
        job.handle = h;
        HANDLE thread = CreateThread(NULL, 0, sync_read_thread, &job, 0, NULL);
        read_pending = thread != NULL;
        if (thread != NULL)
        {
            CloseHandle(thread);
        }
    }
    else
    {
        init_overlapped(&read_ov);
        init_overlapped(&wait_ov);
        init_overlapped(&write_ov);

        read_pending = !ReadFile(h, &read_byte, 1, NULL, &read_ov) && GetLastError() == ERROR_IO_PENDING;
        SetCommMask(h, EV_RXCHAR);
        wait_pending = !WaitCommEvent(h, &event_mask, &wait_ov) && GetLastError() == ERROR_IO_PENDING;

        big = (unsigned char *)malloc(big_size);
        if (big != NULL)
        {
            for (DWORD i = 0; i < big_size; ++i)
            {
                big[i] = (unsigned char)(i % 251);
            }
            write_pending = !WriteFile(h, big, big_size, NULL, &write_ov) && GetLastError() == ERROR_IO_PENDING;
        }
    }

    /* Give the driver a moment to take the bytes it will take and block on the rest. */
    Sleep(500);

    char content[512];
    (void)_snprintf_s(content, sizeof content, _TRUNCATE,
                      "{\"ready\":true,\"pid\":%lu,\"handle\":%llu,\"port\":\"%s\",\"scenario\":\"%s\","
                      "\"read_pending\":%s,\"wait_pending\":%s,\"write_pending\":%s,\"arch\":\"" VSS_CLIENT_ARCH "\"}\n",
                      (unsigned long)GetCurrentProcessId(), (unsigned long long)(ULONG_PTR)h, port, scenario,
                      read_pending ? "true" : "false", wait_pending ? "true" : "false",
                      write_pending ? "true" : "false");
    write_ready(ready_path, content);
    log_line("ready: read pending %d, wait pending %d, write pending %d", read_pending, wait_pending, write_pending);

    /* Stay alive with the handle open. Say when a pending request ends, and why, which is what
       a driver restart or a revoke shows. */
    int read_done = !read_pending || synchronous;
    int wait_done = !wait_pending;
    int write_done = !write_pending;
    for (;;)
    {
        if (!synchronous)
        {
            DWORD count = 0;
            if (!read_done && HasOverlappedIoCompleted(&read_ov))
            {
                BOOL ok = GetOverlappedResult(h, &read_ov, &count, FALSE);
                log_line("read ended: %s, error %lu, %lu bytes", ok ? "ok" : "failed",
                         ok ? 0ul : (unsigned long)GetLastError(), (unsigned long)count);
                read_done = 1;
            }
            if (!wait_done && HasOverlappedIoCompleted(&wait_ov))
            {
                BOOL ok = GetOverlappedResult(h, &wait_ov, &count, FALSE);
                log_line("wait for event ended: %s, error %lu, mask 0x%lx", ok ? "ok" : "failed",
                         ok ? 0ul : (unsigned long)GetLastError(), (unsigned long)event_mask);
                wait_done = 1;
            }
            if (!write_done && HasOverlappedIoCompleted(&write_ov))
            {
                BOOL ok = GetOverlappedResult(h, &write_ov, &count, FALSE);
                log_line("write ended: %s, error %lu, %lu of %lu bytes", ok ? "ok" : "failed",
                         ok ? 0ul : (unsigned long)GetLastError(), (unsigned long)count, (unsigned long)big_size);
                write_done = 1;
            }
        }
        Sleep(100);
    }
}

/* ------------------------------------------------------------------------------------ */
/* errors                                                                                 */
/* ------------------------------------------------------------------------------------ */

enum
{
    errors_wait_ms = 20000 /* how long a step waits for the hub, which sends the errors, before it gives up */
};

/* WaitCommEvent with a guard time. Returns 1 when it ended in `guard_ms`; `mask` is the events. */
static int wait_event(HANDLE h, DWORD guard_ms, DWORD *mask, DWORD *error, long *ms)
{
    OVERLAPPED ov;
    init_overlapped(&ov);
    *mask = 0;
    ULONGLONG start = GetTickCount64();
    BOOL started = WaitCommEvent(h, mask, &ov);
    DWORD count = 0;
    int finished = finish_overlapped(h, &ov, started, guard_ms, &count, error);
    *ms = since(start);
    free_overlapped(&ov);
    return finished;
}

/* ClearCommError: the error bits, which the call clears. */
static int clear_errors(HANDLE h, DWORD *errors, DWORD *error)
{
    COMSTAT stat;
    memset(&stat, 0, sizeof stat);
    *errors = 0;
    BOOL ok = ClearCommError(h, errors, &stat);
    *error = ok ? 0 : GetLastError();
    return ok ? 1 : 0;
}

/*
 * Reads until `want` bytes have come, `limit_ms` has passed or a read fails. The timeouts must
 * be MAXDWORD/MAXDWORD/constant, so that a read returns with the first byte there is. Returns
 * the byte count.
 */
static DWORD read_exact(HANDLE h, unsigned char *buffer, DWORD want, long limit_ms, DWORD *error)
{
    DWORD got = 0;
    ULONGLONG start = GetTickCount64();
    *error = 0;
    while (got < want && since(start) < limit_ms)
    {
        DWORD count = 0;
        long ms = 0;
        if (!timed_read(h, buffer + got, want - got, 3000, &count, error, &ms) || *error != 0)
        {
            break;
        }
        got += count;
    }
    return got;
}

/* The error character, the mask and the timeouts, set and read back. Returns 1 when the port is ready. */
static int errors_setup(report *r, HANDLE h)
{
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb))
    {
        add_step(r, "SetCommState with fErrorChar", 0, GetLastError(), 1, -1, "GetCommState before the change failed");
        return 0;
    }
    dcb.BaudRate = 9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    /* Programs that ask for the error character turn parity checking on with it. */
    dcb.fParity = TRUE;
    dcb.fErrorChar = TRUE;
    dcb.ErrorChar = '?';
    dcb.fAbortOnError = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(h, &dcb))
    {
        add_step(r, "SetCommState with fErrorChar", 0, GetLastError(), 1, -1, "SetCommState failed");
        return 0;
    }
    DCB back;
    memset(&back, 0, sizeof back);
    back.DCBlength = sizeof back;
    if (!GetCommState(h, &back))
    {
        add_step(r, "SetCommState with fErrorChar", 0, GetLastError(), 1, -1, "GetCommState after the change failed");
        return 0;
    }
    int same = back.fErrorChar && back.ErrorChar == '?' && !back.fAbortOnError;
    add_step(r, "SetCommState with fErrorChar", same, 0, 1, -1,
             "error character ?; read back fErrorChar %u, ErrorChar 0x%02x, fAbortOnError %u",
             (unsigned)back.fErrorChar, (unsigned)(unsigned char)back.ErrorChar, (unsigned)back.fAbortOnError);
    if (!same)
    {
        return 0;
    }

    if (!SetCommMask(h, EV_ERR | EV_BREAK))
    {
        add_step(r, "SetCommMask EV_ERR and EV_BREAK", 0, GetLastError(), 1, -1, "SetCommMask failed");
        return 0;
    }
    DWORD mask = 0;
    GetCommMask(h, &mask);
    add_step(r, "SetCommMask EV_ERR and EV_BREAK", mask == (EV_ERR | EV_BREAK), 0, 1, -1, "mask read back 0x%04lx",
             (unsigned long)mask);
    if (mask != (EV_ERR | EV_BREAK))
    {
        return 0;
    }

    /* A read returns with the first byte there is, and waits a second for one. */
    DWORD error = 0;
    if (!set_timeouts(h, MAXDWORD, MAXDWORD, 1000, 0, 0, &error))
    {
        add_step(r, "SetCommTimeouts", 0, error, 1, -1, "SetCommTimeouts failed");
        return 0;
    }
    return 1;
}

/* The first error is a parity error between 'a' and 'b'; the error character stands for its byte. */
static int errors_parity(report *r, HANDLE h)
{
    DWORD mask = 0;
    DWORD error = 0;
    long ms = 0;
    int finished = wait_event(h, errors_wait_ms, &mask, &error, &ms);
    int ok = finished && error == 0 && mask == EV_ERR;
    add_step(r, "WaitCommEvent ends with EV_ERR for a parity error", ok, error, 1, ms,
             "%s; event mask 0x%04lx, EV_ERR 0x%04lx expected",
             finished ? "the wait ended" : "the wait was still pending", (unsigned long)mask, (unsigned long)EV_ERR);
    if (!ok)
    {
        return 0;
    }

    DWORD errors = 0;
    ok = clear_errors(h, &errors, &error) && errors == CE_RXPARITY;
    add_step(r, "ClearCommError reports the parity error", ok, error, 1, -1, "errors 0x%lx, CE_RXPARITY 0x%lx expected",
             (unsigned long)errors, (unsigned long)CE_RXPARITY);
    if (!ok)
    {
        return 0;
    }

    unsigned char text[8];
    memset(text, 0, sizeof text);
    DWORD got = read_exact(h, text, 3, errors_wait_ms, &error);
    ok = got == 3 && memcmp(text, "a?b", 3) == 0;
    add_step(r, "ReadFile reads the bytes around the parity error", ok, error, 1, -1,
             "read %lu bytes, \"%.3s\"; \"a?b\" expected, the error character in place of the bad byte",
             (unsigned long)got, (const char *)text);
    return ok;
}

/* The second error is a break. RTS tells the hub that the first one has been read. */
static int errors_break(report *r, HANDLE h)
{
    if (!EscapeCommFunction(h, SETRTS))
    {
        add_step(r, "EscapeCommFunction SETRTS", 0, GetLastError(), 1, -1, "SETRTS failed");
        return 0;
    }
    DWORD mask = 0;
    DWORD error = 0;
    long ms = 0;
    int finished = wait_event(h, errors_wait_ms, &mask, &error, &ms);
    int ok = finished && error == 0 && mask == EV_BREAK;
    add_step(r, "WaitCommEvent ends with EV_BREAK for a break", ok, error, 1, ms,
             "%s; event mask 0x%04lx, EV_BREAK 0x%04lx expected",
             finished ? "the wait ended" : "the wait was still pending", (unsigned long)mask, (unsigned long)EV_BREAK);
    if (!ok)
    {
        return 0;
    }

    DWORD errors = 0;
    ok = clear_errors(h, &errors, &error) && errors == CE_BREAK;
    add_step(r, "ClearCommError reports the break", ok, error, 1, -1, "errors 0x%lx, CE_BREAK 0x%lx expected",
             (unsigned long)errors, (unsigned long)CE_BREAK);
    return ok;
}

/*
 * The third error is a framing error, and with abort on error set it ends the read that waits.
 * DTR tells the hub that the read is waiting. The hub then sends the error and the bytes "ok", which
 * wait in the buffer until ClearCommError has been called.
 */
static int errors_abort(report *r, HANDLE h)
{
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb))
    {
        add_step(r, "SetCommState with fAbortOnError", 0, GetLastError(), 1, -1,
                 "GetCommState before the change failed");
        return 0;
    }
    dcb.fAbortOnError = TRUE;
    /* Without the error character the framing error adds nothing to the stream: the next read is "ok". */
    dcb.fErrorChar = FALSE;
    if (!SetCommState(h, &dcb))
    {
        add_step(r, "SetCommState with fAbortOnError", 0, GetLastError(), 1, -1, "SetCommState failed");
        return 0;
    }
    DCB back;
    memset(&back, 0, sizeof back);
    back.DCBlength = sizeof back;
    if (!GetCommState(h, &back) || !back.fAbortOnError)
    {
        add_step(r, "SetCommState with fAbortOnError", 0, GetLastError(), 1, -1, "fAbortOnError did not read back");
        return 0;
    }
    add_step(r, "SetCommState with fAbortOnError", 1, 0, 1, -1, "fAbortOnError read back set, fErrorChar clear");

    /* No timeouts: the read waits for all four bytes, which never come. */
    DWORD error = 0;
    if (!set_timeouts(h, 0, 0, 0, 0, 0, &error))
    {
        add_step(r, "SetCommTimeouts", 0, error, 1, -1, "SetCommTimeouts failed");
        return 0;
    }

    OVERLAPPED ov;
    init_overlapped(&ov);
    unsigned char buffer[4];
    memset(buffer, 0, sizeof buffer);
    ULONGLONG start = GetTickCount64();
    BOOL started = ReadFile(h, buffer, sizeof buffer, NULL, &ov);
    DWORD start_error = started ? 0 : GetLastError();
    if (!started && start_error != ERROR_IO_PENDING)
    {
        add_step(r, "ReadFile of four bytes ends with ERROR_OPERATION_ABORTED", 0, start_error, 1, -1,
                 "the read did not start");
        free_overlapped(&ov);
        return 0;
    }
    if (!EscapeCommFunction(h, SETDTR))
    {
        DWORD dtr_error = GetLastError();
        add_step(r, "EscapeCommFunction SETDTR", 0, dtr_error, 1, -1, "SETDTR failed");
        CancelIoEx(h, &ov);
        GetOverlappedResult(h, &ov, &start_error, TRUE);
        free_overlapped(&ov);
        return 0;
    }
    DWORD count = 0;
    if (!started)
    {
        /* finish_overlapped asks for the error of the call that started the read. */
        SetLastError(start_error);
    }
    int finished = finish_overlapped(h, &ov, started, errors_wait_ms, &count, &error);
    long ms = since(start);
    free_overlapped(&ov);
    int ok = finished && error == ERROR_OPERATION_ABORTED;
    add_step(r, "ReadFile of four bytes ends with ERROR_OPERATION_ABORTED", ok, error, 1, ms,
             "%s; the read returned %lu bytes", finished ? "the read ended" : "the read was still waiting",
             (unsigned long)count);
    if (!ok)
    {
        return 0;
    }

    DWORD errors = 0;
    ok = clear_errors(h, &errors, &error) && errors == CE_FRAME;
    add_step(r, "ClearCommError reports the framing error", ok, error, 1, -1, "errors 0x%lx, CE_FRAME 0x%lx expected",
             (unsigned long)errors, (unsigned long)CE_FRAME);
    if (!ok)
    {
        return 0;
    }

    /* What was refused until now works, and the bytes that waited are there. */
    if (!set_timeouts(h, MAXDWORD, MAXDWORD, 1000, 0, 0, &error))
    {
        add_step(r, "SetCommTimeouts", 0, error, 1, -1, "SetCommTimeouts failed after ClearCommError");
        return 0;
    }
    unsigned char text[8];
    memset(text, 0, sizeof text);
    DWORD got = read_exact(h, text, 2, errors_wait_ms, &error);
    ok = got == 2 && memcmp(text, "ok", 2) == 0;
    add_step(r, "ReadFile after ClearCommError reads the bytes that waited", ok, error, 1, -1,
             "read %lu bytes, \"%.2s\"; \"ok\" expected", (unsigned long)got, (const char *)text);
    return ok;
}

static void put_errors_ready(const char *path, int ready)
{
    char content[256];
    (void)_snprintf_s(content, sizeof content, _TRUNCATE,
                      "{\"ready\":%s,\"pid\":%lu,\"arch\":\"" VSS_CLIENT_ARCH "\"}\n", ready ? "true" : "false",
                      (unsigned long)GetCurrentProcessId());
    write_ready(path, content);
}

static int run_errors(const char *port, const char *ready_path, text_buffer *json)
{
    report r;
    memset(&r, 0, sizeof r);
    char path[64];
    device_path(port, path, sizeof path);
    DWORD error = 0;

    HANDLE h = open_path(path, FILE_FLAG_OVERLAPPED, &error);
    add_step(&r, "CreateFile overlapped", h != INVALID_HANDLE_VALUE, error, 1, -1, "path %s", path);
    if (h == INVALID_HANDLE_VALUE)
    {
        put_errors_ready(ready_path, 0);
    }
    else
    {
        /* The ready file comes after the settings: the hub raises its errors by them. */
        int ready = errors_setup(&r, h);
        put_errors_ready(ready_path, ready);
        if (ready && errors_parity(&r, h) && errors_break(&r, h))
        {
            (void)errors_abort(&r, h);
        }
        BOOL ok = CloseHandle(h);
        add_step(&r, "CloseHandle", ok, ok ? 0 : GetLastError(), 1, -1, "closed");
    }

    text_put(json, "{");
    put_identity(json, "errors");
    text_put(json, ",\"port\":");
    text_string(json, port);
    text_fmt(json, ",\"summary\":{\"steps\":%d,\"failed\":%d,\"required_failed\":%d},\n  \"steps\":[\n    ", r.count,
             r.failed, r.required_failed);
    text_put(json, r.steps.data ? r.steps.data : "");
    text_put(json, "\n  ]}\n");
    free(r.steps.data);
    return r.required_failed == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------------------ */
/* The command line.                                                                      */
/* ------------------------------------------------------------------------------------ */

static const char *option(int argc, char **argv, const char *name, const char *fallback)
{
    for (int i = 2; i + 1 < argc; ++i)
    {
        if (strcmp(argv[i], name) == 0)
        {
            return argv[i + 1];
        }
    }
    return fallback;
}

static void usage(void)
{
    fputs("usage: vss_com_client probe [--first N] [--last N] [--out file]\n"
          "       vss_com_client open --port COMn [--out file]\n"
          "       vss_com_client exercise --port COMn [--out file]\n"
          "       vss_com_client hold --port COMn --ready file [--log file] [--scenario overlapped|syncread]\n"
          "       vss_com_client errors --port COMn --ready file [--out file]\n",
          stderr);
}

static void emit(const text_buffer *json, const char *out_path)
{
    if (json->data == NULL)
    {
        return;
    }
    fputs(json->data, stdout);
    fflush(stdout);
    if (out_path != NULL)
    {
        FILE *f = NULL;
        if (fopen_s(&f, out_path, "wb") == 0 && f != NULL)
        {
            fputs(json->data, f);
            fclose(f);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage();
        return 2;
    }
    const char *mode = argv[1];
    const char *out_path = option(argc, argv, "--out", NULL);
    const char *port = option(argc, argv, "--port", NULL);
    text_buffer json;
    memset(&json, 0, sizeof json);
    int code = 2;

    if (strcmp(mode, "probe") == 0)
    {
        int first = atoi(option(argc, argv, "--first", "1"));
        int last = atoi(option(argc, argv, "--last", "16"));
        if (first < 1 || last > 255 || first > last)
        {
            usage();
            return 2;
        }
        code = run_probe(first, last, &json);
    }
    else if (strcmp(mode, "open") == 0 && port != NULL)
    {
        code = run_open(port, &json);
    }
    else if (strcmp(mode, "exercise") == 0 && port != NULL)
    {
        code = run_exercise(port, &json);
    }
    else if (strcmp(mode, "hold") == 0 && port != NULL)
    {
        const char *ready = option(argc, argv, "--ready", NULL);
        if (ready == NULL)
        {
            usage();
            return 2;
        }
        g_log_path = option(argc, argv, "--log", NULL);
        return run_hold(port, ready, option(argc, argv, "--scenario", "overlapped"));
    }
    else if (strcmp(mode, "errors") == 0 && port != NULL)
    {
        const char *ready = option(argc, argv, "--ready", NULL);
        if (ready == NULL)
        {
            usage();
            return 2;
        }
        code = run_errors(port, ready, &json);
    }
    else
    {
        usage();
        return 2;
    }
    emit(&json, out_path);
    free(json.data);
    return code;
}
