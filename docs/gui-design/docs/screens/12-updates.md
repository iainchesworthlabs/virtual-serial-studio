# Screens 12: updates

No mockups yet. The design behind these screens is `docs/updates.md`. Dialog anatomy, components and voice are as in screens 01 and `07-copy-deck.md`. Every string below is wrapped in `tr()` with the context named in its section. `<...>` is a value filled at run time. Dates and times use the user's locale and are local time. A size is written in MB with no decimals from 10 MB up and one decimal below ("48 MB", "3.2 MB").

The words: **update** is the noun and the verb. A version is written as it is published, with no "v": "0.3.0", "0.3.0-rc.1". Never "upgrade" (that is what a licence does to an edition), never "patch", "hotfix" or "latest and greatest". The host is written as it is, "virtual-serial-studio.com". Studio "asks" the site and "checks for updates"; it never "phones home", "syncs" or "connects to the cloud".

## Where the state comes from
The GUI reads one object, `UpdateService`, which owns the timer, the settings and the request. Its transport and its clock are interfaces, so a test gives it a fake of each and every state below has a test. No test makes a request. The service states: `idle` (never checked), `checking`, `current`, `available`, `failed`, `disabled_by_policy`, `no_keys`, and for a download `downloading`, `verifying`, `ready`, `download_failed`.

## Menu and entry points
Help menu, after "Support..." and before About: **Check for updates...**. It is in the action registry, so Search actions (Ctrl+K) finds it as "Check for updates", with the search words "update version new release download". No shortcut key. It opens About on the Updates tab and starts a check.

Other ways in: the update chip in the status bar; the notice banners below; Settings.

## The question at first start (main window)
Context `UpdateNotice`. A NoteBanner of the kind info, in the place the licence notices use and below them when both show.

| Part | Text |
|---|---|
| Sentence | "Check for updates automatically? Once a day Studio would ask virtual-serial-studio.com for the newest version. Nothing about you or this computer is sent." |
| Actions | "Turn on", "No thanks" |

Shown the first time Studio starts for a user who has not answered, and until it is answered. Either answer is kept and the banner does not come back, also after an update. "Turn on" shows the toast "Studio will check for updates once a day." "No thanks" shows the toast "Studio will not check for updates. You can change this in Settings." Not shown when a policy turns update checks off, in a developer build, or in a build that cannot check. The banner takes no focus and is announced politely, once.

## Update chip (status bar)
Context `UpdateChip`. A StatusChip of the kind info to the left of the edition chip, present only in the state `available` (and through a download). Text "Update available". A button with the accessible name "Update available: version <version>. Open the update." It opens the update dialog. The chip is text plus dot shape, never colour alone.

## Update notice (main window)
Context `UpdateNotice`. Shown when a check on the timer finds a version that was not skipped. A check by hand shows its result in About and does not add a banner.

| Case | Kind | Text | Actions |
|---|---|---|---|
| Studio | info | "Virtual Serial Studio <version> is available." | "Details", "Later" |
| Plugins only (`%n`) | info | "An update is available for 1 plugin." / "Updates are available for <n> plugins." | "Details", "Later" |

"Details" opens the update dialog. "Later" hides the banner until the next start; the chip stays. No notice is a modal dialog, steals focus or blocks work.

## About, the Updates tab
Context `About`. The second tab, "Updates", after About and before Environment.

A definition list in a card. Rows that have no value are left out.

| Row label | Value |
|---|---|
| "This version" | "<version>, <kind of install>" |
| "Newest version" | The status sentence below. A polite live region |
| "Last checked" | "<date and time>" or "Never" |
| "Plugins" | One line for each plugin with an update: "<plugin name> <version> is available." |

The kind of install:

| Kind | Text |
|---|---|
| `windows-installer`, `linux-deb`, `linux-rpm` | "installed for everyone on this computer" |
| `windows-zip`, `macos-archive`, `linux-archive` | "unpacked from an archive" |
| `macos-dmg` | "copied from the disk image" |

The status sentence:

| State | Text |
|---|---|
| `idle` | "Not checked yet." |
| `checking` | "Checking..." |
| `current` | "<version> is the newest version." |
| `available` | "<version> is available." |
| `available`, no file for this kind of install | "<version> is available. Download it from virtual-serial-studio.com." |
| `failed` | "Studio could not reach the update server. Check the connection and try again." |
| `failed`, the feed did not verify | "The update information could not be trusted, so nothing was changed." |
| `disabled_by_policy` | "Update checks are turned off by your administrator." |
| `no_keys` | "This build cannot check for updates." |
| A developer build, after a check | "This is a developer build. The newest release is <version>." |

Under the card: a check box "Check for updates automatically", the same setting as in Settings, and a muted line "Once a day Studio asks virtual-serial-studio.com for the newest version. Nothing about you or this computer is sent."

Buttons in the tab, right aligned: "Check now" (disabled while checking, and in `disabled_by_policy` and `no_keys`), and "Update..." (primary of the tab, only in `available` with a file). The dialog footer is unchanged.

Acceptance: every state can be shown from a test; the status sentence is announced when it changes; "Check now" works with the keyboard only and returns focus to itself when the check ends; the tab works at 200 percent text.

## Settings, the Updates group
Context `SettingsDialog`. A group "Updates" after the last group.

| Control | Label | Help line |
|---|---|---|
| Check box | "Check for updates automatically" | "Once a day Studio asks virtual-serial-studio.com for the newest version. Nothing about you or this computer is sent." |
| Check box | "Include pre-release versions" | "Offers release candidates as well as finished releases." |

Both apply at once, as every setting does. In `disabled_by_policy` both are disabled and a muted line reads "Set by your administrator." In `no_keys` both are disabled and the line reads "This build cannot check for updates."

## Update (width 620)
Context `UpdateDialog`. Opens: the chip, "Details" on the notice, "Update..." in About. Title "Update Virtual Serial Studio". Modal to the main window.

### Before the download

| Part | Text |
|---|---|
| Sentence | "Version <new version> is available. You have <this version>." |
| Heading | "What is new" |
| Body | The summary from the update information, as plain text in a scrolling frame of at most eight lines. Left out, with its heading, when the summary is empty |
| Link | "Release notes" (opens the browser). Accessible name "Release notes for version <new version>, opens in your browser" |
| File line (muted) | "<file name>, <size>" |
| Note, when ports run (warn, `%n`) | "1 port is running. Installing stops it." / "<n> ports are running. Installing stops them." |
| Buttons | "Skip this version" (quiet), "Not now", "Download and install" (primary, default) |

When there is no file for this kind of install, the file line reads "There is no package for this kind of install. Download one from virtual-serial-studio.com." and the primary button is "Open the downloads page".

Plugins, when any have an update: a heading "Plugins" and one row for each, "<plugin name> <new version>, you have <version>", with its own button by scope:

| The installed copy | Row text or button |
|---|---|
| For this user | Button "Update" |
| For everyone on this computer | "Installed for everyone. An administrator updates it." and a button "Copy the folder" |
| Needs a newer Studio | "Needs Virtual Serial Studio <version> or later." |
| Released after the licence's updates ended | "<new version> was released after your licence's updates ended on <date>." |

"Skip this version" shows the toast "Studio will not mention <new version> again." and closes.

### During the download

| Part | Text |
|---|---|
| Sentence | "Downloading <file name>..." |
| Progress bar | Accessible name "Download progress", value in percent |
| Line under it | "<done> of <size>" |
| Buttons | "Cancel" |

Then, with the bar full and no button enabled, "Checking the download...".

"Cancel" deletes what arrived, returns to the first page and shows nothing else.

### After the check, by kind of install

| Kind | Sentence | Buttons |
|---|---|---|
| `windows-installer` | "The update is ready. Studio will close and the installer will start. Windows asks for an administrator." | "Not now", "Install now" (primary) |
| `macos-dmg` | "The update is ready. Quit Studio, then drag Virtual Serial Studio onto Applications in the window that opens." | "Not now", "Open the disk image" (primary) |
| `linux-deb` | "The update is ready. Install it with this command, then start Studio again." and the command `sudo apt install <file>` in the fixed font with a CopyButton | "Show in folder", "Install" (primary, only when `pkexec` exists; it asks for an administrator), Close |
| `linux-rpm` | The same, with `sudo dnf install <file>` | The same |
| `windows-zip`, `macos-archive`, `linux-archive` | "The download is in <folder>. This copy of Studio was unpacked from an archive, so unpack the new one in its place." | "Show in folder" (primary), Close |

"Not now" keeps the checked file for this run of Studio; "Update..." in About then goes straight to this page.

### Failures
The error pattern, inside the dialog, with "Try again" (primary) and Close. Nothing is changed on the computer in any of them.

| Failure | Title | Body |
|---|---|---|
| The connection failed or timed out | "Could not download the update" | "Studio could not reach the update server. Check the connection and try again." |
| The download is not the published file | "Could not download the update" | "The download was not the file that was published, so it was deleted." |
| The disk is full or the folder cannot be written | "Could not download the update" | "Studio could not save the download in <folder>." |
| The installer could not be started | "Could not start the installer" | "Studio could not start <file name>. It is in <folder>." with "Show in folder" in place of "Try again" |

Acceptance: every page and every failure can be shown from a test with the fake transport; the progress is announced at most once every ten percent; Escape is "Not now" before a download and "Cancel" during one; the dialog works at 200 percent text with the summary scrolling inside it; no string is built by joining translated fragments; the summary is shown as plain text whatever it holds.
