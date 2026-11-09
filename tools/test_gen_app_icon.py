#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for gen_app_icon.py.

    python3 -m unittest discover -s tools -p "test_gen_app_icon.py" -v

The container and comparison tests need nothing but the standard library. The tests that draw the SVG need
the pinned renderer (python3 -m pip install -r requirements/icon.txt) and are skipped without it; CI installs
it and runs them, together with `gen_app_icon.py --check`.
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_app_icon as icon  # noqa: E402

try:
    import resvg_py  # noqa: F401, E402

    HAVE_RENDERER = True
except ImportError:
    HAVE_RENDERER = False


def solid(size: int, rgba: tuple[int, int, int, int]) -> bytes:
    return icon.encode_png(size, size, bytes(rgba) * (size * size))


class Png(unittest.TestCase):
    def test_round_trip(self) -> None:
        pixels = bytes(range(16)) * 4
        data = icon.encode_png(4, 4, pixels)
        self.assertEqual(icon.decode_png(data), (4, 4, pixels))

    def test_same_pixels_give_same_bytes(self) -> None:
        self.assertEqual(solid(8, (1, 2, 3, 4)), solid(8, (1, 2, 3, 4)))

    def test_rejects_other_data(self) -> None:
        with self.assertRaises(icon.IconError):
            icon.decode_png(b"GIF89a" + bytes(40))

    def test_decodes_every_filter(self) -> None:
        width, height = 3, 5
        rows = [bytes((x * 7 + y * 13) & 255 for x in range(width * 4)) for y in range(height)]
        # Filter each row with a different method and check the decoder undoes all of them.
        raw = bytearray()
        previous = bytes(width * 4)
        for y, row in enumerate(rows):
            mode = y % 5
            filtered = bytearray(width * 4)
            for i in range(width * 4):
                left = row[i - 4] if i >= 4 else 0
                up = previous[i]
                upper_left = previous[i - 4] if i >= 4 else 0
                predictor = (0, left, up, (left + up) >> 1, icon._paeth(left, up, upper_left))[mode]
                filtered[i] = (row[i] - predictor) & 255
            raw += bytes([mode]) + filtered
            previous = row
        import zlib

        data = (
            icon.PNG_SIGNATURE
            + icon._chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + icon._chunk(b"IDAT", zlib.compress(bytes(raw)))
            + icon._chunk(b"IEND", b"")
        )
        self.assertEqual(icon.decode_png(data)[2], b"".join(rows))


class Containers(unittest.TestCase):
    def images(self) -> dict[int, bytes]:
        return {size: solid(size, (size % 256, 0, 0, 255)) for size in icon.ALL_SIZES}

    def test_ico_lists_every_size_once(self) -> None:
        entries = icon.parse_ico(icon.build_ico(self.images()))
        self.assertEqual([size for size, _ in entries], list(icon.ICO_SIZES))
        for size, body in entries:
            self.assertEqual(icon.decode_png(body)[:2], (size, size))

    def test_ico_stores_256_as_zero(self) -> None:
        data = icon.build_ico(self.images())
        last = 6 + 16 * (len(icon.ICO_SIZES) - 1)
        self.assertEqual(data[last : last + 2], b"\x00\x00")

    def test_icns_has_the_standard_elements(self) -> None:
        entries = icon.parse_icns(icon.build_icns(self.images()))
        self.assertEqual([kind for kind, _ in entries], [kind for kind, _ in icon.ICNS_TYPES])
        for (kind, size), (_, body) in zip(icon.ICNS_TYPES, entries):
            self.assertEqual(icon.decode_png(body)[:2], (size, size), kind)
        self.assertIn(("ic10", 1024), icon.ICNS_TYPES)  # 512 points at @2x
        self.assertIn(("ic11", 32), icon.ICNS_TYPES)  # 16 points at @2x

    def test_icns_length_must_match(self) -> None:
        data = icon.build_icns(self.images())
        with self.assertRaises(icon.IconError):
            icon.parse_icns(data + b"\x00")

    def test_sizes_cover_the_platforms(self) -> None:
        self.assertEqual(icon.ICO_SIZES, (16, 24, 32, 48, 64, 128, 256))
        self.assertTrue({16, 32, 48, 64, 128, 256, 512, 1024} <= set(icon.PNG_SIZES))


class Comparison(unittest.TestCase):
    def test_tolerance_allows_a_level_or_two(self) -> None:
        self.assertTrue(icon.pixels_match(solid(4, (10, 10, 10, 255)), solid(4, (12, 9, 10, 255))))

    def test_a_different_colour_does_not_match(self) -> None:
        self.assertFalse(icon.pixels_match(solid(4, (10, 10, 10, 255)), solid(4, (40, 10, 10, 255))))

    def test_a_different_size_does_not_match(self) -> None:
        self.assertFalse(icon.pixels_match(solid(4, (1, 1, 1, 255)), solid(8, (1, 1, 1, 255))))

    def test_differences_names_missing_changed_and_stray_files(self) -> None:
        expected = {"vss_gui.ico": b"ico", "png/vss_gui_16.png": solid(2, (0, 0, 0, 255))}
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.assertEqual(len(icon.differences(expected, directory)), 2)
            icon.write_files(expected, directory)
            self.assertEqual(icon.differences(expected, directory), [])
            (directory / "png" / "vss_gui_16.png").write_bytes(solid(2, (200, 0, 0, 255)))
            (directory / "png" / "vss_gui_17.png").write_bytes(solid(2, (0, 0, 0, 255)))
            problems = icon.differences(expected, directory)
            self.assertEqual(len(problems), 2)
            self.assertIn("vss_gui_16.png does not match", problems[0])
            self.assertIn("vss_gui_17.png is not made", problems[1])


@unittest.skipUnless(HAVE_RENDERER, "resvg-py is not installed (requirements/icon.txt)")
class Rendering(unittest.TestCase):
    def test_master_has_no_text_and_no_font(self) -> None:
        text = icon.SVG.read_text(encoding="utf-8")
        for word in ("<text", "font-family", "<image", "<script", "href="):
            self.assertNotIn(word, text)

    def test_the_tile_is_drawn_and_the_corner_is_transparent(self) -> None:
        for size in (16, 64, 256):
            rgba = icon.render(icon.SVG, size)
            self.assertEqual(rgba[3], 0, "the corner pixel is transparent")
            centre = (size // 2 * size + size // 2) * 4
            self.assertEqual(rgba[centre + 3], 255, "the middle of the icon is opaque")

    def test_rendering_is_repeatable(self) -> None:
        self.assertEqual(icon.render_png(icon.SVG, 48), icon.render_png(icon.SVG, 48))

    def test_the_committed_files_are_current(self) -> None:
        problems = icon.differences(icon.expected_files(), icon.ICON_DIR)
        self.assertEqual(problems, [], "run python3 tools/gen_app_icon.py and commit the result")


if __name__ == "__main__":
    unittest.main()
