//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz harness for the messages of the plugin API. Any bytes may arrive from
// a plugin, so the reader must refuse bad input with an error and a reason,
// and never crash. The input is read as the bytes of a connection, cut into
// frames by the agent's frame reader, and each frame is decoded. What
// decodes must be inside every limit the header promises, must encode again,
// and the frame made from it must decode to the same message and encode to
// the same bytes. The input is also split into a header and a blob and
// decoded directly, and used whole as a header, because a mutated length
// prefix would otherwise keep most inputs from reaching the JSON reader.
//

#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{

namespace agent = vss::agent;
namespace plugin = vss::plugin;

/// A property did not hold: stop, so the fuzzer keeps the input.
void require(bool holds)
{
    if (!holds)
    {
        std::abort();
    }
}

// -- what an accepted message promises ---------------------------------------

/// A text that was accepted holds no control character.
void require_plain(std::string_view text, std::size_t most)
{
    require(text.size() <= most);
    for (const char c : text)
    {
        const auto byte = static_cast<std::uint8_t>(c);
        require(byte >= 0x20 && byte != 0x7f);
    }
}

void require_token(std::string_view text, std::size_t most)
{
    require(!text.empty());
    require(text.size() <= most);
    for (const char c : text)
    {
        require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '.' || c == '-');
    }
}

void require_body(const plugin::hello_message& body)
{
    require(body.plugin_id.size() >= 3);
    require(body.plugin_id.size() <= plugin::max_id_length);
    require_token(body.plugin_id, plugin::max_id_length);
    require(!body.version.empty());
    require_plain(body.version, 64);
    require(body.pid <= plugin::max_message_id);
}

void require_body(const plugin::hello_ack_message& body)
{
    require(body.granted.size() <= plugin::max_message_list);
    for (std::size_t i = 0; i < body.granted.size(); ++i)
    {
        require(plugin::parse_permission(plugin::to_string(body.granted[i])) ==
                body.granted[i]);
        for (std::size_t j = 0; j < i; ++j)
        {
            require(body.granted[i] != body.granted[j]);
        }
    }
    require(body.capabilities.size() <= plugin::max_message_list);
    for (std::size_t i = 0; i < body.capabilities.size(); ++i)
    {
        require_token(body.capabilities[i], 64);
        for (std::size_t j = 0; j < i; ++j)
        {
            require(body.capabilities[i] != body.capabilities[j]);
        }
    }
    require(body.limits.frames_per_second >= 1);
    require(body.limits.frames_burst >= 1);
    require(body.limits.bytes_per_second >= 1);
    require(body.limits.outstanding_requests >= 1);
    require(body.limits.log_lines_per_second >= 1);
    require(body.limits.instances >= 1);
    require_plain(body.data_directory, plugin::max_message_text);
    require(body.settings.size() >= 2);
    require(body.settings.front() == '{');
    require(body.settings.back() == '}');
}

void require_body(const plugin::status_message& body)
{
    require_plain(body.text, plugin::max_status_text);
    require(body.code.size() <= 64);
    if (!body.code.empty())
    {
        require_token(body.code, 64);
    }
}

void require_body(const plugin::log_message& body)
{
    require_plain(body.text, plugin::max_message_text);
}

void require_body(const plugin::shutdown_message& body)
{
    require_plain(body.reason, 64);
}

void require_body(const plugin::error_message& body)
{
    require_token(body.category, 64);
    require_token(body.value, 64);
    require_plain(body.message, plugin::max_message_text);
}

void require_body(const plugin::other_message& body)
{
    require(!body.header.empty());
}

template <class Empty>
void require_body(const Empty&)
{
}

/// What `decode_message` promises about a message it accepts.
void require_inside_limits(const plugin::api_message& message,
                           const agent::frame& frame)
{
    require(plugin::is_op_name(message.op));
    require(message.id <= plugin::max_message_id);
    require(message.re <= plugin::max_message_id);
    require(message.id == 0 || message.re == 0);
    require(message.blob == frame.blob);
    const std::string_view name = plugin::op_of(message.body);
    if (const auto* const other =
            std::get_if<plugin::other_message>(&message.body))
    {
        require(name.empty());
        require(other->header == frame.header);
    }
    else
    {
        require(name == message.op);
    }
    std::visit([](const auto& body) { require_body(body); }, message.body);
}

// -- the frames of a stream --------------------------------------------------

/// The frame as a connection carries it: encoded and read back.  Nothing
/// when it is too large to send, which is the one thing that may stop the
/// frame encoder.
std::optional<agent::frame> carried(const agent::frame& value)
{
    const auto bytes = agent::encode_frame(value);
    if (!bytes)
    {
        require(bytes.error() ==
                agent::make_error_code(agent::protocol_errc::oversized_frame));
        return std::nullopt;
    }
    agent::frame_reader reader;
    reader.feed(*bytes);
    auto next = reader.next();
    require(next.has_value());
    std::optional<agent::frame> ready = std::move(*next);
    if (!ready || !(*ready == value))
    {
        std::abort();
    }
    require(reader.buffered() == 0);
    return ready;
}

/// A message that was read is written, and what is written reads back as
/// the same message and is written as the same bytes.
void check_accepted(const plugin::api_message& first,
                    const agent::frame& frame)
{
    require_inside_limits(first, frame);

    const auto second = plugin::decode_message(frame);
    require(second.has_value());
    require(*second == first);

    const auto encoded = plugin::encode_message(first);
    require(encoded.has_value());
    require(encoded->blob == first.blob);
    if (std::holds_alternative<plugin::other_message>(first.body))
    {
        require(encoded->header == frame.header);
    }

    std::string detail;
    const auto again = plugin::decode_message(*encoded, &detail);
    require(again.has_value());
    require(detail.empty());
    require(*again == first);

    // The same bytes a second time, and with the op left out.
    const auto repeated = plugin::encode_message(*again);
    require(repeated.has_value());
    require(*repeated == *encoded);
    plugin::api_message nameless = first;
    if (!std::holds_alternative<plugin::other_message>(first.body))
    {
        nameless.op.clear();
    }
    const auto from_body = plugin::encode_message(nameless);
    require(from_body.has_value());
    require(*from_body == *encoded);

    // And over a connection.
    const std::optional<agent::frame> sent = carried(*encoded);
    if (sent)
    {
        const auto last = plugin::decode_message(*sent);
        require(last.has_value());
        require(*last == first);
    }
}

void check_frame(const agent::frame& frame)
{
    std::string detail = "left over";
    const auto first = plugin::decode_message(frame, &detail);
    if (first.has_value())
    {
        require(detail.empty());
        check_accepted(*first, frame);
        return;
    }
    require(first.error() == plugin::api_errc::bad_request);
    require(!detail.empty());
    for (const char c : detail)
    {
        // A line for a log: printable ASCII only.
        require(c >= 0x20 && c <= 0x7e);
    }
    const auto second = plugin::decode_message(frame);
    require(second.has_error());
    require(second.error() == first.error());
}

/// The input as one frame: the first byte says how much of the rest is the
/// header.
agent::frame direct_frame(vss::bytes_view input)
{
    agent::frame out;
    if (input.empty())
    {
        return out;
    }
    const vss::bytes_view rest = input.subspan(1);
    const std::size_t header_size = rest.size() * input[0] / 255;
    out.header.assign(reinterpret_cast<const char*>(rest.data()), header_size);
    out.blob.assign(rest.begin() + static_cast<std::ptrdiff_t>(header_size),
                    rest.end());
    return out;
}

} // namespace

// libFuzzer looks for exactly this name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    const vss::bytes_view input(data, size);

    // The bytes of a connection.  A reader stops at its first error.
    agent::frame_reader reader;
    reader.feed(input);
    for (;;)
    {
        auto next = reader.next();
        if (!next)
        {
            break;
        }
        std::optional<agent::frame> ready = std::move(*next);
        if (!ready)
        {
            break;
        }
        check_frame(*ready);
    }

    check_frame(direct_frame(input));

    agent::frame whole;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    whole.header.assign(reinterpret_cast<const char*>(data), size);
    check_frame(whole);
    return 0;
}
