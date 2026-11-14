//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/session.hpp>

#include <algorithm>
#include <utility>

namespace vss::plugin
{

namespace
{

using std::chrono::seconds;

/// The most bytes a frame may have before the plugin has said "hello": a
/// header of the largest size, the two length fields and a little more.  A
/// "hello" carries no blob, so nothing larger is expected yet.
constexpr std::size_t first_frame_limit = max_header_size + 64;

/// A request of the host that waits for its answer.
struct waiting_request
{
    std::uint64_t id = 0;
    session_time deadline;
};

} // namespace

struct host_session::state_data
{
    session_options options;
    session_state state = session_state::waiting_for_hello;
    agent::frame_reader reader{first_frame_limit};
    session_time started;
    api_version api;

    /// When the plugin was last heard from, or a "ping" last went
    /// unanswered: the time the next "ping" is counted from.
    session_time quiet_since;
    /// The "ping" that waits for its "pong", and when it was sent.
    std::uint64_t ping_id = 0;
    session_time ping_sent_at;
    /// How many answers in a row did not come.
    unsigned missed = 0;

    /// The ids the host gives its own requests.
    std::uint64_t next_id = 1;
    std::vector<waiting_request> waiting;
    /// The ids of the plugin's requests that the owner has not answered.
    std::vector<std::uint64_t> open_requests;

    std::uint64_t shutdown_id = 0;
    session_time bye_deadline;

    /// One second of the rates, counted from `started`.
    std::int64_t window = 0;
    std::uint64_t frames_left = 0;
    std::uint64_t bytes_left = 0;
    std::uint32_t log_lines_left = 0;
    /// Whether the plugin went over a rate in this second, and which.
    std::string_view over;
    unsigned seconds_over = 0;

    /// When each message with an unknown "op" came, in the last minute.
    std::vector<session_time> unknown_ops;

    state_data(session_options given, session_time now)
        : options(std::move(given)), started(now), quiet_since(now),
          ping_sent_at(now), bye_deadline(now)
    {
        refill();
    }

    const api_limits& limits() const noexcept { return options.ack.limits; }

    // -- sending -------------------------------------------------------------

    /// Writes a message of the host's own.  These are built here and always
    /// encode; should one not, the session ends, since the two sides would
    /// no longer agree on what was said.
    void send(session_output& out, const api_message& message,
              session_time now)
    {
        result<agent::frame> frame = encode_message(message);
        if (frame.has_error())
        {
            close(out, make_error_code(errc::internal),
                  "a message of the host could not be written", now);
            return;
        }
        out.send.push_back(std::move(*frame));
    }

    // -- ending --------------------------------------------------------------

    void close(session_output& out, error_code reason, std::string detail,
               session_time /*now*/)
    {
        if (state == session_state::closed)
            return;
        state = session_state::closed;
        // Whoever waits for an answer is told that none will come.
        for (const waiting_request& request : waiting)
        {
            answer_event event;
            event.id = request.id;
            event.result = make_error_code(api_errc::plugin_unavailable);
            out.events.emplace_back(std::move(event));
        }
        waiting.clear();
        open_requests.clear();
        ping_id = 0;
        closed_event event;
        event.reason = reason;
        event.detail = std::move(detail);
        out.events.emplace_back(std::move(event));
    }

    void misbehaved(session_output& out, std::string detail, session_time now)
    {
        close(out, make_error_code(plugin_errc::misbehaving),
              std::move(detail), now);
    }

    // -- rates ---------------------------------------------------------------

    /// A new second: what the plugin may send in it.  Frames it did not
    /// use are carried over up to the burst; bytes and log lines are not.
    void refill()
    {
        frames_left = std::min<std::uint64_t>(
            limits().frames_burst, frames_left + limits().frames_per_second);
        bytes_left = limits().bytes_per_second;
        log_lines_left = limits().log_lines_per_second;
    }

    /// Moves to the second `now` is in.  A second the plugin went over a
    /// rate in is counted; one it did not ends the count.
    void move_to(session_output& out, session_time now)
    {
        if (state == session_state::closed)
            return;
        const std::int64_t current =
            std::chrono::duration_cast<seconds>(now - started).count();
        if (current <= window)
            return;
        // The second that just ended: one more in a row over a rate, or
        // the end of the row.
        const std::string_view was_over = over;
        over = {};
        if (was_over.empty())
            seconds_over = 0;
        else
            ++seconds_over;
        const bool too_long = !was_over.empty() &&
                              seconds_over >= options.seconds_over_rate;
        // Seconds in which nothing came end the row as well, and each
        // gives its frames, up to the burst.
        const std::int64_t passed = current - window;
        window = current;
        refill();
        if (passed > 1)
        {
            seconds_over = 0;
            frames_left = limits().frames_burst;
        }

        if (too_long)
        {
            std::string detail = "was over its limit for ";
            detail += std::to_string(options.seconds_over_rate);
            detail += " seconds in a row: ";
            detail += was_over;
            misbehaved(out, std::move(detail), now);
        }
    }

    /// The start of the next second, when reading may go on.
    session_time next_second() const
    {
        return started + seconds(window + 1);
    }

    void went_over(session_output& out, std::string_view limit)
    {
        if (over.empty())
            over = limit;
        out.read_again_at = next_second();
    }

    // -- the plugin's messages ------------------------------------------------

    void take_hello(session_output& out, const api_message& message,
                    session_time now)
    {
        const auto* const hello = std::get_if<hello_message>(&message.body);
        if (hello == nullptr || message.id == 0)
        {
            close(out, make_error_code(plugin_errc::bad_hello),
                  "the first message was not a hello request", now);
            return;
        }
        if (hello->plugin_id != options.plugin_id ||
            hello->version != options.version)
        {
            close(out, make_error_code(plugin_errc::bad_hello),
                  "the hello names another plugin or version than the "
                  "manifest",
                  now);
            return;
        }

        // The version: the plugin's own, or the host's highest when the
        // plugin's is newer than that, inside one major version.  It has
        // to be one the manifest allows and the host still speaks.
        const api_version host_highest = options.host_highest;
        const api_version chosen =
            hello->api <= host_highest ? hello->api : host_highest;
        if (hello->api.major != host_highest.major ||
            chosen < options.host_lowest || !options.accepted.contains(chosen))
        {
            close(out, make_error_code(plugin_errc::api_incompatible),
                  "no version of the plugin API suits the plugin and this "
                  "host",
                  now);
            return;
        }

        api = chosen;
        state = session_state::ready;
        reader.set_limit(agent::max_frame_size);
        quiet_since = now;

        api_message ack;
        ack.re = message.id;
        hello_ack_message body = options.ack;
        body.api = chosen;
        ack.body = std::move(body);
        send(out, ack, now);
        if (state == session_state::closed)
            return;

        ready_event event;
        event.api = chosen;
        event.pid = hello->pid;
        out.events.emplace_back(event);
    }

    /// An answer of the plugin to a request of the host.  False when the
    /// host has no request with that id.
    bool take_answer(session_output& out, api_message& message)
    {
        const auto found = std::ranges::find(waiting, message.re,
                                             &waiting_request::id);
        if (found == waiting.end())
            return false;
        waiting.erase(found);
        answer_event event;
        event.id = message.re;
        event.answer = std::move(message);
        out.events.emplace_back(std::move(event));
        return true;
    }

    /// Counts a message nobody knows what to do with.
    void count_unknown(session_output& out, session_time now)
    {
        std::erase_if(unknown_ops, [&](session_time when) {
            return now - when >= seconds(60);
        });
        unknown_ops.push_back(now);
        if (unknown_ops.size() > options.unknown_ops_per_minute)
        {
            misbehaved(out,
                       "sent more messages that nobody knows in one minute "
                       "than it may",
                       now);
        }
    }

    void take_message(session_output& out, api_message message,
                      session_time now)
    {
        if (state == session_state::waiting_for_hello)
        {
            take_hello(out, message, now);
            return;
        }

        struct visitor
        {
            state_data& self;
            session_output& out;
            api_message& message;
            session_time now;

            visitor(state_data& self_in, session_output& out_in,
                    api_message& message_in, session_time now_in)
                : self(self_in), out(out_in), message(message_in),
                  now(now_in)
            {
            }

            void operator()(const hello_message&) const
            {
                self.misbehaved(out, "said hello a second time", now);
            }
            void operator()(const hello_ack_message&) const
            {
                self.misbehaved(out, "sent a message that only the host "
                                     "sends",
                                now);
            }
            void operator()(const shutdown_message&) const
            {
                self.misbehaved(out, "sent a message that only the host "
                                     "sends",
                                now);
            }
            void operator()(const ping_message&) const
            {
                if (message.id == 0)
                    return;
                api_message pong;
                pong.re = message.id;
                pong.body = pong_message{};
                self.send(out, pong, now);
            }
            void operator()(const pong_message&) const
            {
                if (self.ping_id != 0 && message.re == self.ping_id)
                    self.ping_id = 0;
            }
            void operator()(const status_message& status) const
            {
                out.events.emplace_back(status_event{status});
            }
            void operator()(const log_message& line) const
            {
                // Lines over the rate are dropped and counted against it.
                if (self.log_lines_left == 0)
                {
                    self.went_over(out, "log lines per second");
                    return;
                }
                --self.log_lines_left;
                out.events.emplace_back(log_event{line});
            }
            void operator()(const bye_message&) const
            {
                if (self.state == session_state::stopping)
                {
                    self.close(out, error_code{}, std::string{}, now);
                    return;
                }
                self.close(out, make_error_code(plugin_errc::exited),
                           "said bye without being asked to stop", now);
            }
            void operator()(const ok_message&) const { answer_or_count(); }
            void operator()(const error_message&) const { answer_or_count(); }
            void operator()(const other_message&) const
            {
                if (message.re != 0)
                {
                    answer_or_count();
                    return;
                }
                if (message.id != 0)
                {
                    // A request of the plugin.  It is open until the owner
                    // answers it; too many open, or one id twice, is
                    // refused here.
                    const bool open =
                        std::ranges::find(self.open_requests, message.id) !=
                        self.open_requests.end();
                    if (open || self.open_requests.size() >=
                                    self.limits().outstanding_requests)
                    {
                        self.send(out,
                                  make_error(message.id,
                                             open ? api_errc::bad_request
                                                  : api_errc::busy,
                                             open ? "a request with this id "
                                                    "is still open"
                                                  : "too many requests are "
                                                    "open"),
                                  now);
                        return;
                    }
                    self.open_requests.push_back(message.id);
                }
                out.events.emplace_back(message_event{std::move(message)});
            }

            /// An answer to a request of the host, or, when the host asked
            /// nothing with that number, a message nobody knows.
            void answer_or_count() const
            {
                if (message.re == 0 || !self.take_answer(out, message))
                    self.count_unknown(out, now);
            }
        };
        // The body is looked at through a copy of its alternative's
        // address; `message` itself may be moved from inside the visit.
        const message_body body = message.body;
        std::visit(visitor{*this, out, message, now}, body);
    }

    void take_frame(session_output& out, const agent::frame& frame,
                    session_time now)
    {
        if (frames_left == 0)
            went_over(out, "frames per second");
        else
            --frames_left;

        result<api_message> message = decode_message(frame);
        if (message.has_error())
        {
            if (state == session_state::waiting_for_hello)
            {
                close(out, make_error_code(plugin_errc::bad_hello),
                      "the first message was not a message of the plugin "
                      "API",
                      now);
            }
            else
            {
                misbehaved(out, "sent a message that is not as the plugin "
                                "API describes it",
                           now);
            }
            return;
        }
        // Any message shows the plugin is alive.
        quiet_since = now;
        missed = 0;
        take_message(out, std::move(*message), now);
    }

    // -- time ----------------------------------------------------------------

    void pass_time(session_output& out, session_time now)
    {
        move_to(out, now);
        switch (state)
        {
        case session_state::waiting_for_hello:
            if (now - started >= options.hello_timeout)
            {
                close(out, make_error_code(plugin_errc::no_hello),
                      "did not say hello in time", now);
            }
            return;
        case session_state::stopping:
            if (now >= bye_deadline)
            {
                close(out, make_error_code(plugin_errc::unresponsive),
                      "did not say bye in time", now);
            }
            return;
        case session_state::closed:
            return;
        case session_state::ready:
            break;
        }

        // Requests of the host whose answer did not come.
        for (auto request = waiting.begin(); request != waiting.end();)
        {
            if (now < request->deadline)
            {
                ++request;
                continue;
            }
            answer_event event;
            event.id = request->id;
            event.result = make_error_code(api_errc::timeout);
            out.events.emplace_back(std::move(event));
            request = waiting.erase(request);
            if (!missed_one(out, now))
                return;
        }

        if (ping_id != 0)
        {
            if (now - ping_sent_at < options.pong_timeout)
                return;
            ping_id = 0;
            quiet_since = now;
            if (!missed_one(out, now))
                return;
        }
        if (now - quiet_since >= options.ping_after)
        {
            api_message ping;
            ping.id = next_id++;
            ping.body = ping_message{};
            ping_id = ping.id;
            ping_sent_at = now;
            send(out, ping, now);
        }
    }

    /// One more answer that did not come.  False when that ended the
    /// session.
    bool missed_one(session_output& out, session_time now)
    {
        ++missed;
        if (missed < options.missed_pongs)
            return true;
        close(out, make_error_code(plugin_errc::unresponsive),
              "did not answer several times in a row", now);
        return false;
    }
};

host_session::host_session(session_options options, session_time now)
    : data_(std::make_unique<state_data>(std::move(options), now))
{
}

host_session::~host_session() = default;
host_session::host_session(host_session&&) noexcept = default;
host_session& host_session::operator=(host_session&&) noexcept = default;

session_state host_session::state() const noexcept
{
    return data_->state;
}

api_version host_session::api() const noexcept
{
    return data_->api;
}

session_output host_session::on_bytes(bytes_view data, session_time now)
{
    session_output out;
    state_data& self = *data_;
    self.move_to(out, now);
    if (self.state == session_state::closed)
        return out;

    if (data.size() > self.bytes_left)
    {
        self.bytes_left = 0;
        self.went_over(out, "bytes per second");
    }
    else
    {
        self.bytes_left -= data.size();
    }

    self.reader.feed(data);
    while (self.state != session_state::closed)
    {
        result<std::optional<agent::frame>> next = self.reader.next();
        if (next.has_error())
        {
            if (self.state == session_state::waiting_for_hello)
            {
                self.close(out, make_error_code(plugin_errc::bad_hello),
                           "sent bytes that are not a frame before its "
                           "hello",
                           now);
            }
            else
            {
                self.misbehaved(out, "sent bytes that are not a frame", now);
            }
            break;
        }
        const std::optional<agent::frame>& frame = *next;
        if (!frame.has_value())
            break;
        self.take_frame(out, *frame, now);
    }
    return out;
}

session_output host_session::on_time(session_time now)
{
    session_output out;
    data_->pass_time(out, now);
    return out;
}

std::optional<session_time> host_session::next_deadline() const noexcept
{
    const state_data& self = *data_;
    switch (self.state)
    {
    case session_state::waiting_for_hello:
        return self.started + self.options.hello_timeout;
    case session_state::stopping:
        return self.bye_deadline;
    case session_state::closed:
        return std::nullopt;
    case session_state::ready:
        break;
    }
    session_time next = self.ping_id != 0
                            ? self.ping_sent_at + self.options.pong_timeout
                            : self.quiet_since + self.options.ping_after;
    for (const waiting_request& request : self.waiting)
        next = std::min(next, request.deadline);
    return next;
}

session_output host_session::on_channel_closed(session_time now)
{
    session_output out;
    state_data& self = *data_;
    switch (self.state)
    {
    case session_state::waiting_for_hello:
        self.close(out, make_error_code(plugin_errc::no_hello),
                   "the channel closed before a hello", now);
        break;
    case session_state::ready:
        self.close(out, make_error_code(plugin_errc::exited),
                   "the channel closed without the host asking", now);
        break;
    case session_state::stopping:
        // It was asked to stop and it stopped; the "bye" is a courtesy.
        self.close(out, error_code{}, std::string{}, now);
        break;
    case session_state::closed:
        break;
    }
    return out;
}

result<std::uint64_t> host_session::request(api_message message,
                                            session_time now,
                                            session_output& out,
                                            std::chrono::milliseconds timeout)
{
    state_data& self = *data_;
    if (self.state != session_state::ready)
        return make_error_code(api_errc::plugin_unavailable);
    if (self.waiting.size() >= self.limits().outstanding_requests)
        return make_error_code(api_errc::busy);

    const std::uint64_t id = self.next_id;
    const result<api_message> numbered = with_ids(std::move(message), id, 0);
    if (numbered.has_error())
        return make_error_code(api_errc::bad_request);
    result<agent::frame> frame = encode_message(*numbered);
    if (frame.has_error())
        return make_error_code(api_errc::bad_request);
    ++self.next_id;
    out.send.push_back(std::move(*frame));

    waiting_request waiting;
    waiting.id = id;
    waiting.deadline = now + (timeout.count() > 0
                                  ? timeout
                                  : self.options.request_timeout);
    self.waiting.push_back(waiting);
    return id;
}

result<void> host_session::notify(api_message message, session_time /*now*/,
                                  session_output& out)
{
    state_data& self = *data_;
    if (self.state != session_state::ready)
        return make_error_code(api_errc::plugin_unavailable);
    const result<api_message> plain = with_ids(std::move(message), 0, 0);
    if (plain.has_error())
        return make_error_code(api_errc::bad_request);
    result<agent::frame> frame = encode_message(*plain);
    if (frame.has_error())
        return make_error_code(api_errc::bad_request);
    out.send.push_back(std::move(*frame));
    return {};
}

result<void> host_session::answer(std::uint64_t re, api_message message,
                                  session_time /*now*/, session_output& out)
{
    state_data& self = *data_;
    if (self.state != session_state::ready)
        return make_error_code(api_errc::plugin_unavailable);
    const auto open = std::ranges::find(self.open_requests, re);
    if (re == 0 || open == self.open_requests.end())
        return make_error_code(api_errc::bad_request);
    const result<api_message> numbered = with_ids(std::move(message), 0, re);
    if (numbered.has_error())
        return make_error_code(api_errc::bad_request);
    result<agent::frame> frame = encode_message(*numbered);
    if (frame.has_error())
        return make_error_code(api_errc::bad_request);
    self.open_requests.erase(open);
    out.send.push_back(std::move(*frame));
    return {};
}

session_output host_session::unknown_op(const api_message& message,
                                        session_time now)
{
    session_output out;
    state_data& self = *data_;
    if (self.state != session_state::ready)
        return out;
    if (message.id != 0)
    {
        const auto open = std::ranges::find(self.open_requests, message.id);
        if (open != self.open_requests.end())
        {
            self.open_requests.erase(open);
            self.send(out,
                      make_error(message.id, api_errc::unknown_op,
                                 "this host does not know that message"),
                      now);
        }
    }
    if (self.state != session_state::closed)
        self.count_unknown(out, now);
    return out;
}

session_output host_session::shut_down(std::string_view reason,
                                       session_time now)
{
    session_output out;
    state_data& self = *data_;
    switch (self.state)
    {
    case session_state::waiting_for_hello:
        self.close(out, error_code{}, "stopped before it was ready", now);
        break;
    case session_state::ready:
    {
        api_message message;
        message.id = self.next_id++;
        shutdown_message body;
        body.reason = std::string(reason);
        message.body = std::move(body);
        self.shutdown_id = message.id;
        self.state = session_state::stopping;
        self.bye_deadline = now + self.options.shutdown_timeout;
        self.ping_id = 0;
        self.send(out, message, now);
        break;
    }
    case session_state::stopping:
    case session_state::closed:
        break;
    }
    return out;
}

} // namespace vss::plugin
