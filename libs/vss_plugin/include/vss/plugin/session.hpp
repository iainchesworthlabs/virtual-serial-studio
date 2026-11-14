//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The host's side of the conversation with one plugin: who must speak
// first, how long anything may take, how much a plugin may send, and when
// the host gives up on it.  docs/plugins.md, sections 5.4 to 5.6, 7.2, 7.3
// and 7.9, are the design.
//
// A session does no I/O and has no clock and no thread.  Its owner gives it
// the bytes that arrived and the time, and it answers with the frames to
// send and with what happened.  So every rule here can be tested from
// memory, with a plugin that misbehaves in any way a test cares to write,
// and the same input and times always give the same result.
//
// Everything a plugin sends is untrusted.  A session never waits without a
// limit, never keeps more than its limits allow, and ends itself the moment
// a plugin breaks a rule.  When a session ends, the owner kills the plugin's
// process; a session that has ended accepts nothing more.
//
// One thread at a time may use a session.  Which thread is the owner's
// business.
//

#pragma once

#include <vss/agent/proto/frame.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vss::plugin
{

/// A time for a session: any clock that only goes forward.  The owner
/// passes the same clock to every call.
using session_time = std::chrono::steady_clock::time_point;

struct session_options
{
    /// The plugin's id and version, from its manifest.  The plugin's
    /// "hello" must name the same.
    std::string plugin_id;
    std::string version;
    /// The versions of the plugin API the manifest says the plugin works
    /// with.
    api_range accepted;
    /// The lowest and highest version this host speaks.
    api_version host_lowest = plugin_api_version;
    api_version host_highest = plugin_api_version;
    /// What the host tells the plugin once "hello" is accepted.  The
    /// session fills in the version; `limits` in it are the limits the
    /// session then holds the plugin to.
    hello_ack_message ack;

    /// How long the plugin has to say "hello" after it is started.
    std::chrono::milliseconds hello_timeout{5000};
    /// How long the channel may be quiet before the host sends "ping".
    std::chrono::milliseconds ping_after{5000};
    /// How long the plugin has to answer a "ping".
    std::chrono::milliseconds pong_timeout{5000};
    /// How many "ping"s in a row may go unanswered.  The next one ends the
    /// session.
    unsigned missed_pongs = 3;
    /// How long the plugin has to say "bye" after "shutdown".
    std::chrono::milliseconds shutdown_timeout{3000};
    /// How long a request of the host may wait for its answer when the
    /// caller names no time.
    std::chrono::milliseconds request_timeout{10000};
    /// How many messages with an "op" nobody knows a plugin may send in one
    /// minute.
    unsigned unknown_ops_per_minute = 20;
    /// For how many seconds in a row a plugin may be over a rate before
    /// the session ends.
    unsigned seconds_over_rate = 10;
};

enum class session_state
{
    /// Started; the plugin has not said "hello" yet.
    waiting_for_hello,
    /// "hello" was accepted and "hello_ack" sent.
    ready,
    /// The host asked the plugin to stop and waits for "bye".
    stopping,
    /// Over.  `closed_event` said why.
    closed
};

/// The plugin said "hello", it was accepted, and "hello_ack" is among the
/// frames to send.
struct ready_event
{
    /// The version both sides use.
    api_version api;
    std::uint64_t pid = 0;
};

/// The plugin sent a status or a log line.  Plain text, already checked.
struct status_event
{
    status_message status;
};

struct log_event
{
    log_message line;
};

/// The plugin sent a message that is not one of the session's own: a
/// request or a notification of an extension point.  When it has an `id`,
/// the owner answers it with `host_session::answer`, or the plugin's
/// request stays open until the session ends.
struct message_event
{
    api_message message;
};

/// A request the owner made was answered, or will not be.
struct answer_event
{
    /// The id `host_session::request` returned.
    std::uint64_t id = 0;
    /// Success and `answer` holds the plugin's answer, which may itself be
    /// an "error" message; or `api_errc::timeout` and `answer` is empty; or
    /// `api_errc::plugin_unavailable` when the session ended first.
    error_code result;
    api_message answer;
};

/// The session is over.  It is the last event of a session, and it comes
/// exactly once.
struct closed_event
{
    /// Success when the host asked for the stop and the plugin said "bye".
    /// Otherwise why the session ended:
    ///
    /// - `no_hello`: no "hello" in time, or the channel closed before one.
    /// - `bad_hello`: the first message was not a "hello" this host can
    ///   take: not a message at all, another message, not a request, or
    ///   another plugin id or version than the manifest's.
    /// - `api_incompatible`: no version of the plugin API suits the
    ///   plugin's manifest, its "hello" and this host together.
    /// - `unresponsive`: too many "ping"s went unanswered, or "bye" did
    ///   not come in time.
    /// - `misbehaving`: the plugin broke a rule of the API.  `detail`
    ///   names the rule.
    /// - `exited`: the channel closed, and the host had not asked.
    error_code reason;
    /// One line for the log.  It never repeats what the plugin sent.
    std::string detail;
};

using session_event =
    std::variant<ready_event, status_event, log_event, message_event,
                 answer_event, closed_event>;

/// What a call to a session asks of its owner.
struct session_output
{
    /// Frames to send to the plugin, in this order.
    std::vector<agent::frame> send;
    /// What happened, in the order it happened.
    std::vector<session_event> events;
    /// When set: do not read from the plugin again before this time.  The
    /// plugin sent more than it may this second, and what it sends next
    /// waits in the channel, which is how it is slowed down.
    std::optional<session_time> read_again_at;
};

class host_session
{
public:
    /// A session for a plugin that was started at `now`.
    host_session(session_options options, session_time now);
    ~host_session();
    host_session(host_session&&) noexcept;
    host_session& operator=(host_session&&) noexcept;

    session_state state() const noexcept;

    /// The version in use.  Meaningful from `ready` on.
    api_version api() const noexcept;

    /// The plugin sent these bytes.  They are cut into frames and each
    /// frame is taken as a message, in order.  Bytes that arrive after the
    /// session closed are dropped.
    session_output on_bytes(bytes_view data, session_time now);

    /// Time has passed.  The owner calls this at `next_deadline()`, or
    /// more often; calling it early does nothing.
    session_output on_time(session_time now);

    /// The next time `on_time` has something to do, or nothing when the
    /// session is closed.
    std::optional<session_time> next_deadline() const noexcept;

    /// The channel closed or the process ended.
    session_output on_channel_closed(session_time now);

    /// Sends a request to the plugin and returns its id.  The answer comes
    /// as an `answer_event` with that id: the plugin's answer, a timeout
    /// after `timeout` (the default of the options when it is zero), or
    /// `plugin_unavailable` when the session ends first.
    ///
    /// `api_errc::plugin_unavailable` when the session is not `ready`;
    /// `api_errc::busy` when as many requests wait as the limits allow;
    /// `api_errc::bad_request` when the message cannot be sent as it is.
    /// The message's `id` and `re` are set by the session.
    result<std::uint64_t> request(api_message message, session_time now,
                                  session_output& out,
                                  std::chrono::milliseconds timeout = {});

    /// Sends a notification to the plugin: a message that wants no answer.
    /// The errors of `request`, without `busy`.
    result<void> notify(api_message message, session_time now,
                        session_output& out);

    /// Answers a request of the plugin that came as a `message_event`.
    /// `re` is that message's `id`.  `api_errc::bad_request` when the
    /// plugin has no open request with that id, or the message cannot be
    /// sent as it is; `plugin_unavailable` when the session is not `ready`.
    result<void> answer(std::uint64_t re, api_message message,
                        session_time now, session_output& out);

    /// The owner does not know the "op" of a message that came as a
    /// `message_event`.  When the message was a request it is answered
    /// with `unknown_op`, and the channel stays open.  A plugin that sends
    /// more such messages in a minute than the options allow ends the
    /// session as `misbehaving`.
    session_output unknown_op(const api_message& message, session_time now);

    /// Asks the plugin to stop: sends "shutdown" and waits for "bye".  A
    /// session that is not `ready` is closed at once instead: there is
    /// nobody to ask.
    session_output shut_down(std::string_view reason, session_time now);

private:
    struct state_data;
    std::unique_ptr<state_data> data_;
};

} // namespace vss::plugin
