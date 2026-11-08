#!/usr/bin/env python3
# Copyright (c) 2026 VirtualSerial contributors
# SPDX-License-Identifier: BSL-1.0
# Derived from the Boost.VirtualSerial repository tools.
"""Create a pair of connected pseudo terminals for serial loopback tests.

This replaces the usual ``socat pty,raw,echo=0 pty,raw,echo=0`` dance with a
single Python process, so the serial-mode CI jobs need no extra packages on
Linux or macOS.

The two slave device paths are printed on stdout, one per line:

    /dev/pts/3
    /dev/pts/4

Everything written to the first path appears on the second path and vice
versa.  The process keeps running until it is terminated.
"""

from __future__ import annotations

import os
import select
import signal
import threading
import time
import tty


def make_pair() -> tuple[int, int, str, str]:
    master_a, slave_a = os.openpty()
    master_b, slave_b = os.openpty()

    # Raw mode disables echo and CR/LF translation for both sides, so the
    # bytes the test writes are exactly the bytes the peer reads.
    tty.setraw(slave_a)
    tty.setraw(slave_b)

    name_a = os.ttyname(slave_a)
    name_b = os.ttyname(slave_b)
    return master_a, master_b, name_a, name_b


def relay(master_a: int, master_b: int, stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            readable, _, _ = select.select([master_a, master_b], [], [], 0.2)
        except (OSError, ValueError):
            return
        for fd in readable:
            try:
                data = os.read(fd, 8192)
            except OSError:
                return
            if not data:
                continue
            destination = master_b if fd == master_a else master_a
            try:
                os.write(destination, data)
            except OSError:
                return


def main() -> int:
    stop = threading.Event()

    def terminate(_signum, _frame):
        stop.set()

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)

    master_a, master_b, name_a, name_b = make_pair()
    worker = threading.Thread(
        target=relay, args=(master_a, master_b, stop), daemon=True
    )
    worker.start()

    print(name_a, flush=True)
    print(name_b, flush=True)

    try:
        while not stop.is_set():
            time.sleep(0.2)
    finally:
        stop.set()
        for fd in (master_a, master_b):
            try:
                os.close(fd)
            except OSError:
                # Best effort while exiting: a descriptor that cannot be closed has nothing left to release.
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
