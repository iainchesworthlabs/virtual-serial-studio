//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_instance.hpp>

#include "plugin_api_support.hpp"
#include "test_framework.hpp"

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

std::string header_of(const api_message& message)
{
    return std::get<other_message>(message.body).header;
}

} // namespace

TEST_CASE("plugin api: instance.create is written and read")
{
    instance_create_message body;
    body.instance = 7;
    body.kind = extension_kind::fixture;
    body.type = "modbus-slave";
    body.params = R"({"address":17,"delay":"3ms"})";
    const auto read = round_trip(body, 12);
    REQUIRE_EQ(read.params, std::string(R"({"address":17,"delay":"3ms"})"));

    const auto written = encode_as(body, 12);
    REQUIRE(written.has_value());
    REQUIRE_EQ(written.value().op, std::string("instance.create"));
    REQUIRE_EQ(written.value().id, std::uint64_t{12});
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"instance.create","id":12,"instance":7,)"
                           R"("kind":"fixture","type":"modbus-slave",)"
                           R"("params":{"address":17,"delay":"3ms"}})"));
}

TEST_CASE("plugin api: every kind of instance can be created")
{
    for (const extension_kind kind :
         {extension_kind::generator, extension_kind::fixture,
          extension_kind::modifier, extension_kind::adapter,
          extension_kind::decoder})
    {
        instance_create_message body;
        body.instance = 1;
        body.kind = kind;
        body.type = "t";
        REQUIRE(round_trip(body, 1).kind == kind);
    }
}

TEST_CASE("plugin api: instance.create without params has none")
{
    const auto body = accepted<instance_create_message>(
        R"({"op":"instance.create","id":1,"instance":2,"kind":"adapter",)"
        R"("type":"x"})");
    REQUIRE_EQ(body.params, std::string("{}"));
}

TEST_CASE("plugin api: instance.create is refused when it is wrong")
{
    using M = instance_create_message;
    const std::string head = R"({"op":"instance.create","id":1,)";
    require_refused<M>(head + R"("kind":"fixture","type":"x"})",
                       "instance.create.instance: is required");
    require_refused<M>(head + R"("instance":0,"kind":"fixture","type":"x"})",
                       "instance.create.instance: must be a whole number");
    require_refused<M>(head + R"("instance":"2","kind":"fixture","type":"x"})",
                       "instance.create.instance: must be a whole number");
    require_refused<M>(head + R"("instance":-1,"kind":"fixture","type":"x"})",
                       "instance.create.instance: must be a whole number");
    require_refused<M>(
        head + R"("instance":9007199254740992,"kind":"fixture","type":"x"})",
        "instance.create.instance: must be a whole number");
    require_refused<M>(head + R"("instance":2,"type":"x"})",
                       "instance.create.kind: is required");
    require_refused<M>(head + R"("instance":2,"kind":"daemon","type":"x"})",
                       "instance.create.kind: must be generator");
    require_refused<M>(head + R"("instance":2,"kind":5,"type":"x"})",
                       "instance.create.kind: must be generator");
    require_refused<M>(head + R"("instance":2,"kind":"fixture"})",
                       "instance.create.type: is required");
    require_refused<M>(head +
                           R"("instance":2,"kind":"fixture","type":"Upper"})",
                       "instance.create.type: must be 1 to 64");
    require_refused<M>(head + R"("instance":2,"kind":"fixture","type":""})",
                       "instance.create.type: must be 1 to 64");
    require_refused<M>(head + R"("instance":2,"kind":"fixture","type":7})",
                       "instance.create.type: must be a string");
    require_refused<M>(
        head + R"("instance":2,"kind":"fixture","type":"x","params":[]})",
        "instance.create.params: must be a JSON object");
    require_refused<M>(
        head + R"("instance":2,"kind":"fixture","type":"x","params":"{}"})",
        "instance.create.params: must be a JSON object");
    // A blob, and the wrong role.
    require_refused<M>(head + R"("instance":2,"kind":"fixture","type":"x"})",
                       "instance.create: has no bytes", {1});
    require_refused<M>(
        R"({"op":"instance.create","instance":2,"kind":"fixture","type":"x"})",
        "must have an id");
    require_refused<M>(
        R"({"op":"instance.create","re":4,"instance":2,"kind":"fixture",)"
        R"("type":"x"})",
        "must have an id and no re");
}

TEST_CASE("plugin api: the type and the number of an instance are limited")
{
    using M = instance_create_message;
    const std::string head =
        R"({"op":"instance.create","id":1,"instance":2,"kind":"fixture",)";
    const auto edge =
        accepted<M>(head + R"("type":")" + repeated('a', max_api_id) + R"("})");
    REQUIRE_EQ(edge.type.size(), max_api_id);
    require_refused<M>(head + R"("type":")" + repeated('a', max_api_id + 1) +
                           R"("})",
                       "instance.create.type: must be 1 to 64");

    instance_create_message body;
    body.instance = 2;
    body.type = repeated('a', max_api_id + 1);
    require_not_written(body, 1);
    body.type = "a";
    require_not_written(body, 0);
    body.instance = 0;
    require_not_written(body, 1);
    body.instance = max_api_number;
    (void)round_trip(body, max_api_number);
    body.instance = max_api_number + 1;
    require_not_written(body, 1);
    body.instance = 1;
    body.params = "[]";
    require_not_written(body, 1);
}

TEST_CASE("plugin api: params may nest as far as the header allows")
{
    using M = instance_create_message;
    const std::string head =
        R"({"op":"instance.create","id":1,"instance":2,"kind":"fixture",)"
        R"("type":"x","params":)";
    // The header is 1 deep, so params may hold 6 more levels.
    const std::string six = R"({"a":{"b":{"c":{"d":{"e":{}}}}}})";
    REQUIRE_EQ(accepted<M>(head + six + "}").params, six);
    // One more level than the header allows is not written.
    M body;
    body.instance = 2;
    body.type = "x";
    body.params = R"({"a":{"b":{"c":{"d":{"e":{"f":{"g":{}}}}}}}})";
    require_not_written(body, 1);
    body.params = R"({"a":{"b":{"c":{"d":{"e":{"f":{}}}}}}})";
    (void)round_trip(body, 1);
}

TEST_CASE("plugin api: instance.stop is written and read")
{
    instance_stop_message body;
    body.instance = 9;
    (void)round_trip(body, 3);
    const auto written = encode_as(body, 3);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"instance.stop","id":3,"instance":9})"));
    require_refused<instance_stop_message>(R"({"op":"instance.stop","id":3})",
                                           "instance.stop.instance: is");
    require_refused<instance_stop_message>(
        R"({"op":"instance.stop","id":3,"instance":1})", "has no bytes",
        {1, 2});
    require_refused<instance_stop_message>(
        R"({"op":"instance.stop","instance":1})", "must have an id");
    // A member it does not know is passed over.
    REQUIRE_EQ(accepted<instance_stop_message>(
                   R"({"op":"instance.stop","id":3,"instance":1,"x":[1]})")
                   .instance,
               std::uint64_t{1});
}

TEST_CASE("plugin api: params.changed is written and read")
{
    params_changed_message body;
    body.instance = 4;
    body.params = R"({"address":18})";
    (void)round_trip(body, 99);
    require_refused<params_changed_message>(
        R"({"op":"params.changed","id":1,"instance":4})",
        "params.changed.params: is required");
    require_refused<params_changed_message>(
        R"({"op":"params.changed","id":1,"instance":4,"params":"x"})",
        "params.changed.params: must be a JSON object");
    require_refused<params_changed_message>(
        R"({"op":"params.changed","id":1,"params":{}})",
        "params.changed.instance: is required");
}

TEST_CASE("plugin api: a plugin refuses a value with the common error")
{
    const api_message refusal =
        make_error(5, api_errc::refused, "The address must be odd.");
    const auto frame = encode_message(refusal);
    REQUIRE(frame.has_value());
    const auto read = decode_message(frame.value());
    REQUIRE(read.has_value());
    const auto& error = std::get<error_message>(read.value().body);
    REQUIRE_EQ(error.value, std::string("refused"));
    REQUIRE_EQ(error.message, std::string("The address must be odd."));
    REQUIRE_EQ(read.value().re, std::uint64_t{5});
}

TEST_CASE("plugin api: a typed message is not read as another")
{
    const api_message stop =
        message_of(R"({"op":"instance.stop","id":3,"instance":1})");
    const auto wrong = decode_as<params_changed_message>(stop);
    REQUIRE(wrong.has_error());
    REQUIRE(wrong.error() == api_errc::bad_request);
    // A body of protocol.hpp is not an extension message.
    const api_message ping = message_of(R"({"op":"ping","id":3})");
    REQUIRE(decode_as<instance_stop_message>(ping).has_error());
    // Nor is a request that has lost its id.
    api_message loose = stop;
    loose.id = 0;
    REQUIRE(decode_as<instance_stop_message>(loose).has_error());
}
