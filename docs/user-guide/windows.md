# Windows

## Old programs on Windows

Many programs that talk to serial equipment were written long ago. They are
often 32-bit, they open one fixed port, and they have habits that a port made by
Studio has to meet. This is what to set up, and what to expect.

### 32-bit programs

The driver is 64-bit (x64 and ARM64); there is no 32-bit Windows driver, and the
Windows release is for x86-64. A 32-bit program on 64-bit Windows reaches it as
it reaches any COM port, and Windows passes the serial requests through
unchanged, because they carry no pointers. A small 32-bit test program and a
script that runs it against the driver (tests W1 to W5 and T1 to T7 in
[drivers/windows/README.md](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/drivers/windows/README.md))
pass in the project's Windows 10 and Windows 11 test machines, on both drivers
(T7, which restarts the helper service while a program is attached, is for the
user-mode driver only). There is no 32-bit Windows test machine: the 32-bit
program runs under the 32-bit layer of 64-bit Windows, which is what an old
program does on a current PC. Your program may do something these tests do not,
so try it.

### The name a program opens

A program opens a port by name, and there are two spellings:

- `COM3`, and the other names up to `COM9`, work in every program.
- `\\.\COM12` is the form that works for every number. A program that builds the
  name as `"COM%d"`, and not as the C string `"\\\\.\\COM%d"`, cannot open
  `COM10` or above, because
  Windows reads the short form only up to 9.

For an old program choose a number from 1 to 9 and keep it. Pick it in **Add
port** (the **COM number** field) or with `vss_cli virtual --com=3`; see [Choosing a
COM number (Windows)](#choosing-a-com-number-windows). A program that keeps its
port in a setting file (for example `CommPort=3`) then finds the same port in
every run, and a program that lists only `COM1` to `COM4` can be given a number in
that range if the real ports on the PC leave one free.

Make the ports before you start the program, with the numbers it expects. Many
old programs fill their list of ports once, when they start. A port that is made,
or that comes back, later is not in the list until the program is started again.

### Programs that try every port

Some programs look for their equipment by opening `COM1`, `COM2` and so on up to
`COM16` in turn when they start. Studio's driver ports behave like real ones here:

- **One program has a port at a time.** A second open of a port that a program
  holds fails with "access denied", as it would on real hardware. A probe that
  meets a port held by another program moves on to the next.
- **A probe that finds your port may keep it.** If the first program to start
  probes all sixteen and keeps what it finds, it can take a port meant for
  another. Start the programs one at a time, in the order they should claim their
  ports, and watch the Ports list: **Waiting for program** becomes **Program
  connected** when a program opens the port.
- **Fixed numbers make probing predictable.** With the ports at `COM3`, `COM4` and
  `COM5`, a program that probes finds them where it expects them, in the same
  order, every run.

### What stops when Studio does

Studio's driver ports belong to Studio and are removed when it quits, or if it
stops without warning. A running program loses its port at that moment and may
stop or show an error. Closing the window is not quitting: see
[When the window is closed](setups.md#when-the-window-is-closed).

If the helper service or the driver restarts, it removes the ports your programs
had open. Studio then shows a banner, "Some COM ports were lost. The helper
service or the driver was restarted and removed the ports your programs had
open.", and **Recreate ports** makes them again with the COM numbers they asked
for. A program has to open its port again.

## Choosing a COM number (Windows)

Many older programs open one fixed port, `COM3` for example, and keep it in a
setting file, or list only `COM1` to `COM4`. A port from the Studio driver can
be given the number you choose, as in Device Manager's *Advanced > COM Port
Number*, so the program always finds it.

From the command line:

```
vss_cli virtual --kind=windows-driver --com=3
```

or in a saved topology, as an optional field on a virtual port (1 to 255):

```json
{ "name": "master", "kind": "virtual", "virtual_kind": "windows-driver", "com_number": 3 }
```

Studio also remembers the number it was asked for and saves it with the topology.
A port made from the command line or a setup file with no `com_number` gets the
number Windows gives it: the lowest one free in its COM name database. The same
slot keeps its number from one run to the next, but a different order of
creation can give a different one. The window's *Automatic* in **Add port**
chooses differently (see [Virtual port](adding-ports.md#virtual-port)): it prefers a number
Windows has kept that nothing uses.

In the window, choose the number in **Add port** (the **COM number** field,
*Automatic* by default), or select a driver port and press **Change...** beside
its **COM number** in the Inspector. The change restarts the port, so the window
tells you when a program has it open and refuses until the program lets go; use
[Release port](connecting-ports.md#when-a-program-will-not-let-go-of-a-port-advanced) if it will
not. The library does the same (`set_driver_com_number`, `studio::set_com_number`).

What to know:

- **The helper service is needed.** A COM number is held in Windows' COM name
  database, which only an administrator may change. The service
  `vsscom_helper` does what Device Manager does for you. The Windows installer
  and **Set up Virtual Serial Studio** (see [Installing and removing Studio on
  Windows](#installing-and-removing-studio-on-windows)) install it. Without it, a
  port without a number is made as before, and a request for a number fails with
  a message that says so.
- **A number that another device has is refused.** A real port, a com0com port
  or an unplugged device of another kind that keeps its number for when it comes
  back, are all "taken". A number that only a removed Studio port used is free
  again, and Studio clears it when you ask for it. Studio makes no port and says
  which number was refused, and the number stays free.
- **Automatic takes a number the computer has had.** A new port on *Automatic*
  takes the lowest COM number that Windows has kept and nothing is using, never
  below `COM3`, so deleting `COM17` and making a port again gives `COM17`. A
  number Windows has had is made in a fraction of a second; one it has never had
  takes about 30 seconds while Windows installs a device for it, and the window
  shows that it is working. Right after you delete a port, Windows counts its
  number as taken for about 45 seconds.
- **To change the number of a port that exists, close the program using it
  first.** The port restarts, which takes a second or two. For that moment it
  loops back, and bytes that are waiting carry on afterwards. If a program
  opens the port at that instant, Windows will not restart it again until the
  port is removed and made again.
- Names above `COM9` must be written `\\.\COM12` by programs that open ports by
  name, which most do not. A program that opens `"COM%d"` fails above 9, so
  choose a number from 1 to 9 for such a program.

## Installing and removing Studio on Windows

The Windows release has an installer, `virtual-serial-studio-<tag>-windows-x86_64.exe`,
and a `.zip` with the same files. The zip is the way to run Studio without
administrator rights. It also carries the driver files in `bin\driver`, so
**Set up Virtual Serial Studio** works from it.

The installer puts Studio and `vss_cli` in `Program Files\Virtual Serial Studio`
for all users, and asks Windows for administrator rights once, when it starts. It
adds a Start menu entry and an uninstaller in Apps & features. Installing over an
earlier version replaces it. If Studio or `vss_cli` is running from the install
folder, the installer closes it first, which stops its ports; they start again the
next time Studio is opened. If it cannot close it, it says so and stops.

- **PATH.** One page offers to add the folder of `vss_cli.exe` to the system PATH,
  so that `vss_cli` runs from any terminal opened afterwards. The box is not
  ticked. In a silent install, `/S /ADDTOPATH=1` adds it. It works however long
  the PATH already is, and the uninstaller takes it out again.
- **The driver.** Another page, with its box ticked, installs the user-mode virtual
  COM port driver and its helper service. If a Virtual Serial Studio driver is
  already installed, the page says which version. It offers to update an older
  one, leaves a driver of the same version as it is (with an unticked box,
  *Install the driver again (repair)*) and leaves a newer one alone. Updating
  restarts the helper, so ports that are open are closed, and Windows may ask for
  a restart. In a silent install, `/S` updates the driver only when the
  installer's is newer, `/DRIVER=0` leaves the driver and the helper alone, and
  `/REPAIRDRIVER=1` installs the driver again.
- **When Windows does not trust the driver files.** Studio is installed anyway,
  the driver is not, and the installer says why. The installer adds no
  certificate and does not turn on test signing. To install the driver later, run
  `bin\driver\trust\install-trust.ps1` as administrator on a machine you own (or
  turn on Windows test signing on a test machine and restart), then choose
  **Set up Virtual Serial Studio** in Studio. What happened is in
  `driver-setup.log` in the install folder.

To remove Studio, use Apps & features or `Uninstall.exe`. If Studio is running it
asks first, because closing it stops its ports. The uninstaller removes the
programs, the helper service, the COM ports Studio made, every Virtual Serial
Studio driver package in the driver store (older versions included) and the PATH
entry. It also removes the **Start when I sign in** entry of every signed-in user
and notes in each user's running setup that the ports were stopped, so a Studio
that is installed again offers them with **Start again** and does not start them
by itself. If the helper service cannot be removed, the rest is still removed, the
uninstaller says that the helper stays, and `driver-remove.log` stays in the
install folder. An uninstall run as `Uninstall.exe /S _?=<folder>` removes the
same things but leaves `Uninstall.exe` and the folder.
