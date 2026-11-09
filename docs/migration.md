# Migration to the ICL standard

This page records how Virtual Serial Studio and its library, Boost.VirtualSerial (`virtual-serial`), move into the `iainchesworthlabs` organisation and adopt the ICL C++ standard (`icl-common-infra`, `docs/standard.md`). It covers both repositories and is kept up to date until the work is finished. Dates are 2026.

## Scope

- `virtual-serial-studio` (this repository): brought fully up to the standard.
- `virtual-serial` (the library): hosted in the organisation as a Boost-bound project. It stays under the Boost Software License with Boost conventions, so only the hosting (settings, CI plumbing, releases, policy) follows the standard. Its own migration plan is not kept in its tree.
- Virtual ports on Windows, Linux and macOS, and the design review of the GUI, are part of the same effort and are listed below.

## Done

| Date | What |
|---|---|
| 3 Nov | Both repositories transferred to `iainchesworthlabs`, names unchanged, library first. |
| 3 Nov | The redesigned GUI (32 commits) pushed to the studio's `develop`. |
| 3 Nov | Library `main` merged into `develop` (pull request 2, merge commit). `main` is now an ancestor of `develop`; the tag `v0.1.0-beta.1` is unchanged. |

## Decisions

| Topic | Decision |
|---|---|
| Route for the library | A new Boost library, starting with a preliminary discussion on the Boost developers list. A contribution to Asio may follow. Until a review manager answers, the library stays third-party. |
| Names | `virtual-serial` and `virtual-serial-studio`. No rename. |
| Library branches | `main` and `develop`. Trunk development on `develop`; a release is a tag on a `develop` commit and `main` fast-forwards to it. `main` is renamed to `master` only if the Boost review is accepted. |
| Studio branches | Standard D1: `develop` is the trunk, `main` holds releases, squash merges through the merge queue. |
| Licences | Library: BSL-1.0, with a `REUSE.toml` that annotates existing files. Studio: GPL-3.0-or-later; the files derived from the library keep BSL-1.0 and are recorded in `THIRD_PARTY_NOTICES.md`. |
| Commit policy | Shared text policy in both repositories. The shared check allows a human `Signed-off-by` and does not require one, in both repositories; `CONTRIBUTING.md` asks for it. The old checkers (`tools/check_commit_message.py`, the hard-coded author address) are removed. |
| Naming conventions | Both keep their own. Studio: `snake_case` for functions and types; the rules are written into `.clang-tidy` and the inconsistencies fixed. |
| Platforms | Windows, Linux and macOS only. The label set has no iOS, Android or WebAssembly entries. A platform declaration for desktop-only projects is added to the standard. |
| Test framework | The in-tree framework stays. Coverage gates (90% lines, 80% branches) stay and a ratchet floor is added. |
| GUI tests | Journey tests written with `QTest` drive the core features offscreen, one per feature, named for the feature. |
| Preprocessor rule | Platform code lives in `src/platform/<os>/` and the hard rule (no platform, compiler or library-version `#if`) has no violations. `tools/preprocessor-baseline.json` records the other directives per file, mostly include guards and feature switches, and the policy job fails if a file gains any. |
| Dependencies | Boost headers and Tracy through vcpkg in the organisation fork. The library through a vcpkg port in the fork and FetchContent of a release tag; `VSS_VIRTUAL_SERIAL_DIR` stays as the local override. The deploy key, the repository variable and the sibling-folder default go. |
| Compilers | The standard's floor (D7): GCC 16, Clang 23 and MSVC 19.51 and 19.52, pinned in `.github/toolchain-versions.json`, with a nightly leg on the newest release of each. The pull request gate builds with the floor versions. The legs use the shared `setup-gcc`, `setup-llvm` and `setup-msvc-env` actions, through `.github/actions/setup-toolchain`. `.github/workflows/nightly.yml` builds GCC and Clang at the `gcc_newest` and `llvm_newest` pins, which equal the floor until a newer major is released. MSVC and Apple Clang are the newest the runners carry. |
| Qt | Qt 6.12 is the minimum, pinned once in `.github/toolchain-versions.json` and installed by the shared Qt setup. The CI job that builds the GUI uses the pin. |
| Languages | `en`, `fr`, `es`, `de`, `he`, `yi`, `ja`, `zh_CN` (D3) and `ar` as a ninth. Right-to-left support (`he`, `yi`, `ar`) is on by default. Catalogues other than English are drafts until a fluent reader has been through them, and the documentation says so. |
| Bundle identifier | `com.iainchesworthlabs.virtualserialstudio`. |
| Releases | Studio: first pre-release `v0.1.0-beta.1`, then release candidates. Library: next pre-release `v0.1.0-beta.2`. The macOS build has not been run on real hardware; the release notes and `docs/platforms.md` say so. |
| Network job | The studio gets a listening backend in `vss_ports` (Boost.Asio directly). The library is not changed for it. |
| CI tiers | Standard 11.2 and 11.5, planned by `tools/ci/plan_legs.py` and `.github/ci/plan.json`. A ready pull request runs Linux GCC, ASan/UBSan and the fuzz replay (TSan when threading code changed). The queue adds Windows MSVC, TSan, the Qt GUI build, the performance floors (library code changed), macOS (platform, threading or CMake changes) and the Windows driver (driver changes). After the merge: Linux Clang, macOS, coverage, Tracy and the driver, one run at a time. Differences from the standard: the Linux Clang release build is post-merge only, because the Clang ASan/UBSan leg already builds the code with Clang's warnings as errors on the pull request; the coverage gates (90% lines, 80% branches) are checked after the merge and no threshold changed; the driver is built on every push to develop because the signed packages are taken from those runs; the `deep` label is not used (a `labeled` trigger would re-run the gate on every label change), `gh workflow run ci.yml --ref <branch>` does the same. |
| ccache | Every compile job restores ccache through the shared `setup-ccache` action. Only a run on develop that is not a push saves it (`.github/actions/save-ccache`), and the nightly `ccache-seed` job compiles each preset the gate, the queue and the post-merge run use. A cache saved from a pull request or a queue ref could be restored by nothing else and would fill the 10 GB quota, which is also why `cache-hygiene.yml` exists. The Windows presets set `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` (`/Z7`), because MSVC does not cache otherwise. |
| Disabled capabilities | Faults, benchmark, line matrix, peer open, rename, undo, JSON positions and the network server are built. |
| Design material | Kept in `docs/design/` (tokens and style guides). Review evidence and build-time records are removed. |
| Windows driver | Named `vsscom`. KMDF 1.27. A phased plan: P0 repairs the INF and registers COM names, P1 adds paired ports and the hub end, P2 adds ports on demand, P3 hardens. A user-mode (UMDF) spike follows P0. Packages are test-signed in CI with the organisation certificate. Production signing (Microsoft attestation) is a later step. |
| Linux and macOS ports | Pseudo terminals stay the default, with stable links and a headless `vss_cli vport` command. Kernel options are documented and not built. |
| Test machines | Windows 10 and Windows 11 guests and an Ubuntu 26.04 guest in VMware, rebuilt from ISO. GitHub-hosted runners for Ubuntu, Windows and macOS. There is no macOS hardware; macOS is checked in CI only. |

## Order of pull requests

Studio:

1. This page, plus `.github/repo-settings.json` and `.github/labels.json`.
2. The Qt GUI CI job (the `qtsvg` package does not exist for Qt 6.8.3).
3. Foundations: vcpkg fork baseline, library dependency, vendored `cmake/icl`, toolchain pins, preset names, version rule.
4. Conventions: `.clang-tidy`, `.editorconfig`, `AGENTS.md`.
5. Compliance: `REUSE.toml`, SPDX lines, DCO, `SECURITY.md`, `CONTRIBUTING.md`, the About dialog.
6. CI: shared workflows, planner and aggregator, merge queue triggers, sanitizer legs, CodeQL, zizmor, Dependabot.
7. Test work: GUI journey tests in CI, fuzz harnesses for the topology parser and the endpoint URI parser.
8. Release tooling and packaging.
9. Documentation site and `docs/platforms.md`; languages and right-to-left.
10. Design fixes, the listening backend and the capability backlog, one pull request each.
11. Virtual ports: Windows driver phases, Linux and macOS links and `vport`.

Library: settings, commit policy, CI structure, REUSE, release flow, community files, then the Boost readiness work in the order agreed from the readiness plan. Anything that changes a header, a name or behaviour is agreed first.

Organisation side, in this order: settings part 1 (repository settings, security, environments, labels, variables), the CI pull requests, then settings part 2 (Actions SHA pinning, rulesets, Pages).

## Owner actions

- Run `admin/apply.sh` for each repository: dry run first, then `--apply`.
- Delete the obsolete deploy key and the `VIRTUAL_SERIAL_DEPLOY_KEY` secret after the studio no longer uses them.
- Confirm that a Dependabot pull request reaches the merge queue with `MERGE_QUEUE_TOKEN`. Until then it is unverified.
- Create the `release` environment reviewer and approve each release deployment.
- Create release tags.
- Provide the public certificate chain for the Windows test-signing leaf (done: root and intermediate PEM files).

## TODO(owner)

- TODO(owner): copyright holder name and public contact address for the library's headers and `meta/libraries.json`.
- TODO(owner): whether the library documentation says anything about the tools used to write it.

## Changes to the standard this work raises

- D2: a library meant for another ecosystem keeps that ecosystem's licence.
- Branch, tag and release rules: an exception for a library in an ecosystem whose release scripts create tags.
- A platform declaration for desktop-only projects (sections 10 and 18).
- Rule 6.2: a Widgets form, with `QTest` journey tests run offscreen.
- `sign-windows`: driver files (`.sys`, `.cat`) and `rename: false`.
