# Updates

Studio can ask whether a newer version has been published, tell you, and
download it when you say yes. It is the only thing Studio asks the network for
by itself. Studio always asks you first, and it never installs an update with
nobody watching.

The automatic check is off in a new install. Studio makes no request for an
update until you have said yes, or until you check by hand.

A build that carries no update key cannot check at all, by hand or by itself:
see [A build with no update key](#a-build-with-no-update-key).

## The question at first start

The first time Studio starts for you, a banner in the window asks: "Check for
updates automatically? Once a day Studio would ask virtual-serial-studio.com for
the newest version. Nothing about you or this computer is sent." The buttons are
**Turn on** and **No thanks**. A short message then says what Studio will do:
"Studio will check for updates once a day." or "Studio will not check for
updates. You can change this in Settings."

Either answer is kept, and Studio does not ask again, also after Studio itself
is updated. The banner takes no focus and blocks nothing, and it stays until you
answer it. It is not shown in a developer build, in a build that cannot check
for updates, or when your administrator has turned update checks off.

## Checking by hand

**Help → Check for updates...** opens **Help → About** on the **Updates** tab and
starts a check. The same action is in the command palette (`Ctrl+K`).
The tab shows:

| Row | What it says |
|---|---|
| This version | The version and, once a check has found out, the kind of install: "installed for everyone on this computer", "unpacked from an archive" or "copied from the disk image". |
| Newest version | "Not checked yet.", "Checking...", "`<version>` is the newest version.", "`<version>` is available." or, when there is no package for the way this copy was installed, "`<version>` is available. Download it from virtual-serial-studio.com." |
| Last checked | The date and time, or "Never". |

The tab has no row for plugin updates. Under the rows it has the *Check for
updates automatically* check box and a note: "Once a day Studio asks
virtual-serial-studio.com for the newest version. Nothing about you or this
computer is sent."

**Check now** asks again. When something is available, **Update...** opens the
update dialog (it closes About first). A check by hand works whether or not the
automatic check is on.

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
- When ports are running and the install would close Studio (the Windows
  installer and the macOS disk image), a note: "3 ports are running. Installing
  stops them."

The buttons are **Download and install**, **Skip this version** and **Not now**.
**Skip this version** stops the automatic check from mentioning that version
again ("Studio will not mention `<version>` again."). It speaks again for a
later one, and a check by hand still shows it. **Not now** closes the dialog.

When there is no file for the way this copy was installed, the dialog says "There
is no package for this kind of install. Download one from
virtual-serial-studio.com." and the main button is **Open the downloads page**.

## Download and install

**Download and install** shows "Downloading `<file>`..." with a progress bar and
**Cancel**. **Cancel** (or `Esc`) deletes what arrived and goes back to the first
page. When the file has arrived the page reads "Checking the download...":
Studio compares the file with the SHA-512 in the signed update information, and
uses it only when they match. The address of the file is in the signed update
information and has to be an `https` address; the project's releases are on
GitHub, so the file comes from there. A download follows a redirect only to
another `https` address.

What happens next depends on how this copy of Studio was installed:

| This copy of Studio | After the download is checked |
|---|---|
| Installed with the Windows installer | "The update is ready. Studio will close and the installer will start. Windows asks for an administrator." **Install now** starts the installer, and Studio then closes as Quit does, which stops the running ports. If you say no to the Windows prompt, nothing changes and Studio stays open. The installer shows its own pages and updates in place, so the driver, the helper service and your COM numbers are kept. |
| Copied from the macOS disk image | "The update is ready. Quit Studio, then drag Virtual Serial Studio onto Applications in the window that opens." **Open the disk image** opens it. Studio does not replace itself. |
| Installed from a `.deb` or `.rpm` package on Linux | The dialog shows the command, `sudo apt install <file>` or `sudo dnf install <file>`, with a copy button. Where `pkexec` exists, **Install** runs it and the system asks for an administrator. **Show in folder** shows the file. When the install has worked the dialog says "Restart Studio to use the new version."; when it has not, "The package could not be installed. Use the command above." |
| Unpacked from a zip or another archive (Windows, macOS or Linux) | Once the download is checked, the file is put in your Downloads folder: "The download is in `<folder>`. This copy of Studio was unpacked from an archive, so unpack the new one in its place." **Show in folder** shows it. |

**Not now** keeps the checked file for this run, and **Update...** in About
then goes straight to this page. Studio deletes what an earlier run left in its
download folder when it next starts. Studio never raises its own rights: every
step that needs an administrator is a prompt from the system, for a program the
system starts.

If a download fails, the dialog, "Could not download the update", names what
failed and offers **Try again** and **Close**. It says "Studio could not reach
the update server. Check the connection and try again.", "The download was not
the file that was published, so it was deleted." or "Studio could not save the
download." Nothing is installed and Studio is as it was. If Studio cannot start
the installer or the disk image, the dialog, "Could not start the installer",
says "Studio could not start `<file>`. It is in `<folder>`." and offers **Show in
folder**.

## Turning the check on or off

- In **Tools → Settings**, the **Updates** group at the foot of the **General**
  page has *Check for updates automatically* and *Include pre-release versions*
  ("Offers release candidates as well as finished releases."). The check box in
  the Updates tab of About is the same setting as the first one. Both apply at
  once.
- When the automatic check is on, Studio looks at the clock every 30 seconds and
  checks when at least 60 seconds have passed since it started and a day since
  the last check. A developer build makes no check by itself. A check that fails
  by itself is silent and is tried again an hour later, up to three times; after
  the third retry fails, Studio waits until a day after the first failure.
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

## A build with no update key

Studio checks the signature of the update information against keys that are
built into the program. In the source of this version the list of keys is
empty, so a program built from it cannot check for updates, and it makes no
request to find out. In such a build:

- **Help → Check for updates...** is disabled, and its tooltip says "This build
  cannot check for updates."
- The Updates tab of About shows "This build cannot check for updates." as the
  newest version and "Never" as the last check. **Check now** and the *Check for
  updates automatically* check box are disabled, and there is no **Update...**
  button.
- In **Tools → Settings**, both check boxes in the **Updates** group are
  disabled, with the note "This build cannot check for updates."
- The first-start banner is never shown, so the question is not asked and no
  answer is stored. Nothing runs on the timer.
- No request is made, so nothing is sent to anyone.

A release that is built with the keys behaves as the rest of this page says. If
your administrator has also turned update checks off, the sentence is "Update
checks are turned off by your administrator." in place of this one.

## What is sent

A check is one HTTPS request for one file,
`https://virtual-serial-studio.com/updates/v1/feed.json`. It has no query string,
no cookie and no body. Its `User-Agent` is the fixed text `VirtualSerialStudio`,
with no version and no system in it. Studio downloads the whole file and
chooses for itself, so the site is never told which version you have. The
server sees what any web server sees: the address the request came from and the
time. Studio uses your system's proxy settings and never ignores a certificate
error. A file larger than 64 kB is cut off and refused.

`vss_gui --check-tls` says whether this copy can make a secure connection, which
the check needs. It prints "TLS is available." and exits with 0, or prints "TLS
is not available." and exits with 1. It makes no connection.

The file is signed with an Ed25519 key. Studio checks the signature with a key
it was built with, and refuses a file that does not verify. A build with no key
could not verify any file, so it makes no request. A file that is
older than one Studio has already accepted is refused too, and an update is only
ever a newer version than the one you run.

A download is made only after you say yes. It comes from the address in the
signed update information, which is the project's releases on GitHub, and
GitHub sees the request as it sees any download.

Studio sends nothing about you, your computer or how you use Studio. No
licence is sent, because there is no activation server and no licence check
over the network. There is no usage data, no crash report and no account. See
the [privacy page](https://virtual-serial-studio.com/privacy.html) for the
same statement.
