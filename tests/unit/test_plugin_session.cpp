//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The host's side of one plugin's conversation, with the plugin played from
// memory: one that behaves, and one that breaks each rule in turn.  There is
// no process, no channel and no clock here; a test says what arrives and
// what time it is.
//

#include <vss/agent/proto/frame.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>
#include <vss/plugin/session.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace vss;
using namespace vss::plugin;
using namespace std::chrono_literals;

namespace
{

/// The time a test's plugin was started at.
constexpr session_time start{session_time::duration{1000s}};

session_options options_for_test()
{
    session_options options;
    options.plugin_id = "com.example.plugin";
    options.version = "1.2.0";
    options.accepted = api_range{api_version{1, 0}, 1, std::nullopt};
    options.ack.granted = {permission::ports_read};
    options.ack.capabilities = {"engine_rules"};
    options.ack.data_directory = "/data/plugin";
    return options;
}

/// The bytes of a message as the plugin would send them.
byte_buffer bytes_of(const api_message& message)
{
    const auto frame = encode_message(message);
    REQUIRE(frame.has_value());
    const auto bytes = agent::encode_frame(frame.value());
    REQUIRE(bytes.has_value());
    return bytes.value();
}

api_message hello(std::uint64_t id = 1, api_version api = {1, 0},
                  std::string plugin_id = "com.example.plugin",
                  std::string version = "1.2.0")
{
    api_message message;
    message.id = id;
    hello_message body;
    body.api = api;
    body.plugin_id = std::move(plugin_id);
    body.version = std::move(version);
    body.pid = 4242;
    message.body = std::move(body);
    return message;
}

template <class Body>
api_message make(Body body, std::uint64_t id = 0, std::uint64_t re = 0)
{
    api_message message;
    message.id = id;
    message.re = re;
    message.body = std::move(body);
    return message;
}

/// A message of an extension point, which the session does not know.
api_message other(std::string_view op, std::uint64_t id = 0,
                  std::uint64_t re = 0)
{
    std::string header = R"({"op":")";
    header += op;
    header += '"';
    if (id != 0)
    {
        header += R"(,"id":)";
        header += std::to_string(id);
    }
    if (re != 0)
    {
        header += R"(,"re":)";
        header += std::to_string(re);
    }
    header += '}';
    api_message message;
    message.op = std::string(op);
    message.id = id;
    message.re = re;
    message.body = other_message{header};
    return message;
}

log_message line_of(std::string text)
{
    log_message line;
    line.text = std::move(text);
    return line;
}

/// The messages among the frames a session asked to send.
std::vector<api_message> sent(const session_output& out)
{
    std::vector<api_message> messages;
    for (const agent::frame& frame : out.send)
    {
        const auto message = decode_message(frame);
        REQUIRE(message.has_value());
        messages.push_back(message.value());
    }
    return messages;
}

template <class Event>
std::vector<Event> events_of(const session_output& out)
{
    std::vector<Event> found;
    for (const session_event& event : out.events)
    {
        if (const auto* const wanted = std::get_if<Event>(&event))
            found.push_back(*wanted);
    }
    return found;
}

/// The one `closed_event` of an output, which must be its last event.
closed_event closed_of(const session_output& out)
{
    REQUIRE(!out.events.empty());
    const auto* const closed = std::get_if<closed_event>(&out.events.back());
    REQUIRE(closed != nullptr);
    REQUIRE_EQ(events_of<closed_event>(out).size(), std::size_t{1});
    return *closed;
}

void require_closed(const session_output& out, plugin_errc reason,
                    std::string_view part = {})
{
    const closed_event closed = closed_of(out);
    REQUIRE(closed.reason == reason);
    REQUIRE(!closed.detail.empty());
    REQUIRE(closed.detail.find(part) != std::string::npos);
}

/// A session whose plugin has said a good hello at `start`.
host_session ready_session(session_options options = options_for_test())
{
    host_session session(std::move(options), start);
    const session_output out = session.on_bytes(bytes_of(hello()), start);
    REQUIRE(session.state() == session_state::ready);
    REQUIRE_EQ(events_of<ready_event>(out).size(), std::size_t{1});
    return session;
}

} // namespace

// -- hello ------------------------------------------------------------------

TEST_CASE("plugin session: a good hello is answered and the plugin is ready")
{
    host_session session(options_for_test(), start);
    REQUIRE(session.state() == session_state::waiting_for_hello);
    REQUIRE(session.next_deadline() == start + 5000ms);

    const session_output out =
        session.on_bytes(bytes_of(hello(7)), start + 10ms);
    REQUIRE(session.state() == session_state::ready);
    REQUIRE(session.api() == (api_version{1, 0}));
    REQUIRE(!out.read_again_at.has_value());

    const std::vector<api_message> messages = sent(out);
    REQUIRE_EQ(messages.size(), std::size_t{1});
    REQUIRE_EQ(messages[0].re, std::uint64_t{7});
    const auto* const ack = std::get_if<hello_ack_message>(&messages[0].body);
    REQUIRE(ack != nullptr);
    REQUIRE(ack->api == (api_version{1, 0}));
    REQUIRE(ack->granted == options_for_test().ack.granted);
    REQUIRE(ack->capabilities == options_for_test().ack.capabilities);
    REQUIRE_EQ(ack->data_directory, "/data/plugin");

    const std::vector<ready_event> ready = events_of<ready_event>(out);
    REQUIRE_EQ(ready.size(), std::size_t{1});
    REQUIRE(ready[0].api == (api_version{1, 0}));
    REQUIRE_EQ(ready[0].pid, std::uint64_t{4242});
    REQUIRE_EQ(out.events.size(), std::size_t{1});
}

TEST_CASE("plugin session: bytes may arrive in any pieces")
{
    const byte_buffer bytes = bytes_of(hello());
    host_session one(options_for_test(), start);
    std::size_t acks = 0;
    for (const byte value : bytes)
    {
        const byte_buffer single{value};
        acks += one.on_bytes(single, start).send.size();
    }
    REQUIRE(one.state() == session_state::ready);
    REQUIRE_EQ(acks, std::size_t{1});

    // Two messages in one piece are both taken, in order.
    host_session two(options_for_test(), start);
    byte_buffer both = bytes;
    const byte_buffer status = bytes_of(make(status_message{}));
    both.insert(both.end(), status.begin(), status.end());
    const session_output out = two.on_bytes(both, start);
    REQUIRE_EQ(out.events.size(), std::size_t{2});
    REQUIRE(std::holds_alternative<ready_event>(out.events[0]));
    REQUIRE(std::holds_alternative<status_event>(out.events[1]));
}

TEST_CASE("plugin session: the version is the highest that suits both")
{
    // The plugin is newer than the host inside one major version: the
    // host's highest is used.
    {
        session_options options = options_for_test();
        host_session session(options, start);
        const session_output out =
            session.on_bytes(bytes_of(hello(1, {1, 3})), start);
        REQUIRE(session.state() == session_state::ready);
        REQUIRE(session.api() == (api_version{1, 0}));
        REQUIRE(events_of<ready_event>(out)[0].api == (api_version{1, 0}));
    }
    // The host is newer: the plugin's own version is used.
    {
        session_options options = options_for_test();
        options.host_highest = api_version{1, 4};
        host_session session(options, start);
        static_cast<void>(session.on_bytes(bytes_of(hello(1, {1, 2})), start));
        REQUIRE(session.api() == (api_version{1, 2}));
    }
    // Another major version.
    {
        host_session session(options_for_test(), start);
        require_closed(session.on_bytes(bytes_of(hello(1, {2, 0})), start),
                       plugin_errc::api_incompatible);
        REQUIRE(session.state() == session_state::closed);
    }
    // A version the manifest does not allow.
    {
        session_options options = options_for_test();
        options.accepted = api_range{api_version{1, 1}, 1, std::nullopt};
        host_session session(options, start);
        require_closed(session.on_bytes(bytes_of(hello(1, {1, 3})), start),
                       plugin_errc::api_incompatible);
    }
    // A version the host no longer speaks.
    {
        session_options options = options_for_test();
        options.host_lowest = api_version{1, 2};
        options.host_highest = api_version{1, 4};
        host_session session(options, start);
        require_closed(session.on_bytes(bytes_of(hello(1, {1, 1})), start),
                       plugin_errc::api_incompatible);
    }
}

TEST_CASE("plugin session: no hello in time ends the session")
{
    host_session session(options_for_test(), start);
    REQUIRE(session.on_time(start + 4999ms).events.empty());
    REQUIRE(session.state() == session_state::waiting_for_hello);
    const session_output out = session.on_time(start + 5000ms);
    require_closed(out, plugin_errc::no_hello, "in time");
    REQUIRE(out.send.empty());
    REQUIRE(session.state() == session_state::closed);
    REQUIRE(!session.next_deadline().has_value());

    // A channel that closes before a hello is the same reason.
    host_session gone(options_for_test(), start);
    require_closed(gone.on_channel_closed(start + 1ms),
                   plugin_errc::no_hello, "closed");
}

TEST_CASE("plugin session: a first message that is not a good hello ends "
          "the session")
{
    const auto refused = [](const byte_buffer& bytes) {
        host_session session(options_for_test(), start);
        const session_output out = session.on_bytes(bytes, start);
        require_closed(out, plugin_errc::bad_hello);
        REQUIRE(out.send.empty());
        REQUIRE(events_of<ready_event>(out).empty());
    };
    // Another message first.
    refused(bytes_of(make(ping_message{}, 1)));
    refused(bytes_of(make(status_message{})));
    refused(bytes_of(other("generator.start", 1)));
    // A hello that is not a request: there is nothing to answer.
    refused(bytes_of(hello(0)));
    // Another plugin, or another version, than the manifest's.
    refused(bytes_of(hello(1, {1, 0}, "com.example.other")));
    refused(bytes_of(hello(1, {1, 0}, "com.example.plugin", "9.9.9")));
    // A frame whose header is not a message.
    {
        agent::frame frame;
        frame.header = R"({"no":"op"})";
        refused(agent::encode_frame(frame).value());
    }
    // Bytes that are not a frame at all: a length far over the limit.
    refused(byte_buffer{0xff, 0xff, 0xff, 0x7f, 0, 0, 0, 0});
    // A frame larger than a hello can be, before anything was said.
    {
        agent::frame frame;
        frame.header = R"({"op":"hello"})";
        frame.blob.assign(max_header_size + 4096, byte{0});
        refused(agent::encode_frame(frame).value());
    }
}

// -- being alive ------------------------------------------------------------

TEST_CASE("plugin session: a quiet plugin is pinged and must answer")
{
    host_session session = ready_session();
    REQUIRE(session.next_deadline() == start + 5000ms);
    REQUIRE(session.on_time(start + 4999ms).send.empty());

    session_output out = session.on_time(start + 5000ms);
    std::vector<api_message> messages = sent(out);
    REQUIRE_EQ(messages.size(), std::size_t{1});
    REQUIRE(std::holds_alternative<ping_message>(messages[0].body));
    const std::uint64_t ping = messages[0].id;
    REQUIRE(ping != 0);
    REQUIRE(session.next_deadline() == start + 10000ms);

    // The pong comes: nothing is held against the plugin, and the next
    // ping is counted from it.
    out = session.on_bytes(bytes_of(make(pong_message{}, 0, ping)),
                           start + 5100ms);
    REQUIRE(out.events.empty());
    REQUIRE(session.next_deadline() == start + 10100ms);
    REQUIRE(session.on_time(start + 10099ms).send.empty());
    REQUIRE_EQ(session.on_time(start + 10100ms).send.size(), std::size_t{1});
}

TEST_CASE("plugin session: three pings without an answer end the session")
{
    host_session session = ready_session();
    session_time now = start;
    for (int miss = 1; miss <= 3; ++miss)
    {
        now += 5000ms;
        REQUIRE_EQ(session.on_time(now).send.size(), std::size_t{1});
        now += 5000ms;
        const session_output out = session.on_time(now);
        if (miss < 3)
        {
            REQUIRE(out.events.empty());
            REQUIRE(session.state() == session_state::ready);
        }
        else
        {
            require_closed(out, plugin_errc::unresponsive);
        }
    }
    REQUIRE(session.state() == session_state::closed);
}

TEST_CASE("plugin session: any message from the plugin ends a row of misses")
{
    host_session session = ready_session();
    session_time now = start;
    // Two pings go unanswered.
    for (int miss = 0; miss < 2; ++miss)
    {
        now += 5000ms;
        REQUIRE_EQ(session.on_time(now).send.size(), std::size_t{1});
        now += 5000ms;
        REQUIRE(session.on_time(now).events.empty());
    }
    // The plugin says something, and the count starts again: two more
    // misses do not end the session.
    static_cast<void>(session.on_bytes(bytes_of(make(status_message{})), now));
    for (int miss = 0; miss < 2; ++miss)
    {
        now += 5000ms;
        REQUIRE_EQ(session.on_time(now).send.size(), std::size_t{1});
        now += 5000ms;
        REQUIRE(session.on_time(now).events.empty());
    }
    REQUIRE(session.state() == session_state::ready);
}

TEST_CASE("plugin session: the plugin's own ping is answered")
{
    host_session session = ready_session();
    const session_output out =
        session.on_bytes(bytes_of(make(ping_message{}, 55)), start + 1s);
    const std::vector<api_message> messages = sent(out);
    REQUIRE_EQ(messages.size(), std::size_t{1});
    REQUIRE(std::holds_alternative<pong_message>(messages[0].body));
    REQUIRE_EQ(messages[0].re, std::uint64_t{55});
    REQUIRE(out.events.empty());
}

// -- what a plugin says -----------------------------------------------------

TEST_CASE("plugin session: status and log lines are handed on")
{
    host_session session = ready_session();
    status_message status;
    status.level = status_level::warning;
    status.text = "the device answers slowly";
    byte_buffer bytes = bytes_of(make(status));
    const byte_buffer line = bytes_of(make(line_of("started")));
    bytes.insert(bytes.end(), line.begin(), line.end());

    const session_output out = session.on_bytes(bytes, start + 1s);
    REQUIRE(out.send.empty());
    REQUIRE_EQ(out.events.size(), std::size_t{2});
    const std::vector<status_event> statuses = events_of<status_event>(out);
    REQUIRE_EQ(statuses.size(), std::size_t{1});
    REQUIRE(statuses[0].status == status);
    const std::vector<log_event> lines = events_of<log_event>(out);
    REQUIRE_EQ(lines.size(), std::size_t{1});
    REQUIRE_EQ(lines[0].line.text, "started");
}

TEST_CASE("plugin session: a message of an extension point is handed on, "
          "and a request waits for its answer")
{
    host_session session = ready_session();
    // A notification.
    session_output out =
        session.on_bytes(bytes_of(other("generator.done")), start + 1s);
    std::vector<message_event> messages = events_of<message_event>(out);
    REQUIRE_EQ(messages.size(), std::size_t{1});
    REQUIRE_EQ(messages[0].message.op, "generator.done");
    REQUIRE_EQ(messages[0].message.id, std::uint64_t{0});
    REQUIRE(out.send.empty());

    // A request: the owner answers it, once.
    out = session.on_bytes(bytes_of(other("ports.list", 9)), start + 1s);
    messages = events_of<message_event>(out);
    REQUIRE_EQ(messages.size(), std::size_t{1});
    REQUIRE_EQ(messages[0].message.id, std::uint64_t{9});

    session_output answered;
    REQUIRE(session.answer(9, make(ok_message{}), start + 1s, answered)
                .has_value());
    const std::vector<api_message> replies = sent(answered);
    REQUIRE_EQ(replies.size(), std::size_t{1});
    REQUIRE_EQ(replies[0].re, std::uint64_t{9});
    REQUIRE(std::holds_alternative<ok_message>(replies[0].body));

    session_output again;
    const auto twice =
        session.answer(9, make(ok_message{}), start + 1s, again);
    REQUIRE(twice.has_error());
    REQUIRE(twice.error() == api_errc::bad_request);
    REQUIRE(again.send.empty());
    // An answer to a request the plugin never made.
    REQUIRE(session.answer(77, make(ok_message{}), start + 1s, again)
                .error() == api_errc::bad_request);
    REQUIRE(session.answer(0, make(ok_message{}), start + 1s, again)
                .error() == api_errc::bad_request);
}

TEST_CASE("plugin session: a plugin may not hold too many requests open, "
          "nor use an id twice")
{
    session_options options = options_for_test();
    options.ack.limits.outstanding_requests = 2;
    host_session session = ready_session(options);

    static_cast<void>(session.on_bytes(bytes_of(other("a.one", 1)), start));
    static_cast<void>(session.on_bytes(bytes_of(other("a.two", 2)), start));

    // The same id again while it is open.
    session_output out = session.on_bytes(bytes_of(other("a.one", 1)), start);
    REQUIRE(events_of<message_event>(out).empty());
    std::vector<api_message> replies = sent(out);
    REQUIRE_EQ(replies.size(), std::size_t{1});
    const auto* refusal = std::get_if<error_message>(&replies[0].body);
    REQUIRE(refusal != nullptr);
    REQUIRE_EQ(refusal->value, "bad_request");
    REQUIRE_EQ(replies[0].re, std::uint64_t{1});

    // A third request while two are open.
    out = session.on_bytes(bytes_of(other("a.three", 3)), start);
    REQUIRE(events_of<message_event>(out).empty());
    replies = sent(out);
    REQUIRE_EQ(replies.size(), std::size_t{1});
    refusal = std::get_if<error_message>(&replies[0].body);
    REQUIRE(refusal != nullptr);
    REQUIRE_EQ(refusal->value, "busy");

    // One is answered, and there is room again.
    session_output answered;
    REQUIRE(session.answer(1, make(ok_message{}), start, answered).has_value());
    out = session.on_bytes(bytes_of(other("a.three", 3)), start);
    REQUIRE_EQ(events_of<message_event>(out).size(), std::size_t{1});
    REQUIRE(session.state() == session_state::ready);
}

TEST_CASE("plugin session: a message nobody knows is answered, and too many "
          "end the session")
{
    session_options options = options_for_test();
    options.unknown_ops_per_minute = 3;
    host_session session = ready_session(options);
    session_time now = start;

    const auto unknown = [&](std::uint64_t id) {
        const session_output got =
            session.on_bytes(bytes_of(other("no.such", id)), now);
        const std::vector<message_event> messages =
            events_of<message_event>(got);
        REQUIRE_EQ(messages.size(), std::size_t{1});
        return session.unknown_op(messages[0].message, now);
    };

    // A request is answered with unknown_op and the channel stays open.
    session_output out = unknown(1);
    std::vector<api_message> replies = sent(out);
    REQUIRE_EQ(replies.size(), std::size_t{1});
    const auto* const refusal = std::get_if<error_message>(&replies[0].body);
    REQUIRE(refusal != nullptr);
    REQUIRE_EQ(refusal->category, "vss.plugin.api");
    REQUIRE_EQ(refusal->value, "unknown_op");
    REQUIRE_EQ(replies[0].re, std::uint64_t{1});
    REQUIRE(session.state() == session_state::ready);

    // A notification has nothing to answer and still counts.
    out = unknown(0);
    REQUIRE(out.send.empty());
    out = unknown(2);
    REQUIRE(session.state() == session_state::ready);

    // A minute later the count has run out, and three more are allowed.
    now += 60s;
    static_cast<void>(unknown(3));
    static_cast<void>(unknown(4));
    static_cast<void>(unknown(5));
    REQUIRE(session.state() == session_state::ready);
    // The fourth in one minute ends the session.
    require_closed(unknown(6), plugin_errc::misbehaving, "nobody knows");
    REQUIRE(session.state() == session_state::closed);
}

// -- the host's requests ----------------------------------------------------

TEST_CASE("plugin session: a request of the host gets the plugin's answer")
{
    host_session session = ready_session();
    session_output out;
    const auto id = session.request(other("generator.start"), start + 1s, out);
    REQUIRE(id.has_value());
    const std::vector<api_message> messages = sent(out);
    REQUIRE_EQ(messages.size(), std::size_t{1});
    REQUIRE_EQ(messages[0].id, id.value());
    REQUIRE_EQ(messages[0].op, "generator.start");

    // The answer may be an error of the plugin's: it is still an answer.
    api_message refusal = make_error(id.value(), api_errc::refused, "no");
    const session_output answered =
        session.on_bytes(bytes_of(refusal), start + 2s);
    const std::vector<answer_event> answers = events_of<answer_event>(answered);
    REQUIRE_EQ(answers.size(), std::size_t{1});
    REQUIRE_EQ(answers[0].id, id.value());
    REQUIRE(!answers[0].result.failed());
    REQUIRE(std::holds_alternative<error_message>(answers[0].answer.body));

    // The same answer again answers nothing; it is counted as a message
    // nobody knows, and the session goes on.
    const session_output repeated =
        session.on_bytes(bytes_of(refusal), start + 2s);
    REQUIRE(repeated.events.empty());
    REQUIRE(session.state() == session_state::ready);
}

TEST_CASE("plugin session: a request that is not answered in time says so")
{
    host_session session = ready_session();
    session_output out;
    const auto slow =
        session.request(other("generator.start"), start, out, 2000ms);
    const auto standard = session.request(other("generator.stop"), start, out);
    REQUIRE(slow.has_value());
    REQUIRE(standard.has_value());
    REQUIRE(slow.value() != standard.value());
    REQUIRE(session.next_deadline() == start + 2000ms);

    REQUIRE(events_of<answer_event>(session.on_time(start + 1999ms)).empty());
    std::vector<answer_event> answers =
        events_of<answer_event>(session.on_time(start + 2000ms));
    REQUIRE_EQ(answers.size(), std::size_t{1});
    REQUIRE_EQ(answers[0].id, slow.value());
    REQUIRE(answers[0].result == api_errc::timeout);

    // An answer that comes after its time answers nothing.
    static_cast<void>(session.on_bytes(
        bytes_of(make(ok_message{}, 0, slow.value())), start + 2100ms));

    // The other one has the time the options give.
    answers = events_of<answer_event>(session.on_time(start + 10000ms));
    REQUIRE_EQ(answers.size(), std::size_t{1});
    REQUIRE_EQ(answers[0].id, standard.value());
    REQUIRE(answers[0].result == api_errc::timeout);
}

TEST_CASE("plugin session: the host may not have too many requests open")
{
    session_options options = options_for_test();
    options.ack.limits.outstanding_requests = 2;
    host_session session = ready_session(options);
    session_output out;
    REQUIRE(session.request(other("a.one"), start, out).has_value());
    REQUIRE(session.request(other("a.two"), start, out).has_value());
    const auto third = session.request(other("a.three"), start, out);
    REQUIRE(third.has_error());
    REQUIRE(third.error() == api_errc::busy);
    REQUIRE_EQ(out.send.size(), std::size_t{2});

    // A message that cannot be written is refused, and uses no id.
    api_message bad = other("a.four");
    bad.op = "another.op";
    REQUIRE(session.notify(bad, start, out).error() == api_errc::bad_request);
    REQUIRE(session.notify(other("a.note"), start, out).has_value());
    REQUIRE_EQ(out.send.size(), std::size_t{3});
}

TEST_CASE("plugin session: when a session ends, open requests are told")
{
    host_session session = ready_session();
    session_output out;
    const auto first = session.request(other("a.one"), start, out);
    const auto second = session.request(other("a.two"), start, out);
    const session_output closed = session.on_channel_closed(start + 1s);
    const std::vector<answer_event> answers = events_of<answer_event>(closed);
    REQUIRE_EQ(answers.size(), std::size_t{2});
    REQUIRE_EQ(answers[0].id, first.value());
    REQUIRE_EQ(answers[1].id, second.value());
    for (const answer_event& answer : answers)
        REQUIRE(answer.result == api_errc::plugin_unavailable);
    require_closed(closed, plugin_errc::exited);

    // Nothing more is taken or sent.
    session_output after;
    REQUIRE(session.request(other("a.three"), start + 2s, after).error() ==
            api_errc::plugin_unavailable);
    REQUIRE(session.notify(other("a.note"), start + 2s, after).error() ==
            api_errc::plugin_unavailable);
    REQUIRE(session.answer(1, make(ok_message{}), start + 2s, after).error() ==
            api_errc::plugin_unavailable);
    REQUIRE(after.send.empty());
    const session_output ignored =
        session.on_bytes(bytes_of(make(status_message{})), start + 2s);
    REQUIRE(ignored.events.empty());
    REQUIRE(ignored.send.empty());
    REQUIRE(session.on_time(start + 100s).events.empty());
    REQUIRE(session.on_channel_closed(start + 3s).events.empty());
    REQUIRE(session.shut_down("quit", start + 3s).events.empty());
    REQUIRE(!session.next_deadline().has_value());
}

// -- breaking the rules -----------------------------------------------------

TEST_CASE("plugin session: a plugin that breaks a rule ends its session")
{
    const auto breaks = [](const byte_buffer& bytes, std::string_view part) {
        host_session session = ready_session();
        const session_output out = session.on_bytes(bytes, start + 1s);
        require_closed(out, plugin_errc::misbehaving, part);
        REQUIRE(session.state() == session_state::closed);
    };
    breaks(bytes_of(hello(2)), "hello a second time");
    breaks(bytes_of(make(hello_ack_message{}, 0, 1)), "only the host");
    breaks(bytes_of(make(shutdown_message{}, 3)), "only the host");
    {
        agent::frame frame;
        frame.header = R"({"op":"Not A Name"})";
        breaks(agent::encode_frame(frame).value(), "not as the plugin API");
    }
    {
        agent::frame frame;
        frame.header = "not json";
        breaks(agent::encode_frame(frame).value(), "not as the plugin API");
    }
    breaks(byte_buffer{0xff, 0xff, 0xff, 0x7f}, "not a frame");

    // What came before the bad message in the same piece still counts.
    host_session session = ready_session();
    byte_buffer bytes = bytes_of(make(line_of("last words")));
    const byte_buffer second = bytes_of(hello(2));
    bytes.insert(bytes.end(), second.begin(), second.end());
    const byte_buffer third = bytes_of(make(line_of("never read")));
    bytes.insert(bytes.end(), third.begin(), third.end());
    const session_output out = session.on_bytes(bytes, start + 1s);
    const std::vector<log_event> lines = events_of<log_event>(out);
    REQUIRE_EQ(lines.size(), std::size_t{1});
    REQUIRE_EQ(lines[0].line.text, "last words");
    require_closed(out, plugin_errc::misbehaving);
}

TEST_CASE("plugin session: the line that says why never repeats the plugin")
{
    const std::string marker = "MARKER-FROM-THE-PLUGIN";
    host_session session = ready_session();
    agent::frame frame;
    frame.header = R"({"op":"log","level":"info","text":7,"x":")" + marker +
                   R"("})";
    const session_output out =
        session.on_bytes(agent::encode_frame(frame).value(), start + 1s);
    const closed_event closed = closed_of(out);
    REQUIRE(closed.detail.find(marker) == std::string::npos);

    host_session early(options_for_test(), start);
    const session_output first =
        early.on_bytes(bytes_of(hello(1, {1, 0}, "com.example.marker")), start);
    REQUIRE(closed_of(first).detail.find("marker") == std::string::npos);
}

// -- rates ------------------------------------------------------------------

TEST_CASE("plugin session: a plugin over a rate is slowed, not ended at once")
{
    session_options options = options_for_test();
    options.ack.limits.frames_per_second = 5;
    options.ack.limits.frames_burst = 5;
    host_session session = ready_session(options);
    // The hello was the first frame of this second: four more fit.
    const byte_buffer status = bytes_of(make(status_message{}));
    session_output out;
    for (int i = 0; i < 4; ++i)
    {
        out = session.on_bytes(status, start + 100ms);
        REQUIRE(!out.read_again_at.has_value());
    }
    // One more is still read, and the owner is told to wait for the next
    // second.
    out = session.on_bytes(status, start + 200ms);
    REQUIRE_EQ(events_of<status_event>(out).size(), std::size_t{1});
    REQUIRE(out.read_again_at.has_value());
    REQUIRE(out.read_again_at.value() == start + 1s);
    REQUIRE(session.state() == session_state::ready);

    // In the next second it may send again.
    out = session.on_bytes(status, start + 1s);
    REQUIRE(!out.read_again_at.has_value());
}

TEST_CASE("plugin session: frames a plugin did not use are kept, up to the "
          "burst")
{
    session_options options = options_for_test();
    options.ack.limits.frames_per_second = 2;
    options.ack.limits.frames_burst = 6;
    host_session session = ready_session(options);
    const byte_buffer status = bytes_of(make(status_message{}));
    // One second with one frame (the hello), then two quiet seconds: the
    // burst is there, and no more than the burst.
    session_output out;
    for (int i = 0; i < 6; ++i)
    {
        out = session.on_bytes(status, start + 3s);
        REQUIRE(!out.read_again_at.has_value());
    }
    out = session.on_bytes(status, start + 3s);
    REQUIRE(out.read_again_at.has_value());
}

TEST_CASE("plugin session: bytes and log lines have rates too")
{
    {
        session_options options = options_for_test();
        options.ack.limits.bytes_per_second = 200;
        host_session session = ready_session(options);
        api_message big = other("a.data");
        big.blob.assign(400, byte{1});
        const session_output out = session.on_bytes(bytes_of(big), start + 1s);
        REQUIRE(out.read_again_at.has_value());
        REQUIRE(out.read_again_at.value() == start + 2s);
        // The message was read all the same.
        REQUIRE_EQ(events_of<message_event>(out).size(), std::size_t{1});
    }
    {
        session_options options = options_for_test();
        options.ack.limits.log_lines_per_second = 3;
        host_session session = ready_session(options);
        byte_buffer bytes;
        for (int i = 0; i < 5; ++i)
        {
            const byte_buffer line = bytes_of(make(line_of("line")));
            bytes.insert(bytes.end(), line.begin(), line.end());
        }
        const session_output out = session.on_bytes(bytes, start + 1s);
        // Three lines are kept; the rest are dropped.
        REQUIRE_EQ(events_of<log_event>(out).size(), std::size_t{3});
        REQUIRE(out.read_again_at.has_value());
        REQUIRE(session.state() == session_state::ready);
    }
}

TEST_CASE("plugin session: a plugin that stays over a rate is ended")
{
    session_options options = options_for_test();
    options.ack.limits.frames_per_second = 1;
    options.ack.limits.frames_burst = 1;
    options.seconds_over_rate = 3;
    host_session session = ready_session(options);
    const byte_buffer status = bytes_of(make(status_message{}));
    byte_buffer two = status;
    two.insert(two.end(), status.begin(), status.end());

    // Over the rate in three seconds in a row.
    session_output out;
    for (int second = 1; second <= 3; ++second)
    {
        out = session.on_bytes(two, start + std::chrono::seconds(second));
        REQUIRE(out.read_again_at.has_value());
        REQUIRE(session.state() == session_state::ready);
    }
    // When the third such second is over, so is the session.
    out = session.on_time(start + 4s);
    require_closed(out, plugin_errc::misbehaving, "frames per second");

    // A second inside its rate in between starts the count again.
    host_session patient = ready_session(options);
    static_cast<void>(patient.on_bytes(two, start + 1s));
    static_cast<void>(patient.on_bytes(two, start + 2s));
    static_cast<void>(patient.on_bytes(status, start + 3s));
    static_cast<void>(patient.on_bytes(two, start + 4s));
    static_cast<void>(patient.on_bytes(two, start + 5s));
    REQUIRE(patient.on_time(start + 6s).events.empty());
    REQUIRE(patient.state() == session_state::ready);
}

// -- stopping ---------------------------------------------------------------

TEST_CASE("plugin session: an orderly stop asks, waits and ends")
{
    host_session session = ready_session();
    session_output out = session.shut_down("quit", start + 1s);
    REQUIRE(session.state() == session_state::stopping);
    const std::vector<api_message> messages = sent(out);
    REQUIRE_EQ(messages.size(), std::size_t{1});
    const auto* const body = std::get_if<shutdown_message>(&messages[0].body);
    REQUIRE(body != nullptr);
    REQUIRE_EQ(body->reason, "quit");
    REQUIRE(messages[0].id != 0);
    REQUIRE(session.next_deadline() == start + 4000ms);

    // Asking again does nothing, and no request is taken any more.
    REQUIRE(session.shut_down("quit", start + 1s).send.empty());
    session_output refused;
    REQUIRE(session.request(other("a.one"), start + 1s, refused).error() ==
            api_errc::plugin_unavailable);

    // What the plugin still says while it stops is taken.
    out = session.on_bytes(bytes_of(make(line_of("stopping"))), start + 2s);
    REQUIRE_EQ(events_of<log_event>(out).size(), std::size_t{1});

    out = session.on_bytes(bytes_of(make(bye_message{}, 0, messages[0].id)),
                           start + 2s);
    const closed_event closed = closed_of(out);
    REQUIRE(!closed.reason.failed());
    REQUIRE(session.state() == session_state::closed);
}

TEST_CASE("plugin session: a plugin that does not say bye in time is given "
          "up on")
{
    host_session session = ready_session();
    static_cast<void>(session.shut_down("quit", start + 1s));
    REQUIRE(session.on_time(start + 3999ms).events.empty());
    require_closed(session.on_time(start + 4000ms), plugin_errc::unresponsive,
                   "bye");

    // A plugin that just goes when asked has stopped as asked.
    host_session gone = ready_session();
    static_cast<void>(gone.shut_down("quit", start + 1s));
    const closed_event closed = closed_of(gone.on_channel_closed(start + 2s));
    REQUIRE(!closed.reason.failed());
}

TEST_CASE("plugin session: leaving without being asked is not an orderly "
          "stop")
{
    host_session session = ready_session();
    require_closed(session.on_bytes(bytes_of(make(bye_message{})), start + 1s),
                   plugin_errc::exited, "without being asked");

    host_session dropped = ready_session();
    require_closed(dropped.on_channel_closed(start + 1s), plugin_errc::exited);

    // A stop before the plugin was ready has nobody to ask.
    host_session early(options_for_test(), start);
    const session_output out = early.shut_down("quit", start + 1ms);
    REQUIRE(out.send.empty());
    REQUIRE(!closed_of(out).reason.failed());
    REQUIRE(early.state() == session_state::closed);
}

TEST_CASE("plugin session: the same input and times give the same output")
{
    const auto run = [] {
        host_session session(options_for_test(), start);
        std::vector<agent::frame> frames;
        const auto keep = [&](const session_output& out) {
            frames.insert(frames.end(), out.send.begin(), out.send.end());
        };
        keep(session.on_bytes(bytes_of(hello(3)), start + 1ms));
        keep(session.on_bytes(bytes_of(make(ping_message{}, 4)), start + 2ms));
        keep(session.on_time(start + 5002ms));
        session_output out;
        static_cast<void>(session.request(other("a.one"), start + 6s, out));
        keep(out);
        keep(session.shut_down("quit", start + 7s));
        return frames;
    };
    const std::vector<agent::frame> first = run();
    REQUIRE_EQ(first.size(), std::size_t{5});
    REQUIRE(run() == first);
}
