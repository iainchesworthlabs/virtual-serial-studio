# 10 Running in the background

What happens to ports and links when nobody is looking at Studio: when the window is closed, minimised, or the
computer restarts. The product promise is that **ports just work**. Studio is where you set them up and watch them. It is
not something you have to keep open and in front of you.

## How it works today

| Question | Today |
|---|---|
| Where do the ports run? | Inside the `vss_gui` process. Studio is the engine: the hub, every port, link and fixture. |
| I minimise the window | Everything keeps running. |
| I close the window | Studio quits and every port and link stops. A Windows driver port is removed, so a program that had it open loses it. Studio warns only when a program has a port open. |
| I open Studio again | It starts empty and shows the launcher. The Saved setups row offers the last file, and nothing starts until you open it. |
| I restart the computer | Nothing comes back. Driver ports are made on demand and go with the process. com0com pairs stay but have no links. Nothing starts at sign-in. |
| A setup made with `vss_cli run` | Runs in its own process with its own hub until Ctrl+C. Studio cannot see it (see the "Ports another program made" row in `08-engine-gaps.md`). |
| `vsscom_helper` | A Windows service that makes driver ports and changes COM numbers for ordinary users. It does not run ports or links. |

So the model the user expects (close the window and the ports carry on, sign in and they are back) is not what happens.

## The model

These are the words and the behaviour, whatever process runs the engine.

- **Running setup**: the ports and links running now on this computer. Studio saves it by itself, a second after every
  change, to a per-user file (`<AppData>/running-setup.json`), with the name of the saved setup it came from and
  whether it has unsaved changes. It is not a file you name or open.
- **Saved setups** work as before. File, Save writes the running setup to a named file you keep, share and open again.
  File, Open replaces the running setup with the saved one, which then runs until you quit Studio or open another. You
  can change it while it runs and save it again. A setup you open is never only a preview: opening it is running it.
- **Close the window** (title bar ×, Alt+F4): with no ports, Studio quits. With ports, the window
  goes and the ports keep running. Studio stays in the notification area (the menu bar on macOS). The first time, a
  notification says so.
- **Quit Studio** (File, Quit, Ctrl+Q, or "Stop the ports and quit" in the notification-area menu): stops every port
  and link. While any port runs it asks "Stop the ports and quit?" first, with **Stop the ports and quit** and **Keep
  running**. The body is the existing warning when a program has a port open, and otherwise "This stops 3 ports and 2
  links. Studio offers to start them again the next time it opens." The unsaved-changes prompt comes first, as before.
- **Open Studio again** (Start menu, desktop, `vss_gui`): if Studio is already running, its window comes back with
  everything live. There is only ever one Studio per user. The lock is a local socket only this user can open (a named
  pipe on Windows); it never uses the network.
- **Start when I sign in** (off by default): Studio starts hidden in the notification area at sign-in (`vss_gui
  --background`) and starts the running setup again. With nothing to run it quits at once. It is offered in the
  first-close notification, the notification-area menu and Preferences. With it off, the running setup comes back the
  next time you open Studio.
- **After a restart, a sign-out or a crash** the running setup comes back by itself, because you never quit. **After
  Quit** it does not: a banner over the launcher says "When you last quit, 3 ports and 2 links were running." with
  **Start again**. Building or opening anything else replaces it.
- **Restart as administrator** (Setup check) is not a Quit: the new Studio waits for the old one to end and starts the
  same ports.

### The notification-area icon

Tooltip "Virtual Serial Studio: 3 ports, 2 links running". Menu: **Open Studio**, a disabled status line "3 ports, 2
links running", a separator, **Start when I sign in** (a check item), a separator, **Stop the ports and quit**. A click
or double-click opens the window. The icon is the theme's connect mark, and the warning mark while a Windows driver port
is lost; Studio shows one notification when that starts ("Some COM ports were lost" / "Open Studio to recreate them."),
never one per event.

The first time the window closes with ports running, one notification: "Studio is still running your ports" / "Open it
again from this icon, or quit it there to stop the ports. To have them back after a restart, choose Start when I sign
in in the same menu."

### Settings

Preferences has a **Running** page after General: the sentence "Your ports run on this computer until you quit Studio
or open another setup. The window is only for setting them up and watching them.", then "When I close the window" with
"Keep the ports running in the background" (default) and "Stop the ports and quit" (`ui/closeAction`, `background` or
`quit`), then the **Start when I sign in** switch with the hint "Studio starts in the notification area and starts the
ports that were running before the computer restarted." The sign-in entry itself is that setting (a `Run` value on
Windows, an XDG autostart file on Linux), so nothing is kept in QSettings for it. `ui/backgroundNoticeShown` records the
first-close notification.

The running-setup file is `{"version": 1, "documentPath", "dirty", "stopped", "setup": <the saved-setup JSON>}`, so
the facts and the ports are always written together in one atomic write. The topology schema is unchanged.

### Degrading

| Missing | Then |
|---|---|
| No notification area (some Linux desktops, `QSystemTrayIcon::isSystemTrayAvailable` is false) | Closing with ports asks "Keep the ports running?" ("This desktop has no notification area to keep Studio in. Minimise the window to keep the ports running, or stop them and quit.") with **Cancel**, **Stop the ports and quit** and **Minimise instead**. Nothing hides where it cannot be found again. `--background` starts minimised. |
| No way to start at sign-in (macOS today: a login item needs a signed helper in the bundle) | The switch and the menu item are disabled with "Not available in this build", and the first-close notification leaves that sentence out. |
| The sign-in entry cannot be written | The switch goes back, and "Could not change start at sign-in" says nothing was changed. |
| The running setup cannot be read (damaged, newer format) | Studio starts empty, moves the file to `running-setup.bad.json`, and says "The ports from last time could not be read, so Studio started without them." in the status bar. It never half-applies it. |
| The running setup cannot start (COM number taken, driver missing) | Nothing of it starts (the setup is applied with rollback, like File, Open). The status bar says "Could not start the ports from last time: <reason>", a hidden Studio notifies, and the Start again banner offers it. A Studio started with `--background` stays in the notification area (minimised where there is none) although no port runs, so that the notification and the banner are there to be seen. |

## Where the engine runs: two ways to get there

**A. Studio stays resident.** The engine stays in `vss_gui`. Closing hides the window, a single-instance lock
(`QLocalServer`, named per user) brings back the running Studio, and sign-in start is a per-user entry (Windows `Run`
key, XDG autostart on Linux, a login item on macOS). GUI work only; no change to `libs/`. Limits: a Studio crash still
stops the ports; `vss_cli` and Studio still have separate hubs.

**B. A background agent.** A new per-user process (`vss_agent`, started at sign-in by the same entries, or a systemd
user unit or launchd agent) owns the studio. Studio and `vss_cli` are clients over a local pipe or socket open to that
user only. Closing or crashing Studio never touches a port, and the CLI and the window see the same ports, which also
closes the "ports another program made" gap. Cost: a control protocol for everything `StudioBackend` does, including
the event stream and traffic, a `RemoteStudioBackend`, the agent's own lifecycle and logging, and tests for all of it.
Several PRs and an engine-side library.

**Chosen (2026-11-06): A now, built so B can replace it.** The model above is the same in both, so B later changes no
screen and no string: only which process runs the engine. `StudioBackend` is already the one seam B needs. B is the
"Background agent" row in `08-engine-gaps.md`. Its design, the decisions it needs and a first slice (`vss_agent`,
`vss_cli agent`, loopback ports only) are in `docs/agent.md`; Studio does not use the agent yet.

## Where it lives

| Piece | File |
|---|---|
| Close, Quit, autosave, start again, the banner | `shell/shell_window.cpp` (`enable_background`, `closeEvent`, `quit_studio`) |
| The notification-area icon | `shell/studio_tray.cpp` |
| One Studio per user | `shell/single_instance.cpp`, used by `main.cpp` |
| The running setup file and its facts | `support/running_setup.cpp` |
| The sign-in entry | `support/sign_in_start.cpp` |
| The Running page | `support/settings_dialog.cpp` |

Tests and the self-test never call `enable_background`, so they keep close-means-quit unless a test turns it on with a
temporary file.
