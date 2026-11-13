# Screens 01: launcher and basic flows
Mockups in `reference/mockups/`: Main, AddPort, GivePort, Pair, Attach, Watch. Dialog widths: see `tokens.json` `layout.dialogWidths`. Dialog anatomy: title (20/600) and one sentence, body with 16px gaps, footer bar on `sunken` with a top border, buttons right aligned, primary rightmost. Enter triggers the primary action, Escape cancels, initial focus is the first field.

## Launcher (Main.html)
Opens: at start when the studio is empty, and from View, Show launcher. Replaces the dock grid. Content column 960 wide centred, padding 32 top and 40 sides.
- **Readiness strip** (NoteBanner ok): "Ready. This PC can create ports that other programs can open (com0com detected)." with link "View setup check". Warn variant: "Setup needs attention. Other programs cannot see new ports until a driver is installed." with link "Open setup check". Linux and macOS ok variant: "Ready. New ports are pseudo terminals that other programs can open."
- Heading "What do you want to do?" (display). Sub: "Pick a job. Virtual Serial Studio builds it and tells you what to open in your other program."
- Four LauncherCards in a 2 by 2 grid, 16px gaps:
  1. **Give my program a serial port**. "Create a virtual port and get the name to open. Use it when a program needs a COM port and this PC has none." Result line: "Result: a port name, such as COM7". Opens Add a port with Virtual port selected.
  2. **Connect two programs**. "Make a linked pair so one program can talk to another, like a null-modem cable." "Result: two ports, such as COM7 and COM8". Opens Add a port with Linked pair selected (the Pair form).
  3. **Share one device with several programs**. "Open a real serial device once, then give each program its own port." "Result: one port per program". Opens the share wizard, step 1.
  4. **Watch traffic**. "See what flows through a port without changing it." "Result: a live monitor, read only". Opens the Watch dialog.
- **Saved setups row** (card): "Saved setups", most recent file name in mono, "3 ports, 1 link" style summary, buttons Open and Browse. When there is no history: "No saved setups yet" and only Browse.
- Hint line: "Advanced mode adds addresses, fault injection and port ownership to this window. Nothing you have built changes." The words "Advanced mode" are a link that turns it on.
- Status bar left: "No ports yet".
Acceptance: all four cards reachable and activatable by keyboard; the strip reflects the real setup check result; the launcher disappears when the first port exists and returns after File, New.

## Add a port (AddPort.html), width 760
Opens: Ports, Add port (Ctrl+Shift+N), toolbar Add port, launcher cards 1 and 2. Title "Add a port". Sub "What should the new port be?"
- PickCards (radio group): **Virtual port** "A port that other programs can open. Use it when a program needs a COM port and this PC has none." **Linked pair** "Two ports wired together like a null-modem cable, so two programs can talk to each other." **Real device** "Open a serial device that is plugged into this PC, so you can share it."
- Dashed row "More types" with ADVANCED badge. Simple: "Network endpoint and mock fixture. Turn on Advanced mode to add them." (link). Advanced: expands to two more PickCards: **Network endpoint** "A TCP, UDP or RFC 2217 address, used as a port." and **Mock fixture** "A simulated device for testing without hardware."
- Settings section below, titled per selection. Virtual port: Name (default `port-1`, a unique suffix is added automatically), How many (spin box, 1 to 32, default 1), Kind (combo: "Best available (recommended)", "Pseudo terminal", "In-process loopback"). Hint: "Best available uses com0com on Windows, and a pseudo terminal on Linux and macOS. Up to 32 at once. Several ports are named port-1, port-2 and so on."
- ResultSentence (info): "This creates one port named **port-1**. Studio then shows the name to open in your program, such as COM7." Plural form for more than one.
- Primary button text follows the selection: "Create port", "Create pair", or "Next" (Real device continues to Attach). Real device and Linked pair swap the settings section to the fields of those screens below.
Acceptance: unique names enforced silently with a suffix; 33 or 0 in How many is rejected with the inline message "Choose a number from 1 to 32."; after Create the Port ready dialog opens.

## Port ready (GivePort.html), width 760
Title "COM7 is ready" (the real path). Sub "Your port "port-1" now exists, and other programs can open it."
- Path block (sunken, 1px edge): label "Open this port in your program", path in fixed font at 44px, button "Copy name".
- Paragraph: "In your program, choose **COM7** as the serial port. Settings such as baud rate are chosen by that program. The port stays available while Virtual Serial Studio is running."
- Live note: chip "Open" (ok). "Open" is the normal state of a port Studio holds open: Studio cannot see whether another program has opened it, so it says nothing more. Where the platform can report that (`Capabilities::peer_open`), the chip is "Waiting" (warn, hollow) with "No program has opened COM7 yet. In your program, choose exactly COM7. This panel updates when one opens it." and then "Program connected" (ok) with "A program is using COM7."
- Disclosure "Show details" or "Hide details": Name, Kind ("Auto, resolved to com0com"), Path; "Entry in a saved setup (JSON)" CodeBlock `{ "name": "port-1", "kind": "virtual", "virtual_kind": "auto" }`; "Same thing from the command line" CodeBlock `vss_cli virtual --kind=auto`. Collapsed by default in Simple, expanded by default in Advanced, and the user's choice is remembered.
- Footer: "Connect COM7 to something" (quiet, opens Connect ports with this port preselected), "Create another", "Done" (primary).
- Variants. **Linux and macOS**: title is the path, for example `/dev/pts/3`, with the note "This path exists while Virtual Serial Studio is running." **No driver (loopback result)**: title "port-1 is ready inside Studio", warn note "Other programs cannot open this port. Install a driver to create ports they can open." with a button "Open setup check", and no Copy name button.
Acceptance: Copy name copies the exact path and shows the toast "COM7 copied."; the waiting chip updates without closing the dialog (if the engine cannot detect a program, show only "Open" and omit the waiting sentence, see 08-engine-gaps).

## Connect two programs (Pair.html), width 760
Title "Connect two programs". Sub "Create a linked pair of ports. Whatever one program sends, the other receives."
- Diagram row: box "Program A opens" with the first port in large fixed font, a connector labelled "linked both ways, like a null-modem cable", box "Program B opens" with the second port.
- Fields: Pair name (default `pair-1`), First port and Second port (suggested from free COM numbers, editable on Windows; read-only paths on Linux and macOS). Hint: "Names are suggested from free COM numbers. Change them if a program expects a particular name. On Linux and macOS the ports are paths such as /dev/pts/3 and are chosen for you."
- ResultSentence: "Anything **COM7** sends arrives at **COM8**, and the other way round. DTR is wired to DSR and RTS to CTS, as on a standard null-modem cable."
- Windows warn note: "Windows will ask for administrator permission to create this pair."
- Dashed row "More options" (ADVANCED): "Signal wiring and port kind. Turn on Advanced mode to change them." In Advanced it expands to the line wiring preset (Standard null-modem, Loopback, Custom) and port kind.
- Footer: Cancel, Create pair. With no driver installed the primary is disabled and a warn note reads "Linked pairs need a driver. Open the setup check."
Acceptance: names validated for uniqueness with the inline error "COM3 is already used. Try COM9."; elevation failure shows the error pattern in screens/06.

## Attach a real device (Attach.html), width 760
Title "Attach a real device". Sub "Studio holds the real device open. Your programs connect to a new virtual port that Studio creates, and Studio passes the data between the two."
- Device list (radio rows, enumerated when the dialog opens). A click anywhere on an available row selects it, not only on the radio circle. A double-click selects the row and moves focus to Name. Enter runs the primary action. Each row: mono name, description, chip. Rows: "Serial device" with chip Available; "Serial device. Another program is using it." chip "In use", disabled; "com0com pair end. This is a virtual port, not a device." chip "Virtual", disabled. Button Refresh re-enumerates. Hint: "The list is read when this window opens. Plug a device in, then choose Refresh."
- Device settings (collapsible, collapsed in Simple with a one-line summary, open in Advanced): Baud rate (default 115200), Data bits (8), Parity (None), Stop bits (1), Flow control (None, Hardware (RTS and CTS), Software (XON and XOFF)). Button "Use common defaults". Hint: "Most devices use these settings. If you see garbled text, check the device's manual."
- Name (default `device`), checkbox "Capture traffic in the monitor" (default on).
- ResultSentence: "Studio will hold **COM3** open at 115200 baud. Your programs should not open COM3. They open the new virtual port that Studio creates, and Studio passes data between that port and COM3." Why this wording: a first-time user has to learn that the real device is taken by Studio and that their program connects to a different, new port. The Port ready dialog that follows names that port.
- Address CodeBlock `serial://COM3?baud=115200`, button "Edit address" (in Advanced the address becomes editable and wins over the fields; in Simple it is read-only).
- Footer: Cancel, "Attach device".
Acceptance: unavailable devices cannot be selected and the reason is visible as text; failure to open shows the "Could not open COM3" error pattern with Try again and Choose another.

## Watch traffic (Watch.html), width 760
Title "Watch traffic". Sub "See what flows through a port without changing it."
- Port to watch (combo of existing ports, preselected from the current selection). Name of the watcher (default `<port>-monitor`). Hint "A new Studio-only port that receives a read-only copy." Checkbox "Open the monitor when it is ready" (default on).
- Info: "Watching never changes what **device** sees. The watcher is a read-only copy, and it never drives modem lines." Card: chip "Studio only" and "The watcher is shown in the ports list so you can remove it when you are finished."
- Footer: Cancel, "Start watching" (with the watch icon).
- Behaviour: creates a loopback endpoint plus a `monitor` link from the chosen port, then focuses the Monitor filtered to that port.
- Only ports this Studio holds are listed. A port that another program made (a driver port of a `vss_cli` process, say) belongs to that process, so Studio cannot read its traffic (`Capabilities::external_ports` is off). Beside the combo, when other-program ports exist: "Another program made COM41 and COM42. Studio cannot show or watch ports it did not make itself." (`COM41`; `COM41, COM42 and COM43`; `COM41, COM42, COM43 and 2 more`.)
- Empty list: the combo is disabled and reads "No ports available". A note says why, with the action "Create a port", which closes the window and opens Add port. Nothing exists: info note "No ports to watch yet. Create a port first." Other-program ports exist: warn note "Another program made COM41. Studio cannot show or watch ports it did not make itself. Create a port in Studio instead, then watch that."
