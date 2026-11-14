# Third-party notices

Virtual Serial Studio itself is licensed under the **GNU General Public
License, version 3 or later** (see [LICENSE](LICENSE)). The components below
keep their own licences. Every licence listed here is compatible with
redistributing the combined application under GPL-3.0-or-later.

## Linked or built against

| Component | Version | Licence | How it is used |
|---|---|---|---|
| [Boost.VirtualSerial](https://github.com/iainchesworthlabs/virtual-serial) | v0.1.0-beta.2 | Boost Software License 1.0 | Header-only; the serial transport, backends and mock fixture behind every endpoint |
| [Boost](https://boost.org) | 1.92 headers | Boost Software License 1.0 | Asio, System, JSON |
| [Qt](https://qt.io) | 6.10.3 | LGPL-3.0-only OR GPL-3.0-only OR commercial | The GUI; linked dynamically |
| [Monocypher](https://monocypher.org) | 4.0.2 | BSD-2-Clause OR CC0-1.0 | Vendored unchanged in `third_party/monocypher` and compiled into the program; the Ed25519 signature check behind licence files (`libs/vss_license`) |
| [Tracy](https://github.com/wolfpld/tracy) | 0.14.1 (vcpkg) | BSD-3-Clause | Optional profiling client, compiled in when the build finds the vcpkg package (`VSS_TRACY`, on by default in vcpkg builds; the release packages are not built with vcpkg, so they do not contain it), built from an overlay of the vcpkg port (`cmake/vcpkg-ports/tracy`, MIT, adds the manual-lifetime and no-broadcast features); the capture and CSV tools are built from source in CI |

BSL-1.0 is a permissive licence whose only requirement is to keep the
copyright notice; it may be combined with GPLv3 code. Qt is linked dynamically,
so the LGPL relinking requirement is satisfied by the dynamic link, and a
GPL-3.0-or-later application is one of the uses Qt's own GPLv3 option permits.

## Optional external components

| Component | Licence | How it is used |
|---|---|---|
| [com0com](https://sourceforge.net/projects/com0com/) | GPL (as distributed by the project) | An optional Windows kernel driver. The application detects installed pairs, opens their COM ports, and may invoke `setupc.exe` (with elevation) to create a pair. Nothing from com0com is linked into this application. |
| [Project Wycheproof](https://github.com/C2SP/wycheproof) test vectors | Apache-2.0 | `tests/data/wycheproof/ed25519_test.json`, unchanged. Read by the unit tests only; not distributed with the application. |
| CMake, Ninja, LLVM/Clang | BSD-3-Clause, Apache-2.0, Apache-2.0 with LLVM exception | Build and coverage tooling only; not distributed with the application. |

## Files derived from the Boost.VirtualSerial repository

These files were taken from the library repository and remain under the Boost
Software License 1.0. They carry their own SPDX identifiers and are **not**
relicensed by this project:

| File | Licence |
|---|---|
| `tools/fetch_boost.py` | BSL-1.0 |
| `tools/changelog.py` | BSL-1.0 |
| `tools/test_changelog.py` | BSL-1.0 |
| `tools/pty_loopback.py` | BSL-1.0 |
| `changelog.d/README.md` | BSL-1.0 |

The table above is the component list of the program. `third_party.json` holds the same list in
machine-readable form and is what the **About, Licences** tab shows (Tracy only in a build that has the
profiler); a unit test fails when the two disagree or when a vcpkg dependency is missing. A component that is
not in the package does not belong in that table; it goes under "Optional external components".

The BSL-1.0 text is kept at [LICENSES/BSL-1.0.txt](LICENSES/BSL-1.0.txt).

## Notes on GPL compliance

* Each source file starts with an SPDX licence line naming GPL-3.0-or-later,
  except the derived files listed above and the machine-readable configuration
  files where a comment is not possible.
* The GUI's **Help → About** box and `vss_cli --help` display the copyright,
  the licence and the absence of warranty, as section 5(d) expects of
  interactive interfaces.
* If you convey binaries, section 6 requires you to offer the Corresponding
  Source. Building from this repository produces it.
