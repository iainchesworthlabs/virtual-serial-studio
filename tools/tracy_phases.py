#!/usr/bin/env python3
#
# Copyright (c) 2026 Virtual Serial Studio contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Self times per phase from a Tracy capture exported with `tracy-csvexport -u`.

`tracy-csvexport -u` writes one row per zone instance:

    name,src_file,src_line,ns_since_start,exec_time_ns,thread,value

`tools/tracy_report.py` reads the aggregated export, which has one `gui.event`
row for every event the GUI delivered. This script reads the unwrapped export
instead, so it can

- split `gui.event` by its text (receiver class / event type / object name),
- give each zone its self time (its time less the zones inside it), which is
  where the time inside Qt shows up: the self time of the top-level window's
  `UpdateRequest` and `Expose` events is the compose and present of the window,
- report one table per phase, where a phase is the time range of a zone named
  with `--phases` (the render bench's `bench.repaint`, `bench.pan`,
  `bench.resize`, `bench.move`, `bench.idle` and `bench.traffic` by default).
  A capture without such zones is reported as one phase.

    tracy-csvexport -u trace.tracy > trace.u.csv
    python tools/tracy_phases.py trace.u.csv --gui --phase bench.resize
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

DEFAULT_PHASES = (
    "bench.repaint,bench.pan,bench.resize,bench.move,bench.idle,bench.traffic"
)


@dataclass
class instance:
    name: str
    start_ns: int
    length_ns: int
    thread: str
    text: str
    self_ns: int = 0

    @property
    def end_ns(self) -> int:
        return self.start_ns + self.length_ns

    @property
    def key(self) -> str:
        if self.name == "gui.event" and self.text:
            return "event: " + self.text
        return self.name


def load(path: Path) -> list[instance]:
    """Read the unwrapped export.

    The exporter quotes nothing, and a zone name or its text may hold a comma,
    so the numeric columns are found by their shape: a line number followed by
    a start, a length and a thread.
    """
    rows: list[instance] = []
    try:
        handle = path.open(encoding="utf-8", errors="replace")
    except OSError as error:
        raise ValueError(f"cannot open {path}: {error}") from error
    with handle:
        header = handle.readline()
        if "ns_since_start" not in header:
            raise ValueError(f"{path} is not a tracy-csvexport -u file")
        for line in handle:
            cells = line.rstrip("\r\n").split(",")
            for at in range(2, len(cells) - 3):
                if (
                    cells[at].isdigit()
                    and cells[at + 1].lstrip("-").isdigit()
                    and cells[at + 2].isdigit()
                    and cells[at + 3].isdigit()
                ):
                    rows.append(
                        instance(
                            name=cells[0],
                            start_ns=int(cells[at + 1]),
                            length_ns=int(cells[at + 2]),
                            thread=cells[at + 3],
                            text=",".join(cells[at + 4 :]),
                        )
                    )
                    break
    return rows


def assign_self_times(rows: list[instance]) -> None:
    by_thread: dict[str, list[instance]] = defaultdict(list)
    for row in rows:
        by_thread[row.thread].append(row)
    for items in by_thread.values():
        items.sort(key=lambda item: (item.start_ns, -item.length_ns))
        stack: list[instance] = []
        for row in items:
            row.self_ns = row.length_ns
            while stack and stack[-1].end_ns <= row.start_ns:
                stack.pop()
            if stack:
                stack[-1].self_ns -= row.length_ns
            stack.append(row)


def render(
    rows: list[instance],
    begin_ns: int,
    end_ns: int,
    title: str,
    top: int,
    match: list[str],
) -> str:
    inside = [row for row in rows if begin_ns <= row.start_ns < end_ns]
    lines = [f"### {title}: {(end_ns - begin_ns) / 1e6:.1f} ms", ""]
    steps = sorted(row.length_ns / 1e6 for row in inside if row.name == "bench.step")
    if steps:
        lines.append(
            f"{len(steps)} steps: mean {sum(steps) / len(steps):.2f} ms, "
            f"median {steps[len(steps) // 2]:.2f} ms, "
            f"95th percentile {steps[int(len(steps) * 0.95)]:.2f} ms, "
            f"slowest {steps[-1]:.2f} ms"
        )
        lines.append("")
    totals: dict[str, list[int]] = defaultdict(lambda: [0, 0, 0, 0])
    for row in inside:
        entry = totals[row.key]
        entry[0] += 1
        entry[1] += row.length_ns
        entry[2] += row.self_ns
        entry[3] = max(entry[3], row.length_ns)
    ordered = sorted(totals.items(), key=lambda item: item[1][2], reverse=True)
    if match:
        ordered = [item for item in ordered if any(m in item[0] for m in match)]
    lines.append("| Zone | Calls | Total (ms) | Self (ms) | Slowest (ms) |")
    lines.append("| --- | ---: | ---: | ---: | ---: |")
    for name, (calls, total, own, worst) in ordered[:top]:
        lines.append(
            f"| `{name}` | {calls} | {total / 1e6:.2f} | {own / 1e6:.2f} | "
            f"{worst / 1e6:.2f} |"
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("csv", type=Path, help="trace exported by tracy-csvexport -u")
    parser.add_argument(
        "--phases",
        default=DEFAULT_PHASES,
        help="comma separated zone names whose time ranges are the phases",
    )
    parser.add_argument("--phase", help="report this phase only")
    parser.add_argument(
        "--gui",
        action="store_true",
        help="only the threads that deliver events (the GUI thread)",
    )
    parser.add_argument(
        "--match",
        default="",
        help="comma separated text; only zones that contain one of them",
    )
    parser.add_argument("--top", type=int, default=25, help="rows in each table")
    arguments = parser.parse_args(argv)

    try:
        rows = load(arguments.csv)
    except ValueError as error:
        print(f"tracy_phases: {error}", file=sys.stderr)
        return 2
    if not rows:
        print(f"tracy_phases: {arguments.csv} has no zones", file=sys.stderr)
        return 2

    assign_self_times(rows)
    if arguments.gui:
        gui_threads = {row.thread for row in rows if row.name == "gui.event"}
        rows = [row for row in rows if row.thread in gui_threads]
    names = {name.strip() for name in arguments.phases.split(",") if name.strip()}
    phases = sorted(
        (row for row in rows if row.name in names), key=lambda row: row.start_ns
    )
    if arguments.phase:
        phases = [row for row in phases if row.name == arguments.phase]
        if not phases:
            print(f"tracy_phases: no phase named {arguments.phase}", file=sys.stderr)
            return 1
    match = [text.strip() for text in arguments.match.split(",") if text.strip()]
    top = max(1, arguments.top)
    tables = []
    if phases:
        for phase in phases:
            tables.append(
                render(rows, phase.start_ns, phase.end_ns, phase.name, top, match)
            )
    else:
        begin = min(row.start_ns for row in rows)
        end = max(row.end_ns for row in rows)
        tables.append(render(rows, begin, end, "whole trace", top, match))
    print("\n\n".join(tables))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
