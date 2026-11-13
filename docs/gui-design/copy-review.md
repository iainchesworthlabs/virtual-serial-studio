# Copy review: plain language

Status: applied, except rows 28 and 29. The owner approved the table. The specs, the HTML mockups, the `tr()` calls, the eight translated catalogues (`tools/ts_tool.py`; the English text is the source) and the tests that assert on a changed text now use the proposed text.

- Rows 1 to 9 and 11 to 27 and 30 to 38 were applied as written, with one correction: row 12 keeps its `<a href="advanced">` link around "Advanced mode".
- Row 10 was applied by deleting `ShellWindow::later_package`, which nothing called, and its string. Row 9 is the only string left, and `ShellWindow::placeholder` uses it.
- Rows 28 and 29 were not applied. The copy deck does not name "Engine" for the user: it says "Engine text lives only behind Copy details", and the only other "engine" strings in the window are two internal messages. "Hub running" is also in the user guide and in the workspace spec and mockups. The copy deck says to avoid "hub" but gives no replacement, so the owner needs to pick a word first.
- Rows 3 and 36 were checked against the code and are true: the bundle is saved as a `.zip`, and a Studio-only port is an in-process port that only Studio can open (the same fact appears in the ports table).
- The proposed texts in the table are the ones now in the code. In the translated catalogues each changed text is retranslated, not left unfinished.

## Scope and method

- Reviewed: 1750 distinct English source strings in `apps/vss_gui/src` (taken from the `<source>` entries of the German catalogue, `apps/vss_gui/translations/vss_gui_de.ts`, which lists every `tr()` string once), read against `docs/gui-design/docs/07-copy-deck.md` and the screen specs in `docs/gui-design/docs/screens/`.
- Flagged: 38 strings (rows below). The rest were judged plain and are not listed.
- Left alone on purpose: the user guide's terms (Simple, Advanced, setup, job names), the GNU licence text, the texts the copy deck fixes word for word (the attach, share, remove and driver set-up messages), first-person job labels that the copy deck requires ("I'll type to it from this window"), menu search keywords (they are not shown), numbers and ordinals, and format strings that only join other strings.
- Not flagged although repeated across the specs: "Studio never changes ..." sentences. They state a fact about behaviour and stay.
- The Spec column names the file under `docs/gui-design/docs/` that contains the same text. "none" means no spec contains the text, so the code is the only place to change.
- Changing a row means changing the spec, the `tr()` call, nine catalogues (`tools/ts_tool.py`) and any GUI test that asserts on the text. Strings that appear in both a plain and a rich-text form (with `<b>`) need both changed.

## Other points

- The copy deck says Australian English; `AGENTS.md` says British English. The strings use the spellings that both share. This needs a decision, but it is not a plain-language matter.
- Several messages end with "Nothing was changed." This is long but literal and useful, so it stays.

## Strings to change

| # | Spec | Source | Current text | Proposed text | Reason |
|---|---|---|---|---|---|
| 1 | `screens/06-support-and-system.md` | about_dialog | Copy this into a bug report. It answers "which build is this?" | Copy this into a bug report. It shows which build you are running. | Rhetorical question and answer. |
| 2 | `screens/06-support-and-system.md` | diagnostic_bundle_dialog | Gathers what a helper needs to understand your setup. Nothing is sent anywhere. You choose where to save it. | Collects your setup, event log and environment details into one file. Nothing is sent anywhere. You choose where to save it. | "Gathers" and "a helper" are vague; say what goes in. |
| 3 | `screens/06-support-and-system.md` | diagnostic_bundle_dialog | Nothing leaves this PC until you share the file yourself. Look inside the bundle first if you want to check it. | Nothing is sent. The file is a zip that you can open and read before you share it. | Shorter, and says what the user can do. |
| 4 | `screens/01-launcher-and-basic-flows.md`, `screens/05-templates-and-advanced-tools.md` | add_fixture_dialog, add_port_dialog | A pretend device you can script, for testing without hardware. | A simulated device for testing without hardware. | "Pretend" is casual. |
| 5 | none | add_port_dialog | Programs that open a fixed name such as COM3 find the port again every run. Automatic takes a free number above the ports this PC already has, never COM1 or COM2. A number needs the Studio driver and its helper service, and is used when one port is made. | A program that opens a fixed name such as COM3 needs the same number each time. Automatic takes a free number above the ports this PC already has, never COM1 or COM2. A number needs the Studio driver and its helper service, and is used when one port is made. | "Find the port again" is loose; the sentence is also longer than it needs to be. |
| 6 | none | add_port_dialog | Network endpoints arrive in a later work package. Nothing has been created. | Network endpoints are not available yet. Nothing was created. | "Work package" is project planning language, not for users. |
| 7 | none | add_port_dialog | Mock fixtures arrive in a later work package. Nothing has been created. | Mock fixtures are not available yet. Nothing was created. | "Work package" is project planning language, not for users. |
| 8 | none | monitor_panel | The topology JSON editor arrives with WP7. | The topology JSON editor is not available yet. | "WP7" means nothing to a user. |
| 9 | none | shell_window | %1 is part of a later work package. | %1 is not available yet. | "Work package" is project planning language, not for users. |
| 10 | none | shell_window | %1 arrives with %2. | %1 is not available yet. | Planning language. %2 is a work package name. |
| 11 | `screens/05-templates-and-advanced-tools.md` | benchmark_dialog | Do not link these two ports in Studio. The data has to travel through the cable or driver pair, so the count means something. | Do not link these ports in Studio. The data must travel through the cable or driver pair to be counted. | "So the count means something" is rhetorical. |
| 12 | `screens/01-launcher-and-basic-flows.md` | launcher | Need addresses, fault injection or port ownership? <a href="advanced">Advanced mode</a> adds them to this same window. Nothing you have built changes. | Advanced mode adds addresses, fault injection and port ownership to this window. Nothing you have built changes. | Rhetorical opening question. |
| 13 | none | help_contents_dialog | Every action is in the menus. Start with these. | These are the most used actions. All actions are in the menus. | "Start with these" is chatty. |
| 14 | `screens/06-support-and-system.md` | shortcuts_dialog | Every action has a menu item. These are the quick ways. | Shortcuts for the actions in the menus. | Chatty; the second sentence adds nothing. |
| 15 | none | job_form | Whatever your program sends comes back to it at once. A quick test that its sending and receiving both work. | Whatever your program sends comes back to it at once. Use it to check that the program can send and receive. | "A quick test" is a fragment and loose. |
| 16 | `screens/08-simple-mode.md` | job_form | The device's manual gives it. If text looks garbled later, this is usually why. | Check the device's manual. A wrong rate usually shows as garbled text. | "Gives it" and "later" are loose. |
| 17 | `screens/08-simple-mode.md` | job_form | Handy for checking the device works before your own program is ready. What you type goes to the device, and its replies show here and in your programs. | Use it to check that the device works before your own program is ready. What you type goes to the device, and its replies show here and in your programs. | "Handy" is casual. |
| 18 | `screens/08-simple-mode.md` | job_form | A live list of what is sent each way, on the setup's page. It only looks, so it changes nothing. You can turn it on later too. | Lists the data sent in each direction on the setup's page. It does not change the data. You can turn it on later. | Three short claims about the same thing can be one; "too" is chatty. |
| 19 | `screens/05-templates-and-advanced-tools.md` | line_matrix_panel | Device vanishes, as if unplugged | Device disconnects, as if unplugged | "Vanishes" is dramatic. |
| 20 | `screens/05-templates-and-advanced-tools.md` | line_matrix_panel | Incoming data lost, too fast | Incoming data lost because it arrived too fast | Clipped phrase; say what happened. |
| 21 | none | release_port_dialog | Make the program that has this port let go of it. | Make the program release this port. | "Let go of it" is casual. |
| 22 | `screens/06-support-and-system.md` | settings_dialog | How often counters and states are read. Shorter feels live. Longer is lighter on a busy machine. | A shorter interval updates more often. A longer interval uses less CPU. | Slogan style contrast. |
| 23 | `screens/06-support-and-system.md` | settings_dialog | 50 ms, live | 50 ms, most frequent | "Live" is a slogan word. |
| 24 | `screens/06-support-and-system.md` | settings_dialog | 5000 ms, light | 5000 ms, least frequent | "Light" is a slogan word. |
| 25 | `screens/06-support-and-system.md` | setup_facts | Carry on without a driver | Continue without a driver | "Carry on" is colloquial; the next button already says "Continue". |
| 26 | none | setup_page | Read only. Useful when a program says nothing arrives. | Read only. Use it to check the data when a program reports that nothing arrives. | "Useful when" is vague. |
| 27 | `screens/06-support-and-system.md` | setup_check_dialog, setup_facts | All set | Ready | Colloquial; "Ready" matches the state words in the copy deck. |
| 28 | `screens/02-workspace.md` | status_bar | Hub running | Engine running | "Hub" is an internal term the copy deck says to avoid in primary text. |
| 29 | none | status_bar | Hub stopped | Engine stopped | "Hub" is an internal term the copy deck says to avoid in primary text. |
| 30 | none | share_wizard | The studio is unchanged. Check the device, then try again. | The setup is unchanged. Check the device, then try again. | Wrong word and lower case; every other message says "setup". |
| 31 | none | share_wizard | The most Studio can share is 32 programs | Studio can share a device with at most 32 programs | Awkward word order. |
| 32 | none | simple_page | Nothing is set up yet, so there is no port to look at. Set something up first. | Nothing is set up yet. Set up a port first. | Second sentence repeats the first. |
| 33 | `screens/06-support-and-system.md` | troubleshoot_dialog | Still stuck? A diagnostic bundle gathers the setup, the log and the environment for whoever helps you. Traffic contents are left out unless you choose to include them. | A diagnostic bundle collects the setup, the log and the environment to send to whoever helps you. Traffic contents are left out unless you include them. | Rhetorical opening question. |
| 34 | `screens/08-simple-mode.md` | setup_kinds | Type to the device and read its replies here, without another program. Good for a first check that it works. | Type to the device and read its replies here, without another program. Use it to check that the device works. | "Good for" is a recommendation phrase. |
| 35 | `07-copy-deck.md` | driver_setup_dialog | Windows only lets a driver make COM ports that other programs can open. Setup installs the Virtual Serial Studio driver and a small helper service that makes ports for you, so administrator rights are needed once, now, and not each time you add a port. | Windows only lets a driver make COM ports that other programs can open. Setup installs the Virtual Serial Studio driver and a small helper service that makes ports for you, so administrator rights are needed once, not each time you add a port. | "Now" adds nothing. |
| 36 | none | port_ready_dialog | Your port "%1" now exists inside Studio. | The port "%1" was created. Only Studio can open it. | "Now exists" repeats the title ("%1 is ready inside Studio"). |
| 37 | none | port_ready_dialog | Your port "%1" now exists, and other programs can open it. | The port "%1" was created. Other programs can open it. | "Now exists" repeats the title. |
| 38 | none | common | The system did not allow this. Nothing was changed. Open Tools, Setup check: it says whether your account is missing the serial device group or AppArmor, SELinux or a sandbox is in the way, and shows the command to fix it. | The system did not allow this. Nothing was changed. Open Tools, Setup check to see whether the cause is a missing group, AppArmor, SELinux or a sandbox. It shows the command to fix it. | One sentence carries three claims; shorten. |
