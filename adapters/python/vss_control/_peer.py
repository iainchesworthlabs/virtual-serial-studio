# SPDX-License-Identifier: GPL-3.0-or-later
"""Who serves the endpoint?  This is checked before a single byte is sent.

SECURITY.  The name of the endpoint can be guessed, so a program of another
user could have made it first and be waiting for a token or for commands.
The C++ client refuses such a server and so does this one:

  Windows  (check_pipe_server) the pipe was opened with SECURITY_IDENTIFICATION,
           so the server learns who this is and cannot act as this user.
           Windows is then asked which process serves the pipe
           (GetNamedPipeServerProcessId); the user SID in that process's
           token must be the user SID in this process's token (EqualSid).
  Linux    (check_unix_*) the directory that holds the socket must be a real
           directory of this user that nobody else can enter; the socket file
           must be a socket of this user; and after connect the kernel's
           record of the serving process (SO_PEERCRED) must say this user.
  macOS    the same, with getpeereid(2) in place of SO_PEERCRED.
  others   the check cannot be made, so the connection FAILS.

Every check FAILS CLOSED: an answer that cannot be had, a call that fails or
an exception is a refusal, never a pass.  The functions return nothing when
the server is this user's, and raise TransportError otherwise:

    not_same_user      the server was found and is someone else's
    peer_check_failed  the check could not be made
    not_running        nothing is there (not a security matter)

What is compared is the user the kernel knows, never a name or a path that
the server could write itself.  The user id of a process is the one the
process had when it made the socket or opened the pipe's server end; as in
the C++ client, a process that later hands the endpoint to another one is not
followed.
"""

from __future__ import annotations

import errno
import os
import stat
import struct
import sys

from ._errors import TransportError


def _closed(code, message):
    return TransportError(code, message)


# -- Unix --------------------------------------------------------------------


def check_unix_directory(directory):
    """The directory of the sockets must be a directory (not a link) of this
    user that has no access for group or others, as the server makes it and
    as `check_private_directory` in local_stream_posix.cpp demands.  Anything
    else may have been prepared by someone else."""
    try:
        info = os.lstat(directory)
    except OSError as error:
        if error.errno == errno.ENOENT:
            raise _closed("not_running", "no control endpoint for this user") from None
        raise _closed("peer_check_failed", "cannot look at the socket directory") from None
    if not stat.S_ISDIR(info.st_mode):
        raise _closed("not_same_user", "the socket directory is not a directory")
    if info.st_uid != os.geteuid():
        raise _closed("not_same_user", "the socket directory belongs to another user")
    if info.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise _closed("not_same_user", "the socket directory is open to other users")


def check_unix_socket_file(path):
    """The file must be a socket (not a link to one) that this user owns.
    The C++ client does not look at the file; the check after connect is the
    one that counts, and this one only refuses early."""
    try:
        info = os.lstat(path)
    except OSError as error:
        if error.errno == errno.ENOENT:
            raise _closed("not_running", "no control endpoint for this user") from None
        raise _closed("peer_check_failed", "cannot look at the socket file") from None
    if not stat.S_ISSOCK(info.st_mode):
        raise _closed("not_same_user", "the endpoint is not a socket")
    if info.st_uid != os.geteuid():
        raise _closed("not_same_user", "the socket belongs to another user")


def _linux_peer_uid(sock):
    import socket

    # struct ucred { pid_t pid; uid_t uid; gid_t gid; }
    size = struct.calcsize("iII")
    raw = sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, size)
    _pid, uid, _gid = struct.unpack("iII", raw)
    return uid


def _darwin_peer_uid(sock):
    import ctypes

    library = ctypes.CDLL(None, use_errno=True)
    function = library.getpeereid  # AttributeError when it is missing
    function.restype = ctypes.c_int
    function.argtypes = [
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_uint32),
    ]
    uid = ctypes.c_uint32()
    gid = ctypes.c_uint32()
    if function(sock.fileno(), ctypes.byref(uid), ctypes.byref(gid)) != 0:
        raise OSError(ctypes.get_errno(), "getpeereid failed")
    return uid.value


def unix_peer_uid(sock):
    """The effective user id of the process at the other end of the connected
    Unix socket `sock`.  Raises when the system cannot say."""
    if sys.platform.startswith("linux"):
        return _linux_peer_uid(sock)
    if sys.platform == "darwin":
        return _darwin_peer_uid(sock)
    raise OSError("no way to ask who is at the other end of a socket here")


def check_unix_peer(sock):
    """The process at the other end of the connected socket runs as this
    user.  Called after connect and before anything is sent."""
    try:
        uid = unix_peer_uid(sock)
        ours = os.geteuid()
    except Exception:
        # Not only OSError: a missing function, a bad answer, anything.
        raise _closed(
            "peer_check_failed",
            "cannot tell which user serves the control endpoint",
        ) from None
    if uid != ours:
        raise _closed("not_same_user", "the control endpoint is served by another user")


# -- Windows -----------------------------------------------------------------


def own_sid_text():
    """The SID of the user who runs this process, as text.  It is part of the
    pipe's name."""
    try:
        from . import _win32

        return _win32.sid_text(_win32.own_user())
    except Exception:
        raise _closed("peer_check_failed", "cannot tell which user this is") from None


def _same_user(first, second):
    # One place for the comparison, so that a test can make it say no.
    from . import _win32

    return _win32.sids_equal(first, second)


def check_pipe_server(handle):
    """The process that serves the pipe `handle` (opened by this process)
    runs as this user.  Called after the pipe was opened and before anything
    is written to it."""
    try:
        from . import _win32

        theirs = _win32.pipe_server_user(handle)
        ours = _win32.own_user()
        same = _same_user(theirs, ours)
    except Exception:
        raise _closed(
            "peer_check_failed",
            "cannot tell which user serves the control endpoint",
        ) from None
    if not same:
        raise _closed("not_same_user", "the control endpoint is served by another user")
