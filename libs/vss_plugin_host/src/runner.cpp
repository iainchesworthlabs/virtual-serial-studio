//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/runner.hpp>

#include <vss/agent/proto/frame.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/host/process.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/lifecycle.hpp>
#include <vss/plugin/protocol.hpp>
#include <vss/plugin/session.hpp>

#include "platform/stream_platform.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vss::plugin::host
{

namespace
{

using clock_type = std::chrono::steady_clock;

/// How often a running process is asked whether it has ended, and how
/// often once it is being ended.
constexpr std::chrono::milliseconds watch_interval{250};
constexpr std::chrono::milliseconds end_interval{20};

/// How long a plugin whose channel closed has to be seen dead before the
/// host ends it.
constexpr std::chrono::milliseconds dying_grace{200};

/// How much is read from a plugin at a time.
constexpr std::size_t read_size = 64 * 1024;

} // namespace

class plugin_runner::impl : public std::enable_shared_from_this<impl>
{
public:
    impl(const boost::asio::any_io_executor& executor, runner_options options)
        : strand_(boost::asio::make_strand(executor)),
          options_(std::move(options)), run_(options_.restart, options_.seed),
          session_timer_(strand_), restart_timer_(strand_),
          watch_timer_(strand_), pause_timer_(strand_), end_timer_(strand_)
    {
        publish_now();
    }

    /// Runs `work` on the strand with this object kept alive.
    template <class Work> void post(Work work)
    {
        boost::asio::post(strand_,
                          [self = shared_from_this(),
                           work = std::move(work)]() mutable { work(*self); });
    }

    std::shared_ptr<const runner_snapshot> snapshot() const
    {
        const std::lock_guard lock(snapshot_mutex_);
        return snapshot_;
    }

    // Everything below runs on the strand.

    void set_change_handler(change_handler handler)
    {
        change_handler_ = std::move(handler);
    }

    void set_message_handler(message_handler handler)
    {
        message_handler_ = std::move(handler);
    }

    void start()
    {
        if (abandoned_)
            return;
        apply(run_.start(clock_type::now()));
    }

    void stop(std::string reason)
    {
        if (abandoned_)
            return;
        stop_reason_ = std::move(reason);
        apply(run_.stop(clock_type::now()));
    }

    void abandon()
    {
        abandoned_ = true;
        ++generation_;
        cancel_timers();
        fail_waiting_requests();
        if (process_)
            process_->kill();
        close_streams();
        session_.reset();
        process_.reset();
        process_id_ = 0;
        static_cast<void>(run_.stop(clock_type::now()));
        static_cast<void>(run_.on_stopped(clock_type::now()));
        publish_now();
    }

    void request(api_message message, answer_handler handler,
                 std::chrono::milliseconds timeout)
    {
        if (!session_ || ending_)
        {
            handler(make_error_code(api_errc::plugin_unavailable), {});
            return;
        }
        session_output out;
        const auto id = session_->request(std::move(message), clock_type::now(),
                                          out, timeout);
        if (!id)
        {
            handler(id.error(), {});
            return;
        }
        waiting_[*id] = std::move(handler);
        handle(std::move(out));
    }

    void notify(api_message message)
    {
        if (!session_ || ending_)
            return;
        session_output out;
        static_cast<void>(
            session_->notify(std::move(message), clock_type::now(), out));
        handle(std::move(out));
    }

    void answer(std::uint64_t re, api_message message)
    {
        if (!session_ || ending_)
            return;
        session_output out;
        static_cast<void>(
            session_->answer(re, std::move(message), clock_type::now(), out));
        handle(std::move(out));
    }

    void unknown(const api_message& message)
    {
        if (!session_ || ending_)
            return;
        handle(session_->unknown_op(message, clock_type::now()));
    }

private:
    /// Does what a step of the restart rules asks.
    void apply(const run_step& step)
    {
        restart_timer_.cancel();
        if (step.start_now)
            launch();
        else if (step.stop_now)
            begin_stop();
        else if (step.wake_at)
            wait_to_restart(*step.wake_at);
        publish_soon();
    }

    void wait_to_restart(clock_type::time_point when)
    {
        restart_timer_.expires_at(when);
        restart_timer_.async_wait(
            [self = shared_from_this()](const error_code& cancelled)
            {
                if (cancelled || self->abandoned_)
                    return;
                self->apply(self->run_.on_time(clock_type::now()));
            });
    }

    /// Starts the process and its session.
    void launch()
    {
        ++generation_;
        ending_ = false;
        end_reason_ = error_code{};
        end_detail_.clear();
        api_ = api_version{};
        status_.reset();
        sandbox_ = sandbox_report{};
        ended_by_host_ = false;
        ++output_generation_;

        auto started = plugin_process::start(options_.process);
        if (!started)
        {
            not_started(started.error(), "the process could not be started");
            return;
        }
        process_.emplace(std::move(*started));
        sandbox_ = process_->sandbox();
        process_id_ = process_->id();

        const boost::asio::any_io_executor executor(strand_);
        auto channel =
            platform::adopt_channel(executor, process_->take_channel());
        auto output = platform::adopt_output(executor, process_->take_output());
        if (!channel || !output)
        {
            const error_code failed =
                channel ? output.error() : channel.error();
            process_->kill();
            process_.reset();
            process_id_ = 0;
            not_started(failed, "the plugin's channel could not be used");
            return;
        }
        channel_ = std::move(*channel);
        output_ = std::move(*output);
        session_.emplace(options_.session, clock_type::now());
        arm_session_timer();
        read_channel();
        read_output();
        watch(watch_interval);
    }

    /// A start that made no process.
    void not_started(error_code reason, std::string_view detail)
    {
        last_failure_ = reason;
        detail_ = std::string(detail);
        const auto now = clock_type::now();
        run_step step = run_.on_failed(reason, now);
        // A computer that cannot confine the plugin will not be able to
        // at the next try either.
        if (reason == make_error_code(plugin_errc::sandbox_insufficient))
            step = run_.stop(now);
        apply(step);
    }

    void read_channel()
    {
        channel_->async_read_some(
            boost::asio::buffer(channel_buffer_),
            [self = shared_from_this(), generation = generation_](
                const error_code& failed, std::size_t size)
            {
                if (generation != self->generation_ || !self->session_)
                    return;
                const auto now = clock_type::now();
                if (failed)
                {
                    self->handle(self->session_->on_channel_closed(now));
                    return;
                }
                const std::optional<session_time> again =
                    self->handle(self->session_->on_bytes(
                        bytes_view(self->channel_buffer_.data(), size), now));
                if (generation != self->generation_ || !self->session_ ||
                    self->ending_)
                    return;
                if (again)
                    self->read_channel_at(*again);
                else
                    self->read_channel();
            });
    }

    /// The plugin sent more than it may this second: what it sends next
    /// waits in the channel until `when`, which is what slows it down.
    void read_channel_at(session_time when)
    {
        pause_timer_.expires_at(when);
        pause_timer_.async_wait(
            [self = shared_from_this(),
             generation = generation_](const error_code& cancelled)
            {
                if (cancelled || generation != self->generation_ ||
                    !self->session_ || self->ending_)
                    return;
                self->read_channel();
            });
    }

    void read_output()
    {
        output_->async_read_some(
            boost::asio::buffer(output_buffer_),
            [self = shared_from_this(), generation = output_generation_](
                const error_code& failed, std::size_t size)
            {
                if (generation != self->output_generation_)
                    return;
                if (failed)
                {
                    // The end: what the plugin wrote last has no line
                    // end of its own.
                    if (!self->partial_line_.empty())
                        self->end_output_line();
                    self->output_.reset();
                    self->publish_soon();
                    return;
                }
                self->take_output(size);
                self->read_output();
            });
    }

    /// Standard output and error: cut into lines, held to a number of
    /// bytes each second, and kept as log lines.  It is always read, so
    /// that a plugin never waits on a full pipe.
    void take_output(std::size_t size)
    {
        const auto now = clock_type::now();
        if (now - output_window_ >= std::chrono::seconds{1})
        {
            output_window_ = now;
            output_in_window_ = 0;
        }
        const std::size_t room =
            options_.output_bytes_per_second > output_in_window_
                ? options_.output_bytes_per_second - output_in_window_
                : 0;
        const std::size_t kept = std::min(size, room);
        output_in_window_ += kept;
        output_dropped_ += size - kept;

        for (std::size_t i = 0; i < kept; ++i)
        {
            const char c = output_buffer_[i];
            if (c == '\n')
            {
                end_output_line();
                continue;
            }
            if (partial_line_.size() < options_.longest_output_line)
                partial_line_.push_back(c);
        }
        publish_soon();
    }

    void end_output_line()
    {
        if (!partial_line_.empty() && partial_line_.back() == '\r')
            partial_line_.pop_back();
        add_log(true, log_level::info, std::move(partial_line_));
        partial_line_.clear();
    }

    void add_log(bool from_output, log_level level, std::string text)
    {
        if (options_.kept_log_lines == 0)
            return;
        while (log_.size() >= options_.kept_log_lines)
            log_.pop_front();
        log_.push_back(runner_log_line{from_output, level, std::move(text)});
    }

    /// Does what a call to the session asks: sends its frames and takes
    /// its events.  Returns the time before which the channel is not read
    /// again, when the session gave one.
    std::optional<session_time> handle(session_output out)
    {
        for (const agent::frame& frame : out.send)
        {
            auto bytes = agent::encode_frame(frame);
            if (!bytes)
            {
                finish(bytes.error(), "a message could not be written");
                return std::nullopt;
            }
            send(std::move(*bytes));
        }
        std::vector<api_message> not_known;
        std::optional<closed_event> closed;
        for (session_event& event : out.events)
        {
            if (auto* const ready = std::get_if<ready_event>(&event))
            {
                api_ = ready->api;
                const run_step step = run_.on_ready(clock_type::now());
                static_cast<void>(step);
            }
            else if (auto* const status = std::get_if<status_event>(&event))
            {
                status_ = std::move(status->status);
            }
            else if (auto* const line = std::get_if<log_event>(&event))
            {
                add_log(false, line->line.level, std::move(line->line.text));
            }
            else if (auto* const message = std::get_if<message_event>(&event))
            {
                if (message_handler_)
                    message_handler_(message->message);
                else
                    not_known.push_back(std::move(message->message));
            }
            else if (auto* const answered = std::get_if<answer_event>(&event))
            {
                const auto found = waiting_.find(answered->id);
                if (found != waiting_.end())
                {
                    const answer_handler handler = std::move(found->second);
                    waiting_.erase(found);
                    handler(answered->result, answered->answer);
                }
            }
            else if (auto* const ended = std::get_if<closed_event>(&event))
            {
                closed = std::move(*ended);
            }
        }
        if (closed)
        {
            finish(closed->reason, closed->detail);
            return std::nullopt;
        }
        for (const api_message& message : not_known)
        {
            if (session_ && !ending_)
                handle(session_->unknown_op(message, clock_type::now()));
        }
        arm_session_timer();
        publish_soon();
        return out.read_again_at;
    }

    void arm_session_timer()
    {
        session_timer_.cancel();
        if (!session_ || ending_)
            return;
        const std::optional<session_time> next = session_->next_deadline();
        if (!next)
            return;
        session_timer_.expires_at(*next);
        session_timer_.async_wait(
            [self = shared_from_this(),
             generation = generation_](const error_code& cancelled)
            {
                if (cancelled || generation != self->generation_ ||
                    !self->session_ || self->ending_)
                    return;
                self->handle(self->session_->on_time(clock_type::now()));
            });
    }

    /// Queues bytes for the plugin.  A plugin that does not read cannot
    /// make the host hold more than a limit.
    void send(byte_buffer bytes)
    {
        if (ending_ || !channel_)
            return;
        bytes_waiting_ += bytes.size();
        if (bytes_waiting_ > options_.most_bytes_waiting)
        {
            finish(make_error_code(plugin_errc::unresponsive),
                   "the plugin does not read what it is sent");
            return;
        }
        outgoing_.push_back(std::move(bytes));
        if (!writing_)
            write_next();
    }

    void write_next()
    {
        writing_ = true;
        channel_->async_write(
            boost::asio::buffer(outgoing_.front()),
            [self = shared_from_this(),
             generation = generation_](const error_code& failed, std::size_t)
            {
                if (generation != self->generation_)
                    return;
                self->writing_ = false;
                // A write that fails means the plugin has gone; the read
                // side says so and ends the session.
                if (failed || self->outgoing_.empty())
                    return;
                self->bytes_waiting_ -= self->outgoing_.front().size();
                self->outgoing_.pop_front();
                if (!self->outgoing_.empty())
                    self->write_next();
            });
    }

    /// The host wants the plugin stopped.
    void begin_stop()
    {
        if (session_ && !ending_)
        {
            handle(session_->shut_down(stop_reason_, clock_type::now()));
            return;
        }
        if (!process_)
            apply(run_.on_stopped(clock_type::now()));
    }

    /// The conversation is over, for `reason` (success when the plugin
    /// said "bye" as asked).  The process is ended: at once when the
    /// plugin failed, and after a moment to leave by itself when it
    /// stopped as asked.
    void finish(error_code reason, std::string_view detail)
    {
        if (ending_)
            return;
        ending_ = true;
        end_reason_ = reason;
        end_detail_ = std::string(detail);
        session_timer_.cancel();
        pause_timer_.cancel();
        fail_waiting_requests();
        if (!process_)
        {
            ended();
            return;
        }
        if (!process_->poll().has_value())
        {
            // A plugin that stopped as asked gets a moment to leave by
            // itself.  One whose channel closed is probably ending, and a
            // short wait lets the way it ended be seen.  Any other failure
            // ends it at once.
            const bool closed_by_plugin =
                reason == make_error_code(plugin_errc::exited) ||
                reason == make_error_code(plugin_errc::no_hello);
            if (reason && !closed_by_plugin)
            {
                end_by_host();
            }
            else
            {
                if (!reason)
                    static_cast<void>(process_->request_stop());
                end_timer_.expires_after(reason ? dying_grace
                                                : options_.end_grace);
                end_timer_.async_wait(
                    [self = shared_from_this(),
                     generation = generation_](const error_code& cancelled)
                    {
                        if (cancelled || generation != self->generation_ ||
                            !self->process_)
                            return;
                        self->end_by_host();
                    });
            }
        }
        watch(end_interval);
    }

    void end_by_host()
    {
        if (process_->poll().has_value())
            return;
        ended_by_host_ = true;
        process_->kill();
    }

    /// Asks from time to time whether the process has ended.
    void watch(std::chrono::milliseconds interval)
    {
        watch_timer_.expires_after(interval);
        watch_timer_.async_wait(
            [self = shared_from_this(),
             generation = generation_](const error_code& cancelled)
            {
                if (cancelled || generation != self->generation_ ||
                    !self->process_)
                    return;
                self->look_at_process();
            });
    }

    void look_at_process()
    {
        const std::optional<exit_status> status = process_->poll();
        if (!status)
        {
            watch(ending_ ? end_interval : watch_interval);
            return;
        }
        if (!ending_)
        {
            // The plugin went by itself.  The session says what that means
            // at this point of the conversation.
            if (session_)
                handle(session_->on_channel_closed(clock_type::now()));
            if (!ending_)
                finish(make_error_code(plugin_errc::exited),
                       "the process ended");
        }
        // A process that ended abnormally by itself crashed.
        if (status->abnormal && !ended_by_host_ &&
            end_reason_ == make_error_code(plugin_errc::exited))
        {
            end_reason_ = make_error_code(plugin_errc::crashed);
            end_detail_ = "the process ended abnormally";
        }
        ended();
    }

    /// The process is gone.  The restart rules say what comes next.
    void ended()
    {
        ++generation_;
        cancel_timers();
        if (channel_)
            channel_->close();
        channel_.reset();
        session_.reset();
        process_.reset();
        process_id_ = 0;
        outgoing_.clear();
        bytes_waiting_ = 0;
        writing_ = false;
        ending_ = false;

        const auto now = clock_type::now();
        if (end_reason_)
        {
            last_failure_ = end_reason_;
            detail_ = end_detail_;
            apply(run_.on_failed(end_reason_, now));
        }
        else
        {
            apply(run_.on_stopped(now));
        }
    }

    void fail_waiting_requests()
    {
        auto waiting = std::move(waiting_);
        waiting_.clear();
        for (auto& [id, handler] : waiting)
            handler(make_error_code(api_errc::plugin_unavailable), {});
    }

    void cancel_timers()
    {
        session_timer_.cancel();
        restart_timer_.cancel();
        watch_timer_.cancel();
        pause_timer_.cancel();
        end_timer_.cancel();
    }

    void close_streams()
    {
        ++output_generation_;
        if (channel_)
            channel_->close();
        if (output_)
            output_->close();
        channel_.reset();
        output_.reset();
    }

    /// A new snapshot soon: once for however many changes come first.
    void publish_soon()
    {
        if (publish_waiting_)
            return;
        publish_waiting_ = true;
        boost::asio::post(strand_,
                          [self = shared_from_this()]()
                          {
                              self->publish_waiting_ = false;
                              self->publish_now();
                              if (self->change_handler_)
                                  self->change_handler_();
                          });
    }

    void publish_now()
    {
        const auto now = clock_type::now();
        auto next = std::make_shared<runner_snapshot>();
        next->state = run_.state();
        next->last_failure = last_failure_;
        next->detail = detail_;
        next->failures = run_.failures(now);
        next->failure_limit = run_.failure_limit();
        next->next_start = run_.next_start();
        next->process_id = process_id_;
        next->sandbox = sandbox_;
        next->api = api_;
        next->status = status_;
        next->log.assign(log_.begin(), log_.end());
        next->output_bytes_dropped = output_dropped_;
        next->revision = ++revision_;
        const std::lock_guard lock(snapshot_mutex_);
        snapshot_ = std::move(next);
    }

    boost::asio::strand<boost::asio::any_io_executor> strand_;
    runner_options options_;
    plugin_run run_;
    boost::asio::steady_timer session_timer_;
    boost::asio::steady_timer restart_timer_;
    boost::asio::steady_timer watch_timer_;
    boost::asio::steady_timer pause_timer_;
    boost::asio::steady_timer end_timer_;

    change_handler change_handler_;
    message_handler message_handler_;

    /// Goes up with every start and every end, so that a handler of an
    /// earlier process finds out that it is too late.
    std::uint64_t generation_ = 0;
    /// Goes up when the output of a process is no longer wanted: at the
    /// next start, and when the runner is abandoned.  Until then the
    /// output of a process that has gone is still read to its end.
    std::uint64_t output_generation_ = 0;
    bool abandoned_ = false;
    /// The host ended the process, so the way it ended says nothing.
    bool ended_by_host_ = false;
    bool ending_ = false;
    error_code end_reason_;
    std::string end_detail_;
    std::string stop_reason_;

    std::optional<plugin_process> process_;
    std::optional<host_session> session_;
    std::unique_ptr<byte_stream> channel_;
    std::unique_ptr<byte_stream> output_;
    std::array<byte, read_size> channel_buffer_{};
    std::array<char, read_size> output_buffer_{};

    std::deque<byte_buffer> outgoing_;
    std::size_t bytes_waiting_ = 0;
    bool writing_ = false;
    std::map<std::uint64_t, answer_handler> waiting_;

    std::string partial_line_;
    clock_type::time_point output_window_{};
    std::size_t output_in_window_ = 0;
    std::uint64_t output_dropped_ = 0;
    std::deque<runner_log_line> log_;

    error_code last_failure_;
    std::string detail_;
    std::uint64_t process_id_ = 0;
    sandbox_report sandbox_;
    api_version api_;
    std::optional<status_message> status_;
    std::uint64_t revision_ = 0;
    bool publish_waiting_ = false;

    mutable std::mutex snapshot_mutex_;
    std::shared_ptr<const runner_snapshot> snapshot_;
};

std::shared_ptr<plugin_runner>
plugin_runner::create(const boost::asio::any_io_executor& executor,
                      runner_options options)
{
    return std::shared_ptr<plugin_runner>(new plugin_runner(
        std::make_shared<impl>(executor, std::move(options))));
}

plugin_runner::plugin_runner(std::shared_ptr<impl> state)
    : impl_(std::move(state))
{
}

plugin_runner::~plugin_runner() = default;

void plugin_runner::on_change(change_handler handler)
{
    impl_->post([handler = std::move(handler)](impl& self) mutable
                { self.set_change_handler(std::move(handler)); });
}

void plugin_runner::on_message(message_handler handler)
{
    impl_->post([handler = std::move(handler)](impl& self) mutable
                { self.set_message_handler(std::move(handler)); });
}

void plugin_runner::start()
{
    impl_->post([](impl& self) { self.start(); });
}

void plugin_runner::stop(std::string reason)
{
    impl_->post([reason = std::move(reason)](impl& self) mutable
                { self.stop(std::move(reason)); });
}

void plugin_runner::abandon()
{
    impl_->post([](impl& self) { self.abandon(); });
}

std::shared_ptr<const runner_snapshot> plugin_runner::snapshot() const
{
    return impl_->snapshot();
}

void plugin_runner::request(api_message message, answer_handler handler,
                            std::chrono::milliseconds timeout)
{
    impl_->post(
        [message = std::move(message), handler = std::move(handler),
         timeout](impl& self) mutable
        { self.request(std::move(message), std::move(handler), timeout); });
}

void plugin_runner::notify(api_message message)
{
    impl_->post([message = std::move(message)](impl& self) mutable
                { self.notify(std::move(message)); });
}

void plugin_runner::answer(std::uint64_t re, api_message message)
{
    impl_->post([re, message = std::move(message)](impl& self) mutable
                { self.answer(re, std::move(message)); });
}

void plugin_runner::unknown(api_message message)
{
    impl_->post([message = std::move(message)](impl& self)
                { self.unknown(message); });
}

} // namespace vss::plugin::host
