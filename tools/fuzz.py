#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the libFuzzer targets of the linux-fuzz build.

    python3 tools/fuzz.py --mode regress
    python3 tools/fuzz.py --mode short
    python3 tools/fuzz.py --mode nightly

regress replays the seed corpus and the regression inputs once (the same check
the fuzz_* CTest tests make, now under libFuzzer's AddressSanitizer). short and
nightly fuzz each target for a fixed time. A target's working corpus lives in
--work-dir, which CI keeps between runs, and its crash files go to
<work-dir>/artifacts. The exit status is 1 when any target fails.

--budget-minutes N says how long the whole run may fuzz. The time for each target
is then the smaller of the mode's time and N minutes shared by the targets, so a
new target shortens the others and never lengthens the run. When the share falls
under MIN_SECONDS the script stops with status 2 and says so, which is the signal
to raise the limit of the job or to split the targets across jobs. fuzz.yml passes
its budget and tools/test_fuzz.py checks that the job limit holds it.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Seconds of fuzzing per target (standard section 6.6).
SECONDS = {"short": 60, "nightly": 600}

# The least a target may be fuzzed for when --budget-minutes shares the time between
# the targets. Under this the run is refused, not quietly thinned out.
MIN_SECONDS = {"short": 30, "nightly": 300}

# libFuzzer stops itself at -max_total_time. This is what a target may take beyond
# that (start-up, loading the corpus, writing a crash) before the script kills it and
# counts it as failed, so a hung target cannot run the job out of time unreported.
GRACE_SECONDS = 180

# A replay of the seeds and the regressions takes seconds; this is a hang, not work.
REGRESS_TIMEOUT_SECONDS = 600


def harnesses(root: Path = ROOT) -> list[Path]:
    """Every harness source: libs/<lib>/fuzz/fuzz_*.cpp."""
    return sorted(root.glob("libs/*/fuzz/fuzz_*.cpp"))


def seconds_per_target(mode: str, targets: int, budget_minutes: int | None) -> int:
    """Seconds to fuzz each target: the mode's time, cut to fit the budget when there is one.

    Raises ValueError when the budget leaves a target less than MIN_SECONDS.
    """
    wanted = SECONDS.get(mode, 0)
    if budget_minutes is None or mode not in SECONDS or targets < 1:
        return wanted
    share = budget_minutes * 60 // targets
    if share < MIN_SECONDS[mode]:
        raise ValueError(
            f"{targets} targets in {budget_minutes} minutes leave {share} s for each, under the "
            f"{MIN_SECONDS[mode]} s that {mode} fuzzing needs; raise the limit of the job or split the targets"
        )
    return min(wanted, share)


def command(
    harness: Path, build_dir: Path, mode: str, work_dir: Path, seconds: int
) -> list[str]:
    """The libFuzzer command line for one harness."""
    seeds = harness.parent / "corpus"
    regressions = harness.parent / "regressions"
    binary = str(build_dir / "bin" / harness.stem)
    if mode == "regress":
        return [binary, "-runs=0", str(seeds), str(regressions)]
    return [
        binary,
        f"-max_total_time={seconds}",
        "-timeout=10",
        "-rss_limit_mb=2048",
        f"-artifact_prefix={work_dir / 'artifacts'}/",
        str(work_dir / harness.stem),
        str(seeds),
        str(regressions),
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--mode", choices=["regress", "short", "nightly"], required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "linux-fuzz")
    parser.add_argument("--work-dir", type=Path, default=ROOT / ".cache" / "fuzz")
    parser.add_argument("--seconds", type=int, help="override the time per target")
    parser.add_argument(
        "--budget-minutes",
        type=int,
        help="the most the whole run may fuzz; the time for each target is cut to fit",
    )
    args = parser.parse_args(argv)

    sources = harnesses()
    if not sources:
        print("no fuzz harness found", file=sys.stderr)
        return 1
    try:
        seconds = args.seconds or seconds_per_target(args.mode, len(sources), args.budget_minutes)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    if args.mode != "regress":
        total = seconds * len(sources) / 60
        print(f"{len(sources)} targets, {seconds} s each, {total:.0f} minutes of fuzzing", flush=True)

    (args.work_dir / "artifacts").mkdir(parents=True, exist_ok=True)
    failed = []
    for harness in sources:
        (args.work_dir / harness.stem).mkdir(parents=True, exist_ok=True)
        line = command(harness, args.build_dir, args.mode, args.work_dir, seconds)
        print(f"::group::{harness.stem}", flush=True)
        print(" ".join(line), flush=True)
        limit = REGRESS_TIMEOUT_SECONDS if args.mode == "regress" else seconds + GRACE_SECONDS
        try:
            status = subprocess.run(line, check=False, timeout=limit).returncode
        except subprocess.TimeoutExpired:
            print(f"{harness.stem}: still running after {limit} s, killed", file=sys.stderr)
            status = -1
        print("::endgroup::", flush=True)
        if status != 0:
            print(f"{harness.stem}: exit status {status}", file=sys.stderr)
            failed.append(harness.stem)
    if failed:
        print("failed: " + ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
