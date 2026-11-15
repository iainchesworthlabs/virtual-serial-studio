//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/endpoint.hpp>
#include <vss/core/error.hpp>
#include <vss/core/hub.hpp>
#include <vss/studio/capture.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vss
{

struct replay_options
{
    /// Which records to send: `rx` sends the `R` records (what a port
    /// received, which is what the program on it transmitted), `tx` sends the
    /// `W` records.
    data_direction direction = data_direction::rx;
    /// Playback speed: 2 plays twice as fast, 0.5 half as fast.  Must be above
    /// zero.
    double speed = 1.0;
    /// Start again from the first record when the last has been sent.
    bool loop = false;
    /// Only the records that were captured on this port.  Empty takes every
    /// port, which is right for a `.cap` file, which has no port names.
    std::string from_port;
};

/// Writes the records of a capture into one endpoint at the times they were
/// recorded, so a recorded master or device can drive a bus without the
/// original program.
///
/// A record is sent with `endpoint::write` when its offset (divided by the
/// speed) has passed since `start`.  The write goes out of the endpoint's
/// transport, so what the player stands in for is the program on the far end
/// of that port.  Use it on the end of a port that a program would open, or on
/// a loopback port, whose writes come back in as received bytes.
///
/// A loop lasts as long as the capture did (the offset of its last record, of
/// either direction), so a recording replays at the pace it was taken.
class replay_player
{
public:
    /// `errc::invalid_argument` for a speed that is not above zero, a missing
    /// endpoint, or a capture with nothing to send in the chosen direction.
    static result<std::shared_ptr<replay_player>>
    start(hub& engine, std::shared_ptr<endpoint> target,
          const std::vector<capture_record>& records,
          const replay_options& options = {});

    ~replay_player();

    replay_player(const replay_player&) = delete;
    replay_player& operator=(const replay_player&) = delete;

    /// Stops sending.  Safe to call more than once.
    void stop();

    /// True once every record has been sent (never, for a loop, until stop).
    bool finished() const;

    std::uint64_t records_sent() const;
    /// Bytes handed to the endpoint, which may still be in its queue.
    std::uint64_t bytes_sent() const;
    /// Complete passes through the capture.
    std::uint64_t passes() const;
    /// Writes the endpoint refused.
    std::uint64_t write_errors() const;
    /// Bytes of the records whose write the endpoint has finished.  Only
    /// this player's own writes count, not what a link or a program writes to
    /// the same endpoint.
    std::uint64_t bytes_written() const;
    /// Bytes of the records whose write the endpoint refused or lost.  Once
    /// `bytes_written() + bytes_failed()` reaches `bytes_sent()` every write
    /// has ended.  A write that is dropped when the endpoint closes never
    /// ends, so neither count includes it.
    std::uint64_t bytes_failed() const;

    /// Records that will be sent in one pass.
    std::size_t records_per_pass() const;

private:
    struct state;
    explicit replay_player(std::shared_ptr<state> shared);

    std::shared_ptr<state> state_;
};

} // namespace vss
