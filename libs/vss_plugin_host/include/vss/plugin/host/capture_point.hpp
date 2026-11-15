//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Recording for a plugin: the messages `capture.start`, `capture.stop` and
// `capture.status`.  docs/plugins.md, sections 19.4 to 19.6, are the
// design.
//
// A plugin that was granted `traffic.tap` asks the host to record the
// traffic of some places.  The host does the recording: it taps the hub
// (traffic_tap.hpp) and writes the recording (recording.hpp) into a
// folder of the plugin's data directory, in the open format of section
// 19.6.  The plugin never sees the hub; it reads the recording afterwards,
// like anybody else could.
//
// - One recording at a time for each plugin: a second `capture.start` is
//   answered `busy`.
// - A place is named as the hub names it.  The owner of the ports says
//   which names exist (`place_finder`); a name it does not know is
//   answered `refused`, with the name in the detail.
// - The recording is written under `recordings/<name>/` in the plugin's
//   data directory.  The name is the plugin's (`capture.start.name`, a
//   plain file name) or one the host makes; a folder that exists is never
//   written to again.
// - The limits the plugin asks for are bounded by the message's own
//   limits; when one is reached the recording stops by itself and
//   `capture.status` says so.
// - `capture.stop` finishes the recording; its index is written last, so
//   a recording that was cut off by a crash is never taken for a whole
//   one.  When the plugin goes away the recording is finished the same
//   way.
//
// The tap never blocks a hub strand: what the recording cannot keep up
// with is dropped and counted, and the count is written into the
// recording where it happened.
//

#pragma once

#include <vss/core/hub.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/recording.hpp>
#include <vss/plugin/host/traffic_tap.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vss::plugin::host
{

/// A place a plugin may record, as the owner of the ports knows it.
struct capture_place
{
    /// What to watch in the hub.  Its `name` is the place's name.
    tap_place tap;
    place_kind kind = place_kind::port;
    /// The line settings as text for the recording's index, such as
    /// "115200 8N1"; at most 256 bytes.
    std::string settings;
};

/// Finds a place by its name, or says there is none.  Called on the
/// service's strand.
using place_finder =
    std::function<std::optional<capture_place>(std::string_view name)>;

/// The extension point for one plugin.  `source` must outlive it.
std::unique_ptr<extension_handler>
make_capture_point(const extension_context& context, hub& source,
                   place_finder find);

} // namespace vss::plugin::host
