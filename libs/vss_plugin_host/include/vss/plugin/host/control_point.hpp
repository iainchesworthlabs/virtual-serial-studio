//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plugin's control sessions: the messages `control.session.request`,
// `control.session.close`, `control.session.closed` and `control.frame`.
// docs/plugins.md, section 7.7, is the design, and control_relay.hpp does
// the work: this file reads and writes the messages and nothing more.
//
//   control.session.request   plugin to host, request
//     scopes    1 to 16 scope names of the control API, never `admin`
//     peer      who the plugin says is behind the session: text of 0 to 64
//               bytes, untrusted
//     reason    why, in the plugin's own words: text of 0 to 256 bytes
//   answered by `control.session.opened`, or by an error: `bad_request`
//   (a member is wrong), `not_granted` (the owner said no, `admin` was
//   asked for, or this program has nobody to ask), `busy` (a request is
//   open, or the plugin has as many sessions as it may) or
//   `plugin_unavailable` (no gateway runs, or it did not answer in time).
//
//   control.session.opened    host to plugin, answer
//     session     names the session from here on; never 0
//     scopes      the scopes the session has
//     expires_ms  when the session ends, in milliseconds since 1970 UTC
//
//   control.frame             both ways, notification
//     session   the session
//     frame     one header of the gateway's protocol, as a JSON object
//   and the frame's bytes in the blob.  The host passes a frame on
//   unchanged.  A frame from the plugin that says `hello`, is for a session
//   that is not open or is not a header of the gateway ends the session.
//
//   control.session.close     plugin to host, notification
//     session   the session to end
//
//   control.session.closed    host to plugin, notification
//     session   the session that ended
//     code      `revoked`, `expired`, `gateway_closed`, `refused_frame` or
//               `host_stopped`
//     reason    one plain line for a log
//
// The plugin never holds a token and never opens a connection.  Every
// session is approved by the owner first; without somebody to ask, every
// request is answered `not_granted`.
//
// A plugin that does not read is ended by the runner's own limit on what
// waits to be sent.  The relay's back-pressure towards the gateway is not
// used yet, so a plugin must keep reading while it has a session.
//

#pragma once

#include <vss/plugin/host/control_relay.hpp>
#include <vss/plugin/host/extension.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace vss::plugin::host
{

/// Asks the owner whether a plugin may have a control session.  Called on
/// the service's strand; it must return at once and call `done` later,
/// once, from any thread.
using control_approver = std::function<void(const control_approval_request&,
                                            std::function<void(bool)> done)>;

/// What a test changes: which gateway is spoken to.
struct control_point_options
{
    /// The instance name of the gateway; empty for this user's own.
    std::string instance;
    std::optional<std::string> control_instance;
};

/// The extension point for one plugin.  With no `approve` every request is
/// answered `not_granted`.
std::unique_ptr<extension_handler>
make_control_point(const extension_context& context, control_approver approve,
                   control_point_options options = {});

} // namespace vss::plugin::host
