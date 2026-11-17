//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replay for a plugin: the message `timeline.load`.  docs/plugins.md,
// sections 19.4 and 19.6, and docs/response-timing.md ("Replaying a
// recording") are the design.
//
// A plugin that was granted `ports.write` hands the host a recording it
// holds, by its path in the plugin's data directory, and the name of a port.
// The host reads the recording (recording.hpp) on the slow executor, keeps
// the bytes the recorded place received (`R` lines), and plays them onto the
// port as a timeline: each chunk is written at its recorded time, multiplied
// by `time_scale`, counted from the answer `ok`.  A chunk is never written
// before its time.  How late one was is counted.
//
// - The instance must be one the plugin holds and runs, of kind generator
//   or fixture.  Anything else is `refused`.
// - The recording is a path inside the data directory; the recording is
//   checked whole, as `read_recording` checks it, before anything is played.
//   A recording that fails the check, or is not there, is `refused`.
// - The recorded place is the one named like the port; a recording of one
//   place is played onto any port.  A recording of several places that has
//   none of that name is `refused`.
// - A recording is played from memory: at most `max_timeline_bytes` bytes of
//   traffic and `max_timeline_items` chunks.  A larger one is `refused`.
// - An instance plays one recording at a time.  A second `timeline.load`
//   for it while one plays is answered `busy`.
// - A replay ends by itself after its last chunk, when the instance stops or
//   the plugin goes away.
// - Not played, and not claimed to be: line changes, faults, and the bytes
//   the recorded place sent (`W` lines).
//
// Nothing here blocks the service's strand: the file is read on the slow
// executor, and a write is queued on the endpoint, no more than a window of
// it at a time.
//

#pragma once

#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/generator_point.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace vss::plugin::host
{

/// The most bytes of traffic one replay holds in memory.  A choice.
inline constexpr std::uint64_t max_timeline_bytes = 64ULL * 1024 * 1024;
/// The most chunks one replay holds.  A choice.
inline constexpr std::size_t max_timeline_items = 4'000'000;
/// The most bytes handed to an endpoint and not yet taken.  A choice.
inline constexpr std::uint64_t max_timeline_window = 256ULL * 1024;

/// What the handler did, counted since it was made.
struct timeline_counts
{
    /// Replays that began, and ones that played their last chunk.
    std::uint64_t started = 0;
    std::uint64_t finished = 0;
    /// Requests answered with an error.
    std::uint64_t refused = 0;
    /// Chunks and bytes given to an endpoint.
    std::uint64_t chunks = 0;
    std::uint64_t bytes = 0;
    /// Writes an endpoint failed.
    std::uint64_t write_errors = 0;
    /// The most nanoseconds a chunk was written after its time.
    std::uint64_t latest_ns = 0;
};

class timeline_point : public extension_handler
{
public:
    virtual timeline_counts counts() const = 0;
    /// The number of replays that are playing now.
    virtual std::size_t playing() const = 0;
};

/// The extension point for one plugin.  `find_port` finds the endpoint a
/// replay writes to, as `generator_owner::find_port` does; `slow` is where a
/// recording is read.
std::unique_ptr<timeline_point>
make_timeline_point(const extension_context& context,
                    generator_port_finder find_port,
                    boost::asio::any_io_executor slow);

} // namespace vss::plugin::host
