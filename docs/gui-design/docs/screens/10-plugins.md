# Screens 10: plugins

No mockups yet. The design behind these screens is `docs/plugins.md`; it is under review, and nothing here is built until the owner has agreed it. Dialog anatomy, components and voice are as in screens 01 and `07-copy-deck.md`. Every string below is wrapped in `tr()` with the context named in its section. `<...>` is a value filled at run time. Text that comes from a plugin (its name, description, author, parameter labels, status text) is shown as plain text, exactly as given after control and direction-override characters are removed, and is never passed through `tr()` or used as a format string.

The words: a **plugin** adds something to Studio. It is **installed** when Studio has found it on this computer, **on** when the user has switched it on, and **available** when it is on and can be used now. "Official" means signed by Virtual Serial Studio. Never "safe" or "trusted" as a claim about a plugin, and never that a plugin "is verified": a signature says who made it, not that it is safe. The one place the word appears is the fixed label for a plugin signed by someone else, "Not verified by Virtual Serial Studio" (decided by the owner; `docs/plugins.md`, section 8.1). Never "activate" or "unlock" (the Licence screens' rule).

Decided by the owner: a plugin's fixture types appear in the Advanced toolbox, only while the plugin is installed and on, and are marked as provided by a plugin, with the plugin's name and trust level.

## Where the plugin state comes from
The GUI reads one list, `StudioBackend::plugins()`, a `plugin_view` for each installed plugin, and one signal, `plugins_changed`. `FakeStudioBackend` can return any state, so every variant below has a test. The view is computed by the plugin host (`docs/plugins.md`, sections 5 and 12); the GUI never reads a plugin's files, checks a signature or decides a state. Licence reasons come from the licence service (`screens/09-licence-and-support.md`) and are shown with its sentences, unchanged. `capabilities::plugins` says whether the backend has a plugin host at all; without it the Plugins item is shown disabled with the tooltip "Not available in this build".

Every plugin is in exactly one row of this table. The GUI shows the state, never the reason code alone; the code is in Copy details.

| State | Reason codes (`docs/plugins.md`, section 12) | Chip | Can be switched on | Toolbox |
|---|---|---|---|---|
| Available | running | "On" ok, filled | It is on | Tiles shown |
| Starting | starting, backing off | "Starting" waiting, hollow | It is on | Tiles shown, disabled |
| Off | switched off by the user | "Off" neutral, hollow | Yes | Tiles hidden |
| Needs your decision | untrusted, changed | "Needs your decision" warn, hollow | After the decision | Tiles hidden |
| Blocked by your administrator | policy, sandbox_insufficient with no override allowed | "Blocked" neutral, hollow | No | Tiles hidden |
| Needs more isolation | sandbox_insufficient where the user may override | "Not isolated" warn, hollow | After "Run without isolation..." | Tiles hidden |
| Not used | shadowed, reserved_id | "Not used" neutral, hollow | No | Tiles hidden |
| Needs a licence | not_licensed | "Needs a licence" warn, hollow | It stays on; it runs when the licence allows | Tiles shown, disabled |
| Stopped after failing | quarantined, start_failed, unresponsive, misbehaving | "Stopped" failed, cross | It is on; **Start again** | Tiles shown, disabled |
| Needs a newer Studio | api_incompatible, capability_missing, unsupported manifest version | "Not compatible" neutral, hollow | No | Tiles hidden |
| Can't be used | manifest_invalid, layout_invalid, signature_invalid, revoked, expired | "Can't be used" failed, cross | No | Tiles hidden |
| Not installed | named by a setup, not found on this computer | "Not installed" neutral, hollow | No | Tiles hidden |

A chip is text plus dot shape, never colour alone.

## Menu and entry points
Tools menu, after "Setup check": **Plugins...**. It is in the action registry, so Search actions (Ctrl+K) finds it as "Plugins". No shortcut key. Shown in Simple and Advanced mode.

Other ways in: the "Plugin" mark on a toolbox tile, a port or a link (its menu has "Show in Plugins"); the notice banners below; the Licence dialog's row for an official plugin (a link "Plugins").

## Plugins (width 860)
Context `PluginsDialog`. Opens: Tools, Plugins. Title "Plugins". Sub "Plugins add fixtures, generators, link behaviours and connections to other tools. Each one runs as its own program, apart from Studio."

Header chip: "<n> on" (`%n`; ok) or, when any plugin is in Needs your decision, Stopped after failing or Can't be used, "<n> need attention" (`%n`; "1 needs attention"; warn).

Top NoteBanner, only one, in this order of priority:
- Policy file could not be trusted (warn): "A policy file was found on this computer and could not be trusted, so only official plugins will run." Action "Copy details".
- Official only by policy (info): "Your administrator allows only official plugins on this computer."
- Per-user plugins off by policy (info): "Your administrator installs the plugins on this computer. Plugins in your own plugin folder are not used."
- No licence and at least one official plugin installed (info): "Official plugins need a Pro licence." Action "Licence".

### The list (left, 320 wide)
A list with the accessible name "Installed plugins". Filter field above it, placeholder "Filter plugins". One row for each plugin, sorted with the rows that need attention first, then by name.

Row: the plugin's name (600), second line "<trust line>, version <version>" (12, `ink3`), the state chip on the right, and a Switch at the far right. A copy that is not used (see "Installed for" below) is its own row, directly under the copy that is used, indented, with no Switch.

Trust line, one of:
- "Official"
- "Signed by <author name>"
- "Unsigned"
- "Signed by an author you have not approved"

Switch: accessible name "<plugin name>", label text "On" or "Off". It is disabled, with the row's reason as its tooltip and accessible description, in the states where the table above says the plugin cannot be switched on. Switching on a plugin in Needs your decision opens the decision dialog first. Space toggles.

Row accessible name: "<plugin name>, <trust line>, <state>", for example "Modbus fixture, Official, On".

Empty list: an EmptyState, title "No plugins are installed", text "Plugins are folders that you or your administrator put in the plugin folder. Studio does not download anything." Button "Open the plugin folder". With a filter and no match: "No plugin matches <text>."

Under the list: "Add a plugin..." (quiet), "Import a key..." (quiet; opens "Import an author's key" below), "Open the plugin folder" (quiet; opens the per-user plugin folder in the file manager) and "Check again" (quiet; rescans; toast "Checked. <n> plugins found." with `%n`).

"Add a plugin..." opens a folder chooser with the title "Choose a plugin's folder". Studio copies the folder into the per-user plugin folder and selects the new row, which is in Needs your decision. Nothing runs. Toast "Added for you only. Review it before it runs." A folder with no description file: "That folder is not a plugin. A plugin's folder holds a file named plugin.json." A plugin already installed for you: "<plugin name> is already installed for you. Remove it first to add a different copy."

Studio never asks for administrator rights. A muted line under the buttons, with a CopyButton for the path: "To install a plugin for everyone on this computer, an administrator puts its folder in <machine plugin folder>."

When the administrator has turned per-user plugins off, "Add a plugin..." and "Open the plugin folder" are disabled with the tooltip "Your administrator installs the plugins on this computer."

### The detail (right)
For the selected plugin. Heading: the plugin's name in title type, then the "Plugin" mark and the state chip.

**Why and what to do.** A NoteBanner directly under the heading in every state but Available and Off. One sentence of why, one of what to do, and the action:

| State | Kind | Text | Actions |
|---|---|---|---|
| Starting | info | "Starting." or, after a failure, "It stopped unexpectedly. Starting again in <n> seconds (attempt <a> of 5)." (`%n`) | "Start now" |
| Needs your decision, first time | warn | "You have not decided whether to run this plugin." | "Review and decide" |
| Needs your decision, changed | warn | "This plugin changed since you approved it. Review it again before it runs." | "Review and decide" |
| Blocked, official only | info | "Your administrator allows only official plugins. This one is not official." | none |
| Blocked, named | info | "Your administrator has blocked this plugin." | none |
| Blocked, protection | info | "Your administrator requires more isolation than this computer can give this plugin." | "Copy details" |
| Needs more isolation, override not allowed | info | "Studio cannot keep this plugin apart from your files and other programs on this computer, so it will not run. Only your administrator can allow it." | "Copy details" |
| Needs more isolation | warn | "Studio cannot keep this plugin apart from your files and other programs on this computer, so it will not run." | "Run without isolation...", "Copy details" |
| Not used, same plugin for everyone | info | "The copy installed for everyone is used. This copy, installed for you only, is not." With a newer version here: "This copy is newer (version <version>), and it is still not used. Ask your administrator to update the copy for everyone." | "Show the copy that is used" |
| Not used, different signer | warn | "Another plugin with the same identifier is installed, <trust line>. This one, <trust line>, is not used. They are not the same plugin." | "Show the copy that is used", "Copy details" |
| Not used, reserved name | danger | "Its identifier is one that only official plugins may use, and it is not signed by Virtual Serial Studio. It will not run." | "Copy details" |
| Needs a licence | warn | The licence service's reason sentence, then "Its settings are kept, and it runs again when a licence allows it." | "Licence" |
| Stopped after failing | danger | "It stopped <n> times in 10 minutes, so Studio stopped starting it." (`%n`) or "It did not answer, so Studio stopped it." or "It sent more than it is allowed to (<limit name>), so Studio stopped it." | "Start again", "Show log" |
| Needs a newer Studio | info | "This plugin needs a newer version of Studio. This is version <version>." or "This plugin needs something this build does not have: <capability name>." | none |
| Needs an older plugin | info | "This plugin was made for an older version of Studio. Ask its author for a newer one." | none |
| Can't be used, manifest | danger | "Its description file is not valid, so Studio cannot tell what it is." | "Copy details" |
| Can't be used, signature | danger | "Its signature does not match its files. They may have been changed. Get the plugin again from its author." | "Copy details" |
| Can't be used, withdrawn | danger | "This plugin was withdrawn by Virtual Serial Studio. It will not run." | none |
| Can't be used, expired | warn | "This plugin expired on <date>. Look for a newer version." | Unofficial plugins only: "Run until Studio closes" |
| Not installed | info | "A setup uses this plugin, and it is not installed on this computer. Its parts of the setup are kept." | none |

There is never a way to run a plugin whose signature failed or that was withdrawn.

**Definition list.** Rows that have no value are left out.

| Row label | Value |
|---|---|
| "What it adds" | One line for each: "Fixture: <type name>", "Generator: <type name>", "Link behaviour: <type name>", "Connection: <type name>" |
| "Description" | The plugin's description |
| "Version" | `<version>` in the fixed font |
| "Made by" | `<author name>`; for an official plugin "Virtual Serial Studio" |
| "Signature" | One of the sentences below |
| "Fingerprint" | The first 16 characters of the plugin's hash in the fixed font, with a CopyButton (toast "Fingerprint copied.") |
| "Expires" | `<date>` |
| "Licence" | "Included in your licence", or the licence service's reason sentence |
| "Timing" | "Timed by Studio", "In the path of the data", or "Beside the data" (see below) |
| "Isolation" | "Full", "Partial: <what is missing>", "None on this computer", or, after the override below, "None. You allowed this on <date>." with a quiet button "Undo" |
| "Installed for" | "Everyone on this computer", "You only", or "Came with Studio" |
| "Folder" | The path in the fixed font, with a CopyButton and a quiet button "Show in folder" |

Signature sentences:
- Official: "Signed by Virtual Serial Studio." with the Virtual Serial Studio badge.
- Third-party, approved: "Signed by <author name>. Not verified by Virtual Serial Studio."
- Signed with a key that is not on this computer: "Signed with a key that Studio does not have (<key id>), so the signature could not be checked." with a quiet button "Import the author's key...".
- Unsigned: "Not signed. Studio cannot tell who made it or whether it was changed."

A muted line under the Signature row, always: "A signature shows who made a plugin and that its files are unchanged. It does not show that the plugin is safe."

When the plugin runs with a program that is not part of it: a warn line, "Uses the <runtime name> on this computer. Those files are not covered by the signature."

Timing sentences, as tooltips and accessible descriptions:
- "Timed by Studio": "Studio carries out this plugin's rules itself, so its timing is exact and it keeps working if the plugin stops."
- "In the path of the data": "Data on a link waits for this plugin, for up to <n> microseconds each time."
- "Beside the data": "This plugin watches and acts, and data never waits for it."

**Permissions.** A section headed "What it may do", a list with one row for each permission the plugin declares: a check mark or a dash, the permission sentence, and for one the user turned down the muted word "Not allowed". The plugin's own reason, when it gives one, is a second line in quotes: "The plugin says: “<reason>”". Permission sentences:

| Permission | Sentence |
|---|---|
| `ports.read` | "See your ports and links, their state and their counters." |
| `traffic.tap` | "Read the data that passes through the ports and links you use it on." |
| `ports.write` | "Send data from the ports you use it on." |
| `topology.modify` | "Add, remove and connect ports and links." |
| `modifier.inline` | "Hold, change or drop data on the links you use it on." |
| `faults` | "Cause line errors and disconnections, and set timed replies." |
| `serial.devices` | "See the list of serial devices on this computer." |
| `fs.read` | "Read files in a folder you choose." |
| `fs.write` | "Write files in a folder you choose." |
| `net.listen` | "Accept connections on <address>." With a loopback address: "Accept connections from programs on this computer, on <address>." |
| `net.outbound` | "Connect to <hosts>." |
| `process.spawn` | "Start other programs." |
| `bundle.section` | "Add a section to a support bundle. You see it and choose whether to include it." |

`traffic.tap`, `modifier.inline`, `net.listen`, `net.outbound`, `fs.write` and `process.spawn` are drawn as amber rows with the warning icon. Button under the list: "Change what it may do..." (opens the decision dialog on its permissions step; disabled for a plugin an administrator installed, with the tooltip "Set by your administrator").

**Where it is used.** A section headed "Used in this setup": one row for each instance, "<type name> on <port or link name>", with a quiet button "Show" that selects it on the canvas. None: "Not used in this setup."

**Settings.** Only when the plugin declares settings of its own (not of an instance): a form built from its parameters (see "Settings forms" below).

**Log.** A disclosure "Recent messages (<n>)". A read-only fixed-font view with the accessible name "Messages from <plugin name>", the last 200 lines, each with its time. Button "Copy". Muted line: "These lines come from the plugin."

### Footer
Left: quiet "Remove plugin..." (plugins installed for you only; disabled for a plugin installed for everyone with the tooltip "Installed by your administrator", and for one that came with Studio with the tooltip "Came with Studio"). Right: "Copy details" (quiet), Close (primary).

"Remove plugin..." asks first: title "Remove <plugin name>?", text "Studio moves the plugin's folder to the bin. Setups that use it keep their parts, which stop working until the plugin is installed again." Check box "Also delete its saved data" (off). Buttons Cancel (default), "Remove plugin" (danger).

Copy details gives the plugin id, version, state, reason code, fingerprint, signer key id, who it is installed for, isolation level and the host's message. Never file contents, never a setting marked secret.

Acceptance: every state in the tables can be shown from `FakeStudioBackend`; a state change updates the open dialog without closing it and is announced once as a polite live region; the list, the detail and the footer are reachable in that order by keyboard, and F6 moves between the list and the detail; every switch and button has a 24 px target; the dialog works at 200 percent text with the detail scrolling inside the dialog; both themes from tokens only; no string is built by joining translated fragments; no network call.

## Decide about a plugin (width 640)
Context `PluginDecisionDialog`. Opens from "Review and decide", from switching on a plugin that needs a decision, and from "Change what it may do...". Modal. Initial focus is Cancel. No button is the default except Cancel for anything but an official plugin.

Title, by trust level:
- Official: "Switch on <plugin name>?"
- Signed: "Run a plugin from <author name>?"
- Unsigned: "Run an unofficial plugin?"

Top NoteBanner:
- Official (info): "Signed by Virtual Serial Studio."
- Signed, author approved before (info): "Signed by <author name>, whose plugins you have approved before. Not verified by Virtual Serial Studio."
- Signed, author not approved (warn): "Signed by <author name>. Not verified by Virtual Serial Studio. Only run it if you know who that is."
- Unsigned (danger): "Unofficial plugin. It can do anything Studio can." Second line: "Studio cannot tell who made it or whether it was changed."

A changed plugin adds, above the list (warn): "This is a different version from the one you approved." and, when it asks for more, "It asks for more than before:" with the new permission sentences.

Body: the plugin's name, version and fingerprint (fixed font); "What it adds" as in the detail; then the heading "It asks to:" and the permission list. A permission the plugin marks as needed is a row with a check mark and the muted word "Needed". One it marks as optional is a check box, ticked by default, that the user can untick. The amber rows are amber here too.

Isolation line, by what this computer can give:
- "Studio runs it apart from your files and other programs."
- "Studio can only partly keep it apart on this computer: <what is missing>."
- "Studio cannot keep it apart on this computer. It runs with your own access to files and the network." (official plugins only)
- For any other plugin that cannot be kept apart at all: "Studio cannot keep it apart on this computer, so it will not run here." The run button is then "Decide anyway", which stores the decision and leaves the plugin in Needs more isolation.

A plugin signed with a key that is not on this computer is decided as an unsigned plugin is: the title and the banner are the unsigned ones, with a third line in the banner, "It is signed with a key that Studio does not have (<key id>), so the signature could not be checked." and a quiet button "Import the author's key..." beside it. Approving an author is done by importing the author's key (below), never by a tick in this dialog.

Buttons: Cancel; then "Switch on" (official), "Run this plugin" (signed) or "Run anyway" (unsigned, danger role). Cancel leaves everything unchanged and the plugin off.

Acceptance: the dialog cannot be answered by Enter alone for a signed or unsigned plugin (Cancel is the default); every permission sentence is read by a screen reader with its state ("Needed", "ticked", "not ticked"); the fingerprint shown equals the one in the Plugins dialog; a decision is stored for that fingerprint only.

## Import an author's key (width 560)
Context `PluginKeyImportDialog`. Opens from "Import the author's key..." on a plugin, and from "Import a key..." (quiet) under the list in Plugins. Decided by the owner: an author publishes a small public key file; the user imports it here and compares its fingerprint with what the author published; only then is the key approved. A key is never approved because it lies in a plugin's own folder.

Step 1 is a file chooser, title "Choose an author's public key file", filter "Plugin key files (*.vsskey.json)". A file that is not a key file: "That file is not a plugin key file." A key id that is already in use: "A key with the id <key id> is already approved. An id cannot change its key." or, for an official or administrator's key, "The id <key id> belongs to a key that is already on this computer."

Step 2 is the dialog. Modal. Initial focus and default are Cancel. Title "Approve the key of <author name>?". Sub "Plugins signed with this key will be shown as signed by <author name>. Not verified by Virtual Serial Studio."

Body, a definition list:

| Row label | Value |
|---|---|
| "Author" | `<author name>`, plain text as the file gives it |
| "Key id" | `<key id>` in the fixed font |
| "Fingerprint" | Sixteen hex characters in groups of four, in the fixed font at the large size, with a CopyButton (toast "Fingerprint copied.") |
| "File" | The path in the fixed font |

Under the list, a NoteBanner (warn): "Compare the fingerprint with the one the author published, on their website or in a message from them. If they differ, do not approve the key."

When the file was chosen from inside a plugin's folder, a second NoteBanner (warn): "This key file came with the plugin. Anyone who changed the plugin could have changed this file too, so compare the fingerprint with one from somewhere else."

Check box "The fingerprint is the one the author published" (off). "Approve this key" (primary role, not the default) is disabled until it is ticked. Buttons: Cancel, "Approve this key".

Afterwards: toast "The key of <author name> is approved." Plugins signed with it are checked again at once; one whose signature holds becomes "Signed by <author name>" and still needs its own decision before it runs. One whose signature does not hold with the key becomes Can't be used, with the signature sentence.

The detail of a signed third-party plugin gains a row "Signing key": "<key id>, approved on <date>" with a quiet button "Remove this key...". Removing asks first: title "Remove the key of <author name>?", text "Plugins signed with this key stop running and ask again.", buttons Cancel (default), "Remove key" (danger).

Studio never opens this dialog by itself, never fills in the tick, and never looks for key files in plugin folders.

Acceptance: the dialog cannot be answered by Enter alone; the fingerprint is read by a screen reader character by character, in its groups; a key file with a control character or a direction mark in the name is refused; the second banner appears exactly when the chosen file lies inside a plugin root; approving a key never switches a plugin on.

## Run a plugin without isolation (width 520)
Context `PluginNoIsolationDialog`. Opens from "Run without isolation...". Modal. Initial focus and default are Cancel. It is offered only where `docs/plugins.md`, section 9.2, allows the override: Studio installed for one user, or an administrator's policy that allows it.

Title "Run <plugin name> without isolation?". NoteBanner (danger): "It will run with your own access to your files, the network and other programs." For an unsigned plugin, a second line: "Studio cannot tell who made it or whether it was changed."

Body: the plugin's name, version and fingerprint (fixed font), then "This applies to this version of this plugin only. A changed plugin asks again."

Check box "I understand that Studio is not keeping this plugin apart" (off). "Run without isolation" (danger role) is disabled until it is ticked. Buttons: Cancel, "Run without isolation".

Afterwards the Isolation row says so and offers Undo, and the plugin's row and every part it provides carry the trust line unchanged; nothing presents the plugin as safer than it is.

Acceptance: the dialog cannot be answered by Enter alone; it is never shown on an install for everyone without the policy; the override is stored for that fingerprint only and Undo removes it and stops the plugin; a screen reader hears the banner before the buttons.

## Toolbox (Advanced mode)
Context `Toolbox`, extending `screens/07-canvas-editor.md`. A third group under **Ports** and **Bundles**, with the group label **Plugins**. The group is absent when it would be empty. One tile for each fixture type of each plugin that is installed and on, ordered by plugin name, then type name.

A tile is the same 64 by 56 tile: the fixture icon, the type's name as its label (elided in the middle when it does not fit), and the **plugin mark**: the `plugin` icon from `IconProvider`, 12 px, in the tile's top right corner, in `ink3`. The mark is a shape, so it reads without colour.

- Accessible name: "<type name>, plugin". Accessible description: "Fixture from the plugin <plugin name>. <trust line>." then the type's description.
- Tooltip, three lines: the type's name; "From the plugin <plugin name>"; the trust line.
- Enter, Space or a drag opens the Add sheet as for a built-in fixture, titled "Add <type name>", with the type's settings form under the usual fields and the muted line "Provided by the plugin <plugin name> (<trust line>)."

Recommendation, and the rule: **hide what the user switched off or cannot switch on; show disabled, with the reason, what the user switched on and cannot use right now.**

| Plugin state | Tile | Reason, as tooltip and accessible description |
|---|---|---|
| Available | Enabled | none |
| Starting | Disabled | "<plugin name> is starting." |
| Needs a licence | Disabled | The licence service's reason sentence |
| Stopped after failing | Disabled | "<plugin name> has stopped. Open Plugins to start it again." |
| Off, Needs your decision, Blocked, Needs a newer Studio, Can't be used, Not installed | Hidden | |

Why: a tile for a plugin the user never switched on, or was told they may not run, is clutter and an advertisement; the Plugins dialog is where those are explained. A tile that was there yesterday and is gone today, because a licence ended or the plugin crashed, looks like a fault in Studio, so it stays, disabled, and says why. This is the rule of `08-engine-gaps.md` (never hidden without a reason once the user has it), applied to what the user has chosen to have.

A disabled tile still takes focus, so its reason can be read, and its context menu has "Show in Plugins". In the command palette, a hidden type is absent and a disabled type is listed with its reason, as "Needs Advanced mode" items are.

Generators and link behaviours from plugins are not toolbox tiles: they are added from a port's or a link's menu ("Add a generator", "Add a link behaviour"), where the same rule applies to the plugin entries, under a separator and the heading "Plugins".

Acceptance: switching a plugin off removes its tiles at once, and on returns them, without restarting; a disabled tile's reason is announced; the group label follows the group; the rail's keyboard order is Ports, Bundles, Plugins.

## A plugin's parts on the canvas and in the lists
Context `PluginMark`. Anything a plugin provides carries the plugin mark and says whose it is.

- **Canvas node of a plugin fixture**: the fixture card with the plugin mark beside its icon, and under the name the line "<type name>" (12, `ink3`). StatusChip "Fixture" as for any fixture.
- **Link with a link behaviour**: the plugin mark at the middle of the link. Accessible name of the link gains ", with <type name> from <plugin name>".
- **Ports list and Inspector**: a row "Provided by" with "<type name>, from the plugin <plugin name> (<trust line>)" and the plugin's state chip; a quiet button "Show in Plugins".
- The context menu of each gains "Show in Plugins".

### When the plugin is not available
The setup is never changed and nothing is removed. The port stays a port: it keeps its name, its place on the canvas, its links, its settings and its plugin settings. It stays open as a plain fixture that says nothing, so the programs and links around it keep working.

- The node is drawn in the disabled style (the tokens that disabled controls use) with the chip "Plugin not available" (neutral, hollow) in place of "Fixture". Accessible name: "<port name>, <type name>, plugin not available".
- The Inspector shows a NoteBanner with the reason and the action from this table:

| Plugin state | Kind | Text | Action |
|---|---|---|---|
| Off | info | "This fixture comes from the plugin <plugin name>, which is switched off. The port stays open and sends nothing." | "Switch on" |
| Needs your decision | warn | "This fixture comes from the plugin <plugin name>, which is waiting for your decision. The port stays open and sends nothing." | "Review and decide" |
| Blocked | info | "This fixture comes from the plugin <plugin name>, which your administrator has blocked. The port stays open and sends nothing." | "Show in Plugins" |
| Needs a licence | warn | "This fixture comes from the plugin <plugin name>. " then the licence service's reason sentence, then " The port stays open and sends nothing." (one string with a placeholder for the sentence) | "Licence" |
| Stopped after failing, fixed replies | warn | "The plugin <plugin name> has stopped. This fixture still sends its fixed replies. Anything the plugin worked out itself has stopped." | "Start again" |
| Stopped after failing, no fixed replies | danger | "The plugin <plugin name> has stopped, so this fixture sends nothing." | "Start again" |
| Needs a newer Studio, Can't be used | info | "This fixture comes from the plugin <plugin name>, which cannot run in this version of Studio. The port stays open and sends nothing." | "Show in Plugins" |
| Not installed | info | "This fixture needs the plugin <plugin name>, which is not installed on this computer. The port stays open and sends nothing." | "Show in Plugins" |

- The fixture's plugin settings are shown read-only, with the muted line "These settings are kept. They apply again when the plugin is available."
- A link behaviour whose plugin is not available: the link passes data unchanged, the plugin mark on it is drawn in the disabled style, and the Inspector says "The link behaviour <type name> is not running, so data on this link passes unchanged." with the same action. If the behaviour was set to stop data when the plugin does not answer: "…so data on this link is being dropped." and the link shows its fault state.
- A generator whose plugin is not available stops. The Inspector says "The generator <type name> is not running."

**Opening a setup.** Info NoteBanner at the top of the window, in the place of the Licence notices and below them, once for each file opened: "This setup uses plugins that are not available: <names>. Those parts are kept and do not run." Action "Plugins", "Dismiss". When every one is an official plugin missing only a licence, the Licence screens' banner is shown instead, and not both.

**Saving.** Saving keeps every plugin part exactly as it was read, whether or not the plugin is installed: the type, the plugin's id and the settings. Studio never offers to remove them and never removes them on save. A setup made on a computer with a plugin opens on one without it, and opens again, unchanged, on the first.

The plugin part is written in the setup file itself, as a list named `plugins` on the port or the link (decision 13 of `docs/plugins.md`, section 7.8). There is no second file.

**Removing.** The user can remove such a port or behaviour like any other. Nothing else removes it.

When the plugin becomes available again, its parts start again with their kept settings, without a prompt, and a toast says "<plugin name> is available. <n> parts of this setup are running again." (`%n`; "1 part of this setup is running again.").

Acceptance: a setup with a fixture from each unavailable state opens, shows the right chip and banner, saves, and reads back byte for byte the same; no state removes or renames a port; a screen reader hears the state on the node; the disabled style still meets the contrast rule for text.

## Settings forms
Context `PluginSettingsForm`. One generic form builds every plugin's settings from the parameters its description file declares (`docs/plugins.md`, section 7.8). Plugin code never draws anything.

- One row for each parameter, in the declared order: its label, its control, its unit, and its help text as a muted line. The label is the control's accessible name; the unit is part of it ("Reply delay, milliseconds").
- Controls by type: check box; spin box; combo box; line edit; hex field; a spin box with a unit; a choice among existing ports or links; a path field with "Browse..."; a hidden field with a "Show" toggle for a secret.
- A value outside its limits marks the field `invalid` with the muted line "Enter a value from <min> to <max>." or "Use at most <n> characters." (`%n`) or "Use hexadecimal bytes, such as 02 31 03."
- A value the plugin turns down keeps the old value and shows the plugin's reason under the field: "The plugin says: “<reason>”".
- Buttons, where the form is its own dialog: "Restore defaults" (quiet), Cancel, "Apply" (primary).

Acceptance: a form of every parameter type is reachable and operable by keyboard alone, in both themes, at 200 percent text; each control has a 24 px target and a visible focus ring; no plugin text is interpreted as rich text.

## Plugin notices (main window)
Context `PluginNotice`. NoteBanners at the top of the window, in the same place as the Licence notices and below them. One at a time. Never modal, never taking focus, announced politely, once.

| Event | Kind | Text | Actions |
|---|---|---|---|
| A plugin that is used in this setup stopped after failing | warn | "The plugin <plugin name> has stopped. Parts of this setup that use it are not running." | "Plugins", "Start again", "Dismiss" |
| A link behaviour did not answer in time, passing | warn | "<plugin name> did not answer in time. Data on <link name> is passing unchanged." | "Plugins", "Reset", "Dismiss" |
| A link behaviour did not answer in time, dropping | danger | "<plugin name> did not answer in time. Data on <link name> is being dropped." | "Plugins", "Reset", "Dismiss" |
| A plugin was withdrawn while it ran | warn | "<plugin name> was withdrawn by Virtual Serial Studio and has been stopped." | "Plugins", "Dismiss" |
| A new plugin was found | info | "A new plugin was found: <plugin name>. It will not run until you decide." | "Review and decide", "Dismiss" |
| A plugin changed | warn | "<plugin name> changed since you approved it and has been stopped." | "Review and decide", "Dismiss" |

Status bar, after an action: "<plugin name> is on.", "<plugin name> is off.", "<plugin name> started again."

## A plugin is reading traffic (status bar)
Context `PluginTrafficChip`. While any plugin records or reads the data of a port or link, a StatusChip in the status bar, left of the edition chip: "Recording" (warn, filled dot) for a recording, "Reading data" (neutral, filled dot) for a live view. It is a button. Accessible name: "<plugin name> is recording data on <n> ports. Open Plugins." (`%n`; "on 1 port") or "<plugin name> is reading data on <n> ports. Open Plugins." With more than one plugin: "<n> plugins are reading data. Open Plugins." The chip is there for as long as the reading goes on and cannot be dismissed. Data is never read by a plugin without it.

## New assets and components
- Icon `plugin` in `docs/gui-design/icons/` (a puzzle piece outline in the set's stroke style), loaded through `IconProvider`. To be drawn with the first GUI slice.
- No new component: StatusChip, Switch, NoteBanner, EmptyState and the definition list are reused. The chip kinds used are those `04-components.md` already has.

## Accessibility notes for all of the above
Chips and marks carry text or shape, never colour alone. State changes are polite live regions and are announced once. Initial focus: the list in Plugins, Cancel in the decision dialog and in "Remove <plugin name>?". All of it works in both themes from tokens only, with icons from `IconProvider`, targets of at least 24 px, a visible focus ring, and 200 percent text.
