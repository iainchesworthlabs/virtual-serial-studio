#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the PR gate's static checks locally, before a push (standard 11.2, "Before a push").

    python3 tools/ci/precheck.py                 # the commits since origin/develop
    python3 tools/ci/precheck.py --base <ref>    # against another base

It runs the shared checks of icl-common-infra (branch name, commit and file text, action pinning,
changelog fragment, preprocessor rule, REUSE, clang-format), then this repository's own static
jobs (changelog tool, design tokens, the unit tests of the tools), and prints the jobs the gate
will plan for the diff on a ready pull request and in the merge queue. Each check says SKIP and
why when this machine lacks a tool; a skip is not a failure. The checks read commits, so commit
first.

Open a pull request as a draft and mark it ready when this is clean: a draft runs the static
jobs only, and a ready pull request starts the build jobs (AGENTS.md).

The shared scripts come from, in order: the directory in ICL_COMMON_INFRA, a sibling checkout
../icl-common-infra, or .icl-infra (what CI uses).

Exit status is 1 when a check fails.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import plan_legs  # noqa: E402


def find_infra() -> Path | None:
    candidates = [os.environ.get("ICL_COMMON_INFRA", ""), str(ROOT.parent / "icl-common-infra"), str(ROOT / ".icl-infra")]
    for candidate in candidates:
        if candidate and (Path(candidate) / "scripts" / "precheck.py").is_file():
            return Path(candidate)
    return None


def git(*args: str) -> str:
    res = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=False)
    return res.stdout.strip() if res.returncode == 0 else ""


def default_base() -> str:
    for ref in ("origin/develop", "develop"):
        if git("rev-parse", "--verify", "--quiet", ref):
            return ref
    return "HEAD~1"


def run(name: str, cmd: list[str]) -> bool:
    start = time.monotonic()
    res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=False)
    took = time.monotonic() - start
    status = "PASS" if res.returncode == 0 else "FAIL"
    print(f"{status}  {name:<28} {took:5.1f}s")
    if res.returncode != 0:
        for line in (res.stdout + res.stderr).strip().splitlines()[-15:]:
            print(f"      {line}")
    return res.returncode == 0


def plan_for(base: str) -> None:
    """Print the jobs the gate will plan for the diff, on a pull request and in the queue."""
    infra = find_infra()
    changed = git("diff", "--name-only", f"{base}...HEAD").splitlines()
    if infra is None or not changed:
        print("\nPlan: skipped (no icl-common-infra checkout, or no commits to read)")
        return
    gate = subprocess.run(
        [sys.executable, str(infra / "scripts" / "plan_gate.py"), "--config", str(ROOT / ".github/ci/plan.json")],
        input="\n".join(changed), capture_output=True, text=True, check=False,
    )
    plan = plan_legs.parse_plan(gate.stdout)
    cfg = json.loads((ROOT / ".github/ci/plan.json").read_text(encoding="utf-8"))
    print("\nThe gate will plan:")
    for event, label in (("pull_request", "ready pull request"), ("merge_group", "merge-queue entry")):
        result = plan_legs.decide(event, False, plan, changed, cfg, ROOT)
        jobs = [m["name"] for m in result["matrix"]] + [j for j in plan_legs.JOBS[1:] if result["flags"][j]]
        print(f"  {label:<20} {', '.join(jobs) if jobs else 'static jobs only'}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base")
    args = ap.parse_args(argv)
    base = args.base or default_base()
    py = sys.executable
    ok = True

    infra = find_infra()
    if infra is None:
        print("SKIP  shared checks             no icl-common-infra checkout (set ICL_COMMON_INFRA)")
    else:
        shared = subprocess.run([py, str(infra / "scripts" / "precheck.py"), "--root", str(ROOT), "--base", base],
                                capture_output=True, text=True, check=False)
        # The shared script prints its own plan; ours below replaces it.
        lines = shared.stdout.splitlines()
        print("\n".join(line for line in lines if not line.startswith("The gate will plan")).rstrip())
        print(shared.stderr.rstrip(), end="")
        ok = ok and shared.returncode == 0

    ok = run("changelog tool", [py, "tools/changelog.py", "--check"]) and ok
    ok = run("driver version", [py, "tools/ci/check_driver_version.py", "--base", base]) and ok
    ok = run("design tokens", [py, "tools/check_contrast.py"]) and ok
    ok = run("tool unit tests", [py, "-m", "unittest", "discover", "-s", "tools", "-p", "test_*.py"]) and ok
    ok = run("planner unit tests", [py, "-m", "unittest", "discover", "-s", "tools/ci", "-p", "test_*.py"]) and ok
    plan_for(base)
    print("\nAll checks passed." if ok else "\nA check failed.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
