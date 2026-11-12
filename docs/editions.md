# Editions, licensing and plugins (proposal)

This page records the decisions reached so far about selling Virtual Serial
Studio, and the ones still open. It is a proposal for the owner and for a
solicitor to review. Nothing here is built, and nothing here is legal advice.

## Summary

| | Community | Pro | Enterprise |
|---|---|---|---|
| Price | Free | Annual licence | Price on application |
| Licence of the code | GPL-3.0-or-later | Commercial terms for the Pro parts | Commercial terms and a support agreement |
| Studio and command line tool | Yes | Yes | Yes |
| Windows driver | Test-signed, built from source | Signed through Microsoft | Signed through Microsoft |
| Official plugins (signed by the owner) | No | Yes | Yes |
| Test harness and scripting plugins | No | Yes | Yes |
| Third-party plugins | Yes, with a warning | Yes, with a warning | Yes, with an optional "official only" policy |
| Support | Community | Support included | Support agreement |

The Pro column is a proposal. Which feature belongs to which edition is the
owner's decision.

## Licences by layer

| Layer | Licence | Notes |
|---|---|---|
| Boost.VirtualSerial (the engine) | Boost Software License 1.0 | Permissive: anyone can build on it, including a closed product |
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
every port, topology and file. A short grace period follows expiry. Studio
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
| Behaviour modifiers | Change traffic on a link: corrupt, delay, filter, rewrite | Workable, but inline plugins add latency and need a timeout and a rule for failure |
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

| Plugin | Purpose |
|---|---|
| pytest plugin and Python library | Create ports, link them, inject faults, send and expect bytes, read the event log |
| Robot Framework library | The same, as keywords |
| TestStand and LabVIEW wrappers | For lab test rigs |
| Remote control endpoint | Run Studio from a CI machine; off until the user enables it |
| JUnit XML report output | For CI systems |

The control endpoint listens on a local socket or loopback with an access
token by default. The adapters check the licence file. The agent protocol
itself stays documented and free, so third parties can write their own; Pro is
the maintained, supported, compatibility-tested set.

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
| `virtual-serial` | Public | The engine |
| `icl-common-infra` | As now | Shared CI and build files |

The Pro repository consumes Community as a dependency and never forks it.

## Website

A static site for virtual-serial-studio.com, hosted from a repository so the
documents and download links stay current: a landing page, features, downloads
from the GitHub Releases, documents, pricing and support, and a licence page.
It follows the design tokens of the application.

## Open decisions

1. Which feature belongs to which edition.
2. Whether the engine stays Boost-licensed, since a competitor can build a
   closed product on it.
3. Whether behaviour modifiers are in the first plugin version, or follow
   generators and fixture types once inline latency is measured.
4. Which languages get a plugin example first (C++ and Python are proposed).
5. Whether a plugin fixture type appears in the Advanced toolbox beside the
   built-in ones.
6. Whether an expired Annual licence keeps working with the Pro versions
   released during its term.
7. Whether the remote control endpoint is in the first version.
8. A contributor agreement before the first outside contribution.
9. The result of the licence audit of Qt and the other third-party code.
10. Hosting for the website.
