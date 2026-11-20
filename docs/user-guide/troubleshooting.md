# Troubleshooting

**No COM ports appear on Windows.** A virtual COM port needs a driver: the
Virtual Serial Studio driver or com0com. Open **Tools → Setup check**. With
neither, *Best available* falls back to an in-process loopback, whose path is `loopback:<name>` and which other
applications cannot open. Pseudo terminals do not exist on Windows.

**Windows would not make the port.** The message names the part that is
missing: the driver, or the helper service (not installed, not running, out of
date, or closed to your account). **Set up Virtual Serial Studio** in the dialog
installs or repairs it; see **Tools → Setup check**.

**A COM number was refused.** `COM7 already belongs to another device on this PC`
means a real port, com0com or a port that was removed but keeps the name has it;
pick another number, or remove the old device in Device Manager (*View > Show
hidden devices*). `Choosing a COM number needs the Virtual Serial Studio helper
service` means `vsscom_helper` is not installed or is older than the driver; leave
the number on *Automatic* or install the helper. `A program has this port open`
means close the program, or use Release port in Advanced mode, then ask again.
`No more ports can be made` means you hold 16 driver ports (or the driver 32 in
all): remove one.

**A pseudo terminal path is shown but another program cannot open it.** On
Linux the path is `/dev/pts/N`; it exists only while the studio is running.
Check the permissions of the user running the program.

**A real device cannot be opened.** Another program may hold it, or it was
unplugged. Close the other program and try again. On Linux, add your user to
`dialout` and log in again; the setup check shows the command.

**Network peers cannot connect.** Check the firewall (on Windows, the Setup check
row and the dialog behind it in the Network endpoint dialog read the rules and
show what is missing), that you listened on an
address the peer can reach, and that the port number matches. Test locally with
`127.0.0.1` first.

**The counters do not move.** Check the state in the Ports pane (Advanced) or on
the setup's page; a port that is *Failed* or *Closed* does not carry traffic. Use **Help → Why is nothing
arriving?**, and in Advanced the **Log** tab, which records state changes and
errors.

**A port failed while running.** The row shows *Failed* and the links to it are
kept. Fix the cause (for example, plug the cable back in) and attach the device
again.
