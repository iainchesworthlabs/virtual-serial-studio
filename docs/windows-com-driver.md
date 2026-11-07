# Replacing com0com: virtual COM ports on Windows

The goal is for Virtual Serial Studio to be the only thing a user installs. On
Linux and macOS that is already true — the application creates real pseudo
terminals. On Windows it is not, and cannot be, until a kernel driver exists.

This note explains why, what the driver must do, and how the application is
already shaped to accept it.

## The hard constraint

A Windows `COMx` is a **kernel device**, not a name. When an application does

```c
HANDLE port = CreateFileW(L"\\\\.\\COM5", ...);
SetupComm(port, 4096, 4096);
GetCommState(port, &dcb);
ReadFile(port, buffer, count, &read, &overlapped);
```

it is talking to a device object enumerated under `GUID_DEVCLASS_PORTS`, whose
driver implements the serial IRP/IOCTL contract. `CreateFile`, `GetCommState`,
`SetupComm`, `SetCommTimeouts`, `WaitCommEvent`, `EscapeCommFunction` and the
overlapped read/write paths all map onto `IOCTL_SERIAL_*` requests.

There is no user-mode API that registers a COM port. Registry entries and
symbolic links on their own produce a name that `CreateFile` resolves to
nothing. So:

* **A driver is required.** Nothing in user space can substitute for it.
* **It must implement the serial contract**, not just pass bytes through,
  because applications configure timeouts, purge queues, wait on line events
  and query modem status before they exchange a single byte.
* **It must be signed** for any machine that is not in test-signing mode.

## Platform matrix

| Platform | Mechanism | Driver needed? |
|---|---|---|
| Linux | Pseudo terminal (`/dev/pts/N`); a full tty with termios | No |
| macOS | Pseudo terminal (`/dev/ttysNNN`); a full tty | No |
| Windows | `COMx` device object + serial IOCTLs | **Yes** |

### Linux

A PTY is a tty. `tcsetattr`, `poll`, `select`, `read`, `write` and the termios
flags work as they do on a UART. The modem lines do not: the kernel's PTY has
no `TIOCMGET` or `TIOCMSET`, so a program that opens the slave cannot read or
set DTR, RTS, CTS or DSR. The studio owns the master and holds the slave open,
so the port exists before an application connects.

Two gaps remain. The name is `/dev/pts/N` and changes on every run; see
`posix-virtual-ports.md` for the stable names that `vss_cli vport` makes. And
programs that offer a list of detected ports (pyserial's `list_ports`, Qt's
`QSerialPortInfo`) do not list a PTY; a program that takes a typed or
configured path works.

### macOS

Same answer, with two caveats:

* Applications that insist on `/dev/cu.*` or list ports through IOKit will not
  find a PTY. A stable link from `vss_cli vport` helps programs that take a
  path. Whether a root-owned alias in `/dev` works is not known yet.
* A kext is not a viable distribution path. Apple requires reduced security
  mode and notarisation, and Apple Silicon makes third-party kexts a support
  problem. SerialDriverKit (`IOUserSerial`) is a possible later route to a
  port that IOKit lists; it needs an Apple entitlement and a Mac to try, and it
  is not planned. PTY is the supported answer.

## Options considered for Windows

| Option | Verdict |
|---|---|
| Bundle com0com and drive `setupc.exe` | Works today, and com0com is GPL like this project, but it keeps a third-party driver, an elevation prompt and a dependency we set out to remove. Useful as the transitional fallback. |
| Named pipe presented as a "port" | Not a COM port. Only applications that accept a pipe path can use it. |
| USB/IP with a virtual CDC device | Requires `usbip-win` (another third-party driver) plus a server; fragile under suspend and reconnects. |
| `SerCx2` (serial controller extension) | Designed for real UART controllers and DMA; far more ceremony than a software port needs. |
| UMDF serial device | First judged unworkable; it turned out to work. `vsscom_um` is a UMDF 2 driver for the same port, built from the same sources. It needs no kernel-mode signature, which is why it is offered beside the kernel-mode driver. It cannot be a bus, so its ports are made by a helper service (see "The user-mode driver and its helper"). |
| **KMDF virtual serial function driver** | **Recommended.** A control device creates ports on demand; each port is a PDO implementing the serial contract with a shared-memory data path to the hub. |

## Recommended architecture

```
  ┌──────────────────────────── user mode ────────────────────────────┐
  │  vss_gui / vss_cli                                                │
  │        │                                                          │
  │   vss::studio ── vss_ports::driver_transport                      │
  │        │              │  shared section + events                  │
  └────────┼──────────────┼───────────────────────────────────────────┘
           │              │
  ┌────────┼──────────────┼──────────────── kernel mode ──────────────┐
  │        │        vserial.sys                                       │
  │        │              │                                           │
  │        │         ┌────┴─────┐      ┌──────────────┐               │
  │        │         │ PDO COM5 │ ...  │ PDO COM6     │               │
  │        │         └────┬─────┘      └──────┬───────┘               │
  └────────┼──────────────┼───────────────────┼───────────────────────┘
           │              │                   │
           └──────────────┴───────────────────┴── applications
```

### Control device

`vserial.sys` exposes `\Device\VSerialCtl` with the symlink `\\.\VSerialCtl`.
The hub opens it once and keeps it open for the process lifetime.

| IOCTL | Purpose |
|---|---|
| `IOCTL_VSERIAL_GET_VERSION` | ABI major/minor; the client refuses a major mismatch |
| `IOCTL_VSERIAL_CREATE_PORT` | Create a port; returns instance id, `COMx`, section and event names |
| `IOCTL_VSERIAL_DESTROY_PORT` | Remove a port (also on last handle close, configurable) |
| `IOCTL_VSERIAL_LIST_PORTS` | Diagnostics and the GUI's "driver ports" view |

`CREATE_PORT` takes a request struct: desired COM number (`0` = auto), ring
sizes, queue depth, and flags. The driver creates the PDO, chooses a free COM
number when asked, writes `PortName` under the device's hardware key, creates
`\DosDevices\COMx`, and returns names for the shared section and events. Names
are derived from the instance id so two processes cannot collide.

### The user-mode driver and its helper

A UMDF driver cannot create child devices, so `vsscom_um` has no control device.
Its ports are software devices (`SwDeviceCreate`, hardware ID `VSSCOMUM\PORT`,
instance id the slot), and Windows lets only SYSTEM and administrators make
those. The `vsscom_helper` service, which runs as SYSTEM, makes them for
ordinary users and keeps the same rules as the control device:

* A caller opens the named pipe `\\.\pipe\vsscom_helper` (signed-in users only,
  no remote clients) at identification level and asks for a port. The helper
  reads the caller's SID from the pipe, checks the limits (16 ports for a user,
  32 in all, the lowest free slot) and makes the device with the SID on it as a
  device property.
* It answers when the driver has started on the device, so the COM name and the
  hub interface exist by then.
* The port lives as long as the pipe is open. When the caller closes it or
  dies, the helper closes the device and the port goes.
* The driver reads the owner property when it opens the hub end and compares it
  with the user of the opener's token. A port without an owner was made by hand
  and opens for administrators only.

### Telling the user which piece is missing

A missing driver, a helper that is not installed, stopped or older than the
studio, and a helper that refuses the user all used to come back as the same few
codes, and the GUI showed "permission denied". `vss::probe_driver_setup()` asks
each piece on its own and makes no port: the driver store (the hardware ID key
under `HKLM\SYSTEM\DriverDatabase\DeviceIds`, readable by every user), the service
control manager, and one request on the helper's pipe that changes nothing (a
COM number of 0, which a current helper answers `ERROR_INVALID_PARAMETER` before
it looks at anything, an older one `ERROR_NOT_SUPPORTED`, and one that cannot
tell who the caller is `ERROR_ACCESS_DENIED`). `classify_driver_setup()` names the
first piece that is wrong, in the order a person fixes it. The GUI turns each into
its own sentence and, for the ones a setup fixes, the action "Set up Virtual
Serial Studio" (`drivers/windows/README.md`, "Setting up from Studio"). The code
of the call that failed is kept (`vss::last_driver_failure()`) and goes behind
"Copy details".

The pipe protocol is in `drivers/windows/vsscom_helper/protocol.h`. The studio
tries the kernel-mode driver's control device first, then the helper, then a
free port that already exists.

### COM numbers

A COM name comes from the Ports class installer when Windows builds the port:
the lowest number in the COM name database (`ComDB`) that is free, written as
`PortName` under the device's hardware key. Device Manager's *Advanced > COM
Port Number* changes it. What that takes, found by trying it in a Windows 11
guest, is why the driver does not honour a requested number itself:

* **Writing `PortName` before the port is built does not work.** A key made
  ahead of the device makes the install fail (`CONFIGFLAG_FAILEDINSTALL`), so a
  number cannot be requested in the create request. A request field the driver
  could honour would need the driver to claim the number in `ComDB`, which is a
  user-mode, administrator-only API. The create request therefore keeps its
  `Flags` and `Reserved` fields at zero.
* **Changing a port that exists works**, and is what Device Manager does: claim
  the new number in `ComDB` (refused if another device holds it), write
  `PortName` in the device key, update the friendly name, restart the device so
  that the driver reads the new `PortName`, then release the old number. The
  driver creates the `\DosDevices` link, the `SERIALCOMM` value and the
  interface only when it starts, so the restart makes all three follow.
* **A restart with a handle open fails, and leaves the port unable to restart
  again** until it is removed and made again (`DN_NEED_RESTART`). So the studio
  checks `AppOpen` first, closes its own hub handle, and the helper looks for
  a program by opening the COM port itself for an instant before it restarts.

Everything in that list needs administrator rights, so the helper service
(SYSTEM) does it, for both drivers, on request (`VSSCOM_HELPER_OP_SET_COM_NUMBER`
for a port of the user-mode driver that the pipe made, and
`VSSCOM_HELPER_OP_SET_COM_NUMBER_KM` for a port of the kernel-mode driver, where
the helper asks the control device who made the port,
`IOCTL_VSSCOM_CTL_GET_OWNER`, and compares the caller's SID). A number is taken
if `ComDB` holds it or `\\.\COMn` resolves, which covers real ports, com0com and
ports that have gone but keep their name. The range is 1 to 255.

The studio asks for a number in `create_driver_port(..., com_number)`: it makes
the port as usual, then has the helper change the number before it opens the hub
end, so nothing is open. `set_driver_com_number` does the same later: it closes
the hub handle, asks, opens the hub handle again on the restarted port, and gives
the reads and writes that were waiting to the new handle, so the endpoint never
sees the change.

### Flow control, revoke and who has the port

* **DTR and RTS.** `IOCTL_SERIAL_SET_HANDFLOW` applies its DTR and RTS settings.
  Enable and disable move the line when the settings are given; handshake makes
  the line follow the receive buffer (low at `XoffLimit` bytes free, high again at
  `XonLimit` bytes waiting); toggle holds RTS high from the first queued byte
  until the last has had time to leave at the current baud rate and frame. A line
  in handshake mode refuses `SET_DTR` and `SET_RTS`, as `serial.sys` does. The
  settings reset when the program closes the port. The rules are plain C in
  `vsscom/core`, with unit tests. Output flow control (stopping on CTS or DSR,
  XON/XOFF) is not applied.
* **Revoke.** `IOCTL_VSSCOM_HUB_REVOKE`, on the hub handle, ends the waiting
  requests of the program that has the COM port, marks its handle so that every
  later request on it fails with `STATUS_DEVICE_REMOVED`, clears `AppOpen`,
  resets the port as a close would, and lets a new program open it. Only the user
  who owns the port has a hub handle (`access.c`), so only that user can revoke.
  The program keeps its handle, and closing it later changes nothing.
* **`AppProcessId`.** The hub state names the process that opened the port.
  The studio logs each change of `AppOpen` against the endpoint with the time and
  the process, and `vss::driver_peer` and `vss::peer_open` answer from it. A
  version 3 driver and studio go together: the state grew by one field, so the
  studio refuses a driver of version 2.

### Data path

Per port, one named section holding two single-producer/single-consumer rings:

| Ring | Direction | Producer | Consumer |
|---|---|---|---|
| `to_device` | application → hub | driver `IRP_MJ_WRITE` | user-mode transport |
| `from_device` | hub → application | user-mode transport | driver `IRP_MJ_READ` |

Each ring is a fixed-size byte array plus aligned head/tail indices, a
`space_available` event and a `data_available` event. The driver maps the
section in kernel virtual address space; the client maps it with
`MapViewOfFile`. A read IRP consumes what is available and pends when the ring
is empty; a write IRP copies what fits and pends the remainder until space is
signalled.

This keeps the interrupt-free data path to a memory copy on each side and
avoids per-byte IOCTLs.

### Serial semantics the driver must implement

| Area | What is required |
|---|---|
| Queues | `SET_QUEUE_SIZE`, `PURGE` (abort read/write and reset rings), `GET_COMMSTATUS` sizes |
| Configuration | `GET/SET_COMMCONFIG` backed by the hub's `serial_settings`; reject unsupported values with `STATUS_INVALID_PARAMETER` |
| Timeouts | `SET/GET_TIMEOUTS` with `ReadIntervalTimeout`, `ReadTotalTimeoutMultiplier` and `ReadTotalTimeoutConstant`; the driver must own the timers, or applications block forever |
| Modem lines | `SET/GET_DTR`, `SET/GET_RTS`, `GET_MODEMSTATUS`, `SET_HANDSHAKE` forwarded to the hub over the shared flags page |
| Break | `SET_BREAK`, `SET_XOFF/XON`, `IMMEDIATE_CHAR` |
| Events | `WAIT_ON_MASK` for `EV_RXCHAR`, `EV_TXEMPTY`, `EV_CTS/DSR/RLSD/RING`, `EV_BREAK`, `EV_ERR` |
| Errors | framing, parity, overrun and break injected by the hub surface as `COMMSTATUS` error bits and `EV_ERR` on the next read |
| Lifecycle | cancel pended IRPs on purge, close, surprise removal and destroy; power transitions D0/D3 |

The timeout rules are the subtle part and the usual reason a home-grown
virtual port fails: a `ReadFile` with `ReadIntervalTimeout = MAXDWORD`,
`ReadTotalTimeoutMultiplier = 0` and `ReadTotalTimeoutConstant = 0` means "return
immediately with whatever is buffered", while other combinations mean "wait for
the first byte, then for a gap". The driver has to implement all of them.

### ABI and versioning

* All IOCTLs use `CTL_CODE(FILE_DEVICE_UNKNOWN, ...)` with a struct that starts
  with `{ uint32_t size; uint16_t major; uint16_t minor; }`.
* The client and driver share one header, `vcom_driver_abi.h`, which is built
  into both the user-mode library and the driver.
* A major mismatch is a hard error with a readable message; a minor mismatch
  means "ignore unknown fields".

### Signing and distribution

| Stage | Requirement |
|---|---|
| Development | `bcdedit /set testsigning on`, self-signed certificate, `inf2cat`, `signtool`, `pnputil /add-driver`, reboot |
| Release | EV code-signing certificate and a Microsoft Hardware Dev Center account; attestation signing is enough (WHQL/HLK is not required for attestation) |
| Architectures | x64 and ARM64 builds, both signed |
| Installer | `pnputil` or a WiX/MSI custom action; uninstall must remove ports and the control device cleanly |
| GPL | The driver is GPL-3.0-or-later like the rest of the project; shipping the binary means offering its source, and this repository is that offer |

The certificate is the long-lead item: procurement and identity validation can
take weeks, and Windows 10+ will not load an unsigned kernel driver.

### Signing the studio's Windows programs

The programs people start are signed in CI by the same mechanism as the driver
package: the shared `sign-windows` action of `icl-common-infra`, behind the
`SIGN_WINDOWS` repository variable. Today that action signs with a leaf
certificate from the owner's personal certificate authority (`test`); its
`production` mode, for a certificate that Windows trusts everywhere (an OV or EV
certificate, or a cloud signing service), is reserved and fails until the owner
chooses one. The repository wraps it in `.github/actions/sign-windows-binaries`,
which signs, then runs `tools/ci/verify-windows-signatures.ps1` and fails the leg
when a file is unsigned, altered after signing or has no timestamp.

| Program | Where it is signed | When |
|---|---|---|
| `vss_gui.exe`, `vss_cli.exe`, any `vss_*.dll` | `release.yml`, job `package`, before the zip is made | A tag, a manual run and the Monday run, never a pull request |
| `vsscom_um.dll`, `vsscom_um.cat`, `vsscom_helper.exe` | `ci.yml`, job `driver` | A pull request from this repository, never one from a fork |

The Qt libraries in the package are the Qt Company's and ship as built. There is
no installer; a zip cannot carry an Authenticode signature.

**What the user sees.** The studio and the helper do not ask for administrator
rights when they start: every program has a manifest with `asInvoker`, and
`tests/unit/platform/windows/test_manifest_windows.cpp` and the verify script both
refuse `requireAdministrator` and `highestAvailable`. The helper is a service
that runs as SYSTEM, so it needs no prompt at all. A prompt appears only when
the user chooses *Restart as administrator* (it starts `vss_gui.exe`) or *Allow
in Windows Firewall* (it starts `vss_cli.exe`), and the window explains why first
(`docs/gui-design/docs/07-copy-deck.md`). The prompt names the program and the
publisher of its signature. **Verified publisher** appears only on a machine
that trusts the certificate chain. A test-signed build shows **Unknown
publisher** everywhere except where the owner's CA is installed
(`docs/test-signing.md` in `icl-common-infra`). A name such as "Iain Chesworth
Labs" in the prompt needs the `production` mode.

**What the owner creates** (an agent does not): the organisation secrets
`WIN_TESTSIGN_PFX_BASE64` and `WIN_TESTSIGN_PFX_PASSWORD`, and the repository
variable `SIGN_WINDOWS=test`. Until they exist the signing step logs a warning,
writes a note in the job summary and leaves the files unsigned (named
`-unsigned`); the leg still passes.

**Trying it on your own machine** with a throwaway self-signed certificate, from
PowerShell with the Windows SDK installed and a built `vss_gui.exe`:

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Studio local test' `
  -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddDays(7)
Copy-Item build\windows-msvc-debug-gui\bin\vss_gui.exe $env:TEMP\vss_gui.exe
Set-AuthenticodeSignature $env:TEMP\vss_gui.exe -Certificate $cert -HashAlgorithm SHA256 `
  -TimestampServer http://timestamp.digicert.com
./tools/ci/verify-windows-signatures.ps1 -Mode test -Files $env:TEMP\vss_gui.exe
```

The check passes with `UnknownError` as the status, because this machine does not
trust the certificate. Change one byte of the copy and it fails with
`HashMismatch`. To see the prompt name the publisher, import the certificate
into `Cert:\LocalMachine\Root` and `Cert:\LocalMachine\TrustedPublisher` from an
elevated shell, start the signed copy with *Run as administrator*, and remove
both entries afterwards. Never do this with the owner's keys: they exist only in
CI.

## Effort

| Phase | Scope | Estimate |
|---|---|---|
| 0 | Application interface, detection, configuration, docs (done) | — |
| 1 | ABI header, shared rings, stub driver creating a port that echoes bytes | 2–3 weeks |
| 2 | Full serial contract: timeouts, purge, wait mask, line control, cancellation, power | 3–5 weeks |
| 3 | INF, installer, upgrade/uninstall, version handshake | 1–2 weeks |
| 4 | Signing, VM test matrix, Driver Verifier runs | 2 weeks + certificate lead time |

Driver development also needs a machine where test signing is enabled and a
reboot is acceptable. GitHub-hosted runners cannot reboot mid-job, so driver
smoke tests want a self-hosted Windows VM.

## What the first driver builds

The first driver is simpler than the architecture above and keeps its shape. It
has no shared memory. Each port has two ends on one device object: the COM port,
and a second handle that the studio opens through a device interface with the
reference string `hub`. Bytes cross through two 64 KB queues inside the driver,
with flow control, so `ReadFile` and `WriteFile` on the hub handle replace the
shared rings. See `drivers/windows/README.md` for the table of the two ends.

The control device is a second, small device that the same `vsscom.sys` serves,
installed once from its own INF (`vsscomctl.inf`, class System, because it is
not a port and must not get a COM name). It differs from the sketch above in
three ways, each with a reason:

* It is a **bus**, not a named control object. A request to create a port
  reports a child device to the Plug and Play manager, which installs
  `vsscom.inf` on it and starts the port like any other. The COM name then comes
  from the Ports class installer, and the port shows in Device Manager and
  `SERIALCOMM` as every serial port does.
* A port lives as long as **the file handle that made it**, with no
  configuration. The process ending is the usual way a port goes, so the driver
  does not rely on a program to clean up.
* The **slot** is the lowest free number and is the device's instance id, so a
  port that returns gets the device node and COM name it had. A COM number can
  be chosen and changed through the helper service; see "COM numbers".

The shared-memory data path remains a later step, if the queues turn out to be
too slow.

## What is already in place

* `vss::virtual_port_kind::windows_driver` and
  `vss::supports(...)`, so the GUI, the CLI and stored topologies already
  understand the kind.
* `vss::driver_available()` / `driver_version()` — a list of the driver's
  device interfaces, which opens nothing, and the interface version.
* `vss::create_driver_port()` — asks the control device, or the helper service
  of the user-mode driver, for a port of its own (or, with neither, takes a free
  port that exists), opens its hub end for
  overlapped I/O and returns the transport that carries its bytes and control
  lines. Closing the transport removes a port it made. It reports
  `errc::unsupported` with no driver, `errc::device_busy` when there is no
  control device and every port is taken, `errc::port_exhausted` when the
  control device has no slot left, `errc::permission_denied` and
  `errc::timeout`. With a `com_number` it also reports `errc::already_exists` when
  another device has the number and `errc::invalid_argument` outside 1 to 255;
  `vss::set_driver_com_number()` changes the number of a port it made, and
  `vss::driver_peer()` says what program has a port open.
* Auto-selection order is already: **driver → com0com → loopback** on Windows,
  and **pty** elsewhere. When the driver installs, it wins without a code
  change elsewhere.
* `vss_config` accepts `"virtual_kind": "windows-driver"`, so a saved topology
  made on a driver machine reloads on one, and fails cleanly on a machine
  without it.

## Decision points

1. **Transitional fallback** — keep com0com support behind the driver, or drop
   it once the driver ships? Keeping it costs little and helps users migrate.
2. **COM numbering** — decided: auto-assign, with an optional number asked for
   when the port is made or changed later (see "COM numbers").
3. **Driver signer** — who holds the EV certificate, and does the driver ship
   separately from the application?
4. **Open source** — the driver in this repository under GPL-3.0-or-later, or a
   separate repository? Keeping it here makes the source offer trivial.
