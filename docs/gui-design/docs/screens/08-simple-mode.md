# Screens 08: Simple mode

Mockup: `reference/mockups/SimpleGoals.html`. Visual language: `10-design-language.md`. This screen replaces the
launcher and the dock grid in Simple mode. Advanced mode is the canvas editor of `07-canvas-editor.md`, with the Ports, Inspector,
Send and Monitor panes. Both modes show the same studio, and switching never changes anything already built.

## Why

A first-time user knows two things: the device they have and the program they want to use. Simple mode asks only
about those, in a guided form per job, and then shows one page per setup that says which port to open in which program.
It never asks the user to wire up a part of Studio. Typing to a device and seeing the data are options with one sentence
each on what they do, not things to connect to.

## Layout

The body (between the menu bar, toolbar and status bar, which are unchanged) is two panes on the ground:

- **Your setups** (left, 300 wide, minimum 240). Pane header "Your setups" with a count and a ghost button "New" (icon
  `add`, accessible name "Set up something new"). One row per setup: title (body 600), then the ends it holds, one per
  line in the mono font, each with a dot: filled `ok` when a program has the port open, hollow `amber` when none has,
  none for a device. A setup made in Advanced mode that no job describes carries a neutral chip "Advanced". Selecting a
  row opens its page. Delete on a selected row asks to remove that setup. Empty: "Nothing set up yet. Choose what you
  want to do on the right." Footer row: "Save…", "Open…", and on the right "Remove all" (danger).
- **The page** (right, flexible). One of: the job grid, a job form, a setup page.

A **setup** is what was built together: the ports joined by links, directly or through other ports, or one port with no
links (`shell/setup_group`). Its kind is read back from the ports and links, so a setup made in Advanced mode or loaded
from a file is shown the same way.

## Job grid

Heading "What do you want to do?" (display). Sub: "Choose a job. Studio sets it up and tells you which port to open in
your program. Everything you set up is listed on the left, where you can change or remove it."

Readiness note under it, as on the old launcher: ok "This PC can make ports that other programs can open." with the link
"Details" (opens the setup check), or warn "Other programs cannot see new ports until a driver is installed." with
"Open setup check".

Jobs, as tiles (kind tile icon, title, two-line description) in four groups with group labels:

| Group | Job | Description |
|---|---|---|
| My program needs a serial port | **Give my program a serial port** | For a program that needs a COM port when there is no device, or before the device arrives. |
| | **Connect two programs to each other** | Like a null-modem cable between two programs on this PC. What one sends, the other receives. |
| I have a serial device | **Use my device through a new port** | Your program opens a new port instead of the device. Then you can watch, share or test without changing the program. |
| | **Share my device with several programs** | A device can normally be open in one program at a time. Here each program gets its own port, and all of them talk to the device. |
| | **Try my device from this window** | Type to the device and read its replies here, without another program. Use it to check that the device works. |
| | **Let other computers use my device** | Other computers connect to this PC over the network and talk to the device. |
| Several programs or devices at once | **Put several on one line** | Like an RS-485 line. Everyone hears what the others send, and nobody hears themself. |
| | **Send several into one** | Everything the senders write arrives at one receiver, in the order it arrives. Nothing goes back. |
| | **Let programs take turns sending to a device** | Several programs send to one device, but only one at a time, so their bytes never mix. You choose which. All of them get the device's replies. |
| Look and check | **See the data on a port** | See what goes in and out of a port you already set up. Read only, so nothing changes for the programs using it. |
| | **Check this PC is ready** | Find out whether this PC can make ports that other programs can open, and what to do if not. |

"Put several on one line" is disabled with the tooltip "Not available in this build" where `Capabilities::shared_bus`
is off. "Let other computers use my device" needs `Capabilities::bridge`. Below the tiles: "Open a saved setup…" and
"Start from an example…" (secondary buttons, the existing Open and templates actions).

## Job form

Header (stays at the top while the body scrolls): "← All jobs" (ghost), the job title (title), its description, and on
the right Cancel and the primary "Create". The primary is disabled, with the reason as text, until the form is valid.

Body:

1. **Picture**: the ends of the job in two columns joined by lines (arrows in the direction data flows). It updates as
   ends are added or removed.
2. **Device end** (jobs with a device): segmented "Plugged into this PC" / "On the network".
   - Plugged in: combo of devices from `devices()` with the description, and Refresh. A device another program holds is
     disabled with "Another program is using it". A device already in one of the user's setups is disabled with
     "Already in one of your setups". Hint: "Not listed? Plug it in, then choose Refresh."
   - On the network: address, port (default 4001) and "TCP" or "RFC 2217". Hint: "For a serial server box, such as a
     Moxa or Lantronix. Its manual gives the address and port. Choose RFC 2217 if the box supports it, so programs can
     change the baud rate."
3. **Program ends**: one row per program: port name, and "Program, e.g. PuTTY (optional)". Where
   `Capabilities::com_numbers` is on the port name is a combo of the next free COM numbers (from COM3), and the first
   one is the default. In use means any of: a port Studio lists, a device the PC lists, another row of the form, a
   number Windows keeps for a port that no list shows (an unplugged or removed port: `StudioBackend::reserved_com_numbers`,
   read again whenever the rows are built), and a number Windows refused while the form was open. A number above 9
   shows the hint "Some older programs only list COM1 to COM9." Where the computer cannot say which numbers Windows
   keeps (`reserved_com_numbers` is empty), the combo is not shown, because a list that might offer a taken number is
   worse than none: the row shows "New port" and the hint "Studio could not read which COM numbers Windows keeps, so
   it picks a free one when the port is made and shows it when the port is ready." Elsewhere the row shows
   "Studio picks the name, such as /dev/pts/3." A minus button removes a row while more than the minimum remain. Under
   the rows: "+ Add a program" (and "+ Add a device" where devices may take part), then "Up to N. You can add more
   later." The program name is a label only; it becomes the port's name in Studio. Studio does not start or look for
   programs.
4. **Give my program a serial port** has, instead of a second column, the choice "On the other end": "I'll type to it
   from this window" (what the program sends shows on the setup page, and what you type goes to it) or "Send everything
   straight back" (a fixture that echoes; needs `Capabilities::fixture_echo`, otherwise disabled with "Not available in
   this build").
5. **Let other computers use my device**: "Network port" (1 to 65535, default 4001). Hint: "Other computers connect to
   this PC's address on this port. 4001 is common for serial over the network."
6. **Fields**: "Device speed (baud rate)" with the hint "Check the device's manual. A wrong rate usually shows as
   garbled text." and "Other device settings" (the summary, Change opens the settings section). "Name (optional)",
   whose placeholder is the title Studio would give.
7. **Also** (where the job offers them), as checkboxes with a description each:
   - "Let me type to <device> from this window": "Use it to check that the device works before your own program is ready.
     What you type goes to the device, and its replies show here and in your programs."
   - "Show me the data going through": "Lists the data sent in each direction on the setup's page. It does not change
     the data. You can turn it on later."
8. **Result sentence** (info note), live, per job. For example share: "Everything COM3 sends goes to COM5 (PuTTY) and
   COM6. Whatever any of them sends goes to the device. Your programs open COM5 and COM6, not COM3."

Create builds the setup in one go and rolls back what it made if a step fails, then shows the error pattern of
`screens/06` in the form. On success the new setup is selected in the list and its page opens.

A COM number that Windows refuses (`errc::already_exists`) is named in the sentence, never reported as a bare engine
message: "Could not set it up. COM4 already belongs to another device on this PC, such as a real port, com0com or an
earlier port that Windows remembers. Nothing was changed. Studio has chosen another number for you. Choose Create to try
again." When changing a setup: "Could not change the setup. COM4 already belongs to another device on this PC, such as a
real port, com0com or an earlier port that Windows remembers. What was there before was put back where Studio could.
Studio has chosen another number for you. Choose Apply to try again." The refused number is not offered again in that
form and the row already holds a free number. Any other refusal reads "Could not set it up. Studio reported: <the
reason>. Nothing was changed." or, when changing, "Could not change the setup. Studio reported: <the reason>. What was
there before was put back where Studio could."

## What each job builds

| Job | Engine |
|---|---|
| Give my program a serial port | one virtual port; with "Send everything straight back", a mock fixture with echo on, linked two-way |
| Connect two programs | two virtual ports, linked two-way |
| Use my device through a new port | device, one virtual port, split from the device (as Attach) |
| Share my device | device, N virtual ports, split from the device |
| Try my device | device alone |
| Let other computers use my device | `bridge(serial, tcp://0.0.0.0:port)` |
| Put several on one line | N ports (virtual or device), `bus` |
| Send several into one | N senders, `join` into the receiver |
| Let programs take turns | device, N virtual ports, `split` from the device to the programs with `merge_policy::owner`, then `set_owner` to the first program. On a duplex link the owner rule applies to the routes from the targets back to the source, so only the owner's bytes reach the device and the device's replies reach every program (see `08-engine-gaps.md`) |

Typing in Simple mode uses `send` on the port it is aimed at (the device, or the lone virtual port), so no extra port is
made. The data view reads the traffic events of the setup's ports.

## Setup page

Header: title (the name, or the generated title such as "COM3 shared with 2 programs"), the job's description, and on the
right "Change…" (secondary) and "Remove setup" (danger). A setup made in Advanced mode has "Change in Advanced mode"
instead of Change, and the sub "Made in Advanced mode. You can use it here, and change it in Advanced mode."

Sections, each with a group label:

1. **What to do now** (when the setup has program ports): numbered steps. "In PuTTY, open COM5 at 115200 baud." (the
   baud part only when a device is in the setup), one per program, then "Do not open COM3 itself in your programs.
   Studio has it open." For take turns: "Choose below which program sends. The others only listen."
2. **Ports**: one row per end.
   - Program port: "<label or Program> opens", the port name in the large mono `portName` style, a chip ("Open in a
     program" ok, or "No program yet" amber hollow; just "Open" where `Capabilities::peer_open` is off), Copy name, and
     a minus "Close this port" where the job allows more than its minimum. Waiting: "No program has opened COM6 yet. In
     Data logger, choose exactly COM6. This changes as soon as one does." with the link "Nothing arriving?" (the
     troubleshooter). When the program's baud rate is known and differs from the device's: warn "PuTTY opened COM5 at
     9600 baud, but the device runs at 115200. Text will look garbled. Set 115200 in PuTTY."
   - Device: "Device. Studio has it open, so your programs use their own ports instead.", the name, chip "Open at 115200
     baud".
   - Network listener: "Other computers connect to this PC on network port", the number, and a chip with the count of
     connected computers.
   - Take turns: each program row has "Let this one send" or the chip "Sending now".
   - "+ Add a program" under the rows for share, line, send into one and take turns. Adding or closing one port does
     not interrupt the other programs. For a share, an attached device and take turns the link is edited in place, so
     it keeps its id and its owner (`backend/share_edit`, `hub::add_target`, `hub::remove_target`); so are the senders
     of send into one and the members of a line (`hub::add_source`, `hub::remove_source`, `hub::add_member`,
     `hub::remove_member`).
3. **Type to <port>** (try my device, a lone program port, or when "Let me type" was ticked): one sentence on where the
   text goes, the received text (mono, read only), an input with "Send", and a switch "Enter adds a line ending".
4. **Data going through**: when off, the secondary button "Show me the data going through" with "Read only. Useful when a
   program says nothing arrives." When on, the last lines (time, from, to, text) with Pause, Clear, "Show as hex" and
   Hide.

Footer link: "See it on the canvas (Advanced mode)".

## Change

Change opens the job form filled from the setup, titled "Change <title>", with the primary "Apply". Above the result
sentence a note "What Apply changes" lists the ports that stay open ("Programs using them are not interrupted"), the new
ones and the ones that close ("A program using it loses it"). Apply keeps the ports that stay, makes the new ones and
removes the ports that close. For a share, an attached device and take turns on the same device it adds and takes off
targets on the same link; for send into one with the same receiver it adds and takes off sources (`hub::add_source`,
`hub::remove_source`); for a bus it adds and takes off members (`hub::add_member`, `hub::remove_member`). The link keeps
its id, its options and its owner, and the ports that stay lose no bytes. New ones are added before old ones are taken off,
so the link never has fewer ends than it needs; a failed step puts the link back. For the other setups (a pair, a port with
an echo, a network device, a different device, receiver or kind, or a bus that would pass 64 members while the new ones
join) it replaces the link, because the engine has no in-place edit for them.

## Removing

"Remove setup" and Delete on a list row confirm with "Remove this setup?", naming the ports it closes, as in
`03-information-architecture.md`. "Remove all" asks "Remove all setups?" with the number of setups. After removing, the
status bar message offers Undo where `Capabilities::undo` allows it.

## Acceptance

- Every job can be done with the keyboard alone; the tiles, rows and choices have accessible names.
- The header with Create stays visible at 200 percent text and at the smallest window.
- No Simple-mode string names a Studio part (terminal, monitor, fixture, loopback, link, endpoint, topology).
- A setup made in either mode appears in the other, and changing the mode changes nothing.
