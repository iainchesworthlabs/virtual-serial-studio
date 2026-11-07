# Windows virtual COM port driver

This gives Windows a virtual `COMn` port with no com0com and no hardware. The
design and the reasons for it are in
[docs/windows-com-driver.md](../../docs/windows-com-driver.md).

## Two drivers, one port

There are two drivers for the same port, built from the same sources. They sit
side by side, and the person installing picks the one that suits them. Neither
replaces the other.

| | `vsscom` (kernel mode, KMDF) | `vsscom_um` (user mode, UMDF 2) |
|---|---|---|
| Runs in | the kernel | a driver host process |
| Package | `vsscom.sys`, `vsscom.inf`, `vsscomctl.inf`, two catalogs | `vsscom_um.dll`, `vsscom_um.inf`, one catalog, and the helper service `vsscom_helper.exe` |
| Signing | needs a Microsoft-trusted kernel signature to load without test signing | a user-mode package: needs a signed catalog that the machine trusts, and no kernel-mode signature |
| Ports on demand | yes, through the control device | yes, through the helper service, which runs as SYSTEM |
| Limits for on-demand ports | 16 for a user, 32 in all | the same |
| On-demand hub end | only the user who made the port | the same |
| Hand-made hub end | administrators | administrators |
| The port itself | the same: a COM name, the application end, the hub end, the serial rules | the same |

Both put their hub end on the same interface, `GUID_DEVINTERFACE_VSSCOM`, so the
studio finds the ports of either without being told which driver they belong to.
Their hardware IDs differ (`VSSCOM\VSSCOM0` and `VSSCOMUM\VSSCOM0`), so both can be
installed on one machine.

## State

The table is for the kernel-mode driver unless it says otherwise. The user-mode
driver builds in CI from the same sources; its guest results are in the section
"The user-mode driver" below.

| Piece | State |
|---|---|
| Package: `vsscom.sys`, `vsscom.inf` and `vsscomctl.inf` with a catalog each, for x64 and ARM64 | builds in CI; `InfVerif` and `Inf2Cat` pass |
| User-mode driver with its helper service: tests D1 to D13, D15 to D22, H14, and the studio client (C1) | pass in the Windows 11 and Windows 10 22H2 guests with test signing off, with the CI-signed packages, before and after a restart |
| COM name: device interface, `\DosDevices` link, `SERIALCOMM` | runs in a Windows 10 and a Windows 11 guest |
| Loopback port: waiting reads with the `SetCommTimeouts` rules, `WaitCommEvent`, purge and cancel, modem lines, one handle per end | runs in both guests (tests D1 to D7) |
| Hub end: a second handle on each port that carries its bytes, line settings and lines, with flow control both ways | runs in both guests (tests D8 to D13) |
| Control device: ports on demand, removed with the handle that made them | runs in both guests (tests D14 to D19) |
| Write timeouts, and who may open the hub end and make how many ports | tests D18, D20 and D21 |
| `SET_HANDFLOW` drives DTR and RTS: enable, disable, handshake on the receive buffer, RTS toggle | tests D23 to D25 pass for both drivers in the Windows 10 22H2 guest; the rules are also unit-tested on the host. Not yet run in the Windows 11 guest or on ARM64 |
| Revoke: the hub takes the port back from a program, and the hub state names the process | tests D26 and D27 pass for both drivers in the Windows 10 22H2 guest; not yet run in the Windows 11 guest or on ARM64 |
| COM numbers: choose one when the port is made, change it later, refuse one that is taken (the helper service, for either driver) | tests D28 to D31 and the client test C2 pass for both drivers in the Windows 10 22H2 guest (with the helper installed); not yet run in the Windows 11 guest or on ARM64 |
| Driver Verifier (standard settings) on `vsscom.sys` | the whole suite passes with it on in the Windows 11 guest |
| The studio opening a port (`create_driver_port`) | runs in a guest with `test-client.ps1` (test C1) |
| A 32-bit program (WOW64) and a hung or killed program | the client and `test-wow64.ps1` (W1 to W4, T1 to T7) are written and the client builds in CI; the guest results are in the pull request that added them, and are not yet in this table |
| Signing | not done: the package is unsigned |

## Build

You need Visual Studio 2026 with the C++ desktop workload (the driver build
targets come with it) and access to NuGet. The Windows SDK and the WDK are NuGet
packages named in `vsscom/vsscom.vcxproj`, so nothing else is installed.

```powershell
./drivers/windows/build.ps1 -Platform x64      # or ARM64
```

The kernel-mode package is written to `drivers/windows/artifacts/package/<platform>/`:
`vsscom.sys`, `vsscom.inf`, `vsscomctl.inf` and the catalogs `vsscom.cat` and
`vsscomctl.cat`. The user-mode package is written to
`drivers/windows/artifacts/package-um/<platform>/`: `vsscom_um.dll`,
`vsscom_um.inf` and `vsscom_um.cat`. The helper service is written to
`drivers/windows/artifacts/package-helper/<platform>/`: `vsscom_helper.exe` and
`install-helper.ps1`. `-Driver kmdf`, `-Driver umdf` or `-Driver helper` builds one
of them. CI runs the same script and uploads the packages as the artifacts
`vsscom-<platform>`, `vsscom-um-<platform>-unsigned` or
`vsscom-um-<platform>-testsigned`, and `vsscom-helper-<platform>-unsigned` or
`vsscom-helper-<platform>-testsigned`.

## Signing

The user-mode package is signed in CI with the organisation's test certificate, by
the same `sign-windows` action that signs the studio's programs. The leaf
certificate and its key are the organisation secrets `WIN_TESTSIGN_PFX_BASE64` and
`WIN_TESTSIGN_PFX_PASSWORD`; they reach only the CI step, never a developer's
machine or an assistant. The `SIGN_WINDOWS` repository variable chooses:

| `SIGN_WINDOWS` | The user-mode artifact |
|---|---|
| `false` (the default) | `vsscom-um-<platform>-unsigned`: nothing signed |
| `test` | `vsscom-um-<platform>-testsigned`: `vsscom_um.dll` and `vsscom_um.cat` signed with the leaf certificate and timestamped; `vsscom-helper-<platform>-testsigned`: `vsscom_helper.exe` signed the same way |

The catalog holds the hash of the DLL, so `build.ps1` runs in two stages with the
signing between them: `-Stage build`, sign the DLL, `-Stage catalog`, sign the
catalog. The helper has no catalog and is signed once.

To use the test-signed package on a machine, install the organisation's test CA
certificates there once (they are public; `docs/test-signing.md` in
`icl-common-infra` says how): the root, the intermediate, and the signer's own
certificate as a trusted publisher. `pnputil` cannot answer the question "do you
trust this publisher?", so a driver whose signer is not a trusted publisher is
refused with "the publisher of an Authenticode signed catalog has not been
established as trusted". Then install the package:

```powershell
Import-Certificate -FilePath root-ca.crt -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath intermediate-ca.crt -CertStoreLocation Cert:\LocalMachine\CA
# The signer's public certificate, read from the signature of the catalog:
$signer = (Get-AuthenticodeSignature .\vsscom_um.cat).SignerCertificate
Export-Certificate -Cert $signer -FilePath signer.cer
Import-Certificate -FilePath signer.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
pnputil /add-driver vsscom_um.inf /install
```

`trust/install-trust.ps1` does the three imports in one step, from an elevated
PowerShell, with the public certificates of the CA (it never handles a private key):

```powershell
./trust/install-trust.ps1 -RootCertificate root-ca.pem -IntermediateCertificate intermediate-ca.pem `
    -Package . -ExpectedRootSha256 <fingerprint you hold>
pnputil /add-driver vsscom_um.inf /install
./install-helper.ps1     # the helper service, from its own folder
```

It reads the signer from the signature of the package, checks that it chains to the
root you named, and only then adds it to Trusted Publishers, so a package signed by
anyone else adds nothing. It refuses to run while test signing is on, because an
install then proves nothing about the trust. `-WhatIf` shows what it would change;
`-Uninstall` takes the three certificates out again. **Trusting a root means
trusting everything that CA signs, for every program on the machine**, so use it
only on machines you own. ARM64 is untried: Windows may insist on a Microsoft
signature for the catalog there. The script was run against the CI-signed package
in the Windows 11 and Windows 10 22H2 guests with test signing off: `pnputil`
refused the package before it, and the driver and helper installed and passed the
suite after it.

Windows accepts a user-mode driver whose catalog is signed by a CA it trusts, so
**test signing mode stays off**. A CI-signed package was checked this way in the
Windows 11 guest with test signing off. The kernel-mode driver does not have that
freedom: Windows loads kernel code only if Microsoft signed it, or if test signing
mode is on. The test certificate suffices for the second case only.

## Setting up from Studio: signed and unsigned builds

A new user has installed nothing. Studio checks the PC without making a port
(`vss::probe_driver_setup`: the driver store, the service control manager, and
one request that changes nothing on the helper's pipe), and when the driver is
missing, the helper is not installed, not running, older than Studio or refuses
the user, the first screen, the Setup check and the error of a refused port all
offer one action, **Set up Virtual Serial Studio**. It runs `setup-driver.ps1`
through one Windows permission prompt (UAC). Nothing is downloaded: the script
takes the files from a `driver` folder beside the program (or the folder in
`VSS_DRIVER_PACKAGE`), which holds `setup-driver.ps1`, `driver/` (`vsscom_um.inf`,
`.dll`, `.cat`) and `helper/` (`vsscom_helper.exe`, `install-helper.ps1`). Run it
again to repair a partial install. By hand, from a terminal opened as
administrator:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup-driver.ps1 -PackageRoot <folder>
```

Before it changes anything the script checks the signature of the driver DLL, its
catalog and the helper. It never adds a certificate to the machine.

| Build | What the user sees |
|---|---|
| A release, signed with a certificate the machine trusts (a Microsoft-trusted publisher, or the owner's CA after `trust/install-trust.ps1`) | The Windows prompt names the publisher. Setup installs the driver and the helper and says "Virtual Serial Studio is set up." |
| A test-signed package (`SIGN_WINDOWS=test`) on a machine that does not trust the test CA | The prompt shows the publisher or "Unknown publisher". The script exits 6 before changing anything and Studio says "Windows does not trust the signature on the driver files, so nothing was installed." Run `trust/install-trust.ps1` with the CA's public certificates (it asks for administrator rights and prints the root's fingerprint to check), or turn on test signing (`prepare-test-signing.ps1`, in a guest or on a machine you own), then choose Set up again. |
| An unsigned development build | The same as the line above: exit 6, nothing installed. A development machine either trusts the test certificate or runs in test signing mode. With test signing on, the script accepts a signed but untrusted chain and installs. |
| No `driver` folder beside the program | The Set up button is off and the dialog says the driver files are not with this copy, and shows how to run the script by hand. |

The prompt always belongs to the PowerShell that runs the script; the script is
read from the program's own folder, so in an installed copy (Program Files) only
an administrator can change it. A development copy in a folder you own can be
changed by you, which is as it should be.

## Layout

| Path | What |
|---|---|
| `vsscom/port.c` | shared by both drivers: the COM name, the two interfaces, the file object rules, the device context and the queues of a port |
| `vsscom/io.c` | shared: the application end: reads, writes and the serial control requests, and the pump that moves bytes between the ends |
| `vsscom/hub.c` | shared: the hub end: opening, reading, writing, the state the hub watches |
| `vsscom/core/` | shared: the serial rules in plain C; built for the host and unit-tested |
| `vsscom/vsscom.h`, `vsscom/public.h` | shared: the device context and the entry points; what user mode sees (interface GUIDs, control codes, the hub state, the control requests) |
| `vsscom/driver.c` | kernel mode: driver entry, the name and type of the device object; tells a port from the control device |
| `vsscom/control.c` | kernel mode: the control device, the bus that makes ports on demand, and who made which port (for the helper) |
| `vsscom/access.c` | kernel mode: who opened a handle, and who may open a hub end |
| `vsscom/km/vsscom_platform.h` | kernel mode: the headers and the clock the shared code uses |
| `vsscom/vsscom.inf`, `vsscom/vsscomctl.inf` | kernel mode: the ports (class Ports) and the control device (class System) |
| `vsscom_um/um_driver.c` | user mode: driver entry |
| `vsscom_um/um_access.c` | user mode: who may open a hub end, by impersonating the requestor |
| `vsscom_um/um/vsscom_platform.h` | user mode: the headers and the clock the shared code uses |
| `vsscom_um/vsscom_um.inf` | user mode: the ports (class Ports) |
| `vsscom_um/um/vsscom_um.h` | user mode: how the driver reads the owner of a port the helper made |
| `vsscom_helper/main.cpp` | the helper service: the pipe, the checks, the software devices and the changes of COM number |
| `vsscom_helper/lease_table.hpp` | the helper's slot and quota rules, with nothing of Windows in them; unit-tested |
| `vsscom_helper/com_number.hpp` | the rules for COM numbers and friendly names, with nothing of Windows in them; unit-tested |
| `vsscom_helper/protocol.h` | what the studio, the helper and the user-mode driver share: the pipe, the messages, the owner property |
| `vsscom_helper/install-helper.ps1` | installs or removes the service |
| `setup-driver.ps1` | one elevated step that checks the signatures and installs or repairs the user-mode driver and the helper service; what Studio runs for "Set up Virtual Serial Studio" |
| `trust/install-trust.ps1` | makes a machine trust the owner's CA and the signer of a package, so a test-signed package installs with test signing off |
| `build.ps1` | restore, build, `InfVerif`, `Inf2Cat`, for either driver, the helper or all |
| `test/` | guest scripts, for either driver (`-Flavour kmdf` or `umdf`): `install-driver.ps1` (two ports made by hand, `-Count` for more, and for the kernel-mode driver the control device; `-Helper` installs the helper service), `test-driver.ps1` (tests D1 to D31, and H14 for the helper), `test-client.ps1` (the studio client against the driver), `test-wow64.ps1` (a 32-bit program and the hung-program tests), `hub-io.ps1` (its hub-end helpers), `client32/` (the C program they run), `uninstall-driver.ps1` |

The two drivers include their own `vsscom_platform.h`, found first on the include
path of each project, so the shared sources need no preprocessor conditionals.

## The two ends of a port

Each port is one device with two ends. The application opens `\\.\COMn` as it
would any serial port. The studio opens the hub end through the device interface
`GUID_DEVINTERFACE_VSSCOM` with the reference string `hub`, so the path ends in
`\hub`, and it opens it for overlapped I/O.

| | Application end | Hub end |
|---|---|---|
| Handles | one at a time | one at a time |
| Write | goes to the hub; waits when the 64 KB queue is full | goes to the application; waits when its 64 KB queue is full |
| Read | what the hub wrote, under `SetCommTimeouts` | what the application wrote, as soon as there is any |
| Lines | DTR, RTS out; CTS, DSR, DCD, RI in | sees DTR, RTS, break, baud rate and line control; sets CTS, DSR, DCD, RI |

`IOCTL_VSSCOM_HUB_WAIT_STATE` waits until the application changes something, so
the hub never polls. While no hub handle is open the port loops back, so a port
that nothing has attached still behaves like a port with a loopback plug.

The COM port opens for anyone the Ports class lets in, as any serial port does.
The hub end is the device side of the port, so it is narrower: a port made on
demand opens its hub end only for the user who made it, and a port made by hand
opens it only for administrators. Another user is refused with access denied,
whether or not the hub end is taken. The driver reads the user from the token
that opens the handle, with impersonation applied (`access.c`).

A write that waits for the hub obeys the write timeouts of `SetCommTimeouts`:
the total timeout (multiplier times the bytes, plus the constant) starts when
the write stalls at the head of the queue, and a write that runs out completes
with the bytes it had taken, which stay queued for the hub. With no write
timeout set, a write waits for the hub for as long as it takes.

`IOCTL_SERIAL_SET_HANDFLOW` is applied to DTR and RTS. DTR control enable or
disable drives the line when the settings are given; handshake makes it follow
the receive buffer (low when `XoffLimit` bytes or fewer are free, high again at
`XonLimit` bytes waiting) and then `SET_DTR` and `SET_RTS` on it fail with
`STATUS_INVALID_PARAMETER`, as on `serial.sys`; RTS toggle holds RTS high from
the first queued byte until the last has had time to leave at the baud rate and
frame in force. The settings go back to none each time the program closes the
port. Stopping output on CTS or DSR, and XON/XOFF, are not applied. The rules
are in `vsscom/core` and are unit-tested.

The hub state names the process that has the COM port open (`AppProcessId`).
`IOCTL_VSSCOM_HUB_REVOKE`, on the hub handle, takes the port back from that
program: its waiting reads, writes and `WaitCommEvent` end with
`STATUS_DEVICE_REMOVED`, every later request on its handle fails the same
way, `AppOpen` clears, the port resets as a close would reset it, and another
program can open it at once. The old handle stays in the process, and closing it
later changes nothing. Only the user who owns the port has a hub handle, so only
that user can revoke (`access.c`). Both are version 3 of the interface
(`VSSCOM_VERSION`); the studio refuses a driver of version 2.


## Ports on demand

Install the package once and the control device appears in Device Manager under
System devices as "Virtual Serial Studio Port Control". It makes no port by
itself. A program opens its interface `GUID_DEVINTERFACE_VSSCOMCTL`, sends
`IOCTL_VSSCOM_CTL_CREATE_PORT`, and Windows builds a device
`VSSCOM\PORT\<slot>` that `vsscom.inf` drives. A second or two later the port has
its `COMn` name and its two interfaces, and the program opens the hub end as
above. The control requests and their structures are in `public.h`.

| Rule | Why |
|---|---|
| A port lives as long as the handle that made it | A studio that crashes leaves no port behind. Closing the handle, or the process ending, removes the port. |
| `IOCTL_VSSCOM_CTL_DESTROY_PORT` works on the caller's own ports only | One program cannot remove another's port. |
| A slot is the lowest free number, from 0 to 31 | A port that comes back gets the same device node, so the same COM name. |
| At most 32 ports exist at once, and one user holds at most 16 | The first is a limit for the whole machine, the second stops one user taking every slot. A slot is free again only when Windows has removed the device. |
| Only the user who made a port can open its hub end | Another user cannot take the device side of the port. |
| The control device is open to SYSTEM, administrators and signed-in users | A normal user can make a port without elevation. |
| `IOCTL_VSSCOM_CTL_GET_OWNER` answers SYSTEM and administrators only | The helper service uses it to check who made a port before it changes the port's COM number; other users cannot read who owns a port. |

The slot, and not a name, identifies the port while Windows builds it, because
the `COMn` name comes from the Ports class installer at the end of the build.
Programs find the port by its device instance id.

The COM number is not part of the create request, which keeps `Flags` and
`Reserved` at zero: a number can only be changed on a port that exists, by a
program with administrator rights. The helper service does it for ports of either
driver; see "COM numbers" below and `docs/windows-com-driver.md`.

## Ports on demand for the user-mode driver: the helper service

A user-mode driver cannot make child devices, so `vsscom_um` has no control
device. Its ports are software devices, which only SYSTEM and administrators may
make. The `vsscom_helper` service runs as SYSTEM and makes them for ordinary
users, with the same rules as the control device. The studio asks the control
device first, then the helper, then takes a free port that already exists.

```powershell
# As an administrator, from the folder that holds the helper package:
./install-helper.ps1            # copies vsscom_helper.exe to Program Files, starts the service
./install-helper.ps1 -Uninstall # stops it, removes it and its folder
```

| Rule | How it is kept |
|---|---|
| Any signed-in user may ask; no one on the network may | The pipe `\\.\pipe\vsscom_helper` lets SYSTEM and administrators do everything and signed-in users read and write, and refuses remote clients. |
| A port lives as long as the pipe that asked for it | The helper holds the device and closes it when the pipe closes, whether the program closed it or died. Stopping the service removes every port it made. |
| At most 32 ports exist at once, and one user holds at most 16 | The same numbers as the control device. The user is the SID read from the pipe, so an elevated and an unelevated session of one person share the 16. |
| The lowest free slot, from 0 to 31 | The instance id of the device is the slot, so a port that comes back has the same COM name. |
| Only the user who asked can open the hub end | The helper puts the SID on the device as a property; the driver compares it with the user of the opener. A port with no owner was made by hand and opens for administrators. |
| The request is answered when the driver has started on the device | A machine that has the helper and no user-mode driver gets an error at once (`ERROR_DEV_NOT_EXIST`), and the studio falls back to the ports that exist. |

The helper runs `vsscom_helper.exe --console` in the foreground (elevated) for a
trial. It writes only short lines to the debugger. The protocol is in
`vsscom_helper/protocol.h`.

## COM numbers

A port takes the lowest free COM number when Windows builds it, and keeps it for
the slot from then on. To choose one, as Device Manager's *Advanced > COM Port
Number* does, ask the helper service (`vsscom_helper`, installed with
`install-helper.ps1`; it serves ports of either driver):

| Request | What it does |
|---|---|
| `VSSCOM_HELPER_OP_SET_COM_NUMBER` | The port is one of the user-mode driver's that this pipe made (`Slot`, `Argument` = the number). |
| `VSSCOM_HELPER_OP_SET_COM_NUMBER_KM` | The port is one of the kernel-mode driver's. The helper asks the control device who made it (`IOCTL_VSSCOM_CTL_GET_OWNER`) and compares that with the caller. |

| Rule | How it is kept |
|---|---|
| 1 to 255 | The COM name database holds 256 ports by default. |
| A number another device has is refused (`ERROR_ALREADY_ASSIGNED`) | The helper claims the number in the COM name database without forcing it, and also refuses a number whose `\\.\COMn` already resolves. Real ports, com0com and ports that have gone but keep their name are all covered. |
| Nothing changes unless all of it works | The number is claimed, `PortName` and the friendly name are written, the port is restarted and its name checked; a failure puts the old values back. The old number is released last. |
| A program that has the port open makes it fail (`ERROR_BUSY`) | The helper opens the COM port itself for an instant before it restarts. A restart with a handle open fails and leaves the port unable to restart until it is removed and made again, so it is not tried. The hub handle must be closed by the caller; `create_driver_port` and `set_driver_com_number` in the studio do that. |
| Only the user who made the port may change it | A user-mode port must be the pipe's own; a kernel-mode port's maker must be the caller's SID. |
| The reply comes when the port is running again under the new name | The studio can open the hub end at once. |

A `PortName` written into the device's key before the port is built does not
work: the install then fails. The change is therefore always made on a port that
exists.

## Try it in a guest

Only in a disposable test guest that reverts to a clean snapshot. Copy the
package (`vsscom.sys`, `vsscom.inf`, `vsscomctl.inf` and the two catalogs) and
the scripts from `test/` into one folder in the guest, then, in an elevated
PowerShell:

```powershell
./prepare-test-signing.ps1 -ConfirmDisposableGuest -EnableTestSigning   # makes a throwaway certificate, signs, turns on test signing
# restart the guest
./install-driver.ps1 -ResultPath C:\vss\install.json
./test-driver.ps1 -ResultPath C:\vss\test.json
./uninstall-driver.ps1 -ResultPath C:\vss\uninstall.json
```

`prepare-test-signing.ps1` creates the certificate in the guest with a key that
cannot be exported and trusts it only there; the revert discards it. Test
signing needs Secure Boot off. A release package is signed with an organisation
certificate instead, which is a separate step.

## Old programs: a 32-bit client and the hung-program tests

The programs this driver has to serve are old 32-bit Windows programs, and they
sometimes hang and then cannot start again until the machine restarts. Two things
are tested, with one small C program, `test/client32/vss_com_client.c`, built three
ways (Win32, x64, ARM64) so a 32-bit run has a native one to be compared with.

**There is no 32-bit guest.** The driver is x64 and ARM64 only, and a 32-bit
Windows cannot load an x64 driver. An x86 driver build does not exist and is not
planned. A "32-bit program" here is the Win32 build of the client running under
WOW64 in the usual 64-bit Windows 10 and Windows 11 guests, which is what a 32-bit
program on a 64-bit machine does. The serial requests carry no pointers, so WOW64
passes them straight through; `test-wow64.ps1` is the proof that it does.

The client has four modes: `probe` (opens `COM1` to `COM16`, with and without the
`\.\` prefix), `open` (one open, to see what a second program gets), `exercise`
(`SetupComm`, `SetCommState` with the DTR and RTS control modes, `SetCommTimeouts`
in the usual combinations, `GetCommProperties`, `GetCommConfig`,
`EscapeCommFunction`, overlapped `WaitCommEvent`, `ReadFile` and `WriteFile`,
`PurgeComm`, the modem status) and `hold` (leaves requests pending and stays alive).
Each prints one JSON document. Build it with
`cmake -S drivers/windows/test/client32 -B build/client32-x86 -A Win32` and
`cmake --build build/client32-x86 --config Release`; CI builds it in the driver job
and uploads it as `vss-com-client-x64` (Win32 and x64) and `vss-com-client-ARM64`.

`test-wow64.ps1` runs inside a guest after `install-driver.ps1` (add `-Flavour umdf`
and `-Helper` for the user-mode driver). Its hub end is the echo of `hub-io.ps1`,
which sends back what the program writes and shows DTR as DSR and DCD and RTS as CTS.

| Test | What it does |
|---|---|
| W1 | the 32-bit probe of `COM1` to `COM16`, with the target free and while another program holds it, and the 64-bit probe for comparison |
| W2, W3 | the whole exercise with the 32-bit build, and with the native build |
| W4 | the two builds' answers agree, step by step |
| T1 | a pending read, a pending event wait and a write stalled on the full 64 KB queue; the program is killed. `AppOpen` must clear in under a second and the port reopen |
| T2 | the same with the program suspended, which is the hang: records what a second open returns (access denied is expected), then kills it |
| T3 | a synchronous read with no timeout; the program is killed |
| T4 | the handle in the suspended program is closed from outside (`DuplicateHandle` with `DUPLICATE_CLOSE_SOURCE`, then `CancelIoEx`): does the port come free without killing the program |
| T5 | the driver's revoke request on the suspended program. Skipped until the driver has `IOCTL_VSSCOM_HUB_REVOKE` (hub end, no input, no output) in `vsscom/public.h`; ship `public.h` next to the script, or pass `-RevokeCode` |
| T6 | the port is removed while a handle is held and made again: the slot and COM name that come back are recorded |
| T7 | the helper service is restarted with a program attached (user-mode driver only) |

A test ends `pass`, `fail`, `skip` or `unexpected`. `unexpected` is for a result the
design did not predict, such as a second open that succeeds or a new port that takes
the old COM name; nothing the test was built to prove has failed, and the detail says
what happened. T1 to T3 fail only when the port is not free within a second of the
kill, or does not reopen: those are the cases where the driver itself holds a port.
If they pass, a hang that needs a reboot is not the driver, and T2 and T4 say what
can still be done to a program that is alive but stuck.

`tools/vm/make-driver-bundle.ps1` packs the packages, the scripts and the client into
one zip, and `tools/vm/guest-cell.ps1 -DriverBundle` runs it in a guest from the clean
snapshot (see `docs/vm-testing.md`).
