#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for check_package_contents.py.

    python3 -m unittest discover -s tools/ci -p "test_*.py" -v

One of them reads the real CMake files of this repository, so an install rule
for the licence issuing tool fails here before it reaches a release.
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import check_package_contents as cpc  # noqa: E402


def run(*argv: str) -> tuple[int, str]:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = cpc.main(list(argv))
    return status, out.getvalue()


class Paths(unittest.TestCase):
    def test_what_ships_is_allowed(self) -> None:
        for path in ["bin/vss_gui", "bin/vss_cli", "bin/vss_cli.exe", "bin/Qt6Core.dll",
                     "share/virtual_serial_studio/LICENSES/GPL-3.0-or-later.txt",
                     "opt/virtual-serial-studio/bin/vss_gui", "lib/libvss_core.a",
                     "share/doc/licence-notes.txt", "bin/vss_agent", "bin/vss_gate"]:
            self.assertIsNone(cpc.why_forbidden(path), path)

    def test_the_issuing_tool_is_refused_on_every_system(self) -> None:
        for path in ["bin/vss_license", "bin/vss_license.exe", "BIN\\VSS_LICENSE.EXE",
                     "./opt/virtual-serial-studio/bin/vss_license",
                     "Virtual Serial Studio.app/Contents/MacOS/vss_license"]:
            self.assertEqual(cpc.why_forbidden(path), "the licence issuing tool", path)

    def test_the_tool_libraries_are_refused(self) -> None:
        for path in ["lib/libvss_license_test_keys.a", "lib/vss_license_sign.lib", "lib/libvss_license_tool.a"]:
            self.assertEqual(cpc.why_forbidden(path), "a library of the licence issuing tool", path)

    def test_key_material_is_refused(self) -> None:
        for path in ["share/lic-2026-a.key", "etc/signing.pem", "x/y.seed", "cert.p12", "cert.PFX"]:
            self.assertEqual(cpc.why_forbidden(path), "key material", path)

    def test_a_name_that_only_looks_alike_is_allowed(self) -> None:
        for path in ["share/doc/vss_license.md", "bin/vss_licence_helper", "share/keys.txt", "bin/keygen-notes"]:
            self.assertIsNone(cpc.why_forbidden(path), path)


class Listings(unittest.TestCase):
    def test_the_path_is_the_last_field(self) -> None:
        text = ("-rwxr-xr-x root/root   123456 2026-11-15 04:00 ./opt/virtual-serial-studio/bin/vss_gui\n"
                "lrwxrwxrwx root/root        0 2026-11-15 04:00 ./usr/bin/vss_cli -> /opt/virtual-serial-studio/bin/vss_cli\n"
                "\n"
                "/opt/virtual-serial-studio/bin/vss_cli\n")
        self.assertEqual(cpc.listing_paths(text),
                         ["./opt/virtual-serial-studio/bin/vss_gui", "./usr/bin/vss_cli",
                          "/opt/virtual-serial-studio/bin/vss_cli"])

    def test_a_listing_with_the_tool_fails(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            listing = Path(folder) / "packages.txt"
            listing.write_text("/opt/virtual-serial-studio/bin/vss_gui\n"
                               "-rwxr-xr-x root/root 1 2026-11-15 04:00 ./opt/virtual-serial-studio/bin/vss_license\n",
                               encoding="utf-8")
            status, out = run("--listing", str(listing))
            self.assertEqual(status, 1)
            self.assertIn("vss_license: the licence issuing tool must not be in a package", out)
            self.assertNotIn("vss_gui:", out)

    def test_a_clean_listing_passes(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            listing = Path(folder) / "packages.txt"
            listing.write_text("/opt/virtual-serial-studio/bin/vss_gui\n/opt/virtual-serial-studio/bin/vss_cli\n",
                               encoding="utf-8")
            status, out = run("--listing", str(listing))
            self.assertEqual(status, 0)
            self.assertIn("2 packaged path(s) read", out)


class Trees(unittest.TestCase):
    def make(self, folder: str, names: list[str]) -> Path:
        root = Path(folder) / "stage"
        for name in names:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x")
        return root

    def test_a_staged_tree_without_the_tool_passes(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = self.make(folder, ["bin/vss_gui.exe", "bin/vss_cli.exe", "share/LICENSES/GPL-3.0-or-later.txt"])
            status, out = run("--tree", str(root))
            self.assertEqual(status, 0)
            self.assertIn("3 packaged path(s) read", out)

    def test_a_staged_tree_with_the_tool_or_a_key_fails(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = self.make(folder, ["bin/vss_gui.exe", "bin/vss_license.exe", "share/lic-2026-a.key"])
            status, out = run("--tree", str(root))
            self.assertEqual(status, 1)
            self.assertIn("bin/vss_license.exe: the licence issuing tool", out)
            self.assertIn("share/lic-2026-a.key: key material", out)

    def test_usage_errors(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                cpc.main([])
            self.assertEqual(raised.exception.code, 2)
            with self.assertRaises(SystemExit) as raised:
                cpc.main(["--tree", str(Path(tempfile.gettempdir()) / "no-such-folder-for-this-test")])
            self.assertEqual(raised.exception.code, 2)


class Source(unittest.TestCase):
    def checkout(self, folder: str, tool: str, top: str) -> Path:
        root = Path(folder)
        (root / "apps" / "vss_license").mkdir(parents=True)
        (root / "apps" / "vss_license" / "CMakeLists.txt").write_text(tool, encoding="utf-8")
        (root / "CMakeLists.txt").write_text(top, encoding="utf-8")
        # A build tree and vendored code are not this project's install rules.
        (root / "build" / "x").mkdir(parents=True)
        (root / "build" / "x" / "cmake_install.cmake").write_text(
            "install(TARGETS vss_license_program)\n", encoding="utf-8")
        return root

    def test_the_real_repository_installs_no_licence_tool(self) -> None:
        self.assertEqual(cpc.check_source(ROOT), [])
        status, _ = run("--source", str(ROOT))
        self.assertEqual(status, 0)

    def test_an_install_rule_in_the_tool_is_found(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = self.checkout(
                folder,
                "add_executable(vss_license_program main.cpp)\n"
                "install(TARGETS vss_license_program\n  RUNTIME DESTINATION bin\n  COMPONENT runtime)\n",
                "project(x)\n")
            problems = cpc.check_source(root)
            self.assertTrue(any("apps/vss_license/CMakeLists.txt:2" in p and "vss_license_program" in p
                                for p in problems), problems)
            self.assertTrue(any("has an install() rule" in p for p in problems), problems)

    def test_an_install_rule_elsewhere_is_found(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = self.checkout(
                folder, "add_executable(vss_license_program main.cpp)\n",
                "project(x)\n# install(TARGETS vss_license_program) in a comment is not a rule\n"
                "  INSTALL (TARGETS vss_cli vss_license_test_keys DESTINATION lib)\n")
            problems = cpc.check_source(root)
            self.assertEqual(len(problems), 1, problems)
            self.assertIn("CMakeLists.txt:3", problems[0])
            self.assertIn("vss_license_test_keys", problems[0])

    def test_the_installed_target_list_is_read(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = self.checkout(
                folder, "add_executable(vss_license_program main.cpp)\n",
                "foreach(_vss_target vss_core vss_ports vss_license vss_studio)\nendforeach()\n")
            problems = cpc.check_source(root)
            self.assertEqual(len(problems), 1, problems)
            self.assertIn("the installed targets include vss_license", problems[0])

    def test_a_clean_checkout_passes(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = self.checkout(
                folder, "add_executable(vss_license_program main.cpp)\n",
                "foreach(_vss_target vss_core vss_ports vss_config vss_studio)\nendforeach()\n"
                "install(TARGETS vss_cli RUNTIME DESTINATION bin)\n")
            self.assertEqual(cpc.check_source(root), [])


if __name__ == "__main__":
    unittest.main()
