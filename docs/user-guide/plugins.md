# Plugins

**Tools → Plugins...** lists the plugins installed on this computer. A plugin
adds fixtures, generators, link behaviours or connections to other tools, and
each one runs as its own program, apart from Studio. Studio never downloads a
plugin. A plugin is a folder that you or your administrator put in a plugin
folder.

Studio runs a plugin you have switched on as a program of its own, apart from
Studio, and stops it when you switch it off or close Studio. If it stops
unexpectedly Studio starts it again after a short wait (1, 2, 4 and then 8
seconds), and after five failures in ten minutes it stops trying and says so;
**Start again** tries once more. How far this computer can keep a plugin apart
from your files and other programs is shown for each plugin (**Isolation**). On
Windows Studio runs a plugin in its own sandbox. On Linux and macOS Studio does
not keep a plugin apart yet: an official plugin runs without that and the page
says so. Where it cannot keep a plugin that is not official apart, the plugin
does not run unless you allow it for that version with **Run without
isolation...**.

## The list and the detail

The list is on the left, with a box to filter it by name. A row shows the
plugin's name, who stands behind it, its version, its state and a switch. The
rows that need attention are first. A chip at the top says how many plugins are
on and how many need attention. Choose a row to see its detail on the right. F6
moves between the list and the detail. Under the list are **Add a plugin...**,
**Import a key...**, **Open the plugin folder** and **Check again**, which looks
through the plugin folders again, and under the detail are **Remove plugin...**,
**Copy details** and **Close**. The page shows the machine plugin folder too,
with a button that copies its path.

The line under the name says who stands behind the plugin:

| Line | Meaning |
|---|---|
| Official | Signed by Virtual Serial Studio. Only these show the Virtual Serial Studio badge |
| Signed by *name* | Signed with a key that you or your administrator accepted. Not verified by Virtual Serial Studio |
| Signed by an author you have not approved | Signed with a key that is not on this computer, so the signature could not be checked |
| Unsigned | Nobody signed it. Studio cannot tell who made it or whether it was changed |

A signature shows who made a plugin and that its files are unchanged. It does
not show that the plugin is safe. Everything a plugin says about itself, such as
its name, its description and its messages, is shown as plain text and is never
used to decide what Studio shows about it.

The state of a plugin is one of On, Starting, Off, Needs your decision,
Blocked, Not isolated, Not used, Needs a licence, Stopped, Not compatible,
Can't be used and Not installed. Under the name of the plugin the page says why
and what you can do about it.

## What the detail shows

The detail of a plugin has, from the top: what it adds (a fixture, a generator,
a link behaviour or a connection), its description, version and maker, and what
the running plugin says of itself (**Status**). **Signature** says who stands
behind it, with a **Virtual Serial Studio** badge for an official plugin only.
**Signing key** shows the key's id and the day you accepted it, with **Remove
this key...**. **Fingerprint** has a button that copies it. **Expires**, for a
plugin that has an end date, and **Licence**, for an official plugin, say
whether your licence includes it and, when it does not, why. **Timing** reads
**Timed by Studio** (Studio carries out the plugin's rules itself),
**In the path of the data** (data on a link waits for the plugin) or **Beside the
data** (the plugin watches and acts, and data never waits for it). **Isolation** says how
far it is kept apart. **Installed for** is everyone on this computer, you only or
came with Studio, and **Folder** has **Show in folder** and a button that copies
the path. A plugin whose licence check failed says so (**This plugin did not
accept the licence**) and says what the plugin reported.

- **What it may do** lists the permissions you were asked about. One you turned
  down is marked **Not allowed**. **Change what it may do...** opens the
  question again.
- **What it runs** lists what Studio runs of the plugin, such as one generator
  or one decoder, each with its state (Not running, Waiting, Starting, Running,
  Stopping or Refused) and **Remove**. A refused one says why, and shows the
  plugin's own words as plain text. **Add...** has a menu with an entry for
  each thing the plugin adds. An entry is disabled, with the reason in its tool
  tip, when the plugin allows no more of that kind or needs settings that Studio
  cannot ask for yet. When an entry needs a port or a name, Studio asks for it
  first, and **Add** stays disabled until every field has a value. The list is
  kept for the next start.
- **What it found** is shown under a decoder: a table of the newest frames and
  problems with **Time**, **Place**, **Direction**, **What** and **Kind**. The
  words in it come from the plugin.
- **Used in this setup** lists the parts of the open setup that use the plugin,
  each with **Show**.
- **Recent messages** has the lines the plugin wrote to its log, and **Copy**.
  They come from the plugin and are shown as plain text.

## Deciding about a plugin

A plugin that Studio has not seen before, or whose files changed since you
allowed it, needs your decision before it can run. Switch it on, or choose
**Review and decide**. The question lists the plugin's name, version and
fingerprint, what it adds and each permission it asks for. A permission that the
plugin needs is a row. A permission that the plugin marks as optional has a
check box, ticked, that you can untick. The permissions that give a plugin the
most are marked with a warning.

The dialog starts on **Cancel**, and Enter alone cancels. The button that says
yes reads **Switch on** for an official plugin, **Run this plugin** for one signed
by an author whose key you accepted and **Run anyway** for one that is unsigned or signed
with a key you have not accepted. Where this computer cannot keep a plugin that
is not official apart, it reads **Decide anyway**, and the plugin still does not
run until you choose **Run without isolation...**, where your administrator
allows it. Your answer is kept for that version of the
plugin only. If its files change, Studio asks again, says that you allowed an
earlier version, and lists what the new version asks for beyond the one you
allowed. **Change what it may do...** in the detail of a plugin asks again for
the permissions; it is disabled for a plugin your administrator installed.

## Accepting an author's key

An author of plugins publishes a small public key file, which ends in
`.vsskey.json`. Choose **Import a key...** and pick the file. Studio shows the
author's name, the key's id and its fingerprint in groups of four. Compare the
fingerprint with the one the author published on their website or sent you, tick
**The fingerprint is the one the author published** and choose **Approve this
key**. If the key file you chose is inside a plugin's folder, Studio warns you
that anyone who changed the plugin could have changed the file, so compare the
fingerprint with one from somewhere else. Studio never looks for key files in a
plugin's folder, and accepting a key never switches a plugin on: each plugin
still needs its own decision. **Remove this key...** in the detail of a plugin
asks first, then takes the key back, and the plugins signed with it stop running
and ask again.

## Running a plugin without isolation

Studio runs a plugin apart from your files and other programs when this computer
can. When it cannot, a plugin that is not official is shown as Not isolated.
Where your administrator allows it, **Run without isolation...** lets you run
that version of that plugin with your own access to your files, the network and
other programs. The question stays on **Cancel**, and the button **Run without
isolation** works only after you tick **I understand that Studio is not keeping
this plugin apart**. **Undo** in the Isolation row takes it back.

## Plugins in the rest of the window

- **Toolbox (Advanced).** A **Plugins** group under Ports and Bundles has a tile for each fixture type of a plugin that is switched on. A plugin that is starting, that needs a licence or that stopped keeps its tile, disabled, and the tile says why. A plugin that is switched off, waiting for your decision, blocked, not isolated, not used, not compatible or that cannot be used has no tile. A tile of a plugin that is on is also disabled, with the reason in its tool tip, while Studio cannot play a recording as a device, as for a Recorded device tile. A tile's top right corner has the plugin mark, and its menu has **Show in Plugins**.
- **Ports.** A port that a plugin provides says so in the Inspector (**Provided by**). While its plugin is not available the port stays open and sends nothing, the canvas marks it **Plugin not available**, and the Inspector says why and offers the action that helps: **Switch on**, **Review and decide**, **Licence**, **Start again** or **Show in Plugins**. Its settings are kept.
- **Notices.** One notice at a time appears at the top of the window when a new plugin is found, when a plugin changed and was stopped, when a plugin that your setup uses stopped, or when a plugin was withdrawn. A notice never takes the focus and **Dismiss** hides it.
- **Status bar.** While a plugin reads the data of ports or links, a chip reads **Reading data**, or **Recording** when it records. It opens Plugins.
- **Licence.** The row of the official plugins on the Licence page has a **Plugins** link.

## Decoding traffic

In the Advanced view, while a plugin that has a decoder is switched on, the menu
of a port, and of a monitor link, has **Decode as...**. It is disabled, with the
reason in its tool tip, while no decoder can be used. It opens **Decode** and the
name of the port, which asks for the **Protocol**, says which plugin it comes
from and that it will read the data that passes through the port. **Start
decoding** starts it. Decoding only reads: it never changes or delays the data.

**Tools → Decoded traffic...**, also in the Advanced view, opens a window with
what the decoders found, newest first, in a table with **Time**, **Place**,
**Direction** (Received or Sent), **Type** (Frame or Trigger), **Message** and
**What**. **Triggers only** hides the frames. The window keeps at most 5000 rows.
The words in the table come from the plugin.

A plugin that you allowed to can also record the data of ports and links you
name into its own folder, and play a recording of its own onto a port of your
setup. While a plugin reads data, the status bar chip says **Reading data** or
**Recording**.

## When a plugin asks you something

- **A code.** When a running plugin has a pairing code or a one-time password for
  you, Studio shows it once in **Code from a plugin**, with the plugin's name, a label
  under **The plugin says**, the code in large type and a countdown (**Good for**
  a time, then **This code is no longer good. Ask the plugin for a new one.**). Studio keeps it nowhere: not
  in the log, not on the Plugins page and not in a file. **Copy** is the only way
  it reaches the clipboard, and **Close** ends the dialog.
- **A remote control session.** When a plugin asks to use this computer's ports
  for another computer, **Allow remote control?** shows which computer the plugin
  says it is, why, in the plugin's own words, one sentence for each thing the
  session could do (the ones that let the other side see or send data are
  marked with a warning) and how long it would last, and says that you are asked
  again each time. The dialog starts on **Refuse**, closing it is Refuse, and
  **Allow** lets the session start. Nothing is remembered.

## Adding and removing a plugin

**Add a plugin...** asks for a plugin's folder and looks into it first. A
question shows the plugin's version, who made it, what its signature says, its
fingerprint and whether it can be used, and copies nothing until you choose
**Add plugin**. A plugin that cannot be used cannot be added. The copy goes into
your own plugin folder, for you only, and the new row is selected. Nothing runs,
and Studio says to review the plugin before it runs. If the plugin is already installed for
everyone on this computer, or came with Studio, that copy is the one that runs,
so Studio does not add a second one for you and says so. A copy that is not used
says which copy runs instead. The same question shows the command for installing
it for everyone, `vss_cli plugin install <folder> --for-everyone`, as text to
copy: Studio never asks for administrator rights. **Remove plugin...** asks first
and deletes the plugin's folder; it does not go to the bin. Setups that use the
plugin keep their parts, which stop working until the plugin is installed
again. The plugin's saved data is kept unless you tick **Also delete its saved
data**. It is for a plugin installed for you only: a plugin that your
administrator installed or that came with Studio cannot be removed here. An
administrator can also put a plugin's folder in the machine plugin folder, which
the page shows under the list. If your administrator has turned your own plugin
folder off, **Add a plugin...** and **Open the plugin folder** are disabled.

When Studio cannot use a plugin because of its `plugin.json`, the question says
so and adds what is wrong, as Studio's check found it.

## Listing plugins with `vss_cli plugin list`

```bash
vss_cli plugin list [--json] [--root <folder>]...
```

`plugin list` shows the plugins that are installed on this computer. It only
reads. It runs no plugin and changes nothing.
For each plugin it also makes the checks that come before a run, the same ones
`plugin verify` makes, and shows what they found.

It starts with whether Studio is installed for everyone on this computer or for
you only, and with the administrator's policy file, if there is one. Then it
prints one block for each plugin:

```text
com.example.modbus-fixture  1.2.0  Modbus fixture
  Folder:       C:/Users/me/AppData/Local/Virtual Serial Studio/plugins/modbus-fixture
  Installed for: you only
  Fingerprint:  3f9a1c0d2b7e4a61
  State:        found
  Signature:    Not signed. Studio cannot tell who made it or whether it was changed.
  Trust:        Unsigned
  Key:          none
  May run:      no, sandbox_insufficient: ...
  Licence:      none needed
```

- **Folder** is the plugin's own folder.
- **Installed for** is `everyone on this computer`, `you only` or `came with
  Studio`, from the folder the plugin was found in.
- **Fingerprint** is the first 16 characters of a hash of all the plugin's
  files. A change to any file gives a different fingerprint.
- **State** is `found` when the plugin's files are in order, and nothing more.
  Otherwise it is the reason, such as `manifest_invalid`, a colon and what is
  wrong. When two folders hold a plugin with the same id, the copy found
  first counts. The other is `shadowed`, and a `Not used` line names the folder
  of the copy that counts.
- **Signature**, **Trust**, **Key**, **May run** and **Licence** are what the
  checks found. They are explained under `plugin verify` below. A plugin whose
  manifest could not be read has a `Signature` line that says whether there is
  a `plugin.sig`, and none of the others.

A plugin whose `plugin.json` cannot be read is shown by the name of its folder.
A plugin folder that cannot be looked through is named after the plugins, in a
line that starts `Folder` and ends `was not looked through`. A `Note:` line says
something the computer has to say, such as a policy file that could not be
used, and the last line says how many plugins were found (`No plugins found.`,
`1 plugin found.` or `N plugins found.`). Text that comes from a plugin is
printed as plain text, and each byte that is not printable ASCII is shown as
`?`. The JSON keeps valid UTF-8 as it is.

`--json` prints one JSON object and nothing else: `install` (`for_everyone`,
`for_one_user` or `null`), `policy` (`file`, `present`, `used` and `detail`),
`plugins` (one object for each plugin, with `id`, `version`, `name`, `folder`,
`scope`, `fingerprint`, `signature`, `state`, `detail` and `shadowed_by`, and
for a plugin whose manifest was read the members that `plugin verify --json`
has: `signature_state`, `trust`, `trust_line`, `key_id`, `may_run`,
`run_reason`, `run_detail` and `licence`) and `roots_not_read`. A value that is
not known is `null`.

`--root <folder>` looks in the folders you give instead of the usual ones, and
may be repeated. Each folder counts as your own, and the policy file is not
read. Use it to look at a folder of plugins by hand. The `plugin` commands take
the value of an option as `--root <folder>` or as `--root=<folder>`.

The exit code is 0 when the command ran, whatever it found: a plugin with a
problem is a finding, not a failure of the command. It is 2 for a command line
the program does not understand.

## Checking a plugin with `vss_cli plugin verify`

```bash
vss_cli plugin verify <folder> [--allow-unsigned] [--json]
```

`plugin verify` makes the checks that come before a plugin runs, on a plugin
folder, and says what it found. It only reads. It runs nothing.

```text
com.example.modbus-fixture  1.2.0  Modbus fixture
  Folder:       C:/Downloads/modbus-fixture
  Fingerprint:  3f9a1c0d2b7e4a61
  Signature:    Signed by Example Ltd. Not verified by Virtual Serial Studio.
  Trust:        Signed by Example Ltd
  Key:          example-2026
  May run:      yes
  Licence:      none needed
  Install:      can be installed
```

- **Signature** says whether the signature holds and who made it. "Official"
  plugins are signed by Virtual Serial Studio. A signature shows who made a
  plugin and that its files are unchanged. It does not show that the plugin is
  safe.
- **Trust** is `Official`, `Signed by <author>`, `Unsigned` or `Signed by an
  author you have not approved`. The last is a plugin signed with a key that is
  not on this computer, so the signature could not be checked.
- **Key** is the id of the signing key, or `none`.
- **May run** is `yes`, or `no` and the reason code and a line about it, such as
  `expired` or `sandbox_insufficient`. It says what Studio would decide. The
  command itself runs nothing; `plugin run` below runs a plugin.
- **Licence** is `none needed`, `covered`, or `not covered` and why. `plugin
  verify`, `plugin list` and `plugin install` do not read licence files, so for
  an official plugin that needs an edition they say `not checked: this command
  does not read licence files`. Studio does read the licence; see
  [Licences](licences.md).
- **Install** is `can be installed`, `cannot be installed` and the reason, or
  `cannot be installed unless you give --allow-unsigned`.
- **Problem** is a line that appears only when the folder cannot be used as a
  plugin, such as `manifest_invalid`, with what is wrong. A `Note:` line says
  something the computer has to say, as it does for `plugin list`.

The exit code is 0 when the plugin can be installed, 1 when it cannot, and 2 for
a command line the program does not understand. A plugin cannot be installed
when its folder is not a plugin folder, when a file is not what its manifest
lists or the folder holds a file it does not list, when its signature does not
hold, when its id cannot be the name of a folder (such as `con.example`), or
when it can never run: its id is one that only official plugins may use, or it
was withdrawn. A plugin that has expired, or that this computer cannot isolate,
or that the licence does not cover, can be installed; the line `May run` says
what is wrong. A plugin with no signature can be installed only with
`--allow-unsigned`, and so can one signed with a key nobody has accepted,
because nothing shows who made it. The `Install` line says which of the two it
is.

`--json` prints one JSON object: `id`, `version`, `name`, `folder`,
`fingerprint`, `problem` and `detail` (why the folder cannot be used, or
`null`), `signature_state` (`absent`, `invalid`, `valid` or `unknown_key`),
`trust` (`official`, `third_party_signed`, `signed_unknown_key`, `unsigned` or
`null`), `trust_line`, `key_id`, `may_run`, `run_reason`, `run_detail`,
`licence` (`checked`, `covered` and `detail`), `installable` and `install`.

## Installing a plugin with `vss_cli plugin install`

```bash
vss_cli plugin install <folder> [--for-everyone] [--replace] [--allow-unsigned] [--root <folder>]
vss_cli plugin remove <id> [--for-everyone] [--root <folder>]
```

A plugin arrives as a zip file. Studio does not open zip files. Unzip it with
your system's own tool into a folder of its own, look at the folder with
`plugin verify`, and install from that folder. The folder must hold
`plugin.json` and the files it lists, and usually `plugin.sig`.

`plugin install` makes the same checks as `plugin verify`. If they allow it, it
copies the plugin into your plugin folder, under its id, and prints what it did
and what the checks found. It runs nothing from the plugin. If a check fails, it
says which one and writes nothing.

- The folder is measured, and then each file is copied and checked against that
  measurement as it is copied, including `plugin.json` and `plugin.sig`, which
  are the files that were judged. A file that changed while the plugin was being
  looked at or copied stops the install. The copy is checked against the
  plugin's manifest again before it is put in place.
- A copy keeps the execute permission of the owner and the read and execute
  permissions of the group and others from the file it came from. The owner can
  always read and write it and no one else can write it, and the program that
  the manifest names is made executable for its owner (on Linux and macOS).
- The copy is made in a folder called `.vss-work` inside the plugin folder and is
  renamed into place in one step, so a failure leaves the old plugin or the new
  one and never half of each. Studio ignores every name that starts with a dot
  in a plugin folder, so nothing in `.vss-work` is taken for a plugin. The
  folder is removed when the install ends. If an install is cut off, the next
  install removes what it left.
- A link, or anything that is not a plain file or folder, in the plugin folder
  stops the install.
- A plugin with the same id that is already installed stops the install unless
  you give `--replace`. With it, the old copy is put aside, the new one is put in
  place and the old one is removed. If putting the new one in place fails, the
  old one is put back. If the old one cannot be removed, the command says so, with
  the folder that holds it, and exits with 1; the new plugin is installed.
- `--allow-unsigned` is needed for a plugin that has no signature.
- `--for-everyone` installs into the folder of everyone on this computer. Run
  the command from a prompt that may write there. Studio does not ask for more
  rights. Before it writes anything it looks at the folder, or, when the folder
  is not there, at the nearest folder above it that is, and stops with a message
  when that can be changed by someone who is not an administrator or when it
  cannot be looked at. When the check passes, it makes the plugin folder if it is
  not there. `plugin remove --for-everyone` stops in the same way.
- `--root <folder>` installs into the folder you give. The policy file is read
  as it is for `plugin verify`.

The exit code is 0 when the plugin was installed, 1 when it was not and 2 for a
command line the program does not understand. `plugin remove` uses the same
codes, and 0 means the plugin's folder was removed.

`plugin remove` removes the folder of an installed plugin. It takes the id of
the plugin, such as `com.example.modbus-fixture`, and refuses anything that is
not a plugin id or a folder that is not a plugin. It does not touch the plugin's
data. It runs nothing.

## Running a plugin with `vss_cli plugin run`

```bash
vss_cli plugin run <id> [--root <folder>]... [--data <folder>] [--trust <file>] [--seconds <n>] [--json] [--instance <kind>:<type>[=<params>]]...
```

`plugin run` runs one installed plugin the way Studio's plugin host runs it, and
prints what happens: the plugin starts, says hello, is confined and stops. Use
it to see a plugin work, from a script, or from the tests of a plugin of your
own. It runs the plugin until you press `Ctrl-C`, or until the plugin cannot run
any more. With `--seconds <n>` it stops the plugin after `n` seconds with the
plugin running. It always stops the plugin in order: the plugin is asked to
stop, the command waits until it has gone, and only then ends.

It makes no decision about trust. A plugin runs only when the file given with
`--trust` holds your decision about this exact copy of it, as Studio keeps it.
A plugin that needs your decision is shown as `needs your decision`, and the
command ends with exit code 1. Decide in Studio. The command never writes the
trust file.

```text
state: starting
state: running; process 4120; sandbox partial (not applied: system_calls, user_interface)
output: listening
log info: ready to serve
state: stopping; process 4120; sandbox partial (not applied: system_calls, user_interface)
state: stopped
```

It prints one line each time the state of the plugin changes, and one line for
each new line of the plugin's output or log:

- `state:` is `starting`, `running`, `stopping`, `stopped`, `waiting to start
  again in <n> s (failure <k> of <m>)`, `stopped after failing`, `off`, `needs
  your decision`, `blocked by the administrator`, `needs a licence` or `cannot
  be used`. After it come the `reason` code and the `detail` when there is one,
  the `process` id when the plugin has one, `ended with code <n>` (or `ended
  abnormally with code <n>`) when the process has ended, the `sandbox` level and
  the parts of the sandbox that were not applied, and the `status` the plugin
  last gave. A plugin that fails is started again after a wait. After `<m>`
  failures it is not started again.
- `output:` is a line the plugin wrote to its standard output or standard error.
- `log` and a level, such as `log info:`, is a message the plugin sent to the
  log.
- `note:` is a line the computer has to say, such as a policy file that could
  not be used.
- `secret from the plugin;` is a code or a one-time password that the plugin
  wants you to read, with its label and how long it is good for. Studio shows
  it in a dialog (see [When a plugin asks you something](#when-a-plugin-asks-you-something));
  this command prints it once and keeps it nowhere.

Text that comes from a plugin is printed as plain text, and each byte that is
not printable ASCII is shown as `?`, so a plugin cannot move the cursor or
change the colours of your terminal.

- `--json` prints one JSON object on each line instead. A change of state has
  `type` set to `state` and the members `plugin`, `state` (`starting`,
  `running`, `stopping`, `stopped`, `waiting_to_start_again`,
  `stopped_after_failing`, `off`, `needs_decision`, `blocked_by_administrator`,
  `needs_licence` or `cannot_be_used`), `reason`, `detail`, `process_id`,
  `exit_code` and `exit_abnormal` (only once the process has ended), `sandbox`
  (`level`, `not_applied` and `failure`), `status` (`level`, `text` and `code`),
  `wait_ms`, `failures` and `failure_limit`. A value that is not known is
  `null`. A line of output or log has `type` set to `log`, and `plugin`,
  `source` (`output` or `log`), `level` and `text`. A line the computer has to
  say, such as a policy file that could not be used, has `type` set to `note`
  and `text`. A code for you has `type` set to `secret`, and `plugin`, `label`,
  `text` and `expires_s`.
- `--root <folder>` looks in the folders you give instead of the usual ones, and
  may be repeated. Each folder counts as your own, and the policy file is not
  read. Every option that takes a value can be written `--name value` or
  `--name=value`.
- `--trust <file>` is the file that holds your decisions about plugins. The
  default is `trust.json` in your Virtual Serial Studio folder, the folder that
  holds your `plugins` folder.
- `--data <folder>` is the folder where plugins keep their data, in a folder of
  their own under it. The default is `plugin-data` in your Virtual Serial Studio
  folder.
- `--seconds <n>` stops the plugin after `n` seconds with the plugin running,
  from 1 to 2678400. Without it, or with 0, the command runs until `Ctrl-C`.
  With `--instance`, the seconds count from when the plugin is running and each
  instance is running or refused.
- `--instance <kind>:<type>[=<params>]` asks the plugin host for an instance of
  an extension point of the plugin, and may be repeated. `<kind>` is
  `generator`, `fixture`, `modifier`, `adapter` or `decoder`, `<type>` is the
  `id` of the extension point in the plugin's manifest, and `<params>` is the
  text of a JSON object of at most 64 KiB. Without `=<params>` the parameters
  are `{}`. The command checks only that the parameters are a JSON object; the
  host checks the kind and the type against the manifest, and the plugin is
  free to refuse the parameters. The instances are numbered 1, 2, 3 in the order
  given. A bad `--instance` ends the command with exit code 2 and one line that
  says what is wrong.

The command grants a plugin no engine capabilities and reads no licence files, so
a plugin that needs a licence is shown as `needs a licence`.

### Instances

With `--instance`, the command prints one line each time the state of an
instance changes:

```text
instance 1 adapter:recorder: waiting
instance 1 adapter:recorder: running
instance 2 adapter:nothing: refused; problem capability_missing
instance 3 adapter:recorder: refused; problem misbehaving; reason "refused by the test plugin"
```

After the number and the kind and type comes the state: `waiting` (the plugin is
not ready), `creating`, `running`, `refused` or `stopping`. A refused instance
has the `problem`, a reason code such as `capability_missing` (the manifest has
no such extension point), `too_many` (the manifest allows no more of the type),
`misbehaving` (the plugin answered with an error) or `unresponsive` (the plugin
did not answer), and the `reason` the plugin gave, as plain text. A line is
printed again when the problem or the reason changes. A state that lasts only a
moment may not be printed.

With `--json` a change of an instance has `type` set to `instance` and the
members `plugin`, `instance` (the number), `kind`, `instance_type`, `state`,
`problem` (the code, or `null`), `reason` (the plugin's text, or `null`) and
`params_pending`.

The exit code is 0 when the plugin was running and was stopped by `--seconds` or
by you, 1 when it never ran, or ran and failed too often, and 2 for a command
line the program does not understand or an id that no plugin has. When
`--instance` was given and the command ends by `--seconds`, it is 0 only when
every instance was running at some point; otherwise it is 1, and one line names
the first instance that never ran.

## Where Studio looks for plugins

Studio looks in three folders, widest first. When two of them hold a plugin with
the same id, the first counts.

| | Windows | Linux | macOS |
|---|---|---|---|
| Studio's own | `<program_directory>/../studio-plugins` | the same | the same |
| Machine | `%ProgramData%\Virtual Serial Studio\plugins` | `/usr/local/share/virtual-serial-studio/plugins` | `/Library/Application Support/Virtual Serial Studio/plugins` |
| User | `%LOCALAPPDATA%\Virtual Serial Studio\plugins` | `$XDG_DATA_HOME/virtual-serial-studio/plugins`, or `$HOME/.local/share/virtual-serial-studio/plugins` when `XDG_DATA_HOME` is unset, empty or not an absolute path | `$HOME/Library/Application Support/Virtual Serial Studio/plugins` |
| Policy file | `%ProgramData%\Virtual Serial Studio\policy.json` | `/etc/virtual-serial-studio/policy.json` | `/Library/Application Support/Virtual Serial Studio/policy.json` |

`<program_directory>` is the folder that holds Studio's programs, such as
`<install>/bin`. Studio's own folder is next to it. It is called
`studio-plugins` and not `plugins`, because the Linux package already keeps
Qt's own plug-ins in `<install>/plugins`.

A value from the environment is used only when it is an absolute path. When the
user's folder cannot be worked out, it is left out. A folder that is not there
is passed over.
