//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/studio_backend.hpp"
#include "simple/setup_kinds.hpp"

#include <vss/core/serial_settings.hpp>

#include <QString>

#include <optional>
#include <vector>

namespace vss::gui::simple
{

/// One end of a job, as the form fills it in.
struct end_plan
{
    enum class kind
    {
        /// A new virtual port a program on this PC opens.
        program,
        /// A serial device plugged into this PC.
        device,
        /// A serial device on the network (a serial server box).
        network_device
    };

    kind what = kind::program;
    /// A port of the setup being changed that stays as it is, or 0.
    vss::endpoint_id existing = 0;
    /// The program's name, used as the port's name.  Optional.
    QString label;
    /// The COM number to ask for, or 0 to let Studio pick.
    unsigned com_number = 0;
    /// The device's path, such as COM3 or /dev/ttyUSB0.
    QString device_path;
    QString address;
    unsigned network_port = 4001;
    bool rfc2217 = false;
};

/// Everything a job form gathers.  Which list holds what depends on the job:
///
/// | job | first | second |
/// |---|---|---|
/// | port | the program | |
/// | pair | first program | second program |
/// | attach, share | the device | the programs |
/// | talk, network | the device | |
/// | bus | the members | |
/// | combine | the senders | the receiver |
/// | turns | the programs | the device |
struct job_plan
{
    job kind = job::port;
    std::vector<end_plan> first;
    std::vector<end_plan> second;
    vss::serial_settings serial;
    /// "Send everything straight back" for a lone program port.
    bool echo = false;
    /// The network port other computers connect to.
    unsigned listen_port = 4001;
};

/// The address Studio opens a device end with.  A network device is
/// `tcp://host:port` or `rfc2217://host:port`, with an IPv6 host in brackets.
QString device_uri(const end_plan& end, const vss::serial_settings& serial);

/// `end` with the address, port and protocol a person typed in the address
/// field taken apart: `192.168.1.50:4001` gives the host and the port, and
/// `rfc2217://box:4002` also the protocol.  Nothing when the address is not
/// one (see `parse_device_address`), with `error` set to the sentence for it.
/// An end that is not a network device is returned as it is.
std::optional<end_plan> resolved_network_end(const end_plan& end,
                                             QString* error = nullptr);

/// The plan that describes an existing setup, with every end kept.
job_plan plan_from(const setup_view& setup,
                   const std::vector<port_view>& ports);

/// Builds `plan`.  With `previous`, changes that setup instead: the ends
/// marked `existing` stay open, new ends are made, the link is changed and
/// the ports that are no longer in the plan are removed.  An attached device,
/// a share and programs taking turns on the same device change their link in
/// place (programs are added to it and taken off it), as do the senders of a
/// combine into the same receiver (its sources) and the members of a bus (add
/// the new ones, then take off the old), so the link keeps its id, options and
/// owner and the other ports lose no bytes.  A pair, a lone port with its echo
/// and the network setup have no in-place edit, so their link is replaced.  A
/// failure removes what this call made and puts the previous link back, or
/// leaves it as it was.
///
/// - The ends that chose a COM number are made before the Automatic ones, so
///   an Automatic end cannot take a number another end of the plan chose.
/// - Settings of a device the plan keeps that differ from the device's own are
///   given to the device (`set_port_settings`), and put back if a later step
///   fails.
/// - The network setup opens the device with the plan's settings and opens
///   `tcp-listen://0.0.0.0:<port>`, so other computers can connect.  A change
///   closes the old pair first (both may want the same device and port), and
///   makes it again from its plan if the new one cannot be made.
///
/// Returns a port of the setup, to select it.  When a COM number the plan asked
/// for belongs to another device (`errc::already_exists`), `clashed_com`, if
/// given, is set to that number so the form can name it; otherwise it is 0.
/// `previous_lost`, if given, is set when a failed change could not put the
/// previous setup back, so the form does not say it did; otherwise it is false.
vss::result<vss::endpoint_id> build_setup(StudioBackend& backend,
                                          const job_plan& plan,
                                          const setup_view* previous = nullptr,
                                          unsigned* clashed_com = nullptr,
                                          bool* previous_lost = nullptr);

/// The ports `plan` keeps, makes and closes compared with `previous`, for the
/// "What Apply changes" note.
struct plan_changes
{
    std::vector<vss::endpoint_id> kept;
    int added = 0;
    std::vector<vss::endpoint_id> closed;
};
plan_changes compare(const job_plan& plan, const setup_view& previous);

} // namespace vss::gui::simple
