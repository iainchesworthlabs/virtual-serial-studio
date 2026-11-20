# Licensing (design)

This page is the design for licence files: their format, how Studio checks
them, what happens when one expires, and how the owner issues and tracks them.
It follows the decisions in [Editions, licensing and plugins](editions.md).
The owner has reviewed it, and the decisions are recorded at the end. The
library, the tool and the screens are built (see [What is built, and in what
order](#what-is-built-and-in-what-order)). The check gates nothing a user can
use in this build. The Licence page and the Support page ask what a licence
grants, the control gateway reads the licence to say which edition is in force,
and the plugin host asks whether a licence covers an official plugin before it
starts one. No official plugin exists yet, so a licence shows a state and
unlocks nothing.

The screens are specified in
`docs/gui-design/docs/screens/09-licence-and-support.md`.

## What a licence check can and cannot do

Studio is GPL software. Anyone may build a copy with the check removed, and the
GPL gives them that right. So the check in Studio is not the lock. It tells an
honest customer where they stand, and it tells Studio what to offer.

What is sold is what a patched Studio cannot give. None of these exists yet:

* official plugins, which are commercial programs in their own processes and
  check the licence themselves;
* the driver signed through Microsoft, which only the owner can produce;
* support.

The rules that follow from that:

1. An official plugin verifies the licence file itself, with the same library.
   It never trusts Studio's word that a licence is valid.
2. The checks are deterrents. The design does not try to defeat someone who
   patches a binary or edits their clock with care. It never punishes a paying
   customer to do so.
3. A licence never gates anything in the Community edition. Ports, topologies,
   capture and files work the same with no licence, a valid one, an expired one
   or a broken one.

## The licence file

A licence is one small text file, `<licence id>.vsslic`, that the owner sends to
the customer. It is JSON, so a person can open it, and it has two layers.

### Envelope

```json
{
  "format": 1,
  "purpose": "licence",
  "key_id": "lic-2026-a",
  "payload": "<base64url, no padding, of the payload bytes>",
  "signature": "<base64url, no padding, 64 bytes>"
}
```

The payload is carried as the exact bytes that were signed. The verifier checks
the signature over those bytes first and parses them second. Nothing is put
into a canonical form, so there is no canonicalisation step to get wrong, and
no parser ever reads unsigned data beyond the five envelope fields.

`format` is the version of the envelope and of the rules on this page. A
verifier refuses a format it does not know, and says "made for a newer version
of Studio".

### Payload

The payload is UTF-8 JSON:

```json
{
  "licence_id": "VSS-2026-000123",
  "kind": "annual",
  "edition": "pro",
  "licensee": { "name": "Example Lab Pty Ltd", "contact": "Test team" },
  "issued_at": "2026-11-13T02:10:00Z",
  "starts_at": "2026-11-13T00:00:00Z",
  "expires_at": "2027-11-13T23:59:59Z",
  "grace_days": 28,
  "seats": 3,
  "features": ["plugins.official", "driver.signed", "support"],
  "machines": [],
  "updates_until": null,
  "replaces": null,
  "critical": []
}
```

| Field | Meaning |
|---|---|
| `licence_id` | Unique, never reused. `VSS-<year>-<six digits>`. It is the key of the owner's records and the handle for revocation and support |
| `kind` | `trial`, `annual` or `enterprise` |
| `edition` | `pro` or `enterprise`. Community has no file |
| `licensee.name` | The person or organisation the licence is issued to. Shown on the Licence page |
| `licensee.contact` | Optional. A team or role, not a personal e-mail address |
| `licensee.email` | Optional. The email address the licensee gave when they asked for the licence. It must have the form of an address (ASCII, one `@`, a domain with a dot). It is personal data: Studio shows it on the Licence page of the computer that holds the licence, and a support bundle leaves it out unless the user turns on the row that adds the licensee's name and email |
| `issued_at` | When the owner signed it. Used for key rotation and the clock check |
| `starts_at`, `expires_at` | The term, in UTC. A renewal bought early starts when the old one ends, so no days are lost |
| `grace_days` | Days after `expires_at` during which Pro features still work. 28 for annual, 0 for trial, and whatever was agreed for enterprise (28 unless the agreement says otherwise) |
| `seats` | Number of seats agreed. `0` means the licence is not counted by seat; the Licence page then shows the label "Site licence" |
| `features` | What the licence turns on. See below |
| `machines` | Optional list of computer names. Empty means any computer. See [Seats](#seats) |
| `updates_until` | Optional date for the version window. See [The version window](#the-version-window) |
| `replaces` | Optional id of the licence this one renews or corrects |
| `clock_reset` | Optional token that resets the stored time once. See [The clock check](#the-clock-check) |
| `critical` | Names of fields a verifier must understand. See below |

Rules for reading it:

* The file is at most 64 KiB. Larger files are refused before parsing.
* Unknown fields are ignored, so a later version can add fields. A field named
  in `critical` that the verifier does not know makes the licence invalid, with
  the message "made for a newer version of Studio". This lets the owner add a
  restriction later that old versions cannot silently ignore.
* Duplicate keys, invalid UTF-8, dates not in the `Z` form shown and
  `expires_at` before `starts_at` are errors.
* Dates are compared in UTC. The user sees them in local time and their locale.

### Features

A feature is a dotted name. A licence lists exact names or a prefix ending in
`.*`.

| Feature | Turns on |
|---|---|
| `plugins.official` | Loading official plugins as a group |
| `plugins.<id>` | One official plugin, for a licence that sells plugins singly |
| `driver.signed` | Entitlement to the Microsoft-signed driver package (a term of the licence; see [What never degrades](#what-never-degrades)) |
| `support` | Support. Shown on the Support page |

The plugin framework owns the plugin ids. This library only answers "does the
current licence grant feature X, and if not, why".

A licence gates the official plugins, not the plugin API: the API is public,
documented and free. The official plugins built on it, and the agent's internal
protocol, are private with no compatibility promise.

The administrator policy that allows official plugins only is not a licence
feature. It is the policy of [the plugin framework](plugins.md), section 8.6,
and it works in every edition. No feature name stands for it.

Pro and Enterprise licences turn on the same plugins. An Enterprise licence has
no fixed shape: what it covers, how long it runs and the price are negotiated
with each customer. `edition: enterprise` marks such a licence and adds no
feature of its own. The fields of the file (`seats`, `machines`, the dates,
`grace_days`, `updates_until`) carry whatever was agreed.

## Signatures and keys

### Scheme

Ed25519 (RFC 8032, the pure variant). It is fast, has small keys and
signatures, is deterministic, and has no parameters to choose wrongly.

The message that is signed is not the bare payload:

```
"VSS-SIGNED-FILE-1" 0x00 <purpose> 0x00 <payload bytes>
```

`purpose` is the envelope's `purpose` string. A licence uses `licence`, and the
plugin signer uses its own. With the purpose inside the signed message, a
signature made for one kind of file can never be accepted as another, even if a
key were used for both by mistake. The licence key and the plugin signing key
are separate keys in any case, as the owner decided.

### The implementation

No cryptography is written for this project. The proposal is to vendor
[Monocypher](https://monocypher.org) (two C files, BSD-2-Clause or CC0, audited
by Cure53 in 2020), using its Ed25519 functions. The alternative is libsodium
through vcpkg. Monocypher is preferred because Studio only verifies, the
project today has one vcpkg dependency (Tracy) and takes Boost from a download,
and a new shared library would have to be packaged and signed on three
systems. The owner chose Monocypher.

Whichever is used, the tests run the RFC 8032 test vectors and the Project
Wycheproof Ed25519 vectors (malleable signatures, non-canonical points, small
order keys) against it, and the envelope parser has a fuzz target next to the
existing ones.

Monocypher accepts a public key or an `R` that is not in canonical form
(Wycheproof test 151 is one). The library therefore checks both encodings
itself before it calls Monocypher: the `y` value must be below 2^255 - 19, and
the sign bit must be clear when `x` is zero. With that check every Wycheproof
case gives the expected result.

### The trust list

Studio and each official plugin carry a list of public keys:

| Field | Meaning |
|---|---|
| `key_id` | Such as `lic-2026-a`. Matches the envelope |
| `purpose` | `licence` or the plugin signer's purpose. A key verifies one purpose only |
| `public_key` | 32 bytes |
| `not_before`, `not_after` | The window of `issued_at` the key may sign for |
| `revoked` | Set by a later release. See [Revocation](#revocation) |

Licence keys are in this built-in list only. They are not in the plugin key
list that `docs/plugins.md` describes, they are not signed by the plugin root
key, and the two sides share no key. A mistake or a leak on one side then
cannot touch the other. Revocation is separate in the same way: the
`revocations` purpose here covers licences and licence keys, and the plugin
framework has its own purpose and its own file.

The list is a source file chosen by CMake, not by the preprocessor:
`trusted_keys_release.cpp` in the library. The test key is in a separate
target, `vss_license_test_keys`, that only the test programs and the test mode
of `vss_license` link. The release list holds the owner's two
licence keys, `lic-2026-a` (which signs) and the standby `lic-2026-b`, each for
licences and for revocation lists. A build with an empty list accepts no
licence and is the Community edition, which is the safe way for a list to be
wrong.

Test keys. The tests use the key pair of test vector 1 of RFC 8032, whose
private key is published in the RFC, under the key id `test-rfc8032-1`. It is a
throwaway by construction. The `test-` prefix is refused by the code that loads
the release list, so a test key cannot verify a licence in a product even if
the wrong file were linked. No other private key is ever committed, generated
in CI or written to a log.

### Rotation

Two licence keys exist from the first release: `lic-<year>-a`, which signs, and
a standby `lic-<year>-b`, whose private half has never been on any online
system. Both public halves ship in every build.

* **Planned rotation.** A release adds the next key. The owner starts signing
  with it once the release that carries it has been out long enough (the
  proposal is one release cycle, and never less than the 28 day grace period).
  The old key stays in the list with a `not_after` date, so the licences it
  signed keep working to the end of their terms.
* **A customer on an old Studio** that does not know the new key sees "This
  licence needs a newer version of Studio", with the version to install. The
  owner can still sign for old versions with the old key until its `not_after`.
* **Emergency rotation**, when a key may have leaked. The owner starts signing
  with the standby at once, since it is already trusted everywhere. The next
  release marks the leaked key revoked for licences with `issued_at` after the
  leak date. Licences issued before that date keep working. A forger can
  backdate `issued_at`, so if the leak date is unknown or forgeries appear, the
  release revokes the key outright and the owner reissues every live licence
  from the records. That is why the records must be complete enough to reissue.

### Revocation

Studio does not fetch a revocation list over the network. The check for
updates ([Online updates](updates.md)) could carry one, and that is left out of
the first version, so revocation can only travel with something the customer
installs. There are two carriers:

1. **Each release of Studio and of each official plugin carries a revocation
   list**: licence ids and key ids, with a sequence number.
2. **The list is also a signed file** in the same envelope (`purpose`:
   `revocations`), so it can be sent to a customer or shipped inside a plugin
   package between releases. Studio keeps the list with the highest sequence
   number it has seen and never goes back to a lower one.

The release's own list is in `libs/vss_license/src/revocations_release.cpp`,
which `builtin_revocations()` returns. Today it is empty, with sequence 0. When
a licence or a key is withdrawn, the owner adds its id there and sets the
sequence number to that of the signed list of the same day, so that the release
and the file say the same thing. Studio also reads signed lists (`.vssrev`
files) from the licence folders. A signed list replaces the release's list only
when its sequence number is higher, so signed lists start at 1.

A revoked licence shows as invalid, with the reason "This licence was withdrawn.
Contact support." Revocation is for refunds, chargebacks, a leaked file and a
licence issued in error. It takes effect when the customer next updates, which
is acceptable because the term is a year at most and a trial is 14 days. The
revocation list only ever holds ids, never names.

## Where Studio finds a licence

| Place | Used for |
|---|---|
| Per-user folder (`%LOCALAPPDATA%\Virtual Serial Studio\licences`, `$XDG_DATA_HOME/virtual-serial-studio/licences` or `~/.local/share/virtual-serial-studio/licences`, `~/Library/Application Support/Virtual Serial Studio/licences`) | What Help, Licence, Import licence and `vss_cli licence import` copy the file to |
| Per-machine directory (`%ProgramData%\Virtual Serial Studio\licences`, `/etc/virtual-serial-studio/licences`, `/Library/Application Support/Virtual Serial Studio/licences`) | An administrator installing a licence for the computers they manage. Studio only reads it |

On Linux the per-user folder is under `$XDG_DATA_HOME` when that is an absolute
path, and under `~/.local/share` when it is unset, empty or not absolute. The
latest time seen and the clock resets used are kept in `licence-clock.json`,
beside the per-user `licences` folder.

Studio reads every `.vsslic` in both places (at most 64 from each, in name
order, and none larger than 64 KiB) and uses the best one: a valid
licence beats one in grace, which beats an expired one; then Enterprise beats
Pro; then the latest expiry. The others are listed on the Licence page.
Importing never deletes an older file, and removing a licence is a button with
a confirmation.

## States

The library turns a file, the trust list, the revocation list, the time and
the computer's name into one status:

| State | When | Pro features |
|---|---|---|
| `none` | No file | Off. Community |
| `valid` | Inside the term | On |
| `expiring` | Valid, 30 days or fewer left (trial: 3 days) | On, with a notice |
| `grace` | Past `expires_at`, within `grace_days` | On, with a notice that counts down |
| `expired` | Past the grace period | Off. Community |
| `not_yet_valid` | Before `starts_at` | Off |
| `clock_suspect` | The clock check failed. See below | Off until the clock is right |
| `invalid` | Bad signature, unknown or revoked key, revoked licence, wrong computer, unknown format or critical field, unreadable file | Off |

Every state has a reason code and one sentence for the user. The sentences are
in the screen spec. An `invalid` state never leaves Studio unusable: it is the
Community edition with a notice on the Licence page.

### Expiry and the grace period

* For the last 30 days of a term the Licence page and a banner that can be
  dismissed say when the licence ends and how to renew.
* After `expires_at` the grace period runs. Pro features work. The banner says
  how many days are left and cannot be dismissed for good; it returns once a
  day.
* After the grace period, official plugins do not start. A plugin that is
  running when the grace period ends keeps running until it is stopped or
  Studio exits, so a test run in progress is never cut off part way.
* A trial has no grace period. It ends at `expires_at` with the same rule for
  a plugin that is running.
* Installing a renewal takes effect at once, without a restart.

### What never degrades

* Every port, link, topology, capture and saved file, and every Community
  feature.
* **Opening and saving a file that uses a Pro feature.** A topology that names
  a plugin opens, the plugin's items are shown as unavailable with the reason,
  and saving keeps them exactly as they were. Studio never strips Pro content
  from a file.
* **The installed driver.** Studio never removes or disables the signed driver
  when a licence ends. The entitlement covers obtaining and updating it, which
  is a term of the licence and not a check in the program.
* Reading the Licence and Support pages, and making a support bundle.

### How a feature reports that it is unavailable

This follows `docs/gui-design/docs/08-engine-gaps.md`: a feature asks, and when
the answer is no it is shown disabled with the reason, never hidden without
one and never faked. The engine's `capabilities` struct stays about the engine.
Licensing has its own answer type:

```
entitlement licence_service::check(feature) ->
    { granted: bool, reason: reason_code, licence_id, days_left }
```

The GUI reads it through `StudioBackend`, so `FakeStudioBackend` can put any
state on screen in a test. The reason codes map to exact sentences in the
screen spec, such as "Needs a Pro licence" and "Your licence ended on 13
November 2027".

### The clock check

A deterrent only, as decided.

* Studio keeps the latest time it has seen in a file, `licence-clock.json`, in
  its per-user application data folder, and updates it when it starts and once
  an hour. The file is a small JSON object with `latest_seen` (a time) and
  `applied_resets` (the reset tokens already used). Studio does not pass the
  stored time to a plugin. A plugin runs confined and cannot read Studio's
  folder, so a plugin that checks the licence keeps its own latest time in its
  own data directory, uses its release date and the licence's `issued_at` as
  the other lower bounds, and applies a `clock_reset` token to its own file in
  the same way, once.
* Two more lower bounds cost nothing: the build date of Studio itself and the
  `issued_at` of the licence.
* If the clock reads more than 48 hours before the highest of those, the state
  is `clock_suspect`: "The date on this computer looks wrong". Pro features are
  off until the clock is right. 48 hours covers time zones, daylight saving and
  a dual-boot machine that keeps local time.
* Nothing is ever locked for good. Correcting the clock clears the state. If
  the stored time is itself wrong, because the clock once ran ahead, the user
  sends a support bundle and the owner sends a new licence file with a
  `clock_reset` token in it. When Studio imports a verified licence whose
  token it has not used before, it sets the stored time back to that licence's
  `issued_at` and remembers the token, so one file works once on a computer.
  That uses the signature that is already there and needs no extra secret. A
  later `issued_at` alone cannot do this: the stored time is ahead of the real
  date, so no honest `issued_at` is later than it.

The stored time is plain settings data. Someone can delete it. That is the
limit of a deterrent, and the design accepts it.

## Trials

A trial is a licence file with `kind: trial`, the Pro features and a 14 day
term, issued by the owner on request, as decided. There is no self-starting
trial inside Studio: with no server, a built-in trial is reset by deleting a
settings file, and it would put a second, weaker mechanism beside the signed
one.

* One trial for each organisation, tracked in the owner's records. A second
  one is the owner's call.
* A trial has `seats: 1`, no grace period and no machine binding.
* Buying after a trial is a new file. Nothing is reinstalled and nothing made
  during the trial is lost.

## Seats

One Pro licence is one seat. A seat is two computers: the one you work at and one other that you control or monitor from it. The serial device can be on either.
Controlling one of the two from the other is part of the seat. There is no
server, and Studio never sends a licence anywhere, so nothing can count
installations. Activation and seat counting stay offline. The only connection
Studio makes is the check for updates ([Online updates](updates.md)), which
sends nothing about the licence. The options:

| Option | How it works | For | Against |
|---|---|---|---|
| A. Named licensee, count by the terms | The file says who it is for and how many seats. Studio shows both. The licence terms bind the customer | No friction. Works offline and in a virtual machine, survives new hardware. No owner work after issue. No data collected | Nothing stops a copied file. The licensee's name in the file is the deterrent: a leaked file names its source and can be revoked |
| B. One file for each seat | As A, with a seat number in each file | A leak points at one seat | More files for the customer to place. Still nothing technical stops copying |
| C. Machine names in the file | The file lists computer names. Studio compares its own name | A real limit for a site that wants one. Uses a name the administrator already manages. No hardware fingerprint | A new or renamed computer needs a new file from the owner. A name can be set by the user, so it is a control for honest sites, not a lock |
| D. Hardware fingerprint | The customer sends a code made from hardware ids, and the file is bound to it | Hardest to copy | Breaks on a new disk, a virtual machine or a container. A hardware id is personal data. A steady load of reissue requests on the owner. Hostile to the lab customer |
| E. Seats find each other on the network | Copies of Studio announce themselves and count | A true count on one network | Breaks the rule that Studio makes no network connection of its own except to check for updates. Fails across subnets. Looks like telemetry |

**Recommendation.**

* **Pro: option A.** One file for each order, with `seats: N` and the licensee
  named. Studio shows "Licensed to Example Lab Pty Ltd, 3 seats" on the Licence
  page and in the support bundle. The terms say what a seat is. This matches
  "enforced by the licence file and the licence terms, not by a server" in the
  editions page.
* **Enterprise: whatever is agreed.** The file can carry a seat count
  (option A), computer names (option C), both or neither. `machines` holds
  computer names, compared without regard to case, where a trailing `*` matches
  a prefix (`LAB-RIG-*`). A computer that is not listed gets `invalid` with
  "This licence is for other computers" and the list of names, so the
  administrator can see why.
* **Not D and not E.**

The second computer of a seat installs the same file, whichever of the two has
the serial device.
Studio does not tell the two apart. If the remote monitoring plugin later wants
to check that both ends hold the same licence id when they connect, that is a
check inside a connection the user has already set up, and it belongs to the
remote control design, not to this one.

## The version window

Open decision 3 in the editions page: does an expired Annual licence keep
working with the Pro versions released during its term?

The format supports either answer with one optional field. When
`updates_until` is set, an official plugin whose signed manifest has a release
date on or before that date keeps working after the licence expires, for good.
Plugins released later need a current licence. When the field is absent, expiry
means what the States table says. The release date comes from the plugin's
signed manifest, never from a file time.

The comparison is in the library: `check(status, feature, released)` grants a
feature the licence names when the licence has ended, sets `updates_until` and
the release date is on or before it, and says so in `by_version_window`. It
applies to a licence that has ended and to nothing else: one that is withdrawn,
for other computers, not started, or judged while the clock looks wrong grants
nothing through it. The plugin check (`vss::plugin::check_licence`) passes the
`released` date of the plugin's manifest; a plugin with no release date gets
the plain answer. The Licence page does not yet show a second "ended" variant
for a licence with a window.

| | Subscription only (`updates_until` absent) | Version window (`updates_until` = `expires_at`) |
|---|---|---|
| After expiry | Community, after the grace period | The versions already released keep working. No updates, no support |
| For the customer | Must renew to keep a rig running | A validated rig can be frozen and keeps working. This is how many labs buy tools |
| For the owner | Renewal is needed, so income is steadier | Renewal has to be earned by updates and support. Old versions stay in use and unsupported |
| Complexity | None | Plugins must compare a date. The "expired" screens need a second variant |

**Recommendation.** Ship the field, leave it absent on Annual licences, and
offer it in Enterprise agreements. That keeps the decided behaviour (an expired
licence returns Studio to Community) as the one behaviour to explain and test
for Pro, and lets the owner say yes to a lab that needs a frozen rig without a
change to the format. Changing the default later means issuing files with the
field set; no released version needs to change.

## The tools

### The verification library

`libs/vss_license`, in this repository under GPL-3.0-or-later, because Studio
links it. It has no Qt and no network code, and builds as two parts:

| Part | Does | Used by |
|---|---|---|
| `vss::signed_file` | The envelope: parse, size limits, base64url, the signed message, Ed25519 verify, the trust list with purposes and windows | The licence code, and the plugin signer for its manifests |
| `vss::license` | The payload, the states, the clock check, revocation, machine names, `check(feature)` | Studio's window (its backend), `vss_license`, `vss_cli licence`, the plugin library `vss_plugin`, official plugins |
| `vss::license::store` | The licence files on this computer: the per-user and per-machine folders, the best licence, import, remove, the revocation lists and the stored latest time | Studio's window (its backend, through a thin adaptor) and `vss_cli licence` |

Time, the computer's name and the stored latest time are passed in, so every
state can be tested without touching the clock. Results are
`vss::result<T>`. Signing lives in a third small part, `vss::signed_file_sign`,
that Studio does not link.

`vss::license::store` is where the files are read. It has no Qt. It reads a
file only after checking its size, copies an imported file to a new name in the
per-user folder (it never replaces a file), and never writes to the per-machine
folder. `vss_cli licence` prints what the store found, and
`vss_cli licence import <file>` imports a file through it. The control gateway
(`vss_gate`) reads the licence folders through it as it starts and once a minute
after that, and the plugin host is given the licence on this computer and
asks whether it covers an official plugin. The agent (`vss_agent`) does not ask
it.

The plugin framework session should build its signature check on
`vss::signed_file` and not write a second one. A note saying so is in the
shared notes folder.

### The plugin's own check

An official plugin checks the licence itself and does not take Studio's word
for it. It may not copy or link `libs/vss_license`, which is GPL. So there is a
second reader for a plugin to copy, in `sdk/licence-reader`: one header and one
source file in C++17, which need only the standard library and the plugin
author's own copy of Monocypher.

`sdk/licence-reader` is the one exception to the rule that every first-party
file in this repository is GPL-3.0-or-later. The reader is under the BSD
2-Clause licence and the test vectors beside it are under CC0 1.0. The
exception covers that folder and nothing else. The reader was written from
this page and includes nothing from the rest of the repository.

What the reader does:

* It checks the file as this page describes: the size, the envelope, the
  purpose, the signature, the payload, `critical`, the key's window, the
  clock, the term, the features and the version window.
* The caller passes in the list of keys it trusts, the time, its build date
  and its stored latest time. The reader has no trust list, reads no file and
  reads no clock.
* It answers with one of the reason names of `vss::license::reason` as text,
  and the `licence_id`, which the plugin sends in `licence.report` unchanged.
* It does not look at a revocation list or at `machines`. A plugin has no
  revocation list and no use for the computer's name; Studio checks both
  before it starts the plugin.

`sdk/licence-reader/vectors` holds licence files with the answer a reader must
give for each: a good licence, one that is expiring, in grace, expired or not
yet valid, a wrong purpose, an unknown key, a key outside its window, bad and
malleable signatures, an unknown critical member, an unknown format, a file
over 64 KiB, a missing feature, a suspect clock, bad base64url and duplicate
members. Every file is signed with the published test key `test-rfc8032-1` and
never with a key that issues real licences. A unit test runs each vector
through `libs/vss_license` and through the reference reader, and fails when
either gives another answer or when the two disagree. The same test puts
several thousand changed and odd files through both. The files are made by
the test tool `vss_licence_vectors`; the folder's README has the command.

### `vss_license`

A command line tool, offline, in this repository. It is built with the other
tools (`VSS_BUILD_TOOLS`) as the target `vss_license_program`, and the file is
named `vss_license`. It is the owner's tool: it issues licences, signs
revocation lists and makes keys, and customers do not need it. It is not
installed with Studio and is in no installer, archive, disk image, deb or rpm.
`tools/ci/check_package_contents.py` fails a release if the tool, one of its
libraries or a key file is in a staged tree or a package, and its test fails
if an `install()` rule ever names the tool. A customer imports a licence with
Help, Licence.

| Command | Does |
|---|---|
| `vss_license inspect <file> [--test]` | Prints the envelope and payload as text. Says whether the signature verifies. When it does not, it says that nothing below can be trusted and still prints the payload. Never fails on an expired licence. Exit status 0 whenever the file can be read as a signed file |
| `vss_license verify <file> [--at <date>] [--machine <name>] [--revocations <file>] [--test]` | Runs the same check Studio runs and prints the state, the reason, the time it judged at and which revocation list it used. It always uses the list built into the build (see [Revocation](#revocation)), and takes the list in a `--revocations` file instead only when that file's sequence number is higher. Exit status 0 when the licence's features work (valid, expiring or grace), 1 for any other state, 2 for a usage error. `--at` answers "what will this say in March?" and takes `2027-03-01` or `2027-03-01T00:00:00Z` |
| `vss_license issue --request <file> --out <file> --key-id <id> (--key-file <path> \| --key-env <name>) [--ledger <file>]` | Checks a request (the payload fields, without the signature), signs it, checks its own output as a reader would and writes the licence. A request is read more strictly than a licence is: a field that is not in the payload table is refused and named, since a reader would ignore it and the licence would then differ from the request. `critical` must be empty in this version, because the tool writes no critical fields. `features` must be present (`[]` is allowed). `licence_id`, and `replaces` when given, must be of the form `VSS-<year>-<six digits>`. `issued_at` may be at most 48 hours after the computer's clock, and not after `expires_at`. A trial must have `grace_days` 0, `seats` 1 and no `machines` (see [Trials](#trials)). `revocations` refuses a `sequence` below 1 |
| `vss_license issue --request <file> --dry-run [--ledger <file>]` | Checks the request and prints what the licence will say. Uses no key and writes nothing |
| `vss_license issue --request <file> --out <file> --test` | Signs with the published RFC 8032 test key. No product accepts the result |
| `vss_license revocations --request <file> --out <file> ...` | Signs a revocation list. It takes the options of `issue` except `--ledger`, including `--dry-run` and `--test` |
| `vss_license keygen --key-id <id> --out-private <path>` | Makes a key pair on the owner's own computer, writes the private half to a new file that only the owner can read, and prints the public half as base64url and as the bytes to put in the trust list. It never prints a private key |

The options of `issue` and `revocations`:

| Option | Meaning |
|---|---|
| `--request` | The request file. Required |
| `--out` | The file to write. Required unless `--dry-run` is given. It is never overwritten |
| `--key-id` | The id of the signing key: lower-case letters, digits and hyphens, at most 64. Required with `--key-file` or `--key-env` |
| `--key-file`, `--key-env` | Where the private key is: a file, or the name of an environment variable. Exactly one of them |
| `--ledger` | `issue` only. A JSON Lines file with one JSON object on each line. A line with a `licence_id` puts that id in the ledger, and a licence id that is in the ledger is refused. A ledger that cannot be read, or has a line that is not an object, stops the command |
| `--dry-run` | Checks and prints. Uses no key |
| `--test` | Signs with the published test key, key id `test-rfc8032-1`. It takes no `--key-id`, `--key-file` or `--key-env`. A `--key-id` that starts with `test-` is refused without it |

Exit statuses are those of `tool.hpp`: 0 for success, 1 when the file is not
valid or the command could not do what was asked, and 2 for a usage error.
Run with no command, the tool prints its usage and exits with 2. `--help` or
`help` prints it and exits with 0.

The private key is read from a file or an environment variable, never from the
command line, where it would appear in the process list and the shell history.
Its text is the base64url of the 32 byte seed, 43 characters, with white space
around it ignored, so the file may end with a new line. `keygen` writes it in
that form. The tool never prints key material and has no network code.

`issue` and `revocations` check their own output against the trust list of the
build they run in, with the same check a reader makes, and refuse to write a
file that does not pass. So the public half of a real key must be in
`trusted_keys_release.cpp` of that build before the key can issue. The key's
`not_before` and `not_after` window must also include the `issued_at` of what
it signs. `--test` adds the test key to the list for that one check.

The editions page puts "licence verification and issuing tools" in the private
`virtual-serial-studio-pro` repository. This design uses a different
split, which the owner accepted: the code is public, because Studio must
verify and the security rests on the key and not on the code being hidden, and
what stays private is the key, the customer records, the ledger and the
signing workflow.

## The owner's side

### What is kept, and where

Everything is in the private repository `virtual-serial-studio-pro`, under
`licences/`:

| Path | Holds |
|---|---|
| `customers/<customer id>.json` | Name of the organisation, a contact, country, notes, the ids of their licences. The least needed to issue, renew and support |
| `requests/<licence id>.json` | The request that was signed: the payload fields |
| `issued/<licence id>.vsslic` | The file that was sent, so it can be sent again |
| `ledger.jsonl` | The audit log. One line for each event, appended only |
| `revocations.json` | The revocation list and its sequence number |

A ledger line records what was done, to which licence id, when, by which
workflow run or which computer, with which key id, and the SHA-256 of the file.
Git history is the tamper check: the branch is protected and the signing job is
the only thing that writes to `issued/` and the ledger.

These records hold personal data about customers. The repository stays
private, holds no payment details, and a customer's record is removed on
request once no live licence depends on it. A solicitor should confirm what the
Australian Privacy Act and, for European customers, the GDPR require.

### Issuing

1. The owner writes `customers/` and `requests/` files on a branch and opens a
   pull request. A small script fills in the next licence id and the dates for
   a trial or an annual licence.
2. A check on the pull request runs `vss_license issue --dry-run`: it validates
   the request and prints what the licence will say. No key is involved.
3. The owner merges. The `issue-licence` workflow starts and waits at the
   `licence-signing` environment, which has the owner as required reviewer,
   the same arrangement as the `release` environment.
4. On approval the job signs with the key from the environment secret, runs
   `vss_license verify` on the result, commits the licence to `issued/`,
   appends the ledger line, and stops. It prints the licence id and the hash,
   never the key.
5. The owner sends the file to the customer.

Renewing is the same with `replaces` set and `starts_at` equal to the old
`expires_at`. A script lists the licences that end within 45 days, so the
owner can write to the customer before the in-app notice appears.

Revoking is a pull request that adds the id to `revocations.json` with a
reason and raises the sequence number. The same environment signs the list. It
goes into the next release of Studio and of each plugin, and can be sent to a
customer as a file.

### Where the private keys live

| Key | Lives | Why |
|---|---|---|
| Signing key, `lic-<year>-a` | A secret of the `licence-signing` environment in the private repository. The environment allows the protected default branch only and requires the owner's approval | Signing is gated, logged and repeatable, and the key is not on a laptop |
| The same key, backup | Encrypted with a passphrase, on two offline media kept in two places | A lost GitHub account or secret must not end the ability to issue |
| Standby key, `lic-<year>-b` | Offline only, the same two media. Never uploaded anywhere until it is needed | So that anything that exposes the online key cannot expose the standby too |
| Plugin signing key | Separate, with its own environment, in the plugin framework's design | Decided: separate keys |

The owner generates the keys with `vss_license keygen` on their own computer,
adds the secret and the environment, and adds the public halves to the trust
list. No agent and no CI job creates a key, a secret or an environment.

The workflow pins every action to a full commit, takes no input from outside
the repository, never runs on a pull request, and gives the job no permission
beyond writing to the repository. An environment secret can be read by anyone
who can change the workflow on the protected branch and get a run approved, so
the branch protection and the approval are part of the key's protection. The
stronger arrangement is a hardware token that never releases the key, with the
owner signing by hand on their own computer. `vss_license issue --key-file`
already allows local signing, and adding a token later changes no file format.
The owner chose the environment secret to start with.

### If things go wrong

| Event | Response |
|---|---|
| A customer loses the file | Send `issued/<id>.vsslic` again |
| A licence was issued with a mistake | Issue a new one with `replaces`, and revoke the old id |
| A file is found shared in public | Revoke the id, and talk to the licensee named in it |
| The signing key may have leaked | Emergency rotation, above |
| The private repository is lost | Restore from its backup. The ledger and `issued/` are enough to resend every live licence |

## What is built, and in what order

Each step is its own small pull request with tests in the project's own
framework. All four steps are built.

1. `vss::signed_file` and `vss::license`, with the RFC 8032 and Wycheproof
   vectors, a table test of every state, and a fuzz target for the parser.
2. `vss_license` (`inspect`, `verify`, `issue`, `revocations`, `keygen`), with
   tests that issue with the test key and verify the result.
3. The Licence page, the import flow and the notices, against
   `FakeStudioBackend`.
4. The support bundle additions and the Support page.

Built beyond those four steps: the licence store (`vss::license::store`, used by
Studio, by `vss_cli licence`, and by `vss_gate`, which reads the licence folders
as it starts and once a minute after that, so the `Edition:` line of
`vss_cli control status` agrees with `vss_cli licence`), and the plugin host,
which asks whether the licence on this computer covers an official plugin
(`vss::plugin::check_licence`) before it starts one, and hands the plugin the
licence file when it asks. The gateway only reads the folders; no command of
the gateway needs a licence.

What is not built: any use of `check(feature)` that changes what Studio offers
to a user (the Licence page and the Support page ask it, to say what the
licence grants, the plugin host asks it for an official plugin, and no official
plugin exists), a licence service in the agent, and `vss_cli plugin run`
reading the licence, which it does not. `vss_license` is built with the tools
and is not installed.

The private repository, its workflow, the environment and the real keys are
the owner's to create. This repository gives them the tool and this page.

## Decisions

The owner accepted the recommendations of this page on 13 November 2026:

1. **Seats.** Pro is counted by the terms, with the licensee named in the
   file. Enterprise may name computers. No hardware fingerprint.
2. **Version window.** The `updates_until` field exists. Annual licences do not
   set it, and Enterprise agreements may.
3. **Trials.** Issued by the owner only, one for each organisation, with no
   grace period.
4. **Where the code lives.** The library and `vss_license`, including `issue`,
   are in this public repository. The keys, the records and the signing
   workflow are private. This replaces the line of the editions page that puts
   the tools in the private repository.
5. **The Ed25519 implementation.** Vendored Monocypher.
6. **The signing key.** A secret of an approval-gated environment. A hardware
   token can replace it later with no change to the file format.
7. **Machine names.** A computer name, with a trailing `*` to match a prefix.
8. **The running plugin rule.** A plugin that is running when the grace period
   ends keeps running until it is stopped.
9. **One bundle.** The diagnostic bundle becomes the support bundle. It holds
   no traffic, so the "Traffic contents" option is removed, and it gains a
   manifest of sections that the Pro replay plugin extends.

The owner settled three more points on 14 November 2026:

10. **What a seat is.** One Pro licence is one seat. A seat is two computers:
    the one you work at and one other that you control or monitor from it. The
    serial device can be on either. Remote control between the two is part of
    the seat.
11. **Enterprise.** It has no fixed shape: what the licence covers, how long it
    runs and the price are negotiated with each customer. The plugins are the
    same as Pro, and it has no feature of its own. The file carries whatever
    is agreed, such as seats, computer names and dates. The
    administrator policy that allows official plugins only is not a licence
    feature and is not gated.
12. **The plugin API.** It is public, documented and free. The official plugins
    built on it, and the agent's internal protocol, are private with no
    compatibility promise.

13. **The "Site licence" label.** The Licence page shows "Site licence" for a
    licence with no seat count, and "<n> seats" otherwise. It is the name of the
    label, not a description of Enterprise: what an Enterprise licence covers,
    how long it runs and its price are still negotiated with each customer.

The owner settled the gaps found in the licence review on 16 November 2026:

14. **Both computers hold the file.** A seat is two computers and each one
    installs the licence file, because each one runs the remote plugin and the
    plugin host starts an official plugin only where a licence covers it.
15. **Plugins check the licence themselves.** Rule 1 of "What a licence check
    can and cannot do" stands. The host reads the licence and, when it starts
    an official plugin, hands the plugin the licence file. The plugin verifies
    the signature, the dates and its own feature with a reader of its own, so
    it never takes the host's word. A third-party plugin may do the same to
    protect itself. Two checks of one licence are fine when each is quick. The
    host's side is built: it reads the licence and answers `licence.get` with
    the licence file. The plugin's side is not, because no official plugin
    exists.
16. **A patched Studio is accepted.** Studio is GPL, so someone can build a copy
    that skips the host's check. That is the limit of a deterrent and the
    design accepts it. The check in the plugin is a second, separate check, and
    nothing is added to stop a changed Studio.
17. **Pairing compares licence ids.** When the two remote plugins connect, each
    sends the id of the licence it verified, and a pair with different ids is
    refused with one plain sentence. This needs no server and counts nothing.
    Not built yet.
18. **Trust the customer.** There is no hardware fingerprint, no MAC address and
    no count of installations. A customer who bought a licence is trusted to
    keep to the terms. A check is meant to tell an honest customer where they
    stand, never to get in the way of one.
19. **No revocation through the update feed.** Licences end with their term, and
    a customer can always block the feed, so the feed does not carry a
    revocation list. The built-in list and a list imported from a file stay as
    they are.
20. **Keys.** The owner keeps a second copy of the private keys offline, with a
    written recovery step.
