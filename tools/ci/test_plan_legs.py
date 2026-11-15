#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for plan_legs.py.

    python3 -m unittest discover -s tools/ci -p "test_*.py" -v

They read the real .github/ci/plan.json, so a change to the rules there is tested too.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import plan_legs  # noqa: E402

CFG = json.loads((ROOT / ".github" / "ci" / "plan.json").read_text(encoding="utf-8"))


def plan_for(**overrides: str) -> dict[str, str]:
    base = {"build": "true", "gui": "false", "machinery": "false", "docs_only": "false", "gui_reason": ""}
    base.update(overrides)
    return base


def running(result: dict) -> set[str]:
    return {job for job, on in result["flags"].items() if on}


def names(result: dict) -> list[str]:
    return [leg["name"] for leg in result["matrix"]]


class TierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write(self, path: str, text: str) -> str:
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text, encoding="utf-8")
        return path

    def decide(self, event: str, changed: list[str], draft: bool = False, **plan: str) -> dict:
        return plan_legs.decide(event, draft, plan_for(**plan), changed, CFG, self.root)

    def test_draft_runs_no_build_leg(self) -> None:
        result = self.decide("pull_request", ["libs/vss_core/src/hub.cpp"], draft=True)
        self.assertEqual(result["tier"], "draft")
        self.assertEqual(running(result), set())

    def test_pull_request_is_the_cheap_gate(self) -> None:
        path = self.write("libs/vss_config/src/parse.cpp", "int parse();\n")
        result = self.decide("pull_request", [path])
        self.assertEqual(names(result), ["linux-gcc"])
        self.assertEqual(running(result), {"build", "asan-ubsan", "fuzz-regress"})

    def test_pull_request_adds_tsan_when_threads_are_used(self) -> None:
        path = self.write("libs/vss_core/src/hub.cpp", "boost::asio::strand<int> s;\n")
        result = self.decide("pull_request", [path])
        self.assertIn("tsan", running(result))
        self.assertEqual(result["why"]["tsan"], path)

    def test_pull_request_adds_tsan_for_a_removed_source_file(self) -> None:
        result = self.decide("pull_request", ["libs/vss_core/src/gone.cpp"])
        self.assertIn("tsan", running(result))

    def test_cmake_change_counts_as_threading_code(self) -> None:
        result = self.decide("pull_request", ["CMakePresets.json"])
        self.assertIn("tsan", running(result))

    def test_pull_request_never_runs_the_slow_legs(self) -> None:
        result = self.decide("pull_request", ["apps/vss_gui/src/main.cpp"], gui="true")
        self.assertTrue(running(result).isdisjoint({"macos", "gui", "coverage", "performance", "profiling", "driver"}))
        self.assertNotIn("windows-msvc", names(result))

    def test_docs_only_change_builds_nothing(self) -> None:
        for event in ("pull_request", "merge_group"):
            result = self.decide(event, ["docs/architecture.md"], build="false", docs_only="true")
            self.assertEqual(running(result), set(), event)
            self.assertEqual(result["matrix"], [])

    def test_queue_adds_windows_tsan_and_the_gui(self) -> None:
        path = self.write("libs/vss_config/src/parse.cpp", "int parse();\n")
        result = self.decide("merge_group", [path], gui="true", gui_reason=path)
        self.assertEqual(names(result), ["linux-gcc", "windows-msvc"])
        self.assertEqual(running(result), {"build", "asan-ubsan", "fuzz-regress", "tsan", "gui", "performance"})

    def test_queue_leaves_out_what_the_change_cannot_affect(self) -> None:
        path = self.write("tests/unit/test_parse.cpp", "int parse();\n")
        result = self.decide("merge_group", [path])
        self.assertEqual(running(result), {"build", "asan-ubsan", "fuzz-regress", "tsan"})

    def test_queue_flags_macos_for_platform_code_and_threads(self) -> None:
        platform = self.decide("merge_group", ["libs/vss_ports/src/platform/macos/peer_open_macos.cpp"])
        self.assertIn("macos", running(platform))
        path = self.write("libs/vss_core/include/vss/core/hub.hpp", "std::mutex m;\n")
        threads = self.decide("merge_group", [path])
        self.assertIn("macos", running(threads))
        self.assertEqual(threads["why"]["macos"], path)
        plain = self.write("libs/vss_config/src/parse.cpp", "int parse();\n")
        self.assertNotIn("macos", running(self.decide("merge_group", [plain])))

    def test_queue_runs_the_driver_build_only_for_driver_changes(self) -> None:
        driver = self.decide("merge_group", ["drivers/windows/vsscom/driver.c"], build="false")
        self.assertEqual(running(driver), {"driver"})
        other = self.decide("merge_group", ["libs/vss_config/src/parse.cpp"])
        self.assertNotIn("driver", running(other))

    def test_gate_changes_run_everything_the_tier_has(self) -> None:
        pr = self.decide("pull_request", [".github/workflows/ci.yml"], machinery="true", gui="true")
        self.assertEqual(running(pr), {"build", "asan-ubsan", "fuzz-regress", "tsan"})
        queue = self.decide("merge_group", [".github/workflows/ci.yml"], machinery="true", gui="true")
        self.assertEqual(running(queue), set(plan_legs.JOBS) - {"coverage", "profiling"})

    def test_post_merge_runs_what_the_queue_did_not(self) -> None:
        result = self.decide("push", [])
        self.assertEqual(names(result), ["linux-clang"])
        self.assertEqual(running(result), {"build", "macos", "coverage", "profiling", "driver"})

    def test_every_job_runs_somewhere_before_the_code_ships(self) -> None:
        # A leg moved out of the pull request must still be in the queue or after the merge.
        change = self.write("libs/vss_core/src/hub.cpp", "std::mutex m;\n")
        seen: set[str] = set()
        for event in ("pull_request", "merge_group", "push"):
            result = self.decide(event, [change], gui="true", gui_reason=change)
            seen |= running(result)
            seen |= {f"matrix:{n}" for n in names(result)}
        self.assertEqual(seen, set(plan_legs.JOBS) | {"matrix:linux-gcc", "matrix:linux-clang", "matrix:windows-msvc"})

    def test_dispatch_runs_everything(self) -> None:
        result = self.decide("workflow_dispatch", [])
        self.assertEqual(running(result), set(plan_legs.JOBS))
        self.assertEqual(names(result), ["linux-gcc", "linux-clang", "windows-msvc"])

    def test_unknown_event_is_an_error(self) -> None:
        with self.assertRaises(ValueError):
            plan_legs.tier_of("schedule", False)


class OutputTests(unittest.TestCase):
    def test_outputs_are_key_value_lines_for_every_job(self) -> None:
        result = plan_legs.decide("merge_group", False, plan_for(), [], CFG, ROOT)
        lines = dict(line.split("=", 1) for line in plan_legs.render_outputs(result).splitlines())
        self.assertEqual(lines["tier"], "queue")
        self.assertEqual(json.loads(lines["build_matrix"])["include"][0]["name"], "linux-gcc")
        for job in plan_legs.JOBS:
            self.assertIn(lines[f"run_{job.replace('-', '_')}"], ("true", "false"))

    def test_empty_matrix_is_an_empty_include_list(self) -> None:
        result = plan_legs.decide("pull_request", True, plan_for(), [], CFG, ROOT)
        self.assertEqual(json.loads(plan_legs.render_outputs(result).splitlines()[1].split("=", 1)[1]), {"include": []})

    def test_summary_names_every_job(self) -> None:
        text = plan_legs.render_summary(plan_legs.decide("push", False, plan_for(), [], CFG, ROOT))
        for job in plan_legs.JOBS[1:]:
            self.assertIn(job, text)


class ConfigTests(unittest.TestCase):
    def test_plan_json_has_the_keys_the_script_reads(self) -> None:
        for key in ("driver", "perf", "platform", "threads_always", "threads_scan", "threads_tokens", "gui", "machinery"):
            self.assertIn(key, CFG)

    def test_the_gate_and_its_tests_are_machinery(self) -> None:
        for path in (".github/workflows/ci.yml", "tools/ci/plan_legs.py", ".github/ci/plan.json"):
            self.assertTrue(plan_legs.matches(path, CFG["machinery"]), path)

    def test_a_markdown_file_a_test_reads_is_not_documentation(self) -> None:
        # The GUI test "support: licences match the bundled components" reads THIRD_PARTY_NOTICES.md. A change
        # to it alone must build, or the test fails in the next pull request instead.
        for path in ("THIRD_PARTY_NOTICES.md", "third_party.json"):
            self.assertFalse(plan_legs.matches(path, CFG["docs"]), path)
        self.assertTrue(plan_legs.matches("THIRD_PARTY_NOTICES.md", CFG["build"]))
        self.assertTrue(plan_legs.matches("THIRD_PARTY_NOTICES.md", CFG["gui"]))

    def test_every_markdown_file_the_gui_tests_read_is_built_for(self) -> None:
        import re

        read = re.compile(r'read_repository_file\(\s*QStringLiteral\("([^"]+\.md)"\)')
        found: set[str] = set()
        for source in (ROOT / "apps").rglob("test_*.cpp"):
            found |= set(read.findall(source.read_text(encoding="utf-8")))
        self.assertIn("THIRD_PARTY_NOTICES.md", found)
        for path in found:
            self.assertFalse(plan_legs.matches(path, CFG["docs"]), path)
            self.assertTrue(plan_legs.matches(path, CFG["gui"]), path)

    def test_documentation_still_skips_the_build(self) -> None:
        for path in (
            "README.md",
            "CHANGELOG.md",
            "SECURITY.md",
            "docs/ci.md",
            "changelog.d/a.fixed.md",
            "apps/vss_gui/assets/fonts/README.md",
            "adapters/python/README.md",
            "LICENSE",
        ):
            self.assertTrue(plan_legs.matches(path, CFG["docs"]), path)

    def test_the_shared_gate_plans_a_build_for_the_notices_and_none_for_the_readme(self) -> None:
        import importlib.util
        import os

        candidates = [os.environ.get("ICL_COMMON_INFRA", ""), str(ROOT.parent / "icl-common-infra"), str(ROOT / ".icl-infra")]
        gate = next((Path(c) / "scripts" / "plan_gate.py" for c in candidates if c and (Path(c) / "scripts" / "plan_gate.py").is_file()), None)
        if gate is None:
            # CI checks the shared scripts out into .icl-infra; a skip there would hide a broken plan.json.
            if os.environ.get("CI"):
                self.fail("the shared plan_gate.py is not checked out (.icl-infra), so the planner was not run")
            self.skipTest("no icl-common-infra checkout (set ICL_COMMON_INFRA)")
        spec = importlib.util.spec_from_file_location("plan_gate", gate)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        cfg = {**module.DEFAULT, **CFG}
        notices = module.plan(["THIRD_PARTY_NOTICES.md"], cfg)
        self.assertEqual((notices["docs_only"], notices["build"], notices["gui"]), ("false", "true", "true"))
        readme = module.plan(["README.md", "docs/ci.md"], cfg)
        self.assertEqual((readme["docs_only"], readme["build"]), ("true", "false"))

    def test_threads_token_expression_compiles_and_finds_the_real_code(self) -> None:
        import re

        tokens = re.compile(CFG["threads_tokens"])
        hub = (ROOT / "libs/vss_core/src/hub.cpp").read_text(encoding="utf-8")
        self.assertTrue(tokens.search(hub))
        self.assertFalse(tokens.search("int add(int a, int b) { return a + b; }"))


if __name__ == "__main__":
    unittest.main()
