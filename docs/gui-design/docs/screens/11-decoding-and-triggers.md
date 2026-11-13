# Screens 11: decoding and triggers

No mockups yet. The design behind these screens is `docs/plugins.md`, section 20; nothing here is built until the plugin host and the decode stream exist. Dialog anatomy, components and voice are as in screens 01 and `07-copy-deck.md`. The Monitor these screens extend is in `02-workspace.md`, and the plugin states, marks and notices are in `10-plugins.md`. Every string below is wrapped in `tr()` with the context named in its section. `<...>` is a value filled at run time.

The words: a **frame** is one message of a protocol, as the bytes on the line. A **protocol description** is a file that says how a protocol's frames are built. A **decoder** turns bytes into frames; it comes from a plugin. A **trigger** is one thing the decoder found that does not fit the protocol. Never "error" for a trigger in general (the device under test may be doing what the test wants), never "packet", never "violation". Decoding **reads**; no string may suggest it changes or delays data.

Text from outside Studio is shown as plain text, never as rich text and never as a format string: the names in a protocol description, a decoder's message names, summaries and field names, and a plugin's own trigger text. The bytes themselves were chosen by the device under test, so every value drawn from them is escaped as the Monitor escapes a preview.

## What is in Studio, and what needs the plugin

| Part | Needs | Section |
|---|---|---|
| Splitting the Monitor's rows at a silence, a byte or a count | Nothing. Community | "Rows in the Monitor" |
| Decoded frames, the Triggers tab, what a trigger does | A plugin with a decoder that is installed and on. The official one is Pro | The rest |

The rule of `10-plugins.md` applies: what the user has not switched on is hidden, with no advertisement; what the user switched on and cannot use now stays, disabled, with the reason. So with no decoder available and none ever used in this setup, "Decode as...", the Triggers tab and the Frames view are absent. With a decoder whose plugin is in Starting, Needs a licence or Stopped after failing, they are shown and say why they are not working.

## Where the state comes from
The GUI reads `StudioBackend::decoders()` (the decoder types that plugins provide), `StudioBackend::decodings()` (one view for each place being decoded: its state, protocol, counters and trigger settings), a bounded model of decoded frames beside the Monitor's ring, and a bounded model of triggers. Each changes by a signal that is coalesced with the refresh interval. `FakeStudioBackend` can produce every row, state and trigger kind below, so each has a test. The GUI never reads a protocol description, decodes a byte or decides a trigger; it draws what the host gives it.

Engine capabilities, as `08-engine-gaps.md` requires: a missing one disables its control with the tooltip "Not available in this build", and nothing is hidden for that reason alone. `events_bus_fault_detail` and `engine_rules` are the names `docs/plugins.md` and `docs/response-timing.md` use; the other three names are proposed here and are fixed when the host adds them.

| Capability | Without it |
|---|---|
| `event_times` (every chunk has the engine's time) | "After a silence" in the Rows menu is disabled. Decoders are not offered |
| `decode_stream` | "Decode as..." is disabled |
| `events_bus_fault_detail` | A "Bus contention" trigger has no second line, and its detail says "Detail not available in this build." |
| `capture_ring` | "Keep a recording" is disabled |
| `engine_rules` | "On the next matching frame, exactly" is disabled |

## Rows in the Monitor (Community)
Context `MonitorRows`. A combo in the Monitor's toolbar, after Follow, with the label "Rows" and the accessible name "How the Monitor splits rows". Simple and Advanced mode. It changes how bytes are shown and nothing else: nothing is marked as right or wrong, and the counters and the export of bytes do not change.

| Entry | Field beside it | What a row is |
|---|---|---|
| "As received" (default) | none | One row for each chunk, as today |
| "After a silence" | Spin box "Silence", 1 to 10 000, unit combo "ms" or "µs", default 5 ms | A new row starts when nothing arrived at that port and direction for that long |
| "After a byte" | Hex field "Byte", one or two bytes, default `0A` | A new row starts after that byte or pair |
| "Every n bytes" | Spin box "Bytes", 1 to 4096, default 16 | A new row every n bytes of a port and direction |

- The choice applies to the ports in the Monitor's port filter and is kept in the window's settings, not in the setup file.
- A row never joins bytes of two ports or two directions. A row's Time is the time of its first byte's chunk.
- A row that grows past 4096 bytes is closed there, and its Preview ends with the muted text "(continued)".
- Under the field, for "After a silence", a muted line: "On a virtual port a silence is a pause between two writes by the program. Two messages written at once have no silence between them."
- A place that is being decoded uses its decoder's frames in the Frames view (below); this menu then applies to the Bytes view and to the places that are not decoded.

Acceptance: each entry is reachable by keyboard and its field takes focus when the entry is chosen; changing the entry rebuilds the visible rows from the ring without losing the scroll position's time; 10 000 rows with "Every n bytes" at 1 stay responsive (no per-row widgets); the muted line is the field's accessible description.

## Decode a port or a link (width 640)
Context `DecodeDialog`. Opens from "Decode as..." in the menu of a port, a link or a bus (canvas, Ports list and Topology), and from the Inspector's Decoding card. Advanced mode. Title "Decode <place name>". Sub "Shows the data on <place name> as messages of a protocol, and reports what does not fit. Decoding only reads. It never changes or delays the data."

- **Protocol** (combo, accessible name "Protocol"): one entry for each decoder type of each available plugin, by name, then a separator and "A protocol description file...". Under the combo, the muted line "From the plugin <plugin name> (<trust line>)." A type whose plugin is on and cannot be used now is listed disabled with the reason from the Toolbox table of `10-plugins.md`.
- **Description file**, only for "A protocol description file...": a path field with "Browse..." (file chooser titled "Choose a protocol description", filter "Protocol descriptions (*.json)"). When a file is chosen, a definition list under it from the file: "Name", "Version", "Messages" (a count). A file that cannot be used marks the field `invalid` with one of: "This file is not a protocol description." "This description was made for a newer version of Studio." "The description has a mistake: <reason>" (the reader's own sentence, plain text).
- **Settings**: the decoder's own parameters, built by the Settings form of `10-plugins.md`.
- **Test cases**, only when a folder named `corpus` lies beside the description: a quiet button "Run its test cases" and the result as one line, "<n> of <m> test cases pass." (`%n`) or "Test case <name> differs at line <n>." with a quiet "Copy details".
- An info NoteBanner at the bottom: "<plugin name> will read the data that passes through <place name>." A place already decoded by something else: warn, "<place name> is already decoded as <protocol name>. Starting this replaces it."

Footer: Cancel, "Start decoding" (primary; disabled until the protocol and, where needed, the file are valid). When the plugin has not been given the permission to read data on that place, the button is "Review and start", which opens the decision dialog of `10-plugins.md` on its permissions step first.

Afterwards: toast "Decoding <place name> as <protocol name>." The status bar's "Reading data" chip of `10-plugins.md` shows for as long as the decoding runs.

Acceptance: the dialog is operable by keyboard alone; a description of 256 KiB is read without blocking the GUI thread (the check runs on a worker, and the button shows a busy state); no string from the file is interpreted as markup; Cancel changes nothing.

## Decoding in the Inspector and on the canvas
Context `DecodingCard`. A card "Decoding" in the Inspector of a port, link or bus that is decoded, under the link or line controls.

| Row | Value |
|---|---|
| "Protocol" | `<protocol name>`, and `<version>` in the fixed font when the description has one |
| "From" | "<type name>, from the plugin <plugin name> (<trust line>)" and the plugin's state chip |
| "State" | A chip: "Decoding" (ok, filled), "Starting" (waiting, hollow), "Fell behind" (warn, hollow), "Not decoding" (neutral, hollow), "Stopped" (failed, cross) |
| "Frames" | A count |
| "Triggers" | A count, as a link that opens the Triggers tab filtered to this place |
| "Not decoded" | "<n> bytes in <m> gaps" (`%n`), only when a gap happened |

Buttons: "Change..." (opens the dialog above), "Stop decoding" (quiet), "When a trigger happens..." (quiet).

"Not decoding" and "Stopped" add a NoteBanner with the plugin's reason and action, as the table "When the plugin is not available" in `10-plugins.md` does, ending "The data on <place name> is not affected."

On the canvas the place carries the plugin mark of `10-plugins.md`. Its accessible name gains ", decoded as <protocol name>". Its menu has "Decode as...", "Stop decoding" and "Show triggers".

## Frames in the Monitor
Context `MonitorFrames`. When at least one place in the Monitor's port filter is decoded, a SegmentedControl appears in the toolbar before Rows: **Frames** and **Bytes**, accessible name "Show frames or bytes". Frames is chosen when a decoding starts. Bytes is the Monitor exactly as `02-workspace.md` has it.

In Frames, a decoded place contributes one row for each frame instead of one for each chunk. Places that are not decoded keep their byte rows, in time order among the frames.

| Column | For a frame |
|---|---|
| Time | The time of the frame's first byte |
| Port | As today |
| Direction | DirectionBadge, as today |
| Bytes | The frame's length |
| Message | New column. The message's name, elided at the end. Empty for a row that is not a frame |
| Preview | The decoder's summary in the proportional font. With Hex ticked, the frame's bytes |

Rows that are not frames, on a decoded place:

| Row | Message | Preview |
|---|---|---|
| Bytes between frames | "Not in a frame" (muted) | The bytes |
| A gap in decoding | "Not decoded" (muted) | "<n> bytes were not decoded here. The decoder fell behind. The data was not affected." (`%n`) |
| The decoder stopped | "Not decoded" (muted) | "Decoding stopped here." Rows after it are byte rows |

**A frame with a trigger** has a mark at the start of its Message cell: the `trigger` icon with the kind's name as its tooltip, drawn in `warn`. The mark is a shape, so it reads without colour. The row's accessible name ends ", <kind name>". With more than one trigger, the tooltip lists them and the name ends ", <n> triggers" (`%n`).

Row accessible name: "<time>, <port>, <received or sent>, <message name>, <n> bytes" (`%n`), then the triggers.

**Frame detail.** A pane under the table, shown when a frame row is selected and closed with its own close button or Escape. Heading: the message's name, then "<n> bytes, <duration>" where the place has byte times. A table with the accessible name "Fields of <message name>":

| Column | Content |
|---|---|
| Field | The field's name |
| Value | The value. Studio draws it from the bytes where it knows the type (a number in decimal and hex, text escaped as the Preview is); otherwise the decoder's text. A named value shows as "<name> (<number>)" |
| Bytes | The field's bytes as hex, fixed font |

Selecting a field row highlights its bytes in a hex line above the table (an underline and the `accentSoft` fill, never colour alone). Under the table, one NoteBanner for each trigger on the frame, with the kind's sentence from "Trigger kinds" and the action "Show in Triggers". Buttons: "Copy frame as hex", "Copy fields" (tab-separated text).

The filter row's "Search payload" also matches message names and summaries in Frames. Export in Frames saves the visible rows as text with the Message column; the bytes are unchanged by decoding and are exported from Bytes.

Performance: frames are a second bounded ring (10 000 rows by default, a named constant) filled in batches at the refresh interval. No per-row widgets. The detail pane is one widget that is refilled. Pause stops appending to both rings; decoding and counting go on.

Acceptance: switching between Frames and Bytes keeps the selected time in view; a frame row and its bytes in Bytes can be reached from each other ("Show bytes", "Show frame" in the row's menu); a decoder that stops leaves byte rows and never an empty table; a summary with control characters or direction marks is shown with them removed; F6 moves between the table and the detail pane; everything works at 200 percent text and in both themes.

## Triggers tab
Context `TriggersTab`. A tab "Triggers" after "Monitor", in Simple and Advanced mode, present while a decoding exists in the setup or triggers of this run are held. The tab's label carries the count since it was last looked at: "Triggers (3)". Its accessible name is "Triggers, <n> new" (`%n`).

- **Toolbar**: Pause, Clear (clears the list on screen; the run's counts stay), Follow (default on), a combo "Kind" ("All kinds", then each kind that has occurred with its count), the Monitor's port combo, a search field "Search triggers" (Ctrl+F), "When a trigger happens..." (quiet), Export.
- **Counts strip** under the toolbar: one StatusChip for each kind that has occurred, "<kind name> <n>", neutral; a chip is a button that sets the Kind filter. The counts are the run's exact counts. When the list holds fewer triggers than happened, a muted line at the right: "Showing the last <n> of <m>." (`%n`)
- **Columns**: Time (HH:MM:SS.mmm, the time of the bytes the trigger is about, not when it was reported), Place, Kind, Message, What (the kind's sentence).
- **Empty state**: with a decoding running, title "No triggers", text "Everything decoded on <place names> has fitted the protocol so far." With none running: title "Nothing is being decoded", text "Triggers come from decoding a port or a link.", button "Decode a port..." where a decoder is available.
- **Export** saves the triggers that match the filter as JSON lines in the public trigger format (`docs/plugins.md`, section 20.7), one trigger on a line. File chooser title "Export triggers". Toast "Saved <file name>."

**Trigger detail.** A pane under the table for the selected trigger. Heading: the kind's name and the place. A definition list; rows with no value are left out:

| Row label | Value |
|---|---|
| "When" | The time, and "<seconds> s after the run started" with nanosecond digits in the fixed font |
| "Place" | `<place name>`, the direction, and on a bus "sent by <member name>" |
| "Protocol" | `<protocol name>` `<version>` |
| "Message" | `<message name>` |
| "Field" | `<field name>`, "bytes <a> to <b> of the frame" |
| "Expected" | Hex, fixed font |
| "Actual" | Hex, fixed font |
| "Request" | "<message name> at <time>", for a missing or unmatched reply |
| "Cause" | "A bus fault at the same time: <fault name>." or "An action of this decoding caused it." |
| "Recording" | "Kept: <file name>" with a quiet button "Show in folder" |
| "Reported by" | "<type name>, from the plugin <plugin name>" |

Buttons: "Show in Monitor" (primary; selects the frame in Frames, or the bytes in Bytes when the frame is no longer held), "Copy details" (the trigger as JSON). When the Monitor's ring no longer holds the bytes: "Show in Monitor" is disabled with the tooltip "The Monitor no longer holds this data."

### Trigger kinds
The Kind column and every chip use the name. The What column and the banners use the sentence, built by Studio from the trigger's fields. A plugin's own text is used only for a kind the plugin declared, and is then shown as "The plugin says: “<text>”".

| Kind | Name | Sentence |
|---|---|---|
| `framing` | "Framing" | "A frame started and did not end." "A frame was longer than <n> bytes." (`%n`) "<n> bytes arrived between frames." (`%n`) |
| `unknown_type` | "Unknown message" | "Message type <value> is not in the protocol description." |
| `unknown_value` | "Unknown value" | "<field name> is <value>, which the description does not allow." |
| `check` | "Check value" | "Check value is <actual>, expected <expected>." |
| `length` | "Length" | "The length field says <n> bytes and <m> arrived." |
| `timeout` | "No reply" | "No reply to <message name> within <duration>." |
| `sequence` | "Out of sequence" | "A reply arrived with no request." "A second request was sent before the reply." "<field name> went from <a> to <b>." |
| `contention` | "Bus contention" | "Two members sent at the same time." "<member name> sent too soon after the last frame." "<member name> held the line." "<member name> sent with its transmitter off." "The line was overrun." |
| `line_error` | "Line error" | "A parity error.", "A framing error on the line.", "An overrun.", "A break." each followed by " during this frame." where it fell inside one |
| `decoder_lost` | "Not decoded" | "<n> bytes were not decoded. The decoder fell behind." (`%n`) or "The decoder stopped." |
| `trigger_storm` | "Too many triggers" | "<n> more triggers in this second are counted and not listed." (`%n`) |
| `custom` | The name the plugin declared, plain text | The plugin's text, as above |

A "Check value" trigger never says which data byte is wrong. Where a bus fault covers the same bytes, the Cause row says so, and that is the only case where the detail names damaged bytes.

A "Not decoded" trigger is drawn muted, not in `warn`: it says detection had a hole, not that the device did something. A test that reads the counts can tell the two apart by kind.

Acceptance: every kind and every sentence can be shown from `FakeStudioBackend`; the counts strip equals the host's counts when the list has dropped rows; a storm of 10 000 triggers a second keeps the window responsive and adds at most one batch a refresh; new triggers are announced as a polite live region at most once a refresh, as "<n> new triggers" (`%n`), and not one by one; Export reads back as valid JSON lines; no plugin text is used as a format string.

## When a trigger happens (width 720)
Context `TriggerActionsDialog`. Opens from "When a trigger happens..." in the Triggers toolbar and in the Decoding card. One dialog for one decoding. Title "When a trigger happens on <place name>". Sub "Every trigger is listed in Triggers and written to the log. Here you choose what else happens."

A table with one row for each kind the decoder can report, and a first row "Any kind". Columns:

| Column | Control |
|---|---|
| Kind | The kind's name |
| "Keep a recording" | Check box |
| "Then do" | A combo: "Nothing" (default), "Raise a fault", "Set a line", "Send bytes", "Switch a rule set on", "Switch a rule set off" |

Under the table, for the selected row:

- **Keep a recording**: spin boxes "Seconds before" (0 to 60, default 5) and "Seconds after" (0 to 60, default 2). Muted line: "Studio keeps the data around the trigger from a recording that is already running in memory. The recording is saved to <folder>." A quiet button "Choose folder...". When no recording runs for the place: "This starts recording <place name> in memory, up to <size>."
- **Then do**: the fields of the chosen action (the fault kind; the line and its level; a hex field for the bytes and the port to send from; the rule set), and **When**, a radio group:
  - "As soon as Studio hears of the trigger" with the muted line "A few milliseconds after the data. It is too late to answer the frame that caused it."
  - "On the next matching frame, exactly" with the muted line "Studio does it itself on the next frame that starts with <bytes>, with exact timing." Offered for a kind and a message whose start is fixed; otherwise disabled with the tooltip "This message has no fixed start, so Studio cannot match it by itself."

Notes in the dialog:

- When nothing in "Then do" is chosen anywhere, an info NoteBanner: "Decoding only reads. Nothing here changes the data."
- When anything is chosen, a warn NoteBanner: "An action that damages data causes more triggers. Studio stops the actions of this decoding after 10 in one second and goes on decoding."
- When the plugin has not been given the permission an action needs, the action is listed disabled with the tooltip "<plugin name> is not allowed to do this." and a quiet button "Change what it may do..." under the table (the decision dialog of `10-plugins.md`).

Footer: "Restore defaults" (quiet), Cancel, "Apply" (primary). The choices are settings of the decoding and are saved with it in the setup.

Delivery to a test harness has no screen: a harness reads triggers through the automation connection (`docs/plugins.md`, section 20.8), and the Triggers tab shows the same records.

Acceptance: with every "Then do" at "Nothing" and no permission but reading, the dialog offers recordings only and no action can be reached by any path; the table and the fields under it are reachable by keyboard in that order; the "When" sentences are the accessible descriptions of their radio buttons; a triggered action appears in the Log tab with the trigger's number.

## Notices and the status bar
Context `DecodingNotice`. NoteBanners at the top of the window, in the place and by the rules of the plugin notices in `10-plugins.md`. One at a time, never modal.

| Event | Kind | Text | Actions |
|---|---|---|---|
| A decoder stopped | warn | "Decoding of <place name> has stopped. The data is not affected." | "Plugins", "Start again", "Dismiss" |
| A decoder fell behind | info | "Decoding of <place name> fell behind and missed <n> bytes. The data is not affected." (`%n`) Shown once for a run, then counted in the Decoding card | "Show triggers", "Dismiss" |
| Actions stopped by the limit | warn | "The actions of the decoding on <place name> were stopped: more than 10 in one second. Decoding goes on." | "Start the actions again", "Show triggers", "Dismiss" |
| A recording was kept | none; a toast | "Kept a recording of <place name> for a trigger." Action "Show in folder" | |
| A recording could not be kept | warn | "A recording for a trigger on <place name> could not be saved: <reason>" | "Choose folder...", "Dismiss" |

Status bar: while this run has triggers that are not "Not decoded", a StatusChip left of the "Reading data" chip, "<n> triggers" (`%n`; "1 trigger"; warn, hollow). It is a button that opens the Triggers tab. Accessible name: "<n> triggers. Open Triggers." It is cleared by Clear in the Triggers tab and by a new run.

## New assets and components
- Icon `trigger` in `docs/gui-design/icons/` (a flag outline in the set's stroke style), loaded through `IconProvider`. To be drawn with the first GUI slice.
- No new component. SegmentedControl, StatusChip, NoteBanner, EmptyState, DirectionBadge and the definition list are reused, and the frame and trigger tables are delegates over models, as the Monitor is.
- Dialog widths 640 and 720 are added to `tokens.json` `layout.dialogWidths` with the first GUI slice.

## Accessibility notes for all of the above
Marks and chips carry text or shape, never colour alone. Every table has an accessible name and each row a name that says what a sighted user reads across it. New rows and triggers are announced in batches, politely, never one by one. The detail panes are reachable with F6 and closed with Escape, which returns focus to the row. All of it works in both themes from tokens only, with icons from `IconProvider`, targets of at least 24 px, a visible focus ring, and 200 percent text.
