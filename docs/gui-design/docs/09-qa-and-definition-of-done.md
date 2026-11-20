# 09 QA and definition of done

## Every work package
1. Builds on the CI compilers with `VSS_WARNINGS_AS_ERRORS=ON` (`cmake --preset debug-gui`, `cmake --preset release-gui`).
2. `ctest --preset debug-gui` passes. `QT_QPA_PLATFORM=offscreen vss_gui --selftest` passes and now covers the new widgets.
3. `python3 tools/check_contrast.py` reports `FAILURES: 0` (from this bundle's `tools/`, copied to the repo by WP1).
4. No hard-coded colours, font families or font sizes outside `Theme` (the grep test passes).
5. Spec compliance: every string, default and state in the relevant `docs/screens/` file is present. Differences are listed in `docs/design-deviations.md` with a reason.
6. Accessibility rules in `06-accessibility.md` hold for every new dialog and widget.
7. `changelog.d/` fragment added and `python tools/changelog.py --check` passes. SPDX header on new files. `clang-format` clean. Commit messages pass the repository hook.
8. Docs updated: `docs/user-guide/` and `docs/ux.md` describe what changed.

## Matrix to exercise before release
| Axis | Values |
|---|---|
| Platform | Windows 10 or 11 (with and without com0com), Ubuntu with Qt, macOS |
| Theme | Follow system (light and dark), forced light, forced dark, OS high contrast |
| Scale | 100, 150, 200 percent display scale |
| Text | Default and 200 percent text size |
| Window | 1280 by 800, 960 by 640 |
| Input | Mouse, keyboard only, screen reader |
| Mode | Simple and Advanced, switching live with a populated setup |
| Language | Pseudo-localisation (every string 40 percent longer) to catch clipping |

## Manual usability script (five people per persona per round)
1. Get a COM port for a program with no help (Operator). Pass: no hint, no wrong turns.
2. Fix a missing driver from the setup check (Operator). Pass: reaches a working port unprompted.
3. Share a device with three programs and confirm each gets data (Integrator). Pass: all three verified in the monitor.
4. Find why COM7 shows no traffic (Operator, Integrator). Pass: names the cause from the checklist.
5. Reproduce a parity error and save the case (Diagnostician). Pass: the bundle contains the fixture and the fault.
Measures: success, time, errors, System Usability Scale (target 80 or higher).
