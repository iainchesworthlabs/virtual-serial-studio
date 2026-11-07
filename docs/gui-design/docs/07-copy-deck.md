# 07 Copy deck and voice

Screen-specific strings are in `docs/screens/`. Take them exactly. This page holds the rules and the shared vocabulary.

## Voice
- Plain, short, active. Say what happened, why, and what to do. Sentence case everywhere. No exclamation marks, no jokes, no blame.
- Use Australian English spelling: colour, licence (noun), organise, behaviour.
- No em dashes. Use commas, colons or full stops.
- Name things the user sees: port, program, device, pair, link. Avoid endpoint, URI, link_mode, hub, transport in primary text. They may appear in Details areas that are clearly labelled.
- Port names, paths and addresses are shown in the fixed font and are bold inside result sentences.
- Buttons are verbs or short phrases: Create port, Attach device, Share device, Start bridge, Try again. Never OK or Yes. Cancel always means leave everything unchanged.

## Shared vocabulary
| Term | Meaning |
|---|---|
| Port | Anything a program can open or Studio can route: virtual, real, network, fixture. |
| Program | An application that opens a port. |
| Device | A real serial device on this PC. Studio holds it open; programs never open it. |
| Virtual port | The port a program opens in place of the device. Studio creates it and passes the data between it and the device. |
| Setup | Everything built together: a linked pair, a shared device with its program ports, or an attached device with its virtual port. |
| Linked pair | Two virtual ports wired like a null-modem cable. |
| Link | A connection between ports: two-way, one-way, monitor, split, join, shared bus. |
| Studio only | An in-process port that other programs cannot open. |
| Fixture | A mock device for testing. |
| Sending from | The port a send goes out of. For a fixture the bytes arrive on it, as if the device had sent them, and travel along its links. |
| Advanced mode | The same window with more tools. Not a separate edition. |

## State words
Open, Studio only, Closed, Failed, Fixture, Setup ready, Needs attention, Passed, Not checked yet. Direction badges: RX, TX, DROP. Switch labels include the state: "DTR on", "RTS off".

## Error template
Title: "Could not <verb> <thing>" (for example "Could not open COM3"). Body: one sentence of likely cause, one sentence of what to do. Buttons: the fix, an alternative, "Copy details". Engine text lives only behind Copy details.

## Implementation rules
- Wrap every string in `tr()` with a per-screen context. No concatenating translated fragments: use placeholders (`%1`) and `%n` plurals. Bold names inside sentences are applied with rich text, but the accessible text must still read as a plain sentence.
- Keep each screen's strings in one place (a `strings` header or a single translation unit per screen) so a copy review is a single diff.
- Dates and numbers use the user's locale. Byte sizes read "1.2 kB", "10.0 MB".
- Placeholders like `<version>` and `<date>` in the specs are values filled at runtime, never literal text.

## Explaining the device and its port
First-time users do not know that an attached or shared device is taken over by Studio. Say it in these words, everywhere the idea appears:
- Attach, subtitle: "Studio holds the real device open. Your programs connect to a new virtual port that Studio creates, and Studio passes the data between the two."
- Attach, result: "Studio will hold **COM3** open at 115200 baud. Your programs should not open COM3. They open the new virtual port that Studio creates, and Studio passes data between that port and COM3."
- Share, step 1 and 2: "Your programs will open the ports Studio creates, not **COM3** itself." and "You can add another program after sharing, and remove any program's port later with Remove port."
- Never write "I open this port" or any first-person wording that leaves out who opens what.

## Removing
- Setup: title "Remove this setup?", body "This closes <n> ports and <m> links: <names>. A program that has one of these ports open loses it. Nothing is saved until you save the setup.", buttons "Remove setup" and "Cancel".
- Port in a setup: title "Remove <port>?", body "<port> is part of a setup with <n> other ports: <names>. Remove only <port> and its links, or close the whole setup. Nothing is saved until you save the setup.", buttons "Cancel", "Remove only <port>", "Remove the whole setup".
- Program port of a share: the body says "<port> is one of <n> programs sharing a device, in a setup with ..." and "the others keep sharing".
- Port with no links: title "Remove <port>?", body "Remove <port> and its links? Nothing is saved until you save the setup.", buttons "Remove port" and "Cancel".
- Status bar after a setup: "Removed the setup (2 ports and 1 link)."

## Simple mode words
Simple mode asks about the user's things only: their device and their programs. It never names a part of Studio
(terminal, monitor, fixture, loopback, link, endpoint, topology) as something to choose or connect. Typing to a device is
"Let me type to COM3 from this window"; seeing traffic is "Show me the data going through"; an echo is "Send everything
straight back". Every choice has one sentence on what it does and when it helps. The program's name is optional and is a
label only: "Program, e.g. PuTTY (optional)". Full strings: `screens/08-simple-mode.md`.

## Asking for administrator rights (Windows)
Studio starts without administrator rights and never asks for them at start-up. Two steps ask, and only after the user chose them: Restart as administrator (Setup check) and Allow in Windows Firewall. Each says why in the window, before the Windows prompt appears, and tells the user what to look for in the prompt:
- Setup check, Administrator rights row: "Creating a new linked pair needs administrator rights, once per pair. Everything else runs without them." then, from the account's state, "Your account is an administrator, so Windows will ask you to confirm." or "Your account is not an administrator, so Windows will ask for an administrator's password.", then "The Windows prompt names the program and its publisher: choose Yes if they match. Studio then restarts and starts the same ports."
- Firewall dialog, above the command: "Changing the firewall needs administrator rights, so Windows will show a permission prompt. It names the program and its publisher: choose Yes if they match. Studio itself keeps running without administrator rights."
- Declined: "You declined the Windows permission prompt, so nothing was changed."
The prompt names the signer of the program that asks (`vss_gui.exe` for the restart, `vss_cli.exe` for the firewall rule). A build that is not signed with a certificate the machine trusts shows "Unknown publisher", and the owner's docs (`docs/windows-com-driver.md`, Signing) say why.

## When a port cannot be made (Windows)
A refused port never shows the engine's code ("permission denied", "unsupported", "I/O error") as the message. Studio asks which piece of the driver setup is wrong (`StudioBackend::check_driver`, which sends the helper service one request that changes nothing) and says that piece. The dialog keeps the title of the template ("Could not create the port"); the body is one of these, each ending with the next step; the engine code and what the check saw are behind Copy details only. Where Setup can fix the problem the dialog, banner or error also carries the action "Set up Virtual Serial Studio".
- The driver is not installed: "Windows needs the Virtual Serial Studio driver to make a COM port that other programs can open, and this PC does not have it yet. Nothing was changed. Choose Set up Virtual Serial Studio to install it."
- The helper service is not installed: "The helper service that makes ports without asking for administrator rights each time is not installed. Nothing was changed. Choose Set up Virtual Serial Studio to install it."
- The helper service is not running: "The helper service that makes ports is installed but not running. Nothing was changed. Choose Set up Virtual Serial Studio to start it again."
- The helper service is older than this program: "The helper service on this PC is older than this program, so it cannot make ports. Nothing was changed. Choose Set up Virtual Serial Studio to update it."
- The helper service refuses this user: "Windows did not let your account use the helper service that makes ports. Nothing was changed. Choose Set up Virtual Serial Studio to repair it, or ask an administrator of this PC."
- Anything else, or a check that could not tell: "Windows would not make the port, and Studio could not tell which part is at fault. Nothing was changed. Open Tools, Setup check to see what is missing, then copy the details if you ask for help." No Set up action; the Simple mode form offers Copy details instead.
Short headings, for the Set up dialog and the Setup check: "The Virtual Serial Studio driver is not installed", "The Virtual Serial Studio helper is not installed", "The Virtual Serial Studio helper is not running", "The Virtual Serial Studio helper is out of date", "Your account cannot use the Virtual Serial Studio helper", "Windows would not make the port".

## Set up Virtual Serial Studio (Windows)
One guided action installs or repairs the driver and the helper service. It is offered on the first screen (the Launcher banner and the Simple mode banner), as the action of the Setup check's driver row, and in the error of a port that could not be made, whenever the check finds the driver missing, the helper not installed, not running, too old, or refusing the user. Its dialog is the user's agreement and changes nothing until the button is pressed:
- Title "Set up Virtual Serial Studio"; subtitle the short heading above, or "Install or repair the driver and helper this PC needs." when nothing is wrong yet.
- "Windows only lets a driver make COM ports that other programs can open. Setup installs the Virtual Serial Studio driver and a small helper service that makes ports for you, so administrator rights are needed once, now, and not each time you add a port."
- "Windows will show one permission prompt. It names the program and its publisher: choose Yes if they match. Studio itself keeps running without administrator rights. Nothing is downloaded: the files come from this copy of Virtual Serial Studio."
- "The helper restarts, so ports that are open now are closed. Studio offers to make them again."
- "Files: <folder>", then "The same setup as a command, if you prefer to run it yourself in a terminal opened as administrator:" and the command. Without the driver files: "The driver files are not with this copy of Virtual Serial Studio, so it cannot set up for you. Install Virtual Serial Studio from its installer, or follow the steps for a build you made yourself." and the button is off.
- Buttons: Copy details, Set up Virtual Serial Studio, Close. While it runs the window answers, Close is off, and the note reads "Setting up. Answer the Windows permission prompt, then wait: this can take a minute."
- Results: "Virtual Serial Studio is set up. The driver and the helper service are installed and running." Studio asks again afterwards, and if the answer is still wrong says "Setup finished, but this PC still cannot make ports: <heading>. Copy the details if you ask for help." Others: restart needed ("Setup finished, but Windows needs a restart before the driver can be used. Restart Windows, then open Virtual Serial Studio again."), declined ("You declined the Windows permission prompt, so nothing was changed."), an unsigned or untrusted build ("Windows does not trust the signature on the driver files, so nothing was installed. Released builds are signed and install without this step. For a build you made yourself, trust its test certificate or turn on test signing first (the Windows driver notes say how), then try again."), the driver or the helper failing, missing files, no administrator rights, and "Setup did not finish. Copy the details, or run the command shown in a terminal opened as administrator to see what happened."
