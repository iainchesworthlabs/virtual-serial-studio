//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plugin shows the user a secret once: a pairing code, a one-time
// password.  The message is `secret.show`.
//
// A plugin has no window, and its `status` text and log lines are kept,
// shown again and may be written to a file, so a secret must never travel
// in them.  With `secret.show` the plugin hands the secret to the host,
// which shows it to the user in a dialog of its own and keeps it nowhere:
// not in a log, not in a snapshot, not in a file.
//
//   secret.show   plugin to host, request
//     label       what the secret is, for the user: plain text of 1 to 64
//                 bytes, such as "Pairing code"
//     text        the secret: plain text of 1 to 512 bytes
//     expires_s   how long it is good for: 1 to 3600 seconds
//   answered `ok` when the user has seen it, or with the error
//   `not_granted` when they closed it without looking or this program
//   cannot show it.  One at a time for each plugin: a second while one is
//   shown is answered `busy`.
//
// Everything in the message is the plugin's own words.  The label is
// cleaned and cut before it is shown; the dialog must set the label and
// the secret apart from Studio's own text, name the plugin from its
// manifest, and never render either as anything but plain text.
//

#pragma once

#include <vss/plugin/host/extension.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace vss::plugin::host
{

/// What the program is asked to show.
struct secret_to_show
{
    std::string plugin_id;
    /// From the manifest, cleaned: untrusted text.
    std::string plugin_name;
    /// Cleaned and cut: untrusted text.
    std::string label;
    /// The secret.  Show it; keep it nowhere.
    std::string text;
    std::chrono::seconds expires{0};
};

/// Shows a secret and says whether the user saw it.  Called on the
/// service's strand; it must return at once and call `done` later,
/// exactly once, from any thread.
using secret_shower =
    std::function<void(secret_to_show secret, std::function<void(bool)> done)>;

/// The extension point for one plugin.  With no `show` every request is
/// answered `not_granted`: a program that has nowhere to show a secret.
std::unique_ptr<extension_handler>
make_secret_point(const extension_context& context, secret_shower show);

} // namespace vss::plugin::host
