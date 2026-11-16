//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// See generator_feed.hpp.
//

#include <vss/plugin/host/generator_feed.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace vss::plugin::host
{

namespace
{

constexpr std::uint32_t max_window_batches = 4096;

std::uint32_t clamp_part(std::uint32_t part) noexcept
{
    return part < 1 || part > max_feed_scale_part ? 1 : part;
}

} // namespace

const char* to_string(feed_refusal reason) noexcept
{
    switch (reason)
    {
    case feed_refusal::not_running:
        return "not_running";
    case feed_refusal::empty:
        return "empty";
    case feed_refusal::too_large:
        return "too_large";
    case feed_refusal::not_declared:
        return "not_declared";
    case feed_refusal::offset_out_of_range:
        return "offset_out_of_range";
    case feed_refusal::out_of_order:
        return "out_of_order";
    case feed_refusal::no_credit:
        return "no_credit";
    case feed_refusal::no_batch_credit:
        return "no_batch_credit";
    }
    return "unknown";
}

const char* to_string(feed_end_reason reason) noexcept
{
    switch (reason)
    {
    case feed_end_reason::stopped:
        return "stopped";
    case feed_end_reason::done:
        return "done";
    case feed_end_reason::misbehaving:
        return "misbehaving";
    case feed_end_reason::plugin_gone:
        return "plugin_gone";
    case feed_end_reason::port_failed:
        return "port_failed";
    }
    return "unknown";
}

generator_feed::generator_feed(feed_options options)
    : options_(options)
{
    options_.window_bytes =
        std::clamp<std::uint64_t>(options_.window_bytes, 1, max_feed_window);
    options_.window_batches =
        std::clamp<std::uint32_t>(options_.window_batches, 1, max_window_batches);
    options_.max_batch_bytes = std::clamp<std::uint64_t>(
        options_.max_batch_bytes, 1, options_.window_bytes);
    options_.grant_threshold = std::clamp<std::uint64_t>(
        options_.grant_threshold, 1, options_.window_bytes);
    options_.refusals_to_end = std::max<std::uint32_t>(options_.refusals_to_end, 1);
    options_.time_scale.num = clamp_part(options_.time_scale.num);
    options_.time_scale.den = clamp_part(options_.time_scale.den);
    if (options_.max_run.count() < 0)
    {
        options_.max_run = std::chrono::nanoseconds{0};
    }
    if (options_.late_tolerance.count() < 0)
    {
        options_.late_tolerance = std::chrono::nanoseconds{0};
    }
    if (options_.retry_after.count() < 1)
    {
        options_.retry_after = std::chrono::milliseconds{1};
    }
}

feed_stats generator_feed::stats() const noexcept
{
    return stats_;
}

feed_output generator_feed::start(feed_time now)
{
    feed_output out;
    if (state_ != feed_state::created)
    {
        out.wake = wake_;
        return out;
    }
    state_ = feed_state::running;
    start_ = now;
    stats_.credit_bytes = options_.window_bytes;
    stats_.credit_batches = options_.window_batches;
    out.credit = feed_credit{options_.window_bytes, options_.window_batches};
    return out;
}

std::optional<feed_refusal>
generator_feed::schedule(const feed_batch& batch,
                         std::optional<feed_time>& due) const
{
    due.reset();
    if (options_.timing == feed_timing::async)
    {
        if (batch.offset.has_value())
        {
            return feed_refusal::not_declared;
        }
        return std::nullopt;
    }
    if (!batch.offset.has_value())
    {
        return feed_refusal::not_declared;
    }
    const auto offset_ns = batch.offset->count();
    if (offset_ns < 0)
    {
        return feed_refusal::offset_out_of_range;
    }
    // offset * num / den, rounded down, without overflow: the whole part
    // of the division is checked against the longest run first.
    const auto offset = static_cast<std::uint64_t>(offset_ns);
    const std::uint64_t num = options_.time_scale.num;
    const std::uint64_t den = options_.time_scale.den;
    const auto longest = static_cast<std::uint64_t>(options_.max_run.count());
    const std::uint64_t whole = offset / den;
    const std::uint64_t part = offset % den;
    if (whole > longest / num)
    {
        return feed_refusal::offset_out_of_range;
    }
    const std::uint64_t scaled = whole * num + part * num / den;
    if (scaled > longest)
    {
        return feed_refusal::offset_out_of_range;
    }
    due = start_ + std::chrono::duration_cast<feed_time::duration>(
                       std::chrono::nanoseconds{
                           static_cast<std::chrono::nanoseconds::rep>(scaled)});
    return std::nullopt;
}

void generator_feed::refuse(feed_refusal reason, feed_output& out)
{
    ++stats_.refused_batches;
    ++stats_.refused_by_reason[static_cast<std::size_t>(reason)];
    if (state_ == feed_state::ended)
    {
        return;
    }
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        stats_.refused_batches, std::numeric_limits<std::uint32_t>::max()));
    out.events.emplace_back(feed_refused_event{reason, count});
    if (count >= options_.refusals_to_end)
    {
        std::string detail = "refused ";
        detail.append(std::to_string(count));
        detail.append(" batches, the last: ");
        detail.append(to_string(reason));
        finish(feed_end_reason::misbehaving, std::move(detail), out);
    }
}

void generator_feed::finish(feed_end_reason reason, std::string detail,
                            feed_output& out)
{
    if (state_ == feed_state::ended)
    {
        return;
    }
    state_ = feed_state::ended;
    queue_.clear();
    stats_.held_batches = 0;
    stats_.held_bytes = 0;
    wake_.reset();
    retry_at_.reset();
    out.events.emplace_back(feed_ended_event{reason, std::move(detail)});
}

void generator_feed::grant(feed_output& out)
{
    if (state_ != feed_state::running || stats_.owed_bytes == 0)
    {
        return;
    }
    const bool enough = stats_.owed_bytes >= options_.grant_threshold ||
                        queue_.empty() || stats_.credit_bytes == 0 ||
                        stats_.credit_batches == 0;
    if (!enough)
    {
        return;
    }
    out.credit = feed_credit{stats_.owed_bytes, stats_.owed_batches};
    stats_.credit_bytes += stats_.owed_bytes;
    stats_.credit_batches += stats_.owed_batches;
    stats_.owed_bytes = 0;
    stats_.owed_batches = 0;
}

void generator_feed::pump(feed_time now, port_writer& port, feed_output& out)
{
    while (!queue_.empty())
    {
        held_batch& head = queue_.front();
        if (head.due.has_value() && *head.due > now)
        {
            break;
        }
        if (retry_at_.has_value() && now < *retry_at_)
        {
            break;
        }
        const bytes_view rest =
            bytes_view{head.data}.subspan(head.written);
        const port_write_result result = port.write(options_.port, rest);
        if (result.failed)
        {
            finish(feed_end_reason::port_failed, "the port can take no more",
                   out);
            return;
        }
        const std::size_t taken =
            result.not_ready ? 0 : std::min(result.taken, rest.size());
        if (taken > 0)
        {
            if (!head.counted)
            {
                head.counted = true;
                if (head.due.has_value())
                {
                    const auto lateness =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            now - *head.due);
                    if (lateness > options_.late_tolerance)
                    {
                        ++stats_.late_batches;
                        stats_.max_lateness =
                            std::max(stats_.max_lateness, lateness);
                    }
                }
            }
            head.written += taken;
            stats_.held_bytes -= taken;
            stats_.owed_bytes += taken;
            stats_.written_bytes += taken;
            out.written += taken;
            retry_at_.reset();
        }
        if (head.written == head.data.size())
        {
            ++stats_.written_batches;
            ++stats_.owed_batches;
            --stats_.held_batches;
            queue_.pop_front();
            continue;
        }
        retry_at_ = now + std::chrono::duration_cast<feed_time::duration>(
                              options_.retry_after);
        break;
    }

    if (queue_.empty())
    {
        retry_at_.reset();
        wake_.reset();
        if (state_ == feed_state::draining)
        {
            finish(feed_end_reason::done, "the plugin is done", out);
        }
        return;
    }
    const held_batch& head = queue_.front();
    std::optional<feed_time> wake = head.due;
    if (retry_at_.has_value())
    {
        wake = wake.has_value() ? std::max(*wake, *retry_at_) : *retry_at_;
    }
    wake_ = wake;
}

feed_output generator_feed::on_batch(feed_batch batch, feed_time now,
                                     port_writer& port)
{
    feed_output out;
    if (state_ != feed_state::running)
    {
        refuse(feed_refusal::not_running, out);
        out.wake = wake_;
        return out;
    }
    const std::size_t size = batch.data.size();
    std::optional<feed_time> due;
    std::optional<feed_refusal> why;
    if (size == 0)
    {
        why = feed_refusal::empty;
    }
    else if (size > options_.max_batch_bytes)
    {
        why = feed_refusal::too_large;
    }
    else
    {
        why = schedule(batch, due);
    }
    if (!why.has_value() && due.has_value() && last_due_.has_value() &&
        *due < *last_due_)
    {
        why = feed_refusal::out_of_order;
    }
    if (!why.has_value() && size > stats_.credit_bytes)
    {
        why = feed_refusal::no_credit;
    }
    if (!why.has_value() && stats_.credit_batches == 0)
    {
        why = feed_refusal::no_batch_credit;
    }
    if (why.has_value())
    {
        refuse(*why, out);
        out.wake = wake_;
        return out;
    }

    stats_.credit_bytes -= size;
    --stats_.credit_batches;
    stats_.held_bytes += size;
    ++stats_.held_batches;
    ++stats_.accepted_batches;
    stats_.accepted_bytes += size;
    if (due.has_value())
    {
        last_due_ = due;
    }
    held_batch held;
    held.data = std::move(batch.data);
    held.due = due;
    queue_.push_back(std::move(held));
    pump(now, port, out);
    grant(out);
    out.wake = wake_;
    return out;
}

feed_output generator_feed::on_time(feed_time now, port_writer& port)
{
    feed_output out;
    if (state_ == feed_state::running || state_ == feed_state::draining)
    {
        pump(now, port, out);
        grant(out);
    }
    out.wake = wake_;
    return out;
}

feed_output generator_feed::on_port_ready(feed_time now, port_writer& port)
{
    retry_at_.reset();
    return on_time(now, port);
}

feed_output generator_feed::on_done(feed_time now, port_writer& port)
{
    feed_output out;
    if (state_ == feed_state::running)
    {
        state_ = feed_state::draining;
        pump(now, port, out);
    }
    else if (state_ != feed_state::ended)
    {
        refuse(feed_refusal::not_running, out);
    }
    out.wake = wake_;
    return out;
}

feed_output generator_feed::stop()
{
    feed_output out;
    finish(feed_end_reason::stopped, "stopped by the host", out);
    return out;
}

feed_output generator_feed::on_plugin_gone()
{
    feed_output out;
    finish(feed_end_reason::plugin_gone, "the plugin went away", out);
    return out;
}

} // namespace vss::plugin::host
