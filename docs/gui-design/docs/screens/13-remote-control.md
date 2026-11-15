# Screens 13: remote control

No mockups yet, and nothing here is built. The design behind these screens is `docs/remote-control.md` ("Seeing that it is on"). Dialog anatomy, components and voice are as in screens 01 and `07-copy-deck.md`. Every string below is wrapped in `tr()` with the context named in its section. `<...>` is a value filled at run time. Times use the user's locale and are local time.

This spec covers two things: the chip in the status bar that says whether remote control is on, and the dialog that lists the programs connected and lets the person at the computer disconnect them. It does not cover turning remote control on from Studio, making tokens, or control from another computer; "Not in this spec" at the end says why.

The words: the feature is **remote control**, lower case in a sentence. The program that serves it is the **control gateway** (`vss_gate`), named only where the person has to start or stop it. One connection of one program is a **session**; the dialog says **program** wherever it can, because that is what the person sees. A program is **disconnected**, never "kicked", "killed" or "terminated". A session that receives the bytes of a port is **reading traffic**, never "tapping", "sniffing" or "spying". Remote control is **on** or **off**; it is never "enabled", "active" or "live".

## Where the state comes from
The GUI reads one object, `ControlService`. It is a client of the control gateway's administration endpoint, which is a local pipe (Windows) or socket (Linux, macOS) that only this user can open. It is not a network connection, and Studio opens no listener for it. The service uses the control client library (`vss::control::client`) and names itself "Virtual Serial Studio" in `hello`.

The service does its asking on a thread of its own. Once every two seconds it asks `status` and `sessions.list`, and it passes the answer to the window with a queued connection; an answer that is the same as the last one is not passed on. The window never waits for the gateway: every action below returns at once and its result arrives later. When the gateway does not answer within five seconds the service closes the connection and starts again.

The service states:

| State | When |
|---|---|
| `off` | Nothing answers at this user's administration address. This is the usual state |
| `on` | The gateway answered `hello` and `status` |
| `not_answering` | Something is at the address and it did not answer in time, refused Studio, or speaks a version of the API that this Studio does not |
| `unavailable` | This build has no control client (`capabilities::remote_control` is false; see "Degrading") |

The session of Studio itself is left out of every count and of the list. The transport and the clock of the service are interfaces, so a test gives it a fake of each and every state below has a test. No test starts a gateway for the window.

## Remote control chip (status bar)
Context `RemoteControlChip`. A StatusChip on the right of the status bar, to the left of the update chip and the edition chip. It is always there, in Simple and in Advanced mode, also when remote control is off: no setting hides it. It is a button and opens the Remote control dialog.

| State | Kind | Text | Accessible name |
|---|---|---|---|
| `off` | neutral, hollow dot | "Remote control: off" | "Remote control is off. Open remote control." |
| `on`, no program connected | info, filled dot | "Remote control: local only" | "Remote control is on for programs on this computer. No program is connected. Open remote control." |
| `on`, programs connected (`%n`) | info, filled dot | "Remote control: local and 1 session" / "Remote control: local and <n> sessions" | "Remote control is on. 1 program is connected. Open remote control." / "Remote control is on. <n> programs are connected. Open remote control." |
| `on`, a program is reading traffic (`%n`) | warn, filled dot | "Remote control: 1 session, reading traffic" / "Remote control: <n> sessions, reading traffic" | "Remote control is on. 1 program is connected and traffic is being read. Open remote control." / "Remote control is on. <n> programs are connected and traffic is being read. Open remote control." |
| `not_answering` | warn, hollow dot | "Remote control: not answering" | "The control gateway is not answering. Open remote control." |

The fourth row is used when any session has a traffic stream, however many sessions there are. The kind is shown by the dot's shape and by the words as well as the colour. The colours are the tokens of the chip kinds; the chip has no colour of its own.

The chip's text is not a live region: it changes too often for that. The toast below is what is announced.

Below the compact width the chip shows its dot and "Remote" only; its accessible name and its tooltip stay whole. The tooltip is the accessible name without its last sentence.

## Notification-area icon
Context `TrayIcon`. While the state is `on`, the tooltip gains a second line with the chip's text, and the menu gains **Stop remote control...** above "Stop the ports and quit", which opens the confirmation below over the window. In every other state the tooltip and the menu are as `10-running-in-the-background.md` says.

## Notice when a program connects
Context `RemoteControlNotice`. A toast, not a dialog.

| Case | Text | Action |
|---|---|---|
| The first program connects while none was connected | "<program> connected to remote control." | "Show" |
| More than one connects within the same two seconds (`%n`) | "<n> programs connected to remote control." | "Show" |
| A program starts reading traffic and none was reading | "<program> is reading the traffic of your ports." | "Show" |

"Show" opens the Remote control dialog. `<program>` is the file name of the program as the operating system gives it ("python.exe"), and "A program" when the gateway does not know it. Nothing is shown for a program that connects while others are connected, nor for one that disconnects: a test run connects many times and must not fill the screen. A Studio that is hidden in the notification area shows the same text as one notification, at most one a minute.

## Remote control (width 720)
Context `RemoteControlDialog`. Opens: the chip; Tools, **Remote control...**; "Show" on the toast. The action is in the action registry, so Search actions (Ctrl+K) finds it as "Remote control", with the search words "remote control gateway sessions script automation api". No shortcut key. Title "Remote control". Not modal: the person can leave it open and work.

### Status
The sub line of the dialog, by state. A polite live region.

| State | Text |
|---|---|
| `off` | "Remote control is off. No program can watch or change your ports through it." |
| `on`, no program connected | "Remote control is on for programs on this computer that run as you. No program is connected." |
| `on`, programs connected (`%n`) | "Remote control is on for programs on this computer that run as you. 1 program is connected." / "Remote control is on for programs on this computer that run as you. <n> programs are connected." |
| `not_answering` | "The control gateway is running and does not answer Studio. Stop it with the command below and start it again." |

In the state `off` the body is one group, "Turn it on", with the sentence "Remote control is served by a separate program, the control gateway. Start it from a terminal. It runs until you stop it, and stopping it closes no port." and a CodeBlock with the command `vss_gate` and a CopyButton (accessible name "Command", "Copy command"). Under it a link, "How to control Studio from a script" (accessible name "How to control Studio from a script, opens in your browser"), which opens the how-to page (`docs/control-how-to.md`) at its address on virtual-serial-studio.com. Until the site has that page the link is left out.

In the state `not_answering` the body is the same group with the heading "Stop it" and the command `vss_cli control stop`.

### Programs connected
In the state `on`: a table, one row for each session, the oldest first. The pane header reads "Programs connected" with the count badge.

| Column | Value |
|---|---|
| "Program" | The file name of the program, and under it in the muted style what it called itself, when that is different ("python.exe" / "nightly-rig-test"). "Unknown program" when the gateway knows neither. Shown as plain text whatever it holds, and cut with an ellipsis at 40 characters with the whole text in the tooltip |
| "Connected as" | "You" for a program that runs as this user and brought no token; "Token <id>" for one that brought a token, with the token's id in the fixed font |
| "Through" | "Local" for the pipe or socket; "TCP on this computer" for the loopback address |
| "Since" | The time it connected: "<time>" today, "<date and time>" before today |
| "Requests" | A number |
| "Receiving" | "Nothing", "Events", "Traffic" or "Events and traffic". "Traffic" is shown with the warn chip kind, since it means the bytes of the ports are being read |
| "Lost" | The number of events the session was too slow to take; empty for none. Tooltip "Events this program was too slow to receive." |
| (no heading) | The button "Disconnect". Accessible name "Disconnect <program>" |

The process number is not a column; it is in the row's tooltip ("Process <number>") for a person who has to find the program in the task manager.

Empty state, in place of the table: "No program is connected." and the muted line "A program connects with the Python client, `vss_cli control` or the API. It needs no setting here." with the link "How to control Studio from a script".

"Disconnect" asks nothing: the program can connect again, so nothing is lost that a confirmation would save. The row goes when the gateway says the session is closed, and the toast reads "Disconnected <program>." If the gateway refuses or does not answer, the row stays and the status bar says "Could not disconnect <program>: <reason>". The button is disabled between the press and the answer.

The table is a real table for a screen reader: each row's name is "<program>, connected as <who>, through <transport>, since <time>, receiving <what>". Arrow keys move between rows, Tab reaches the row's button, and Delete on a row presses "Disconnect". Rows keep their place while the list refreshes, and the selection and the focus stay on the same session.

### Footer
"Stop remote control..." (destructive, on the left, only in the state `on`) and "Close" (default; Escape).

"Stop remote control..." opens a confirmation. Title "Stop remote control?". Body, with no program connected: "This stops the control gateway. Your ports keep running. To turn remote control on again, start the gateway." With programs connected (`%n`): "This disconnects 1 program and stops the control gateway. Your ports keep running. To turn remote control on again, start the gateway." / "This disconnects <n> programs and stops the control gateway. Your ports keep running. To turn remote control on again, start the gateway." Buttons "Stop remote control" (destructive, never the default) and Cancel. After it the state is `off` and the toast reads "Remote control is off." If the gateway refuses or does not answer, the dialog stays as it was and the status bar says "Could not stop remote control: <reason>".

## Degrading

| Missing | Then |
|---|---|
| The build has no control client (`capabilities::remote_control` is false) | The chip is not shown, because a chip that said "off" while a gateway ran would be wrong. Tools, Remote control... is disabled with the tooltip "Not available in this build", as `08-engine-gaps.md` says for every missing capability |
| The gateway is older and does not know `sessions.list` | State `on`; the chip reads "Remote control: local only" only when `status` counts no session but Studio's, and otherwise "Remote control: on". The dialog shows, in place of the table, "This control gateway cannot list the programs connected. `vss_cli control sessions` may." |
| The gateway speaks a newer version of the API | `not_answering` |

## Acceptance
Every state of the chip and every row of every table above can be shown from a test with the fake transport. The window's thread never waits for the service: a test with a transport that never answers shows the window still painting and the chip going to `not_answering` after five seconds. The chip cannot be hidden by any setting, mode or width. A program's name, and what it called itself, are shown as plain text whatever they hold (a test gives them markup, a newline and 500 characters). The dialog works with the keyboard only, in both themes and at 200 percent text; every target is at least 24 px; no string is built by joining translated fragments. Studio opens no listener and makes no network request for any of it (the self-test checks that the process has no listening socket with the dialog open).

## Not in this spec
These are in the design and are not specified here, each for a reason that has to be settled first.

| Thing | Why not yet |
|---|---|
| Turning remote control on from Studio | Nothing starts `vss_gate` for a user yet (`docs/agent.md`, PR 6). Until something does, the dialog gives the command |
| Whether programs may only monitor or may also control | The gateway's `status` does not say which role local programs have. It needs a field first |
| The TCP port, when the gateway serves one | `status` does not give the loopback address. It needs a field first |
| A *watched* badge on a port whose bytes a session reads | `sessions.list` says that a session has a traffic stream and not which ports it is for |
| A notice when a program is refused | Refusals are in the audit log, which the API cannot read yet |
| Listing and revoking tokens, and making one | Making a token shows a secret once; that needs its own review. `vss_cli control token` does all three today |
| Control from another computer | Not in this program: it is a Pro plugin, with screens of its own |
