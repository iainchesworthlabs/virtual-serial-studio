#!/usr/bin/env python3
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Unit tests for tools/tracy_phases.py.

Run with:

    python -m unittest discover -s tools -p "test_*.py" -v
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import tracy_phases

HEADER = "name,src_file,src_line,ns_since_start,exec_time_ns,thread,value"

# One phase of 10 ms on thread 1 with one step. Inside it the window's update
# takes 6 ms, of which a child's paint takes 2 ms. A zone on another thread
# and a zone before the phase must not be counted in it.
CSV = "\n".join(
    [
        HEADER,
        "gui.event,app.cpp,83,100,1000000,1,QTimer / Timer",
        "bench.resize,bench.cpp,10,10000000,10000000,1,",
        "bench.step,bench.cpp,20,10000000,9000000,1,",
        "gui.event,app.cpp,83,11000000,6000000,1,Window / UpdateRequest / main",
        "gui.event,app.cpp,83,12000000,2000000,1,QLabel / Paint / a, b",
        "gui.monitor.paint_cell,panel.cpp,90,12100000,300000,1,preview",
        "gui.monitor.paint_cell,panel.cpp,90,12500000,100000,1,time",
        "hub.route,hub.cpp,427,12000000,500000,2,",
    ]
)


class tracy_phases_test(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.csv = Path(self._directory.name) / "trace.u.csv"
        self.csv.write_text(CSV + "\n", encoding="utf-8")

    def tearDown(self) -> None:
        self._directory.cleanup()

    def run_tool(self, *arguments: str) -> tuple[int, str]:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = tracy_phases.main([str(self.csv), *arguments])
        return code, output.getvalue()

    def test_self_time_leaves_out_the_zones_inside(self) -> None:
        rows = tracy_phases.load(self.csv)
        tracy_phases.assign_self_times(rows)
        update = next(row for row in rows if "UpdateRequest" in row.text)
        self.assertEqual(update.length_ns, 6000000)
        self.assertEqual(update.self_ns, 4000000)

    def test_text_with_a_comma_is_kept_whole(self) -> None:
        rows = tracy_phases.load(self.csv)
        self.assertIn("QLabel / Paint / a, b", [row.text for row in rows])

    def test_a_phase_holds_only_its_own_zones(self) -> None:
        code, text = self.run_tool("--phase", "bench.resize")
        self.assertEqual(code, 0)
        self.assertIn("bench.resize: 10.0 ms", text)
        self.assertIn("1 steps", text)
        self.assertIn("`event: Window / UpdateRequest / main` | 1 | 6.00 | 4.00", text)
        self.assertIn("hub.route", text)
        self.assertNotIn("QTimer", text)

    def test_the_monitor_cells_are_split_by_column(self) -> None:
        code, text = self.run_tool("--phase", "bench.resize")
        self.assertEqual(code, 0)
        self.assertIn("`gui.monitor.paint_cell: preview` | 1 | 0.30", text)
        self.assertIn("`gui.monitor.paint_cell: time` | 1 | 0.10", text)

    def test_gui_keeps_the_threads_that_deliver_events(self) -> None:
        code, text = self.run_tool("--gui")
        self.assertEqual(code, 0)
        self.assertNotIn("hub.route", text)

    def test_an_unknown_phase_fails(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            code, _ = self.run_tool("--phase", "bench.none")
        self.assertEqual(code, 1)

    def test_a_file_of_another_shape_is_refused(self) -> None:
        self.csv.write_text("name,total_ns\nhub.route,1\n", encoding="utf-8")
        with contextlib.redirect_stderr(io.StringIO()):
            code, _ = self.run_tool()
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
