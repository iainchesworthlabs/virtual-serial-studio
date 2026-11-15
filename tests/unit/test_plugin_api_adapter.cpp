//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_adapter.hpp>

#include "plugin_api_support.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

using namespace vss;
using namespace vss::plugin;
using namespace vss::plugin_api_test;

namespace
{

std::string repeated(char c, std::size_t count)
{
    return std::string(count, c);
}

byte_buffer bytes_of(std::size_t count)
{
    byte_buffer out(count);
    for (std::size_t i = 0; i < count; ++i)
        out[i] = static_cast<std::uint8_t>(i * 7);
    return out;
}

std::string header_of(const api_message& message)
{
    return std::get<other_message>(message.body).header;
}

/// `["p","p",...]` with `count` names.
std::string place_list(std::size_t count)
{
    std::string out = "[";
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            out.append(",");
        out.append("\"p");
        out.append(std::to_string(i));
        out.append("\"");
    }
    out.append("]");
    return out;
}

} // namespace

TEST_CASE("plugin api: agent.call is written and read")
{
    agent_call_message body;
    body.agent_op = "list";
    body.args = R"({"kind":"port"})";
    const auto read = round_trip(body, 4);
    REQUIRE(read.data.empty());
    const auto written = encode_as(body, 4);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"agent.call","id":4,)"
                           R"("agent_op":"list","args":{"kind":"port"}})"));
    REQUIRE(written.value().blob.empty());

    // The arguments are left out when there are none, and the bytes are
    // in the blob.
    const auto plain = accepted<agent_call_message>(
        R"({"op":"agent.call","id":1,"agent_op":"send"})", bytes_of(5));
    REQUIRE_EQ(plain.args, std::string("{}"));
    REQUIRE(plain.data == bytes_of(5));

    agent_call_message with_bytes;
    with_bytes.agent_op = "send";
    with_bytes.data = bytes_of(40);
    const auto again = round_trip(with_bytes, 9);
    REQUIRE(again.data == with_bytes.data);
    const auto wire = encode_as(with_bytes, 9);
    REQUIRE(wire.has_value());
    REQUIRE(wire.value().blob == with_bytes.data);
    REQUIRE_EQ(header_of(wire.value()),
               std::string(R"({"op":"agent.call","id":9,)"
                           R"("agent_op":"send","args":{}})"));
}

TEST_CASE("plugin api: agent.call is refused when it is wrong")
{
    using message_type = agent_call_message;
    const std::string head = R"({"op":"agent.call","id":1,)";
    require_refused<message_type>(R"({"op":"agent.call","id":1})",
                                  "agent.call.agent_op: is required");
    require_refused<message_type>(head + R"("agent_op":7})",
                                  "agent.call.agent_op: must be a string");
    require_refused<message_type>(head + R"("agent_op":""})",
                                  "agent.call.agent_op: must be 1 to 64");
    require_refused<message_type>(head + R"("agent_op":"List"})",
                                  "agent.call.agent_op: must be 1 to 64");
    require_refused<message_type>(head + R"("agent_op":"a b"})",
                                  "agent.call.agent_op: must be 1 to 64");
    require_refused<message_type>(head + R"("agent_op":"a/b"})",
                                  "agent.call.agent_op: must be 1 to 64");
    require_refused<message_type>(head + R"("agent_op":"list","args":null})",
                                  "agent.call.args: must be a JSON object");
    require_refused<message_type>(head + R"("agent_op":"list","args":[1]})",
                                  "agent.call.args: must be a JSON object");
    require_refused<message_type>(
        R"({"op":"agent.call","re":1,"agent_op":"list"})", "must have an id");
    require_refused<message_type>(R"({"op":"agent.call","agent_op":"list"})",
                                  "must have an id");
}

TEST_CASE("plugin api: the operation of agent.call is a token of 64 bytes")
{
    using message_type = agent_call_message;
    const std::string head = R"({"op":"agent.call","id":1,"agent_op":")";
    REQUIRE_EQ(accepted<message_type>(head + repeated('a', 64) + "\"}")
                   .agent_op.size(),
               std::size_t{64});
    REQUIRE_EQ(accepted<message_type>(head + "a.b-c_9\"}").agent_op,
               std::string("a.b-c_9"));
    require_refused<message_type>(head + repeated('a', 65) + "\"}",
                                  "agent.call.agent_op: must be 1 to 64");

    message_type body;
    body.agent_op = repeated('a', 64);
    (void)round_trip(body, 1);
    body.agent_op = repeated('a', 65);
    require_not_written(body, 1);
    body.agent_op.clear();
    require_not_written(body, 1);
    body.agent_op = "Bad";
    require_not_written(body, 1);
    body.agent_op = "list";
    body.args = "[]";
    require_not_written(body, 1);
    body.args = "not json";
    require_not_written(body, 1);
    // A request needs a number.
    body.args = "{}";
    require_not_written(body, 0);
}

TEST_CASE("plugin api: agent.call has 0 to 256 KiB of bytes in the blob")
{
    using message_type = agent_call_message;
    const std::string header =
        R"({"op":"agent.call","id":1,"agent_op":"send"})";
    REQUIRE_EQ(
        accepted<message_type>(header, bytes_of(max_api_blob)).data.size(),
        max_api_blob);
    REQUIRE_EQ(accepted<message_type>(header).data.size(), std::size_t{0});
    require_refused<message_type>(header, "has a blob of 262145 bytes",
                                  bytes_of(max_api_blob + 1));

    message_type body;
    body.agent_op = "send";
    body.data = bytes_of(max_api_blob);
    (void)round_trip(body, 2);
    body.data = bytes_of(max_api_blob + 1);
    require_not_written(body, 2);
}

TEST_CASE("plugin api: agent.result is written and read")
{
    agent_result_message body;
    body.result = R"({"ports":2})";
    body.data = bytes_of(12);
    const auto read = round_trip(body, 3);
    REQUIRE(read.data == body.data);
    const auto written = encode_as(body, 3);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"agent.result","re":3,)"
                           R"("result":{"ports":2}})"));
    REQUIRE(written.value().blob == body.data);

    agent_result_message nothing;
    const auto none = round_trip(nothing, 4);
    REQUIRE_EQ(none.result, std::string("{}"));
    REQUIRE(none.data.empty());
    const auto plain =
        accepted<agent_result_message>(R"({"op":"agent.result","re":5})");
    REQUIRE_EQ(plain.result, std::string("{}"));
}

TEST_CASE("plugin api: agent.result is refused when it is wrong")
{
    using message_type = agent_result_message;
    const std::string head = R"({"op":"agent.result","re":3,)";
    require_refused<message_type>(head + R"("result":1})",
                                  "agent.result.result: must be a JSON object");
    require_refused<message_type>(head + R"("result":null})",
                                  "agent.result.result: must be a JSON object");
    require_refused<message_type>(R"({"op":"agent.result","id":3,"result":{}})",
                                  "must have a re and no id");
    require_refused<message_type>(R"({"op":"agent.result","result":{}})",
                                  "must have a re and no id");
    REQUIRE_EQ(
        accepted<message_type>(head + R"("result":{}})", bytes_of(max_api_blob))
            .data.size(),
        max_api_blob);
    require_refused<message_type>(head + R"("result":{}})",
                                  "has a blob of 262145 bytes",
                                  bytes_of(max_api_blob + 1));

    message_type body;
    body.data = bytes_of(max_api_blob + 1);
    require_not_written(body, 3);
    body.data.clear();
    body.result = "[]";
    require_not_written(body, 3);
    body.result = "{}";
    // An answer needs the number of its request.
    require_not_written(body, 0);
}

TEST_CASE("plugin api: agent.subscribe is written and read")
{
    agent_subscribe_message body;
    body.data = true;
    body.places = {"COM7", "bus 1"};
    (void)round_trip(body, 6);
    const auto written = encode_as(body, 6);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"agent.subscribe","id":6,"data":true,)"
                           R"("places":["COM7","bus 1"]})"));

    // Without bytes and without places: every place the grant covers.
    agent_subscribe_message events;
    (void)round_trip(events, 7);
    const auto plain = encode_as(events, 7);
    REQUIRE(plain.has_value());
    REQUIRE_EQ(header_of(plain.value()),
               std::string(R"({"op":"agent.subscribe","id":7,"data":false})"));
    const auto read =
        accepted<agent_subscribe_message>(R"({"op":"agent.subscribe","id":1})");
    REQUIRE(!read.data);
    REQUIRE(read.places.empty());
    const auto empty = accepted<agent_subscribe_message>(
        R"({"op":"agent.subscribe","id":1,"places":[]})");
    REQUIRE(empty.places.empty());
}

TEST_CASE("plugin api: agent.subscribe is refused when it is wrong")
{
    using message_type = agent_subscribe_message;
    const std::string head = R"({"op":"agent.subscribe","id":1,)";
    require_refused<message_type>(
        head + R"("data":1})", "agent.subscribe.data: must be true or false");
    require_refused<message_type>(
        head + R"("data":"true"})",
        "agent.subscribe.data: must be true or false");
    require_refused<message_type>(head + R"("places":"COM7"})",
                                  "agent.subscribe.places: must be a list");
    require_refused<message_type>(
        head + R"("places":[7]})",
        "agent.subscribe.places[0]: must be a string");
    require_refused<message_type>(
        head + R"("places":["a",null]})",
        "agent.subscribe.places[1]: must be a string");
    require_refused<message_type>(
        head + R"("places":[""]})",
        "agent.subscribe.places[0]: must be 1 to 128 bytes");
    require_refused<message_type>(
        head + R"("places":["a\nb"]})",
        "agent.subscribe.places[0]: must be plain text");
    require_refused<message_type>(
        head + "\"places\":[\"a\xe2\x80\x8f"
               "b\"]}",
        "agent.subscribe.places[0]: must be plain text");
    require_refused<message_type>(head + R"("data":true})", "has no bytes",
                                  {1});
    require_refused<message_type>(R"({"op":"agent.subscribe","re":1})",
                                  "must have an id");
}

TEST_CASE("plugin api: agent.subscribe has up to 128 places of 128 bytes")
{
    using message_type = agent_subscribe_message;
    const std::string head = R"({"op":"agent.subscribe","id":1,"places":)";
    REQUIRE_EQ(
        accepted<message_type>(head + place_list(128) + "}").places.size(),
        std::size_t{128});
    require_refused<message_type>(
        head + place_list(129) + "}",
        "agent.subscribe.places: has more than 128 entries");
    REQUIRE_EQ(
        accepted<message_type>(head + "[\"" + repeated('n', 128) + "\"]}")
            .places[0]
            .size(),
        max_api_name);
    require_refused<message_type>(
        head + "[\"" + repeated('n', 129) + "\"]}",
        "agent.subscribe.places[0]: must be 1 to 128 bytes");

    message_type body;
    for (std::size_t i = 0; i < 128; ++i)
        body.places.push_back("p" + std::to_string(i));
    (void)round_trip(body, 3);
    body.places.push_back("one more");
    require_not_written(body, 3);
    body.places.pop_back();
    body.places[0] = repeated('n', 129);
    require_not_written(body, 3);
    body.places[0] = "";
    require_not_written(body, 3);
    body.places[0] = "a\nb";
    require_not_written(body, 3);
}

TEST_CASE("plugin api: adapter.state is written and read")
{
    adapter_state_message body;
    body.listening = true;
    body.address = "127.0.0.1:7410";
    body.protocol = "scpi-tcp";
    (void)round_trip(body);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"adapter.state","listening":true,)"
                           R"("address":"127.0.0.1:7410",)"
                           R"("protocol":"scpi-tcp"})"));

    adapter_state_message idle;
    idle.protocol = "scpi-tcp";
    (void)round_trip(idle);
    const auto quiet = encode_as(idle);
    REQUIRE(quiet.has_value());
    REQUIRE_EQ(header_of(quiet.value()),
               std::string(R"({"op":"adapter.state","listening":false,)"
                           R"("protocol":"scpi-tcp"})"));
}

TEST_CASE("plugin api: adapter.state is refused when it is wrong")
{
    using message_type = adapter_state_message;
    const std::string head = R"({"op":"adapter.state",)";
    require_refused<message_type>(head + R"("address":"a:1","protocol":"p"})",
                                  "adapter.state.listening: is required");
    require_refused<message_type>(
        head + R"("listening":"yes","protocol":"p"})",
        "adapter.state.listening: must be true or false");
    require_refused<message_type>(head + R"("listening":true,"protocol":"p"})",
                                  "adapter.state.address: is required");
    require_refused<message_type>(
        head + R"("listening":true,"address":"","protocol":"p"})",
        "adapter.state.address: must not be empty");
    require_refused<message_type>(
        head + R"("listening":true,"address":4,"protocol":"p"})",
        "adapter.state.address: must be a string");
    require_refused<message_type>(head + R"("listening":true,"address":"a\tb",)"
                                         R"("protocol":"p"})",
                                  "adapter.state.address: must be plain text");
    require_refused<message_type>(
        head + R"("listening":false,"address":"a:1",)"
               R"("protocol":"p"})",
        "adapter.state.address: must not be given when "
        "listening is false");
    require_refused<message_type>(head + R"("listening":false,"address":null,)"
                                         R"("protocol":"p"})",
                                  "adapter.state.address: must not be given");
    require_refused<message_type>(head + R"("listening":false})",
                                  "adapter.state.protocol: is required");
    require_refused<message_type>(head + R"("listening":false,"protocol":""})",
                                  "adapter.state.protocol: must not be empty");
    require_refused<message_type>(head + R"("listening":false,"protocol":3})",
                                  "adapter.state.protocol: must be a string");
    require_refused<message_type>(head + R"("listening":false,"protocol":"p"})",
                                  "has no bytes", {1});
    require_refused<message_type>(
        R"({"op":"adapter.state","id":2,"listening":false,)"
        R"("protocol":"p"})",
        "neither an id nor a re");
}

TEST_CASE("plugin api: adapter.state has the limits of its members")
{
    using message_type = adapter_state_message;
    const std::string head = R"({"op":"adapter.state","listening":true,)";
    REQUIRE_EQ(accepted<message_type>(head + R"("address":")" +
                                      repeated('a', 256) + R"(","protocol":")" +
                                      repeated('p', 64) + "\"}")
                   .address.size(),
               max_adapter_address);
    require_refused<message_type>(
        head + R"("address":")" + repeated('a', 257) + R"(","protocol":"p"})",
        "adapter.state.address: is longer than 256 bytes");
    require_refused<message_type>(
        head + R"("address":"a","protocol":")" + repeated('p', 65) + "\"}",
        "adapter.state.protocol: is longer than 64 bytes");

    message_type body;
    body.listening = true;
    body.address = repeated('a', 256);
    body.protocol = repeated('p', 64);
    (void)round_trip(body);
    body.address = repeated('a', 257);
    require_not_written(body);
    body.address = "a";
    body.protocol = repeated('p', 65);
    require_not_written(body);
    body.protocol.clear();
    require_not_written(body);
    body.protocol = "p";
    body.address.clear();
    require_not_written(body);
    // No address when it is not listening.
    body.listening = false;
    body.address = "a:1";
    require_not_written(body);
    // It is a notification: no id.
    body.address.clear();
    require_not_written(body, 3);
}

TEST_CASE("plugin api: a request of an adapter is answered plugin_unavailable")
{
    const api_message message = make_plugin_unavailable(7);
    REQUIRE_EQ(message.op, "error");
    REQUIRE_EQ(message.id, std::uint64_t{0});
    REQUIRE_EQ(message.re, std::uint64_t{7});
    REQUIRE(message.blob.empty());
    const auto* const body = std::get_if<error_message>(&message.body);
    REQUIRE(body != nullptr);
    REQUIRE_EQ(body->category, "vss.plugin.api");
    REQUIRE_EQ(body->value, "plugin_unavailable");
    REQUIRE(!body->message.empty());

    // It is written and read as the "error" message of protocol.hpp.
    const auto frame = encode_message(message);
    REQUIRE(frame.has_value());
    REQUIRE_EQ(frame.value().header,
               std::string(R"({"op":"error","re":7,)"
                           R"("category":"vss.plugin.api",)"
                           R"("value":"plugin_unavailable",)"
                           R"("message":"The plugin is not running."})"));
    std::string detail;
    const auto read = decode_message(frame.value(), &detail);
    REQUIRE(read.has_value());
    REQUIRE(read.value() == message);
    REQUIRE_EQ(read.value().re, std::uint64_t{7});
    const auto* const read_body =
        std::get_if<error_message>(&read.value().body);
    REQUIRE(read_body != nullptr);
    REQUIRE_EQ(read_body->value, std::string("plugin_unavailable"));

    // The value is one of api_errc.
    REQUIRE_EQ(to_string(api_errc::plugin_unavailable),
               std::string_view("plugin_unavailable"));
    // The same for every request number.
    REQUIRE(
        encode_message(make_plugin_unavailable(max_message_id)).has_value());
}

TEST_CASE("plugin api: agent.event is written and read")
{
    agent_event_message body;
    body.subscription = 12;
    body.kind = "data";
    body.place = "COM7";
    body.t_ns = 18204113250;
    body.data = {1, 2, 3};
    const auto read = round_trip(body);
    REQUIRE(read.data == body.data);
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE(written.value().blob == body.data);
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"agent.event","subscription":12,)"
                           R"("kind":"data","place":"COM7",)"
                           R"("t_ns":18204113250})"));
    // An event without bytes, as a subscription without data gets.
    body.kind = "counters";
    body.data.clear();
    (void)round_trip(body);
}

TEST_CASE("plugin api: agent.event is refused when it is wrong")
{
    using message_type = agent_event_message;
    const std::string head = R"({"op":"agent.event",)";
    const std::string rest =
        R"("subscription":1,"kind":"data","place":"p","t_ns":2})";
    (void)accepted<message_type>(head + rest);
    require_refused<message_type>(head +
                                      R"("kind":"data","place":"p","t_ns":2})",
                                  "agent.event.subscription: is required");
    require_refused<message_type>(
        head + R"("subscription":0,"kind":"data","place":"p","t_ns":2})",
        "agent.event.subscription: must be a whole number");
    require_refused<message_type>(
        head + R"("subscription":1,"place":"p","t_ns":2})",
        "agent.event.kind: is required");
    require_refused<message_type>(
        head + R"("subscription":1,"kind":"Data","place":"p","t_ns":2})",
        "agent.event.kind: must be 1 to 64");
    require_refused<message_type>(
        head + R"("subscription":1,"kind":"data","t_ns":2})",
        "agent.event.place: is required");
    require_refused<message_type>(
        head + R"("subscription":1,"kind":"data","place":"","t_ns":2})",
        "agent.event.place: must not be empty");
    require_refused<message_type>(
        head + R"("subscription":1,"kind":"data","place":"p"})",
        "agent.event.t_ns: is required");
    require_refused<message_type>(
        head + R"("subscription":1,"kind":"data","place":"p","t_ns":-1})",
        "agent.event.t_ns: must be a whole number");
    require_refused<message_type>(R"({"op":"agent.event","id":1,)" + rest,
                                  "neither an id nor a re");
    require_refused<message_type>(R"({"op":"agent.event","re":1,)" + rest,
                                  "neither an id nor a re");
    // The bytes: none to 64 KiB.
    REQUIRE_EQ(
        accepted<message_type>(head + rest, bytes_of(max_agent_event_data))
            .data.size(),
        max_agent_event_data);
    require_refused<message_type>(head + rest, "has a blob of 65537 bytes",
                                  bytes_of(max_agent_event_data + 1));
    agent_event_message body;
    body.subscription = 1;
    body.kind = "k";
    body.place = "p";
    body.data = bytes_of(max_agent_event_data + 1);
    require_not_written(body);
    body.data.clear();
    body.kind = repeated('k', max_agent_event_kind + 1);
    require_not_written(body);
    body.kind = repeated('k', max_agent_event_kind);
    (void)round_trip(body);
    body.place = repeated('p', max_api_name + 1);
    require_not_written(body);
}

TEST_CASE("plugin api: no place of agent.subscribe is named twice")
{
    require_refused<agent_subscribe_message>(
        R"({"op":"agent.subscribe","id":1,"places":["a","b","a"]})",
        "agent.subscribe.places: names a place twice");
    agent_subscribe_message body;
    body.places = {"a", "a"};
    require_not_written(body, 1);
}
