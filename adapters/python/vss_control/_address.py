# SPDX-License-Identifier: GPL-3.0-or-later
"""Where the gateway's public endpoint is.  Only the address is worked out
here; whether to trust what answers there is decided in _peer.py.

This is libs/vss_agent/src/platform/{windows,posix}/local_stream_*.cpp for
`local_endpoint::control`, and it must give the same address:

  Windows  \\\\.\\pipe\\vss-control-<SID of this user>[-<instance>]
  Unix     $XDG_RUNTIME_DIR/virtual-serial-studio/control[-<instance>].sock
           or, when XDG_RUNTIME_DIR is empty or not set,
           <TMPDIR or /tmp>/virtual-serial-studio-<euid>/control[-<instance>].sock

An instance name is up to 32 of `A-Z a-z 0-9 _ -`.  It is the only part a
caller chooses, so an address cannot be made to point anywhere else.
"""

from __future__ import annotations

import os
import re
import sys

_INSTANCE = re.compile(r"[A-Za-z0-9_-]{0,32}")
_DIRECTORY = "virtual-serial-studio"
_ENDPOINT = "control"


def valid_instance_name(name):
    return isinstance(name, str) and _INSTANCE.fullmatch(name) is not None


def windows_pipe_name(sid_text, instance=""):
    name = "\\\\.\\pipe\\vss-%s-%s" % (_ENDPOINT, sid_text)
    if instance:
        name += "-" + instance
    return name


def unix_runtime_directory(environ=None, euid=None):
    """The directory that holds the sockets of this user."""
    if environ is None:
        environ = os.environ
    runtime = environ.get("XDG_RUNTIME_DIR", "")
    if runtime:
        return os.path.join(runtime, _DIRECTORY)
    base = environ.get("TMPDIR", "") or "/tmp"
    if euid is None:
        euid = os.geteuid()
    return os.path.join(base, "%s-%d" % (_DIRECTORY, euid))


def unix_socket_path(instance="", environ=None, euid=None):
    name = _ENDPOINT
    if instance:
        name += "-" + instance
    return os.path.join(unix_runtime_directory(environ, euid), name + ".sock")


def unix_path_limit():
    """The most bytes of a socket path: `sizeof(sockaddr_un::sun_path) - 1`,
    107 on Linux and 103 on macOS and the BSDs."""
    return 107 if sys.platform.startswith("linux") else 103
