# Screens 09: licence and support

No mockups yet. The design behind these screens is `docs/licensing.md`; the owner has accepted it. Dialog anatomy, components and voice are as in screens 01 and `07-copy-deck.md`. Every string below is wrapped in `tr()` with the context named in its section. `<...>` is a value filled at run time. Dates use the user's locale in its long form ("13 November 2027") and are local time.

The words: **licence** is the noun and **license** the verb, as in the copy deck. The editions are written "Community", "Pro" and "Enterprise". "Official plugins" is the name of what a licence turns on. Never "activate", "register" or "unlock": there is no activation, and the copy must not suggest one.

## Where the licence state comes from
The GUI reads one value, `StudioBackend::licence()`, and one question, `StudioBackend::entitlement(feature)`. `FakeStudioBackend` can return any state, so every variant below has a test. The states and reason codes are those of `docs/licensing.md`, "States". The GUI never reads a licence file itself and never decides a state.

## Menu and entry points
Help menu, after "Copy environment": **Licence...** and **Support...**, then About. Both are in the action registry, so Search actions (Ctrl+K) finds them as "Licence" and "Support". Neither has a shortcut key.

Other ways in: the edition chip in the status bar opens Licence. The notice banners below have a "Licence" action. The About dialog's sub line gains a second sentence, "Edition: <edition>.", and its footer link "Create diagnostic bundle" becomes "Create support bundle" (see "Support bundle" below).

## Edition chip (status bar)
Context `LicenceChip`. A StatusChip at the right of the status bar, a button with the accessible name "Edition: <text>. Open the licence page."

| State | Text | Kind |
|---|---|---|
| none | "Community" | neutral |
| valid | "Pro" or "Enterprise" | ok |
| expiring | "Pro, <n> days left" (`%n`; "Pro, 1 day left"; "Pro, ends today") | warn |
| grace | "Pro, licence ended" | warn |
| expired, not_yet_valid, clock_suspect, invalid | "Community" with the warn dot | warn |

Trial: "Pro trial" and "Pro trial, <n> days left". The chip is text plus dot shape, never colour alone.

## Licence (width 760)
Context `LicenceDialog`. Opens: Help, Licence. Title "Licence". Sub "Shows which edition this is and what your licence includes. Studio checks a licence on this computer and never contacts a server."

### Summary card
A card with the edition in display type, a StatusChip, and a definition list. Rows that have no value are left out, not shown empty.

| Row label | Value |
|---|---|
| "Edition" | "Community", "Pro", "Enterprise", "Pro (trial)" |
| "Licensed to" | `<licensee name>` |
| "Licence number" | `<licence id>` in the fixed font, with a CopyButton (toast "<licence id> copied.") |
| "Term" | "<start date> to <expiry date>" |
| "Status" | The state sentence below |
| "Seats" | "<n> seats" (`%n`; "1 seat"), then on the next line in muted text "A seat is two computers: the one you work at and one other that you control or monitor from it. The serial device can be on either." A licence with no seat count (`seats` is 0) shows "Site licence" and no muted line. "Site licence" is the name of that label only: it is not a promise about what an Enterprise customer gets, which is whatever was negotiated |
| "Computers" | Only when the licence names computers: the names in the fixed font, then "This computer is <computer name>." |
| "Includes" | One line for each: "Official plugins", "Microsoft-signed driver", "Support" |

State chip and sentence:

| State | Chip | Sentence |
|---|---|---|
| none | "Community" neutral | "No licence is installed. This is the Community edition: free software, complete for capturing, monitoring and sharing serial ports." |
| valid | "Valid" ok | "Valid until <expiry date>." |
| expiring | "Ends soon" warn | "Valid until <expiry date>. <n> days left." (`%n`; "Ends today.") |
| grace | "Ended" warn | "Ended on <expiry date>. Official plugins keep working until <grace end date>, then switch off." |
| expired | "Ended" warn | "Ended on <expiry date>. Studio is running as the Community edition. Your ports, setups and files are unchanged." |
| not_yet_valid | "Not started" neutral, hollow | "Starts on <start date>." |
| clock_suspect | "Check the date" warn | "The date on this computer looks wrong. Official plugins are off until the date is right. If the date is right, create a support bundle and contact support." |
| invalid, bad signature or unreadable | "Not valid" danger | "This file is not a valid Virtual Serial Studio licence. It may be damaged or incomplete. Ask for the file again." |
| invalid, unknown key or format | "Not valid" danger | "This licence needs a newer version of Studio. Install the latest version, then import the licence again." |
| invalid, revoked | "Not valid" danger | "This licence was withdrawn. Contact support." |
| invalid, wrong computer | "Not valid" danger | "This licence is for other computers. This computer is <computer name>. Ask your administrator for a licence that names it." |

A NoteBanner under the card for the states that need an action:
- expiring (warn): "Your licence ends on <expiry date>. Renew to keep official plugins and support." Action "How to renew".
- grace (warn): "Your licence ended on <expiry date>. Official plugins switch off on <grace end date>." Action "How to renew".
- expired (info): "Official plugins are off. Nothing you made is lost: every port, setup and file still opens and saves." Action "How to renew".
- trial, valid (info): "This is a 14 day trial of the Pro edition. It ends on <expiry date>. Nothing you make during the trial is lost."

"How to renew" opens a small dialog (width 640), title "Renew your licence", text "A licence is renewed by installing a new licence file. Write to <support address> and quote your licence number, <licence id>. Studio does not open a web page or send anything itself." Buttons "Copy licence number", "Copy address", primary Close. The address comes from one function, like `com0com_project_url`, so there is one place to change it.

### What is unavailable, and why
A section headed "What this edition includes", a table with columns "Feature", "Status" and "Why". One row for each feature the licence service knows: "Official plugins", "Microsoft-signed driver", "Support", and one row for each installed official plugin by its name. Status is a chip, "Available" (ok) or "Not available" (neutral, hollow). The Why column holds the reason sentence, and it is the same sentence the feature's own disabled control shows:

| Reason code | Sentence |
|---|---|
| no_licence | "Needs a Pro licence." |
| not_in_licence | "Your licence does not include this." |
| expired | "Your licence ended on <expiry date>." |
| not_yet_valid | "Your licence starts on <start date>." |
| clock_suspect | "The date on this computer looks wrong." |
| invalid | "The installed licence is not valid." |
| wrong_computer | "Your licence is for other computers." |

Rule for every Pro control elsewhere in the window, following `08-engine-gaps.md`: it is shown, disabled, with the reason sentence as its tooltip and in its accessible description, and where there is room a link "Licence" beside it. A Pro feature is never hidden without a reason and never looks available when it is not. This is separate from "Not available in this build", which stays the text for a missing engine capability; if both apply, the build message wins.

### Other licences
Only when more than one file is installed: a disclosure "Other licences on this computer (<n>)" with one row for each: licence number, edition, "<start date> to <expiry date>", its state chip, and a quiet button "Remove". Muted line: "Studio uses the licence that gives you the most."

A licence from the per-machine directory shows the chip "Installed by an administrator" and has no Remove button.

### Footer
Quiet "Remove licence" (only when a per-user licence is in use), "Import licence..." (primary when the state is none, expired or invalid; otherwise a normal button), Close (primary otherwise).

"Remove licence" asks first: title "Remove this licence?", text "Studio returns to the Community edition. Your ports, setups and files are unchanged. Keep the licence file so you can import it again." Buttons Cancel (default), "Remove licence" (danger).

Acceptance: every state in the tables can be shown from `FakeStudioBackend`; the status sentence is a polite live region and updates when a licence is imported without closing the dialog; dates follow the locale; the dialog works at 200 percent text with the table scrolling inside the dialog; no string is built by joining translated fragments.

## Import a licence
Context `LicenceImport`. "Import licence..." opens the native file dialog, title "Import a licence", filter "Licence files (*.vsslic)" and "All files (*)". A `.vsslic` file dropped on the Licence dialog does the same. Studio checks the file, and only a file whose signature verifies is copied to the user's licence folder. The original is left where it was.

Results:
- Valid: toast "Licence imported. This is now the <edition> edition." The page updates in place. No restart.
- In grace or expired, signature good: the file is imported, and a warn NoteBanner in the dialog says "This licence ended on <expiry date>. It was imported, but it does not turn anything on."
- Not yet valid: imported, info NoteBanner "This licence starts on <start date>."
- Same licence already installed: toast "That licence is already installed."
- Not valid: nothing is copied. Error pattern, title "Could not import the licence", body the sentence for the reason from the state table above, buttons "Copy details" (quiet), "Choose another file", Close (primary). Copy details gives the file name, the reason code and the library's message, never the file's contents.
- File cannot be read: title "Could not import the licence", body "Studio could not read <file name>. Check that the file still exists and that you are allowed to open it." Same buttons.

Acceptance: an invalid file never replaces a valid licence; importing works with the keyboard only; the result is announced.

## Licence notices (main window)
Context `LicenceNotice`. A NoteBanner at the top of the window, in the same place as the "Recreate ports" banner and below it when both show. One at a time.

| State | Kind | Text | Actions |
|---|---|---|---|
| expiring, 30 days or fewer (trial: 3) | info | "Your licence ends on <expiry date>." | "Licence", "Dismiss" |
| grace | warn | "Your licence ended on <expiry date>. Official plugins switch off in <n> days." (`%n`; "switch off tomorrow"; "switch off today") | "Licence", "Remind me tomorrow" |
| expired, first start after the grace period | info | "Your licence has ended, so official plugins are off. Studio is now the Community edition. Your ports, setups and files are unchanged." | "Licence", "Dismiss" |
| clock_suspect | warn | "The date on this computer looks wrong, so official plugins are off." | "Licence", "Dismiss" |

"Dismiss" on the expiring notice hides it until 7 days before the end, then until the last day. The expired notice is shown once. No notice is ever a modal dialog, steals focus or blocks work. Banners are announced politely, once.

A topology that uses an official plugin while the plugin is unavailable: the plugin's items stay in the setup, drawn in the disabled style with the chip "Not available" and the reason sentence. Info banner when the file opens: "This setup uses official plugins that are not available: <names>. Those parts are kept in the file but do not run." Action "Licence". Saving keeps them unchanged.

## Support (width 760)
Context `SupportDialog`. Opens: Help, Support. Title "Support". Sub "Where to get help, and what to send so a helper can understand your setup."

- **Card "Your support"**, by state:
  - Licence includes support: chip "Support included" (ok). "Your <edition> licence includes support until <expiry date>. Write to <support address> and quote your licence number, <licence id>." Buttons "Copy address", "Copy licence number".
  - Community, or a licence without support: chip "Community support" (neutral). "The Community edition is supported by its users. Search the documents and the issue list first, then open an issue. Support from the developer comes with a Pro or Enterprise licence." Buttons "Copy documents address", "Copy issues address".
  - Licence ended: chip "Support ended" (warn). "Support ended with your licence on <expiry date>." Button "How to renew".
  Studio copies addresses to the clipboard and does not open a browser or a mail program, in line with "nothing is sent". Muted line under the card: "Studio never sends anything itself. You choose what to share."
- **Card "Before you write"**: three numbered lines. "Say what you expected and what happened instead." "Say which port or program it happened with." "Attach a support bundle. It answers most first questions."
- **Card "Support bundle"**: "A support bundle is one zip file with the version, the driver state, your setup, the log and the list of serial ports on this computer. You see everything in it before you save it." Button "Create support bundle" (primary of the dialog). Opens the dialog below.
- **Card "This copy of Studio"**: rows "Version", "Edition", "Licence number" (when there is one), "Operating system", in the fixed font. Button "Copy" (toast "Details copied."). These are the lines a helper asks for first.

Footer: links "Help contents", "Why is nothing arriving?"; primary Close.

Acceptance: all three card variants reachable from `FakeStudioBackend`; no network call, no browser, no mail program; every button has a 24 px target and an accessible name that says what is copied.

## Support bundle (width 860)
Context `SupportBundleDialog`. This is the existing diagnostic bundle dialog (screens 06, `support/bundle.cpp`, `support/redact.cpp`, `StoredZipWriter`), extended and renamed. There is one bundle, not two. "diagnostic bundle" becomes "support bundle" in every string: the dialog, the Troubleshoot footer and muted line, the About footer link and the File menu item. The file name becomes `vss-support-<date>.zip`. `capabilities::diagnostics` still gates it.

Title "Create a support bundle". Sub "Gathers what a helper needs to understand your setup. Nothing is sent anywhere. You choose where to save it."

Two columns. Left, the checklist "Include"; right, the preview.

| Row | File | Default | Holds |
|---|---|---|---|
| "Version and environment" | `environment.txt` | on | The Environment tab of About, plus the edition |
| "Driver state" | `driver.txt` | on | Whether the Studio driver and com0com are present, their versions, how the driver is signed, the helper service's state, the setup check's results |
| "Setup" | `setup.json` | on | The topology as it would be saved |
| "Event log" | `log.txt` | on | The Log tab |
| "Port counters" | `counters.csv` | on | As today |
| "Serial ports on this computer" | `serial-ports.txt` | on | One line for each real serial port: name, description, vendor and product ids, and driver. See below |
| "Licence" | `licence.txt` | on when a licence is installed; the row is absent otherwise | Licence number, edition, term and state. Not the licence file |
| "Licensee name" | in `licence.txt` | off | Amber row: "The name your licence was issued to. Turn this on if you are writing to support about the licence itself." |

What is always left out or changed, stated in a muted block under the checklist with the heading "Always left out":
"The bytes your ports sent and received are never included. Passwords, tokens and keys are replaced with [redacted]. The path of your home folder is shortened to ~. The name of this computer and your user name are not included. Serial numbers of devices are not included. The licence file itself is never included."

The computer name is needed when the question is about a licence that names computers. Then one more off-by-default amber row appears, only when the installed licence names computers: "This computer's name", "Needed when a licence names computers."

**No traffic.** The support bundle never holds payload bytes. The off-by-default "Traffic contents" row of the old diagnostic bundle is removed, with `render_traffic` and `traffic.txt`. Recording traffic and packing it for replay belongs to the Pro plugin "Diagnostics and replay" (the plugin framework's design), which extends this bundle's format as described under "Bundle format" below.

**Serial ports.** The list comes from `StudioBackend::devices()`: path, description and hardware id (vendor and product). Virtual devices are listed and marked "virtual". The serial number part of a hardware id is removed. The driver name is not something the backend reports today, so the column reads "not known" until a platform probe supplies it (a new `capabilities` flag, `device_driver_names`, false in every backend for now). Never a guessed driver.

**Preview.** The right column is headed "Preview". A list of the files that will be written, each with its size ("1.2 kB"); selecting one shows its exact contents, after redaction, in a read-only fixed-font view. The first file is selected when the dialog opens. Unticking a row removes its file from the list at once. Above the view: "This is exactly what the file will contain." A file larger than 256 kB shows its first 256 kB and the line "Preview shortened. The saved file is complete." The preview is built by the same function that writes the zip, so the two cannot differ.

Info NoteBanner at the bottom of the left column: "Nothing leaves this computer until you share the file yourself."

Footer: left, the file name `vss-support-<date>.zip` in the fixed font and "<n> files" (`%n`). Buttons "Copy environment only" (quiet), Cancel, "Save bundle" (primary; opens the native save dialog). After saving: toast "Saved <file name>." with the action "Show in folder".

**Bundle format.** One zip, written by `StoredZipWriter`, built so that a plugin can add to it without a second format.
- `manifest.json` at the root, written first: `{ "bundle_format": 1, "created": "<UTC time>", "studio_version": "<version>", "sections": [ ... ] }`. Each section is `{ "id": "environment", "title": "Version and environment", "files": ["environment.txt"], "sensitive": false, "added_by": "studio" }`. The ids of the sections above are `environment`, `driver`, `setup`, `log`, `counters`, `serial-ports` and `licence`.
- A section is the unit of consent: one row in the checklist, one group in the preview, one entry in the manifest. A section the user left out has no entry and no files. A reader ignores sections it does not know.
- Studio's own files stay at the root, as today. A plugin's section puts its files in a folder named after its section id. The id `traffic` and the folder `traffic/` are reserved for the Diagnostics and replay plugin and Studio never writes them.
- A section a plugin adds appears in the same checklist and the same preview. It is marked `"sensitive": true`, drawn as an amber row, off by default, and it names the plugin ("Added by <plugin name>"). Nothing reaches the zip without its row ticked and its preview available.
- `bundle_format` rises only for a change an old reader cannot ignore.

Failure: title "Could not save the bundle", body "Studio could not write to that folder. Choose another place, such as your Documents folder." Buttons "Copy details" (quiet), "Choose another place" (primary), Cancel.

Acceptance: with every amber row off, a test proves the bundle holds no computer name, payload bytes, user name, home path, device serial number, licensee name or licence file, using a backend seeded with marker values; the preview text equals the bytes written for every file; `manifest.json` lists exactly the sections that were ticked; the zip is written only where the user chose; the dialog makes no network call; the checklist, file list and preview are reachable in that order by keyboard, and the preview view has the accessible name "Preview of <file name>".

## Accessibility notes for all of the above
Chips carry text and dot shape. State changes (import result, a banner appearing) are polite live regions and are announced once. Every dialog sets initial focus on its safest control: Close on Licence and Support, the checklist on the bundle, Cancel on "Remove this licence?". All work in both themes from tokens only, with icons from `IconProvider`.
