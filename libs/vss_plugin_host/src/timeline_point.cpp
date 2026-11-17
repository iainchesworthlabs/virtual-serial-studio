//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/timeline_point.hpp>

#include <vss/core/bytes.hpp>
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

/// What is read from a recording: the chunks the place received.
struct loaded_timeline
{
    /// Why it could not be loaded; empty when it could.
    std::string problem;
    std::vector<std::uint64_t> times;
    std::vector<std::size_t> ends;
    byte_buffer bytes;
};

/// Reads the recording and keeps the `R` lines of one place.  Runs on the
/// slow executor and touches no shared state.
loaded_timeline load_timeline(const std::filesystem::path& folder,
                              const std::string& place)
{
    loaded_timeline out;
    std::string detail;
    const auto found = read_recording(folder, &detail);
    if (!found)
    {
        out.problem = detail.empty() ? "the recording could not be read"
                                     : detail;
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
    if (index.places[*chosen].bytes > max_timeline_bytes)
    {
        out.problem = "the recording holds more traffic than a replay keeps";
        return out;
    }
    auto reader = found->open_place(*chosen);
    if (!reader)
    {
        out.problem = "the recording could not be read";
        return out;
    }
    for (;;)
    {
        auto next = reader->next();
        if (!next)
        {
            out.problem = std::string(reader->detail());
            if (out.problem.empty())
                out.problem = "the recording could not be read";
            return out;
        }
        if (!next->has_value())
            break;
        const recording_event& event = **next;
        if (event.kind != recording_event_kind::data ||
            event.direction != data_direction::rx)
            continue;
        if (out.times.size() >= max_timeline_items ||
            out.bytes.size() + event.bytes.size() > max_timeline_bytes)
        {
            out.problem = "the recording is larger than a replay keeps";
            return out;
        }
        out.times.push_back(event.t_ns);
        out.bytes.insert(out.bytes.end(), event.bytes.begin(),
                         event.bytes.end());
        out.ends.push_back(out.bytes.size());
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
        stop_all();
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
            std::shared_ptr<endpoint> port_, loaded_timeline data_,
            double scale_, std::uint64_t id_)
            : timer(executor), port(std::move(port_)), data(std::move(data_)),
              scale(scale_), id(id_)
        {
        }

        boost::asio::steady_timer timer;
        std::shared_ptr<endpoint> port;
        loaded_timeline data;
        double scale;
        std::uint64_t id;
        clock_type::time_point origin;
        std::size_t next = 0;
        std::uint64_t outstanding = 0;
        bool waiting_for_port = false;
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
             port = std::move(port)]() mutable
            {
                loaded_timeline data;
                try
                {
                    data = load_timeline(folder, place);
                }
                catch (...)
                {
                    data.problem = "the recording could not be read";
                }
                boost::asio::post(
                    executor,
                    [alive, this, id, instance, scale, port = std::move(port),
                     data = std::move(data)]() mutable
                    {
                        if (!*alive)
                            return;
                        loaded(id, instance, scale, std::move(port),
                               std::move(data));
                    });
            });
    }

    void loaded(std::uint64_t id, std::uint64_t instance, double scale,
                std::shared_ptr<endpoint> port, loaded_timeline data)
    {
        loading_.erase(instance);
        if (!context_.instances || !context_.instances->running(instance))
        {
            refuse(id, api_errc::plugin_unavailable,
                   "the instance stopped before the recording was read");
            return;
        }
        if (!data.problem.empty())
        {
            refuse(id, api_errc::refused, data.problem);
            return;
        }
        auto made = std::make_unique<run>(context_.executor, std::move(port),
                                          std::move(data), scale, ++numbers_);
        run* const current = made.get();
        runs_.emplace(instance, std::move(made));
        ++counts_.started;
        answer(id, ok_answer());
        // The times count from the answer.
        current->origin = clock_type::now();
        step(instance, current->id);
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

    /// Writes every chunk that is due, then waits for the next.
    void step(std::uint64_t instance, std::uint64_t number)
    {
        const auto found = runs_.find(instance);
        if (found == runs_.end() || found->second->id != number)
            return;
        run& current = *found->second;
        const loaded_timeline& data = current.data;
        while (current.next < data.times.size())
        {
            const auto scaled = static_cast<std::uint64_t>(
                std::floor(static_cast<double>(data.times[current.next]) *
                           current.scale));
            const auto due =
                current.origin + std::chrono::nanoseconds(scaled);
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
            if (current.outstanding >= max_timeline_window)
            {
                // The port has not taken what it was given: the chunk goes
                // when it has, and is late.
                current.waiting_for_port = true;
                return;
            }
            const std::size_t from =
                current.next == 0 ? 0 : data.ends[current.next - 1];
            const std::size_t to = data.ends[current.next];
            const auto late = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - due)
                    .count());
            counts_.latest_ns = std::max(counts_.latest_ns, late);
            ++counts_.chunks;
            counts_.bytes += to - from;
            ++current.next;
            if (to == from)
                continue;
            current.outstanding += to - from;
            byte_buffer copy(
                data.bytes.begin() + static_cast<std::ptrdiff_t>(from),
                data.bytes.begin() + static_cast<std::ptrdiff_t>(to));
            current.port->write(
                std::move(copy),
                [alive = alive_, this, executor = context_.executor, instance,
                 number, count = to - from](boost::system::error_code ec,
                                            std::size_t)
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
