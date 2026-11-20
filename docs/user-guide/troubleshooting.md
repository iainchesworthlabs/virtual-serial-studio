# Troubleshooting

**No COM ports appear on Windows.** A virtual COM port needs a driver: the
Virtual Serial Studio driver or com0com. Open **Tools → Setup check**. With
neither, *Best available* falls back to an in-process loopback, whose path is `loopback:<name>` and which other
applications cannot open. Pseudo terminals do not exist on Windows.

**Windows would not make the port.** The message names the part that is
missing: the driver, or the helper service (not installed, not running, out of
date, or closed to your account), or a driver older than the one that comes with
this copy of Studio ("The Virtual Serial Studio driver on this PC needs
updating"). **Set up Virtual Serial Studio** in the dialog (**Update the driver**
for an old driver) installs or repairs it, and **Copy details** copies what the
check found for a request for help; see **Tools → Setup check** and [The driver
and its helper service](when-the-system-asks.md#windows).

**A COM number was refused.** `COM7 already belongs to another device on this PC`
means a real port, com0com or a port that was removed but keeps the name has it;
pick another number, or remove the old device in Device Manager (*View > Show
hidden devices*). `Choosing a COM number needs the Virtual Serial Studio helper
service` means the helper service, `vsscom_helper`, is not installed or is too old;
**Set up Virtual Serial Studio** installs or updates it, or leave the number on
*Automatic*. `Choose a number from 1 to 255.` means the number is outside that
range. `A program has this port open`
means close the program, or use Release port in Advanced mode, then ask again.
`No more ports can be made` means you hold 16 driver ports (or the driver 32 in
all): remove one. A number that a Studio port once had and that you removed can
be used again.

**A pseudo terminal path is shown but another program cannot open it.** On
Linux the path is `/dev/pts/N`; it exists only while the studio is running.
Check the permissions of the user running the program.

**A real device cannot be opened.** Another program may hold it, or it was
unplugged. Close the other program and try again. On Linux, add your user to
`dialout` and log in again; the setup check shows the command.

**Network peers cannot connect.** Check the firewall (on Windows, the Setup check
row and the dialog behind it in the Network endpoint dialog read the rules and
show what is missing; on Linux and macOS the Setup check has a *Firewall* or
*macOS firewall* row), that you listened on an
address the peer can reach, and that the port number matches. Test locally with
`127.0.0.1` first. See [When the operating system asks, or says
no](when-the-system-asks.md).

**The counters do not move.** Check the state in the Ports pane (Advanced) or on
the setup's page; a port that is *Failed* or *Closed* does not carry traffic. Use **Help → Why is nothing
arriving?**, and in Advanced the **Log** tab, which records state changes and
errors.

**A port failed while running.** The row shows *Failed* and the links to it are
kept. Fix the cause (for example, plug the cable back in), then choose **Stop**
and **Start** in the port's menu on the diagram or in the Inspector (Advanced) to
open it again. A network client that was made with **Create anyway and keep
trying** shows **Retrying** and connects by itself when the other computer
answers. A port you stopped shows **Stopped** and is not *Failed*; **Start**
opens it. If the device does not open, attach it again.

**Some COM ports were lost.** On Windows the banner says "Some COM ports were
lost. The helper service or the driver was restarted and removed the ports your
programs had open." **Recreate ports** makes them again with the COM numbers they
asked for. A program that had one open has to open it again.

**The window stops answering, or the canvas is drawn badly.** When the window
does not answer for two seconds, Studio writes a line to `stalls.log` in its
local data folder, naming what it was doing; the file stays on your computer and
is useful in a request for help. If the canvas is drawn badly, choose **Software
(CPU)** under **Tools → Settings → Performance → Rendering** and restart Studio,
or start it once with `vss_gui --render=software`.

**The update check says "This build cannot check for updates."** The build has no
update key, so it makes no request. See [A build with no update
key](updates.md#a-build-with-no-update-key).
