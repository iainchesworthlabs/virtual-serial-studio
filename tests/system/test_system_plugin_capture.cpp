//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The extension point `capture.*` (`make_capture_point`) on its own.  A test
// makes the handler with a hand-made `extension_context`: the link records
// what is answered, the executor is an `io_context` that the test runs by
// hand, and the hub is a real one with a mock port and a loopback port, so
// that the traffic tap sees real events.  Nothing here starts a process.  The
// recordings are written into a scratch folder and read back with
// `read_recording`.
//

#include "support/test_support.hpp"

#include <vss/core/control_lines.hpp>
#include <vss/core/event.hpp>
#include <vss/core/types.hpp>
#include <vss/plugin/api_capture.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/capture_point.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/recording.hpp>
#include <vss/plugin/host/traffic_tap.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/io_context.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using vss::control_line;
using vss::data_direction;
using vss::endpoint_event;
using vss::event_filter;
using vss::event_kind;
using vss::plugin::api_errc;
using vss::plugin::api_message;
using vss::plugin::capture_result_message;
using vss::plugin::capture_start_message;
using vss::plugin::capture_status_message;
using vss::plugin::capture_stop_message;
using vss::plugin::decode_as;
using vss::plugin::encode_as;
using vss::plugin::error_message;
using vss::plugin::ok_message;
using vss::plugin::other_message;
using vss::plugin::permission;
using vss::plugin::ping_message;
using vss::plugin::host::capture_place;
using vss::plugin::host::extension_context;
using vss::plugin::host::extension_handler;
using vss::plugin::host::make_capture_point;
using vss::plugin::host::read_recording;
using vss::plugin::host::recording_event;
using vss::plugin::host::recording_event_kind;
using vss::plugin::host::recording_index_name;
using vss::testkit::wait_until;
using vss_test_support::add_loopback;
using vss_test_support::add_mock;
using vss_test_support::event_recorder;
using vss_test_support::mock_endpoint;
using vss_test_support::scratch_directory;
using vss_test_support::start_hub;
using vss_test_support::subscription_guard;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;

constexpr std::string_view port_name = "port-a";
constexpr std::string_view lines_name = "port-lines";
constexpr std::uint64_t first_id = 7;

/// What the handler said to the plugin: the request it answers, and the
/// answer.
struct answered
{
    std::uint64_t re = 0;
    api_message message{};
};

/// The handler with everything around it.  The order of the members is the
/// order they are made in, and the handler goes first: it must not outlive
/// the hub it taps, the executor or the folder it writes into.
struct rig
{
    explicit rig(std::vector<permission> granted = {permission::traffic_tap})
        : hub(start_hub()), data(scratch.path() / "data"),
          serial(add_mock(*hub, std::string(port_name))),
          wire(add_loopback(*hub, std::string(lines_name)))
    {
        extension_context context;
        context.plugin_id = "com.example.capture";
        context.plugin_name = "Capture plugin";
        context.plugin_version = "1.2.3";
        context.granted = std::move(granted);
        context.data_directory = data;
        context.executor = io.get_executor();
        context.link.answer = [this](std::uint64_t re, api_message message)
        { answers.push_back(answered{re, std::move(message)}); };
        handler = make_capture_point(
            context, *hub,
            [this](std::string_view name) { return find(name); });
    }

    rig(const rig&) = delete;
    rig& operator=(const rig&) = delete;

    /// The places the plugin may record: the two ports of the rig.
    std::optional<capture_place> find(std::string_view name) const
    {
        capture_place place;
        place.settings = "115200 8N1";
        if (name == port_name)
            place.tap.endpoints = {serial.endpoint->id()};
        else if (name == lines_name)
            place.tap.endpoints = {wire->id()};
        else
            return std::nullopt;
        place.tap.name = std::string(name);
        return place;
    }

    /// The strand's work, run now.
    void pump()
    {
        io.poll();
        io.restart();
    }

    /// Offers the handler a request, which it must take and answer at once,
    /// and returns the answer.
    answered ask(const api_message& request)
    {
        const std::size_t before = answers.size();
        REQUIRE(handler->take(request));
        REQUIRE_EQ(answers.size(), before + 1);
        return answers.back();
    }

    answered start(const capture_start_message& body);
    answered stop();
    answered status();

    /// The answer to `capture.status`, or nothing when it is an error.
    std::optional<capture_result_message> report();

    /// Sends text from the mock port into the hub.
    void send(const std::string& text) { serial.inject_read(text); }

    /// Runs the strand until the recording holds `bytes` bytes of traffic.
    bool recorded(std::uint64_t bytes)
    {
        return wait_until(
            [&]
            {
                pump();
                const auto current = report();
                return current.has_value() && current->bytes >= bytes;
            },
            20000ms);
    }

    /// Runs the strand until the recording is over.
    bool over()
    {
        return wait_until(
            [&]
            {
                pump();
                const auto current = report();
                return current.has_value() && !current->running;
            },
            20000ms);
    }

    /// The folder of a recording, from its path in a `capture.result`.
    fs::path folder_of(std::string_view relative) const
    {
        return data / fs::path(relative);
    }

    scratch_directory scratch{};
    boost::asio::io_context io{};
    std::shared_ptr<vss::hub> hub;
    fs::path data;
    mock_endpoint serial;
    std::shared_ptr<vss::endpoint> wire;
    std::vector<answered> answers{};
    std::uint64_t next_id = 100;
    std::unique_ptr<extension_handler> handler{};
};

// -- messages -----------------------------------------------------------------

template <class Message>
api_message request_of(const Message& body, std::uint64_t id)
{
    auto encoded = encode_as(body, id);
    if (encoded.has_error())
        throw vss_test::failure("a request would not encode: " +
                                std::string(Message::op()));
    return std::move(encoded).value();
}

/// A request whose body is the header text as it is, whatever it holds: the
/// handler reads strictly, and a session would not have passed some of these.
api_message raw_request(std::string_view op, std::uint64_t id,
                        std::string_view header)
{
    api_message message;
    message.op = std::string(op);
    message.id = id;
    message.body = other_message{std::string(header)};
    return message;
}

/// A `capture.start` for one place of the rig, with the name the plugin gives
/// the recording.  Empty: the host chooses.
capture_start_message start_of(std::string_view place, std::string name = {})
{
    capture_start_message body;
    body.places = {std::string(place)};
    body.name = std::move(name);
    return body;
}

answered rig::start(const capture_start_message& body)
{
    return ask(request_of(body, next_id++));
}

answered rig::stop()
{
    return ask(request_of(capture_stop_message{}, next_id++));
}

answered rig::status()
{
    return ask(request_of(capture_status_message{}, next_id++));
}

std::optional<capture_result_message> rig::report()
{
    const answered reply = status();
    if (std::holds_alternative<error_message>(reply.message.body))
        return std::nullopt;
    auto read = decode_as<capture_result_message>(reply.message);
    if (read.has_error())
        throw vss_test::failure("the status answer does not read back");
    return std::move(read).value();
}

/// The code an answer carries: `ok`, or the name of the `api_errc` of an
/// error.
std::string code_of(const answered& answer)
{
    if (std::holds_alternative<ok_message>(answer.message.body))
        return "ok";
    const auto* const error = std::get_if<error_message>(&answer.message.body);
    if (error == nullptr)
        return answer.message.op;
    REQUIRE_EQ(error->category, std::string("vss.plugin.api"));
    return error->value;
}

std::string code_name(api_errc code)
{
    return std::string(vss::plugin::to_string(code));
}

/// A `capture.result` that must be there.
capture_result_message result_of(const answered& answer)
{
    auto read = decode_as<capture_result_message>(answer.message);
    if (read.has_error())
        throw vss_test::failure("the answer is not a capture.result: " +
                                code_of(answer));
    return std::move(read).value();
}

// -- recordings ---------------------------------------------------------------

/// The lines of the file of one place of a recording, read back with the
/// reader of the format.
std::vector<recording_event> events_of(const fs::path& folder,
                                       std::size_t place = 0)
{
    std::string detail;
    auto found = read_recording(folder, &detail);
    if (found.has_error())
        throw vss_test::failure("the recording was refused: " + detail);
    const auto& whole = found.value();
    auto opened = whole.open_place(place);
    if (opened.has_error())
        throw vss_test::failure("a place file would not open");
    auto& reader = opened.value();

    std::vector<recording_event> events;
    for (;;)
    {
        auto next = reader.next();
        if (next.has_error())
            throw vss_test::failure("a place file was refused: " +
                                    std::string(reader.detail()));
        if (!next.value().has_value())
            break;
        events.push_back(*next.value());
    }
    return events;
}

/// The traffic of the data lines, in order.
std::string text_of(const std::vector<recording_event>& events)
{
    std::string text;
    for (const recording_event& event : events)
    {
        if (event.kind == recording_event_kind::data)
            text.append(event.bytes.begin(), event.bytes.end());
    }
    return text;
}

/// Whether every data line has the direction `direction`.
bool all_data_is(const std::vector<recording_event>& events,
                 data_direction direction)
{
    for (const recording_event& event : events)
    {
        if (event.kind == recording_event_kind::data &&
            event.direction != direction)
            return false;
    }
    return true;
}

std::string slurp(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::size_t entries_in(const fs::path& folder)
{
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(folder))
    {
        static_cast<void>(entry);
        ++count;
    }
    return count;
}

std::uint64_t size_of(const std::string& text)
{
    return static_cast<std::uint64_t>(text.size());
}

} // namespace

TEST_CASE("plugin capture: a start without traffic.tap is not_granted")
{
    rig fixture(std::vector<permission>{permission::ports_read});
    const answered reply = fixture.start(start_of(port_name, "alpha"));
    REQUIRE_EQ(reply.re, std::uint64_t{100});
    REQUIRE_EQ(code_of(reply), code_name(api_errc::not_granted));

    // Nothing was made, and nothing runs.
    REQUIRE(!fs::exists(fixture.data / "recordings"));
    REQUIRE_EQ(code_of(fixture.status()), code_name(api_errc::refused));
}

TEST_CASE("plugin capture: a start naming a place nobody knows is refused")
{
    rig fixture;
    capture_start_message body = start_of(port_name, "alpha");
    body.places.emplace_back("no-such-place");
    REQUIRE_EQ(code_of(fixture.start(body)), code_name(api_errc::refused));

    // The same for a place that is the only one.
    REQUIRE_EQ(code_of(fixture.start(start_of("no-such-place"))),
               code_name(api_errc::refused));
    REQUIRE_EQ(code_of(fixture.status()), code_name(api_errc::refused));
    REQUIRE(!fs::exists(fixture.data / "recordings" / "alpha"));
}

TEST_CASE("plugin capture: a malformed start is bad_request")
{
    rig fixture;
    const std::vector<std::string> headers{
        R"({"op":"capture.start","id":7})",
        R"({"op":"capture.start","id":7,"places":5})",
        R"({"op":"capture.start","id":7,"places":[]})",
        R"({"op":"capture.start","id":7,"places":["port-a"],"name":5})",
        R"({"op":"capture.start","id":7,"places":["port-a"],"name":"a/b"})",
        R"({"op":"capture.start","id":7,"places":["port-a"],"limit_bytes":0})",
        R"({"op":"capture.start","id":7,"places":["port-a"],"ring":61})",
    };
    for (const std::string& header : headers)
    {
        const answered reply =
            fixture.ask(raw_request("capture.start", first_id, header));
        if (reply.re != 0 || code_of(reply) != code_name(api_errc::bad_request))
            throw vss_test::failure("not bad_request: " + header);
    }

    // A body this code knows under the op is wrong too.
    api_message request;
    request.op = "capture.start";
    request.id = first_id;
    request.body = ping_message{};
    REQUIRE_EQ(code_of(fixture.ask(request)), code_name(api_errc::bad_request));

    // And nothing was started by any of them.
    REQUIRE_EQ(code_of(fixture.status()), code_name(api_errc::refused));
    REQUIRE(!fs::exists(fixture.data / "recordings" / "a"));
}

TEST_CASE("plugin capture: a status shows a running recording with its name")
{
    rig fixture;
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               std::string("ok"));
    const std::string text = "hello from the port";
    fixture.send(text);
    REQUIRE(fixture.recorded(size_of(text)));

    const answered reply = fixture.status();
    const capture_result_message current = result_of(reply);
    REQUIRE_EQ(reply.message.op, std::string("capture.result"));
    REQUIRE(current.running);
    REQUIRE(!current.ring);
    REQUIRE_EQ(current.bytes, size_of(text));
    REQUIRE_EQ(current.dropped, std::uint64_t{0});
    REQUIRE_EQ(current.recording, std::string("recordings/alpha"));
    // The index is written last, so a recording that runs has no size yet.
    REQUIRE_EQ(current.size, std::uint64_t{0});
    REQUIRE(!fs::exists(fixture.folder_of(current.recording) /
                        std::string(recording_index_name)));
}

TEST_CASE("plugin capture: a start without a name is recordings/recording-1, "
          "then recording-2")
{
    rig fixture;
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name))), std::string("ok"));
    fixture.send("one");
    REQUIRE(fixture.recorded(3));
    const auto first = fixture.report();
    REQUIRE(first.has_value());
    REQUIRE(first->running);
    REQUIRE_EQ(first->recording, std::string("recordings/recording-1"));

    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
    REQUIRE(fs::exists(fixture.data / "recordings" / "recording-1" /
                       std::string(recording_index_name)));

    // The host never writes into a folder that is there already.
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name))), std::string("ok"));
    const auto second = fixture.report();
    REQUIRE(second.has_value());
    REQUIRE(second->running);
    REQUIRE_EQ(second->recording, std::string("recordings/recording-2"));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
}

TEST_CASE("plugin capture: a second start while one runs is busy")
{
    rig fixture;
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               std::string("ok"));
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "beta"))),
               code_name(api_errc::busy));
    REQUIRE_EQ(code_of(fixture.start(start_of(lines_name, "gamma"))),
               code_name(api_errc::busy));

    // The first still runs, and the others made nothing.
    const auto current = fixture.report();
    REQUIRE(current.has_value());
    REQUIRE(current->running);
    REQUIRE_EQ(current->recording, std::string("recordings/alpha"));
    REQUIRE(!fs::exists(fixture.data / "recordings" / "beta"));
    REQUIRE(!fs::exists(fixture.data / "recordings" / "gamma"));
}

TEST_CASE("plugin capture: stop finishes the recording and what was sent is "
          "in it")
{
    rig fixture;
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               std::string("ok"));
    const std::string first = "the first chunk, ";
    const std::string second = "and the second.";
    fixture.send(first);
    fixture.send(second);
    const std::string sent = first + second;
    REQUIRE(fixture.recorded(size_of(sent)));

    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));

    const fs::path folder = fixture.data / "recordings" / "alpha";
    REQUIRE(fs::exists(folder / std::string(recording_index_name)));
    const std::vector<recording_event> events = events_of(folder);
    REQUIRE(text_of(events) == sent);
    // The port received the bytes, which a recording writes as `R`.
    REQUIRE(all_data_is(events, data_direction::rx));

    const auto after = fixture.report();
    REQUIRE(after.has_value());
    REQUIRE(!after->running);
    REQUIRE_EQ(after->bytes, size_of(sent));
    REQUIRE_EQ(after->dropped, std::uint64_t{0});
    REQUIRE_EQ(after->recording, std::string("recordings/alpha"));
    REQUIRE(after->size > 0);
}

TEST_CASE("plugin capture: the index names the place, its settings and the "
          "plugin")
{
    rig fixture;
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               std::string("ok"));
    fixture.send("abc");
    REQUIRE(fixture.recorded(3));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));

    auto found = read_recording(fixture.data / "recordings" / "alpha");
    REQUIRE(found.has_value());
    const auto& index = found.value().index;
    REQUIRE_EQ(index.places.size(), std::size_t{1});
    REQUIRE_EQ(index.places.front().name, std::string(port_name));
    REQUIRE_EQ(index.places.front().settings, std::string("115200 8N1"));
    REQUIRE_EQ(index.places.front().bytes, std::uint64_t{3});
    REQUIRE_EQ(index.made_by.plugin, std::string("com.example.capture"));
    REQUIRE_EQ(index.made_by.version, std::string("1.2.3"));
}

TEST_CASE("plugin capture: stop with nothing running is ok, and a status "
          "before any start is refused")
{
    rig fixture;
    REQUIRE_EQ(code_of(fixture.status()), code_name(api_errc::refused));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
    REQUIRE_EQ(code_of(fixture.status()), code_name(api_errc::refused));

    // A recording that was stopped is stopped for good, and stopping it
    // again is not an error either.
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               std::string("ok"));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
    const auto after = fixture.report();
    REQUIRE(after.has_value());
    REQUIRE(!after->running);
}

TEST_CASE("plugin capture: a name that is a folder already is refused and the "
          "folder is left alone")
{
    rig fixture;
    const fs::path taken = fixture.data / "recordings" / "taken";
    fs::create_directories(taken);
    {
        std::ofstream out(taken / "keep.txt", std::ios::binary);
        out << "keep this";
    }

    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "taken"))),
               code_name(api_errc::refused));
    REQUIRE_EQ(entries_in(taken), std::size_t{1});
    REQUIRE_EQ(slurp(taken / "keep.txt"), std::string("keep this"));
    REQUIRE(!fs::exists(taken / std::string(recording_index_name)));
    REQUIRE_EQ(code_of(fixture.status()), code_name(api_errc::refused));

    // A recording that was made before is as much taken as any folder.
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               std::string("ok"));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
    const std::string index_before =
        slurp(fixture.data / "recordings" / "alpha" /
              std::string(recording_index_name));
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               code_name(api_errc::refused));
    REQUIRE_EQ(slurp(fixture.data / "recordings" / "alpha" /
                     std::string(recording_index_name)),
               index_before);
}

TEST_CASE("plugin capture: a recording stops at limit_bytes and a new one may "
          "start")
{
    rig fixture;
    capture_start_message body = start_of(port_name, "limited");
    body.limit_bytes = 10;
    REQUIRE_EQ(code_of(fixture.start(body)), std::string("ok"));

    // More than the limit, in several chunks.
    std::string sent;
    for (int round = 0; round < 3; ++round)
    {
        const std::string chunk = "0123456789";
        fixture.send(chunk);
        sent += chunk;
    }
    REQUIRE(fixture.over());

    const auto after = fixture.report();
    REQUIRE(after.has_value());
    REQUIRE(!after->running);
    REQUIRE(after->bytes <= body.limit_bytes);
    REQUIRE(after->size > 0);
    REQUIRE_EQ(after->recording, std::string("recordings/limited"));

    // What was kept is the start of what was sent, whole.
    const fs::path folder = fixture.data / "recordings" / "limited";
    REQUIRE(fs::exists(folder / std::string(recording_index_name)));
    const std::string kept = text_of(events_of(folder));
    REQUIRE_EQ(size_of(kept), after->bytes);
    REQUIRE(sent.starts_with(kept));

    // The recording is over, so the plugin may record again.
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "again"))),
               std::string("ok"));
    const auto again = fixture.report();
    REQUIRE(again.has_value());
    REQUIRE(again->running);
    REQUIRE_EQ(again->recording, std::string("recordings/again"));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
}

TEST_CASE("plugin capture: plugin_gone while recording finishes the "
          "recording")
{
    rig fixture;
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               std::string("ok"));
    const std::string sent = "bytes before the plugin went";
    fixture.send(sent);
    REQUIRE(fixture.recorded(size_of(sent)));

    fixture.handler->plugin_gone();

    const fs::path folder = fixture.data / "recordings" / "alpha";
    REQUIRE(fs::exists(folder / std::string(recording_index_name)));
    REQUIRE(text_of(events_of(folder)) == sent);

    // A plugin that starts again starts from nothing: no recording is
    // reported, and it may record under a new name.
    REQUIRE_EQ(code_of(fixture.status()), code_name(api_errc::refused));
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "beta"))),
               std::string("ok"));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
}

TEST_CASE("plugin capture: plugin_gone with nothing running is harmless")
{
    rig fixture;
    fixture.handler->plugin_gone();
    REQUIRE_EQ(code_of(fixture.status()), code_name(api_errc::refused));
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               std::string("ok"));
    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
    fixture.handler->plugin_gone();
    REQUIRE_EQ(code_of(fixture.status()), code_name(api_errc::refused));
}

TEST_CASE("plugin capture: destroying the handler while recording leaves an "
          "index")
{
    rig fixture;
    REQUIRE_EQ(code_of(fixture.start(start_of(port_name, "alpha"))),
               std::string("ok"));
    const std::string sent = "bytes before the handler went";
    fixture.send(sent);
    REQUIRE(fixture.recorded(size_of(sent)));

    fixture.handler.reset();

    // What was posted to the strand and had not run finds nothing to do.
    fixture.pump();
    fixture.send("after");
    fixture.pump();

    const fs::path folder = fixture.data / "recordings" / "alpha";
    REQUIRE(fs::exists(folder / std::string(recording_index_name)));
    REQUIRE(text_of(events_of(folder)) == sent);
}

TEST_CASE("plugin capture: a change of a control line is a line event")
{
    rig fixture;
    REQUIRE_EQ(code_of(fixture.start(start_of(lines_name, "wires"))),
               std::string("ok"));

    // A recorder that subscribes after the tap has the event after it.
    event_recorder seen;
    event_filter filter;
    filter.kind = event_kind::lines;
    const subscription_guard guard(
        *fixture.hub, filter,
        [seen](const endpoint_event& event) { seen.record(event); });
    REQUIRE(!fixture.wire->set_line(control_line::dtr, true));
    REQUIRE(wait_until([&] { return seen.count(event_kind::lines) >= 1; },
                       20000ms));
    fixture.pump();

    REQUIRE_EQ(code_of(fixture.stop()), std::string("ok"));
    const std::vector<recording_event> events =
        events_of(fixture.data / "recordings" / "wires");
    REQUIRE(!events.empty());
    const recording_event& change = events.front();
    REQUIRE(change.kind == recording_event_kind::line);
    REQUIRE(change.lines.dtr());
    REQUIRE_EQ(change.changed.bits, vss::line_mask(control_line::dtr));
}
