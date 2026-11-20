# Settings and help

## Finding things: the command palette

`Ctrl+K`, or the **Search actions** box in the menu bar, opens a list of every
action. Type a few letters, use the arrow keys and press `Enter`. Actions that
need Advanced mode are listed under **Needs Advanced mode**, with the button
**Turn on Advanced mode**.

## Settings

**Tools → Settings** (`Ctrl+,`) has four pages: **General**, **Running**,
**Appearance** and **Performance**. Changes apply immediately; there is no Apply
button.

- **General**: *Advanced mode*, *Default baud rate* (offered first when you
  attach a device) and *Ask before discarding an unsaved setup*. At the foot of
  the page is the **Updates** group, described below.
- **Running**: *When I close the window* (*Keep the ports running in the
  background* or *Stop the ports and quit*) and *Start when I sign in*, which
  starts Studio hidden in the notification area and starts the ports that were
  running before; see
  [When the window is closed](setups.md#when-the-window-is-closed). Where a build
  cannot start at sign-in the switch is off and disabled, with the tooltip "Not
  available in this build".
- **Appearance**: *Theme* (follow the system, light or dark), *Density*
  (comfortable or compact) and *Link lines* (curved, the default, or right
  angles with rounded corners). Text size, display scale, high contrast and reduced
  motion follow your operating system; **Open system settings** opens the
  operating system's page for them.
- **Performance**: *Refresh interval*, 50 to 5000 ms. Shorter feels live;
  longer is lighter on a busy machine. Then three groups:
  - *Rendering* chooses how the canvas is drawn: **Automatic** (the graphics card
    when there is a usable one), **Hardware (GPU)** or **Software (CPU)**. The
    choice is read when Studio starts, so a change needs a restart, and the page
    says "Restart Studio to use this choice." A line, "Drawing now: ...", says
    what is in use. When there is no usable graphics device Studio draws on the
    processor and says why, for example "Software (CPU), because: ...". For one
    run, `vss_gui --render=auto|hardware|software` (or `--render <mode>`) and the
    environment variable `VSS_RENDER` win over the setting, the option over the
    variable, and the page then says "VSS_RENDER or --render is set, so it
    overrides this choice." **About → Environment** has a *Renderer* row that
    shows the one in use.
  - *Profiling*: *Record a performance trace* lets a Tracy profiler watch where
    Studio spends its time, which is how a slowdown is found. Studio remembers
    the switch and records again from its next start, unless it is started with
    `--no-profile` or `VSS_PROFILE=0`. It listens on this computer only and
    sends nothing until you connect the profiler; the status bar shows the
    address while it is listening. *Let other computers connect* opens it to your
    network and can be chosen only before the listener has opened (`vss_gui
    --profile-remote` does it at start). Turning the trace off stops recording,
    but the port stays open until Studio quits. Builds without the profiler show
    the switches as unavailable ("Not available in this build"). See
    [profiling](../profiling.md).
  - *Debug access*, described under Help and diagnostics below.

  Separately from these switches, Studio writes a line to `stalls.log` in its
  local data folder when the window stops answering for two seconds, naming
  what it was doing. The file stays on your computer.

The **Updates** group at the foot of the General page has *Check for updates
automatically* and *Include pre-release versions*. See
[Updates](updates.md#turning-the-check-on-or-off). **Reset to defaults** restores
the other settings and leaves these two as they are.

The settings are stored with `QSettings`. On Windows they are in the registry
under `HKCU\Software\iainchesworthlabs\Virtual Serial Studio`, and the saved
running setup is the file `running-setup.json` in `%APPDATA%\iainchesworthlabs\Virtual
Serial Studio`. On Linux and macOS the files are under `iainchesworthlabs/Virtual
Serial Studio` in the same way, and on macOS the settings file is
`com.iainchesworthlabs.Virtual Serial Studio.plist`. Settings made by builds
before this naming are not carried over.

<picture><source srcset="../../images/settings-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/settings-light.png" width="860" height="760" alt="The Settings dialog on the General page, with Advanced mode, Default baud rate, the unsaved setup prompt and the Updates group; Running, Appearance and Performance are listed on the left"></picture>

## Help and diagnostics

- **Tools → Setup check** says what this PC needs so other programs can use
  your ports. On Windows it looks for the Virtual Serial Studio driver (and
  names its version) or com0com, whether administrator rights will be needed,
  and the real serial devices. Either driver counts as ready, and the com0com
  install guidance appears only when neither is found. When Studio's own driver
  or its helper service is missing, out of date or refused, the row offers
  **Set up Virtual Serial Studio** or **Update the driver**; see [The driver and
  its helper service](when-the-system-asks.md#windows). It also says whether
  Windows Firewall lets other computers reach a port you listen on (the
  *Windows Firewall* row reads OK and says the firewall is not involved while
  nothing is waiting for other computers to connect). On
  Linux and macOS it looks at pseudo terminals and your serial port group, and
  adds rows for the firewall, confinement and local network access when they
  matter; see [When the operating system asks, or says
  no](when-the-system-asks.md). A last row, *Continue without a driver*, has
  **Continue in test mode**. The chip at the top reads "Ready", "Not checked
  yet" or "1 thing to fix" (or "`<n>` things to fix"), **Re-check** asks again,
  and on Windows **Why is a driver needed?** shows how to install com0com. The
  status bar chip shows "Setup ready" or "Setup needs attention" and opens the
  same dialog.

<picture><source srcset="../../images/setup-check-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/setup-check-light.png" width="760" height="720" alt="The Setup check dialog on Windows: the driver, administrator rights, real serial devices and Windows Firewall, each marked OK, and a last row, Continue without a driver"></picture>

- **Help → Why is nothing arriving?** walks five steps between the device and
  your program: *The port exists and is open*, *The port is linked to the
  device*, *The device is sending*, *A program has opened `<the port>`* and *Data
  reaches the program*. Each reads "Passed", "Needs attention" or "Not checked
  yet", with a sentence that says what to try. A **Try this** card gives the
  steps for your program, with **Copy `<port path>`** and **Check again**, and
  **Create support bundle** is in the footer. On Linux Studio can see whether a
  program has opened a pseudo terminal, and on Windows whether one has opened a
  port of the Studio driver, so the fourth check says so there. Everywhere else
  it cannot and reads "Not checked yet". The last check always reads "Not checked
  yet", because Studio cannot see inside your program.

<picture><source srcset="../../images/troubleshoot-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/troubleshoot-light.png" width="640" height="760" alt="The Why is nothing arriving dialog: the port is open, it has no link, and the later checks are not yet run"></picture>

- **File → Support bundle** saves a ZIP named `vss-support-<date>.zip` (the date
  as `yyyy-mm-dd`). The list on the left has a row for each part, and a row you
  untick leaves the bundle:

  | Row | File |
  |---|---|
  | Version and environment | `environment.txt` |
  | Driver state | `driver.txt` |
  | Setup | `setup.json` |
  | Event log | `log.txt` |
  | Port counters | `counters.csv` |
  | Serial ports on this computer | `serial-ports.txt` |
  | Calls to Windows | `trace.txt` (off until you turn it on) |
  | Licence | `licence.txt`: the licence number, edition, term and state, when a licence is installed |

  **Calls to Windows** adds each call Studio made to Windows or to its serial
  library that failed, with the code Windows gave and the calls around it. Two
  more rows, *Licensee name* and *This computer's name*, are off until you turn
  them on and add those to `licence.txt`. The zip starts with `manifest.json`,
  which lists the parts you chose. The bytes your ports sent and received are
  never included. Passwords, tokens and keys are replaced with `[redacted]`, the
  path of your home folder is shortened to `~`, and your computer and user names,
  device serial numbers and the licence file are left out. The right side of the
  dialog shows each file exactly as it will be saved (the first 256 kB of a
  longer one; the saved file is whole). **Save bundle** asks where to save it, and
  afterwards offers **Show in folder**; if the save fails, **Choose another place**
  and **Copy details** are offered. Nothing is sent anywhere; you choose where to
  save the file.
- **Settings → Performance → Debug access** lets programs on this computer ask
  a running Studio what its ports, its engine and Windows are doing. Use it to
  find out why a port will not open, why Windows refuses a COM number, or why a
  network client does not connect. It is off every time Studio starts. While it
  is on, the status bar shows **Debug access on**, and pressing that turns it
  off. A program that wants to use it asks first, and you choose **Don't
  allow**, **Allow once** or **Allow until Studio quits**. Each use is written to
  a log in Studio's data folder. It uses a pipe only you can open, and nothing
  is sent anywhere. The first switch reads "Let programs on this computer read
  what Studio is doing". The second, "Let them read the bytes of traffic for ten
  minutes", lets a program read the bytes your ports carry, for ten minutes at
  most, and **Disconnect all programs** ends every connection. `vss_cli debug` is
  the command line client:
  `vss_cli debug snapshot` shows every port with its state, counters and last
  error, `vss_cli debug trace --follow` shows the calls Studio makes to Windows
  as they happen, with the code Windows gave for each one that fails, and
  `vss_cli debug com` shows which COM numbers Windows keeps and why. See
  [A debug API for a running Studio](../debug-api.md).
- **Help → Support...** says where to get help for your edition, what to write
  before you ask, and has the **Create support bundle** button and a **Copy**
  button for the version, edition, licence number and operating system. It
  copies addresses to the clipboard and does not open a web page or a mail
  program.

- **Help → Help contents** (`F1`) has four cards, each with **Open**: *Setup
  check*, *Why is nothing arriving?*, *Keyboard shortcuts* and *About Virtual
  Serial Studio*.
- **Help → Keyboard shortcuts** lists every shortcut, with a *Filter shortcuts*
  box; **Help → Copy diagnostic information** copies the build information and
  the environment for a bug report.
- **Help → About** shows the application icon, the version, the licence and the
  edition at the top, and has six tabs:
  - *About*: the GPLv3 notice, a link to the source code and, on Windows, a
    sentence about the driver versions (see [The driver and its helper
    service](when-the-system-asks.md#windows)).
  - *Updates*: see [Updates](updates.md#checking-by-hand).
  - *Build information*: version, Git tag, Git revision, Short revision, Working
    tree, Branch, Build number, Built, Build type, Compiler, Qt built with and
    Boost.
  - *Environment*: Operating system, Architecture, Kernel, Qt running, Platform
    plugin, Display, Renderer, Language and Profiling.
  - *Licences*: the components this build contains: Qt 6, Qt Canvas Painter,
    Boost, Boost.VirtualSerial, Monocypher and, in a build with profiling, Tracy.
  - *Fonts*: IBM Plex Mono, Noto Sans Arabic and Noto Sans Hebrew, which are built
    into the program, each with its version and licence.

  The version reads like `v0.1.0-rc.5+18.gabc1234.build.123`: the release, the
  number of commits after it, the short commit and the CI run number, with
  `.dirty` at the end when the tree was modified. A plugin's version is written
  the same way, so a build of a plugin shows which commit it was made from. The
  two data tabs each have a button (**Copy build information** and **Copy
  environment**). The footer has **Create support bundle**, **Copy diagnostic
  information**, which copies both tabs, and **Close**.

The application is available in nine languages (English, French, Spanish,
German, Hebrew, Yiddish, Japanese, Simplified Chinese and Arabic). It uses your
system's preferred languages in order and loads the first one it has; any
English variety is English, and Chinese is Simplified. There is no language
setting in the window. Hebrew, Yiddish and Arabic switch the layout to right to
left. The translations are machine translations that no speaker has reviewed.
`VSS_LANGUAGE` names one language to try (for example `VSS_LANGUAGE=de`), and
`pseudo` or `pseudo-rtl` shows every text accented and longer, to find text that
clips. **About → Environment** shows the language chosen. Studio carries three
fonts so that Hebrew, Arabic and the byte columns look the same on every system.
No Chinese or Japanese font is included. See [i18n.md](../i18n.md).

## Keyboard shortcuts

`Ctrl` is `Cmd` on macOS.

| Shortcut | Action |
|---|---|
| `Ctrl+N` / `Ctrl+O` | New / Open |
| `Ctrl+S` / `Ctrl+Shift+S` | Save / Save as |
| `Ctrl+Q` | Quit (the platform's Quit shortcut) |
| `Ctrl+Shift+N` | Add port |
| `Ctrl+Shift+C` | Copy port name |
| `F2` | Rename the selected port |
| `Delete` | Remove the selected port or link |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / Redo the last change that can be undone |
| `Ctrl+L` | Connect ports |
| `Ctrl+K` | Search actions |
| `Ctrl+,` | Settings |
| `F5` | Refresh now |
| `Ctrl+Shift+A` | Advanced mode |
| `Ctrl+0` | Fit the topology (Advanced) |
| `Ctrl+D` | Duplicate the selected ports (Advanced) |
| `Ctrl+Shift+B` | Show or hide the toolbox (Advanced) |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Next pane / Previous pane (Advanced) |
| `Enter`, `Alt+Enter` | In the topology: inspect the focused port, open Settings for the selected ports (Alt+Enter while editing, Advanced) |
| `F1` | Help contents |
| `Space`, `Ctrl+F` | In the monitor: pause, search (the search box is in Advanced); `Ctrl+F` also searches the log |
