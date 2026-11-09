# Releasing

A release is a `vMAJOR.MINOR.PATCH` tag on a `develop` commit, such as `v0.1.0`
or `v0.1.0-rc.1`. A tag with a suffix is a pre-release. The standard that this
follows is section 13 of `docs/standard.md` in `icl-common-infra`.

## What a release holds

`.github/workflows/release.yml` builds one package for each system and the
shared publish workflow adds the checks.

| Package | Built on | Holds |
|---|---|---|
| `virtual-serial-studio-<tag>-linux-x86_64.tar.gz` | Ubuntu 26.04 | `bin/vss_gui`, `bin/vss_cli`, the Qt libraries in `lib/` and `plugins/`, the licence texts. Needs glibc 2.42 or newer |
| `virtual-serial-studio_<version>-1_amd64.deb` | Ubuntu 26.04 | The same programs and Qt libraries in `/opt/virtual-serial-studio`, links in `/usr/bin`, a launcher entry, an icon, the licence texts in `/usr/share/doc/virtual-serial-studio`. For Debian and Ubuntu; see [Linux packages](#linux-packages) |
| `virtual-serial-studio-<version>-<release>.x86_64.rpm` | Ubuntu 26.04 | The same files, for Fedora and Red Hat |
| `virtual-serial-studio-<tag>-macos-arm64.tar.gz` | macOS 26 | `vss_gui.app`, `bin/vss_cli`, the licence texts |
| `virtual-serial-studio-<tag>-windows-x86_64.zip` | Windows 2025 | `bin/vss_gui.exe`, `bin/vss_cli.exe`, the Qt DLLs, the licence texts |

The release also has `SHA512SUMS`, a detached GPG signature (`.asc`) for every
file, an SPDX software bill of materials and a build provenance attestation for
every package. The libraries and headers (the `development` install component)
are not in the packages.

## Signing

| System | State |
|---|---|
| Windows | The `SIGN_WINDOWS` repository variable chooses: `false` (the default) leaves the files unsigned and names the package `-unsigned`, `test` signs with a leaf certificate from the owner's own certificate authority (`-testsigned`), and `production` is reserved for the signing service that has not been chosen yet. `vss_gui.exe`, `vss_cli.exe` and any `vss_*.dll` are signed before the zip is made and the signatures are verified; a run without the secrets leaves them unsigned and says so in the job summary |
| macOS | Not signed or notarised yet. Gatekeeper asks the user to allow the app. The owner signs and notarises with `tools/macos/sign-and-notarize.sh` (below) |
| Linux | Not signed beyond the GPG signature on the file |

A test-signed Windows package is trusted only on a machine that has the owner's
certificate authority installed. The release notes say so.

## Before you tag

`python3 tools/release_preflight.py --tag v0.1.0-rc.1` reads these conditions from
GitHub and the working tree and lists the ones that are not met. It changes nothing
and exits with 1 until all are met, so run it again after each fix.

| Condition | How it is checked |
|---|---|
| The tag matches `CMakeLists.txt` and `vcpkg.json` | `tools/check_release_version.py` |
| No open code-scanning alert in first-party paths | `gh api "repos/{owner}/{repo}/code-scanning/alerts?state=open&ref=refs/heads/develop"`; paths under `.cache/`, `build/`, `_deps/` and `vcpkg_installed/` are third-party code and do not count |
| No open issue labelled `nightly-analysis` | `gh issue list --label nightly-analysis` |
| CI, Nightly, Platform matrix and Docs are green | The latest completed run of each on `develop` (Docs runs for pull requests and `main`, so its latest run on any branch) |
| The Release dry run is green | The latest completed `release.yml` run on `develop`. The Monday schedule runs on `develop`, the default branch, so this is its status; `gh workflow run release.yml --ref develop` makes one now |
| `docs/signing-key.asc` and `SECURITY.md` are in the repository | The publish job fails after signing when the key is missing |
| The signing secrets exist | `REPO_GPG_PRIVATE_KEY`, `REPO_GPG_PASSPHRASE`, `RELEASE_BOT_APP_ID` and `RELEASE_BOT_PRIVATE_KEY`, plus the two `WIN_TESTSIGN_*` secrets while `SIGN_WINDOWS` is `test` |
| The `release` environment has a required reviewer | The owner approves the publish job there |

Make the first release a release candidate, `v0.1.0-rc.1`. It runs the whole publish
path (checksums, signing, attestation, the GitHub Release) as a pre-release, takes its
notes from the pending fragments, needs no change to `CHANGELOG.md` and does not move
`main`. Tag `v0.1.0` after the candidate has been unpacked and tried on each system.

## Making a release

1. Raise the version in `CMakeLists.txt` (`project(VERSION)`) and `vcpkg.json`
   in a pull request. `tools/check_release_version.py` fails the release when
   the tag and these two disagree.
2. Run `python3 tools/changelog.py --release <version>` to fold the fragments
   in `changelog.d/` into `CHANGELOG.md` for a final release, in the same pull
   request. A pre-release takes its notes from the pending fragments.
3. Tag the merged commit on `develop` and push the tag. The `package` jobs
   build, the owner approves the `release` environment, and the `publish` job
   signs, attests and publishes. For a final release it then moves `main` to
   the tag.

## Trying it first

`gh workflow run release.yml --ref develop` builds every package and keeps them
as workflow artifacts without publishing. The same run happens every Monday and
on a pull request that changes the workflow, so a change to the install rules
cannot go unseen for long. The Monday run is on `develop`, so it is the status of
the packaging jobs for that branch. Unpack each package on its system and run
`vss_cli selftest`; start `vss_gui` on a machine with a display.

## What the owner provides

These are not in the repository and an agent does not create them.

- `docs/signing-key.asc`, the public GPG key whose private half is the
  organisation secret `REPO_GPG_PRIVATE_KEY`, and its fingerprint in
  `SECURITY.md` with the commands to verify a download. Both are in the
  repository now and hold the organisation's release key (the same file as in
  the other repositories). The publish job checks every signature against it, so
  if the secret is ever a different key the run fails; the owner then updates both.
- The organisation secrets `REPO_GPG_PASSPHRASE`, `RELEASE_BOT_APP_ID` and
  `RELEASE_BOT_PRIVATE_KEY`.
- The `release` environment with the owner as required reviewer.
- For a test-signed Windows package: `SIGN_WINDOWS=test` and the secrets
  `WIN_TESTSIGN_PFX_BASE64` and `WIN_TESTSIGN_PFX_PASSWORD`.

## Linux packages

`cmake/packaging/VssPackage-linux.cmake` sets up CPack (`-DVSS_PACKAGE=ON`; the
shared settings are in `cmake/VssPackage.cmake`) and the release workflow runs it
after the archive step. The package is named `virtual-serial-studio` and takes its
version from `project(VERSION)`. The tag decides the rest:

| Tag | deb version | rpm version and release |
|---|---|---|
| `v0.1.0` | `0.1.0-1` | `0.1.0-1` |
| `v0.1.0-rc.1` | `0.1.0~rc.1-1` | `0.1.0-0.rc.1` |

A tilde sorts before everything, so `0.1.0~rc.1` upgrades to `0.1.0`; an rpm release
that starts with 0 sorts before the 1 of the final release. The workflow passes the
label as `-DVSS_PACKAGE_PRERELEASE=rc.1`.

**Where the files go.** The programs, the Qt libraries and the Qt plug-ins are the
`runtime` install component, in `/opt/virtual-serial-studio` exactly as in the
`.tar.gz`, and `/usr/bin/vss_gui` and `/usr/bin/vss_cli` are links to them. A tree
under `/usr` would put `libQt6*.so*` in `/usr/lib`, where it would clash with the
distribution's own Qt packages and shadow them for other programs. The packages
carry the tree the release job has already run, with the run path
`$ORIGIN/../lib`, which the loader resolves from the real file, so the links work.
The launcher entry, the icon and the licence texts are the `integration` component.

**Qt is bundled, not a dependency.** `vss_gui` is built and tested with the Qt kit
pinned in `.github/toolchain-versions.json`, which is newer than the Qt in the
distributions the packages are for. Depending on the distribution's Qt would run
something the release job did not test. The cost is size, and that a Qt fix reaches
users with a new Studio package, not with the distribution's update.

**Dependencies.** The packages depend on what they do not carry: glibc 2.42 or newer
(the same floor as the `check-glibc-floor` step), `libstdc++` from GCC 16, and the
system libraries that Qt, its `xcb` plug-in and its image and font code load, with
the xcb libraries listed under Known gaps. `pkexec` (Debian: `pkexec`, Red Hat:
`polkit`) is recommended: Studio uses it to offer to add the user to the serial group.
Studio needs no udev rule and no group of its own. A pseudo terminal needs no
permission, and a real serial device is opened with the group that already owns it
(`dialout` on Debian, Ubuntu and Fedora); the setup check tells the user, so the
package does not change any account.

**What the workflow checks.** `tools/ci/check_linux_packages.sh` prints the metadata,
checks the version against the tag, then installs the deb in an Ubuntu 26.04 container
and the rpm in a Fedora 44 container (both with glibc 2.42 or newer and GCC 16, the
oldest systems the packages claim), runs `vss_cli selftest`, runs `ldd` on the programs,
the Qt libraries and the plug-ins Studio uses to find a missing dependency, validates the launcher
entry, starts `vss_gui --version` under Xvfb (which loads the `xcb` plug-in), and
removes the package. Run it on a machine with docker:

```
tools/ci/check_linux_packages.sh <directory with the .deb and .rpm> v0.1.0-rc.1
```

## Known gaps

- The Linux package needs glibc 2.42 or newer, which is Ubuntu 26.04 or later.
  Boost.Asio's serial port code calls `cfsetspeed`, and glibc 2.42 gave that
  function a new symbol version, so a program built there will not start on
  Ubuntu 24.04 (glibc 2.39). The release workflow passes `allowed: "2.42"` to
  the `check-glibc-floor` action for this reason. The package also relies on the
  system's libstdc++ from GCC 16. Building on an older system, or linking the C++
  runtime statically, would lower the floor; neither is done yet. The deb and rpm
  declare the same floors (glibc 2.42, libstdc++ 16), so a system below them
  refuses to install them.
- The Linux package carries the `xcb` Qt platform plugin only, and that plugin needs
  `libxcb-cursor0` and the other xcb libraries from the system. On Ubuntu:
  `sudo apt install libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1
  libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxkbcommon-x11-0`.
  Without them `vss_gui` stops at start with "Could not load the Qt platform plugin
  xcb". There is no Wayland plugin.
  The platform matrix (T9) found this on a machine without them.
  The deb and rpm depend on these libraries, so installing the package installs
  them; the command above is for the `.tar.gz` only.
- On Windows `vss_gui.exe` belongs to the Windows subsystem, so it opens no console window. Started from a
  terminal it attaches to that terminal and prints `--version`, `--help` and `--selftest` there. `cmd` and
  PowerShell do not wait for such a program: use `start /wait` or `Start-Process -Wait` when the exit code
  matters. The `gui_subsystem` test checks the subsystem of the built program.
- The Windows and macOS packages have no installer. Unpack them where you like. Linux has a deb
  and an rpm (above); the Linux `.tar.gz` stays for other distributions.

## macOS signing and notarisation

Gatekeeper opens a download without a warning only when the app is signed with a
Developer ID Application certificate, built with the hardened runtime, notarised by
Apple and stapled. The pieces are in the repository; the identity is not.

- `apps/vss_gui/macos/vss_gui.entitlements`: the entitlements of the notarised,
  non-sandboxed release. It is empty on purpose (Studio needs no hardened-runtime
  exception). Add a key only with a reason beside it.
- `apps/vss_gui/macos/vss_gui.sandbox.entitlements`: the starting point for a
  sandboxed build (serial devices, USB, network client and server, files the user
  picks). The release does not use it.
- `apps/vss_gui/macos/Info.plist.in`: the bundle's property list, with
  `NSLocalNetworkUsageDescription` so macOS 15 can ask for the Local Network
  permission.
- `tools/macos/sign-and-notarize.sh --app vss_gui.app --cli bin/vss_cli`: signs every
  nested library, plug-in and framework, then the app, with `--options runtime` and a
  timestamp; checks the signature; sends a zip to `xcrun notarytool`; staples the
  ticket; asks `spctl` to assess the app. `--dry-run` prints the commands and runs
  none.

The script reads `VSS_SIGN_IDENTITY` (the certificate name in the keychain) and
`VSS_NOTARY_PROFILE` (a profile saved once with `xcrun notarytool
store-credentials`). Neither is in the repository, a workflow does not hold them
yet, and an agent does not create them. Making the Release workflow call the script
needs the owner to add the certificate and the profile as organisation secrets.
