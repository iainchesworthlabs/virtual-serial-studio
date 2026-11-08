# Virtual port workstreams

Creating a serial port should never require a physical one. This page records
the two workstreams that closed the gaps, what each one delivered, and what is
left.

## Where the platforms stand

| Platform | Today | Gap |
|---|---|---|
| Linux | A pseudo terminal per virtual port (`/dev/pts/N`). `vss_cli vport` gives each a stable name and runs until stopped. See [Virtual ports on Linux and macOS](posix-virtual-ports.md) | Programs that list ports do not list a PTY, and the PTY has no modem lines |
| macOS | The same mechanism (`/dev/ttysNNN`), exercised by CI on hosted runners only | The same as Linux. The studio cannot tell whether a program has the port open. Nothing has been run on a Mac that anyone looks at. A DriverKit driver would be the route to a non-PTY device and is not planned |
| Windows | Two drivers for one virtual `COMn` port, `vsscom` (kernel mode) and `vsscom_um` (user mode, with the `vsscom_helper` service), plus com0com when installed and an in-process loopback device otherwise. See [Replacing com0com](windows-com-driver.md) | The kernel-mode package is unsigned, so Windows loads it only with test signing on. ARM64 is built but has no guest. The driver raises no line-error events (`EV_ERR`, `EV_BREAK`) |

So Linux and macOS create real devices with a stable name, and Windows has a
first-party driver. What remains is signing the kernel-mode package, and the
kernel-level options for POSIX that are documented and not built.

## The two workstreams

Each ran on its own branch and has merged into `develop`; the branches are gone.
The definition of done for each, and where it stands:

### Windows driver

| Item | State |
|---|---|
| The KMDF project builds in CI on a Windows runner with the WDK, produces `vsscom.sys` and a stamped `vsscom.cat`, and passes `InfVerif` | Done, for x64 and ARM64. The same script builds the user-mode driver and the helper service |
| `pnputil /add-driver vsscom.inf /install` creates COM ports from the INF, named `COM<n>` by the Ports class | Done. `drivers/windows/test/install-driver.ps1` makes two by hand, and the control device makes more on demand |
| A loopback mode returns written bytes on read; control-line IOCTLs and the common `IOCTL_SERIAL_*` set are answered | Done. Beyond loopback, a hub end carries the bytes to the studio |
| The VMware test script installs the driver in a clean snapshot, runs a serial test and reports a machine-readable result | Done: `tools/vm/guest-cell.ps1 -DriverBundle` runs `drivers/windows/test` in the Windows 10 and Windows 11 guests. See [Testing in the VSS guests](vm-testing.md) |
| `vss::driver_available()`, `driver_version()` and `create_driver_port()` return the driver's real values, and auto selection prefers it over com0com | Done |

### POSIX virtual ports

| Item | State |
|---|---|
| A stable name points at the live PTY for the lifetime of the port | Done. The link directory is `$VSS_VPORT_DIR`, then `$XDG_RUNTIME_DIR/vss`, then `/run/vss` and `/tmp/vss` on Linux or `/tmp/vss` on macOS; `--stable-dir` names it directly |
| A headless command owns one or more ports until interrupted, prints their paths, and removes the links on exit, including on SIGINT and SIGTERM | Done: `vss_cli vport` |
| Tests cover link creation, replacement of a stale link, cleanup on shutdown and the permission rules | Done: `tests/unit/test_stable_link.cpp`, and test T8 of the platform matrix on every system with pseudo terminals |
| Documentation shows how an application opens the stable path | Done: [Virtual ports on Linux and macOS](posix-virtual-ports.md) |

## Working on several branches at once

Each git worktree is a full checkout on its own branch, so a session can start
in one without changing anything else. Rules that apply to every branch:

- Commit messages and pull request text carry no trailers other than a human
  `Signed-off-by`, no attribution and no tool markers; see
  [Contributing](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/CONTRIBUTING.md).
  The `Commit text` CI job enforces it.
- Rebase on `develop` before pushing; the shared harness and CI live there.
- A branch with no pull request does not run CI on push, so dispatch it
  explicitly with `gh workflow run ci.yml --ref <branch>`. [Continuous
  integration](ci.md) says what each tier runs.
- Feature branches merge into `develop`; `main` only receives release merges.

## VM testing

See [vm-testing.md](vm-testing.md) for the VMware harness and the driver test
loop.
