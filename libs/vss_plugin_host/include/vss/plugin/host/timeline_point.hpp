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
// - A recording is read as it plays, a block ahead, so its size is not
//   limited by memory.  A read that fails part way ends the replay and is
//   counted.
// - An instance plays one recording at a time.  A second `timeline.load`
//   for it while one plays is answered `busy`.
// - A replay ends by itself after its last chunk, when the instance stops or
//   the plugin goes away.
// - Played: the bytes the recorded place received (`R` lines) and, where the
//   port follows control lines, the lines the device drove (CTS, DSR, DCD, RI)
//   after each recorded change.  Not played, and counted: faults, line
//   changes on a port that does not follow control lines, and the bytes the
//   recorded place sent (`W` lines), which the program wrote and not the
//   device.  A request with `include_sent` true plays the `W` lines too, in
//   recorded order, and counts none of them as skipped.
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

/// A replay reads ahead until this many bytes wait to be played, and reads
/// at most this many bytes in one go.  A choice.
inline constexpr std::uint64_t timeline_read_ahead_bytes = 1ULL << 20;
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
    /// Control line changes applied to a port.
    std::uint64_t line_changes = 0;
    /// Not replayed: faults, line changes the port cannot follow, and chunks
    /// of bytes the recorded place sent.
    std::uint64_t faults_skipped = 0;
    std::uint64_t lines_skipped = 0;
    std::uint64_t sent_skipped = 0;
    /// Writes an endpoint failed, and replays that ended on a read error.
    std::uint64_t write_errors = 0;
    std::uint64_t read_errors = 0;
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
