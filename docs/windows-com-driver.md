# Replacing com0com: virtual COM ports on Windows

The goal is for Virtual Serial Studio to be the only thing a user installs. On
Linux and macOS that is already true: the application creates real pseudo
terminals. On Windows it needs a driver, and the repository now has two, built
from the same sources.

This note explains why a driver is needed, what it does and why it is shaped
the way it is. How to build, install and test it, the layout of `drivers/windows`
and the state of each piece are in
[`drivers/windows/README.md`](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/drivers/windows/README.md).

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
* **It must be signed.** A kernel-mode driver needs a Microsoft signature on any
  machine that is not in test-signing mode. A user-mode driver needs a catalog
  signed by a publisher the machine trusts.

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

Two gaps remain: the name changes on every run, and programs that offer a list
of detected ports do not list a PTY. [Virtual ports on Linux and
macOS](posix-virtual-ports.md) describes the stable names that `vss_cli vport`
makes and what other programs see.

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
| UMDF serial device | First judged unworkable; it turned out to work. `vsscom_um` is a UMDF 2 driver for the same port, built from the same sources. It needs no kernel-mode signature, which is why it is offered beside the kernel-mode driver. It cannot be a bus, so its ports are made by a helper service (see [The user-mode driver and its helper](#the-user-mode-driver-and-its-helper)). |
| **KMDF virtual serial function driver** | **Built** (`vsscom`). A control device creates ports on demand as child devices; each port implements the serial contract and passes bytes to the studio through a second handle on the same device. |

## Architecture

Each port is one device with two ends. The application opens `\\.\COMn` as it
would any serial port. The studio opens the second end, the hub end, through the
device interface `GUID_DEVINTERFACE_VSSCOM` with the reference string `hub`.
Bytes the application writes are read from the hub end, and bytes written to the
hub end are what the application reads.

```
  vss_gui / vss_cli
    vss::studio -- create_driver_port()
       |  asks for a port                         |  opens the hub end
       v                                          |  (overlapped I/O)
  control device of vsscom.sys                    |
  or vsscom_helper (SYSTEM) for vsscom_um         |
       |  makes the device VSSCOM\PORT\<slot>     |
       v                                          v
  +------------------------ one port ------------------------+
  |  application end \\.\COMn  <-- two 64 KB queues -->  hub end |
  +---------------------------+-------------------------------+
                              v
                        applications
```

With no hub handle open the port loops back, as a port with a loopback plug
does. `drivers/windows/README.md` has the table of the two ends.

### Control device

The kernel-mode driver, `vsscom.sys`, also serves a second, small device that
makes ports on demand. It is installed once from its own INF (`vsscomctl.inf`,
class System, because it is not a port and must not get a COM name). A program
opens its interface `GUID_DEVINTERFACE_VSSCOMCTL`.

| IOCTL | Purpose |
|---|---|
| `IOCTL_VSSCOM_CTL_GET_VERSION` | The version of the control interface and the most ports that can exist |
| `IOCTL_VSSCOM_CTL_CREATE_PORT` | Make a port; returns its slot |
| `IOCTL_VSSCOM_CTL_DESTROY_PORT` | Remove a port the caller made |
| `IOCTL_VSSCOM_CTL_LIST_PORTS` | The slots in use, and which of them this handle made |
| `IOCTL_VSSCOM_CTL_GET_OWNER` | Who made a port; SYSTEM and administrators only, for the helper service |

The structures are in `drivers/windows/vsscom/public.h`. The create request
holds only `Flags` and `Reserved`, both zero. Three choices were made on purpose,
each with a reason:

* The control device is a **bus**, not a named control object. A request to
  create a port reports a child device to the Plug and Play manager, which
  installs `vsscom.inf` on it and starts the port like any other. The COM name
  then comes from the Ports class installer, and the port shows in Device
  Manager and `SERIALCOMM` as every serial port does.
* A port lives as long as **the file handle that made it**, with no
  configuration. The process ending is the usual way a port goes, so the driver
  does not rely on a program to clean up.
* The **slot** is the lowest free number and is the device's instance id, so a
  port that returns gets the device node and COM name it had. A COM number can
  be chosen and changed through the helper service; see [COM numbers](#com-numbers).

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
studio, and a helper that refuses the user would all come back as the same few
codes, and the GUI would show "permission denied". `vss::probe_driver_setup()` asks
each piece on its own and makes no port: the driver store (the hardware ID key
under `HKLM\SYSTEM\DriverDatabase\DeviceIds`, readable by every user), the service
control manager, and one request on the helper's pipe that changes nothing (a
COM number of 0, which a current helper answers `ERROR_INVALID_PARAMETER` before
it looks at anything, an older one `ERROR_NOT_SUPPORTED`, and one that cannot
tell who the caller is `ERROR_ACCESS_DENIED`). `classify_driver_setup()` names the
first piece that is wrong, in the order a person fixes it. The GUI turns each into
its own sentence and, for the ones a setup fixes, the action "Set up Virtual
Serial Studio", which runs `drivers/windows/setup-driver.ps1` through one Windows
permission prompt (`drivers/windows/README.md`, "Setting up from Studio"). The code
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

Three behaviours sit on top of the plain port. Their rules are in
[`drivers/windows/README.md`](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/drivers/windows/README.md),
under "The two ends of a port", and are not repeated here.

* `IOCTL_SERIAL_SET_HANDFLOW` drives DTR and RTS (enable, disable, handshake on
  the receive buffer, RTS toggle). Output flow control is not applied. The rules
  are plain C in `vsscom/core`, with unit tests.
* `IOCTL_VSSCOM_HUB_REVOKE` takes the port back from the program that has it, so
  that a hung program does not keep a port until the machine restarts. Only the
  user who owns the port has a hub handle, so only that user can revoke.
* The hub state names the process that has the COM port open (`AppProcessId`).
  The studio logs each change of `AppOpen` against the endpoint with the time
  and the process, and `vss::driver_peer` and `vss::peer_open` answer from it.
  This came with version 3 of the port interface.
* `IOCTL_VSSCOM_HUB_LINE_ERROR` raises a break, a framing, parity or overrun
  error, or a full receive queue at the program; see [Line errors](#line-errors).

### Data path

The drivers use no shared memory. Each port has two 64 KB queues inside the
driver, one for each direction, with flow control, so `ReadFile` and `WriteFile`
on the hub handle replace per-byte requests. A read on the application end
consumes what is queued and waits when the queue is empty, under the rules of
`SetCommTimeouts`. A write on either end copies what fits and waits for space.

A shared section holding two single-producer, single-consumer rings, as a data
path without a system call per transfer, was the original idea. It is not built.
It is a later step, if the queues turn out to be too slow.

### Serial semantics

The driver answers the `IOCTL_SERIAL_*` requests that applications send before
they exchange a byte. The common ones are implemented:

| Area | What the driver does |
|---|---|
| Queues | `SET_QUEUE_SIZE`, `PURGE` (aborts and clears the queues), `GET_COMMSTATUS` (the queue counts, and the line errors the hub raised, which it clears) |
| Configuration | `GET/SET_BAUD_RATE`, `GET/SET_LINE_CONTROL`, `GET/SET_CHARS`, `GET_PROPERTIES`, `CONFIG_SIZE`. The baud rate, data bits, parity and stop bits are kept in the hub state, so the studio sees what the program chose |
| Timeouts | `GET/SET_TIMEOUTS` with `ReadIntervalTimeout`, `ReadTotalTimeoutMultiplier` and `ReadTotalTimeoutConstant`; the driver owns the timers, or applications block forever. Write timeouts are honoured too |
| Modem lines | `SET/CLR_DTR`, `SET/CLR_RTS`, `GET_DTRRTS`, `GET_MODEMSTATUS` and `GET/SET_HANDFLOW`. The application's DTR, RTS and break show in the hub state; the hub sets CTS, DSR, DCD and RI with `IOCTL_VSSCOM_HUB_SET_LINES` |
| Break and flow | `SET_BREAK_ON/OFF`, `SET_XOFF/XON`, `IMMEDIATE_CHAR` |
| Events | `SET/GET_WAIT_MASK` and `WAIT_ON_MASK`. `EV_RXCHAR`, `EV_TXEMPTY` and the line events (`EV_CTS`, `EV_DSR`, `EV_RLSD`, `EV_RING`) are raised. `EV_ERR` and `EV_BREAK` are raised by the line errors the hub injects, and `EV_RXFLAG` when the event character arrives (see [Line errors](#line-errors)) |
| Errors | `ClearCommError` reports and clears the injected errors; `fErrorChar` and the break character replace the bad byte; `fAbortOnError` ends reads and writes until `ClearCommError` |
| Lifecycle | Pending requests are cancelled by a purge and when the handle is closed |

The timeout rules are the subtle part and the usual reason a home-grown
virtual port fails: a `ReadFile` with `ReadIntervalTimeout = MAXDWORD`,
`ReadTotalTimeoutMultiplier = 0` and `ReadTotalTimeoutConstant = 0` means "return
immediately with whatever is buffered", while other combinations mean "wait for
the first byte, then for a gap". The driver implements all of them, in
`vsscom/core`, with unit tests.

### Line errors

A program learns of a bad line through `ClearCommError`, `EV_ERR` and `EV_BREAK`,
the error character and `fAbortOnError`. A virtual port has no line to go bad, so
the hub end says when one does. The rules below are in `vsscom/core`
(`vsscom_line_error_*`, `vsscom_errors_*`) with unit tests, and both drivers
carry them out with the same `io.c` and `hub.c`.

**Where the rules come from.** The constants were checked against `ntddser.h`
and `winbase.h` of SDK and WDK 10.0.28000. The behaviour follows the serial
driver sample Microsoft publishes (`SerialProcessLSR`, `SerialPutChar` and
`SerialCompleteIfError` in `Windows-driver-samples/serial/serial`), which is the
nearest public statement of what the in-box `serial.sys` does. Nothing here was
compared with a hardware port, and where the sample leaves room the narrower
reading was taken; those places are marked "choice".

**The request.** `IOCTL_VSSCOM_HUB_LINE_ERROR` on the hub handle, with
`VSSCOM_HUB_LINE_ERROR { Errors, HasByte, Byte }`: one bad character time.
`Errors` holds one or more of break, framing, overrun, queue overrun and parity,
with the values of `SERIAL_ERROR_*`. `HasByte` and `Byte` give the character the
receiver made of it, if any. `vss::raise_driver_line_error` sends it.

**Order.** The request waits in the queue that holds the hub's `WriteFile`
requests, and is served in its turn. So an error takes effect after every byte
the hub wrote before it has entered the receive buffer, and before any byte
written after it. A read that was already waiting is given those earlier bytes
first: when bytes and an error reach the head of the queue in the same pass, the
driver serves the reads before it raises the error. Like a write, the request
waits while the receive buffer has no room for the bytes the error adds. It
completes when the error is latched, its bytes are in the buffer and its events
are out; reads that an abort on error ends are completed just after it. From the studio the order is that of
the calls on the transport: an error follows the bytes the transport has already
handed to the driver.

**What enters the stream.** The byte, when the hub gave one, is read by the
program in its place, unless the program's settings replace it:

| Error | `SERIAL_ERROR_CHAR` clear (`fErrorChar` off) | `SERIAL_ERROR_CHAR` set |
|---|---|---|
| Parity, framing | the byte, if any | `ErrorChar` in place of the byte, and also when there is no byte |
| Overrun | the byte, if any | `ErrorChar` (for the character that was lost), then the byte, if any |
| Break | the byte, if any; `BreakChar` in its place when `SERIAL_BREAK_CHAR` is set | the same: the break character is its own setting |
| Queue overrun | nothing | nothing |

A break together with a parity or framing error is treated as a break, as in the
sample. Choice: a break adds no NUL of its own; a hub that wants the NUL a UART
delivers with a break sends byte 0. Choice: a queue overrun does not overwrite
the last buffered byte with `ErrorChar`, which the sample does; the receive
buffer of this driver never overflows (a hub write waits for room), so the error
is only what the hub says it is. Choice: an error is raised whatever the line
settings are, for example a parity error with parity off; the hub is the wire.
`SetCommState` sets `SERIAL_ERROR_CHAR` from `fErrorChar`; `SERIAL_BREAK_CHAR`
has no `DCB` field and needs `IOCTL_SERIAL_SET_HANDFLOW`.
The error, break and event characters (`IOCTL_SERIAL_SET_CHARS`) and the
switches in the flow settings go back to a new port's values, all 0, each time
the program closes the port, with the other settings that reset then, so the
next program does not inherit them (test D39).

**What is latched.** The error bits, until `IOCTL_SERIAL_GET_COMMSTATUS`
(`ClearCommError`) returns them in `SERIAL_STATUS.Errors`, which clears them.
Several errors add up into one report. A purge, a new wait mask and a hub that
closes leave them; the program closing the port (or a revoke) clears them. An
error raised while no program has the port open is dropped, as on a port whose
receiver is off, and the request still succeeds.

**Events.** Framing, parity and overrun raise `EV_ERR`; a break raises
`EV_BREAK`; a queue overrun raises no event (the sample raises `EV_ERR` only for
the three line-status errors). A byte the error adds raises `EV_RXCHAR`. They
follow the rule every event has: an event in the mask completes a pending
`WAIT_ON_MASK`; with none pending it is kept and the next wait returns at once;
`SET_WAIT_MASK` (including a mask of 0) drops what was kept and completes a
pending wait with no events; an event outside the mask is not kept. Dropping the
event does not drop the error: `ClearCommError` still reports it.

An event is not lost to a cancelled wait. When a program cancels its
`WaitCommEvent` (`CancelIoEx`, or a thread that ends) and an event in the mask is
raised before the next wait, the event waits in the history, as it does when no
wait was ever made: it is collected by the next wait, or dropped by
`SET_WAIT_MASK`. That includes an event that was raised just as the wait was
cancelled and had been given to the wait: if the framework had already taken the
request, the driver puts the events back (`VssComFinishWait`) instead of
reporting them to nobody. Like `serial.sys`, the history is only emptied by a
wait that collects it or a new mask.

**`EV_RXFLAG`** is raised when `SERIAL_CHARS.EventChar` is among the bytes that
enter the receive buffer, from the hub or from the loopback. As in the sample
there is no switch for it other than the mask, and the default event character
is 0. A replacement character does not raise it.

**Abort on error.** While `SERIAL_ERROR_ABORT` (`fAbortOnError`) is set and any
error is latched, every request on the application handle except
`IOCTL_SERIAL_GET_COMMSTATUS` completes with `STATUS_CANCELLED`
(`ERROR_OPERATION_ABORTED`), as `SerialCompleteIfError` does: reads, writes and
the other control requests, `SetCommMask` and `PurgeComm` among them. The reads
and writes that were waiting end the same way when the error arrives. A pending
`WaitCommEvent` is not ended by it; the event completes it if the mask has it.
`ClearCommError` reads the error and the port works again. The state is
recomputed from the setting and the latch each time, so turning the setting on
with an error latched holds the port at once, and closing the port releases it.

**What an aborted write leaves behind.** The sample's `SerialCompleteIfError`
completes the request with `STATUS_CANCELLED` and `Information` 0, and the
sample's cancel and purge paths (`SerialTryToCompleteCurrent`, which sets
`Information` to 0 for `STATUS_CANCELLED`) do the same for the write that is
running. The sample has no queue behind the write: it sends from the program's
buffer, so a cancelled write leaves nothing but what the UART already holds,
which goes out on the wire. `ntddser.h` says nothing more (`SERIAL_ERROR_ABORT`
is only the switch). This driver does have a queue, the 64 KB transmit buffer
that the hub reads, and a write that waits for room has put part of itself in
it. The rule is: the program was told the write failed, so what the hub has not
read of it is taken back, and what the hub has read was sent. The request
completes with `STATUS_CANCELLED` and `Information` equal to the bytes the hub
read, which is the number actually sent; with the sample's wording, it is the
UART's contents that went out. Bytes of earlier writes that completed stay in the
queue, since those writes succeeded. The write's bytes are always the newest in
the queue (nothing is queued behind the write that waits, and the hub reads the
oldest first), so taking back is dropping the newest of them
(`vsscom_ring_drop_newest`). Choice: this applies to abort on error only. A write
that times out, is cancelled by `CancelIoEx` or is ended by `PurgeComm` keeps
the behaviour it had, and a timed-out write still reports the bytes it handed to
the queue.

**Locks.** There is one lock per port (`Lock`: a spin lock in the kernel-mode
driver, a framework wait lock in the user-mode one, behind `VSSCOM_LOCK`). It
guards the latch, the rings, the settings and the event state. It is never held
while a request is completed or a queue is called, and no other lock is taken
with it, so there is no order to get wrong. The error request adds no queue and
no lock: it is parked in the hub write queue, which has no cancel callback, so
the framework completes it when the hub handle closes, as it does a write.

The framework cancels those requests only after the cleanup callback of the hub
handle, and that callback runs the pump, with the receive buffer just emptied.
The first guest run showed what that does: a line error that had been waiting
for room was raised by a hub that had gone. A closing hub handle is therefore
marked (`Closed` in its file context, set under the lock), and the pump ends the
requests of a marked handle with `STATUS_CANCELLED`. The same held for a hub
write that was waiting, whose bytes went into the loopback buffer; it ends the
same way now.

A waiting `WAIT_ON_MASK` sits alone in a manual queue. Exactly one party takes
it out: the driver, with `WdfIoQueueRetrieveNextRequest` (an event, a new mask, a
close, a revoke), or the framework, which calls `EvtIoCanceledOnQueue` for a
cancel or a closing handle. A request the driver has retrieved is the driver's to
complete and the framework no longer cancels it through the queue; a request the
framework cancelled is no longer in the queue, so the retrieve finds nothing.
Either way it is completed once, and a request that neither side has taken is
still in the queue, where the handle's cleanup finds it. The injecting request
and `ClearCommError` meet only under the lock: an error is either latched before
the status is read, and reported, or after, and reported next time.

An event that is raised between the driver clearing "waiting" under the lock and
its retrieve, while the framework cancels that same wait, is taken out of the
history; the retrieve then finds nothing. The driver puts those events back
into the history under the lock, or into a wait that was parked since, so
the next wait collects them (see Events above). Test D40 checks that in every
round of the cancel race the event is seen by the cancelled wait or by the next.

**Rejected.** A separate queue for errors, with a sequence number to place them
among the bytes: more state for the same order the write queue already gives. An
error flag carried inside a hub write (a header in the byte stream): it would
change the meaning of `WriteFile` on the hub handle. Completing the request at
once and applying the error later: the hub could not tell when it had happened,
and a closing hub would leave an error behind. A fallback for a version 3
driver: the project has no installed base to protect.

### Versions

* Every request is a `CTL_CODE(FILE_DEVICE_VSSCOM, ...)` with `METHOD_BUFFERED`.
  The structures are in `drivers/windows/vsscom/public.h`, which the driver, the
  helper service and `vss_ports` all include.
* `IOCTL_VSSCOM_GET_VERSION` answers with `VSSCOM_VERSION`, now 4: version 2
  added the hub end, version 3 added `AppProcessId`, the revoke request and
  the effect of the DTR and RTS settings of `SET_HANDFLOW`, and version 4 added
  the line error request with the error bits, events and abort on error that go
  with it. The studio opens a port only when the version is exactly the one it
  was built for, so a studio and a driver from different sides of this change do
  not work together: install the driver that came with the studio.
* The control interface has a version of its own, `VSSCOM_CTL_VERSION`, now 1.
  `IOCTL_VSSCOM_CTL_GET_OWNER` was added with version 3 of the ports; an older
  driver answers `STATUS_INVALID_DEVICE_REQUEST`.
* The helper's pipe has its own protocol in `vsscom_helper/protocol.h`. A helper
  from before COM numbers answers `ERROR_NOT_SUPPORTED`, which the setup check reports as a helper older than Studio.

### Signing and distribution

| Stage | Requirement |
|---|---|
| Development | A guest with test signing on (`bcdedit /set testsigning on`), a throwaway certificate, `inf2cat`, `signtool`, `pnputil /add-driver`. `drivers/windows/test/prepare-test-signing.ps1` does it in a disposable guest |
| User-mode driver | A catalog signed by a certificate the machine trusts. CI signs it with the organisation's test certificate when `SIGN_WINDOWS` is `test`, and `drivers/windows/trust/install-trust.ps1` makes a machine trust it with test signing off |
| Kernel-mode driver, release | Windows loads kernel code only if Microsoft signed it or test signing is on. That needs an EV code-signing certificate and a Microsoft Hardware Dev Center account; attestation signing is enough (WHQL/HLK is not required for attestation). Not done: the kernel-mode package is unsigned |
| Architectures | x64 and ARM64 are built, `InfVerif` and `Inf2Cat` pass. ARM64 is built but untried in a guest |
| Installer | The Studio installer ([releasing](releasing.md#the-windows-installer)) sets up the user-mode driver and the helper service by running `setup-driver.ps1` and removes them with `remove-driver.ps1`; it installs them only where Windows trusts their signature, and never adds a certificate or turns on test signing. The kernel-mode driver is not in it. `drivers/windows/setup-driver.ps1` installs or repairs the user-mode driver and the helper service from the files beside the program, and Studio offers it as "Set up Virtual Serial Studio". The kernel-mode package is added with `pnputil`, as the guest script `install-driver.ps1` does |
| GPL | The driver is GPL-3.0-or-later like the rest of the project; shipping the binary means offering its source, and this repository is that offer |

The certificate is the long-lead item for the kernel-mode driver: procurement
and identity validation can take weeks, and Windows 10 and later will not load
an unsigned kernel driver without test signing.

### Signing the studio's Windows programs

The programs people start are signed in CI by the same mechanism as the driver
package: the shared `sign-windows` action of `icl-common-infra`, behind the
`SIGN_WINDOWS` repository variable. Today that action signs with a leaf
certificate from the owner's personal certificate authority (`test`), which is
installed on the owner's machines and guests; that is enough while the studio is
only run there. Its `production` mode, for a certificate that Windows trusts
everywhere, is reserved for later. The repository wraps it in
`.github/actions/sign-windows-binaries`, which signs, then runs
`tools/ci/verify-windows-signatures.ps1` and fails the leg when a file is
unsigned, altered after signing or has no timestamp.

| Program | Where it is signed | When |
|---|---|---|
| `vss_gui.exe`, `vss_cli.exe`, any `vss_*.dll` | `release.yml`, job `package`, before the zip and the installer are made | A tag, a manual run and the Monday run, never a pull request |
| The NSIS installer `.exe` | `release.yml`, job `package`, after it is built from those signed files | The same runs. Its manifest asks for administrator rights, which the check accepts for an installer only |
| `vsscom_um.dll`, `vsscom_um.cat`, `vsscom_helper.exe` | `ci.yml`, job `driver` | Wherever that job runs (a merge-queue entry that changes the driver, every push to `develop`, a manual run), except for a pull request from a fork |

The Qt libraries in the package are the Qt Company's and ship as built. The
release is a zip, which cannot carry an Authenticode signature, so the files in
it are signed one by one. The kernel-mode driver is not signed (see above).

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

**Where the certificate comes from.** The organisation secrets
`WIN_TESTSIGN_PFX_BASE64` and `WIN_TESTSIGN_PFX_PASSWORD` are shared with every
repository of the organisation, and the repository variable `SIGN_WINDOWS` is
`test`, so the legs sign with the same test certificate as the other
repositories. No new secret is needed. What the step does without the secrets
is in [Windows signing](ci.md#windows-signing).

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

## Status of the plan

The plan began as five phases. What landed, and what did not:

| Phase | Scope | State |
|---|---|---|
| 0 | Application interface, detection, configuration, docs | Done |
| 1 | A driver that creates a port and echoes bytes | Done, with a hub end and two queues in place of shared rings (see [Data path](#data-path)) |
| 2 | The serial contract: timeouts, purge, wait mask, line control, cancellation | Done. Line errors are raised when the hub injects them (see [Line errors](#line-errors)) |
| 3 | INF, installer, upgrade and uninstall, version handshake | INFs, the version handshake, `setup-driver.ps1` and `install-helper.ps1` (with `-Uninstall`) exist, and the guest scripts install and remove the packages. There is no MSI |
| 4 | Signing, the guest test matrix, Driver Verifier | Guest tests and Driver Verifier (standard settings, kernel-mode driver) pass. The user-mode package is test-signed in CI. The kernel-mode package is not signed |

Driver development needs a machine where test signing is enabled and a reboot is
acceptable. GitHub-hosted runners cannot reboot mid-job, so the driver is built
and packaged in CI and run in VMware guests; see
[Testing in the VSS guests](vm-testing.md).

## What is already in place

* `vss::virtual_port_kind::windows_driver` and `vss::supports(...)`, so the GUI,
  the CLI and stored topologies understand the kind.
* `vss::driver_available()` and `driver_version()`: a list of the driver's
  device interfaces, which opens nothing, and the interface version.
* `vss::create_driver_port()`: asks the control device, or the helper service
  of the user-mode driver, for a port of its own (or, with neither, takes a free
  port that exists), opens its hub end for overlapped I/O and returns the
  transport that carries its bytes and control lines. Closing the transport
  removes a port it made. It reports `errc::unsupported` with no driver,
  `errc::device_busy` when there is no control device and every port is taken,
  `errc::port_exhausted` when the control device has no slot left,
  `errc::permission_denied` and `errc::timeout`. With a `com_number` it also
  reports `errc::already_exists` when another device has the number and
  `errc::invalid_argument` outside 1 to 255.
* `vss::set_driver_com_number()` changes the number of a port it made,
  `vss::revoke_driver_port()` takes a port back from the program that has it,
  `vss::raise_driver_line_error()` raises a line error at that program, and
  `vss::driver_peer()` says what program has a port open.
* `vss::probe_driver_setup()` and `classify_driver_setup()`, described above.
* Auto-selection order on Windows is **driver, then com0com, then the
  in-process loopback device**; Linux and macOS use a **pty**. A com0com pair is
  used only when there is no driver, or when the driver could not make a port and
  no COM number was asked for.
* `vss_config` accepts `"virtual_kind": "windows-driver"`, so a saved topology
  made on a driver machine reloads on one, and fails cleanly on a machine
  without it.

## Decision points

1. **Transitional fallback.** Decided: com0com support stays behind the driver.
   It costs little and helps users migrate.
2. **COM numbering.** Decided: auto-assign, with an optional number asked for
   when the port is made or changed later (see [COM numbers](#com-numbers)).
3. **Driver signer.** Open. Nothing signs the kernel-mode driver yet. The
   organisation's test certificate signs the user-mode package. Who holds an EV
   certificate for attestation signing, and whether the driver ships separately
   from the application, is not decided.
4. **Open source.** Decided: the driver lives in this repository under
   GPL-3.0-or-later, which makes the source offer trivial.
