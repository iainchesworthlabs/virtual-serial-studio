# Editions, licensing and plugins (proposal)

This page records the decisions reached so far about selling Virtual Serial
Studio, and the ones still open. It is a proposal for the owner and for a
solicitor to review. Nothing here is built, and nothing here is legal advice.

## Who each edition is for

* **Community: the hobbyist and the reverse engineer.** Hooks up their hardware
  to a serial port, watches and records what is on the wire, shares a real
  device between programs, and works out an unknown protocol. This is free and
  stays complete for that job: capture, monitor, timestamps, hex and text views,
  and the Simple and Advanced modes.
* **Pro and Enterprise: the test lab.** Uses Studio as automated test equipment:
  scripted rigs that drive devices and programs, controlled response timing,
  fault injection, repeatable runs and reports, driven from the lab's own test
  harness and language.

## Summary

The base application and the test-signed driver are the Community edition. Pro
and Enterprise add plugins (connectivity, automation, monitoring), the Microsoft-signed driver and support.

| | Community | Pro | Enterprise |
|---|---|---|---|
| Price | Free | Annual licence | Price on application |
| Seats | Unlimited | Per seat: your computer, plus one other computer you monitor | Site licence, not per seat |
| Licence of the code | GPL-3.0-or-later | Commercial terms for the Pro parts | Commercial terms and a support agreement |
| Studio and command line tool | Yes | Yes | Yes |
| Windows driver | Test-signed by Iain Chesworth Labs, built from source | Signed through Microsoft | Signed through Microsoft |
| Official plugins, signed by the owner | No | Yes | Yes |
| Test harness, scripting and remote monitoring plugins | No | Yes | Yes |
| Third-party plugins | Yes, with a warning | Yes, with a warning | Yes, with an optional "official only" policy |
| Support | Community | Support included | Support agreement |

With no plugins, Studio is the Community edition. Going Pro adds plugins, and the
plugins are about connectivity: driving Studio from other tools and watching it
from another computer. A seat is enforced by the licence file and the licence terms,
not by a server, so a lab that wants a hard seat count asks for an Enterprise
licence that can name machines.

## Licences by layer

| Layer | Licence | Notes |
|---|---|---|
| Boost.VirtualSerial (the engine) | GPL-3.0-or-later (decided; it is Boost today) | The value is in the services and plugins, not the engine. Under the GPL anyone who builds on it must publish their source |
| Studio, `vss_cli` and the test-signed driver | GPL-3.0-or-later | The Community edition |
| Microsoft-signed driver, official plugins, licence keys | Commercial | The Pro and Enterprise editions |

All commits in this repository are the owner's, apart from four Dependabot
version bumps, so the owner holds the copyright and can offer the same code
under a second licence. Two points need a solicitor: copyright in code written
with AI assistance, and the GPL-3.0 clause on installation information for
consumer devices as it applies to a signed driver.

"Free for non-commercial use only" is not possible under the GPL, which allows
commercial use. A licence that forbids commercial use is a source-available
licence, not open source. The model above keeps the GPL for Community and sells
a commercial licence and support.

## The signed driver

The driver today is test-signed: a machine needs the owner's certificate
authority installed, and the installer skips the driver step without it. A
driver signed through the Windows Hardware Dev Center (attestation signing)
installs on an ordinary Windows machine. That needs a Microsoft partner account
and an EV code-signing certificate.

The source stays GPL, so anyone can build their own driver, but only the owner
can have it signed under their name. The signed binary is distributed under
commercial terms.

## Licence files

A licence is a file that Studio verifies offline with a public key built into
it. There is no activation server and no telemetry.

| Licence | Term | Notes |
|---|---|---|
| Trial | 14 days | Full Pro features, issued on request |
| Annual | 1 year | Renewed by sending a new file |
| Enterprise | Price on application | Custom term, site licence or a machine binding |

When a licence expires, the Pro features switch off with a visible "licence
expired" notice and a renew link, and Studio falls back to Community. It keeps
every port, topology and file. A grace period of 28 days follows expiry before the plugins switch off. Studio
remembers the latest date it has seen and refuses to go backwards, as a
deterrent only.

## Plugins

Plugins run out of process, on the protocol of the per-user agent
([the agent](agent.md)). A separate process is not part of Studio's process, so
plugin authors choose their own licence, a crashing plugin cannot take Studio
down, and its permissions can be limited. In-process plugins with a C ABI may
follow, behind a signature check.

### Extension points

| Kind | What it does | Fit |
|---|---|---|
| Generators | Produce bytes or traffic patterns onto a port | Very good |
| Fixture types | Simulate a device that answers like a real instrument or modem | Good |
| Behaviour modifiers | Change traffic on a link: corrupt, delay, filter, rewrite | Workable, but inline plugins add latency and need a timeout and a rule for failure. In the first version |
| Automation and test harness adapters | Let other tools drive Studio | Pro, see below |

A plugin declares its extension points, parameters and permissions in a
manifest. Studio draws a plugin's settings from the declared parameters, so
plugin code never runs in the GUI process, and the settings pages keep the
accessible names, keyboard access and themes the GUI rules require. Timing that
must be exact, such as RS-485 turnaround, stays in the engine.

### Trust levels

| Kind | Loads | What the user sees |
|---|---|---|
| Official, signed by the owner | Yes | A Virtual Serial Studio badge |
| Third-party, signed by its author | After the user trusts that author | The author's name and "not verified by Virtual Serial Studio" |
| Third-party, unsigned | After an explicit confirmation | "Unofficial plugin. It can do anything Studio can." |

A trust decision is remembered per plugin and tied to the file's hash, so a
changed plugin asks again. An administrator can set a policy that allows only
official plugins. There is no live revocation without a network, so each release
carries a revocation list and plugins carry an expiry date. Third-party plugins
are not supported by the owner, and the support terms say so.

The signing key for plugins is separate from the driver and release keys, is
never in the repository or in logs, and is used only by a signing job that the
owner approves.

### Automation and test harness plugins (Pro)

All of this is in the first version.

| Plugin | Purpose |
|---|---|
| Python library and pytest plugin | Create ports, link them, inject faults, send and expect bytes, read the event log |
| Bindings for other languages | C and C++ (a stable C ABI), C# and .NET, Java, Node.js; the C ABI is what the others wrap, so a lab can bind its own language |
| Robot Framework library | The same, as keywords for Robot Framework, an open-source, keyword-driven test automation framework (Python-based) that many hardware and QA labs use to write tests as readable tables |
| TestStand and LabVIEW wrappers | For lab test rigs |
| Remote control endpoint | Run Studio from a CI machine or a rig controller; off until the user enables it |
| Reports | JUnit XML and a plain log of a run, for CI systems |

The control endpoint is local only by default (see "Access to the control
endpoint" below). The adapters check the licence file. The agent protocol
itself stays documented and free, so third parties can write their own; Pro is
the maintained, supported, compatibility-tested set.

### Access to the control endpoint

Security is designed in, and each step needs more setup than the last:

1. **Local only (default).** A local socket or loopback, reachable only by
   programs on the same computer and account.
2. **Token.** The user generates a token on the receiving side and gives it to
   the sending side. Tokens can be revoked, and can be limited to certain
   actions.
3. **Administrator control.** Plugins add OAuth 2.0 and OpenID Connect, an
   external identity provider, and the client credentials grant for unattended
   rigs, for sites that manage access centrally.

Remote monitoring (one other computer watching a running Studio) uses the same
endpoint and the same access levels, and is a Pro plugin.

### Controlling responses and timing (Pro)

A test lab needs to decide how a simulated device answers and when. These are
behaviour modifiers and fixtures that a harness can set and change during a run:

* a response delay or jitter, fixed or drawn from a distribution, so a test can
  slow a device down to a real instrument's speed, or past a timeout;
* scripted replies: when this pattern arrives, answer with this, after this
  long, a given number of times, or in this order;
* faults on cue: drop, corrupt, truncate or duplicate a reply, raise a line
  error, drop a control line at a chosen moment;
* a deterministic mode with a seeded random source, so a run can be repeated
  exactly;
* timestamps on everything the harness sees, in one clock.

Delay, jitter and the modem-line and fault tiles already exist in Studio for
manual use. For a harness they must be settable from the API, and they must be
fast enough that the plugin is not the bottleneck. Timing that must be exact,
such as RS-485 turnaround, stays in the engine, so an inline plugin never sits
in a microsecond path. Where a response must be quicker than a round trip to
another process allows, the rule is declared to the engine, which carries it
out itself.

## Studio features to add

* **Support bundle.** A button that saves a file with the version, driver
  state, topology, logs and the list of real serial ports (name, vendor and
  driver). The user saves it and sends it; nothing is sent automatically.
* **Licence page** showing the edition, the term and the expiry.
* **Plugin page** listing installed plugins with their signer and status.
* **Capability flags.** A Pro feature shows as unavailable, with the reason,
  as `docs/gui-design/docs/08-engine-gaps.md` already requires.

## Repositories

| Repository | Visibility | Holds |
|---|---|---|
| `virtual-serial-studio` | Public | Community: Studio, command line tool, test-signed driver, plugin API and protocol |
| `virtual-serial-studio-pro` | Private | Signed driver packaging and its signing pipeline, official plugins, licence verification and issuing tools |
| `virtual-serial` | Public | The engine, to be relicensed to GPL-3.0-or-later |
| `icl-common-infra` | As now | Shared CI and build files |

The Pro repository consumes Community as a dependency and never forks it.

## Website

A static site for virtual-serial-studio.com (the domain is bought), hosted on
GitHub Pages with a CNAME record, so the documents and download links stay current. A shop would need paid hosting, which is
decided when it is needed. The first version has: a landing page, features, downloads
from the GitHub Releases, documents, pricing and support, and a licence page.
It follows the design tokens of the application.

## Decisions made

* Community is the base application plus the test-signed driver, with no plugins.
  Pro adds plugins, the signed driver and support. Enterprise is a site licence.
* Pro is per seat: your computer plus one other you monitor. Enterprise is not per seat.
* The engine moves from Boost to GPL-3.0-or-later.
* All the proposed bindings are wanted: C and C++, Python, C#/.NET, Java,
  Node.js, LabVIEW and TestStand, plus Robot Framework.
* An expired licence returns Studio to Community. The plugins switch off after a
  28 day grace period.
* The control endpoint starts local only, then token, then administrator-controlled access.
* No outside contributions are accepted for now, so no contributor agreement is
  needed yet.
* The licence audit of Qt and the other third-party code goes ahead.
* The site is hosted on GitHub Pages first, with paid hosting for a shop later.

## Open decisions

1. How response timing is declared to the engine, so that the fast cases run
   without a round trip to a plugin.
2. Whether a plugin fixture type appears in the Advanced toolbox beside the
   built-in ones.
3. Whether an expired Annual licence keeps working with the Pro versions
   released during its term.
4. How a seat is counted when there is no server: by trust, by the terms and
   by a machine name in the file.
5. Whether remote monitoring sends a live view or a recording.
6. The result of the licence audit.
7. What the Boost relicensing means for the two published releases of
   `virtual-serial`: anyone who has them keeps the Boost rights to those copies.
