//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/agent/proto/frame.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include "test_framework.hpp"

#include <boost/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace vss;
using namespace vss::plugin;
using namespace std::string_view_literals;

namespace
{

namespace bj = boost::json;

/// A marker that no detail line may ever contain.
constexpr std::string_view marker = "MARKER_7f3a";

// -- building and reading frames ---------------------------------------------

agent::frame frame_of(std::string_view header, byte_buffer blob = {})
{
    agent::frame out;
    out.header = std::string(header);
    out.blob = std::move(blob);
    return out;
}

/// Decodes a header that must be accepted.
api_message decode_ok(std::string_view header, byte_buffer blob = {})
{
    std::string detail = "stale";
    const auto decoded =
        decode_message(frame_of(header, std::move(blob)), &detail);
    REQUIRE_EQ(detail, std::string{});
    REQUIRE(decoded.has_value());
    return decoded.value();
}

/// The detail line for a header that must be refused as `bad_request`.
std::string refusal_of(std::string_view header)
{
    std::string detail;
    const auto decoded = decode_message(frame_of(header), &detail);
    REQUIRE(decoded.has_error());
    REQUIRE(decoded.error() == api_errc::bad_request);
    REQUIRE(!detail.empty());
    // Without a place for the detail the answer is the same.
    const auto again = decode_message(frame_of(header));
    REQUIRE(again.has_error());
    REQUIRE(again.error() == api_errc::bad_request);
    return detail;
}

void require_refused(std::string_view header, std::string_view fragment)
{
    const std::string detail = refusal_of(header);
    if (detail.find(fragment) == std::string::npos)
    {
        std::string text = "the detail for ";
        text += header.substr(0, 200);
        text += "\n  is: ";
        text += detail;
        text += "\n  expected to hold: ";
        text += fragment;
        throw vss_test::failure(text);
    }
}

std::string repeated(char c, std::size_t count)
{
    return std::string(count, c);
}

api_message message_of(message_body body, std::uint64_t id = 0,
                       std::uint64_t re = 0)
{
    api_message out;
    out.id = id;
    out.re = re;
    out.body = std::move(body);
    return out;
}

/// The header `encode_message` writes, which must be there.
std::string header_of(const api_message& message)
{
    const auto encoded = encode_message(message);
    REQUIRE(encoded.has_value());
    return encoded.value().header;
}

/// `message` is written as `expected`, and that reads back as `message`
/// with its op filled in.
void check_wire(const api_message& message, const std::string& expected)
{
    const auto encoded = encode_message(message);
    REQUIRE(encoded.has_value());
    REQUIRE_EQ(encoded.value().header, expected);
    REQUIRE(encoded.value().blob == message.blob);

    std::string detail = "stale";
    const auto decoded = decode_message(encoded.value(), &detail);
    REQUIRE_EQ(detail, std::string{});
    REQUIRE(decoded.has_value());
    api_message want = message;
    if (want.op.empty())
        want.op = std::string(op_of(want.body));
    REQUIRE(decoded.value() == want);

    // Writing what was read gives the same bytes.
    REQUIRE_EQ(header_of(decoded.value()), expected);
}

void require_not_written(const api_message& message)
{
    const auto encoded = encode_message(message);
    REQUIRE(encoded.has_error());
    REQUIRE(encoded.error() == api_errc::bad_request);
}

const byte_buffer& sample_blob()
{
    static const byte_buffer blob{0x00, 0xff, 0x10, 0x20};
    return blob;
}

// -- the messages written in the tests ---------------------------------------

hello_message sample_hello()
{
    hello_message body;
    body.api = api_version{1, 0};
    body.plugin_id = "com.example.tool";
    body.version = "1.2.3";
    body.pid = 4242;
    return body;
}

hello_ack_message sample_hello_ack()
{
    hello_ack_message body;
    body.api = api_version{1, 0};
    body.granted = {permission::ports_read, permission::traffic_tap};
    body.capabilities = {"engine_rules", "gpu_canvas"};
    body.data_directory = "/data/com.example.tool";
    body.settings = R"({"rate":9600,"name":"a"})";
    return body;
}

constexpr std::string_view hello_text =
    R"({"op":"hello","id":1,"api":"1.0","plugin_id":"com.example.tool",)"
    R"("version":"1.2.3","pid":4242})";

constexpr std::string_view hello_ack_text =
    R"({"op":"hello_ack","re":1,"api":"1.0",)"
    R"("granted":["ports.read","traffic.tap"],)"
    R"("capabilities":["engine_rules","gpu_canvas"],)"
    R"("limits":{"frames_per_second":2000,"frames_burst":4000,)"
    R"("bytes_per_second":8388608,"outstanding_requests":32,)"
    R"("log_lines_per_second":100,"instances":16},)"
    R"("data_directory":"/data/com.example.tool",)"
    R"("settings":{"rate":9600,"name":"a"}})";

constexpr std::string_view status_text =
    R"({"op":"status","level":"warning","text":"slow","code":"slow.link"})";

constexpr std::string_view log_text =
    R"({"op":"log","level":"info","text":"started"})";

constexpr std::string_view shutdown_text =
    R"({"op":"shutdown","id":2,"reason":"quit"})";

constexpr std::string_view error_text =
    R"({"op":"error","re":5,"category":"vss.plugin.api","value":"busy",)"
    R"("message":"too many"})";

/// The header of a known message, by its op.
std::string_view base_text(std::string_view op)
{
    if (op == "hello")
        return hello_text;
    if (op == "hello_ack")
        return hello_ack_text;
    if (op == "status")
        return status_text;
    if (op == "log")
        return log_text;
    if (op == "shutdown")
        return shutdown_text;
    return error_text;
}

/// A header with the member set to the JSON text `value`, or removed when
/// `value` is empty.
std::string with_member(std::string_view op, std::string_view key,
                        std::string_view value)
{
    bj::object object = bj::parse(base_text(op)).as_object();
    if (value.empty())
        object.erase(key);
    else
        object[key] = bj::parse(value);
    return bj::serialize(object);
}

/// `{"op":"ping","<key>":<value>}`
std::string ping_with(std::string_view key, std::string_view value)
{
    std::string out = R"({"op":"ping",")";
    out += key;
    out += R"(":)";
    out += value;
    out += '}';
    return out;
}

/// `count` objects inside each other, each with one key `a`.
std::string nested_objects(std::size_t count)
{
    std::string out;
    for (std::size_t i = 0; i < count; ++i)
        out += R"({"a":)";
    out += '1';
    for (std::size_t i = 0; i < count; ++i)
        out += '}';
    return out;
}

/// A message of the op `op` whose header holds arrays `count` deep.
std::string deep_header(std::string_view op, std::size_t count)
{
    std::string out = R"({"op":")";
    out += op;
    out += R"(","x":)";
    out.append(count, '[');
    out += '1';
    out.append(count, ']');
    out += '}';
    return out;
}

/// A hello_ack whose "settings" is the JSON text `settings`.
std::string hello_ack_with_settings(std::string_view settings)
{
    std::string out =
        R"({"op":"hello_ack","re":1,"api":"1.0","granted":[],)"
        R"("capabilities":[],"data_directory":"","settings":)";
    out += settings;
    out += '}';
    return out;
}

/// `text` as a JSON string, for text that needs no escapes.
std::string quoted_json(std::string_view text)
{
    std::string out = "\"";
    out += text;
    out += '"';
    return out;
}

/// A JSON list of `count` strings `<prefix>0`, `<prefix>1` and so on.
std::string list_json(std::string_view prefix, std::size_t count)
{
    std::string out = "[";
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            out += ',';
        out += '"';
        out += prefix;
        out += std::to_string(i);
        out += '"';
    }
    out += ']';
    return out;
}

/// The hello_ack header with one limit set to the JSON text `value`.
std::string with_limit(std::string_view key, std::string_view value)
{
    bj::object object = bj::parse(hello_ack_text).as_object();
    object["limits"].as_object()[key] = bj::parse(value);
    return bj::serialize(object);
}

} // namespace

// -- the names ---------------------------------------------------------------

TEST_CASE("plugin protocol: what an op name may be")
{
    for (const std::string_view good :
         {"hello", "hello_ack", "generator.start", "a", "a1", "rules.set",
          "fixture.line.set", "x_y.z_1", "a.b.c.d", "plugin_unavailable"})
    {
        REQUIRE(is_op_name(good));
    }
    REQUIRE(is_op_name(repeated('a', max_op_length)));
    REQUIRE(!is_op_name(repeated('a', max_op_length + 1)));
    REQUIRE(is_op_name("a." + repeated('b', max_op_length - 2)));
    REQUIRE(!is_op_name("a." + repeated('b', max_op_length - 1)));

    for (const std::string_view bad :
         {""sv, "Hello"sv, "hELLO"sv, "1a"sv, "_a"sv, ".a"sv, "a."sv,
          "a..b"sv, "a b"sv, "a-b"sv, "a/b"sv, "a:b"sv, "a\n"sv, "a\"b"sv,
          "caf\xc3\xa9"sv, "\xff"sv, "a\0b"sv, "A"sv, "."sv, ".."sv})
    {
        REQUIRE(!is_op_name(bad));
    }
}

TEST_CASE("plugin protocol: errors have a name, a message and a category")
{
    const std::array names{
        std::pair{api_errc::success, "success"sv},
        std::pair{api_errc::unknown_op, "unknown_op"sv},
        std::pair{api_errc::bad_request, "bad_request"sv},
        std::pair{api_errc::refused, "refused"sv},
        std::pair{api_errc::not_granted, "not_granted"sv},
        std::pair{api_errc::not_declared, "not_declared"sv},
        std::pair{api_errc::timeout, "timeout"sv},
        std::pair{api_errc::busy, "busy"sv},
        std::pair{api_errc::unsupported, "unsupported"sv},
        std::pair{api_errc::plugin_unavailable, "plugin_unavailable"sv}};
    REQUIRE_EQ(std::string(api_category().name()),
               std::string("vss.plugin.api"));
    REQUIRE(api_category() != plugin_category());
    REQUIRE(api_category() != error_category());
    int expected_value = 0;
    for (const auto& [value, name] : names)
    {
        REQUIRE_EQ(to_string(value), name);
        REQUIRE_EQ(static_cast<int>(value), expected_value);
        ++expected_value;

        const error_code code = value;
        REQUIRE(code.category() == api_category());
        REQUIRE_EQ(code.value(), static_cast<int>(value));
        REQUIRE(code == value);
        REQUIRE(code == make_error_code(value));
        REQUIRE(!code.message().empty());
        REQUIRE(code.message().find("unknown") == std::string::npos);
        // Each name is a value of the "error" message.
        REQUIRE(is_op_name(name));
    }
    REQUIRE(!error_code{api_errc::success});
    REQUIRE(static_cast<bool>(error_code{api_errc::busy}));
    REQUIRE(error_code{api_errc::busy} != error_code{api_errc::timeout});
    REQUIRE(error_code{api_errc::busy} != plugin_errc::crashed);
}

TEST_CASE("plugin protocol: the levels have names")
{
    REQUIRE_EQ(to_string(status_level::ok), "ok"sv);
    REQUIRE_EQ(to_string(status_level::warning), "warning"sv);
    REQUIRE_EQ(to_string(status_level::error), "error"sv);
    REQUIRE_EQ(to_string(log_level::debug), "debug"sv);
    REQUIRE_EQ(to_string(log_level::info), "info"sv);
    REQUIRE_EQ(to_string(log_level::warning), "warning"sv);
    REQUIRE_EQ(to_string(log_level::error), "error"sv);
}

TEST_CASE("plugin protocol: each body has its op")
{
    REQUIRE_EQ(op_of(hello_message{}), "hello"sv);
    REQUIRE_EQ(op_of(hello_ack_message{}), "hello_ack"sv);
    REQUIRE_EQ(op_of(ping_message{}), "ping"sv);
    REQUIRE_EQ(op_of(pong_message{}), "pong"sv);
    REQUIRE_EQ(op_of(status_message{}), "status"sv);
    REQUIRE_EQ(op_of(log_message{}), "log"sv);
    REQUIRE_EQ(op_of(shutdown_message{}), "shutdown"sv);
    REQUIRE_EQ(op_of(bye_message{}), "bye"sv);
    REQUIRE_EQ(op_of(ok_message{}), "ok"sv);
    REQUIRE_EQ(op_of(error_message{}), "error"sv);
    REQUIRE_EQ(op_of(other_message{}), ""sv);
}

TEST_CASE("plugin protocol: the limits are the ones the header states")
{
    REQUIRE_EQ(plugin_api_version.major, 1);
    REQUIRE_EQ(plugin_api_version.minor, 0);
    REQUIRE_EQ(max_op_length, std::size_t{64});
    REQUIRE_EQ(max_message_text, std::size_t{1024});
    REQUIRE_EQ(max_status_text, std::size_t{200});
    REQUIRE_EQ(max_message_list, std::size_t{256});
    REQUIRE_EQ(max_header_depth, std::size_t{8});
    REQUIRE_EQ(max_message_id, std::uint64_t{9007199254740991});
    const api_limits defaults;
    REQUIRE_EQ(defaults.frames_per_second, 2000u);
    REQUIRE_EQ(defaults.frames_burst, 4000u);
    REQUIRE_EQ(defaults.bytes_per_second, 8388608u);
    REQUIRE_EQ(defaults.outstanding_requests, 32u);
    REQUIRE_EQ(defaults.log_lines_per_second, 100u);
    REQUIRE_EQ(defaults.instances, 16u);
}

// -- each message there and back ---------------------------------------------

TEST_CASE("plugin protocol: hello is written and read")
{
    check_wire(message_of(sample_hello(), 1), std::string(hello_text));

    // Without a pid the member is left out, and a pid of 0 is the same.
    hello_message body = sample_hello();
    body.pid = 0;
    check_wire(message_of(body, 2),
               R"({"op":"hello","id":2,"api":"1.0",)"
               R"("plugin_id":"com.example.tool","version":"1.2.3"})");

    // The largest pid and version, and the extremes of the api.
    body.pid = max_message_id;
    body.api = api_version{65535, 65535};
    body.version = repeated('v', 64);
    check_wire(message_of(body, max_message_id),
               R"({"op":"hello","id":9007199254740991,"api":"65535.65535",)"
               R"("plugin_id":"com.example.tool","version":")" +
                   repeated('v', 64) + R"(","pid":9007199254740991})");

    body.api = api_version{0, 0};
    body.pid = 1;
    body.version = "caf\xc3\xa9 \"1\" \\ 2";
    check_wire(message_of(body, 3),
               "{\"op\":\"hello\",\"id\":3,\"api\":\"0.0\","
               "\"plugin_id\":\"com.example.tool\","
               "\"version\":\"caf\xc3\xa9 \\\"1\\\" \\\\ 2\",\"pid\":1}");

    // The decoded message keeps the op and the fields.
    const api_message read = decode_ok(hello_text);
    REQUIRE_EQ(read.op, "hello");
    REQUIRE_EQ(read.id, std::uint64_t{1});
    REQUIRE_EQ(read.re, std::uint64_t{0});
    const auto* const hello = std::get_if<hello_message>(&read.body);
    REQUIRE(hello != nullptr);
    REQUIRE(*hello == sample_hello());
    REQUIRE(read.blob.empty());

    // The members may come in any order, with white space between.
    const api_message shuffled = decode_ok(
        R"( { "pid" : 4242, "version":"1.2.3", "id":1, "plugin_id":)"
        R"("com.example.tool", "api":"1.0", "op":"hello" } )");
    REQUIRE(shuffled == read);
}

TEST_CASE("plugin protocol: hello_ack is written and read")
{
    check_wire(message_of(sample_hello_ack(), 0, 1),
               std::string(hello_ack_text));

    // Nothing granted, nothing declared, nothing set.
    hello_ack_message empty;
    check_wire(message_of(empty, 0, 9),
               R"({"op":"hello_ack","re":9,"api":"0.0","granted":[],)"
               R"("capabilities":[],"limits":{"frames_per_second":2000,)"
               R"("frames_burst":4000,"bytes_per_second":8388608,)"
               R"("outstanding_requests":32,"log_lines_per_second":100,)"
               R"("instances":16},"data_directory":""})");

    // Every limit set, to the least and to the most.
    hello_ack_message limits;
    limits.api = api_version{1, 0};
    limits.limits.frames_per_second = 1;
    limits.limits.frames_burst = 4294967295u;
    limits.limits.bytes_per_second = 2;
    limits.limits.outstanding_requests = 4294967294u;
    limits.limits.log_lines_per_second = 3;
    limits.limits.instances = 4;
    check_wire(message_of(limits, 0, 2),
               R"({"op":"hello_ack","re":2,"api":"1.0","granted":[],)"
               R"("capabilities":[],"limits":{"frames_per_second":1,)"
               R"("frames_burst":4294967295,"bytes_per_second":2,)"
               R"("outstanding_requests":4294967294,)"
               R"("log_lines_per_second":3,"instances":4},)"
               R"("data_directory":""})");

    // All permissions, and a text with a non-ASCII character in it.
    hello_ack_message all;
    all.api = api_version{1, 3};
    for (const permission item :
         {permission::ports_read, permission::traffic_tap,
          permission::ports_write, permission::topology_modify,
          permission::modifier_inline, permission::faults,
          permission::serial_devices, permission::fs_read,
          permission::fs_write, permission::data_private,
          permission::net_listen, permission::net_outbound,
          permission::process_spawn, permission::bundle_section})
    {
        all.granted.push_back(item);
    }
    all.capabilities = {"a", "b_2", "c"};
    all.data_directory = "/home/caf\xc3\xa9";
    all.settings = R"({"list":[1,25,"x",null,true,{"k":-3}]})";
    check_wire(
        message_of(all, 0, 3),
        "{\"op\":\"hello_ack\",\"re\":3,\"api\":\"1.3\",\"granted\":"
        "[\"ports.read\",\"traffic.tap\",\"ports.write\","
        "\"topology.modify\",\"modifier.inline\",\"faults\","
        "\"serial.devices\",\"fs.read\",\"fs.write\",\"data.private\","
        "\"net.listen\",\"net.outbound\",\"process.spawn\","
        "\"bundle.section\"],\"capabilities\":[\"a\",\"b_2\",\"c\"],"
        "\"limits\":{\"frames_per_second\":2000,\"frames_burst\":4000,"
        "\"bytes_per_second\":8388608,\"outstanding_requests\":32,"
        "\"log_lines_per_second\":100,\"instances\":16},"
        "\"data_directory\":\"/home/caf\xc3\xa9\","
        "\"settings\":{\"list\":[1,25,\"x\",null,true,{\"k\":-3}]}}");

    // What is read.
    const api_message read = decode_ok(hello_ack_text);
    REQUIRE_EQ(read.op, "hello_ack");
    REQUIRE_EQ(read.id, std::uint64_t{0});
    REQUIRE_EQ(read.re, std::uint64_t{1});
    const auto* const ack = std::get_if<hello_ack_message>(&read.body);
    REQUIRE(ack != nullptr);
    REQUIRE(*ack == sample_hello_ack());
}

TEST_CASE("plugin protocol: limits and settings that are left out keep "
          "their defaults")
{
    const api_message read = decode_ok(
        R"({"op":"hello_ack","re":4,"api":"1.0","granted":[],)"
        R"("capabilities":[],"data_directory":""})");
    const auto* const ack = std::get_if<hello_ack_message>(&read.body);
    REQUIRE(ack != nullptr);
    REQUIRE(ack->limits == api_limits{});
    REQUIRE_EQ(ack->settings, std::string("{}"));

    // One limit given keeps the others at their defaults.
    const api_message some = decode_ok(
        R"({"op":"hello_ack","re":4,"api":"1.0","granted":[],)"
        R"("capabilities":[],"limits":{"instances":3,"future":9},)"
        R"("data_directory":"","settings":{}})");
    const auto* const some_ack = std::get_if<hello_ack_message>(&some.body);
    REQUIRE(some_ack != nullptr);
    api_limits expected;
    expected.instances = 3;
    REQUIRE(some_ack->limits == expected);
    REQUIRE_EQ(some_ack->settings, std::string("{}"));

    // An empty limits object is the same as none.
    const api_message none = decode_ok(
        R"({"op":"hello_ack","re":4,"api":"1.0","granted":[],)"
        R"("capabilities":[],"limits":{},"data_directory":""})");
    const auto* const none_ack = std::get_if<hello_ack_message>(&none.body);
    REQUIRE(none_ack != nullptr);
    REQUIRE(none_ack->limits == api_limits{});
}

TEST_CASE("plugin protocol: ping, pong, bye and ok are written and read")
{
    check_wire(message_of(ping_message{}, 7), R"({"op":"ping","id":7})");
    check_wire(message_of(pong_message{}, 0, 7), R"({"op":"pong","re":7})");
    check_wire(message_of(bye_message{}, 0, 8), R"({"op":"bye","re":8})");
    check_wire(message_of(ok_message{}, 0, 9), R"({"op":"ok","re":9})");

    // As notifications they have neither member.
    check_wire(message_of(ping_message{}), R"({"op":"ping"})");
    check_wire(message_of(pong_message{}), R"({"op":"pong"})");
    check_wire(message_of(bye_message{}), R"({"op":"bye"})");
    check_wire(message_of(ok_message{}), R"({"op":"ok"})");

    const api_message read = decode_ok(R"({"op":"pong","re":7})");
    REQUIRE_EQ(read.op, "pong");
    REQUIRE_EQ(read.id, std::uint64_t{0});
    REQUIRE_EQ(read.re, std::uint64_t{7});
    REQUIRE(std::holds_alternative<pong_message>(read.body));
}

TEST_CASE("plugin protocol: status is written and read")
{
    status_message body;
    body.level = status_level::warning;
    body.text = "slow";
    body.code = "slow.link";
    check_wire(message_of(body), std::string(status_text));

    // The text and the code are left out when they are empty.
    check_wire(message_of(status_message{}), R"({"op":"status","level":"ok"})");
    body.level = status_level::error;
    body.text.clear();
    check_wire(message_of(body, 4),
               R"({"op":"status","id":4,"level":"error","code":"slow.link"})");
    body.code.clear();
    body.text = "caf\xc3\xa9 \"x\" \\";
    check_wire(message_of(body),
               "{\"op\":\"status\",\"level\":\"error\","
               "\"text\":\"caf\xc3\xa9 \\\"x\\\" \\\\\"}");

    // The longest text, and a code of every kind of character.
    body.text = repeated('t', max_status_text);
    body.code = "a.b-c_d9" + repeated('z', 56);
    check_wire(message_of(body),
               R"({"op":"status","level":"error","text":")" +
                   repeated('t', max_status_text) +
                   R"(","code":"a.b-c_d9)" + repeated('z', 56) + R"("})");

    // Present but empty is the same as left out.
    const api_message empty =
        decode_ok(R"({"op":"status","level":"ok","text":"","code":""})");
    REQUIRE(empty.body == message_body{status_message{}});
}

TEST_CASE("plugin protocol: log is written and read")
{
    log_message body;
    body.text = "started";
    check_wire(message_of(body), std::string(log_text));

    // The text is always written, even when it is empty.
    body.level = log_level::debug;
    body.text.clear();
    check_wire(message_of(body), R"({"op":"log","level":"debug","text":""})");
    body.level = log_level::warning;
    body.text = repeated('l', max_message_text);
    check_wire(message_of(body),
               R"({"op":"log","level":"warning","text":")" +
                   repeated('l', max_message_text) + R"("})");
    body.level = log_level::error;
    body.text = "a/b \"c\" \\d \xe2\x82\xac";
    check_wire(message_of(body),
               "{\"op\":\"log\",\"level\":\"error\","
               "\"text\":\"a/b \\\"c\\\" \\\\d \xe2\x82\xac\"}");
}

TEST_CASE("plugin protocol: shutdown is written and read")
{
    shutdown_message body;
    body.reason = "quit";
    check_wire(message_of(body, 2), std::string(shutdown_text));
    body.reason.clear();
    check_wire(message_of(body, 2), R"({"op":"shutdown","id":2})");
    body.reason = repeated('r', 64);
    check_wire(message_of(body),
               R"({"op":"shutdown","reason":")" + repeated('r', 64) + R"("})");
    body.reason = "an update, soon";
    check_wire(message_of(body, 3),
               R"({"op":"shutdown","id":3,"reason":"an update, soon"})");
}

TEST_CASE("plugin protocol: error is written and read")
{
    error_message body;
    body.category = "vss.plugin.api";
    body.value = "busy";
    body.message = "too many";
    check_wire(message_of(body, 0, 5), std::string(error_text));

    body.message.clear();
    check_wire(message_of(body, 0, 5),
               R"({"op":"error","re":5,"category":"vss.plugin.api",)"
               R"("value":"busy"})");
    body.category = "vss";
    body.value = "not_found";
    body.message = repeated('m', max_message_text);
    check_wire(message_of(body, 0, 6),
               R"({"op":"error","re":6,"category":"vss","value":"not_found",)"
               R"("message":")" +
                   repeated('m', max_message_text) + R"("})");
    body.category = repeated('c', 63) + ".";
    body.value = repeated('v', 64);
    body.message.clear();
    check_wire(message_of(body),
               R"({"op":"error","category":")" + repeated('c', 63) +
                   R"(.","value":")" + repeated('v', 64) + R"("})");
}

TEST_CASE("plugin protocol: a message of this file keeps its blob")
{
    const api_message read =
        decode_ok(R"({"op":"ping","id":1})", sample_blob());
    REQUIRE(read.blob == sample_blob());

    api_message message = message_of(ping_message{}, 1);
    message.blob = sample_blob();
    const auto encoded = encode_message(message);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().blob == sample_blob());
    REQUIRE_EQ(encoded.value().header, std::string(R"({"op":"ping","id":1})"));

    // A blob that is large is copied whole.
    byte_buffer large(std::size_t{200000}, 0x5a);
    large.front() = 1;
    large.back() = 2;
    const api_message big = decode_ok(R"({"op":"ok","re":1})", large);
    REQUIRE(big.blob == large);
}

TEST_CASE("plugin protocol: an empty op is filled in when writing")
{
    api_message message = message_of(ping_message{}, 1);
    REQUIRE(message.op.empty());
    REQUIRE_EQ(header_of(message), std::string(R"({"op":"ping","id":1})"));
    message.op = "ping";
    REQUIRE_EQ(header_of(message), std::string(R"({"op":"ping","id":1})"));
    // A message read keeps its op and writes the same.
    const api_message read = decode_ok(R"({"op":"ping","id":1})");
    REQUIRE_EQ(read.op, "ping");
    REQUIRE_EQ(header_of(read), std::string(R"({"op":"ping","id":1})"));
}

// -- id and re ---------------------------------------------------------------

TEST_CASE("plugin protocol: id and re of a request and of an answer")
{
    for (const std::string_view key : {"id"sv, "re"sv})
    {
        const std::uint64_t zero = 0;
        for (const std::uint64_t number :
             {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{4294967296},
              std::uint64_t{9007199254740990}, max_message_id})
        {
            const api_message read =
                decode_ok(ping_with(key, std::to_string(number)));
            REQUIRE_EQ(read.op, "ping");
            REQUIRE_EQ(key == "id" ? read.id : read.re, number);
            REQUIRE_EQ(key == "id" ? read.re : read.id, zero);
            REQUIRE(std::holds_alternative<ping_message>(read.body));
        }
    }
    // Neither: a notification.
    const api_message none =
        decode_ok(R"({"op":"log","level":"info","text":""})");
    REQUIRE_EQ(none.id, std::uint64_t{0});
    REQUIRE_EQ(none.re, std::uint64_t{0});
    // An unknown op carries them the same way.
    const api_message other = decode_ok(R"({"op":"x.y","re":12})");
    REQUIRE_EQ(other.id, std::uint64_t{0});
    REQUIRE_EQ(other.re, std::uint64_t{12});
}

TEST_CASE("plugin protocol: an id or a re that is not a number from 1 to "
          "2^53-1 is refused")
{
    struct member
    {
        std::string_view key;
        std::string_view fragment;
    };
    for (const member& which :
         {member{"id"sv,
                 "id: must be a whole number from 1 to 9007199254740991"sv},
          member{"re"sv,
                 "re: must be a whole number from 1 to 9007199254740991"sv}})
    {
        for (const std::string_view value :
             {"0"sv, "-1"sv, "-0"sv, "9007199254740992"sv, "9007199254740993"sv,
              "18446744073709551615"sv, "18446744073709551616"sv,
              "99999999999999999999999"sv, "1.5"sv, "1.0"sv, "1e2"sv,
              "0.5"sv, "\"1\""sv, "\"\""sv, "null"sv, "true"sv, "false"sv,
              "[1]"sv, "{}"sv})
        {
            require_refused(ping_with(which.key, value), which.fragment);
        }
    }
}

TEST_CASE("plugin protocol: id and re together are refused")
{
    require_refused(R"({"op":"ping","id":1,"re":1})", "not both");
    require_refused(R"({"op":"ping","re":2,"id":3})", "not both");
    require_refused(R"({"op":"x.y","id":2,"re":3})", "not both");
    require_refused(
        R"({"op":"ok","id":9007199254740991,"re":9007199254740991})",
        "not both");
    // The first member that is wrong is the one named.
    require_refused(R"({"op":"ping","id":0,"re":1})", "id: must be");
    require_refused(R"({"op":"ping","id":1,"re":0})", "re: must be");
}

// -- members this file does not know ------------------------------------------

TEST_CASE("plugin protocol: members a message does not know are ignored")
{
    const std::string extra =
        R"(,"zz_future":{"a":[1,{"b":null}]},"zz_more":"text","zz_n":1.5)";
    for (const std::string_view op :
         {"hello"sv, "hello_ack"sv, "status"sv, "log"sv, "shutdown"sv,
          "error"sv})
    {
        const std::string_view base = base_text(op);
        std::string widened(base.substr(0, base.size() - 1));
        widened += extra;
        widened += '}';
        const api_message plain = decode_ok(base);
        const api_message wide = decode_ok(widened);
        REQUIRE(plain == wide);
        // Writing leaves the extra members out.
        REQUIRE_EQ(header_of(wide), std::string(base));
    }
    for (const std::string_view header :
         {R"({"op":"ping","id":1,"level":5,"x":[]})"sv,
          R"({"op":"pong","re":1,"text":{}})"sv,
          R"({"op":"bye","re":1,"api":"junk"})"sv,
          R"({"op":"ok","re":1,"granted":null})"sv})
    {
        const api_message read = decode_ok(header);
        REQUIRE(!std::holds_alternative<other_message>(read.body));
    }
    // A limit this version does not know, inside "limits".
    const api_message read = decode_ok(
        R"({"op":"hello_ack","re":1,"api":"1.0","granted":[],)"
        R"("capabilities":[],"limits":{"frames_per_second":5,)"
        R"("future_limit":"x"},"data_directory":""})");
    const auto* const ack = std::get_if<hello_ack_message>(&read.body);
    REQUIRE(ack != nullptr);
    REQUIRE_EQ(ack->limits.frames_per_second, 5u);
}

// -- other messages ----------------------------------------------------------

TEST_CASE("plugin protocol: an op this file does not know is passed on with "
          "its header")
{
    const std::string header =
        R"( { "op" : "generator.start", "id": 3, "instance": 1,)"
        R"( "extra": [1, 2], "text": 5 } )";
    const api_message read = decode_ok(header, sample_blob());
    REQUIRE_EQ(read.op, "generator.start");
    REQUIRE_EQ(read.id, std::uint64_t{3});
    REQUIRE_EQ(read.re, std::uint64_t{0});
    REQUIRE(read.blob == sample_blob());
    const auto* const other = std::get_if<other_message>(&read.body);
    REQUIRE(other != nullptr);
    REQUIRE_EQ(other->header, header);

    // Written again it is the same bytes.
    const auto encoded = encode_message(read);
    REQUIRE(encoded.has_value());
    REQUIRE_EQ(encoded.value().header, header);
    REQUIRE(encoded.value().blob == sample_blob());

    // A name that begins like one this file knows is another name.
    for (const std::string_view op :
         {"ping2"sv, "hello.x"sv, "okay"sv, "error.fatal"sv, "log_line"sv,
          "status.set"sv, "shutdown_now"sv, "pong_"sv})
    {
        std::string text = R"({"op":")";
        text += op;
        text += R"(","re":4,"junk":[1,"two"]})";
        const api_message other_read = decode_ok(text);
        REQUIRE_EQ(other_read.op, std::string(op));
        REQUIRE_EQ(other_read.re, std::uint64_t{4});
        REQUIRE(std::holds_alternative<other_message>(other_read.body));
        const auto written = encode_message(other_read);
        REQUIRE(written.has_value());
        REQUIRE_EQ(written.value().header, text);
    }

    // A member the extension point owns is not looked at, so it may hold
    // what this file would refuse in a known message.
    const api_message free_form = decode_ok(
        R"({"op":"rules.set","id":5,"level":null,"text":"\u0001",)"
        R"("granted":"x"})");
    REQUIRE(std::holds_alternative<other_message>(free_form.body));
}

TEST_CASE("plugin protocol: an unknown op is still held to the header rules")
{
    require_refused(R"({"op":"x.y","id":0})", "id: must be");
    require_refused(R"({"op":"x.y","id":1,"id":1})", "given twice");
    require_refused(R"({"op":"x.y","id":1,"re":1})", "not both");
    require_refused(R"({"op":"x.Y"})", "op: must be a message name");
    require_refused(deep_header("x.y", 8), "is not JSON");
    require_refused(R"({"op":"x.y")", "is not JSON");
    require_refused(R"({"op":"x.y"} trailing)", "after the JSON document");
}

TEST_CASE("plugin protocol: messages go through the frame reader and back")
{
    std::vector<api_message> sent;
    sent.push_back(message_of(sample_hello(), 1));
    sent.push_back(message_of(sample_hello_ack(), 0, 1));
    sent.push_back(message_of(ping_message{}, 2));
    sent.push_back(message_of(pong_message{}, 0, 2));
    api_message with_blob = message_of(ok_message{}, 0, 3);
    with_blob.blob = sample_blob();
    sent.push_back(with_blob);
    api_message other;
    other.op = "generator.write";
    other.id = 9;
    other.body = other_message{R"({"op":"generator.write","id":9,"n":1})"};
    other.blob = byte_buffer(300, 0x7f);
    sent.push_back(other);

    byte_buffer wire;
    for (const api_message& message : sent)
    {
        const auto frame = encode_message(message);
        REQUIRE(frame.has_value());
        const auto bytes = agent::encode_frame(frame.value());
        REQUIRE(bytes.has_value());
        wire.insert(wire.end(), bytes.value().begin(), bytes.value().end());
    }

    agent::frame_reader reader;
    reader.feed(wire);
    for (api_message message : sent)
    {
        auto next = reader.next();
        REQUIRE(next.has_value());
        std::optional<agent::frame> ready = std::move(next).value();
        REQUIRE(ready.has_value());
        const auto decoded = decode_message(ready.value());
        REQUIRE(decoded.has_value());
        if (message.op.empty())
            message.op = std::string(op_of(message.body));
        REQUIRE(decoded.value() == message);
    }
    REQUIRE_EQ(reader.buffered(), std::size_t{0});
}

// -- headers that are refused ------------------------------------------------

TEST_CASE("plugin protocol: a header that is not one JSON object is refused")
{
    struct bad_header
    {
        std::string_view header;
        std::string_view fragment;
    };
    constexpr auto not_json = "header: is not JSON"sv;
    constexpr auto not_object = "header: must be a JSON object"sv;
    constexpr auto twice = "has a key given twice in one object"sv;
    constexpr auto after = "has text after the JSON document"sv;
    constexpr auto not_name = "op: must be a message name"sv;
    const std::array cases{
        bad_header{""sv, not_json},
        bad_header{"not json"sv, not_json},
        bad_header{"{"sv, not_json},
        bad_header{R"({"op":"ping")"sv, not_json},
        bad_header{R"({"op":"ping",})"sv, not_json},
        bad_header{R"({'op':'ping'})"sv, not_json},
        bad_header{R"({"op":"ping" "id":1})"sv, not_json},
        bad_header{"{\"op\":\"ping\",\"a\":\"\xff\"}"sv, not_json},
        bad_header{"{\"op\":\"ping\",\"a\":\"\xc3\"}"sv, not_json},
        bad_header{R"({"op":"ping","a":"\ud800"})"sv, not_json},
        bad_header{R"({"op":"ping"} x)"sv, after},
        bad_header{R"({"op":"ping"}{"op":"ping"})"sv, after},
        bad_header{R"([])"sv, not_object},
        bad_header{R"([{"op":"ping"}])"sv, not_object},
        bad_header{R"("ping")"sv, not_object},
        bad_header{"5"sv, not_object},
        bad_header{"null"sv, not_object},
        bad_header{"true"sv, not_object},
        bad_header{R"({"op":"ping","op":"pong"})"sv, twice},
        bad_header{R"({"op":"ping","id":1,"id":1})"sv, twice},
        bad_header{R"({"op":"ping","a":{"b":1,"b":2}})"sv, twice},
        bad_header{R"({"op":"ping","a":[{"b":1,"b":2}]})"sv, twice},
        bad_header{R"({})"sv, "op: is required"sv},
        bad_header{R"({"id":1})"sv, "op: is required"sv},
        bad_header{R"({"OP":"ping"})"sv, "op: is required"sv},
        bad_header{R"({"op":null})"sv, "op: must be a string"sv},
        bad_header{R"({"op":5})"sv, "op: must be a string"sv},
        bad_header{R"({"op":["ping"]})"sv, "op: must be a string"sv},
        bad_header{R"({"op":{}})"sv, "op: must be a string"sv},
        bad_header{R"({"op":""})"sv, not_name},
        bad_header{R"({"op":"Ping"})"sv, not_name},
        bad_header{R"({"op":"1ping"})"sv, not_name},
        bad_header{R"({"op":"_ping"})"sv, not_name},
        bad_header{R"({"op":".ping"})"sv, not_name},
        bad_header{R"({"op":"ping."})"sv, not_name},
        bad_header{R"({"op":"a..b"})"sv, not_name},
        bad_header{R"({"op":"a b"})"sv, not_name},
        bad_header{R"({"op":"a-b"})"sv, not_name},
        bad_header{R"({"op":"ping\n"})"sv, not_name},
        bad_header{R"({"op":"caf\u00e9"})"sv, not_name},
        bad_header{R"({"op":"ping\u0000"})"sv, not_name}};
    for (const bad_header& item : cases)
        require_refused(item.header, item.fragment);

    // The longest op that is a name, and one byte more.
    std::string longest = R"({"op":")";
    longest += repeated('a', 64);
    longest += R"("})";
    REQUIRE(std::holds_alternative<other_message>(decode_ok(longest).body));
    std::string too_long = R"({"op":")";
    too_long += repeated('a', 65);
    too_long += R"("})";
    require_refused(too_long, not_name);
}

TEST_CASE("plugin protocol: white space around the document is allowed")
{
    REQUIRE(std::holds_alternative<ping_message>(
        decode_ok("{\"op\":\"ping\"} \r\n\t ").body));
    REQUIRE(std::holds_alternative<ping_message>(
        decode_ok(" \n{ \"op\" : \"ping\" }").body));
}

TEST_CASE("plugin protocol: the header may nest 8 deep and no deeper")
{
    REQUIRE(std::holds_alternative<ping_message>(
        decode_ok(deep_header("ping", 7)).body));
    require_refused(deep_header("ping", 8), "header: is not JSON");
    require_refused(deep_header("ping", 100), "header: is not JSON");
    // The same for an op that is not known, and for objects.
    REQUIRE(std::holds_alternative<other_message>(
        decode_ok(deep_header("x.y", 7)).body));
    require_refused(deep_header("x.y", 8), "header: is not JSON");

    std::string objects = R"({"op":"ping","x":)";
    objects += nested_objects(7);
    objects += '}';
    REQUIRE(std::holds_alternative<ping_message>(decode_ok(objects).body));
    std::string too_deep = R"({"op":"ping","x":)";
    too_deep += nested_objects(8);
    too_deep += '}';
    require_refused(too_deep, "header: is not JSON");
}

// -- members of known messages -----------------------------------------------

TEST_CASE("plugin protocol: a member that is missing or of the wrong type is "
          "refused")
{
    struct member_case
    {
        std::string_view op;
        std::string_view key;
        std::string_view wrong;
        std::string_view fragment;
        bool required = false;
    };
    constexpr auto text_type = "must be a string"sv;
    constexpr auto whole =
        "must be a whole number from 0 to 9007199254740991"sv;
    const std::array cases{
        member_case{"hello"sv, "api"sv, "5"sv, text_type, true},
        member_case{"hello"sv, "plugin_id"sv, "5"sv, text_type, true},
        member_case{"hello"sv, "version"sv, "5"sv, text_type, true},
        member_case{"hello"sv, "pid"sv, "\"4\""sv, whole, false},
        member_case{"hello_ack"sv, "api"sv, "5"sv, text_type, true},
        member_case{"hello_ack"sv, "granted"sv, "\"x\""sv, "must be a list"sv,
                    true},
        member_case{"hello_ack"sv, "capabilities"sv, "{}"sv,
                    "must be a list"sv, true},
        member_case{"hello_ack"sv, "limits"sv, "5"sv, "must be an object"sv,
                    false},
        member_case{"hello_ack"sv, "data_directory"sv, "5"sv, text_type, true},
        member_case{"hello_ack"sv, "settings"sv, "[]"sv,
                    "must be a JSON object"sv, false},
        member_case{"status"sv, "level"sv, "5"sv, text_type, true},
        member_case{"status"sv, "text"sv, "5"sv, text_type, false},
        member_case{"status"sv, "code"sv, "5"sv, text_type, false},
        member_case{"log"sv, "level"sv, "5"sv, text_type, true},
        member_case{"log"sv, "text"sv, "5"sv, text_type, true},
        member_case{"shutdown"sv, "reason"sv, "5"sv, text_type, false},
        member_case{"error"sv, "category"sv, "5"sv, text_type, true},
        member_case{"error"sv, "value"sv, "5"sv, text_type, true},
        member_case{"error"sv, "message"sv, "5"sv, text_type, false}};
    for (const member_case& item : cases)
    {
        std::string place(item.op);
        place += '.';
        place += item.key;
        place += ": ";

        const std::string missing = with_member(item.op, item.key, {});
        if (item.required)
        {
            std::string fragment = place;
            fragment += "is required";
            require_refused(missing, fragment);
        }
        else
        {
            static_cast<void>(decode_ok(missing));
        }

        std::string fragment = place;
        fragment += item.fragment;
        require_refused(with_member(item.op, item.key, item.wrong), fragment);
        // null is not the same as absent.
        require_refused(with_member(item.op, item.key, "null"), place);
    }
}

TEST_CASE("plugin protocol: each text has the limit the header gives it")
{
    struct limit_case
    {
        std::string_view op;
        std::string_view key;
        std::size_t limit = 0;
        std::string_view fragment;
    };
    const std::array cases{
        limit_case{"hello"sv, "version"sv, 64, "must be 1 to 64 bytes"sv},
        limit_case{"hello_ack"sv, "data_directory"sv, 1024,
                   "is longer than 1024 bytes"sv},
        limit_case{"status"sv, "text"sv, 200, "is longer than 200 bytes"sv},
        limit_case{"status"sv, "code"sv, 64, "must be at most 64"sv},
        limit_case{"log"sv, "text"sv, 1024, "is longer than 1024 bytes"sv},
        limit_case{"shutdown"sv, "reason"sv, 64, "is longer than 64 bytes"sv},
        limit_case{"error"sv, "category"sv, 64, "must be 1 to 64"sv},
        limit_case{"error"sv, "value"sv, 64, "must be 1 to 64"sv},
        limit_case{"error"sv, "message"sv, 1024,
                   "is longer than 1024 bytes"sv}};
    for (const limit_case& item : cases)
    {
        const std::string at_limit = quoted_json(repeated('a', item.limit));
        const std::string over = quoted_json(repeated('a', item.limit + 1));
        static_cast<void>(decode_ok(with_member(item.op, item.key, at_limit)));
        std::string fragment(item.op);
        fragment += '.';
        fragment += item.key;
        fragment += ": ";
        fragment += item.fragment;
        require_refused(with_member(item.op, item.key, over), fragment);
    }

    // The limit counts bytes, not characters: 100 two-byte characters fill
    // a status text, and one more byte is over it.
    std::string two_byte = "\"";
    for (int i = 0; i < 100; ++i)
        two_byte += R"(\u00e9)";
    std::string over_by_one = two_byte;
    over_by_one += "a\"";
    two_byte += '"';
    static_cast<void>(decode_ok(with_member("status", "text", two_byte)));
    require_refused(with_member("status", "text", over_by_one),
                    "status.text: is longer than 200 bytes");
}

TEST_CASE("plugin protocol: a text that may be empty may be empty")
{
    for (const std::string_view key : {"text"sv, "code"sv})
        static_cast<void>(decode_ok(with_member("status", key, R"("")")));
    static_cast<void>(decode_ok(with_member("log", "text", R"("")")));
    static_cast<void>(decode_ok(with_member("shutdown", "reason", R"("")")));
    static_cast<void>(decode_ok(with_member("error", "message", R"("")")));
    static_cast<void>(
        decode_ok(with_member("hello_ack", "data_directory", R"("")")));
    // These are not texts: they hold one byte at least.
    require_refused(with_member("hello", "version", R"("")"),
                    "hello.version: must be 1 to 64 bytes");
    require_refused(with_member("error", "category", R"("")"),
                    "error.category: must be 1 to 64");
    require_refused(with_member("error", "value", R"("")"),
                    "error.value: must be 1 to 64");
}

TEST_CASE("plugin protocol: a text with a control character or a direction "
          "mark is refused")
{
    struct place
    {
        std::string_view op;
        std::string_view key;
    };
    const std::array places{place{"hello"sv, "version"sv},
                            place{"hello_ack"sv, "data_directory"sv},
                            place{"status"sv, "text"sv},
                            place{"log"sv, "text"sv},
                            place{"shutdown"sv, "reason"sv},
                            place{"error"sv, "message"sv}};
    // JSON escapes, so that this file stays ASCII.
    const std::array bad{
        R"("a\u0001b")"sv, R"("a\nb")"sv,     R"("a\tb")"sv,
        R"("a\rb")"sv,     R"("\u007f")"sv,   R"("\u0085")"sv,
        R"("\u009f")"sv,   R"("\u061c")"sv,   R"("\u200e")"sv,
        R"("\u200f")"sv,   R"("\u2028")"sv,   R"("\u2029")"sv,
        R"("\u202a")"sv,   R"("\u202b")"sv,   R"("\u202c")"sv,
        R"("\u202d")"sv,   R"("\u202e")"sv,   R"("\u2066")"sv,
        R"("\u2067")"sv,   R"("\u2068")"sv,   R"("\u2069")"sv,
        R"("\ufeff")"sv,   R"("a\u0000b")"sv, R"("\u001f")"sv};
    const std::array good{R"("caf\u00e9")"sv,    R"("\u2603")"sv,
                          R"("\ud83d\ude00")"sv, R"("\u00a0")"sv,
                          R"("\u200b")"sv,       R"("a b")"sv,
                          R"("\u2065")"sv};
    for (const place& item : places)
    {
        std::string fragment(item.op);
        fragment += '.';
        fragment += item.key;
        fragment += ": must be plain text";
        for (const std::string_view value : bad)
            require_refused(with_member(item.op, item.key, value), fragment);
        for (const std::string_view value : good)
            static_cast<void>(decode_ok(with_member(item.op, item.key, value)));
    }
}

TEST_CASE("plugin protocol: the version of the api")
{
    for (const std::string_view op : {"hello"sv, "hello_ack"sv})
    {
        std::string place(op);
        place += ".api: ";
        std::string not_version = place;
        not_version += "must be a version such as 1.0";
        for (const std::string_view bad :
             {R"("")"sv,       R"("1")"sv,        R"("1.")"sv,
              R"(".1")"sv,     R"("1.x")"sv,      R"("x.1")"sv,
              R"("01.0")"sv,   R"("1.00")"sv,     R"("1.01")"sv,
              R"("00.0")"sv,   R"("1.0.0")"sv,    R"("70000.0")"sv,
              R"("1.65536")"sv, R"("65536.0")"sv, R"("-1.0")"sv,
              R"("+1.0")"sv,   R"("1.-1")"sv,     R"(" 1.0")"sv,
              R"("1.0 ")"sv,   R"("1,0")"sv,      R"("1..0")"sv,
              R"("1.0\n")"sv,  R"("\u0661.\u0660")"sv, R"("123456.0")"sv,
              R"("1e1.0")"sv,  R"("1_0")"sv,      R"("v1.0")"sv,
              R"("1.0-rc1")"sv, R"("0x1.0")"sv})
        {
            require_refused(with_member(op, "api", bad), not_version);
        }
        std::string not_text = place;
        not_text += "must be a string";
        require_refused(with_member(op, "api", "1.0"), not_text);
        for (const std::string_view good :
             {R"("0.0")"sv, R"("1.0")"sv, R"("1.10")"sv, R"("10.20")"sv,
              R"("65535.65535")"sv, R"("0.65535")"sv, R"("65535.0")"sv,
              R"("9.9")"sv})
        {
            static_cast<void>(decode_ok(with_member(op, "api", good)));
        }
    }
    const api_message read =
        decode_ok(with_member("hello", "api", R"("65535.65535")"));
    const auto* const hello = std::get_if<hello_message>(&read.body);
    REQUIRE(hello != nullptr);
    REQUIRE_EQ(hello->api.major, 65535);
    REQUIRE_EQ(hello->api.minor, 65535);
}

TEST_CASE("plugin protocol: the plugin id, the pid and the version of a hello")
{
    for (const std::string_view bad :
         {R"("")"sv, R"("ab")"sv, R"("nodots")"sv, R"("Com.Example")"sv,
          R"("com..example")"sv, R"("com.example.")"sv, R"(".com.example")"sv,
          R"("com.exa mple")"sv, R"("com.example-")"sv, R"("com.exam_ple")"sv,
          R"("com.caf\u00e9")"sv})
    {
        require_refused(with_member("hello", "plugin_id", bad),
                        "hello.plugin_id: must be a plugin id");
    }
    for (const std::string_view good :
         {R"("a.b")"sv, R"("com.example-tool")"sv, R"("a-b.c-d")"sv})
    {
        static_cast<void>(decode_ok(with_member("hello", "plugin_id", good)));
    }
    require_refused(
        with_member("hello", "plugin_id",
                    quoted_json(repeated('a', max_id_length) + ".b")),
        "hello.plugin_id: must be a plugin id");
    static_cast<void>(decode_ok(
        with_member("hello", "plugin_id",
                    quoted_json(repeated('a', max_id_length - 2) + ".b"))));

    for (const std::string_view bad :
         {"-1"sv, "1.5"sv, "1.0"sv, "1e3"sv, R"("1")"sv, "null"sv, "true"sv,
          "[]"sv, "9007199254740992"sv, "18446744073709551615"sv,
          "18446744073709551616"sv})
    {
        require_refused(with_member("hello", "pid", bad),
                        "hello.pid: must be a whole number from 0 to "
                        "9007199254740991");
    }
    for (const std::string_view good :
         {"0"sv, "1"sv, "4294967296"sv, "9007199254740991"sv})
    {
        static_cast<void>(decode_ok(with_member("hello", "pid", good)));
    }
    // A pid that is left out is 0.
    const api_message read = decode_ok(with_member("hello", "pid", {}));
    const auto* const hello = std::get_if<hello_message>(&read.body);
    REQUIRE(hello != nullptr);
    REQUIRE_EQ(hello->pid, std::uint64_t{0});
}

TEST_CASE("plugin protocol: the levels of a status and of a log")
{
    for (const std::string_view bad :
         {R"("info")"sv, R"("OK")"sv, R"("Ok")"sv, R"("")"sv, R"(" ok")"sv,
          R"("ok ")"sv, R"("fatal")"sv, R"("debug")"sv})
    {
        require_refused(with_member("status", "level", bad),
                        "status.level: must be ok, warning or error");
    }
    for (const std::string_view bad :
         {R"("warn")"sv, R"("fatal")"sv, R"("INFO")"sv, R"("")"sv, R"("ok")"sv,
          R"("Error")"sv, R"("info ")"sv})
    {
        require_refused(with_member("log", "level", bad),
                        "log.level: must be debug, info, warning or error");
    }
    for (const std::string_view good :
         {R"("ok")"sv, R"("warning")"sv, R"("error")"sv})
    {
        static_cast<void>(decode_ok(with_member("status", "level", good)));
    }
    for (const std::string_view good :
         {R"("debug")"sv, R"("info")"sv, R"("warning")"sv, R"("error")"sv})
    {
        static_cast<void>(decode_ok(with_member("log", "level", good)));
    }
    const api_message read =
        decode_ok(with_member("status", "level", R"("error")"));
    const auto* const status = std::get_if<status_message>(&read.body);
    REQUIRE(status != nullptr);
    REQUIRE(status->level == status_level::error);
}

TEST_CASE("plugin protocol: the code of a status and the names of an error")
{
    for (const std::string_view bad :
         {R"("Upper")"sv, R"("has space")"sv, R"("x/y")"sv, R"("a:b")"sv,
          R"("caf\u00e9")"sv, R"("a\nb")"sv, R"("a,b")"sv, R"("a+b")"sv})
    {
        require_refused(with_member("status", "code", bad),
                        "status.code: must be at most 64 lower-case letters");
    }
    for (const std::string_view good :
         {R"("a")"sv, R"("slow.link")"sv, R"("a-b_c.d")"sv, R"("1")"sv,
          R"("x-1.y_z")"sv})
    {
        static_cast<void>(decode_ok(with_member("status", "code", good)));
    }

    for (const std::string_view bad :
         {R"("Vss")"sv, R"("a-b")"sv, R"("a b")"sv, R"("a/b")"sv,
          R"("caf\u00e9")"sv, R"("a:b")"sv})
    {
        require_refused(with_member("error", "category", bad),
                        "error.category: must be 1 to 64 lower-case letters");
    }
    for (const std::string_view good :
         {R"("vss")"sv, R"("vss.plugin.api")"sv, R"("a_b.c1")"sv})
    {
        static_cast<void>(decode_ok(with_member("error", "category", good)));
    }
    for (const std::string_view bad :
         {R"("a.b")"sv, R"("A")"sv, R"("a-b")"sv, R"("a b")"sv,
          R"("caf\u00e9")"sv})
    {
        require_refused(with_member("error", "value", bad),
                        "error.value: must be 1 to 64 lower-case letters");
    }
    for (const std::string_view good :
         {R"("unknown_op")"sv, R"("x1")"sv, R"("_")"sv})
    {
        static_cast<void>(decode_ok(with_member("error", "value", good)));
    }
}

TEST_CASE("plugin protocol: the lists of a hello_ack")
{
    // Capabilities: 256 different names are allowed, 257 are not.
    static_cast<void>(decode_ok(
        with_member("hello_ack", "capabilities", list_json("c", 256))));
    require_refused(
        with_member("hello_ack", "capabilities", list_json("c", 257)),
        "hello_ack.capabilities: has more than 256 entries");
    require_refused(
        with_member("hello_ack", "capabilities", R"(["a","b","a"])"),
        "hello_ack.capabilities: holds a name twice");
    for (const std::string_view bad :
         {R"([""])"sv, R"(["Engine"])"sv, R"(["a-b"])"sv, R"(["a.b"])"sv,
          R"(["a b"])"sv, R"([5])"sv, R"([null])"sv, R"([["a"]])"sv,
          R"(["caf\u00e9"])"sv, R"(["a","B"])"sv})
    {
        require_refused(with_member("hello_ack", "capabilities", bad),
                        "hello_ack.capabilities: holds something that is not "
                        "1 to 64 lower-case letters, digits and underscores");
    }
    std::string longest = "[";
    longest += quoted_json(repeated('c', 64));
    longest += ']';
    std::string too_long = "[";
    too_long += quoted_json(repeated('c', 65));
    too_long += ']';
    static_cast<void>(
        decode_ok(with_member("hello_ack", "capabilities", longest)));
    require_refused(with_member("hello_ack", "capabilities", too_long),
                    "hello_ack.capabilities: holds something");
    for (const std::string_view good :
         {R"(["9x"])"sv, R"(["_"])"sv, R"(["a_b_2"])"sv, R"([])"sv})
    {
        static_cast<void>(
            decode_ok(with_member("hello_ack", "capabilities", good)));
    }

    // Permissions: the list is cut at 256 before the entries are looked at.
    require_refused(
        with_member("hello_ack", "granted", list_json("ports.read", 257)),
        "hello_ack.granted: has more than 256 entries");
    std::string many = "[";
    for (int i = 0; i < 256; ++i)
    {
        if (i != 0)
            many += ',';
        many += R"("ports.read")";
    }
    many += ']';
    require_refused(with_member("hello_ack", "granted", many),
                    "hello_ack.granted: holds a permission twice");
    require_refused(
        with_member("hello_ack", "granted",
                    R"(["ports.read","traffic.tap","ports.read"])"),
        "hello_ack.granted: holds a permission twice");
    for (const std::string_view bad :
         {R"(["ports.sideways"])"sv, R"(["Ports.Read"])"sv,
          R"(["ports_read"])"sv, R"([""])"sv, R"([5])"sv, R"([null])"sv,
          R"([["ports.read"]])"sv, R"(["ports.read","x"])"sv,
          R"(["ports.read "])"sv})
    {
        require_refused(with_member("hello_ack", "granted", bad),
                        "hello_ack.granted: holds something that is not a "
                        "permission this version knows");
    }
    static_cast<void>(decode_ok(with_member("hello_ack", "granted", "[]")));
    static_cast<void>(decode_ok(
        with_member("hello_ack", "granted", R"(["faults","fs.read"])")));
}

TEST_CASE("plugin protocol: the limits object of a hello_ack")
{
    for (const std::string_view key :
         {"frames_per_second"sv, "frames_burst"sv, "bytes_per_second"sv,
          "outstanding_requests"sv, "log_lines_per_second"sv, "instances"sv})
    {
        std::string place = "hello_ack.limits.";
        place += key;
        place += ": must be a whole number from 1 to 4294967295";
        for (const std::string_view bad :
             {"0"sv, "-1"sv, "4294967296"sv, "1.5"sv, "1.0"sv, R"("5")"sv,
              "null"sv, "true"sv, "[]"sv, "18446744073709551616"sv,
              "-4294967295"sv})
        {
            require_refused(with_limit(key, bad), place);
        }
        for (const std::string_view good :
             {"1"sv, "4294967295"sv, "2147483648"sv})
        {
            static_cast<void>(decode_ok(with_limit(key, good)));
        }
    }
    for (const std::string_view bad :
         {"[]"sv, R"("x")"sv, "5"sv, "null"sv, "true"sv})
    {
        require_refused(with_member("hello_ack", "limits", bad),
                        "hello_ack.limits: must be an object");
    }
}

// -- settings ----------------------------------------------------------------

TEST_CASE("plugin protocol: settings are read as one compact JSON object")
{
    const api_message read = decode_ok(hello_ack_with_settings(
        R"( { "b" : [ 1 , 2 , { "c" : null } ] , "a" : 15 , )"
        R"("s":"x\"y\u00e9" } )"));
    const auto* const ack = std::get_if<hello_ack_message>(&read.body);
    REQUIRE(ack != nullptr);
    std::string expected = R"({"b":[1,2,{"c":null}],"a":15,"s":"x\"y)";
    expected += "\xc3\xa9";
    expected += R"("})";
    REQUIRE_EQ(ack->settings, expected);

    // Left out, empty, or empty with white space: the same.
    for (const std::string_view settings :
         {"{}"sv, "{ }"sv, "{\n}"sv})
    {
        const api_message empty =
            decode_ok(hello_ack_with_settings(settings));
        const auto* const empty_ack =
            std::get_if<hello_ack_message>(&empty.body);
        REQUIRE(empty_ack != nullptr);
        REQUIRE_EQ(empty_ack->settings, std::string("{}"));
    }

    for (const std::string_view bad :
         {"[]"sv, R"("x")"sv, "5"sv, "null"sv, "true"sv, "1.5"sv})
    {
        require_refused(hello_ack_with_settings(bad),
                        "hello_ack.settings: must be a JSON object");
    }
}

TEST_CASE("plugin protocol: a fraction in settings keeps its value")
{
    // The text of a number may change, as the JSON writer spells it; the
    // value does not.
    const api_message read = decode_ok(
        hello_ack_with_settings(R"({"a":1.5,"b":[0.1,-2.25e-3,1e300]})"));
    const auto* const ack = std::get_if<hello_ack_message>(&read.body);
    REQUIRE(ack != nullptr);
    const bj::value settings = bj::parse(ack->settings);
    const bj::object& object = settings.as_object();
    REQUIRE_NEAR(object.at("a").as_double(), 1.5, 0.0);
    const bj::array& list = object.at("b").as_array();
    REQUIRE_EQ(list.size(), std::size_t{3});
    REQUIRE_NEAR(list.at(0).as_double(), 0.1, 0.0);
    REQUIRE_NEAR(list.at(1).as_double(), -2.25e-3, 0.0);
    REQUIRE_NEAR(list.at(2).as_double(), 1e300, 0.0);
}

TEST_CASE("plugin protocol: settings may nest as far as the header allows")
{
    // The header is one level, so the settings may have 7 more.
    const api_message read =
        decode_ok(hello_ack_with_settings(nested_objects(7)));
    const auto* const ack = std::get_if<hello_ack_message>(&read.body);
    REQUIRE(ack != nullptr);
    REQUIRE_EQ(ack->settings, nested_objects(7));
    require_refused(hello_ack_with_settings(nested_objects(8)),
                    "header: is not JSON");

    // Written the same way.
    hello_ack_message body;
    body.settings = nested_objects(7);
    REQUIRE(encode_message(message_of(body, 0, 1)).has_value());
    body.settings = nested_objects(8);
    require_not_written(message_of(body, 0, 1));
}

TEST_CASE("plugin protocol: settings read back as the same value")
{
    for (const std::string_view settings :
         {R"({"n":[0,-0,1e5,1.5e-7,12345678901234567890,)"
          R"(-9223372036854775808]})"sv,
          R"({"n":[0.1,1E2,-5,123456789012345678901234567890,1e308]})"sv,
          R"({"t":true,"f":false,"z":null,"s":"","e":[],"o":{}})"sv,
          R"({"s":"\u0001\u001f\"\\\/ \u00e9 \ud83d\ude00"})"sv})
    {
        const api_message first = decode_ok(hello_ack_with_settings(settings));
        const auto written = encode_message(first);
        REQUIRE(written.has_value());
        const auto second = decode_message(written.value());
        REQUIRE(second.has_value());
        REQUIRE(second.value() == first);
        const auto again = encode_message(second.value());
        REQUIRE(again.has_value());
        REQUIRE_EQ(again.value().header, written.value().header);
    }
}

TEST_CASE("plugin protocol: settings with a number that JSON cannot write")
{
    // A number too large for a double is refused, so that what is written
    // is always text that is JSON.
    for (const std::string_view settings :
         {R"({"a":1e999})"sv, R"({"a":-1e999})"sv, R"({"a":[1e400]})"sv})
    {
        require_refused(hello_ack_with_settings(settings), "settings");
    }
    // The largest double is fine.
    const api_message read =
        decode_ok(hello_ack_with_settings(R"({"a":1.7976931348623157e308})"));
    const auto written = encode_message(read);
    REQUIRE(written.has_value());
    const auto again = decode_message(written.value());
    REQUIRE(again.has_value());
    REQUIRE(again.value() == read);
}

// -- writing is as strict as reading -----------------------------------------

TEST_CASE("plugin protocol: a message whose op is not the body's is not "
          "written")
{
    api_message message = message_of(ping_message{}, 1);
    message.op = "pong";
    require_not_written(message);
    message.op = "x.y";
    require_not_written(message);
    message.op = "Ping";
    require_not_written(message);
    message.op = "PING";
    require_not_written(message);
    message.op = "ping";
    REQUIRE(encode_message(message).has_value());

    api_message hello = message_of(sample_hello(), 1);
    hello.op = "hello_ack";
    require_not_written(hello);
    hello.op = "hello";
    REQUIRE(encode_message(hello).has_value());
}

TEST_CASE("plugin protocol: an id and a re are written only within their range")
{
    REQUIRE(
        encode_message(message_of(ping_message{}, max_message_id)).has_value());
    REQUIRE(encode_message(message_of(ok_message{}, 0, max_message_id))
                .has_value());
    REQUIRE(encode_message(message_of(ping_message{}, 1)).has_value());
    require_not_written(message_of(ping_message{}, max_message_id + 1));
    require_not_written(message_of(ok_message{}, 0, max_message_id + 1));
    constexpr std::uint64_t largest = std::numeric_limits<std::uint64_t>::max();
    require_not_written(message_of(ping_message{}, largest));
    require_not_written(message_of(ok_message{}, 0, largest));
    require_not_written(message_of(ping_message{}, 1, 1));
    require_not_written(message_of(ping_message{}, 5, 7));
    require_not_written(
        message_of(ping_message{}, max_message_id, max_message_id));
}

TEST_CASE("plugin protocol: a body that could not be read back is not written")
{
    // hello
    for (const std::string_view bad_id :
         {""sv, "ab"sv, "nodots"sv, "Com.Example"sv, "com..example"sv,
          "com.exa mple"sv, "com.example\n"sv})
    {
        hello_message body = sample_hello();
        body.plugin_id = std::string(bad_id);
        require_not_written(message_of(body, 1));
    }
    for (const std::string_view bad_version :
         {""sv, "a\nb"sv, "a\tb"sv, "\x01"sv, "a\x7f"sv, "\xff"sv,
          "ab\xc3"sv, "\xc0\x80"sv, "\xed\xa0\x80"sv,
          "a\xe2\x80\xae\xe2\x80\xac"sv,
          "\xc2\x85"sv, "\xef\xbb\xbf"sv, "a\xe2\x80\xa8"sv})
    {
        hello_message body = sample_hello();
        body.version = std::string(bad_version);
        require_not_written(message_of(body, 1));
    }
    {
        hello_message body = sample_hello();
        body.version = repeated('v', 65);
        require_not_written(message_of(body, 1));
        body.version = repeated('v', 64);
        REQUIRE(encode_message(message_of(body, 1)).has_value());
        body = sample_hello();
        body.pid = max_message_id + 1;
        require_not_written(message_of(body, 1));
        body.pid = max_message_id;
        REQUIRE(encode_message(message_of(body, 1)).has_value());
    }

    // hello_ack
    {
        hello_ack_message body = sample_hello_ack();
        body.granted = {permission::faults, permission::faults};
        require_not_written(message_of(body, 0, 1));
    }
    for (const std::string_view bad_name :
         {""sv, "Engine"sv, "a-b"sv, "a.b"sv, "a b"sv, "a\n"sv, "\xff"sv})
    {
        hello_ack_message body = sample_hello_ack();
        body.capabilities = {std::string(bad_name)};
        require_not_written(message_of(body, 0, 1));
    }
    {
        hello_ack_message body = sample_hello_ack();
        body.capabilities = {"a", "b", "a"};
        require_not_written(message_of(body, 0, 1));
        body.capabilities = {repeated('a', 65)};
        require_not_written(message_of(body, 0, 1));
        body.capabilities = {repeated('a', 64)};
        REQUIRE(encode_message(message_of(body, 0, 1)).has_value());
        body.capabilities.clear();
        for (int i = 0; i < 256; ++i)
            body.capabilities.push_back("c" + std::to_string(i));
        REQUIRE(encode_message(message_of(body, 0, 1)).has_value());
        body.capabilities.push_back("c256");
        require_not_written(message_of(body, 0, 1));

        body = sample_hello_ack();
        body.data_directory = repeated('d', 1024);
        REQUIRE(encode_message(message_of(body, 0, 1)).has_value());
        body.data_directory = repeated('d', 1025);
        require_not_written(message_of(body, 0, 1));
        body.data_directory = "a\nb";
        require_not_written(message_of(body, 0, 1));
        body.data_directory = "\xff";
        require_not_written(message_of(body, 0, 1));

        body = sample_hello_ack();
        body.limits.frames_per_second = 0;
        require_not_written(message_of(body, 0, 1));
        body = sample_hello_ack();
        body.limits.instances = 0;
        require_not_written(message_of(body, 0, 1));
    }
    for (const std::string_view bad_settings :
         {""sv, "[]"sv, "1"sv, "null"sv, "\"x\""sv, "{"sv, "{} x"sv,
          R"({"a":1,"a":2})"sv, "{'a':1}"sv, "not json"sv})
    {
        hello_ack_message body = sample_hello_ack();
        body.settings = std::string(bad_settings);
        require_not_written(message_of(body, 0, 1));
    }

    // status, log, shutdown, error
    {
        status_message body;
        body.text = repeated('t', 201);
        require_not_written(message_of(body));
        body.text = "a\nb";
        require_not_written(message_of(body));
        body.text = "\xff";
        require_not_written(message_of(body));
        body.text.clear();
        for (const std::string_view bad_code :
             {"Upper"sv, "has space"sv, "x/y"sv, "a\nb"sv, "\xc3\xa9"sv})
        {
            body.code = std::string(bad_code);
            require_not_written(message_of(body));
        }
        body.code = repeated('c', 65);
        require_not_written(message_of(body));
        body.code = repeated('c', 64);
        REQUIRE(encode_message(message_of(body)).has_value());
    }
    {
        log_message body;
        body.text = repeated('t', 1025);
        require_not_written(message_of(body));
        body.text = std::string("a\0b", 3);
        require_not_written(message_of(body));
        body.text = "a\xe2\x80\xae\xe2\x80\xacz";
        require_not_written(message_of(body));
        body.text = "caf\xc3";
        require_not_written(message_of(body));
    }
    {
        shutdown_message body;
        body.reason = repeated('r', 65);
        require_not_written(message_of(body, 1));
        body.reason = "a\nb";
        require_not_written(message_of(body, 1));
        body.reason = repeated('r', 64);
        REQUIRE(encode_message(message_of(body, 1)).has_value());
    }
    {
        error_message body;
        body.category = "vss";
        body.value = "x";
        REQUIRE(encode_message(message_of(body, 0, 1)).has_value());
        error_message empty_category = body;
        empty_category.category.clear();
        require_not_written(message_of(empty_category, 0, 1));
        error_message empty_value = body;
        empty_value.value.clear();
        require_not_written(message_of(empty_value, 0, 1));
        for (const std::string_view bad_name :
             {"Vss"sv, "a-b"sv, "a b"sv, "\xc3\xa9"sv})
        {
            error_message other = body;
            other.category = std::string(bad_name);
            require_not_written(message_of(other, 0, 1));
        }
        body.value = "a.b";
        require_not_written(message_of(body, 0, 1));
        body.value = repeated('v', 65);
        require_not_written(message_of(body, 0, 1));
        body.value = "x";
        body.category = repeated('c', 65);
        require_not_written(message_of(body, 0, 1));
        body.category = "vss";
        body.message = repeated('m', 1025);
        require_not_written(message_of(body, 0, 1));
        body.message = "a\tb";
        require_not_written(message_of(body, 0, 1));
    }
}

TEST_CASE("plugin protocol: an other_message is written only when it fits")
{
    const auto other = [](std::string_view op, std::string_view header,
                          std::uint64_t id = 0, std::uint64_t re = 0) {
        api_message out;
        out.op = std::string(op);
        out.id = id;
        out.re = re;
        out.body = other_message{std::string(header)};
        return out;
    };

    // What fits is written as it is, white space and all.
    const std::string spaced = R"( {"op" : "x.y", "id": 4, "q": [1, 2]} )";
    const auto written = encode_message(other("x.y", spaced, 4));
    REQUIRE(written.has_value());
    REQUIRE_EQ(written.value().header, spaced);
    REQUIRE(encode_message(other("x.y", R"({"op":"x.y"})")).has_value());
    REQUIRE(encode_message(other("x.y", R"({"op":"x.y","re":3})", 0, 3))
                .has_value());
    REQUIRE(encode_message(
                other("x.y", R"({"op":"x.y","id":9007199254740991})",
                      max_message_id))
                .has_value());

    // The header is not one JSON object.
    for (const std::string_view bad :
         {""sv, "x"sv, "[]"sv, "5"sv, "null"sv, R"({"op":"x.y")"sv,
          R"({"op":"x.y"} x)"sv, R"({"op":"x.y","a":1,"a":2})"sv,
          R"({"op":"x.y","op":"x.y"})"sv})
    {
        require_not_written(other("x.y", bad));
    }
    require_not_written(other("x.y", deep_header("x.y", 8)));
    REQUIRE(encode_message(other("x.y", deep_header("x.y", 7))).has_value());

    // Its op is not the message's.
    require_not_written(other("x.y", R"({"op":"x.z"})"));
    require_not_written(other("x.y", R"({"op":"X.Y"})"));
    require_not_written(other("x.y", R"({"op":5})"));
    require_not_written(other("x.y", R"({"id":1})", 1));
    require_not_written(other("x.y", "{}"));
    require_not_written(other("", R"({"op":""})"));
    require_not_written(other("", R"({"op":"x.y"})"));
    require_not_written(other("X.y", R"({"op":"X.y"})"));
    require_not_written(other("x..y", R"({"op":"x..y"})"));

    // The op is one this file reads as a message of its own.
    for (const std::string_view known :
         {"hello"sv, "hello_ack"sv, "ping"sv, "pong"sv, "status"sv, "log"sv,
          "shutdown"sv, "bye"sv, "ok"sv, "error"sv})
    {
        std::string header = R"({"op":")";
        header += known;
        header += R"("})";
        require_not_written(other(known, header));
    }

    // Its id and re are not the message's.
    require_not_written(other("x.y", R"({"op":"x.y","id":4})", 5));
    require_not_written(other("x.y", R"({"op":"x.y","id":4})", 0));
    require_not_written(other("x.y", R"({"op":"x.y"})", 4));
    require_not_written(other("x.y", R"({"op":"x.y","re":4})", 0, 5));
    require_not_written(other("x.y", R"({"op":"x.y","re":4})", 4, 0));
    require_not_written(other("x.y", R"({"op":"x.y"})", 0, 4));
    require_not_written(other("x.y", R"({"op":"x.y","id":4,"re":4})", 4, 4));
    require_not_written(other("x.y", R"({"op":"x.y","id":"4"})", 4));
    require_not_written(other("x.y", R"({"op":"x.y","id":4.5})", 4));
    require_not_written(other("x.y", R"({"op":"x.y","id":0})", 0));
    require_not_written(other("x.y", R"({"op":"x.y","re":0})", 0, 0));
    require_not_written(
        other("x.y", R"({"op":"x.y","id":9007199254740992})",
              max_message_id + 1));
}

TEST_CASE("plugin protocol: an unknown body is written with its blob")
{
    api_message message;
    message.op = "generator.write";
    message.id = 2;
    message.body = other_message{R"({"op":"generator.write","id":2})"};
    message.blob = sample_blob();
    const auto written = encode_message(message);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob == sample_blob());
    const auto read = decode_message(written.value());
    REQUIRE(read.has_value());
    REQUIRE(read.value() == message);
}

// -- make_error --------------------------------------------------------------

TEST_CASE("plugin protocol: make_error builds an error answer")
{
    const api_message message = make_error(7, api_errc::busy, "too many");
    REQUIRE_EQ(message.op, "error");
    REQUIRE_EQ(message.id, std::uint64_t{0});
    REQUIRE_EQ(message.re, std::uint64_t{7});
    REQUIRE(message.blob.empty());
    const auto* const body = std::get_if<error_message>(&message.body);
    REQUIRE(body != nullptr);
    REQUIRE_EQ(body->category, "vss.plugin.api");
    REQUIRE_EQ(body->value, "busy");
    REQUIRE_EQ(body->message, "too many");
    check_wire(message,
               R"({"op":"error","re":7,"category":"vss.plugin.api",)"
               R"("value":"busy","message":"too many"})");

    // Every value has its name, and no message leaves it out.
    for (const api_errc value :
         {api_errc::success, api_errc::unknown_op, api_errc::bad_request,
          api_errc::refused, api_errc::not_granted, api_errc::not_declared,
          api_errc::timeout, api_errc::busy, api_errc::unsupported,
          api_errc::plugin_unavailable})
    {
        const api_message made = make_error(1, value, {});
        const auto* const made_body = std::get_if<error_message>(&made.body);
        REQUIRE(made_body != nullptr);
        REQUIRE_EQ(made_body->value, std::string(to_string(value)));
        REQUIRE(made_body->message.empty());
        REQUIRE(encode_message(made).has_value());
    }

    // Answering nothing in particular.
    const api_message none = make_error(0, api_errc::unknown_op, "x");
    REQUIRE_EQ(none.re, std::uint64_t{0});
    check_wire(none,
               R"({"op":"error","category":"vss.plugin.api",)"
               R"("value":"unknown_op","message":"x"})");
}

TEST_CASE("plugin protocol: make_error cuts a long message at a character")
{
    // Plain letters: cut at the limit.
    const api_message plain =
        make_error(1, api_errc::refused, repeated('a', 5000));
    const auto* const body = std::get_if<error_message>(&plain.body);
    REQUIRE(body != nullptr);
    REQUIRE_EQ(body->message.size(), max_message_text);
    REQUIRE(encode_message(plain).has_value());

    // Exactly the limit is kept whole, one byte over loses the last byte.
    REQUIRE_EQ(std::get<error_message>(
                   make_error(1, api_errc::refused, repeated('a', 1024)).body)
                   .message.size(),
               std::size_t{1024});
    REQUIRE_EQ(std::get<error_message>(
                   make_error(1, api_errc::refused, repeated('a', 1025)).body)
                   .message.size(),
               std::size_t{1024});

    // Three-byte characters: 341 of them are 1023 bytes, and the next would
    // not fit, so none of it is kept.
    std::string euros;
    for (int i = 0; i < 400; ++i)
        euros += "\xe2\x82\xac";
    const api_message cut = make_error(1, api_errc::refused, euros);
    const auto* const cut_body = std::get_if<error_message>(&cut.body);
    REQUIRE(cut_body != nullptr);
    REQUIRE_EQ(cut_body->message.size(), std::size_t{1023});
    REQUIRE(cut_body->message == euros.substr(0, 1023));
    REQUIRE(encode_message(cut).has_value());

    // Four-byte characters after 1022 letters: no room for the character.
    std::string text = repeated('a', 1022);
    text += "\xf0\x9f\x98\x80";
    const api_message emoji = make_error(1, api_errc::refused, text);
    REQUIRE_EQ(std::get<error_message>(emoji.body).message,
               repeated('a', 1022));
    // And with room, it is kept.
    text = repeated('a', 1020);
    text += "\xf0\x9f\x98\x80";
    REQUIRE_EQ(std::get<error_message>(
                   make_error(1, api_errc::refused, text).body)
                   .message,
               text);
}

TEST_CASE("plugin protocol: make_error replaces what could not be written")
{
    struct replaced
    {
        std::string_view given;
        std::string_view expected;
    };
    const std::array cases{
        replaced{"a\nb\tc\rd"sv, "a b c d"sv},
        replaced{"\x01\x02"sv, "  "sv},
        replaced{"x\x7f""y"sv, "x y"sv},
        replaced{"x\xc2\x85y"sv, "x y"sv},
        replaced{"x\xc2\x9fy"sv, "x y"sv},
        replaced{"x\xe2\x80\xae\xe2\x80\xacy"sv, "x  y"sv},
        replaced{"x\xe2\x80\x8fy"sv, "x y"sv},
        replaced{"x\xe2\x81\xa6\xe2\x81\xa9y"sv, "x  y"sv},
        replaced{"x\xe2\x80\xa8y"sv, "x y"sv},
        replaced{"x\xd8\x9cy"sv, "x y"sv},
        replaced{"x\xef\xbb\xbfy"sv, "x y"sv},
        replaced{"a\0b"sv, "a b"sv},
        replaced{"\xff\xfe"sv, "  "sv},
        replaced{"ab\xc3"sv, "ab "sv},
        replaced{"\xc0\x80"sv, "  "sv},
        replaced{"\xed\xa0\x80"sv, "   "sv},
        replaced{"\xf4\x90\x80\x80"sv, "    "sv},
        replaced{"\xe2\x82"sv, "  "sv},
        replaced{"caf\xc3\xa9 \xe2\x82\xac"sv, "caf\xc3\xa9 \xe2\x82\xac"sv},
        replaced{"\xc2\xa0\xe2\x80\x8b"sv, "\xc2\xa0\xe2\x80\x8b"sv}};
    for (const replaced& item : cases)
    {
        const api_message made =
            make_error(3, api_errc::bad_request, item.given);
        const auto* const body = std::get_if<error_message>(&made.body);
        REQUIRE(body != nullptr);
        REQUIRE_EQ(body->message, std::string(item.expected));
        // It always writes, and reads back the same.
        const auto written = encode_message(made);
        REQUIRE(written.has_value());
        const auto read = decode_message(written.value());
        REQUIRE(read.has_value());
        REQUIRE(read.value() == made);
    }

    // A hostile message of the full size.
    std::string hostile;
    for (int i = 0; i < 2000; ++i)
    {
        if (i % 3 == 0)
            hostile += '\n';
        else if (i % 3 == 1)
            hostile += '\xff';
        else
            hostile += "\xe2\x80\xae\xe2\x80\xac";
    }
    const api_message made = make_error(3, api_errc::bad_request, hostile);
    REQUIRE(encode_message(made).has_value());
    REQUIRE(std::get<error_message>(made.body).message.size() <=
            max_message_text);
}

// -- what a detail line holds ------------------------------------------------

TEST_CASE("plugin protocol: a detail line never repeats what was sent")
{
    // Each header carries `MARKER_7f3a` where a plugin could put anything,
    // and each is refused.
    const std::vector<std::string> hostile{
        R"({"op":"ping","MARKER_7f3a":1,"MARKER_7f3a":2})",
        R"({"op":"x.y","MARKER_7f3a":1,"MARKER_7f3a":2})",
        R"({"op":"ping","a":{"MARKER_7f3a":1,"MARKER_7f3a":1}})",
        R"({"op":"MARKER_7f3a"})",
        R"({"op":"ping","id":"MARKER_7f3a"})",
        R"({"op":"ping","re":MARKER_7f3a})",
        R"({"op":"x.y","id":"MARKER_7f3a"})",
        R"({"op":"ping"} MARKER_7f3a)",
        R"(MARKER_7f3a)",
        R"(["MARKER_7f3a"])",
        R"({"MARKER_7f3a":1})",
        R"({"op":["MARKER_7f3a"]})",
        with_member("hello", "api", R"("MARKER_7f3a")"),
        with_member("hello", "plugin_id", R"("MARKER_7f3a")"),
        with_member("hello", "version", R"("MARKER_7f3a\u0001")"),
        with_member("hello", "pid", R"("MARKER_7f3a")"),
        with_member("hello_ack", "granted", R"(["MARKER_7f3a"])"),
        with_member("hello_ack", "capabilities", R"(["MARKER_7f3a"])"),
        with_member("hello_ack", "capabilities",
                    R"(["marker_7f3a","marker_7f3a"])"),
        with_member("hello_ack", "granted",
                    R"(["ports.read","ports.read"])"),
        with_member("hello_ack", "limits", R"("MARKER_7f3a")"),
        with_limit("instances", R"("MARKER_7f3a")"),
        with_member("hello_ack", "data_directory", R"("MARKER_7f3a\u202e")"),
        with_member("hello_ack", "settings", R"("MARKER_7f3a")"),
        hello_ack_with_settings(R"({"MARKER_7f3a":1,"MARKER_7f3a":2})"),
        with_member("status", "level", R"("MARKER_7f3a")"),
        with_member("status", "code", R"("MARKER_7f3a")"),
        with_member("status", "text", R"("MARKER_7f3a\u0001")"),
        with_member("log", "level", R"("MARKER_7f3a")"),
        with_member("log", "text", R"("MARKER_7f3a\u202e")"),
        with_member("shutdown", "reason", R"("MARKER_7f3a\u0000")"),
        with_member("error", "category", R"("MARKER_7f3a")"),
        with_member("error", "value", R"("MARKER_7f3a")"),
        with_member("error", "message", R"("MARKER_7f3a\n")")};
    for (const std::string& header : hostile)
    {
        const std::string detail = refusal_of(header);
        REQUIRE(detail.find("MARKER_7f3a") == std::string::npos);
        REQUIRE(detail.find("marker_7f3a") == std::string::npos);
        // A line for a log: no control character in it.
        for (const char c : detail)
            REQUIRE(c >= 0x20 && c <= 0x7e);
    }

    // The same for a marker too long to fit, at the limit of a text.
    std::string long_text = R"({"op":"log","level":"info","text":")";
    long_text += marker;
    long_text += repeated('a', 1100);
    long_text += R"("})";
    REQUIRE(refusal_of(long_text).find(marker) == std::string::npos);
    REQUIRE(refusal_of(long_text).size() < 200);
}

TEST_CASE("plugin protocol: the detail of a frame that is accepted is empty")
{
    std::string detail = "left over";
    const auto decoded = decode_message(frame_of(hello_text), &detail);
    REQUIRE(decoded.has_value());
    REQUIRE(detail.empty());
    // Asking for no detail is allowed.
    REQUIRE(decode_message(frame_of(hello_text), nullptr).has_value());
    REQUIRE(decode_message(frame_of("{"), nullptr).has_error());
}

TEST_CASE("plugin protocol: the first problem is the one named")
{
    // Two members are wrong; the one that the table lists first is named.
    const std::string detail = refusal_of(
        R"({"op":"hello","id":1,"api":"x","plugin_id":"Y","version":""})");
    REQUIRE_EQ(detail, std::string("hello.api: must be a version such as 1.0"));
    const std::string second = refusal_of(
        R"({"op":"log","level":"info","text":5,"extra":1})");
    REQUIRE_EQ(second, std::string("log.text: must be a string"));
}

TEST_CASE("plugin protocol: a header has a size limit, and many keys cost "
          "little")
{
    // A header of many members, each key different: the largest that fits.
    // Finding a repeated key must not take time that grows with the square
    // of their number, or one frame could hold the host up.
    std::string header = R"({"op":"vendor.many")";
    std::size_t count = 0;
    while (header.size() + 24 < max_header_size)
    {
        header += ",\"k";
        header += std::to_string(count++);
        header += "\":0";
    }
    header += '}';
    REQUIRE(header.size() <= max_header_size);
    REQUIRE(count > 4000);
    std::string detail;
    const auto many = decode_message(frame_of(header), &detail);
    REQUIRE(many.has_value());
    REQUIRE(detail.empty());

    // The same with the first key given again at the very end.
    std::string repeated = header;
    repeated.pop_back();
    repeated += ",\"k0\":1}";
    require_refused(repeated, "has a key given twice");

    // One byte over the limit is refused before it is read at all: what
    // follows the limit is not even JSON.
    std::string large = R"({"op":"ping","pad":")";
    large.append(max_header_size, 'x');
    require_refused(large, "is larger than");
    std::string exact = R"({"op":"ping","pad":")";
    exact.append(max_header_size - exact.size() - 2, 'x');
    exact += "\"}";
    REQUIRE_EQ(exact.size(), max_header_size);
    REQUIRE(decode_message(frame_of(exact)).has_value());
}

TEST_CASE("plugin protocol: the ids of a message can be set, in the header "
          "of an unknown message too")
{
    // A message this file knows: the two fields.
    api_message ping;
    ping.body = ping_message{};
    const auto asked = with_ids(ping, 7, 0);
    REQUIRE(asked.has_value());
    REQUIRE_EQ(asked.value().id, std::uint64_t{7});
    REQUIRE_EQ(asked.value().re, std::uint64_t{0});
    REQUIRE(encode_message(asked.value()).has_value());

    // An unknown message: its header carries them, in place of what it
    // had, and the rest of the header is kept.
    api_message vendor;
    vendor.op = "vendor.start";
    vendor.body =
        other_message{R"({"op":"vendor.start","id":99,"rate":5})"};
    const auto numbered = with_ids(vendor, 12, 0);
    REQUIRE(numbered.has_value());
    const auto frame = encode_message(numbered.value());
    REQUIRE(frame.has_value());
    REQUIRE_EQ(frame.value().header,
               R"({"op":"vendor.start","rate":5,"id":12})");

    const auto answer = with_ids(vendor, 0, 31);
    REQUIRE(answer.has_value());
    REQUIRE_EQ(encode_message(answer.value()).value().header,
               R"({"op":"vendor.start","rate":5,"re":31})");

    const auto plain = with_ids(vendor, 0, 0);
    REQUIRE(plain.has_value());
    REQUIRE_EQ(encode_message(plain.value()).value().header,
               R"({"op":"vendor.start","rate":5})");

    // A header that is not an object cannot be numbered.
    api_message broken;
    broken.op = "vendor.start";
    broken.body = other_message{"[1,2]"};
    const auto refused = with_ids(broken, 1, 0);
    REQUIRE(refused.has_error());
    REQUIRE(refused.error() == api_errc::bad_request);
}
