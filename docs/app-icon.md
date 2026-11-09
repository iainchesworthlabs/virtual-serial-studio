# Application icon

Virtual Serial Studio has one icon: two serial connectors facing each other across a split link. It is drawn in the
design tokens' `accent`, `onAccent` and `accentLine` colours, has no text and so needs no font, and looks the same in the
light and dark themes, as an application icon does.

## Where the files are

Everything is under `apps/vss_gui/assets/icon/`. The SVG is the only file to edit; the rest is made from it.

| File | Holds | Used for |
|---|---|---|
| `vss_gui.svg` | The master, 1024 by 1024 | The Qt resource `:/app/vss_gui.svg` (window, taskbar, dock and About icon), the `scalable` hicolor icon, and the source of every other file |
| `vss_gui.ico` | 16, 24, 32, 48, 64, 128 and 256 px | The icon of `vss_gui.exe` and `vss_cli.exe` in Explorer, the Start menu and the Windows installer |
| `vss_gui.icns` | 16 to 512 px and the @2x sizes up to 1024 px | `vss_gui.app/Contents/Resources/vss_gui.icns`, named by `CFBundleIconFile`, and the disk image |
| `png/vss_gui_<size>.png` | 16, 22, 24, 32, 48, 64, 128, 256, 512 and 1024 px | The Linux hicolor theme, and anything that needs a bitmap. `png/vss_gui_512.png` is the one for a package that wants a single file |

## Making the files

```
python3 -m pip install -r requirements/icon.txt
python3 tools/gen_app_icon.py
```

`tools/gen_app_icon.py` draws the SVG with resvg (the `resvg-py` package, pinned in `requirements/icon.txt`, which has wheels
for Windows, macOS and Linux) and writes the PNG, ICO and ICNS containers itself, so the same pixels always give the same
bytes. `python3 tools/gen_app_icon.py --check` draws the SVG again and fails when a committed file no longer matches it
(the pixels are compared with a tolerance of 3 levels, because another resvg build can move an anti-aliased edge by a level
or two). The Design tokens job in `ci.yml` runs the check and `tools/test_gen_app_icon.py`, so a change to the SVG without
the derived files does not merge.

## How each platform gets it

| Platform | Where it is wired |
|---|---|
| All | `IconProvider::app_icon()` loads `:/app/vss_gui.svg` (`apps/vss_gui/src/theme/theme_resources.qrc`) and `main.cpp` passes it to `QGuiApplication::setWindowIcon`. It is drawn at any size and device pixel ratio |
| Windows | `cmake/VssWindowsResources.cmake` adds the `.ico` as the first icon resource of every target with a `VSS_ICON` property, next to the version information. `vss_gui` and `vss_cli` set it; each also sets its own `VSS_FILE_DESCRIPTION`. Product name, company, copyright and file and product version (from `project(VERSION)`) are the same in both |
| macOS | `apps/vss_gui/CMakeLists.txt` puts the `.icns` in the bundle's `Resources` and sets `MACOSX_BUNDLE_ICON_FILE`, which `macos/Info.plist.in` writes as `CFBundleIconFile` |
| Linux | The `runtime` install component holds `share/icons/hicolor/<size>x<size>/apps/vss_gui.png` for 16 to 512 px, `share/icons/hicolor/scalable/apps/vss_gui.svg` and `share/applications/vss_gui.desktop` (`apps/vss_gui/linux/vss_gui.desktop`). The program sets the desktop file name `vss_gui`, so a Wayland desktop pairs the window with the entry |

## Checks

- `vss_gui --selftest` fails when the application icon is missing or draws nothing at 16, 32 and 256 px.
- The theme tests check the icon at every size and that a theme change leaves it as it was; the About tests and the UI
  journey "About shows the application icon, the version and the source" check that the About window shows it.
- The Windows resource has the icon and the version block for both programs; the version comes from `project(VERSION)` in
  the top `CMakeLists.txt`, so a release changes it in one place.

Help, About shows the icon, the version, the licence (GPL-3.0-or-later) and a link to the source. The link opens in the
system's browser when the user follows it; the program makes no network request itself.
