# User experience design

This page records who the window is for, how the two levels differ, and the
human-factors reasoning behind the layout. The task-by-task instructions live
in [user-guide.md](user-guide.md). The screen-by-screen specifications are in
[the screen specifications](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/gui-design/docs/00-product-brief.md).

## Who it is for

Three classes of user share the same engine but not the same vocabulary.

### The operator

"I have a program that needs a serial port and the machine has none."

- Knows the port name their program expects, not link modes or URIs.
- Cares that the port exists, is stable, and can be opened.
- Works at the **Simple** level, ideally without ever seeing the word
  *topology*.

### The integrator

"I need several programs to share one device, or to feed one program from
several sources."

- Understands split, join and redirection conceptually, but not the library's
  `link_mode` enum.
- Often runs against a test rig and repeats the same setup.
- Uses **Simple** for the routine work and **Advanced** when wiring network
  endpoints or owner policies.

### The diagnostician

"Why is nothing arriving, and which build is this?"

- Reads counters, control lines and the traffic log.
- Wants raw addresses, fixtures, the topology JSON and reproducible commands.
- Uses **Advanced** plus `vss_cli --verbose`, the support bundle and the
  Tracy profiling build.

The two levels are two vocabularies over the same operations, not a basic and
a pro feature set. Every port and link created at the Simple level is an
ordinary studio object, and switching levels never changes what has been built.

## Principles

1. **Start from the job, not the mechanism.** The Simple level is a list of
   your setups and a grid of jobs: give my program a port, connect two programs,
   use or share a device, put several on one line, see the data. Each ends by
   saying what to open in the other program.
2. **Recognition over recall.** Operations are menu items, toolbar buttons or
   dialogs that list the actual ports by name. No address typing is needed for
   the everyday jobs, and the command palette (`Ctrl+K`) finds any action by
   its words.
3. **Progressive disclosure.** Simple shows the everyday jobs. Advanced adds
   the diagram, the panes and the options, and a "More options" row in a dialog
   says what is there and how to turn it on.
4. **Plain language first.** "Two-way", "one-way", "split", "join", "watch",
   "linked pair". Internal names stay in the code.
5. **Say what will happen.** Every dialog prints a sentence describing the
   result, with the real names in bold, before the button is pressed.
6. **Prevent rather than report.** Dialogs refuse self-links, loops and too few
   targets before anything is created, and the add-port dialog appends unique
   suffixes silently.
7. **Fail visibly and locally.** Errors name the operation and carry the
   engine's message behind "Copy details"; they do not close the window or
   discard the setup. A wizard that fails part-way removes what it created.
8. **Degrade, never pretend.** When the engine cannot do something, the control
   is shown disabled with "Not available in this build", or the simpler variant
   is shown. The list is in
   [08-engine-gaps.md](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/gui-design/docs/08-engine-gaps.md).
9. **Everything works from the keyboard.** Every action has a menu item, and
   the shortcut list is generated from the same action registry as the menus.
10. **Accessibility.** Names and roles on every control, a visible focus
    ring, 24 pixel targets, both themes and 200 percent text, checked by a
    test that walks every dialog.
11. **Unsaved work.** A prompt protects it on exit; it can be
    turned off for scripted sessions.

## The two levels

| Surface | Simple | Advanced |
|---|---|---|
| Menus | Same menus; some items hidden | Adds Network endpoint, Mock fixture, Export as command, Link wiring, Owner, Add port/fixture/pair, Tidy layout, Fit, Duplicate, Benchmark, Toolbox, Log, Topology JSON |
| Main area | **Your setups** list and a page: the job grid, a job form, or the page of one setup | The topology diagram with a toolbox and a row of buttons (Attach, Connect, Mock, Share, Fit, Remove) |
| Panes | None: the setup's page holds what to open, **Type to** and **Data going through** | Ports, Inspector, Send and Monitor |
| Ports | A row per port on the setup's page, with a copy button and a chip | A table with name, role and path, state, RX, TX, Dropped and a filter |
| Inspector | Not shown | State, path, COM number, traffic, control lines (DTR and RTS switches, Break), fixture behaviour, Release port |
| Send and Monitor | A text box and a live data list per setup | Send (Repeat, Saved sends) and Monitor (filters, search, ASCII or Hex, Export) with Log and Topology JSON tabs |
| Dialogs | Guided, address read only, settings folded away | Address editable, settings open, More options |
| Status bar | Counts, rates, hub state, readiness chip | Adds a chip when bytes were dropped |

At Simple the Ports, Inspector, Send and Monitor panes and the diagram are not
shown at all: a setup's own page carries the feedback a beginner needs (which
port to open, whether a program has opened it, what is arriving). Advanced
brings the panes and the diagram back, and adds a canvas to build on.

Switching is one checkable menu item (**View → Advanced mode**, `Ctrl+Shift+A`),
the segmented switch at the right of the menu bar, a switch in Settings, or the
links in the launcher and the dialogs. The setting is global and persisted. The
current level is always on screen so a screenshot is self-describing.

### Screen map

Simple:

```
Menu: File Edit Ports Topology Tools View Help     [Search actions] [Simple|Advanced]
┌──────────────────┐  ┌──────────────────────────────────────────────┐
│ Your setups      │  │ Job grid, a job form, or the page of a setup │
│  (list)          │  │                                              │
│ Save… Open…      │  │                                              │
│        Remove all│  │                                              │
└──────────────────┘  └──────────────────────────────────────────────┘
Status: Ports Links RX TX Hub                          Setup ready
```

Advanced:

```
Menu: File Edit Ports Topology Tools View Help     [Search actions] [Simple|Advanced]
┌───────────────┐  ┌──────────────────────┐  ┌──────────────────┐
│ Ports         │  │ Buttons              │  │ Inspector        │
│               │  │ Toolbox | Topology   │  ├──────────────────┤
│               │  │          Owner bar   │  │ Send             │
└───────────────┘  └──────────────────────┘  └──────────────────┘
Monitor | Log | Topology JSON
Status: Ports Links RX TX Hub  [dropped]               Setup ready
```

At the Advanced level with no ports, the diagram is empty and the toolbox is
shown; **View → Show launcher** brings up a page with the four first jobs. At the
minimum window of 960 by 640 the toolbar shows icons only and the panes are
shorter.

## Task flows

### Give my program a port (operator)

At Simple, the **Give my program a serial port** job and **Create**; the setup's
page then says which port to open. At Advanced (or from the Ports menu), **Ports
→ Add port** and **Create port**: the **Port ready** dialog shows the name to
open in large type, with **Copy name**. The disclosure below it holds the
saved-setup entry and the equivalent `vss_cli` command.

Design notes: the name field is pre-filled and the suffix is automatic; the
dialog explains the three kinds in one sentence each; the result sentence
changes with the count; the dialog says so when there is no driver ("port-1
is ready inside Studio").

### Connect two programs (operator)

**Connect two programs** makes a linked pair like a null-modem cable. The
dialog shows both ports as large names under "Program A opens" and "Program B
opens", and says which control lines are wired.

### Wire existing ports (integrator)

**Topology → Connect ports** chooses a relationship in plain words (two-way,
one-way, monitor, split, join, shared bus), lists only ports that exist, and updates its
summary sentence live. It validates before closing; a failure leaves the
dialog's data intact. At Advanced the same links can be drawn on the diagram by
dragging from one box to another.

### Share a device (integrator)

**Share a device** is a three-step wizard: choose the device, say how many
programs and see which port each will open, then a summary of what to open in
each program. When there are too few driver pairs it says so and offers to
create another pair or keep the extras inside Studio. It is one rollback unit:
a failure removes what it created. A program can be added to or removed from a
share later without disturbing the other programs.

### Watch traffic (diagnostician)

**Watch traffic** adds a read-only copy of a port as a Studio-only port, so the
watcher can be removed when finished. It never drives modem lines.

### Diagnose (diagnostician)

Select a port: the Inspector shows state, counters and control lines. **Help →
Why is nothing arriving?** walks the steps from device to program. **Tools →
Setup check** reports what the PC needs. **File → Support bundle** gathers
the setup, log and environment; it never holds the bytes the ports carried.
**About → Environment** gives the build.

## Human factors decisions

- **The job grid is a page, not a wizard.** It names the jobs in the user's
  words and says what each produces. Each job is one short form with a sentence
  that says what will be built, and the finished setup stays in the list on the
  left. At Advanced a launcher page with four jobs is available from **View →
  Show launcher** and **File → New**.
- **Readiness is always visible.** A strip on the job grid and the launcher, and
  a chip in the status bar, say whether other programs can open the ports Studio
  creates, because "the port exists but nothing can see it" is the commonest
  failure.
- **Say only what Studio can see.** Where Studio can tell that a program has
  opened a port (a pseudo terminal on Linux, a port of the Studio driver on
  Windows) the port reads *Waiting for program* and then *Program connected*.
  Elsewhere a healthy port reads *Open* and the troubleshooting list says "Not
  checked yet" for the last steps. It does not guess.
- **The log is Advanced.** It is useful when diagnosing and noise for
  everyone else; the monitor carries the everyday feedback.
- **Simple speaks of setups, not wiring.** The page of a setup says which port
  to open in which program and hides the boxes and lines; a beginner should not
  have to read a diagram to use a port. **See it on the canvas (Advanced
  mode)** shows the diagram, where the boxes are the mental model. A list of
  links is available from **View** at Advanced for screen readers and for people
  who prefer one.
- **Level is global, not per document.** A user works as an operator or as a
  diagnostician, and does not switch from file to file.
- **Refresh is adjustable.** The default, 250 ms, feels live; busy machines can
  slow it down without changing what the studio does.
- **Theme and density follow the user.** The theme follows the desktop by
  default and can be forced, because lab machines and screenshots often need a
  fixed light or dark surface. Density is comfortable or compact.
- **No fonts are shipped.** The window inherits the platform UI font, so text
  metrics, scripts and accessibility settings match the rest of the desktop.
- **Nine languages, right to left included.** Hebrew, Yiddish and Arabic mirror
  the whole layout. See [i18n.md](i18n.md).

## Accessibility

- Every control has an accessible name and role.
- Colour is never the only signal: states are printed as text in the table and
  inspector, and the topology labels its links.
- Shortcuts are standard where the platform has a standard, and the Help menu
  lists them all.
- Dialogs are keyboard-navigable, validate on accept, and keep the user's input
  when validation fails.
- `vss_gui --selftest` walks the main window and every dialog in both themes,
  and a second CTest entry repeats the walk at 200 percent scale. It fails on
  an unnamed focusable control, a target under 24 by 24, a control that cannot
  take keyboard focus, or clipped text.
- `VSS_HIGH_CONTRAST=1` drops the custom stylesheet and uses the system
  palette; `VSS_REDUCE_MOTION=1` makes every transition zero-duration.

## Open design questions

- Should the Connect dialog remember the last relationship per user?
- Which engine capabilities are worth building next? The disabled ones are
  undo of adding and removing ports, and observing whether a program opened a
  port on macOS or on a Windows port that is not from the Studio driver. Each
  is a row in
  [08-engine-gaps.md](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/gui-design/docs/08-engine-gaps.md).

## Roadmap

1. Undo and redo for adding and removing ports with the menus and dialogs, once
   the engine can restore endpoint identities exactly. Link, owner and name
   changes, and the edits made on the Advanced diagram, can be undone now.
2. Saving the line wiring in the setup file. The matrix and fault
   injection are done.
3. Showing whether another program has opened a port on macOS, and on a
   Windows port that is not from the Studio driver. Linux pseudo terminals and
   Studio driver ports show the *Waiting for program* and *Program connected*
   states already.
4. Reconnect for a port that failed, with the note and button the design
   specifies. The **Recreate ports** banner covers Windows driver ports that a
   restart took away.
5. Real-device templates that open the attach dialog.
6. Applying the baud rate and framing an RFC 2217 client asks for to a real
   device that the server is linked to.
