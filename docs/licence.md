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

## Studio is free software

Studio, the command line, the libraries and the Windows driver are under the GNU General Public License, version 3 or later (`GPL-3.0-or-later`). You can read the text in [`LICENSE`](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/LICENSE) on GitHub. The licence lets you use the program for any purpose, including at work, and change it and share it. If you distribute a program that includes this code, the licence's terms apply to that program too. Take your own advice for your case.

A few parts have other licences, each marked where it applies:

- A small reader for licence files, for plugin authors, is under the BSD 2-Clause licence so that a plugin can copy it.
- A few files that came from Boost.VirtualSerial keep the Boost Software License 1.0.
- Qt Canvas Painter, which draws the diagram, is available under the GPL version 3 or a commercial licence from The Qt Company. Studio uses it under the GPL, so a package that contains it is under version 3 of the GPL and not "or any later version".

[Third-party notices](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/THIRD_PARTY_NOTICES.md) lists every component, its version and its licence, and Studio's About window shows them too.

## What Pro adds: a licence file

A Pro or Enterprise licence is one signed file with the extension `.vsslic`. It is not a key that you type. Studio checks it on your computer and never contacts a server, because there is no activation server.

- **Install it.** Choose **Help, Licence, Import licence**, or use the command line. It takes effect at once, with no restart. An administrator can install one for every user of a computer.
- **See what it says.** **Help, Licence** shows the licence number, who it was issued to, its term, how many seats it counts and the computers it names, when it names any. The edition shown in the status bar opens the same page.
- **What it covers.** A licence lists what it turns on, such as official plugins and the driver signed by Microsoft. Neither exists yet, so a licence switches nothing on in Studio today. <span class="vss-chip vss-chip--plan">Planned</span>
- **Keep it safe.** The licence names the person or organisation it was issued to. It stays on your computer and is left out of a support bundle unless you turn that row on.

The guide has [where the licence files go](user-guide/licences.md) on each system.

### What a licence never does

A licence never limits ports, setups, capture or files. With no licence, with a good one, with one that has ended or with one that is broken, your ports, links, setups, captures and saved files work the same.

Studio tells you when a licence is about to end: 30 days before for a normal licence and 3 days before for a trial. An annual or Enterprise licence can have a grace period after its end, and a trial has none. After that Studio is the Community edition and says so once. Nothing you made is lost or locked.

If the date on your computer is more than 48 hours earlier than the latest time Studio has seen, Studio shows "Check the date" until the date is right.

## Boost.VirtualSerial

The engine under Studio's ports is Boost.VirtualSerial, a library from [its own repository](https://github.com/iainchesworthlabs/virtual-serial), under the Boost Software License 1.0. That licence is permissive: it asks you to keep the copyright notice, and it can be combined with GPL code. The program built from both is under the GPL.

## The Windows driver

The driver's source is on GitHub, under `GPL-3.0-or-later` like the rest. The driver in a release is signed with a certificate from the certificate authority of Iain Chesworth Labs, so Windows accepts it only on a computer where that authority is installed. Anyone can build their own driver from the source. A driver signed by Microsoft is <span class="vss-chip vss-chip--plan">Planned</span>, and its licence terms are not decided.

## Pro and Enterprise terms

The terms are not published yet, so this page says nothing about price, seats, trials, support, warranty or liability. [Pricing](pricing.md) says what each edition gives you, and [Support](support.md#talk-to-us) says how to ask.
