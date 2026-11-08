# Platforms and the test matrix

Virtual Serial Studio is built and tested on three operating systems. This page lists the cells that are tested, what each test
proves, how to run a cell, and the last result of each. A test that cannot run reports `not_run`, never `pass`.

## Cells

| Cell | Kind | Runs |
|---|---|---|
| `ubuntu-24.04`, `ubuntu-26.04` | hosted runner | every night, on request, and on a pull request that changes the matrix |
| `windows-2025` | hosted runner | the same |
| `macos-15`, `macos-26` (Apple silicon), `macos-15-intel`, `macos-26-intel` | hosted runner | the same |
| `VSS - Windows 10`, `VSS - Windows 11` | VMware guest on the owner's machine | on request, with the driver |
| `VSS - Ubuntu 26.04` | VMware guest on the owner's machine | on request |
| `wsl-debian` | WSL2 Debian on the owner's machine | during development |

The build uses the toolchains pinned in `.github/toolchain-versions.json`: GCC 16 on Linux, MSVC (toolset 14.51) on
Windows and Qt 6.12.0 for the GUI. macOS uses the Apple Clang that the runner carries. The pull request gate and the
other legs of `ci.yml` are described in [Continuous integration](ci.md).

The hosted cells are the workflow `.github/workflows/platform-matrix.yml`. The guests are described in
[`vm-testing.md`](vm-testing.md). A hosted macOS or Windows cell proves the build and the tests on a real installation of
that system. It does not prove that anyone has seen the program on a real display.

## Tests

| Id | What it proves |
|---|---|
| T1 | The `ctest` suites (unit, system, GUI) pass. On Linux and macOS the system tests use a pseudo terminal pair from `tools/pty_loopback.py`. |
| T2 | A separate process opens two virtual ports and exchanges 4 KB each way, in order, through the hub. |
| T3 | A split topology (one device, two listeners) and a join topology (two publishers, one device), loaded from JSON, route every byte, each publisher's bytes in order. |
| T4 | `vss_cli selftest` passes. |
| T5 | `vss_gui --selftest` passes on the offscreen platform. |
| T6 | The main window renders in the light and the dark theme (`vss_gui --screenshot`). The images are artifacts for a person to look at; they are never committed. |
| T7 | What the system and common tools show for a virtual port: the device node and its mode, `stty`, `/proc/tty/drivers` or `ioreg`, and pyserial's port list. These are recorded observations. |
| T8 | `vss_cli vport` makes two stable links that carry 1 KB each way, a second `vport` is refused the names a running one holds, links left by a killed process are replaced, and SIGTERM removes them and exits 0. |
| T9 | A release archive (`--package`) is unpacked with no build environment, and `vss_cli selftest` and `vss_gui --version` run from it. On Linux every library the GUI is linked to must be found, and the GUI starts under Xvfb when the machine has no display. A cell given no package skips T9. |
| T10 | `vss_cli bench` moves 4 MB over a pseudo terminal pair. The throughput is recorded; the floor is set after three runs agree. |
| T11 | An experiment with a kernel serial gadget in the Ubuntu guest. |

Windows cells skip T2, T3, T7, T8 and T10, which need pseudo terminals. The Windows guests run them against the driver once it
exists.

## Running a cell

```bash
cmake --preset linux-release-gui && cmake --build --preset linux-release-gui
ctest --preset linux-release-gui
python3 tools/matrix/run_cell.py --cell my-machine --build-dir build/linux-release-gui --out out
python3 tools/matrix/merge_results.py out --check
```

`run_cell.py` writes `result-<cell>.json` and one log per test into `out`. The JSON has one record per test with `pass`, `fail`,
`skip` or `not_run`, and an `observations` object with what T7 and T10 measured. `merge_results.py` prints the table below, and with
`--check` exits with 1 when any applicable test failed or did not run.

## Last results

The last hosted run in which every cell passed is [run 42024525324](https://github.com/iainchesworthlabs/virtual-serial-studio/actions/runs/42024525324), on commit `f249d31` of the guest-cell pull request, on 2026-11-05. Every cell passed every applicable test, including T8 and T9, and the macOS cells ran T5 and T6 again (the GUI had become an application bundle, which the runner did not find until that run). The scheduled runs on `develop` from 2026-11-06 failed, so the table below has not been refreshed since; the run list of the workflow has the current state.

| Cell | System | T1 | T2 | T3 | T4 | T5 | T6 | T7 | T8 | T9 | T10 | T11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `ubuntu-24.04` | Linux 7.0 (Azure), glibc | pass | pass | pass | pass | pass | pass | pass | pass | pass | 76.9 MB/s | skip |
| `ubuntu-26.04` | Linux 7.0 (Azure), glibc | pass | pass | pass | pass | pass | pass | pass | pass | pass | 76.9 MB/s | skip |
| `windows-2025` | Windows Server 2025 (10.0.26100) | pass | skip | skip | pass | pass | pass | skip | skip | pass | skip | skip |
| `macos-15` | macOS 15.8, Apple silicon | pass | pass | pass | pass | pass | pass | pass | pass | pass | 5.4 MB/s | skip |
| `macos-26` | macOS 26.6, Apple silicon | pass | pass | pass | pass | pass | pass | pass | pass | pass | 4.9 MB/s | skip |
| `macos-15-intel` | macOS 15.7, Intel | pass | pass | pass | pass | pass | pass | pass | pass | pass | 2.4 MB/s | skip |
| `macos-26-intel` | macOS 26.6, Intel | pass | pass | pass | pass | pass | pass | pass | pass | pass | 2.9 MB/s | skip |
| `VSS - Ubuntu 26.04` (guest) | Ubuntu 26.04, Linux 7.0.0-42-generic | pass | pass | pass | pass | pass | pass | pass | pass | pass | 27.2 MB/s | pass |
| `VSS - Windows 11` (guest) | Windows 11 Pro 10.0.26200 | pass | skip | skip | pass | pass | pass | skip | skip | pass | skip | skip |
| `VSS - Windows 10` (guest) | Windows 10 Pro 10.0.19045 | pass | skip | skip | pass | pass | pass | skip | skip | pass | skip | skip |

What these runs showed:

- T9 found that the Linux package carries only the `xcb` Qt plugin, and that plugin needs `libxcb-cursor0` and other xcb libraries from the system. On a machine without them `vss_gui` stops at start. `releasing.md` lists the libraries.
- The macOS GUI is an application bundle (`vss_gui.app`), and the runner now looks inside it.
- macOS refuses 921600 baud on a pseudo terminal, so T10 asks for 115200.
- On macOS, `ls /dev/cu.* /dev/tty.*` finds nothing and `ioreg -c IOSerialBSDClient` is empty while a studio pseudo terminal exists, and pyserial lists no port, so programs that list ports through IOKit do not see it. On Linux pyserial lists the 32 `ttyS` nodes of the runner and not the studio pseudo terminal. A kernel serial port (T11, `ttyACM0`) is in its list.
- Pseudo terminal throughput on the Intel macOS runners is two to four times lower than on Apple silicon, which is why T10 records the rate and holds it to no floor yet.

## The guest cells

The guest rows ran on 2026-11-05 on `develop` at `dbf0c75` with the changes of the guest-cell pull request, each from the `clean` snapshot, with the tests run inside the guest. `tools/vm/guest-cell.ps1` ran them; [`vm-testing.md`](vm-testing.md) says how. The programs were built outside the guests (the Linux one in WSL Debian with GCC 13, the Windows one on the owner's machine with MSVC and Qt 6.12.0) and copied in with their Qt libraries, so a guest cell proves that the build runs and passes on that system, not that the guest can build it. T9 ran the unsigned package from the Release workflow's dry run (run 41937590004) for that system. All 26 `ctest` suites passed on the Windows guests and all 21 on Ubuntu (which has no `vm_*` tests).

- **T11 runs on the Ubuntu guest.** The stock kernel has no `dummy_hcd`, but it has `usbip-vudc`, a virtual USB device controller, and `vhci-hcd`. With `g_serial` and `usbipd --device` they give a kernel serial pair: `/dev/ttyACM0` on the host side and `/dev/ttyGS0` on the gadget side. The test checks 1 KB each way, and `vss_cli bench` moves 1 MB between them at 3.8 MB/s. pyserial lists `ttyACM0`, which it does not do for a studio pseudo terminal.
- **The driver tests are not a matrix cell.** `drivers/windows/test` runs on the same guests through `guest-cell.ps1 -DriverBundle`, and the state of each driver test is kept in [`drivers/windows/README.md`](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/drivers/windows/README.md).
- A guest needs more than the program. The Ubuntu guest needed `cmake` for `ctest`, the OpenGL, EGL and fontconfig libraries for Qt, and Xvfb with the xcb libraries for T9. The Windows guests needed the `offscreen` platform plugin (`windeployqt` ships only `windows`), PowerShell 7 for the `vm_*` suites, and the build drive path, because some GUI tests read source files through the absolute path they were built with.
- On the Windows guests the cell also takes a screenshot of the program on the guest's desktop (a scheduled task starts it in the logged-on user's session). It runs on both. On the guests' 1024 by 768 screen the main window is taller than the screen, so the status bar and the Simple and Advanced switch are off the bottom, and the toolbar shows icons without their text labels, which the 1280 by 800 offscreen images do not show.
- The GUI tests that wait a fixed few milliseconds for an event can miss it on a slow guest. Windows 10 (two virtual CPUs) failed `gui_dialogs`, `gui_backend` and a timed-out `matrix_tools` on different runs while the host was busy, and passed on a quiet host. The guest scripts now rerun failed suites once and put the names in the T1 detail, so a rerun is never hidden.
- The GUI test gallery (every window and dialog, in both themes and at the compact size) comes back with each guest cell. It is for review and for the user guide, and it is not committed.

## macOS

The macOS build is compiled and tested only on GitHub-hosted macOS runners (macOS 15 and 26, Apple silicon and Intel). It has
not been run on a Mac that anyone looks at. The graphical interface has not been seen on a real display, the application bundle
is not signed or notarised (`SIGN_MACOS` is `false`, and `tools/macos/sign-and-notarize.sh` has not been run on a Mac) and Gatekeeper behaviour has not been checked, and no third-party serial program has been tried
against a macOS virtual port. Virtual ports on macOS are pseudo terminals (`/dev/ttysNNN`), which programs that list ports through
IOKit do not show.

## Windows

Virtual ports on Windows need a driver: `vsscom` (kernel mode) or `vsscom_um` (user mode, with its helper service). Until one is
installed, `auto` falls back to com0com when present, and to an in-process loopback device otherwise. See
[`windows-com-driver.md`](windows-com-driver.md); Studio checks the setup and offers to install it.

### Prompts Windows shows that the program did not ask for

A UAC window that appears while the tests run is not a request from the tests for
administrator rights. Windows Firewall shows the prompt when a program starts
listening on every network address and has no rule yet; pressing **Allow access**
then needs administrator approval, and that is the UAC window. The system tests
and the performance suite listen on the loopback address only, which the firewall
never asks about, and a test must keep to that: bind a test server to
`127.0.0.1`, never to the wildcard address.

Every program the project builds on Windows carries an application manifest with
`requestedExecutionLevel asInvoker` (`cmake/windows/vss.manifest`, attached by
`cmake/VssWindowsResources.cmake`). Without a manifest Windows decides from the file
name and version strings whether to treat a program as an installer, so the
manifest means Windows never guesses. The unit tests read the manifest of the test
program itself.

The earlier prompts left firewall rules behind, one per build folder and
protocol, named after the program. They are harmless, but a machine that ran the
tests from many build folders has dozens. Remove them in the Windows Defender Firewall
with Advanced Security console (**Inbound Rules**, sort by name, delete the
`vss_system_tests` rules) or from an administrator PowerShell:

```powershell
Get-NetFirewallRule -DisplayName vss_system_tests | Remove-NetFirewallRule
```

Check the list first with `Get-NetFirewallRule -DisplayName vss_system_tests`;
Studio never adds, changes or removes a firewall rule by itself.

### Elevation and Windows Firewall

The permissions layer (`libs/vss_ports`, `vss/ports/permissions.hpp`) is shared by the
GUI and `vss_cli permissions`. `observe()` reads the host (the process token with
`GetTokenInformation`, and Windows Firewall through `INetFwPolicy2`, read only);
`evaluate()` turns that into facts with reason codes and is plain code, so the
tests inject observations and check every outcome on every system. A platform that
cannot be asked answers `unknown`, and the Setup check shows "Not checked yet".

- **Administrator.** Elevated, an administrator account that has not elevated (a
  filtered token) and a standard user are three different facts, because Windows
  asks for consent in the second and for a password in the third.
- **One privileged step, in its own process.** Studio is never run elevated to do
  one thing. The firewall rule is made by `vss_cli permissions firewall-allow`,
  started with the `runas` verb so Windows shows its own prompt. The helper
  validates its arguments (one port, one protocol, local subnet unless the caller
  said otherwise, never every network), refuses a program that is not `vss_gui.exe`
  or `vss_cli.exe` in its own folder, and replaces only a rule with its own name.
  The prompt shows the helper's name and publisher. A release signed in `test` mode
  reads "Unknown publisher" on a machine that does not trust the owner's CA (see
  [the Windows driver notes](windows-com-driver.md#signing-the-studios-windows-programs)).
- **Reading the rules.** An inbound rule decides a listener when it is enabled,
  names the program (or no program), covers the protocol and the port, and applies
  to a network the computer is on now. A block rule wins. "Block all incoming
  connections" overrides every rule and is reported as such. Rules for a service
  are skipped.
- **Capability.** `capabilities::firewall_rule` is true where the platform has the
  step. Without it, or without `vss_cli` beside the program, the dialog explains
  and shows the command, and the button reads "Not available in this build".

Not run on a real machine (a check never changes the firewall of the machine the
tests run on, so the step itself is covered by unit tests of the rule text, the
argument round trip and the helper's refusals): adding and removing the rule in a
Windows guest, the UAC prompt for a standard user, and a machine joined to a
domain with policy-managed rules.

### Serial access, confinement and the firewall on Linux

The same permissions layer reads what an ordinary user can read: the groups of
the process, `stat` and `access(2)` of each serial device, `/proc/self/attr/current`
(an AppArmor profile or an SELinux context), `/sys/fs/selinux/enforce`,
`FLATPAK_ID` and `SNAP_NAME`, `/etc/ufw/ufw.conf`, and the presence of
`/run/firewalld.pid`. `access(2)` answers for the file mode only, which is what
makes a policy denial recognisable: a device the mode allows and an open that was
refused is AppArmor, SELinux or a sandbox, and the fact says so with the fix for the
one in force.

- **One privileged step.** Putting the user in the serial group is
  `pkexec usermod -aG <group> <user>`, started with `posix_spawn` after the user
  presses the button. polkit's agent shows the command it is asked to run. The step
  names only the current user and only `dialout`, `uucp`, `tty` or `lock`.
- **Honest limits.** A bare label such as `kernel` (WSL) is not an AppArmor profile.
  Firewall rules need root, so without `ufw` or `firewalld` the fact is `unknown`.
  Whether a polkit agent is running cannot be asked in advance, so a declined or
  missing prompt is reported after the fact.
- **Capability.** `capabilities::serial_group_step` is true where `pkexec` and
  `usermod` are installed; otherwise the dialog only explains and shows the command.

Run so far: the Linux part of the library compiled with GCC 14 and
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`, its unit tests, and a
read-only probe in a Debian WSL distribution, which found `/dev/ttyS0` owned by
`dialout` with the user outside the group and printed the exact command. Not run:
the polkit prompt and the group change (they change the account), AppArmor and
SELinux denials on real profiles, and a Flatpak or Snap build. Those need the
Ubuntu guest of [Testing in the VSS guests](vm-testing.md) and a Fedora guest.

### The Application Firewall, Local Network and the sandbox on macOS

The permissions layer reads: `stat` and `access(2)` of each serial device, the
`APP_SANDBOX_CONTAINER_ID` that the App Sandbox sets, the Darwin release from
`uname` (24 is macOS 15, where the Local Network permission starts), and, only when a
listener is open to other computers, `socketfilterfw --getglobalstate` and
`--getblockall` run with a small fixed environment and a three second limit
(`capture_command` in `src/platform/posix/permissions_posix.hpp`). The output is
parsed by `parse_alf_global_state` and `parse_alf_block_all`, which leave a state
unknown when the text is not what macOS prints.

- **No step.** The firewall answer is given in the prompt macOS shows. The fix for
  "Don't Allow" is two `sudo socketfilterfw` commands that the user runs; Studio
  does not run them, so there is no capability flag to gate.
- **Local Network** has no query and is reported as information from macOS 15 only
  when a listener is open to other computers. The bundle's `Info.plist`
  (`apps/vss_gui/macos/Info.plist.in`) carries `NSLocalNetworkUsageDescription`;
  without it macOS refuses the request without asking.
- **Release.** `apps/vss_gui/macos/vss_gui.entitlements` is the hardened-runtime
  entitlements file for the notarised release: empty on purpose, because Studio needs
  no exception (no JIT, no unsigned memory, no foreign libraries). A sandboxed build
  starts from `vss_gui.sandbox.entitlements`. `tools/macos/sign-and-notarize.sh` signs
  from the inside out, notarises and staples; see [Releasing](releasing.md).

Not run: anything on a Mac. The macOS file was compiled with GCC 14 against a stand-in
for `mach-o/dyld.h`, its parsing was tested with the text macOS prints, the shared
command runner was tested on Linux, and the `Info.plist` template was expanded with
CMake and read back as a property list. The Application Firewall query, the prompts,
signing and notarisation need a Mac with an Apple Developer ID.
