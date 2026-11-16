//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the tests of the extension point messages share: a frame from a
// header, a typed message written and read back through the frame reader, and
// the check that a header is refused with `bad_request` and a line that names
// the member.
//

#pragma once

#include <vss/agent/proto/frame.hpp>
#include <vss/core/bytes.hpp>
#include <vss/plugin/api_message.hpp>

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vss::plugin_api_test
{

/// The message `header` and `blob` make, as `decode_message` reads it.
inline plugin::api_message message_of(std::string_view header,
                                      byte_buffer blob = {})
{
    agent::frame frame;
    frame.header = std::string(header);
    frame.blob = std::move(blob);
    std::string detail;
    const auto decoded = plugin::decode_message(frame, &detail);
    if (decoded.has_error())
        throw vss_test::failure("the header is not a message: " + detail);
    return decoded.value();
}

/// `body` written, carried through an encoded frame and read as a typed
/// message again.  The two must be equal.  `number` is the id of a request or
/// the re of an answer.
template <class M> M round_trip(const M& body, std::uint64_t number = 0)
{
    const auto written = plugin::encode_as<M>(body, number);
    REQUIRE(written.has_value());
    REQUIRE_EQ(written.value().op, std::string(M::op()));

    const auto frame = plugin::encode_message(written.value());
    REQUIRE(frame.has_value());
    const auto bytes = agent::encode_frame(frame.value());
    REQUIRE(bytes.has_value());
    agent::frame_reader reader;
    reader.feed(bytes.value());
    auto next = reader.next();
    REQUIRE(next.has_value());
    const std::optional<agent::frame>& ready = next.value();
    REQUIRE(ready.has_value());

    const auto decoded = plugin::decode_message(*ready);
    REQUIRE(decoded.has_value());
    std::string detail = "stale";
    const auto typed = plugin::decode_as<M>(decoded.value(), &detail);
    REQUIRE_EQ(detail, std::string{});
    REQUIRE(typed.has_value());
    REQUIRE(typed.value() == body);

    // Writing what was read gives the same bytes.
    const auto again = plugin::encode_as<M>(typed.value(), number);
    REQUIRE(again.has_value());
    REQUIRE(again.value() == written.value());
    return typed.value();
}

/// The typed message in `header`, which must be accepted.
template <class M> M accepted(std::string_view header, byte_buffer blob = {})
{
    std::string detail = "stale";
    const auto typed =
        plugin::decode_as<M>(message_of(header, std::move(blob)), &detail);
    if (typed.has_error())
        throw vss_test::failure("refused: " + detail + "\n  header: " +
                                std::string(header.substr(0, 300)));
    REQUIRE_EQ(detail, std::string{});
    return typed.value();
}

/// `header` is refused as `bad_request`, and the line for the log holds
/// `fragment`.
template <class M>
void require_refused(std::string_view header, std::string_view fragment,
                     byte_buffer blob = {})
{
    const plugin::api_message message = message_of(header, std::move(blob));
    std::string detail;
    const auto typed = plugin::decode_as<M>(message, &detail);
    REQUIRE(typed.has_error());
    REQUIRE(typed.error() == plugin::api_errc::bad_request);
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
    // The same answer when there is nowhere to put the detail.
    const auto again = plugin::decode_as<M>(message);
    REQUIRE(again.has_error());
    REQUIRE(again.error() == plugin::api_errc::bad_request);
}

/// `header` is refused, and the line for the log does not hold `text`: a
/// detail never repeats what the plugin sent.
template <class M>
void require_detail_without(std::string_view header, std::string_view text,
                            byte_buffer blob = {})
{
    const plugin::api_message message = message_of(header, std::move(blob));
    std::string detail;
    const auto typed = plugin::decode_as<M>(message, &detail);
    REQUIRE(typed.has_error());
    REQUIRE(!detail.empty());
    if (detail.find(text) != std::string::npos)
        throw vss_test::failure("the detail repeats " + std::string(text) +
                                ": " + detail);
}

/// `body` cannot be written: it would not read back.
template <class M>
void require_not_written(const M& body, std::uint64_t number = 0)
{
    const auto written = plugin::encode_as<M>(body, number);
    REQUIRE(written.has_error());
    REQUIRE(written.error() == plugin::api_errc::bad_request);
}

} // namespace vss::plugin_api_test
