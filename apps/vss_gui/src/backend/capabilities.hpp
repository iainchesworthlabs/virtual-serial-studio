//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_BACKEND_CAPABILITIES_HPP
#define VSS_GUI_BACKEND_CAPABILITIES_HPP

namespace vss::gui
{

/// What this build of the engine can actually do.
///
/// A screen asks this once and degrades visibly when a capability is absent:
/// a disabled control with the tooltip "Not available in this build", or the
/// simpler variant the design specifies.  It never simulates the missing
/// behaviour.  The values here are the ones WP0 verified against the code;
/// see `docs/gui-design/implementation-plan.md`.
struct capabilities
{
    /// Create virtual ports of every kind the platform supports.
    bool create_ports = true;
    /// Enumerate serial devices and com0com pairs.
    bool devices = true;
    /// Duplex, split, join, redirect and monitor links with options.
    bool links = true;
    /// Set and release the owner of an owner-policy link.
    bool owner = true;
    /// Drive DTR, RTS and BREAK and read CTS, DSR, DCD and RI.
    bool control_lines = true;
    /// Create a scriptable mock fixture.
    bool fixtures = true;
    /// Inject faults into a mock fixture.  The engine backend provides it
    /// through `vss::studio::inject_fault`; a backend that cannot reach the
    /// fixture leaves it false.
    bool faults = false;
    /// A fixture can echo every byte written to it.  The engine has no echo
    /// mode of its own; `vss::studio::set_fixture_echo` builds one from the
    /// mock's receive path, so `vss_core` does not change.  A backend without
    /// it leaves this false: the Inspector and the menus show Echo disabled
    /// with "Not available in this build" and nothing simulates it.  The byte
    /// generator is not gated: it is plain sends on a timer in the GUI.
    bool fixture_echo = false;
    /// Per-line wiring matrix.  False: the engine only has a boolean
    /// null-modem crossing.
    bool line_matrix = false;
    /// Choose the COM number of a port of the Windows driver, when it is made
    /// and afterwards.  False where there is no such driver.
    bool com_numbers = false;
    /// Make a program let go of a port of the Windows driver: take the port
    /// back, or end the program.  False where there is no such driver.
    bool release_port = false;
    /// Observe whether another program opened the advertised path.  False:
    /// `endpoint_info::open` is the studio's own transport state.
    bool peer_open = false;
    /// Make the ports of the Windows driver again after the helper service or
    /// the driver restarted and took them away (`vss::studio::reopen`).  The
    /// window shows a banner with a "Recreate ports" action while any are lost.
    /// A backend without it shows no banner and does not claim the ports are
    /// fine.
    bool recreate_ports = false;
    /// The baud rate and framing a program set, the clients of a listener and
    /// a warning when the ports of a link run at different speeds
    /// (`endpoint_info::peer_format`, `clients`).  The engine backend provides
    /// them; which ports can say depends on the platform (a Windows driver port
    /// and a Linux or macOS pseudo terminal can, a TCP pipe has no format).  A
    /// backend without it shows no format or client line and no warning.
    bool line_format = false;
    /// Rename a port.  The engine backend provides it through
    /// `vss::studio::rename_port`, which keeps the hub, the studio's records
    /// and a saved setup in step.
    bool rename_port = false;
    /// Give a port new serial settings after it was made
    /// (`vss::studio::reconfigure_port`).  A backend without it opens the
    /// Settings dialog read-only, with Apply disabled and "Not available in
    /// this build".
    bool port_settings = false;
    /// Undo and redo of changes that have an exact inverse: connecting or
    /// removing a link, and changing an owner.  Never a snapshot restore.
    bool undo = false;
    /// Run the benchmark from the GUI.
    bool benchmark = false;
    /// Create the diagnostic bundle.  The ZIP writer lives in the GUI layer,
    /// so this is present whenever the GUI is built.
    bool diagnostics = true;
    /// Report JSON validation errors with a line number.  The engine's
    /// `from_json` returns only an error code, so the editor finds the line
    /// itself (see `json_validation.cpp`); the flag says the editor does it.
    bool json_positions = false;
    /// The setup check can gather driver and device facts.
    bool setup_check = true;
    /// The share scenario and its rollback.
    bool share = true;
    /// TCP, UDP and RFC 2217 endpoints built from a URI.
    bool bridge = true;
    /// Wait for other computers to connect (a listening TCP, UDP or RFC 2217
    /// endpoint). False: the backend cannot open `tcp-listen://`,
    /// `udp-listen://` or `rfc2217-listen://` endpoints. The engine can
    /// (`vss_ports`' listen transport), so only a backend without one leaves
    /// this off.
    bool network_server = false;
    /// A shared multi-drop bus link: every member hears every other and none
    /// hears itself (`vss::hub::bus`).  The engine backend provides it.  A
    /// backend without it keeps the star (a split with a join tap): the
    /// Connect dialog shows the bus choice disabled with "Not available in
    /// this build", and a setup file with a bus is refused with that reason.
    bool shared_bus = false;
    /// A shared bus that behaves as a half-duplex line: one port sends at a
    /// time and Studio reports when two send at once
    /// (`vss::half_duplex_options`).  The engine backend provides it.  A
    /// backend without it shows the control disabled with "Not available in
    /// this build", and the JSON editor refuses a file that uses it with that
    /// reason.
    bool half_duplex_bus = false;
    /// List, and watch the traffic of, ports that another program made.  False
    /// in every build today: a Windows driver port belongs to the process that
    /// made it (its hub end takes one handle and the port goes when that
    /// process ends), and the hub keeps no registry that a second process can
    /// read.  The window then says that such ports are not shown, and names
    /// the ones Windows reports (`StudioBackend::other_program_ports`).  See
    /// `docs/gui-design/docs/08-engine-gaps.md`.
    bool external_ports = false;
    /// Create another com0com null-modem pair (Windows, needs elevation).
    /// The share wizard's warn note calls the adapter method this gates.
    bool com0com_pair_creation = true;
    /// Add one Windows Firewall rule for a listener, through the elevated
    /// helper, after the user agrees.  False where the platform has no such
    /// step or the helper is not installed; the dialog then only explains and
    /// shows the command, with "Not available in this build".
    bool firewall_rule = false;
    /// Put the user in the group that owns the serial devices, through polkit,
    /// after they agree (Linux with pkexec installed).  False elsewhere; the
    /// dialog then only explains and shows the command.
    bool serial_group_step = false;
};

} // namespace vss::gui

#endif // VSS_GUI_BACKEND_CAPABILITIES_HPP
