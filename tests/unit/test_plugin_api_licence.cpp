//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_licence.hpp>

#include "plugin_api_support.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace vss;
using namespace vss::plugin;
using namespace vss::plugin_api_test;

namespace
{

std::string header_of(const api_message& message)
{
    return std::get<other_message>(message.body).header;
}

byte_buffer file_of(std::size_t count)
{
    return byte_buffer(count, std::uint8_t{0x7b});
}

} // namespace

TEST_CASE("plugin api: licence.get is a request with no members")
{
    const licence_get_message body;
    (void)round_trip(body, 3);
    const auto written = encode_as(body, 3);
    REQUIRE(written.has_value());
    REQUIRE_EQ(written.value().op, std::string("licence.get"));
    REQUIRE_EQ(written.value().id, std::uint64_t{3});
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"licence.get","id":3})"));
    REQUIRE(licence_get_message::from() == api_side::plugin);
    REQUIRE(licence_get_message::role() == api_role::request);
}

TEST_CASE("plugin api: licence.get is refused when it is wrong")
{
    using message_type = licence_get_message;
    (void)accepted<message_type>(R"({"op":"licence.get","id":1})");
    // A member that a newer version added is left alone.
    (void)accepted<message_type>(R"({"op":"licence.get","id":1,"later":1})");
    require_refused<message_type>(R"({"op":"licence.get"})", "must have an id");
    require_refused<message_type>(R"({"op":"licence.get","re":1})",
                                  "must have an id");
    require_refused<message_type>(R"({"op":"licence.get","id":1})",
                                  "has no bytes", file_of(1));
    require_refused<message_type>(R"({"op":"licence.report","id":1})",
                                  "is not the message in this frame");
    require_not_written(licence_get_message{}, 0);
}

TEST_CASE("plugin api: licence.file carries the bytes of a file")
{
    licence_file_message body;
    body.present = true;
    body.data = file_of(300);
    const auto read = round_trip(body, 5);
    REQUIRE(read.present);
    REQUIRE_EQ(read.data.size(), std::size_t{300});
    const auto written = encode_as(body, 5);
    REQUIRE(written.has_value());
    REQUIRE_EQ(written.value().re, std::uint64_t{5});
    REQUIRE_EQ(written.value().blob.size(), std::size_t{300});
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"licence.file","re":5,"present":true})"));
    REQUIRE(licence_file_message::from() == api_side::host);
    REQUIRE(licence_file_message::role() == api_role::answer);

    // No file at all.
    const licence_file_message none;
    const auto absent = round_trip(none, 5);
    REQUIRE(!absent.present);
    REQUIRE(absent.data.empty());
    const auto text = encode_as(none, 5);
    REQUIRE(text.has_value());
    REQUIRE_EQ(header_of(text.value()),
               std::string(R"({"op":"licence.file","re":5,"present":false})"));
}

TEST_CASE("plugin api: licence.file has the limits of a licence file")
{
    using message_type = licence_file_message;
    const std::string present =
        R"({"op":"licence.file","re":2,"present":true})";
    const std::string absent =
        R"({"op":"licence.file","re":2,"present":false})";
    REQUIRE_EQ(accepted<message_type>(present, file_of(1)).data.size(),
               std::size_t{1});
    REQUIRE_EQ(
        accepted<message_type>(present, file_of(max_licence_file)).data.size(),
        max_licence_file);
    require_refused<message_type>(present, "has a blob of 0 bytes");
    require_refused<message_type>(present, "has a blob of 65537 bytes",
                                  file_of(max_licence_file + 1));
    (void)accepted<message_type>(absent);
    require_refused<message_type>(absent, "has no licence file", file_of(1));
    require_refused<message_type>(R"({"op":"licence.file","re":2})",
                                  "licence.file.present: is required");
    require_refused<message_type>(R"({"op":"licence.file","re":2,"present":1})",
                                  "licence.file.present");
    require_refused<message_type>(
        R"({"op":"licence.file","re":2,"present":"yes"})",
        "licence.file.present");
    require_refused<message_type>(
        R"({"op":"licence.file","id":2,"present":false})", "must have a re");

    message_type body;
    body.present = true;
    require_not_written(body, 2);
    body.data = file_of(max_licence_file + 1);
    require_not_written(body, 2);
    body.present = false;
    body.data = file_of(1);
    require_not_written(body, 2);
    body.data.clear();
    (void)round_trip(body, 2);
}

TEST_CASE("plugin api: licence.report names a reason and a licence")
{
    licence_report_message body;
    body.reason = license::reason::expired;
    body.licence_id = "VSS-2026-000123";
    const auto read = round_trip(body);
    REQUIRE(read.reason == license::reason::expired);
    REQUIRE_EQ(read.licence_id, std::string("VSS-2026-000123"));
    const auto written = encode_as(body);
    REQUIRE(written.has_value());
    REQUIRE_EQ(header_of(written.value()),
               std::string(R"({"op":"licence.report","reason":"expired",)"
                           R"("licence_id":"VSS-2026-000123"})"));
    REQUIRE(licence_report_message::from() == api_side::plugin);
    REQUIRE(licence_report_message::role() == api_role::notification);

    // No licence number is none written.
    const licence_report_message bare;
    const auto without = encode_as(bare);
    REQUIRE(without.has_value());
    REQUIRE_EQ(header_of(without.value()),
               std::string(R"({"op":"licence.report","reason":"ok"})"));
    REQUIRE_EQ(std::string(licence_file_capability),
               std::string("licence.file"));
}

TEST_CASE("plugin api: every reason of the licence library has a name")
{
    using license::reason;
    const std::pair<reason, std::string_view> names[] = {
        {reason::ok, "ok"},
        {reason::no_licence, "no_licence"},
        {reason::expired, "expired"},
        {reason::not_yet_valid, "not_yet_valid"},
        {reason::clock_suspect, "clock_suspect"},
        {reason::malformed, "malformed"},
        {reason::newer_version, "newer_version"},
        {reason::bad_signature, "bad_signature"},
        {reason::revoked, "revoked"},
        {reason::wrong_computer, "wrong_computer"},
        {reason::not_in_licence, "not_in_licence"}};
    for (const auto& [value, name] : names)
    {
        licence_report_message body;
        body.reason = value;
        REQUIRE(round_trip(body).reason == value);
        REQUIRE_EQ(std::string(to_string(value)), std::string(name));
    }
}

TEST_CASE("plugin api: licence.report is refused when it is wrong")
{
    using message_type = licence_report_message;
    const std::string head = R"({"op":"licence.report")";
    require_refused<message_type>(head + "}",
                                  "licence.report.reason: is required");
    require_refused<message_type>(head + R"(,"reason":"fine"})",
                                  "licence.report.reason: must be");
    require_refused<message_type>(head + R"(,"reason":1})",
                                  "licence.report.reason");
    require_refused<message_type>(head + R"(,"reason":"Expired"})",
                                  "licence.report.reason");
    require_refused<message_type>(
        R"({"op":"licence.report","id":1,"reason":"ok"})",
        "has neither an id nor a re");
    require_refused<message_type>(head + R"(,"reason":"ok"})", "has no bytes",
                                  file_of(1));
    const std::string with_id = head + R"(,"reason":"ok","licence_id":")";
    const std::string longest(max_licence_id, 'a');
    REQUIRE_EQ(accepted<message_type>(with_id + longest + R"("})").licence_id,
               longest);
    REQUIRE_EQ(accepted<message_type>(with_id + R"(a"})").licence_id,
               std::string("a"));
    require_refused<message_type>(with_id + longest + R"(a"})",
                                  "licence.report.licence_id: is longer than");
    require_refused<message_type>(
        with_id + R"("})", "licence.report.licence_id: must not be empty");
    require_refused<message_type>(
        with_id + R"(a\u0007"})",
        "licence.report.licence_id: must be plain text");
    require_refused<message_type>(head + R"(,"reason":"ok","licence_id":5})",
                                  "licence.report.licence_id");
    require_refused<message_type>(head + R"(,"reason":"ok","licence_id":null})",
                                  "licence.report.licence_id");

    message_type body;
    body.licence_id = std::string(max_licence_id + 1, 'a');
    require_not_written(body);
    body.licence_id = "VSS-2026-000123";
    require_not_written(body, 4);
    (void)round_trip(body);
}

TEST_CASE("plugin api: a refusal of licence.report does not repeat the id")
{
    require_detail_without<licence_report_message>(
        R"({"op":"licence.report","reason":"ok","licence_id":"private-text\u0007"})",
        "private-text");
}
