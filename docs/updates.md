# Online updates (design)

Status: **proposal**, written on 15 November 2026. The owner decided the four main points the same day; section 13
records them and lists what is still open. Nothing here is built yet. Section 12 is the order of work.

Studio learns that a newer version of itself, or of an official plugin, has been published, tells the user, and
on a yes downloads it, checks it and starts the install. This page is the design: what the user sees, the file
Studio reads, how it is signed and published, how a download is checked and installed on each system, and what
changes in the rest of the project.

It builds on [Licensing](licensing.md) (the signed-file envelope and the Ed25519 code), on
[The plugin framework](plugins.md) (discovery, the official signature, the kinds of install) and on
[Releasing](releasing.md) (the packages and the publish job), and does not repeat them.

## 1. Scope

In scope:

- A check for a newer Studio, by hand (Help, About) and on a timer.
- The same check for the official plugins that are installed.
- Download, verification and the start of the install, after the user says yes.
- The file that describes what is current (the *update feed*), its signature and the job that publishes it.
- A setting and an administrator's policy that turn the check off.

Not in the first version:

- Updates for third-party plugins. An author's own feed can follow once official plugins work.
- A plugin store. A plugin that is not installed is never offered.
- Updates that install with nobody watching. Studio always asks first, and the install is always started by the
  user.
- Delta downloads. A download is a whole package.
- A check from `vss_cli`. The command line tool has no TLS code; adding one means OpenSSL as a dependency on
  every system. A rig without a window is updated by whoever deploys it.
- Delivering a newer official key list or a revocation list over the same channel. The feed leaves room for it
  (section 10).

**One install for every edition.** Community and Pro are the same program; a licence file is the difference
(`editions.md`). So there is one feed and one set of packages, and the check never looks at the licence to decide
what Studio to offer. The licence matters in one place only: whether an official plugin's newer version is inside
the licence's version window (section 8).

## 2. The rule this changes

Until now the project rule has been "no network access from the GUI". This design replaces it with:

> Studio makes no network connection of its own except to check for updates and to download an update the user
> agreed to. It sends nothing about the user, the computer or how Studio is used. No telemetry, no analytics.

That sentence, or the fact behind it, is written in several places, and each is changed in the same pull request as
the code that makes it true (section 12, step 6):

| Where | Now says |
|---|---|
| `AGENTS.md`, Working rules | "No network access from the GUI, no telemetry, no analytics." |
| `docs/plugins.md`, sections 1, 2, 8.5 and 8.9 | "No network from the GUI"; "an update client or any network access" is a non-goal; "There is no network, so..." |
| `docs/editions.md`, Licence files | "There is no activation server and no telemetry." This stays true. The page gains one sentence on the update check |
| `docs/licensing.md`, Seats and Revocation | "the GUI makes no network connection". Activation and seat counting stay offline; the sentence is narrowed |
| `docs/user-guide.md`, Network addresses | "Studio does not use the network unless you add an endpoint" |
| `website/pages/privacy.html` | "No network connection of its own... It does not check for updates." |
| `docs/gui-design/design-deviations.md` | `Qt6::Network` "is not an approved dependency". The shell already links it for the single-instance socket; the update check is its second use |

What does not change: no activation server, no licence check over the network, no usage data, no crash reports, no
account. A licence is never sent anywhere.

## 3. What the user sees

The words below are the proposal for the screen spec (`docs/gui-design/docs/screens/12-updates.md`, written in step
4). The spec is the final word on strings.

### 3.1 Ways in

| Place | What is there |
|---|---|
| Help menu | **Check for updates...**, after Support and before About. In the action registry, so Search actions finds it |
| About dialog | A new **Updates** tab, second, after About. The header's sub line is unchanged |
| Settings | An **Updates** group: "Check for updates automatically" and "Include pre-release versions" |
| First start | The question of section 3.5, once |
| Status bar | A chip, "Update available", only while there is one. It opens the update dialog |
| Plugins page (when it exists) | A row's version shows "1.3.0 available" with the same dialog behind it |

### 3.2 The Updates tab of About

| Row | Content |
|---|---|
| "This version" | The version and the kind of install: "0.2.0, installed for everyone" or "0.2.0, unpacked from an archive" |
| "Newest version" | "0.2.0 is the newest version." or "0.3.0 is available." or "Not checked yet." |
| "Last checked" | A date and time in the locale's format, or "Never" |
| "Plugins" | One line for each installed official plugin that has a newer version; absent when there are none |
| Check box | "Check for updates automatically". The same setting as in Settings |
| Buttons | **Check now**; **Update...** when something is available |

The status sentence is a polite live region. While a check runs the button is disabled and the sentence reads
"Checking...". A check never blocks the dialog.

### 3.3 When a check finds something

A check on the timer never opens a window by itself. It shows the status bar chip and one notice banner (the kind
the licence notices use): "Virtual Serial Studio 0.3.0 is available." with the actions **Details** and **Later**.
Later hides the banner until the next start; the chip stays.

The **update dialog**:

| Part | Content |
|---|---|
| Title | "Update Virtual Serial Studio" |
| Sentence | "Version 0.3.0 is available. You have 0.2.0." |
| What is new | The release's summary from the feed, as plain text, and a link "Release notes" that opens in the browser |
| The file | Its name and size: "virtual-serial-studio-v0.3.0-windows-x86_64.exe, 48 MB" |
| Note, when ports run | "3 ports are running. Installing stops them." |
| Buttons | **Download and install** (default), **Skip this version**, **Not now** |

Skip this version stores the version; the timer check stays quiet about it and speaks again for a later one. A
check by hand always shows it.

After **Download and install**: a progress bar with **Cancel**, then "Checking the download...", then the step
for this kind of install (section 7). A failure names what failed and leaves Studio as it was:

| Failure | Sentence |
|---|---|
| No connection, a timeout, a TLS error | "Studio could not reach the update server. Check the connection and try again." |
| The feed does not verify | "The update information could not be trusted, so nothing was changed." |
| The download does not match | "The download was not the file that was published, so it was deleted." |
| Turned off by policy | "Update checks are turned off by your administrator." |
| A build with no update key | "This build cannot check for updates." |
| A developer build | "This is a developer build. The newest release is 0.3.0." (no install is offered) |

### 3.4 Accessibility

As for every screen (`AGENTS.md`, GUI rules): names and roles, keyboard reach, focus ring, both themes, 24 px
targets, 200 percent text. The progress bar has an accessible value. Nothing flashes or steals focus when a timer
check finishes.

### 3.5 The question at first start

The automatic check is **off** in a new install, and Studio makes no request until the user has said yes
(decision 1). The first time Studio starts for a user who has not been asked, the window shows one notice banner:

| Part | Content |
|---|---|
| Sentence | "Check for updates automatically? Once a day Studio would ask virtual-serial-studio.com for the newest version. Nothing about you or this computer is sent." |
| Actions | **Turn on** and **No thanks** |

- Either answer is stored (`updates/asked`) and the banner never comes back, whichever version is installed
  later. An update of Studio keeps the answer and does not ask again.
- The banner stays until it is answered. It takes no focus and blocks nothing, so the first start is not held up
  and a test that drives the window is not interrupted.
- Turn on sets "Check for updates automatically"; the first check then runs as section 6.2 says. No thanks leaves
  it off. The setting is in Settings and in About for a change of mind, and a check by hand works either way.
- The banner is not shown when an administrator's policy turns update checks off (section 6.3), or in a
  developer build.

## 4. The update feed

One small signed file says what is current. Studio fetches it, verifies it and compares it with what is installed.
Nothing is asked of a server beyond that one file, so the server can be a static host and learns nothing but that
a request was made.

### 4.1 Address

```
https://virtual-serial-studio.com/updates/v1/feed.json
```

The address is on the owner's own domain so that hosting can move without a new Studio. `v1` is the feed's major
format: a Studio that reads version 1 keeps asking for `v1`, and a format it could not read is published beside it
as `v2`. The address is compiled in. It is not a setting and no environment variable changes it in a release
build; the tests give the service another transport (section 9).

The same file is also an asset of the release it was made for, `update-feed.json`, for a person who wants to read
it.

### 4.2 The envelope

The feed is a `vss::signed_file` envelope (`docs/licensing.md`, "Envelope") with the purpose `update-feed`. The
purpose is inside the signed message, so no licence, plugin or revocation signature passes as a feed, and a feed
key signs nothing else.

`signed_file::parse` refuses a file over 64 KiB. The feed stays inside that: two Studio releases with seven files
each are about 6 KiB, and a plugin adds about 1 KiB. If the plugin list outgrows it, the feed gains the address of
a second signed file; that is not designed now.

### 4.3 The payload

```json
{
  "feed_version": 1,
  "serial": 12,
  "issued_at": "2026-12-01T03:40:00Z",
  "studio": [
    {
      "channel": "stable",
      "version": "0.2.0",
      "released": "2026-12-01",
      "summary": "Plain text, at most 2000 characters.",
      "notes_url": "https://github.com/iainchesworthlabs/virtual-serial-studio/releases/tag/v0.2.0",
      "files": [
        { "kind": "windows-installer", "arch": "x86_64",
          "name": "virtual-serial-studio-v0.2.0-windows-x86_64.exe",
          "url": "https://github.com/iainchesworthlabs/virtual-serial-studio/releases/download/v0.2.0/virtual-serial-studio-v0.2.0-windows-x86_64.exe",
          "size": 50132336,
          "sha512": "<128 lower-case hex characters>" }
      ]
    },
    { "channel": "preview", "version": "0.3.0-rc.1", "released": "2026-12-09", "...": "..." }
  ],
  "plugins": [
    {
      "id": "com.virtualserialstudio.decoding",
      "version": "1.3.0",
      "released": "2026-12-01",
      "requires_studio": "0.2.0",
      "summary": "Plain text.",
      "notes_url": "https://...",
      "files": [
        { "os": "windows", "arch": "x86_64", "name": "...", "url": "https://...", "size": 123456,
          "sha512": "<128 lower-case hex characters>" }
      ]
    }
  ]
}
```

| Field | Rule |
|---|---|
| `feed_version` | 1. Any other value is refused |
| `serial` | A whole number from 1, higher in every feed that is published. Studio keeps the highest it has accepted and refuses a lower one, so an old feed cannot be put back |
| `issued_at` | UTC. Checked against the signing key's window (`check_key_window`) |
| `studio` | At most one entry for each channel: `stable` (the newest final release) and `preview` (the newest pre-release, present only while it is newer than the newest final release) |
| `version` | Semantic versioning 2.0.0 without build metadata: `0.2.0`, `0.3.0-rc.1`. The same form as `build_info::version` |
| `released` | A date. For a plugin it is the date the licence's version window is compared with (section 8) |
| `summary` | Plain text. Shown as text, never as HTML or rich text |
| `notes_url`, `url` | `https` only, at most 2048 characters, no user name or password in it |
| `kind` | `windows-installer`, `windows-zip`, `macos-dmg`, `macos-archive`, `linux-deb`, `linux-rpm`, `linux-archive` |
| `os` (plugins) | `windows`, `macos`, `linux` |
| `arch` | `x86_64` or `arm64` |
| `size` | Bytes, from 1 to 4 GiB. Studio stops a download that passes it |
| `sha512` | Of the file's bytes. What the download is checked against |
| `requires_studio` | The oldest Studio the plugin version runs with. A newer plugin is not offered to an older Studio; the dialog says which Studio it needs |

The reader is as strict as the other signed files: an unknown key, a key given twice, a wrong type, a list over
its limit (2 Studio entries, 16 files in an entry, 64 plugins) or a value outside its rule refuses the whole feed.
The signature is checked before the payload is parsed, so no parser reads unsigned data beyond the envelope's five
fields.

A file that Studio does not know how to install (a `kind` this version does not list) cannot appear: an unknown
`kind` refuses the feed. A new kind therefore needs `v2`, or is added while no release that reads `v1` would
choose it. With no deployed versions to protect yet (the project has no compatibility burden today), this is the
simple rule; it can be relaxed to "skip an unknown kind" before the first release that carries the check.

### 4.4 What Studio does with it

1. Verify the envelope with the built-in feed keys, then check the key's window against `issued_at`.
2. Parse the payload. Refuse a `serial` lower than the highest accepted. Store a higher one.
3. Pick the channel: `preview` when "Include pre-release versions" is on **or** the running version is itself a
   pre-release, and the preview entry is newer than the stable one; otherwise `stable`.
4. Compare by semantic version precedence. Only a version **greater** than the running one is an update. The feed
   can never cause a downgrade.
5. Pick the file whose `kind` matches how this copy was installed (section 7.1) and whose `arch` matches the
   program. No such file: "0.3.0 is available" with a link to the downloads page, and no install button.
6. For each installed official plugin that is not shadowed and did not come with Studio: the feed's entry with the
   same id, when its version is greater, its `requires_studio` is met and a file for this system exists.

A developer build (`0.2.0-dev`, a version that no tag produced) runs the check only by hand and is never offered
an install.

## 5. Signing and keys

The feed is the root of the whole thing: it names the file to run and its hash. HTTPS protects the request, but
a feed that is trusted only because of where it came from would make the web host, the domain registration and
every certificate authority part of what can put a program on a user's computer. So the feed is signed with a key
that only the owner's release job holds, and Studio checks that signature with a public key it was built with.

| Key | Holds the private half | Use |
|---|---|---|
| `upd-<year>-a` | The `release` environment of the repository, as a secret. That environment already needs the owner's approval before the publish job runs | Signs every feed |
| `upd-<year>-b` | Offline only, with the owner's other standby keys. Never uploaded until it is needed | Takes over at once if the first key leaks, because every build already trusts it |

This is the arrangement the licence keys have (`docs/licensing.md`, "Rotation"), for the same reasons, and it uses
the same code: a `signed_file::trusted_key` for each, with the purpose `update-feed`, in
`libs/vss_update/src/trusted_keys_release.cpp`. Rotation and revocation work as they do there: a release adds the
next key; a rotated key gets a `not_after`; a leaked key gets a `revoked_from`.

The feed keys are **not** in the official plugin key list. That list holds the two plugin purposes and the
verifier refuses any other (`docs/plugins.md`, section 8.9); the owner decided the same for licence keys. The
feed, the licences and the plugins share the envelope and nothing else.

**The owner makes the keys.** Nothing in this work creates, holds or reads a private key. Until the owner adds the
public halves, the list is empty, a check answers "This build cannot check for updates", and the tests use the
throwaway test key that the licence tests use (`Vss::license_test_keys`, a key id that starts with `test-`, which
the product list never returns).

**A leaked feed key is not enough to push a bad update.** The attacker must also control what Studio reads from the
feed's address: the host, the domain or the user's network path with a certificate Studio accepts. Both together
are needed. That is the reason for signing at all, and it is also why the standby key must never share a system
with the first.

## 6. The check

### 6.1 The request

- One `GET` of the feed address over HTTPS. No query string, no body, no cookie, no saved state between requests.
- The `User-Agent` is the fixed text `VirtualSerialStudio`. It carries no version, no operating system and no
  identifier. Studio downloads the whole feed and chooses for itself, so the server is never told what is
  installed.
- TLS errors are never ignored and there is no "continue anyway".
- No redirect is followed for the feed. A download (section 7) follows redirects to `https` addresses only, since
  GitHub serves release files from a second host, and the hash decides whether the bytes are right.
- The system's proxy settings are used. Labs sit behind proxies.
- 15 seconds to connect and receive; a body over 64 KiB is cut off and refused.

What the server can see is what any web server sees of any request: the address the request came from and the
time. Nothing else is sent. The privacy page says exactly this.

### 6.2 When

- **By hand**: Help > Check for updates, or Check now in About. Always allowed unless a policy forbids it.
- **On the timer**, when "Check for updates automatically" is on, and it is off until the user turns it on
  (section 3.5): once a day. The check runs 60 seconds after the window is shown, so start-up is not slowed, when
  24 hours have passed since the last one; a Studio that stays open checks again when the 24 hours are up. A check
  that fails on the timer is silent and is tried again an hour later, three times, then left until the next day.
- Stored in `QSettings`: `updates/automatic` (false until the user says otherwise), `updates/asked`,
  `updates/includePrerelease`, `updates/lastCheck`, `updates/lastSerial`, `updates/skippedVersion`.

### 6.3 Turning it off

| Who | How | Effect |
|---|---|---|
| The user | Off from the start. Settings, or the check box in About, or the answer at first start | No timer check. A check by hand still works |
| An administrator | `"update_check": false` in the policy file (`docs/plugins.md`, section 8.6) | No timer check and no check by hand. The menu item and the button are disabled with "Update checks are turned off by your administrator." |

The policy file already has the ownership test that makes it an administrator's. Unlike the plugin rules, a policy
file that cannot be trusted does **not** turn the check off or on: the update check ignores an untrusted file and
follows the user's setting, because the worst an attacker gains by forcing a check is one request for a public
file.

### 6.4 Threads

`QNetworkAccessManager` lives on the GUI thread and is asynchronous, so nothing waits. Verifying a feed is one
Ed25519 check of a few kilobytes and is done where the reply arrives. A download is hashed as its bytes arrive, in
pieces of at most 64 KiB, so no single step is long. The file is written through a `QSaveFile`-like temporary and
never exists under its final name until the hash matched. Nothing in the update code touches the hub, an endpoint
strand or the agent.

## 7. Download and install

### 7.1 Which kind of install this is

| System | Test | Kind |
|---|---|---|
| Windows | The installer's Apps & features key (`...\Uninstall\VirtualSerialStudio`, 32-bit view) names the folder this program runs from | `windows-installer` |
| Windows | Anything else | `windows-zip` |
| macOS | The program is inside an `.app` bundle | `macos-dmg` |
| macOS | Anything else | `macos-archive` |
| Linux | `dpkg-query -S <program file>` names a package | `linux-deb` |
| Linux | `rpm -qf <program file>` names a package | `linux-rpm` |
| Linux | Neither | `linux-archive` |

The Linux questions are asked once, off the GUI thread, with a time limit, the first time a check needs the
answer. Each system's test is its own source file chosen in CMake, with no `#if`. A wrong answer cannot install
anything by itself: it only chooses which published file is offered, and the hash still has to match.

### 7.2 Download

- Into Studio's own cache folder (`QStandardPaths::CacheLocation`, `updates/`), under a temporary name. Files left
  there from an earlier run are deleted at start.
- The size in the feed is the limit, and the SHA-512 of what arrived must equal the feed's. Anything else is
  deleted and reported (section 3.3).
- **What is checked is what runs** (the rule of `docs/plugins.md`, section 5.4). On Windows the file is opened once
  with writing and deleting denied to others, hashed through that handle, and the handle is kept until the
  installer process has started. A program running as the same user therefore cannot swap the file between the
  check and the moment Windows asks for administrator rights. Elsewhere the file is handed to a system tool that
  asks for its own confirmation, and the same open-then-hash order is used.

### 7.3 Install, by kind

| Kind | After the download is checked |
|---|---|
| `windows-installer` | Studio says what will happen ("Studio will close and the installer will start."), stops the running ports the way Quit does, starts the installer and exits. The installer asks for administrator rights itself, shows its own pages and upgrades in place (`docs/releasing.md`, "Upgrade and uninstall"): the driver, the helper service and the COM numbers are kept |
| `windows-zip` | Saved to the Downloads folder. "Show in folder". The text says that this copy was unpacked from an archive and that the new one is unpacked in its place |
| `macos-dmg` | The disk image is opened. The text says to quit Studio and drag the app onto Applications. Studio does not replace its own bundle in the first version |
| `macos-archive` | As `windows-zip` |
| `linux-deb`, `linux-rpm` | The command is shown with a Copy button (`sudo apt install <file>`, `sudo dnf install <file>`), and, where `pkexec` exists, a button that runs it through `pkexec`, as the serial group step does. Then "Restart Studio to use the new version." |
| `linux-archive` | As `windows-zip` |

Studio never raises its own rights. Every step that needs an administrator is a system prompt that the user
answers, from a program the system starts.

Not in the first version, and worth doing once the plain path is proven: a silent Windows upgrade that starts
Studio again when it finishes (the installer's `/S` needs to learn the earlier driver and PATH choices and to
relaunch); replacing the macOS bundle in place; a signed apt and dnf repository so the system's own updater does
the work on Linux.

### 7.4 What the packages need

HTTPS needs a TLS backend in the Qt that ships:

| System | Backend | Change |
|---|---|---|
| Windows | Schannel (`tls/qschannelbackend.dll`) | The deploy step should already copy it because the program links `Qt6::Network`. The release check gains a test that it is there |
| macOS | Secure Transport, in the bundle's `PlugIns/tls` | The same |
| Linux | OpenSSL (`plugins/tls/libqopensslbackend.so`), which loads the system's `libssl.so.3` | The backend goes into `plugins/tls`; the deb and rpm depend on the system's OpenSSL 3 package; `check_linux_packages.sh` adds the file to its `ldd` list |

`vss_gui --selftest` gains one line: whether TLS is available. A package without it fails the release job, not the
user's first check.

## 8. Plugins

- **Official plugins only.** The feed lists plugins whose id starts with `com.virtualserialstudio.`. A feed entry
  with any other id refuses the feed.
- **Only what is installed.** Studio compares the feed with what discovery found (`docs/plugins.md`, section 5.2).
  It never offers a plugin the user does not have.
- **Scope decides who can update it** (the table in `docs/plugins.md`, section 5.2):

  | The installed copy | What the dialog offers |
  |---|---|
  | Came with Studio | Nothing of its own. It is updated with Studio, and the row says so |
  | For this user | Download, check and replace the folder in the per-user root |
  | For everyone on this computer | The download and the path, with the sentence that an administrator installs it. Studio does not raise its rights |

- **Two checks, not one.** The download must match the feed's SHA-512, and the unpacked plugin must then pass the
  plugin verifier with an official signature like any other plugin. The feed key cannot make a plugin official, and
  the plugin key cannot publish a feed.
- **A new version is a new hash**, so the trust decision is asked again (`docs/plugins.md`, section 8.4), with the
  "asks for more than the last one" line when the permissions grew.
- **The licence's version window.** When the licence has `updates_until` and the feed entry's `released` is after
  it, the row reads "1.3.0 was released after your licence's updates ended on <date>." and no install is offered,
  because that version would not run (`docs/licensing.md`, "The version window"). With no licence, a plugin that
  needs one is not installed in the first place, so nothing is offered.
- **How a plugin is packed for download** is the plugin install work's to decide (`vss_cli plugin install`). The
  feed gives a file, a size and a hash, and hands the checked file to that installer.

The plugin host, the Plugins page and the install command are not built (`docs/plugins.md`, "What is built").
So the feed format, the reader and the comparison cover plugins from the start, and the plugin rows appear in the
window when there is a Plugins page to put them on (section 12, step 7).

## 9. Code layout

| Part | Where | Notes |
|---|---|---|
| Versions: parse and compare semantic versions | `libs/vss_update` | No Qt, no network, no I/O. Namespace `vss::update` |
| The feed: verify, parse, limits | `libs/vss_update` | Uses `vss::license::signed_file`. A fuzz target for the reader |
| The choice: channel, newer-than, the file for an install kind, plugin candidates | `libs/vss_update` | Pure functions of the feed, the running version and the installed plugins |
| The feed keys | `libs/vss_update/src/trusted_keys_release.cpp` | Public halves only. Empty until the owner adds them |
| The service: timer, state, settings, policy | `apps/vss_gui/src/updates/update_service.*` | A `QObject` on the GUI thread. Talks to the network through a small `feed_transport` interface, so every test uses a fake and no test opens a socket |
| The transport | `apps/vss_gui/src/updates/network_transport.*` | The only file that includes `QNetworkAccessManager` |
| The kind of install | `apps/vss_gui/src/updates/platform/<system>/install_kind.cpp` | One file for each system, chosen in CMake |
| Download, hash, hand-over | `apps/vss_gui/src/updates/update_download.*` and `platform/<system>/start_install.cpp` | The Windows file holds the handle (section 7.2) |
| The screens | `apps/vss_gui/src/updates/update_dialog.*`, the Updates tab in `support/about_dialog.cpp`, the rows in `support/settings_dialog.cpp`, the action in `shell/` | Strings from the screen spec, in `tr()` |
| The feed tool | `tools/update_feed.py`, `tools/test_update_feed.py` | Builds the payload from a release's files and `SHA512SUMS`, signs it by calling OpenSSL as `tools/plugin_sign.py` does, and verifies a feed with a public key |
| Publishing | `.github/workflows/release.yml` (a `feed` job after `publish`), `tools/site_build.py` (copies the newest feed to `updates/v1/feed.json`) | Section 10 |

`FakeStudioBackend` is not involved: the update service is not part of the engine's backend. The service has its
own fake transport and a fake clock, and every state of section 3 can be shown from a test.

## 10. Publishing the feed

1. The release workflow's `publish` job publishes the release as it does today.
2. A new `feed` job runs after it, in the `release` environment. It lists the newest final release and the newest
   pre-release, reads each one's `SHA512SUMS` and file list, writes the payload with the next `serial`, signs it
   with `UPDATE_FEED_PRIVATE_KEY` and uploads `update-feed.json` to the new release. The `serial` is the number of
   releases published so far, read from the releases list, so it rises with every release and needs no stored
   state. A feed that has to be published again between releases (a summary corrected, a file withdrawn) is made by
   a manual run that is given the serial.
3. The job verifies what it signed with the public key in the repository before it uploads, so a secret that is
   not the key Studio trusts fails the run, as the GPG check does for the packages.
4. Without the secret the job writes "no update feed was signed" to the summary and succeeds, as the macOS signing
   step does. A pull request never signs.
5. The website build (`tools/site_build.py --refresh-release`) downloads `update-feed.json` from the newest
   release and writes it to `updates/v1/feed.json`. It does not parse, change or sign it. `tools/site_check.py`
   verifies the copy with the public key.
6. The Docs workflow publishes the site from `main`. A final release moves `main`, so the site and the feed follow.
   After a pre-release the owner runs the Docs workflow by hand on `main`, which `website/README.md` already asks
   for so that the downloads page lists the new files.

**Withdrawing a release.** Publish a feed with a higher serial that no longer names it. Studio then offers nothing,
or the release before. A release that is already installed is not removed; that is what a revocation list or a
newer release is for.

**Later: key lists and revocation lists.** `docs/plugins.md` (sections 8.5 and 8.9) and `docs/licensing.md`
("Revocation") say that these lists can only travel inside a release because there is no network. The feed could
carry the address and hash of a newer `official-keys.json` and `revocations.json`. Each has its own signature and
serial and is checked exactly as an imported file is, so the feed would add a way to deliver them and no new trust.
It is left out of the first version to keep it small.

## 11. Threats

| Threat | What stops it |
|---|---|
| Someone on the network path changes the feed or the download | HTTPS; then the feed's signature; then the download's SHA-512 from the signed feed |
| The web host, the domain or a certificate authority is taken over | The feed's signature. The attacker has no feed key, so Studio accepts nothing they write |
| A feed key leaks | Not enough alone (section 5). The standby key signs from then on, and the next release revokes the leaked one |
| An old, correctly signed feed is served again to offer an old version with a known fault | `serial`: lower than the highest accepted is refused. And an update is only ever a greater version than the one running |
| The newest feed is withheld, so users are not told of a fix | **Not stopped.** Studio shows "Last checked" with the date of the last feed it accepted. An attacker who can do this can already stop the request |
| A download is swapped between the check and the run | The file is held open from the hash to the start of the installer (section 7.2) |
| The feed's text is used to mislead (a false summary, markup, a long string) | Plain text only, length limits, signed by the owner's key in the first place |
| A huge or endless reply | 64 KiB for the feed; the feed's `size` for a download; time limits on both |
| The request is used to track users | Nothing identifying is sent (section 6.1). The address of a request is visible to the host, as with any request |
| A plugin update that is not what it says | The plugin's own official signature, checked after the hash (section 8) |
| The check is used to get a user to approve an administrator prompt for the wrong program | The prompt is for the file that was hashed, by the handle rule. On Windows the installer is also signed, and the prompt names the publisher |
| An administrator's wish that computers make no requests | The policy (section 6.3) |

## 12. Order of work

Each step is one pull request against `develop`, with its tests and a changelog fragment where the user sees a
change. Steps 1 and 2 touch no window and make no request, so they can merge before the owner has decided the
points of section 13.

| Step | What | Size and who |
|---|---|---|
| 1 | `libs/vss_update`: versions, the feed reader and verifier, the choice, the (empty) key list, unit tests, a fuzz target | Small. Well specified by sections 4 and 5 |
| 2 | `tools/update_feed.py` and its tests; the `feed` job in `release.yml`; the copy in `site_build.py` and the check in `site_check.py`; `docs/releasing.md` | Small. The workflow change is reviewed for what a pull request can reach |
| 3 | The screen spec `screens/12-updates.md` and the copy deck entries | Small, and the owner reads the words |
| 4 | The service (transport, timer, settings, policy key), the question at first start, the Updates tab, the Help action, the Settings rows, the chip and banner, the update dialog up to "Download and install"; translations; accessibility checks | Medium |
| 5 | Download, hash and the install step for each kind; the install-kind tests; the TLS backend in the packages and the release checks | Medium. The Windows hand-over and the packaging need a guest run |
| 6 | The rule change of section 2 in every place it is written; the privacy page; the user guide; the README if an option was added | Small. Goes in with step 4, because that is when the sentence becomes true |
| 7 | Plugin rows and the plugin install hand-over | After the Plugins page and `vss_cli plugin install` exist |

For the owner, outside the repository:

- Make `upd-2026-a` and `upd-2026-b` as the licence keys were made, and give the two public halves for
  `trusted_keys_release.cpp`.
- Add `UPDATE_FEED_PRIVATE_KEY` (the private half of `upd-2026-a`) as a secret of the `release` environment.
- Publish the website, so that the feed's address exists (`website/README.md`, "Publishing").

## 13. Decisions

Decided by the owner on 15 November 2026:

| # | Question | Decision |
|---|---|---|
| 1 | Is the automatic check on or off in a new install? | **Off.** Studio asks once, at the first start, whether to turn it on (section 3.5). When it is on: once a day, 60 seconds after start (section 6.2) |
| 2 | Where does Studio read the feed? | **The owner's domain** (section 4.1), copied there from the release asset by the website build |
| 3 | Is the feed signed with its own Ed25519 keys, which the owner makes and keeps? | **Yes** (section 5): `upd-<year>-a` in the release environment's secrets and `upd-<year>-b` offline as the standby |
| 4 | Does the Windows installer show its pages on an update, or run silently and restart Studio? | **It shows its pages.** A silent upgrade that starts Studio again is a later piece of work (section 7.3) |

Proposed, and open until the owner says otherwise:

| # | Question | Proposed |
|---|---|---|
| 5 | Who is offered pre-releases? | A user who turned on "Include pre-release versions", and anyone who is running a pre-release already |
| 6 | Are updates for third-party plugins wanted? | Not in the first version |
| 7 | Should a Linux package from a future apt or dnf repository leave updating to the system? | Yes, when such a repository exists. Until then the deb and rpm are updated as section 7.3 says |
| 8 | Is "the first start" of decision 1 the first start ever for that user, or the first start of each newly installed version? | The first start ever: one answer is kept across updates (section 3.5). Asking again after every update would be a prompt for each release |
