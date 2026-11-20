# Using Virtual Serial Studio

This guide is for people using the desktop application. It covers the two
levels of the window, the everyday jobs, the expert tools, and what to check
when something does not work.

The application is a window over the `vss::studio` engine: it creates ports,
wires them together, and shows what flows through them. Most of it can also be
scripted with `vss_cli`; the command equivalents are on the [Command line
equivalents](command-line.md) page.

The pictures show Virtual Serial Studio drawn with sample ports, in the light
theme and in the dark theme; a page shows the one that matches the colour
scheme of your browser. Port names such as `COM7` and `/dev/pts/1` are examples;
yours will differ.

## Contents by task

The guide is in pages by task, and each line below says what you can do on its page.

- [Getting started](getting-started.md): choose Simple or Advanced, find your way around the window, do a job and make your first port.
- [Adding ports](adding-ports.md): add a virtual port, a linked pair, a real device, a network endpoint or a mock fixture.
- [Links between ports](connecting-ports.md): build on the diagram, connect ports, and let go of a port that a program holds.
- [Sharing a device](sharing-a-device.md): let several programs use one device, keep one master with a silent listener, and see who is on the other side of a port.
- [Timing and control lines](timing-and-control-lines.md): add delay and jitter to a link, pace it like a wire, use RS-232, RS-422 and RS-485, and work with the control lines.
- [Windows](windows.md): run old programs on Windows, choose a COM number, and install or remove Studio.
- [Monitor and capture](monitor-and-capture.md): watch traffic, send bytes into a port and keep capture files.
- [Setups](setups.md): save a setup, open one or start from a template, and know what happens when the window is closed or the ports are taken away.
- [Network addresses](network.md): use the address forms, let other computers connect, use a device on another computer and keep it safe.
- [When the operating system asks, or says no](when-the-system-asks.md): see what Windows, Linux and macOS ask for and what to do when one says no.
- [Settings and help](settings-and-help.md): find things with the command palette, change the settings, run the checks and learn the keyboard shortcuts.
- [Plugins](plugins.md): decide about a plugin, accept an author's key, add and remove plugins, and list, check, install and run them from the command line.
- [Updates](updates.md): check for an update, download and install it, turn the check on or off, and see what is sent.
- [Licences](licences.md): import a licence, renew it or remove it, and see what happens when it ends.
- [What this build cannot do](limits.md): see which controls are not available in this build and what is not built yet.
- [Troubleshooting](troubleshooting.md): find what Studio shows when something does not work and what to check.
- [Command line equivalents](command-line.md): find the `vss_cli` command for a job, record, replay and script a device, and see the other commands.
- [Scenario recipes](../scenarios.md): follow worked recipes that can be built in the window, scripted or written as a topology.
- [Controlling Studio from a script](../control-how-to.md): drive the ports from the command line or from a Python test script.
- [Virtual ports on Linux and macOS](../posix-virtual-ports.md): see how a virtual port works on those systems and how to give it a stable name.
- [User experience design](../ux.md): read why the window is laid out as it is.

See [User experience design](../ux.md) for the design reasoning, [Scenario recipes](../scenarios.md)
for worked examples and [the screen specifications](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/gui-design/docs/00-product-brief.md).
