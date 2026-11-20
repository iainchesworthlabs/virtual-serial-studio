# When the operating system asks, or says no

Windows, Linux and macOS each protect the computer in ways that can stop a
serial program or a network listener. Studio looks at these for you in **Tools →
Setup check**, says what it found, and tells you the smallest thing to do. It
follows three rules:

- **It tells you before anything happens.** Studio never raises its own rights,
  never changes a firewall rule and never changes a system setting without a
  button you press, after it has shown you exactly what it will do.
- **It says "Not checked yet" when it cannot tell.** It does not guess.
- **It never turns a protection off.** The fix is always a narrow permission, or
  a command for you to run yourself.

## Windows

**The driver and its helper service.** A COM port that other programs can open
needs a driver. The Setup check row *Virtual port driver* says which one Windows
has: the Virtual Serial Studio driver (with its version), or com0com. When a
piece of Studio's own set-up is missing, the row, the start page and the error
of a port that could not be made name it: "The Virtual Serial Studio driver is
not installed", "The Virtual Serial Studio helper is not installed", "The
Virtual Serial Studio helper is not running", "The Virtual Serial Studio helper
is out of date", "Your account cannot use the Virtual Serial Studio helper" or
"The Virtual Serial Studio driver on this PC needs updating". Each comes with
one button, **Set up Virtual Serial Studio**, or **Update the driver** when the
driver is older than the one that comes with this copy of Studio.

The button opens the *Set up Virtual Serial Studio* dialog. It installs or
repairs the driver and the helper service through one Windows permission prompt,
which names the program and its publisher; Studio itself keeps running without
administrator rights. Nothing is downloaded: the files come from this copy of
Studio. Studio checks the signatures first and installs nothing, and says so, when
Windows does not trust them. The helper restarts, so the ports that are open
are closed and Studio offers to make them again; an update can also ask for a
Windows restart before the new driver is used. The dialog shows the same set-up
as a command for you to run in a terminal opened as administrator, and **Copy
details** copies what the check found for a request for help. With neither driver
installed, the row offers **Install com0com** instead, which shows the steps
and a link to the project page; Studio installs nothing for you there.

Studio reads three driver versions: the one in the Windows driver store, the one
that comes with this copy, and the one the running ports speak. The Setup check,
the About tab of **Help → About** and the set-up dialog say in words
which is older, the same or newer, and nothing needs updating when the
installed one is the same or newer.

**Administrator rights.** Studio and its command line run as you, and none of
them asks for administrator rights when it starts. A step that needs them (a new
com0com pair, setting up the driver, a firewall rule) asks Windows for
permission when you press its button. Windows shows its own **User Account
Control** window; that window is the permission. The Setup check row
*Administrator rights* reads one of three ways. With the Virtual Serial Studio
driver installed, it says the driver makes ports without administrator rights.
When Studio already runs as administrator, it says it can create linked pairs.
Otherwise it says that a new linked pair needs administrator rights once per
pair, adds whether Windows will ask you only to confirm (your account is an
administrator) or to type an administrator's password (it is not), and offers
**Restart as administrator**. That restarts the whole program with those rights,
and the new Studio starts the same ports; if you decline the prompt, Studio keeps
running as before and says so.

**Windows Firewall.** Nothing is asked while Studio listens on this PC only
(`127.0.0.1`). When a network endpoint waits for other computers on `0.0.0.0` or a
network address, Windows Firewall blocks them until a rule allows it. The
*Network endpoint* dialog then shows **Check Windows Firewall for this port**, and
the Setup check shows a *Windows Firewall* row. They open a dialog that says:

- what Windows Firewall will do to this port, from reading its rules (Studio only
  reads them);
- the one rule Studio can add: this program, one protocol, one port, computers on
  your local network only, on the network you are on now (private, domain or
  public) and no other (when the PC is on all three at once, the rule is for the
  private network, since a rule never covers every network);
- the same rule as a `netsh` command, if you would rather run it yourself.

**Allow in Windows Firewall** starts a small helper (`vss_cli`, the command line
beside the program) through the Windows permission prompt. The helper does that
one thing and refuses any program that is not Studio. A rule that blocks the
program, or the *Block all incoming connections* setting, is never changed by
Studio; the dialog says where to change it. The Setup check row shows the same
facts with the buttons **Allow through the firewall** (or **How to allow it**
when this build cannot add the rule) and **How to change it**. It looks at ports
that listen with `tcp-listen://`, `udp-listen://` and `rfc2217-listen://`,
including addresses that end with `?allow=`. The rule is named for the program,
protocol and port, for example `Virtual Serial Studio vss_gui.exe TCP 4001`.
Remove it in Windows Defender Firewall (the name starts with *Virtual Serial
Studio*), or with `netsh advfirewall firewall delete rule name="<the name>"`.
The dialog shows the command that adds the rule, not the one that removes it.

The same check is available from a terminal and changes nothing:

```
vss_cli permissions check --listen=tcp-listen://0.0.0.0:4001
```

## Linux

**Serial devices.** Who may open `/dev/ttyUSB0` is decided by the group that owns
it (`dialout` on Debian, Ubuntu and Fedora, `uucp` on Arch). The Setup check has
three rows for this: *Pseudo terminals* (whether `/dev/ptmx` exists, which new
virtual ports need), *Serial port group membership* (whether your account is in
the `dialout` group; **How to add me** shows `sudo usermod -aG dialout $USER`)
and *Real serial devices*. When serial devices are found, the row *Serial device
access* reads the owner and mode of each one and says which of four things is
true:

- you may open it;
- your account is not in the group, and the dialog shows
  `sudo usermod -aG dialout <you>`. If `pkexec` (polkit) is installed the row's
  button is **Add me to the group** and the dialog has **Add me to the dialout
  group** (it names the device's group), which asks your desktop for your
  password and runs that one command. Without `pkexec` the button is **How to add
  me**. Sign out and back in afterwards;
- your account is in the group but this login does not know yet (sign out and
  in, or `newgrp dialout` in one terminal);
- the group may not read and write the device: the dialog shows the `ls -l`
  command. Studio does not change device permissions, because that is a system
  setting.

**When the file permissions allow it and it is still refused.** AppArmor, SELinux,
Flatpak and Snap can refuse a device or the folder Studio keeps its link names in
(`$XDG_RUNTIME_DIR/vss` when that is set, as it is in a desktop login, otherwise
`/run/vss`, then `/tmp/vss`; `VSS_VPORT_DIR` names another one). Studio recognises this when a device that the
permissions allow was refused, and the *Confinement* row (shown only when something
confines the program) gives the fix for the one that applies:

| Confinement | What Studio shows |
|---|---|
| Flatpak | `flatpak override --user --device=all <application id>` |
| Snap | `snap connections <snap>`; a serial device needs the `serial-port` interface connected |
| AppArmor | the lines to add to `/etc/apparmor.d/local/<profile>` (devices, pseudo terminals, link folder) and `sudo apparmor_parser -r /etc/apparmor.d/<profile>` |
| SELinux | `sudo ausearch -m AVC,USER_AVC -ts recent`, then `audit2allow -M vss_studio` and `semodule -i`; read what `audit2allow` proposes before you install it |

SELinux in enforcing mode is normal on Fedora and Red Hat systems, so it is
information and not a problem. Studio never edits a profile or a policy.

**Firewall.** Studio cannot read `ufw` or `firewalld` rules without administrator
rights. When a listener is open to other computers and `ufw` or `firewalld` is on,
the *Firewall* row shows the command that opens the port (`sudo ufw allow
4001/tcp`, or `sudo firewall-cmd --permanent --add-port=4001/tcp` and a reload).
With neither on, it says "Not checked yet" because `nftables` or `iptables` may
still filter.

`vss_cli permissions check --device=/dev/ttyUSB0 --listen=tcp-listen://0.0.0.0:4001`
prints the same facts.

## macOS

**Application Firewall.** Nothing is asked while Studio listens on this Mac only
(`127.0.0.1`). When a network endpoint waits for other computers and the firewall
is on, macOS shows its own question the first time one of them connects: *Do you
want the application to accept incoming network connections?* Choose **Allow**.
macOS remembers the answer for each program. The Setup check row *macOS firewall*
says which case you are in and, if you chose *Don't Allow* by mistake, shows the two
`socketfilterfw` commands that fix it (or you can set the program to *Allow
incoming connections* in **System Settings → Network → Firewall → Options**).
*Block all incoming connections* overrides every program, so there the row asks
you to turn that off. Studio only reads the firewall state; it never changes the
firewall.

**Local Network.** From macOS 15 an app needs your permission to find and connect
to devices on your local network, and macOS asks once. Studio cannot read the
answer, so the row *Local Network access* is information, and says where the switch
is (**System Settings → Privacy & Security → Local Network**) for the case where
you chose *Don't Allow* and other computers cannot connect.

**Serial devices.** `/dev/cu.*` devices are ordinary files that an ordinary user
may open, so the *Serial device access* row mostly says "You may open the serial
devices Studio found." The row *Serial port access* has no group check on macOS and
reads "Not checked yet": if a device cannot be opened, look at its permissions in
`/dev`. If a
USB serial adapter does not appear at all on a Mac with Apple silicon, macOS may be
asking you to allow the accessory (**System Settings → Privacy & Security → Allow
accessories to connect**); that prompt cannot be read from here either.

**The App Sandbox.** The release runs without the sandbox. A sandboxed build needs
the entitlements `com.apple.security.device.serial`, `network.client` and
`network.server`; the *Confinement* row names them if Studio finds itself sandboxed.

**Downloaded builds.** The macOS release is a disk image for Apple silicon. Open it,
drag `vss_gui.app` onto the `Applications` link and eject the image; `bin/vss_cli`
is in the image beside the app. Until a release is signed and notarised (see
[Releasing](../releasing.md)), Gatekeeper refuses to open the app the first time
("cannot be opened because the developer cannot be verified"). Try to open it once,
then open **System Settings → Privacy & Security** and choose **Open Anyway** beside
the message about `vss_gui`. On macOS 14 and earlier, Control-click the app, choose
**Open**, then **Open** again. Removing the quarantine flag also works:
`xattr -dr com.apple.quarantine /Applications/vss_gui.app`.
