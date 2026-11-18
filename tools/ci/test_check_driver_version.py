#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ci/check_driver_version.py.

    python3 -m unittest discover -s tools/ci -p "test_check_driver_version.py" -v
"""

from __future__ import annotations

import datetime
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_driver_version as cdv  # noqa: E402

INF = "[Version]\r\nSignature=\"$WINDOWS NT$\"\r\nDriverVer=01/01/2026,1.0.0.0\r\nPnpLockdown=1\r\n"


class Tree(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="vss-driver-version-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.write("drivers/windows/vsscom/public.h", "#define VSSCOM_VERSION 5\n")
        self.write("drivers/windows/vsscom/driver.c", "int main(void) { return 0; }\n")
        self.write("drivers/windows/vsscom_helper/main.cpp", "int main() {}\n")
        for inf in cdv.INFS:
            self.write(inf.as_posix(), INF)
        self.write(cdv.VERSION_FILE.as_posix(), json.dumps({"revision": 0, "date": "01/01/2026", "inputs_sha256": "0"}))
        cdv.bump(self.root, datetime.date(2026, 3, 4))

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))

    def record(self) -> dict:
        return json.loads((self.root / cdv.VERSION_FILE).read_text(encoding="utf-8"))


class Check(Tree):
    def test_a_fresh_bump_is_clean_and_names_the_interface_and_the_revision(self) -> None:
        self.assertEqual(cdv.check(self.root), [])
        self.assertEqual(self.record()["revision"], 1)
        inf = (self.root / cdv.INFS[2]).read_bytes().decode("utf-8")
        self.assertIn("DriverVer=03/04/2026,1.5.1.0\r\n", inf)

    def test_a_changed_source_without_a_bump_fails_and_says_what_to_run(self) -> None:
        self.write("drivers/windows/vsscom/driver.c", "int main(void) { return 1; }\n")
        problems = cdv.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("--bump", problems[0])
        self.assertIn("revision was not raised", problems[0])

    def test_a_bump_after_the_change_is_clean_and_raises_the_revision(self) -> None:
        self.write("drivers/windows/vsscom_helper/main.cpp", "int main() { return 1; }\n")
        cdv.bump(self.root, datetime.date(2026, 3, 5))
        self.assertEqual(cdv.check(self.root), [])
        self.assertEqual(self.record()["revision"], 2)

    def test_a_new_file_and_a_removed_file_count_as_a_change(self) -> None:
        self.write("drivers/windows/vsscom_um/new.c", "x\n")
        self.assertTrue(cdv.check(self.root))
        (self.root / "drivers/windows/vsscom_um/new.c").unlink()
        self.assertEqual(cdv.check(self.root), [])
        (self.root / "drivers/windows/vsscom_helper/main.cpp").unlink()
        self.assertTrue(cdv.check(self.root))

    def test_the_interface_number_is_part_of_the_version_and_of_the_inputs(self) -> None:
        self.write("drivers/windows/vsscom/public.h", "#define VSSCOM_VERSION 6\n")
        self.assertTrue(any("--bump" in p for p in cdv.check(self.root)))
        cdv.bump(self.root, datetime.date(2026, 3, 4))
        self.assertEqual(cdv.check(self.root), [])
        self.assertIn("1.6.2.0", (self.root / cdv.INFS[0]).read_text(encoding="utf-8"))

    def test_an_inf_with_another_driverver_fails_but_the_line_alone_is_not_an_input(self) -> None:
        path = self.root / cdv.INFS[1]
        path.write_bytes(path.read_bytes().replace(b"1.5.1.0", b"1.2.0.0"))
        problems = cdv.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("vsscomctl.inf has DriverVer=03/04/2026,1.2.0.0", problems[0])

    def test_any_other_change_of_an_inf_is_an_input(self) -> None:
        path = self.root / cdv.INFS[2]
        path.write_bytes(path.read_bytes() + b"; changed\r\n")
        self.assertTrue(cdv.check(self.root))

    def test_line_endings_do_not_change_the_hash(self) -> None:
        before = cdv.inputs_hash(self.root)
        path = self.root / "drivers/windows/vsscom/driver.c"
        path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(cdv.inputs_hash(self.root), before)

    def test_build_output_is_not_an_input(self) -> None:
        before = cdv.inputs_hash(self.root)
        self.write("drivers/windows/vsscom/x64/Release/vsscom.sys", "binary")
        self.write("drivers/windows/vsscom_um/obj/a.obj", "binary")
        self.write("drivers/windows/vsscom_um/vsscom_um.vcxproj.user", "mine")
        self.assertEqual(cdv.inputs_hash(self.root), before)

    def test_a_missing_version_file_is_a_problem_not_a_crash(self) -> None:
        (self.root / cdv.VERSION_FILE).unlink()
        self.assertTrue(cdv.check(self.root))


class AgainstTheBase(Tree):
    def base(self) -> str:
        return (self.root / cdv.VERSION_FILE).read_text(encoding="utf-8")

    def test_a_driver_that_changed_and_was_bumped_is_above_the_base(self) -> None:
        base = self.base()
        self.write("drivers/windows/vsscom/driver.c", "changed\n")
        cdv.bump(self.root, datetime.date(2026, 3, 5))
        self.assertEqual(cdv.check(self.root, base), [])

    def test_a_hash_updated_by_hand_without_a_higher_revision_is_caught(self) -> None:
        base = self.base()
        self.write("drivers/windows/vsscom/driver.c", "changed\n")
        record = self.record()
        record["inputs_sha256"] = cdv.inputs_hash(self.root)
        (self.root / cdv.VERSION_FILE).write_text(json.dumps(record), encoding="utf-8")
        problems = cdv.check(self.root, base)
        self.assertTrue(any("not above the base" in p for p in problems), problems)

    def test_the_date_may_not_go_back(self) -> None:
        base = self.base()
        self.write("drivers/windows/vsscom/driver.c", "changed\n")
        cdv.bump(self.root, datetime.date(2026, 3, 5))
        record = self.record()
        record["date"] = "03/01/2026"
        (self.root / cdv.VERSION_FILE).write_text(json.dumps(record), encoding="utf-8")
        for inf in cdv.INFS:
            path = self.root / inf
            path.write_bytes(path.read_bytes().replace(b"03/05/2026", b"03/01/2026"))
        problems = cdv.check(self.root, base)
        self.assertTrue(any("before the base" in p for p in problems), problems)

    def test_bump_never_moves_the_date_back(self) -> None:
        cdv.bump(self.root, datetime.date(2025, 1, 1))
        self.assertEqual(self.record()["date"], "03/04/2026")

    def test_a_change_to_something_that_is_not_the_driver_needs_no_bump(self) -> None:
        base = self.base()
        self.write("drivers/windows/setup-driver.ps1", "# not an input\n")
        self.assertEqual(cdv.check(self.root, base), [])


class TheRepository(unittest.TestCase):
    def test_the_driver_in_this_checkout_has_the_version_that_it_is_made_of(self) -> None:
        self.assertEqual(cdv.check(cdv.ROOT), [])


if __name__ == "__main__":
    unittest.main()
