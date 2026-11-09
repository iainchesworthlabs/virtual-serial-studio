<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Application icon

`vss_gui.svg` is the master of the Virtual Serial Studio icon. Two serial connectors face each other across a split
link. The tile is the `accent` colour, the connectors are `onAccent` and the link is `accentLine`, all from
`apps/vss_gui/src/theme/resources/tokens.json`. The icon has no text, so it needs no font, and it is the same in the
light and dark themes, as an application icon is.

Edit only the SVG. Everything else here is made from it:

| File | Holds | Used by |
|---|---|---|
| `vss_gui.ico` | 16, 24, 32, 48, 64, 128 and 256 px | The Windows resource (`vss_gui.exe` and `vss_cli.exe`) and the Windows installer |
| `vss_gui.icns` | 16 to 512 px and the @2x sizes up to 1024 px | The macOS bundle (`CFBundleIconFile`) and the disk image |
| `png/vss_gui_<size>.png` | 16, 22, 24, 32, 48, 64, 128, 256, 512 and 1024 px | The Linux hicolor icon theme (`vss_gui_512.png` is the one the `.desktop` file points at in packages that want a single file), and anything that needs a bitmap |

## Making the files

```
python3 -m pip install -r requirements/icon.txt
python3 tools/gen_app_icon.py
```

`tools/gen_app_icon.py` draws the SVG with resvg (the `resvg-py` package, pinned in `requirements/icon.txt`; it has
wheels for Windows, macOS and Linux and needs no other library). It writes the PNG, ICO and ICNS containers itself with
fixed chunk order and no metadata, so the same pixels always give the same bytes.

`python3 tools/gen_app_icon.py --check` draws the SVG again and fails when a committed file no longer matches. It
compares the decoded pixels with a tolerance of 3 levels per channel, because another resvg build can move an
anti-aliased edge by a level or two. CI runs it in the Design tokens job, and `tools/test_gen_app_icon.py` runs the same
comparison wherever `resvg-py` is installed.

Commit the SVG and the derived files together.
