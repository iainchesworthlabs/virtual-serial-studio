# SPDX-License-Identifier: GPL-3.0-or-later
"""The connection to the gateway: a Unix domain socket or a named pipe, with
real timeouts on both.

A transport has three calls and no thread:

    send_all(data, deadline)   writes all of it, or raises TimeoutError
    recv_some(size, deadline)  up to `size` bytes; b"" when the other end is
                               gone; raises TimeoutError at the deadline
    close()

`deadline` is a time.monotonic() value.  A call that raises TimeoutError may
have written part of `data`, so the caller closes the connection.  A
recv_some that raises TimeoutError has read nothing: what it would have read
is still there for the next call.  Any other failure is an OSError.

Unix.  The socket's own timeout is set before each call.

Windows.  A named pipe opened as `open(path, "r+b", buffering=0)` blocks
until the other end acts and has no timeout.  So the pipe is opened with
FILE_FLAG_OVERLAPPED and every read and write is an overlapped call that is
waited for with a time limit; at the limit the call is cancelled
(CancelIoEx) and its end is waited for, so nothing is left running that
points at memory this module is about to free.  No thread is involved.
Overlapped I/O keeps the timeout inside the call, where a reader thread would
need a queue, a way to stop a read that cannot be interrupted, and a way to
tell its error from the caller's.
"""

from __future__ import annotations

import ctypes
import math
import os
import socket
import time

from . import _address, _peer
from ._errors import TransportError

_NO_WAIT = 0.0


def _remaining(deadline):
    return deadline - time.monotonic()


# -- Unix --------------------------------------------------------------------


class SocketTransport:
    """A connected stream socket.  Used for the Unix domain socket; it asks
    nothing of the socket but send, recv and a timeout."""

    def __init__(self, sock):
        self._sock = sock

    def _arm(self, deadline):
        # A timeout of 0 makes the socket non-blocking: the call is tried
        # once and says at once that it would block.
        self._sock.settimeout(max(_remaining(deadline), _NO_WAIT))

    def send_all(self, data, deadline):
        flags = getattr(socket, "MSG_NOSIGNAL", 0)
        view = memoryview(data)
        while len(view):
            self._arm(deadline)
            try:
                sent = self._sock.send(view, flags)
            except (socket.timeout, BlockingIOError):
                raise TimeoutError("the write took too long") from None
            view = view[sent:]

    def recv_some(self, size, deadline):
        self._arm(deadline)
        try:
            return self._sock.recv(size)
        except (socket.timeout, BlockingIOError):
            raise TimeoutError("nothing came in time") from None
        except ConnectionResetError:
            return b""

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass


def _open_socket(instance, timeout):
    path = _address.unix_socket_path(instance)
    if len(os.fsencode(path)) > _address.unix_path_limit():
        raise TransportError(
            "address_too_long",
            "the socket path is longer than this system allows; use a shorter "
            "instance name or a shorter TMPDIR",
        )
    _peer.check_unix_directory(os.path.dirname(path))
    _peer.check_unix_socket_file(path)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        try:
            sock.connect(path)
        except (FileNotFoundError, ConnectionRefusedError):
            raise TransportError("not_running", "no control endpoint for this user") from None
        except socket.timeout:
            raise TransportError("client_timeout", "connecting took too long") from None
        except OSError as error:
            raise TransportError("io_error", "cannot connect: %s" % error.strerror) from None
        # Nothing is sent before this passes.
        _peer.check_unix_peer(sock)
    except BaseException:
        sock.close()
        raise
    return SocketTransport(sock)


# -- Windows -----------------------------------------------------------------


def _wait_milliseconds(deadline):
    # At least 0, at most INFINITE - 1: never an unlimited wait.
    seconds = max(_remaining(deadline), 0.0)
    return min(int(math.ceil(seconds * 1000.0)), 0xFFFFFFFE)


class PipeTransport:
    """A named pipe handle opened with FILE_FLAG_OVERLAPPED."""

    def __init__(self, handle):
        from . import _win32

        self._w = _win32
        self._api = _win32.load()
        self._handle = handle
        self._event = None
        event = self._api.CreateEventW(None, True, False, None)
        if not event:
            # The caller still owns `handle` and closes it.
            self._handle = None
            raise _win32.win_error(_win32.last_error())
        self._event = event

    def _io(self, function, pointer, size, deadline):
        """One overlapped call, waited for until the deadline.  Returns the
        number of bytes moved.  Raises TimeoutError, or OSError (with the
        Windows error in `winerror`)."""
        w = self._w
        api = self._api
        # Both stay alive until the operation has ended, one way or another:
        # this function does not return before that.
        overlapped = w.OVERLAPPED()
        overlapped.hEvent = self._event
        moved = w.DWORD(0)
        api.ResetEvent(self._event)
        done = function(
            self._handle, pointer, size, ctypes.byref(moved), ctypes.byref(overlapped)
        )
        if done:
            return moved.value
        failure = w.last_error()
        if failure != w.ERROR_IO_PENDING:
            raise w.win_error(failure)

        finished = False
        timed_out = False
        success = False
        try:
            waited = api.WaitForSingleObject(self._event, _wait_milliseconds(deadline))
            if waited == w.WAIT_OBJECT_0:
                success = bool(
                    api.GetOverlappedResult(
                        self._handle,
                        ctypes.byref(overlapped),
                        ctypes.byref(moved),
                        False,
                    )
                )
                failure = 0 if success else w.last_error()
                finished = True
            elif waited == w.WAIT_TIMEOUT:
                timed_out = True
            else:
                raise w.win_error(w.last_error())
        finally:
            if not finished:
                # Time is up (or something went wrong): stop the call and wait
                # until it has ended.  It may have finished meanwhile, in which
                # case the result below is its real one.
                api.CancelIoEx(self._handle, ctypes.byref(overlapped))
                success = bool(
                    api.GetOverlappedResult(
                        self._handle,
                        ctypes.byref(overlapped),
                        ctypes.byref(moved),
                        True,
                    )
                )
                failure = 0 if success else w.last_error()
        if success or (timed_out and moved.value > 0):
            return moved.value
        if timed_out and failure == w.ERROR_OPERATION_ABORTED:
            raise TimeoutError("the call took too long")
        raise w.win_error(failure)

    def send_all(self, data, deadline):
        w = self._w
        buffer = ctypes.create_string_buffer(bytes(data), len(data))
        offset = 0
        while offset < len(data):
            offset += self._io(
                self._api.WriteFile,
                ctypes.byref(buffer, offset),
                len(data) - offset,
                deadline,
            )

    def recv_some(self, size, deadline):
        w = self._w
        buffer = ctypes.create_string_buffer(size)
        try:
            count = self._io(self._api.ReadFile, buffer, size, deadline)
        except OSError as error:
            if getattr(error, "winerror", None) in w.END_OF_STREAM:
                return b""
            raise
        return buffer.raw[:count]

    def close(self):
        handle, self._handle = self._handle, None
        event, self._event = self._event, None
        if handle:
            self._api.CloseHandle(handle)
        if event:
            self._api.CloseHandle(event)

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


def open_pipe_handle(name, deadline):
    """Opens the client end of the pipe `name` for overlapped I/O, at
    identification level, as the C++ client does.  Returns a handle.  Raises
    TransportError."""
    from . import _win32 as w

    api = w.load()
    access = w.GENERIC_READ | w.GENERIC_WRITE
    # SECURITY_IDENTIFICATION: whatever serves the pipe may learn who this is
    # and may not act as this user.
    flags = w.FILE_FLAG_OVERLAPPED | w.SECURITY_SQOS_PRESENT | w.SECURITY_IDENTIFICATION
    for _attempt in range(5):
        handle = api.CreateFileW(name, access, 0, None, w.OPEN_EXISTING, flags, None)
        if handle and handle != w.INVALID_HANDLE_VALUE:
            return handle
        failure = w.last_error()
        if failure == w.ERROR_FILE_NOT_FOUND:
            raise TransportError("not_running", "no control endpoint for this user")
        if failure == w.ERROR_ACCESS_DENIED:
            raise TransportError("not_same_user", "the control endpoint belongs to another user")
        if failure != w.ERROR_PIPE_BUSY:
            raise TransportError("io_error", "cannot open the pipe: %s" % w.win_error(failure))
        # Every instance is taken for a moment; the server makes another.
        api.WaitNamedPipeW(name, min(1000, max(_wait_milliseconds(deadline), 1)))
    raise TransportError("busy", "the control endpoint is busy", retryable=True)


def _open_pipe(instance, timeout):
    from . import _win32 as w

    name = _address.windows_pipe_name(_peer.own_sid_text(), instance)
    handle = open_pipe_handle(name, time.monotonic() + timeout)
    try:
        # Nothing is sent before this passes.
        _peer.check_pipe_server(handle)
        return PipeTransport(handle)
    except BaseException:
        w.load().CloseHandle(handle)
        raise


def open_transport(instance, timeout):
    """Connects to this user's gateway and checks who serves the endpoint."""
    if os.name == "nt":
        return _open_pipe(instance, timeout)
    return _open_socket(instance, timeout)
