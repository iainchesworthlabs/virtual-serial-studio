# Updates

Studio can ask whether a newer version has been published, tell you, and
download it when you say yes. It is the only thing Studio asks the network for
by itself. Studio always asks you first, and it never installs an update with
nobody watching.

The automatic check is off in a new install. Studio makes no request for an
update until you have said yes, or until you check by hand.

## The question at first start

The first time Studio starts for you, a banner in the window asks: "Check for
updates automatically? Once a day Studio would ask virtual-serial-studio.com for
the newest version. Nothing about you or this computer is sent." The buttons are
**Turn on** and **No thanks**.

Either answer is kept, and Studio does not ask again, also after Studio itself
is updated. The banner takes no focus and blocks nothing, and it stays until you
answer it. It is not shown in a developer build, in a build that cannot check
for updates, or when your administrator has turned update checks off.

## Checking by hand

**Help → Check for updates...** opens **Help → About** on the **Updates** tab and
starts a check. The same action is found as "Check for updates" with `Ctrl+K`.
The tab shows:

| Row | What it says |
|---|---|
| This version | The version and the kind of install: "installed for everyone on this computer", "unpacked from an archive" or "copied from the disk image". |
| Newest version | "Not checked yet.", "Checking...", "`<version>` is the newest version." or "`<version>` is available." |
| Last checked | The date and time, or "Never". |
| Plugins | One line for each installed official plugin that has a newer version. Left out when there is none. |

**Check now** asks again. When something is available, **Update...** opens the
update dialog. A check by hand works whether or not the automatic check is on.

If the check fails, the sentence says why: "Studio could not reach the update
server. Check the connection and try again." or "The update information could
not be trusted, so nothing was changed." Nothing on your computer is changed.
A developer build says "This is a developer build. The newest release is
`<version>`." and offers no install.

## When an update is found

A check that runs by itself never opens a window. It shows a banner, "Virtual
Serial Studio `<version>` is available.", with **Details** and **Later**, and puts
an **Update available** chip in the status bar. **Later** hides the banner until
Studio is next started. The chip stays.

**Details**, the chip and **Update...** in About open the update dialog,
"Update Virtual Serial Studio". It says "Version `<new version>` is available. You
have `<this version>`." and shows:

- **What is new**: the summary from the update information, as plain text, and a
  **Release notes** link that opens in your browser.
- The file, with its name and size, such as `virtual-serial-studio-v0.3.0-windows-x86_64.exe, 48 MB`.
- When ports are running, a note: "3 ports are running. Installing stops them."

The buttons are **Download and install**, **Skip this version** and **Not now**.
**Skip this version** stops the automatic check from mentioning that version
again. It speaks again for a later one, and a check by hand still shows it.
**Not now** closes the dialog.

When there is no file for the way this copy was installed, the dialog says "There
is no package for this kind of install. Download one from
virtual-serial-studio.com." and the main button is **Open the downloads page**.

## Download and install

**Download and install** shows a progress bar with **Cancel**. **Cancel** deletes
what arrived and goes back to the first page. Then it reads "Checking the
download...": Studio compares the file with the SHA-512 in the signed update
information, and uses it only when they match. The file comes from the project's
releases on GitHub.

What happens next depends on how this copy of Studio was installed:

| This copy of Studio | After the download is checked |
|---|---|
| Installed with the Windows installer | "The update is ready. Studio will close and the installer will start. Windows asks for an administrator." **Install now** starts the installer, and Studio then closes as Quit does, which stops the running ports. If you say no to the Windows prompt, nothing changes and Studio stays open. The installer shows its own pages and updates in place, so the driver, the helper service and your COM numbers are kept. |
| Copied from the macOS disk image | "The update is ready. Quit Studio, then drag Virtual Serial Studio onto Applications in the window that opens." **Open the disk image** opens it. Studio does not replace itself. |
| Installed from a `.deb` or `.rpm` package on Linux | The dialog shows the command, `sudo apt install <file>` or `sudo dnf install <file>`, with a copy button. Where `pkexec` exists, **Install** runs it and the system asks for an administrator. **Show in folder** shows the file. Start Studio again afterwards. |
| Unpacked from a zip or another archive (Windows, macOS or Linux) | Once the download is checked, the file is put in your Downloads folder: "The download is in `<folder>`. This copy of Studio was unpacked from an archive, so unpack the new one in its place." **Show in folder** shows it. |

**Not now** keeps the checked file until Studio quits. **Update...** in About
then goes straight to this page. Studio never raises its own rights: every step
that needs an administrator is a prompt from the system, for a program the
system starts.

If a download fails, the dialog names what failed and offers **Try again**. It
says "Studio could not reach the update server. Check the connection and try
again.", "The download was not the file that was published, so it was deleted."
or "Studio could not save the download." Nothing is installed and
Studio is as it was.

## Turning the check on or off

- In **Tools → Settings**, the **Updates** group has *Check for updates
  automatically* and *Include pre-release versions* ("Offers release candidates
  as well as finished releases."). The check box in the Updates tab of About is
  the same setting as the first one. Both apply at once.
- When the automatic check is on, Studio checks once a day, 60 seconds after it
  starts. A check that fails by itself is silent and is tried again an hour
  later, up to three times.
- With it off, Studio checks only when you use **Check for updates...** or
  **Check now**.
- An administrator turns the check off for everyone on a computer with
  `"update_check": false` in the policy file (see [Where Studio looks for
  plugins](plugins.md#where-studio-looks-for-plugins) for where it is, and the key's place
  in [the plugin framework](../plugins.md), section 8.6). Studio then makes no check
  by itself and none by hand. **Check for updates...** and **Check now** are
  disabled, the Settings check boxes are disabled with "Set by your
  administrator.", and the sentence in About reads "Update checks are turned off
  by your administrator." A policy file that cannot be trusted is ignored by the
  update check, which then follows your setting.
- `vss_cli` makes no update check. A computer that runs without a window is
  updated by whoever sets it up.

## What is sent

A check is one HTTPS request for one file,
`https://virtual-serial-studio.com/updates/v1/feed.json`. It has no query string,
no cookie and no body. Its `User-Agent` is the fixed text `VirtualSerialStudio`,
with no version and no system in it. Studio downloads the whole file and
chooses for itself, so the site is never told which version you have. The
server sees what any web server sees: the address the request came from and the
time. Studio uses your system's proxy settings and never ignores a certificate
error.

The file is signed with an Ed25519 key. Studio checks the signature with a key
it was built with, and refuses a file that does not verify. A file that is
older than one Studio has already accepted is refused too, and an update is only
ever a newer version than the one you run.

A download is made only after you say yes. It comes from the project's releases
on GitHub, which sees the request as it sees any download.

Studio sends nothing about you, your computer or how you use Studio. No
licence is sent, because there is no activation server and no licence check
over the network. There is no usage data, no crash report and no account. See
the [privacy page](https://virtual-serial-studio.com/privacy.html) for the
same statement.
