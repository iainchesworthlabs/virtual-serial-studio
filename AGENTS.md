# Instructions for coding agents

This file applies to any automated contributor. Read it before changing the repository. The project standard is
[`docs/standard.md`](https://github.com/iainchesworthlabs/icl-common-infra/blob/develop/docs/standard.md) in
`icl-common-infra`; `docs/migration.md` lists where this repository differs from it.

## Working rules

- Work on a branch named `<prefix>/<slug>` with the prefix `feature`, `bugfix`, `docs`, `chore` or `agent`, and a
  lower-case slug with hyphens. Never commit to `develop` or `main` directly.
- Open the pull request as a draft and mark it ready when the work is complete and `python3 tools/ci/precheck.py` is
  clean. A draft runs the planner and the static checks only; a ready pull request starts the build jobs, and every push
  to it starts them again. Push once the work is ready, not after each small step. `gh workflow run ci.yml --ref
  <branch>` runs every leg before a change to threads, platform code or the build is queued.
- Commit with `git commit -s`. Write the subject in the imperative, 72 characters or fewer, and the body as a plain
  description of what changed and why. Commit messages and pull request text carry no attribution of any kind.
- Open pull requests against `develop`. Squash merge is the only method.
- Do not create or edit secrets, tags, releases, repository settings or rulesets. The owner does those.
- Do not edit `cmake/icl/`. Change `icl-common-infra` and run `python3 tools/sync_cmake.py --infra <checkout>`.
- Boost.VirtualSerial comes from `iainchesworthlabs/virtual-serial`. Do not change `libs/vss_core` behaviour or the
  JSON topology schema without being asked. If a capability is missing, follow `docs/gui-design/docs/08-engine-gaps.md`:
  add a capability flag and degrade visibly.
- Warnings are errors in every build. The build must stay clean on GCC, Clang, AppleClang and MSVC, so write portable
  code and never disable a test, warning, hook or lint to get green.
- No secrets in the repository or in logs. No network access from the GUI, no telemetry, no analytics.

## Build and test

```
cmake --preset linux-debug-gui        # windows-msvc-debug-gui or macos-debug-gui elsewhere
cmake --build --preset linux-debug-gui
ctest --preset linux-debug-gui
QT_QPA_PLATFORM=offscreen ./build/linux-debug-gui/bin/vss_gui --selftest
python3 tools/changelog.py --check
```

Qt is found through `VSS_QT_ROOT`, `ICL_QT_ROOT`, `QT_ROOT_DIR` or `QTDIR`. The project has its own test framework
(`tests/`, `vss_unit_tests`); use it and do not add another.

## Code rules

- C++23. Every first-party source file carries an SPDX licence line naming GPL-3.0-or-later.
- Names follow `.clang-tidy`: `snake_case` for namespaces, types, functions and variables, and a trailing `_` on data
  members. The Qt widgets in `apps/vss_gui` are the exception, as `apps/vss_gui/.clang-tidy` says. Layout follows
  `.clang-format` (Allman braces, 80 columns).
- Threading: each endpoint runs on its own strand and events are dispatched on the hub's dispatcher strand. The GUI
  thread must never be called from, or block on, those threads. Marshal events to the GUI thread with queued
  connections and coalesce them. `unsubscribe` cannot recall a delivery that is already running, so a sink must
  capture state that outlives the hub (a `shared_ptr`), never a stack local.
- `vss::result<T>` is `boost::system::result`. The engine event sink is a `std::function` behind `vss::EventSink`.

## GUI rules

The specs are in `docs/gui-design/`. Specs, tokens and the HTML mockups are text; for a visual check write an offscreen
`grab()` PNG for a person to review, and do not commit it.

- Use design tokens only. No literal colours, font families or font sizes outside the `Theme` module. Icons come from
  `IconProvider`. The only fonts in the repository are the OFL
  faces in `apps/vss_gui/assets/fonts/` (Noto Sans Hebrew and Arabic, IBM Plex Mono), registered by `Theme`; do not add
  others without the owner's say.
- Copy strings exactly from the specs and wrap them in `tr()`.
- Every widget has an accessible name and role, is reachable by keyboard, shows a visible focus ring, works in both
  themes, has targets of at least 24 px and works at 200 percent text.

## If you change X, update Y

| You change | Also update |
|---|---|
| A user-visible behaviour | A fragment in `changelog.d/` |
| A CMake option or preset | `README.md` |
| A workflow in `.github/workflows` | Pin every `uses:` to a full SHA with a version comment |
| A toolchain version | `.github/toolchain-versions.json` only |
| A new source file | The SPDX licence header, copied from an existing file |
