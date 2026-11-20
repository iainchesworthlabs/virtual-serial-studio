---
# The licensing page of the product site. It is docs/licence.md because docs/licensing.md is the design of licence files, which stays on GitHub.
template: marketing.html
layout: prose
title: Licensing
title_tag: "Licensing: Virtual Serial Studio"
description: "Virtual Serial Studio is free software under the GPL, version 3 or later. How a licence file works for Pro, the Boost.VirtualSerial dependency and the driver."
---
<!-- OWNER DECISION: the terms of Pro and Enterprise: the licence terms themselves, the definition of a seat, the trial terms, the grace period, the version window (updates_until), the warranty and liability wording, and what the commercial terms say about the Windows driver signed by Microsoft. None of it is decided in the repository, and this page states none of it. docs/licensing.md and docs/editions.md are design pages, and they describe more than is built. -->
<!-- OWNER DECISION: the text of this page needs a solicitor. docs/editions.md names two points: who holds the copyright in parts of the code (docs/editions.md says which), and the GPL-3.0 clause on installation information as it applies to a signed driver. -->

# Licensing

<div class="icl-callout"><strong>DRAFT</strong><p>DRAFT, not legal advice, to be reviewed before it is relied on. It says what the repository shows. The terms of Pro and Enterprise are not written yet.</p></div>

## The program is free software

Studio, the command line, the libraries and the Windows driver are under the GNU General Public License, version 3 or later (`GPL-3.0-or-later`). The text is in [`LICENSE`](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/LICENSE) in the repository. Every first-party source file has a line that names the licence, and `REUSE.toml` covers the rest. The GPL lets you use the program for any purpose, including at work, and to change it and share it. If you distribute a program that includes this code, the licence's terms apply to that program too. Take your own advice for your case.

There are a few exceptions, each marked where it applies:

- `sdk/licence-reader`, a small reader for licence files, is under the BSD 2-Clause licence so that a plugin can copy it.
- A few files that came from the Boost.VirtualSerial repository keep the Boost Software License 1.0. `THIRD_PARTY_NOTICES.md` lists them.
- Qt Canvas Painter, which draws the diagram, is available under the GPL version 3 or a commercial licence from The Qt Company, and Studio uses it under the GPL. A package that contains it is conveyed under version 3 of the GPL, not "or any later version".

[Third-party notices](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/THIRD_PARTY_NOTICES.md) lists every component, its version and its licence, and the About window shows them too.

## What Pro adds: a licence file

A Pro or Enterprise licence is one signed file with the extension `.vsslic`. It is not a key you type. Studio checks it on the computer, and never contacts a server. There is no activation server.

- **How it is checked.** The file carries an Ed25519 signature. Studio checks it against public keys that are built into the program, and refuses a file that does not verify. A build with no keys accepts no licence and is the Community edition.
- **How you install it.** Choose **Help, Licence, Import licence**, or run `vss_cli licence import <file>`. Studio copies a file with a good signature into your licence folder, and takes effect at once with no restart. An administrator can put a file in a per-machine folder for every user of the computer.
- **What it says.** The licence number, who it was issued to, its term, how many seats it counts (a licence with no seat count is a site licence), and the computers it names, when it names any. **Help, Licence** shows these, and the edition chip in the status bar opens the same page.
- **What it covers.** A licence lists the features it turns on. The names are `plugins.official`, `plugins.<id>` for one official plugin, `driver.signed` and `support`. Official plugins and the driver signed by Microsoft are not built, so no feature of Studio is switched on by a licence today. <span class="vss-chip vss-chip--plan">Planned</span>
- **Who reads it.** Studio, the control gateway and the plugin host read the licence file on the computer. The plugin host hands it to a plugin when the plugin asks, always to an official plugin and to another only with the `licence.read` permission. An official plugin, when one exists, is meant to check it for itself.

### What a licence never does

A licence never limits ports, setups, capture or files. With no licence, with a valid one, with one that has ended or with one that is broken, ports, links, setups, captures and saved files work the same.

When a licence is about to end, Studio says so in the last 30 days of the term (3 days for a trial). An annual or Enterprise licence can have a grace period after its end, and a trial has none. After that Studio is the Community edition and says so once. Nothing you made is lost or locked.

If the date on the computer is more than 48 hours before the latest time Studio has seen, the state is "Check the date" until the date is right.

### Where the files go

| System | Your licence folder | The per-machine folder |
| --- | --- | --- |
| Windows | `%LOCALAPPDATA%\Virtual Serial Studio\licences` | `%ProgramData%\Virtual Serial Studio\licences` |
| Linux | `$XDG_DATA_HOME/virtual-serial-studio/licences`, or `~/.local/share/virtual-serial-studio/licences` | `/etc/virtual-serial-studio/licences` |
| macOS | `~/Library/Application Support/Virtual Serial Studio/licences` | `/Library/Application Support/Virtual Serial Studio/licences` |

Studio reads every `.vsslic` file in both folders and uses the one that gives the most. It only reads the per-machine folder.

## Boost.VirtualSerial

The engine under Studio's ports is Boost.VirtualSerial, a header-only library from [its own repository](https://github.com/iainchesworthlabs/virtual-serial), under the Boost Software License 1.0. CMake fetches a release of it when you build Studio. The Boost licence is a permissive licence that asks you to keep the copyright notice, and it can be combined with GPL code. The program built from both is conveyed under the GPL.

## The Windows driver

The driver's source is in the repository, under `GPL-3.0-or-later` like the rest. The driver in a release is signed with a certificate from the certificate authority of Iain Chesworth Labs, so Windows accepts it only on a computer where that authority is installed. Anyone can build their own driver from the source. A driver signed by Microsoft is <span class="vss-chip vss-chip--plan">Planned</span>, and its licence terms are not decided.

## Pro and Enterprise terms

The terms are not published yet, so this page says nothing about price, seats, trials, support, warranty or liability. [Pricing](pricing.md) says what each edition holds, and [Support](support.md#talk-to-us) says how to ask.
