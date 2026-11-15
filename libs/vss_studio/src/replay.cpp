//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>

#include <vss/studio/replay.hpp>

#include "timed_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace vss
{

struct replay_player::state : std::enable_shared_from_this<state>
{
    std::shared_ptr<detail::timed_queue> queue;
    std::shared_ptr<endpoint> target;
    /// The records to send, sorted by offset.
    std::vector<capture_record> records;
    std::int64_t period_ms = 1;
    double speed = 1.0;
    bool loop = false;
    time_point origin{};

    std::atomic<bool> stopped{false};
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> sent_bytes{0};
    std::atomic<std::uint64_t> pass_count{0};
    std::atomic<std::uint64_t> errors{0};
    /// Bytes the endpoint has written, and bytes of records it refused; both
    /// are counted when the write's handler runs.
    std::atomic<std::uint64_t> written_bytes{0};
    std::atomic<std::uint64_t> failed_bytes{0};

    /// The time a capture offset has in this run.
    time_point when(std::uint64_t pass, std::int64_t offset_ms) const
    {
        const double total_ms =
            static_cast<double>(pass) * static_cast<double>(period_ms) +
            static_cast<double>(offset_ms);
        return origin + std::chrono::microseconds(
                            std::llround(total_ms * 1000.0 / speed));
    }

    /// Queues one pass through the capture.  The last action of a pass starts
    /// the next, so a loop never holds more than two passes at once.
    void schedule_pass(std::uint64_t pass)
    {
        VSS_PROFILE_ZONE("studio.replay.schedule_pass");
        auto self = shared_from_this();
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            queue->at(when(pass, records[index].offset_ms),
                      [self, index]
                      {
                          if (self->stopped.load())
                              return;
                          const auto& record = self->records[index];
                          const std::size_t size = record.data.size();
                          self->target->write(
                              record.data,
                              [self, size](boost::system::error_code error,
                                           std::size_t)
                              {
                                  if (error)
                                  {
                                      self->errors.fetch_add(1);
                                      self->failed_bytes.fetch_add(size);
                                  }
                                  else
                                  {
                                      self->written_bytes.fetch_add(size);
                                  }
                              });
                          self->sent.fetch_add(1);
                          self->sent_bytes.fetch_add(size);
                      });
        }

        queue->at(when(pass, records.back().offset_ms),
                  [self, pass]
                  {
                      if (self->stopped.load())
                          return;
                      self->pass_count.fetch_add(1);
                      if (self->loop)
                          self->schedule_pass(pass + 1);
                      else
                          self->done.store(true);
                  });
    }
};

replay_player::replay_player(std::shared_ptr<state> shared)
    : state_(std::move(shared))
{
}

replay_player::~replay_player()
{
    try
    {
        stop();
    }
    catch (...)
    {
        // best effort: a destructor must not throw, and nothing can act on a
        // failed stop
    }
}

result<std::shared_ptr<replay_player>>
replay_player::start(hub& engine, std::shared_ptr<endpoint> target,
                     const std::vector<capture_record>& records,
                     const replay_options& options)
{
    VSS_PROFILE_ZONE("studio.replay.start");
    VSS_PROFILE_MESSAGE("studio.replay.started");
    if (!target || !(options.speed > 0.0) || !std::isfinite(options.speed))
        return make_error_code(errc::invalid_argument);

    auto shared = std::make_shared<state>();
    shared->speed = options.speed;
    shared->loop = options.loop;

    // A loop lasts as long as the capture did, whichever direction its last
    // record has, so the pace of the recording is kept from pass to pass.
    std::int64_t last_offset = 0;
    for (const auto& record : records)
    {
        last_offset = std::max(last_offset, record.offset_ms);
        if (record.direction != options.direction)
            continue;
        if (!options.from_port.empty() && record.port != options.from_port)
            continue;
        if (record.data.empty())
            continue;
        shared->records.push_back(record);
    }
    if (shared->records.empty())
        return make_error_code(errc::invalid_argument);

    std::stable_sort(shared->records.begin(), shared->records.end(),
                     [](const capture_record& a, const capture_record& b)
                     { return a.offset_ms < b.offset_ms; });
    shared->period_ms = std::max<std::int64_t>(last_offset, 1);
    shared->target = std::move(target);
    shared->queue = std::make_shared<detail::timed_queue>(engine.executor());
    shared->origin = clock_type::now();
    shared->schedule_pass(0);

    return std::shared_ptr<replay_player>(new replay_player(std::move(shared)));
}

void replay_player::stop()
{
    VSS_PROFILE_ZONE("studio.replay.stop");
    state_->stopped.store(true);
    state_->queue->stop();
}

bool replay_player::finished() const
{
    return state_->done.load();
}

std::uint64_t replay_player::records_sent() const
{
    return state_->sent.load();
}

std::uint64_t replay_player::bytes_sent() const
{
    return state_->sent_bytes.load();
}

std::uint64_t replay_player::passes() const
{
    return state_->pass_count.load();
}

std::uint64_t replay_player::write_errors() const
{
    return state_->errors.load();
}

std::uint64_t replay_player::bytes_written() const
{
    return state_->written_bytes.load();
}

std::uint64_t replay_player::bytes_failed() const
{
    return state_->failed_bytes.load();
}

std::size_t replay_player::records_per_pass() const
{
    return state_->records.size();
}

} // namespace vss
