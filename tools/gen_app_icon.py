#!/usr/bin/env python3
# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make the application icon files from the master SVG, or check that they are current.

    python3 tools/gen_app_icon.py            write the derived files
    python3 tools/gen_app_icon.py --check    fail if a committed file differs from the SVG

The master is apps/vss_gui/assets/icon/vss_gui.svg. From it this makes, in the same directory:

    vss_gui.ico             16, 24, 32, 48, 64, 128 and 256 px (Windows)
    vss_gui.icns            16 to 512 px and the @2x sizes up to 1024 px (macOS)
    png/vss_gui_<size>.png  16, 22, 24, 32, 48, 64, 128, 256, 512 and 1024 px (Linux hicolor, Qt)

The SVG is rendered with resvg (the `resvg-py` package, pinned in requirements/icon.txt). The PNG, ICO and
ICNS containers are written here, with fixed chunk order, no metadata and no timestamps, so the same pixels
always give the same bytes. A different resvg build can move an anti-aliased edge by a level or two, so
--check compares the decoded pixels with a small tolerance and not the bytes.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ICON_DIR = ROOT / "apps" / "vss_gui" / "assets" / "icon"
SVG = ICON_DIR / "vss_gui.svg"

ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
PNG_SIZES = (16, 22, 24, 32, 48, 64, 128, 256, 512, 1024)
# ICNS element types and the pixel size of the PNG each holds. ic11 to ic14 are the @2x forms of 16, 32,
# 128 and 256 points; ic10 is 512 points at @2x.
ICNS_TYPES = (
    ("icp4", 16),
    ("icp5", 32),
    ("icp6", 64),
    ("ic07", 128),
    ("ic08", 256),
    ("ic09", 512),
    ("ic10", 1024),
    ("ic11", 32),
    ("ic12", 64),
    ("ic13", 256),
    ("ic14", 512),
)
ALL_SIZES = tuple(sorted(set(ICO_SIZES) | set(PNG_SIZES) | {size for _, size in ICNS_TYPES}))

# A pixel may differ by this much per channel and still count as the same icon.
TOLERANCE = 3

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class IconError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------
# PNG: a decoder for the 8-bit RGBA images resvg writes, and a deterministic encoder.


def _chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def encode_png(width: int, height: int, rgba: bytes) -> bytes:
    """An 8-bit RGBA PNG with filter 0 on every row, one IDAT and nothing else."""
    if len(rgba) != width * height * 4:
        raise IconError("pixel data does not match the image size")
    stride = width * 4
    raw = b"".join(b"\x00" + rgba[row * stride : (row + 1) * stride] for row in range(height))
    return (
        PNG_SIGNATURE
        + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + _chunk(b"IDAT", zlib.compress(raw, 9))
        + _chunk(b"IEND", b"")
    )


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode_png(data: bytes) -> tuple[int, int, bytes]:
    """Width, height and RGBA bytes of a non-interlaced 8-bit RGBA or RGB PNG."""
    if data[:8] != PNG_SIGNATURE:
        raise IconError("not a PNG file")
    pos = 8
    header = None
    idat = bytearray()
    while pos + 8 <= len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        if len(body) != length:
            raise IconError("truncated PNG chunk")
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length
    if header is None:
        raise IconError("PNG has no header")
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in (2, 6) or interlace != 0:
        raise IconError("only non-interlaced 8-bit RGB and RGBA PNG files are supported")
    channels = 4 if colour == 6 else 3
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    if len(raw) != (stride + 1) * height:
        raise IconError("PNG pixel data has the wrong length")
    rows: list[bytearray] = []
    previous = bytearray(stride)
    for row in range(height):
        base = row * (stride + 1)
        mode = raw[base]
        line = bytearray(raw[base + 1 : base + 1 + stride])
        if mode == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif mode == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 255
        elif mode == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 255
        elif mode == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                upper_left = previous[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, previous[i], upper_left)) & 255
        elif mode != 0:
            raise IconError(f"unknown PNG filter {mode}")
        rows.append(line)
        previous = line
    pixels = bytearray()
    for line in rows:
        if channels == 3:
            for i in range(0, stride, 3):
                pixels += line[i : i + 3] + b"\xff"
        else:
            pixels += line
    return width, height, bytes(pixels)


# ---------------------------------------------------------------------------------------------------------
# Rendering.


def render(svg_path: Path, size: int) -> bytes:
    """The SVG at size by size pixels as RGBA bytes."""
    try:
        import resvg_py
    except ImportError as exc:
        raise IconError(
            "the resvg-py package is needed to render the icon: python3 -m pip install -r requirements/icon.txt"
        ) from exc
    png = bytes(resvg_py.svg_to_bytes(svg_path=str(svg_path), width=size, height=size))
    width, height, rgba = decode_png(png)
    if (width, height) != (size, size):
        raise IconError(f"resvg drew {width}x{height} instead of {size}x{size}")
    return rgba


def render_png(svg_path: Path, size: int) -> bytes:
    return encode_png(size, size, render(svg_path, size))


# ---------------------------------------------------------------------------------------------------------
# ICO and ICNS: both hold PNG images as they are.


def build_ico(images: dict[int, bytes]) -> bytes:
    """An icon file with one PNG per size in ICO_SIZES. A size of 256 is stored as 0, as the format says."""
    sizes = [size for size in ICO_SIZES if size in images]
    out = bytearray(struct.pack("<HHH", 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    for size in sizes:
        data = images[size]
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for size in sizes:
        out += images[size]
    return bytes(out)


def parse_ico(data: bytes) -> list[tuple[int, bytes]]:
    if len(data) < 6 or struct.unpack_from("<HH", data, 0) != (0, 1):
        raise IconError("not an ICO file")
    (count,) = struct.unpack_from("<H", data, 4)
    entries = []
    for index in range(count):
        width, height, _, _, _, _, length, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * index)
        body = data[offset : offset + length]
        if len(body) != length:
            raise IconError("truncated ICO image")
        entries.append(((width or 256), body))
    return entries


def build_icns(images: dict[int, bytes]) -> bytes:
    body = b"".join(kind.encode("ascii") + struct.pack(">I", 8 + len(images[size])) + images[size] for kind, size in ICNS_TYPES)
    return b"icns" + struct.pack(">I", 8 + len(body)) + body


def parse_icns(data: bytes) -> list[tuple[str, bytes]]:
    if data[:4] != b"icns" or len(data) < 8:
        raise IconError("not an ICNS file")
    (total,) = struct.unpack_from(">I", data, 4)
    if total != len(data):
        raise IconError("ICNS length does not match the file")
    pos = 8
    entries = []
    while pos + 8 <= len(data):
        kind = data[pos : pos + 4].decode("ascii", "replace")
        (length,) = struct.unpack_from(">I", data, pos + 4)
        if length < 8 or pos + length > len(data):
            raise IconError("truncated ICNS element")
        entries.append((kind, data[pos + 8 : pos + length]))
        pos += length
    return entries


# ---------------------------------------------------------------------------------------------------------
# The set of files.


def expected_files(svg_path: Path = SVG) -> dict[str, bytes]:
    """Path (relative to the icon directory) to content, for every derived file."""
    images = {size: render_png(svg_path, size) for size in ALL_SIZES}
    files = {"vss_gui.ico": build_ico(images), "vss_gui.icns": build_icns(images)}
    for size in PNG_SIZES:
        files[f"png/vss_gui_{size}.png"] = images[size]
    return files


def pixels_match(a: bytes, b: bytes, tolerance: int = TOLERANCE) -> bool:
    """Whether two PNG files have the same size and pixels to within tolerance per channel."""
    if a == b:
        return True
    wa, ha, pa = decode_png(a)
    wb, hb, pb = decode_png(b)
    if (wa, ha) != (wb, hb):
        return False
    return all(abs(x - y) <= tolerance for x, y in zip(pa, pb))


def differences(expected: dict[str, bytes], directory: Path) -> list[str]:
    """What is wrong with the files in directory, as sentences. Empty when they are current."""
    problems: list[str] = []
    for name, want in expected.items():
        path = directory / name
        if not path.is_file():
            problems.append(f"{name} is missing")
            continue
        have = path.read_bytes()
        if have == want:
            continue
        try:
            if name.endswith(".png"):
                same = pixels_match(have, want)
            elif name.endswith(".ico"):
                a, b = parse_ico(have), parse_ico(want)
                same = [s for s, _ in a] == [s for s, _ in b] and all(pixels_match(x, y) for (_, x), (_, y) in zip(a, b))
            else:
                a, b = parse_icns(have), parse_icns(want)
                same = [k for k, _ in a] == [k for k, _ in b] and all(pixels_match(x, y) for (_, x), (_, y) in zip(a, b))
        except (IconError, zlib.error, struct.error) as exc:
            problems.append(f"{name} cannot be read: {exc}")
            continue
        if not same:
            problems.append(f"{name} does not match vss_gui.svg")
    known = set(expected)
    for path in sorted((directory / "png").glob("*.png")) if (directory / "png").is_dir() else []:
        if f"png/{path.name}" not in known:
            problems.append(f"png/{path.name} is not made from vss_gui.svg")
    return problems


def write_files(files: dict[str, bytes], directory: Path) -> None:
    for name, data in files.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare the committed files with the SVG and write nothing")
    ap.add_argument("--svg", type=Path, default=SVG, help="the master SVG (default: %(default)s)")
    ap.add_argument("--out", type=Path, default=ICON_DIR, help="the directory to write to or check (default: %(default)s)")
    args = ap.parse_args(argv)
    try:
        files = expected_files(args.svg)
        if args.check:
            problems = differences(files, args.out)
            for problem in problems:
                print(f"gen_app_icon: {problem}", file=sys.stderr)
            if problems:
                print("gen_app_icon: run python3 tools/gen_app_icon.py and commit the result", file=sys.stderr)
                return 1
            print(f"gen_app_icon: {len(files)} files match {args.svg.name}")
            return 0
        write_files(files, args.out)
    except IconError as exc:
        print(f"gen_app_icon: {exc}", file=sys.stderr)
        return 2
    print(f"gen_app_icon: wrote {len(files)} files to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
