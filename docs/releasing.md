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
| `virtual-serial-studio-<tag>-macos-arm64.dmg` | macOS 26 | A disk image with `vss_gui.app`, an `Applications` link to drag it onto, `bin/vss_cli` and the licence texts. The same files as the tar.gz, made by CPack (below) |
| `virtual-serial-studio-<tag>-windows-x86_64.zip` | Windows 2025 | `bin/vss_gui.exe`, `bin/vss_cli.exe`, the Qt DLLs, the licence texts, and in `bin/driver` the user-mode COM port driver, its helper service and the set-up scripts |
| `virtual-serial-studio-<tag>-windows-x86_64.exe` | Windows 2025 | The same files as the zip, as an NSIS installer that also sets up the driver and the helper service ([below](#the-windows-installer)) |

The release also has `SHA512SUMS`, a detached GPG signature (`.asc`) for every
file, an SPDX software bill of materials and a build provenance attestation for
every package. The libraries and headers (the `development` install component)
are not in the packages.

## Signing

| System | State |
|---|---|
| Windows | The `SIGN_WINDOWS` repository variable chooses: `false` (the default) leaves the files unsigned and names the package `-unsigned`, `test` signs with a leaf certificate from the owner's own certificate authority (`-testsigned`), and `production` is reserved for the signing service that has not been chosen yet. `vss_gui.exe`, `vss_cli.exe` and any `vss_*.dll` are signed before the zip and the installer are made, as are the driver files in `bin/driver` (`vsscom_um.dll`, its catalog and `vsscom_helper.exe`, signed by the same action the `driver` job of `ci.yml` uses). The installer is built from those signed files, and then the installer `.exe` itself is signed. Every signature is verified; a run without the secrets leaves them all unsigned and says so in the job summary. The `Uninstall.exe` that the installer writes on the machine is not signed ([the installer](#the-windows-installer)) |
| macOS | Not signed or notarised yet, so the `.dmg` and the tar.gz are unsigned and Gatekeeper asks the user to allow the app (see "Installing on macOS"). The `SIGN_MACOS` repository variable turns signing on once the owner has added the certificate and notary secrets (below); until then the job summary says the package is unsigned |
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

## Where `main` fits

All work happens on `develop`. `main` holds only released code: it does not move
until a final release, when the publish job fast-forwards it to the tag, so
`main` always names the latest release. A release candidate (a tag with a
suffix) never moves it. Nobody commits or opens a pull request into `main`, and a
fresh branch or session starts from `develop`, not `main`.

## The release notes

The page of a release has the layout of the established releases of the other
projects in this organisation, so a reader sees the same thing every time:

1. **A heading and a summary.** `## What's Changed since v<previous>` (leave out
   "since" for the first release), then two or three sentences that say what the
   release is for in the user's words: the biggest things first, what is new, what
   changed, what a user should know before installing.
2. **The changelog section**, as `### Added`, `### Changed`, `### Fixed` and
   `### Security` lists, copied from `CHANGELOG.md` (for a release candidate, from
   `python3 tools/changelog.py --dry-run`). Each entry is a bold headline and a
   plain-language explanation (`changelog.d/README.md`). Relative links are
   rewritten to the tagged files.
3. **`## Install`**: a table of the file for each system and how to install it
   (the installer, then the archive), what is signed and what is not, the
   machine requirements (glibc 2.42 on Linux; the Windows driver and what to do
   when it is not trusted; Gatekeeper on macOS), and how to check a download
   (`SHA512SUMS`, the `.asc` signatures and `gh attestation verify`, with a link to
   `SECURITY.md`).
4. **`**Full Changelog**: …/compare/v<previous>...v<this>`** (leave out for the first
   release).
5. **For a pre-release, a closing block quote** that says what has been tested on
   which system and what has not, and asks for reports.

The publish job writes the section from the changelog (item 2). Writing the
summary, the install section and the closing note is a step of the release, done
after the publish job has run and before the release is announced:

```bash
gh release view v0.1.0 --json body --jq .body > notes.md   # edit: add 1, 3, 4, 5
gh release edit v0.1.0 --notes-file notes.md
```

Read the page the way a user does before you tell anyone about it. Anything a
user would not recognise (a work-package name, a CI job, a test count) belongs in
a pull request, not in the notes.

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
- For a signed and notarised macOS package: the Developer ID certificate, a notary API key,
  and the variables and secrets listed under "macOS signing and notarisation".

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

**Drivers and system integration: nothing is installed beyond the files.** Windows needs
a driver; Linux does not, and the packages add no kernel module, DKMS source, udev rule,
`tmpfiles.d` entry, polkit action, group change or maintainer script, so there is nothing
to remove on uninstall or purge beyond the files the package owns. Each part was checked
in the code:

| Part | Conclusion |
|---|---|
| Virtual ports | A pseudo terminal from `/dev/ptmx`, which any user may open. No kernel module |
| Link directory (`stable_link.cpp`) | `$VSS_VPORT_DIR`, then `$XDG_RUNTIME_DIR/vss` (the normal case in a session), then `/run/vss`, then `/tmp/vss`. Studio makes the directory itself with mode 0700 and refuses one that another user owns, so a root-owned `/run/vss` from a `tmpfiles.d` entry would be refused and is not shipped. A user without a session runtime directory gets `/tmp/vss` |
| Serial devices | Opened with the group that already owns the device (`dialout` on Debian, Ubuntu and Fedora, `uucp` on Arch). The distributions ship the udev rules that give a device that group, so Studio adds none |
| The group step | The Setup check shows `sudo usermod -aG dialout <user>` and, when `pkexec` and `usermod` exist, a button that runs it through `pkexec`, which uses polkit's own `exec` action, so Studio ships no action file. The packages recommend `pkexec` (deb) or suggest `polkit` (rpm) |
| A post-install hint about the group | Not added. The script runs as root, without knowing which user will run Studio, and a package manager prints it to nobody; the Setup check names the exact command for the person who needs it |
| AppArmor and SELinux | The Setup check prints the lines to add when a profile blocks Studio. The packages ship no profile or policy. A Fedora system with SELinux enforcing was not tried: the container checks run without it, so the labels files under `/opt` get by default are unchecked |

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
- The macOS disk image is unsigned until the owner supplies the certificate and the notary credentials. Gatekeeper
  blocks an unsigned app that was downloaded; "Installing on macOS" says how to allow it.
- The macOS image is arm64 only. There is no Intel build.

## Installing on macOS

Open the `.dmg`, drag `vss_gui.app` onto the `Applications` link in the window and eject
the image. The image also holds the command line tool, `bin/vss_cli`. It is not inside the
app on purpose: macdeployqt seals the app bundle (and the signing script seals it again),
and a file added afterwards breaks the seal, which stops the app opening on Apple silicon.
To put the tool on `PATH`, copy it out of the mounted image:

```bash
sudo install -m 755 "/Volumes/Virtual Serial Studio/bin/vss_cli" /usr/local/bin/vss_cli
```

A second drag target would have meant a loose, unsigned executable the user has to find, and
an installer package (`.pkg`) would have needed its own Developer ID Installer certificate
and a second notarisation. The tar.gz holds the same files for a user who prefers it.

**The image is not signed yet.** Until it is, macOS treats the app as from an unidentified
developer. The first time, Gatekeeper refuses to open it ("cannot be opened because the
developer cannot be verified"). Try to open it once, then open System Settings, Privacy &
Security, and choose Open Anyway beside the message about `vss_gui` (on macOS 14 and earlier,
Control-click the app, choose Open, then Open again). Removing the quarantine flag also works:

```bash
xattr -dr com.apple.quarantine /Applications/vss_gui.app
```

The Release workflow builds the image on a macOS 26 runner with CPack's DragNDrop
generator. The settings are in `cmake/packaging/VssPackage-darwin.cmake`, chosen by
`cmake/VssPackage.cmake` from the system name (each system has its own
`cmake/packaging/VssPackage-<system>.cmake`). A developer can make one from a macOS build directory with
`cpack --config build/macos-release-gui/CPackConfig.cmake -G DragNDrop`. The job then mounts
the image with `hdiutil attach`, runs `vss_cli selftest` and `vss_gui --version` from the
mounted volume and detaches it, and uploads the image with the other packages.

### What macOS needs besides the app

Nothing. The image installs no driver, kernel extension, system extension, launch daemon
or privileged helper, so there is nothing to uninstall: drag `vss_gui.app` (and `vss_cli`,
if it was copied) to the Bin. The Windows package differs because Windows has a COM port
driver; the reasons for each item on macOS are:

| Item | Needed? | Why |
|---|---|---|
| Virtual ports | No driver | They are pseudo terminals (`/dev/ttysNNN`) opened with `openpty`, which any user process may do. A SerialDriverKit driver would make ports that programs listing ports through IOKit could see, but it needs an Apple entitlement and is not planned (`posix-virtual-ports.md`) |
| Stable links, `/tmp/vss` | No installer step | `vss_cli vport` creates the directory when it runs, with mode 0700, owned by the user. macOS empties `/tmp` at restart, and the links go with the process anyway |
| Serial devices, `/dev/cu.*` | No installer step | The notarised, non-sandboxed app opens them as the user does, with no prompt. Only a sandboxed build needs the serial and USB entitlements in `vss_gui.sandbox.entitlements`, and the release is not sandboxed |
| Local Network | Already covered | `NSLocalNetworkUsageDescription` is in `Info.plist.in`. macOS 15 and later asks once, the first time a network endpoint is used. The Application Firewall asks once about incoming connections for a listener |
| `vss_cli` on `PATH` | Optional, by hand | The image does not change `PATH`. The `install` command under "Installing on macOS" puts the tool in `/usr/local/bin` |

This comes from reading the code and `docs/platforms.md`; it has not been tried on a Mac.
A third-party program that offers a list of ports will not list a virtual one; one given
the path (`/dev/ttysNNN` or a link in `/tmp/vss`) works.

## The Windows installer

`cmake/packaging/VssPackage-windows.cmake` configures CPack's NSIS generator, and
`cmake/packaging/windows/driver.nsh` is the part that handles the driver. The installer
holds Studio, the command line tool and the virtual COM port driver with its helper
service.

**How it is made.** In the Windows leg of `release.yml`:

1. The programs are built and installed into `stage`.
2. `.github/actions/build-windows-driver` builds and signs the user-mode driver
   (`vsscom_um.dll`, then its catalog, then `vsscom_helper.exe`), the same action the
   `driver` job of `ci.yml` uses, in the same `SIGN_WINDOWS` mode.
3. `drivers/windows/stage-package.ps1` puts the driver package and its scripts in
   `stage/bin/driver`, where Studio's "Set up Virtual Serial Studio" looks for them
   (`setup-driver.ps1`, `remove-driver.ps1`, `driver/`, `helper/`, `trust/`). They are in
   the zip as well.
4. The programs are signed, and `cpack -G NSIS` packs that signed tree
   (`CPACK_INSTALLED_DIRECTORIES` names it, and an empty `CPACK_INSTALL_CMAKE_PROJECTS`
   stops CPack installing unsigned copies over it). The installer `.exe` is then signed
   and verified.
5. On the runner the installer is run silently twice (the first with `/ADDTOPATH=1`), the
   second time as an upgrade, then
   `vss_cli selftest` and `vss_gui --version` run from `%ProgramFiles%\Virtual Serial Studio`,
   and a silent uninstall must leave no folder, Apps & features entry, Start menu folder,
   helper service or driver package, and must give the machine PATH back as it was. The
   check also requires the folder on the PATH exactly once after the upgrade. Before the
   upgrade and before the uninstall it starts `vss_cli virtual` from the installed folder
   and requires that the installer closed it, and before the uninstall it makes a start at
   sign-in entry and a running setup for the runner's user and requires the entry gone and
   the setup marked as stopped afterwards.

NSIS comes from Chocolatey through the `setup-toolchain` action, at the version in
`.github/toolchain-versions.json`.

**What it does.**

- Installs for all users into `%ProgramFiles%\Virtual Serial Studio` and asks for
  administrator rights once, when it starts. There is **no per-user mode**: CPack's NSIS
  template cannot offer both, the Start menu and PATH entries would differ between them,
  and the driver is per machine. The zip is the way to run Studio without administrator
  rights.
- Shows the GPL text on the licence page, adds a Start menu folder with **Virtual Serial
  Studio** and **Uninstall**, and registers an uninstaller under Apps & features. The entry
  is in the 32-bit registry view (`HKLM\Software\WOW6432Node\...\Uninstall\VirtualSerialStudio`)
  because the NSIS program is a 32-bit one.
- Removes an earlier version before it installs: the registry key and the folder carry no
  version, so any version finds any other.
- Offers, on a page whose box is not ticked, to put `bin` on the system PATH so that
  `vss_cli` runs from any terminal. `bin` also holds the Qt DLLs, which is why it is not
  the default. A silent install (`/S`) adds nothing unless it is given `/ADDTOPATH=1`, and
  `/D=<folder>` installs elsewhere. CPack's own PATH code is switched off
  (`CPACK_NSIS_MODIFY_PATH OFF`): it keeps the PATH in an NSIS string of at most 1024
  characters, refuses a longer PATH with "PATH too long installer unable to modify PATH!"
  and, on the way out, can cut a long PATH short. `bin\edit-path.ps1` edits the registry
  value itself instead, so any length works, every other entry stays as written, a folder
  already there is not added twice, and a PATH that would pass Windows' 32767-character limit
  is left alone and reported. The uninstaller takes the folder out again (an upgrade leaves
  it). `tools/test_edit_path.py` tests the script against a scratch registry key.
- Has a page with a ticked box, **Install the virtual COM port driver and its helper
  service**. A silent install does the same unless it is given `/DRIVER=0`.

**Which driver, and why the user-mode one.** The installer ships `vsscom_um`, the user-mode
driver, with `vsscom_helper`. The kernel-mode driver needs a Microsoft signature to load
without test signing and nothing signs it yet, so shipping it would be an installer that
cannot work on an ordinary machine. The user-mode driver needs only a catalog signed by a
publisher the machine trusts, no kernel-mode signature and no test signing, and it is the
one Studio's own set-up flow and its helper service are built for. The kernel-mode package
is still built and uploaded by the `driver` job of `ci.yml` for the guests.

**What the driver step does.** After the files are copied the installer runs
`bin\driver\setup-driver.ps1` (the script behind "Set up Virtual Serial Studio") with its
output in `<install folder>\driver-setup.log`, and records the result in the Apps &
features key as the value `VssDriver`:

| `VssDriver` | Meaning |
|---|---|
| `installed` | The driver package is in the driver store and the helper service runs |
| `installed-restart-needed` | The same, and Windows asks for a restart (the installer sets the restart flag) |
| `skipped` | The box was cleared, or `/DRIVER=0` was given |
| `not-trusted` | Windows does not trust the signature on the driver files (script exit 6). Nothing was changed: no package, no service |
| `failed-<code>` | The script failed with that exit code; the log has the reason |

The installer **does not add certificates and does not turn test signing on**, and it has
no option that does. When the state is `not-trusted` it installs Studio anyway and shows
a message saying that the driver was not installed, why, what to do (run
`bin\driver\trust\install-trust.ps1` with the public certificates of the authority that
signed the build, or turn on test signing on a test machine, then choose **Set up Virtual
Serial Studio**) and where the log is. A silent install shows nothing, exits 0 and leaves
the log and the `VssDriver` value; automation reads the value. A signed and trusted
package installs with no step by hand.

**Upgrade and uninstall.** An upgrade runs the old uninstaller in place, and the installer
tells that apart from a real uninstall (the old uninstaller runs from the install folder
itself, which a normal uninstall never does): it leaves the driver package, the helper
service and the ports' COM numbers alone, replaces the files, and runs the set-up again,
which stops the helper before it replaces `vsscom_helper.exe`. A real uninstall runs
`bin\driver\remove-driver.ps1`: it stops and deletes the helper service (the ports it made go
with it), deletes the `vsscom_um` package from the driver store (which removes the devices
that use it), checks that none is left and removes the helper's folder. It touches no
certificate and no setting, and no other driver. If it fails, the uninstaller says so and
keeps `driver-remove.log`.

**A Studio that is running.** Windows cannot replace or delete a program that runs, so
before an upgrade copies the files and before an uninstall deletes them the installer
closes `vss_gui.exe` and `vss_cli.exe`, in every session, when they run from the install
folder (`bin\stop-studio.ps1`; a Studio from the zip or a build tree is not touched). The
programs are ended, not asked: an elevated installer has no way to ask the Studio of
another user to quit. Studio writes what runs to the user's `running-setup.json` a second
after every change, so the ports start again the next time it is opened; nothing opens it
again after an upgrade. A person who runs the uninstaller is asked first ("Virtual Serial
Studio is running. Uninstalling closes it, and that stops its ports."); an upgrade and a
silent uninstall ask nobody, because the old version is uninstalled before the new
installer shows its first page.

**What an uninstall takes from each user.** Studio keeps two things outside the install
folder that matter after it is gone, and a real uninstall (not an upgrade) deals with both:

- The **Start when I sign in** entry, a value under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. It is deleted for every user whose
  registry hive is loaded (the users who are signed in), and only when it starts the
  program in this install folder. An entry of a user who is not signed in stays; it starts
  nothing, because the program is gone, and Windows says nothing about it.
- `running-setup.json` in each profile's `AppData\Roaming\iainchesworthlabs\Virtual
  Serial Studio`. It is **not deleted**, because it can be the only copy of a setup nobody
  saved. The uninstaller records in it that the ports were stopped, which is what File,
  Quit does, so a Studio that is installed again shows "When you last quit, ..." with
  **Start again** and does not start old ports by itself.

The uninstaller runs as an administrator and these files are in folders that ordinary
users own, so it never deletes there, skips a folder or file that is a link, and writes a
file only when it holds the exact text `"stopped": false`. Settings in
`HKCU\Software\iainchesworthlabs\Virtual Serial Studio` and saved setups are left, as before.
`tools/test_stop_studio.py` tests the script against scratch folders and a scratch
registry key.

**What was tried.** The Windows 11 guest (`VSS - Windows 11`, from its `clean` snapshot),
with a package signed by a throwaway certificate, built by
`drivers/windows/build.ps1`, never on the development machine:

- Test signing on and the signer trusted (the certificate in Trusted Root and Trusted
  Publishers, as `trust/install-trust.ps1` does for the owner's authority): `/S /DRIVER=0`
  installs Studio only; a silent upgrade over it installs the driver, the helper runs and
  the package is in the store; a second upgrade keeps both and does not call
  `remove-driver.ps1`; a silent uninstall leaves no folder, entry, Start menu folder,
  service, package or device.
- Test signing off and the signer not trusted: the same steps, with the driver step
  ending in `not-trusted`, no service, no package, the log naming the reason, and
  test signing still off.
- The pages and the message were looked at on the guest's screen.
- Closing a running Studio and the per-user clean-up were added later and have **not** been
  tried on the guest: the script has its own tests and the hosted runner checks one user
  (the runner's own). Two signed-in users, a user who is not signed in, a Studio in the
  notification area with driver ports, and the question the uninstaller asks are still to
  be looked at there.

The `driver` job and the installer steps of `release.yml` are not a driver test: a hosted
runner trusts neither an unsigned nor a test-signed package, so there the outcome is
`not-trusted` and the checks are that Studio installed, nothing of the driver did, the log
says why, and the uninstall is clean. They do run the real driver build and signing.
`tools/ci/check-windows-installer.ps1` makes those checks, and it runs by hand in a guest
too. Every program it starts has a time limit; one that runs out is stopped and the process
tree, the driver state and the installer logs are printed.

**The signed path, and why a pull request run was not enough.** A run that signs
(`SIGN_WINDOWS=test`, a tag, a manual run, the Monday run) hung once in the install step for
the whole job limit. The hosted Windows runner has **test signing on**. With unsigned driver
files `setup-driver.ps1` stops before it asks Windows anything (no signature), which is the
path every pull request run takes. With signed files, and test signing on, it used to accept
the package and call `pnputil /add-driver`, and for a signer that is not in Trusted
Publishers `pnputil` asks "Would you like to install this device software?" on the desktop,
where nobody can answer, and waits for ever. A guest reached through `vmrun` has no such
desktop and `pnputil` refused at once, which is why it was not seen there. Now:

- `setup-driver.ps1` accepts a package that is not trusted outright only when its signer is
  in Trusted Publishers (`trust/install-trust.ps1` puts it there), and otherwise ends in
  exit 6, `not-trusted`, before it calls `pnputil`. A refusal from `pnputil` for a trust
  error (`0x800B0109` and its neighbours) is exit 6 as well.
- Every `pnputil` call has a limit (180 seconds to add, 300 to delete), and the whole of
  `setup-driver.ps1` and `remove-driver.ps1` ends with exit code 124 after fifteen and ten
  minutes. The install and uninstall steps of the workflow have their own `timeout-minutes`.
- A pull request run also **rehearses the signed path**
  (`tools/ci/rehearse-signed-windows-installer.ps1`): the driver files in a copy of the
  stage tree are signed with a throwaway certificate that nothing trusts, a second
  installer is built from the copy, and it is installed, upgraded and uninstalled with the
  same check. A signed run does not need it, because its own installer takes that path.

**What is left**

- The release has a signed driver only as far as `SIGN_WINDOWS` signs it: `test` gives the
  owner's own certificate authority, which a machine trusts once `install-trust.ps1` has
  run, and `production` waits for the signing service. Until then an ordinary machine ends
  in `not-trusted`, which is as honest as the installer can be.
- The kernel-mode driver is not shipped. It needs the Microsoft signature (an EV
  certificate and attestation signing, in `docs/windows-com-driver.md`).
- ARM64: the release builds x64 only. `stage-package.ps1 -Platform ARM64` and the CPack
  file know the name, and the installer is untried on ARM64.
- The `Uninstall.exe` that the installer writes into the install folder is not signed.
  NSIS makes it at install time, and signing it needs a second build pass that runs the
  installer. The installer, and every program and driver file in it, is signed.
- The installer does not offer to trust the certificate authority. That would need the
  authority's public certificates in the package and a page that explains what trusting a
  root means; the existing script does it, with its checks, from the command line.
- The Qt deploy step puts `vc_redist.x64.exe` in `bin`; the installer carries it (about
  25 MB) but does not run it.
- The installer and the uninstaller use `apps/vss_gui/assets/icon/vss_gui.ico` (the cache variable
  `VSS_WINDOWS_ICON` in the CPack file repoints it). The icon of the Start menu entry and of the
  Apps & features entry is the one in `vss_gui.exe`, which comes from the resource file that
  gives the program its icon.

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
yet, and an agent does not create them. 
The Release workflow is wired to call the script, and stays off until the owner turns it on.
`tools/macos/ci-signing.sh` runs in the macOS leg of every run. If the repository variable
`SIGN_MACOS` is not `true`, or any value below is missing, it writes "unsigned" to the job
summary, warns, and succeeds. Otherwise it imports the certificate into a temporary
keychain, stores the notary credentials, and signs and notarises the staged app and tool (the
tar.gz) and, through the environment, the copy that CPack stages for the disk image
(`cmake/packaging/VssPackage-darwin-sign.cmake`). A pull request never signs. None of this has run yet:
it was written without the certificate, so the first signed run needs watching.

The owner adds:

| Name | Kind | Holds |
|---|---|---|
| `SIGN_MACOS` | variable | `true` to sign; absent or anything else leaves the package unsigned |
| `MACOS_SIGN_IDENTITY` | variable | The certificate name, `Developer ID Application: ... (TEAMID)` |
| `MACOS_SIGN_CERT_BASE64` | secret | The certificate and private key as a `.p12` file, base64 encoded |
| `MACOS_SIGN_CERT_PASSWORD` | secret | The `.p12` password |
| `MACOS_NOTARY_KEY_BASE64` | secret | An App Store Connect API key (`.p8`), base64 encoded |
| `MACOS_NOTARY_KEY_ID`, `MACOS_NOTARY_ISSUER` | secrets | The key id and the issuer id of that key |

The script notarises and staples the app. The disk image itself is not signed or notarised;
its contents are, which is what Gatekeeper checks when the app is opened.
