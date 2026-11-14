# SPDX-License-Identifier: GPL-3.0-or-later
"""The checks of who serves the endpoint (vss_control/_peer.py).  They are
tested with stand-ins for the operating system's answers, so every branch
runs on every platform, and every failure must be a refusal."""

import errno
import os
import socket
import stat
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))

from vss_control import _peer  # noqa: E402
from vss_control._errors import TransportError  # noqa: E402

OWN = 1000


def info(mode, uid=OWN):
    return os.stat_result((mode, 1, 1, 1, uid, 0, 0, 0, 0, 0))


def failing_lstat(code):
    def lstat(_path):
        raise OSError(code, os.strerror(code))

    return lstat


class AsUser(unittest.TestCase):
    """Runs the test as the effective user OWN."""

    def setUp(self):
        patch = mock.patch.object(os, "geteuid", create=True, return_value=OWN)
        patch.start()
        self.addCleanup(patch.stop)

    def refused(self, code, function, *arguments):
        with self.assertRaises(TransportError) as caught:
            function(*arguments)
        self.assertEqual(caught.exception.code, code)


class DirectoryTests(AsUser):
    def check(self, result):
        with mock.patch.object(os, "lstat", result if callable(result) else lambda _p: result):
            _peer.check_unix_directory("/run/x")

    def test_a_private_directory_of_this_user_passes(self):
        self.check(info(stat.S_IFDIR | 0o700))

    def test_not_a_directory_is_refused(self):
        for kind in (stat.S_IFLNK, stat.S_IFREG, stat.S_IFSOCK):
            with self.subTest(kind=kind):
                self.refused("not_same_user", self.check, info(kind | 0o700))

    def test_another_owner_is_refused(self):
        self.refused("not_same_user", self.check, info(stat.S_IFDIR | 0o700, uid=0))

    def test_access_for_group_or_others_is_refused(self):
        for mode in (0o750, 0o770, 0o705, 0o701, 0o710, 0o755, 0o777):
            with self.subTest(mode=oct(mode)):
                self.refused("not_same_user", self.check, info(stat.S_IFDIR | mode))

    def test_a_missing_directory_means_nothing_runs(self):
        self.refused("not_running", self.check, failing_lstat(errno.ENOENT))

    def test_any_other_failure_is_a_failed_check(self):
        self.refused("peer_check_failed", self.check, failing_lstat(errno.EACCES))


class SocketFileTests(AsUser):
    def check(self, result):
        with mock.patch.object(os, "lstat", result if callable(result) else lambda _p: result):
            _peer.check_unix_socket_file("/run/x/control.sock")

    def test_a_socket_of_this_user_passes(self):
        self.check(info(stat.S_IFSOCK | 0o600))

    def test_other_files_and_other_owners_are_refused(self):
        self.refused("not_same_user", self.check, info(stat.S_IFREG | 0o600))
        self.refused("not_same_user", self.check, info(stat.S_IFLNK | 0o777))
        self.refused("not_same_user", self.check, info(stat.S_IFSOCK | 0o600, uid=0))

    def test_missing_and_unreadable(self):
        self.refused("not_running", self.check, failing_lstat(errno.ENOENT))
        self.refused("peer_check_failed", self.check, failing_lstat(errno.EACCES))


class FakeSocket:
    def __init__(self, raw=None, error=None):
        self.raw = raw
        self.error = error

    def getsockopt(self, _level, _option, _size):
        if self.error is not None:
            raise self.error
        return self.raw

    def fileno(self):
        return 3


class UnixPeerTests(AsUser):
    def linux(self):
        platform = mock.patch.object(sys, "platform", "linux")
        option = mock.patch.object(socket, "SO_PEERCRED", 17, create=True)
        platform.start()
        option.start()
        self.addCleanup(platform.stop)
        self.addCleanup(option.stop)

    def credentials(self, uid):
        return FakeSocket(raw=struct.pack("iII", 4242, uid, OWN))

    def test_linux_same_user_passes(self):
        self.linux()
        _peer.check_unix_peer(self.credentials(OWN))

    def test_linux_another_user_is_refused(self):
        self.linux()
        for uid in (0, OWN + 1, 4294967294):
            with self.subTest(uid=uid):
                self.refused("not_same_user", _peer.check_unix_peer, self.credentials(uid))

    def test_linux_a_failed_or_odd_answer_is_a_failed_check(self):
        self.linux()
        self.refused("peer_check_failed", _peer.check_unix_peer, FakeSocket(error=OSError(errno.EBADF, "bad")))
        self.refused("peer_check_failed", _peer.check_unix_peer, FakeSocket(raw=b"\x00\x01"))
        self.refused("peer_check_failed", _peer.check_unix_peer, FakeSocket(error=RuntimeError("odd")))

    def test_macos_goes_through_getpeereid(self):
        with mock.patch.object(sys, "platform", "darwin"):
            with mock.patch.object(_peer, "_darwin_peer_uid", return_value=OWN):
                _peer.check_unix_peer(FakeSocket())
            with mock.patch.object(_peer, "_darwin_peer_uid", return_value=0):
                self.refused("not_same_user", _peer.check_unix_peer, FakeSocket())
            with mock.patch.object(_peer, "_darwin_peer_uid", side_effect=AttributeError("no getpeereid")):
                self.refused("peer_check_failed", _peer.check_unix_peer, FakeSocket())

    def test_a_system_that_cannot_say_fails_closed(self):
        for platform in ("freebsd14", "sunos5", "aix", "cygwin"):
            with self.subTest(platform=platform):
                with mock.patch.object(sys, "platform", platform):
                    self.refused("peer_check_failed", _peer.check_unix_peer, FakeSocket(raw=struct.pack("iII", 1, OWN, OWN)))


@unittest.skipUnless(os.name == "nt", "the named pipe check is Windows only")
class PipeServerTests(unittest.TestCase):
    def test_a_handle_that_is_no_pipe_is_a_failed_check(self):
        from vss_control import _win32

        # The pseudo handle of this process is a handle, and not a pipe.
        self.refused_handle(_win32.load().GetCurrentProcess())

    def refused_handle(self, handle, code="peer_check_failed"):
        with self.assertRaises(TransportError) as caught:
            _peer.check_pipe_server(handle)
        self.assertEqual(caught.exception.code, code)

    def test_the_comparison_decides(self):
        from vss_control import _win32

        user = _win32.own_user()
        with mock.patch.object(_win32, "pipe_server_user", return_value=user):
            _peer.check_pipe_server(0)
            with mock.patch.object(_peer, "_same_user", return_value=False):
                self.refused_handle(0, "not_same_user")
            with mock.patch.object(_peer, "_same_user", side_effect=OSError("odd")):
                self.refused_handle(0)
        with mock.patch.object(_win32, "pipe_server_user", side_effect=OSError("denied")):
            self.refused_handle(0)

    def test_own_sid_text_has_the_form_of_a_sid(self):
        text = _peer.own_sid_text()
        self.assertRegex(text, r"^S-1-\d+(-\d+)+$")


if __name__ == "__main__":
    unittest.main()
