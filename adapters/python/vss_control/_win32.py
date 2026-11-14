# SPDX-License-Identifier: GPL-3.0-or-later
"""The few Windows calls this package makes, through ctypes.

Every function has its argument and result types declared: without them
ctypes passes a 64-bit handle as a 32-bit integer.  Nothing here runs at
import time; `load()` builds the table on first use, on Windows only.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
SECURITY_SQOS_PRESENT = 0x00100000
SECURITY_IDENTIFICATION = 0x00010000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TOKEN_QUERY = 0x0008
TOKEN_USER_CLASS = 1  # TokenUser in TOKEN_INFORMATION_CLASS

ERROR_FILE_NOT_FOUND = 2
ERROR_ACCESS_DENIED = 5
ERROR_HANDLE_EOF = 38
ERROR_BROKEN_PIPE = 109
ERROR_NO_DATA = 232
ERROR_PIPE_NOT_CONNECTED = 233
ERROR_PIPE_BUSY = 231
ERROR_OPERATION_ABORTED = 995
ERROR_IO_PENDING = 997

WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258

INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
DWORD = wintypes.DWORD

# What a read says when the other end has gone: the end of the stream.
END_OF_STREAM = (
    ERROR_BROKEN_PIPE,
    ERROR_PIPE_NOT_CONNECTED,
    ERROR_NO_DATA,
    ERROR_HANDLE_EOF,
)


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_size_t),
        ("InternalHigh", ctypes.c_size_t),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", ctypes.c_void_p),
    ]


class UserSid:
    """The user of a token: the bytes of a TOKEN_USER, whose first field is a
    pointer to the SID inside the same bytes.  The buffer is kept for as long
    as the SID is used."""

    def __init__(self, buffer):
        self._buffer = buffer
        self.pointer = ctypes.c_void_p.from_buffer(buffer).value
        if not self.pointer:
            raise OSError("the token has no user")


class _Api:
    def __init__(self):
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
        handle = ctypes.c_void_p
        dword = wintypes.DWORD
        boolean = wintypes.BOOL
        pointer = ctypes.c_void_p
        overlapped = ctypes.POINTER(OVERLAPPED)

        def bind(library, name, result, *arguments):
            function = getattr(library, name)
            function.restype = result
            function.argtypes = list(arguments)
            setattr(self, name, function)

        bind(
            kernel32,
            "CreateFileW",
            handle,
            wintypes.LPCWSTR,
            dword,
            dword,
            pointer,
            dword,
            dword,
            handle,
        )
        bind(kernel32, "CreateEventW", handle, pointer, boolean, boolean, wintypes.LPCWSTR)
        bind(kernel32, "ResetEvent", boolean, handle)
        bind(kernel32, "CloseHandle", boolean, handle)
        bind(kernel32, "ReadFile", boolean, handle, pointer, dword, ctypes.POINTER(dword), overlapped)
        bind(kernel32, "WriteFile", boolean, handle, pointer, dword, ctypes.POINTER(dword), overlapped)
        bind(kernel32, "GetOverlappedResult", boolean, handle, overlapped, ctypes.POINTER(dword), boolean)
        bind(kernel32, "CancelIoEx", boolean, handle, overlapped)
        bind(kernel32, "WaitForSingleObject", dword, handle, dword)
        bind(kernel32, "WaitNamedPipeW", boolean, wintypes.LPCWSTR, dword)
        bind(kernel32, "GetNamedPipeServerProcessId", boolean, handle, ctypes.POINTER(wintypes.ULONG))
        bind(kernel32, "OpenProcess", handle, dword, boolean, dword)
        bind(kernel32, "GetCurrentProcess", handle)
        bind(kernel32, "LocalFree", pointer, pointer)
        bind(advapi32, "OpenProcessToken", boolean, handle, dword, ctypes.POINTER(handle))
        bind(advapi32, "GetTokenInformation", boolean, handle, ctypes.c_int, pointer, dword, ctypes.POINTER(dword))
        bind(advapi32, "EqualSid", boolean, pointer, pointer)
        bind(advapi32, "ConvertSidToStringSidW", boolean, pointer, ctypes.POINTER(pointer))


_api = None


def load():
    """The table of calls.  Raises OSError where there is no Windows API."""
    global _api
    if _api is None:
        try:
            _api = _Api()
        except (AttributeError, OSError) as error:
            raise OSError("the Windows API is not available: %s" % error) from None
    return _api


def last_error():
    return ctypes.get_last_error()


def win_error(code):
    return ctypes.WinError(code)


def process_user(process):
    """The UserSid of the token of `process`, a handle with
    PROCESS_QUERY_LIMITED_INFORMATION."""
    api = load()
    token = ctypes.c_void_p()
    if not api.OpenProcessToken(process, TOKEN_QUERY, ctypes.byref(token)):
        raise win_error(last_error())
    try:
        size = wintypes.DWORD(0)
        # The first call only says how large the answer is.
        api.GetTokenInformation(token, TOKEN_USER_CLASS, None, 0, ctypes.byref(size))
        if size.value == 0:
            raise win_error(last_error())
        buffer = ctypes.create_string_buffer(size.value)
        if not api.GetTokenInformation(
            token, TOKEN_USER_CLASS, buffer, size.value, ctypes.byref(size)
        ):
            raise win_error(last_error())
        return UserSid(buffer)
    finally:
        api.CloseHandle(token)


def pipe_server_user(pipe):
    """The UserSid of the process that serves the pipe handle `pipe`, which
    this process opened."""
    api = load()
    process_id = wintypes.ULONG(0)
    if not api.GetNamedPipeServerProcessId(pipe, ctypes.byref(process_id)):
        raise win_error(last_error())
    process = api.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, process_id.value)
    if not process:
        raise win_error(last_error())
    try:
        return process_user(process)
    finally:
        api.CloseHandle(process)


def own_user():
    """The UserSid of the user who runs this process."""
    return process_user(load().GetCurrentProcess())


def sid_text(user):
    """A UserSid as text (`S-1-5-21-...`)."""
    api = load()
    text = ctypes.c_void_p()
    if not api.ConvertSidToStringSidW(user.pointer, ctypes.byref(text)):
        raise win_error(last_error())
    try:
        return ctypes.wstring_at(text.value)
    finally:
        api.LocalFree(text)


def sids_equal(first, second):
    return bool(load().EqualSid(first.pointer, second.pointer))
