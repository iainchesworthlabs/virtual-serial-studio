#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for release_verify.py and the two scripts the publish workflow runs.

    python3 -m unittest discover -s tools/ci -p "test_release_verify.py" -v
"""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import release_verify  # noqa: E402


def release(files: dict[str, bytes], listed: list[str] | None = None, signed: bool = True) -> Path:
    """A directory laid out the way the publish job leaves it: the files, SHA512SUMS and the signatures."""
    directory = Path(tempfile.mkdtemp())
    for name, content in files.items():
        (directory / name).write_bytes(content)
    lines = [f"{hashlib.sha512(files[n]).hexdigest()}  {n}" for n in (listed if listed is not None else sorted(files))]
    (directory / "SHA512SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")
    if signed:
        for name in [*files, "SHA512SUMS"]:
            (directory / f"{name}.asc").write_text("signature", encoding="utf-8")
    return directory


GOOD = {
    "virtual-serial-studio_0.1.0.rc.5-1_amd64.deb": b"deb",
    "virtual-serial-studio-v0.1.0-rc.5-linux-x86_64.tar.gz": b"tar",
    "sbom.spdx.json": b"{}",
}


class Verify(unittest.TestCase):
    def test_a_release_in_order_has_no_problem(self):
        self.assertEqual(release_verify.problems(release(GOOD)), [])

    def test_a_tilde_in_a_file_name_is_refused(self):
        files = {**GOOD, "virtual-serial-studio_0.1.0~rc.5-1_amd64.deb": b"x"}
        del files["virtual-serial-studio_0.1.0.rc.5-1_amd64.deb"]
        found = release_verify.problems(release(files))
        self.assertTrue(any("~rc.5" in text and "GitHub changes" in text for text in found), found)
        # The sums name the file under the name it has here, which the release will not carry.
        self.assertTrue(any("SHA512SUMS names" in text for text in found), found)

    def test_a_file_the_sums_leave_out_is_named(self):
        found = release_verify.problems(release(GOOD, listed=["virtual-serial-studio-v0.1.0-rc.5-linux-x86_64.tar.gz"]))
        self.assertIn("SHA512SUMS does not list sbom.spdx.json", found)

    def test_the_sbom_is_one_of_the_files_the_sums_must_list(self):
        names = sorted(GOOD)
        names.remove("sbom.spdx.json")
        found = release_verify.problems(release(GOOD, listed=names))
        self.assertEqual([t for t in found if "does not list" in t], ["SHA512SUMS does not list sbom.spdx.json"])

    def test_a_name_in_the_sums_that_is_not_in_the_release_is_named(self):
        directory = release(GOOD)
        with (directory / "SHA512SUMS").open("a", encoding="utf-8") as sums:
            sums.write(f"{'0' * 128}  gone.deb\n")
        found = release_verify.problems(directory)
        self.assertIn("SHA512SUMS names gone.deb, which is not in the release", found)

    def test_a_wrong_sum_is_named(self):
        directory = release(GOOD)
        (directory / "sbom.spdx.json").write_bytes(b"changed")
        self.assertIn("SHA512SUMS has a wrong sum for sbom.spdx.json", release_verify.problems(directory))

    def test_a_file_without_a_signature_is_named(self):
        directory = release(GOOD)
        (directory / "sbom.spdx.json.asc").unlink()
        self.assertIn("sbom.spdx.json has no signature (sbom.spdx.json.asc)", release_verify.problems(directory))

    def test_a_release_without_an_sbom_is_refused(self):
        files = {k: v for k, v in GOOD.items() if k != "sbom.spdx.json"}
        self.assertIn("sbom.spdx.json is missing", release_verify.problems(release(files)))

    def test_missing_sums_and_an_empty_directory_are_refused(self):
        directory = release(GOOD)
        (directory / "SHA512SUMS").unlink()
        self.assertIn("SHA512SUMS is missing", release_verify.problems(directory))
        self.assertTrue(release_verify.problems(Path(tempfile.mkdtemp())))

    def test_the_exit_status_follows_the_problems(self):
        self.assertEqual(release_verify.main(["release_verify.py", str(release(GOOD))]), 0)
        self.assertEqual(release_verify.main(["release_verify.py", str(release(GOOD, signed=False))]), 1)
        self.assertEqual(release_verify.main(["release_verify.py"]), 2)


@unittest.skipIf(sys.platform == "win32" or not shutil.which("bash"), "the workflow runs these with bash on Linux")
class Scripts(unittest.TestCase):
    def test_the_pre_release_script_adds_the_sbom_to_the_directory(self):
        dist = Path(tempfile.mkdtemp())
        done = subprocess.run(
            ["bash", str(HERE / "release_prepare.sh"), str(dist), "0.1.0-rc.5", "v0.1.0-rc.5"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(json.loads((dist / "sbom.spdx.json").read_text(encoding="utf-8"))["spdxVersion"], "SPDX-2.3")

    def test_the_verify_script_passes_its_arguments_on(self):
        good = subprocess.run(["bash", str(HERE / "release_verify.sh"), str(release(GOOD)), "key.asc"], capture_output=True, check=False)
        bad = subprocess.run(["bash", str(HERE / "release_verify.sh"), str(release(GOOD, signed=False)), "key.asc"], capture_output=True, check=False)
        self.assertEqual(good.returncode, 0)
        self.assertEqual(bad.returncode, 1)


if __name__ == "__main__":
    unittest.main()
