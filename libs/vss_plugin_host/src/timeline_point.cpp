//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/timeline_point.hpp>

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/plugin/api_capture.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/recording.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vss::plugin::host
{

namespace
{

using clock_type = std::chrono::steady_clock;

/// The most items one read takes, besides the bytes.
constexpr std::size_t read_items = 8192;

/// One thing to play, at a recorded time.
struct item
{
    std::uint64_t t_ns = 0;
    /// Empty for a change of the control lines.
    byte_buffer bytes;
    bool is_lines = false;
    control_lines inputs;
};

/// What one read of the recording found.
struct batch
{
    /// Why the read failed; empty when it did not.
    std::string problem;
    std::vector<item> items;
    std::uint64_t bytes = 0;
    bool at_end = false;
    /// What the read passed over.
    std::uint64_t faults = 0;
    std::uint64_t sent = 0;
};

/// The reader of one place.  Only the slow executor touches it, one read at a
/// time.
struct source
{
    place_reader reader;
    /// Whether the bytes the place sent are played too.
    bool include_sent = false;
};

batch read_some(source& from)
{
    batch out;
    while (out.items.size() < read_items &&
           out.bytes < timeline_read_ahead_bytes)
    {
        auto next = from.reader.next();
        if (!next)
        {
            out.problem = std::string(from.reader.detail());
            if (out.problem.empty())
                out.problem = "the recording could not be read";
            return out;
        }
        auto& found = *next;
        if (!found.has_value())
        {
            out.at_end = true;
            return out;
        }
        recording_event& event = *found;
        switch (event.kind)
        {
        case recording_event_kind::data:
        {
            if (event.direction != data_direction::rx && !from.include_sent)
            {
                ++out.sent;
                break;
            }
            item made;
            made.t_ns = event.t_ns;
            made.bytes = std::move(event.bytes);
            out.bytes += made.bytes.size();
            out.items.push_back(std::move(made));
            break;
        }
        case recording_event_kind::line:
        {
            item made;
            made.t_ns = event.t_ns;
            made.is_lines = true;
            made.inputs = control_lines{event.lines.inputs()};
            out.items.push_back(std::move(made));
            break;
        }
        case recording_event_kind::fault:
            ++out.faults;
            break;
        case recording_event_kind::dropped:
            break;
        }
    }
    return out;
}

/// What opening a recording gave: a reader and its first read, or why not.
struct opened
{
    std::string problem;
    std::shared_ptr<source> from;
    batch first;
};

/// Checks the recording whole, as `read_recording` does, then opens the place
/// that is to be played and reads the first block.  Runs on the slow
/// executor and touches no shared state.
opened open_timeline(const std::filesystem::path& folder,
                     const std::string& place, bool include_sent)
{
    opened out;
    std::string detail;
    const auto found = read_recording(folder, &detail);
    if (!found)
    {
        out.problem =
            detail.empty() ? "the recording could not be read" : detail;
        return out;
    }
    const recording_index& index = found->index;
    std::optional<std::size_t> chosen;
    for (std::size_t i = 0; i < index.places.size(); ++i)
    {
        if (index.places[i].name == place)
            chosen = i;
    }
    if (!chosen && index.places.size() == 1)
        chosen = 0;
    if (!chosen)
    {
        out.problem = "the recording has no place named " + place;
        return out;
    }
    auto reader = found->open_place(*chosen);
    if (!reader)
    {
        out.problem = "the recording could not be read";
        return out;
    }
    out.from = std::make_shared<source>(source{std::move(*reader), include_sent});
    out.first = read_some(*out.from);
    if (!out.first.problem.empty())
    {
        out.problem = out.first.problem;
        out.from.reset();
    }
    return out;
}

class timeline_handler final : public timeline_point
{
public:
    timeline_handler(extension_context context, generator_port_finder find,
                     boost::asio::any_io_executor slow)
        : context_(std::move(context)), find_(std::move(find)),
          slow_(std::move(slow)), alive_(std::make_shared<bool>(true))
    {
    }

    ~timeline_handler() override
    {
        *alive_ = false;
        try
        {
            stop_all();
        }
        catch (...)
        {
            // best effort: nothing can be done about a failure to stop while
            // the object is being destroyed.
        }
    }

    bool take(const api_message& message) override
    {
        if (message.op != timeline_load_message::op())
            return false;
        load(message);
        return true;
    }

    void instances_changed() override
    {
        for (auto it = runs_.begin(); it != runs_.end();)
        {
            if (!context_.instances ||
                !context_.instances->running(it->first))
            {
                it->second->timer.cancel();
                it = runs_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void plugin_gone() override { stop_all(); }

    timeline_counts counts() const override { return counts_; }
    std::size_t playing() const override { return runs_.size(); }

private:
    /// One replay.
    struct run
    {
        run(const boost::asio::any_io_executor& executor,
            std::shared_ptr<endpoint> port_in, std::shared_ptr<source> from_in,
            double scale_in, std::uint64_t id_in)
            : timer(executor), port(std::move(port_in)),
              from(std::move(from_in)), scale(scale_in), id(id_in)
        {
        }

        boost::asio::steady_timer timer;
        std::shared_ptr<endpoint> port;
        std::shared_ptr<source> from;
        double scale;
        std::uint64_t id;
        clock_type::time_point origin;
        std::deque<item> queue;
        std::uint64_t queued_bytes = 0;
        bool at_end = false;
        bool reading = false;
        bool follows_lines = false;
        bool waiting_for_data = false;
        bool waiting_for_port = false;
        std::uint64_t outstanding = 0;
    };

    void answer(std::uint64_t id, api_message message)
    {
        if (id != 0)
            context_.link.answer(id, std::move(message));
    }

    void refuse(std::uint64_t id, api_errc code, const std::string& text)
    {
        ++counts_.refused;
        answer(id, error_answer(code, text));
    }

    void stop_all()
    {
        for (auto& each : runs_)
            each.second->timer.cancel();
        runs_.clear();
    }

    void load(const api_message& message)
    {
        const auto asked = decode_as<timeline_load_message>(message);
        if (!asked)
        {
            refuse(message.id, api_errc::bad_request,
                   "the timeline was not asked for as it must be");
            return;
        }
        if (!context_.may(permission::ports_write))
        {
            refuse(message.id, api_errc::not_granted,
                   "the plugin may not write to ports");
            return;
        }
        const auto found = context_.instances
                               ? context_.instances->find(asked->instance)
                               : std::nullopt;
        if (!found || !context_.instances->running(asked->instance) ||
            (found->spec.kind != extension_kind::generator &&
             found->spec.kind != extension_kind::fixture))
        {
            refuse(message.id, api_errc::refused,
                   "the instance is not a running generator or fixture");
            return;
        }
        if (runs_.contains(asked->instance) ||
            loading_.contains(asked->instance))
        {
            refuse(message.id, api_errc::busy,
                   "the instance is playing a recording already");
            return;
        }
        std::shared_ptr<endpoint> port;
        if (find_)
            port = find_(asked->place);
        if (!port)
        {
            refuse(message.id, api_errc::refused,
                   "there is no port named " + asked->place +
                       " that the plugin may write to");
            return;
        }
        loading_.insert(asked->instance);
        const std::filesystem::path folder =
            context_.data_directory / std::filesystem::path(asked->recording);
        boost::asio::post(
            slow_,
            [alive = alive_, this, executor = context_.executor,
             id = message.id, instance = asked->instance,
             scale = asked->time_scale, folder, place = asked->place,
             include_sent = asked->include_sent,
             port = std::move(port)]() mutable
            {
                opened result;
                try
                {
                    result = open_timeline(folder, place, include_sent);
                }
                catch (...)
                {
                    result = opened{};
                    result.problem = "the recording could not be read";
                }
                boost::asio::post(
                    executor,
                    [alive, this, id, instance, scale, port = std::move(port),
                     result = std::move(result)]() mutable
                    {
                        if (!*alive)
                            return;
                        loaded(id, instance, scale, std::move(port),
                               std::move(result));
                    });
            });
    }

    void loaded(std::uint64_t id, std::uint64_t instance, double scale,
                std::shared_ptr<endpoint> port, opened result)
    {
        loading_.erase(instance);
        if (!context_.instances || !context_.instances->running(instance))
        {
            refuse(id, api_errc::plugin_unavailable,
                   "the instance stopped before the recording was read");
            return;
        }
        if (!result.problem.empty())
        {
            refuse(id, api_errc::refused, result.problem);
            return;
        }
        auto made = std::make_unique<run>(context_.executor, std::move(port),
                                          std::move(result.from), scale,
                                          ++numbers_);
        run* const current = made.get();
        current->follows_lines = current->port->info().supports_control_lines;
        runs_.emplace(instance, std::move(made));
        ++counts_.started;
        take_batch(*current, std::move(result.first));
        answer(id, ok_answer());
        // The times count from the answer.
        current->origin = clock_type::now();
        step(instance, current->id);
    }

    /// Adds what a read found to the replay's queue.
    void take_batch(run& current, batch got)
    {
        counts_.faults_skipped += got.faults;
        counts_.sent_skipped += got.sent;
        current.at_end = got.at_end;
        current.queued_bytes += got.bytes;
        for (item& each : got.items)
            current.queue.push_back(std::move(each));
    }

    /// Asks for the next block when the queue is running low.
    void read_ahead(std::uint64_t instance, run& current)
    {
        if (current.reading || current.at_end ||
            current.queued_bytes >= timeline_read_ahead_bytes / 2)
            return;
        current.reading = true;
        boost::asio::post(
            slow_,
            [alive = alive_, this, executor = context_.executor, instance,
             number = current.id, from = current.from]
            {
                batch got;
                try
                {
                    got = read_some(*from);
                }
                catch (...)
                {
                    got.problem = "the recording could not be read";
                }
                boost::asio::post(
                    executor,
                    [alive, this, instance, number, got = std::move(got)]() mutable
                    {
                        if (*alive)
                            read_done(instance, number, std::move(got));
                    });
            });
    }

    void read_done(std::uint64_t instance, std::uint64_t number, batch got)
    {
        const auto found = runs_.find(instance);
        if (found == runs_.end() || found->second->id != number)
            return;
        run& current = *found->second;
        current.reading = false;
        if (!got.problem.empty())
        {
            // The file changed or failed part way: what was queued is played,
            // and nothing more.
            ++counts_.read_errors;
            current.at_end = true;
            got = batch{};
            got.at_end = true;
        }
        take_batch(current, std::move(got));
        if (current.waiting_for_data)
        {
            current.waiting_for_data = false;
            step(instance, number);
        }
    }

    void on_taken(std::uint64_t instance, std::uint64_t number,
                  std::size_t count, bool failed)
    {
        const auto found = runs_.find(instance);
        if (found == runs_.end() || found->second->id != number)
            return;
        run& current = *found->second;
        current.outstanding -=
            std::min<std::uint64_t>(current.outstanding, count);
        if (failed)
            ++counts_.write_errors;
        if (current.waiting_for_port)
        {
            current.waiting_for_port = false;
            step(instance, number);
        }
    }

    void finish(std::uint64_t instance)
    {
        ++counts_.finished;
        runs_.erase(instance);
    }

    /// Plays every item that is due, then waits for the next.
    void step(std::uint64_t instance, std::uint64_t number)
    {
        const auto found = runs_.find(instance);
        if (found == runs_.end() || found->second->id != number)
            return;
        run& current = *found->second;
        for (;;)
        {
            read_ahead(instance, current);
            if (current.queue.empty())
            {
                if (current.at_end)
                    break;
                // The next block is not here yet: whatever comes is late.
                current.waiting_for_data = true;
                return;
            }
            const item& next = current.queue.front();
            const auto scaled = static_cast<std::uint64_t>(
                std::floor(static_cast<double>(next.t_ns) * current.scale));
            const auto due = current.origin + std::chrono::nanoseconds(scaled);
            const auto now = clock_type::now();
            if (now < due)
            {
                current.timer.expires_at(due);
                current.timer.async_wait(
                    [alive = alive_, this, instance,
                     number](const boost::system::error_code& ec)
                    {
                        if (*alive && !ec)
                            step(instance, number);
                    });
                return;
            }
            if (!next.is_lines && current.outstanding >= max_timeline_window)
            {
                // The port has not taken what it was given: the chunk goes
                // when it has, and is late.
                current.waiting_for_port = true;
                return;
            }
            const auto late = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - due)
                    .count());
            counts_.latest_ns = std::max(counts_.latest_ns, late);
            item played = std::move(current.queue.front());
            current.queue.pop_front();
            if (played.is_lines)
            {
                if (current.follows_lines)
                {
                    current.port->set_peer_inputs(played.inputs);
                    ++counts_.line_changes;
                }
                else
                {
                    ++counts_.lines_skipped;
                }
                continue;
            }
            current.queued_bytes -= std::min<std::uint64_t>(
                current.queued_bytes, played.bytes.size());
            ++counts_.chunks;
            counts_.bytes += played.bytes.size();
            if (played.bytes.empty())
                continue;
            current.outstanding += played.bytes.size();
            const std::size_t count = played.bytes.size();
            current.port->write(
                std::move(played.bytes),
                [alive = alive_, this, executor = context_.executor, instance,
                 number, count](boost::system::error_code ec, std::size_t)
                {
                    const bool failed =
                        ec && ec != boost::asio::error::operation_aborted;
                    boost::asio::post(executor,
                                      [alive, this, instance, number, count,
                                       failed]
                                      {
                                          if (*alive)
                                              on_taken(instance, number, count,
                                                       failed);
                                      });
                });
        }
        finish(instance);
    }

    extension_context context_;
    generator_port_finder find_;
    boost::asio::any_io_executor slow_;
    std::shared_ptr<bool> alive_;
    std::map<std::uint64_t, std::unique_ptr<run>> runs_;
    std::set<std::uint64_t> loading_;
    std::uint64_t numbers_ = 0;
    timeline_counts counts_;
};

} // namespace

std::unique_ptr<timeline_point>
make_timeline_point(const extension_context& context,
                    generator_port_finder find_port,
                    boost::asio::any_io_executor slow)
{
    return std::make_unique<timeline_handler>(context, std::move(find_port),
                                              std::move(slow));
}

} // namespace vss::plugin::host
