//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the messages of the extension points of the plugin API share.
// docs/plugins.md, section 7, is the design, and docs/plugin-api-reference.md
// lists every message.  protocol.hpp has the messages every plugin uses; the
// api_*.hpp headers have one struct for each message of an extension point.
//
// A struct names its message (`op`), who sends it (`from`) and what kind of
// message it is (`role`).  `decode_as<M>` reads a typed message from an
// `api_message` that `decode_message` returned as an `other_message`, and
// `encode_as` writes one.  A message has the rules of protocol.hpp, and more:
//
//   - A request has an "id" and no "re".  An answer has a "re" and no "id".
//     A notification has neither.  A message that is not as its role says is
//     refused, so that no one waits for an answer that will not come.
//   - A member the struct knows is read strictly: its type, its range, its
//     length and its text.  A member it does not know is passed over, as a
//     newer minor version may add one (docs/plugins.md, section 7.2).
//   - The bytes of traffic are in the blob and never in JSON.  A message with
//     bytes has them in `data`; a message without has no blob.
//   - Nothing is allocated for a size the header only claims.  A list or a
//     text is counted before it is copied, and a blob is the frame's own.
//
// Nothing here does I/O, keeps state or trusts a size before it has checked it.
//

#pragma once

#include <vss/plugin/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace vss::plugin
{

/// Which side sends a message.
enum class api_side
{
    host,
    plugin
};

std::string_view to_string(api_side value) noexcept;

/// What a message is, which says what its "id" and "re" must be.
enum class api_role
{
    /// Has an "id", and the receiver answers it with "ok", an "error", or a
    /// message of its own.
    request,
    /// Has neither "id" nor "re".
    notification,
    /// Has a "re": it answers a request.
    answer
};

std::string_view to_string(api_role value) noexcept;

/// The largest number of bytes in a blob that any message of an extension
/// point carries.  The frame's own limit is 1 MiB; this is lower, so that a
/// frame of the plugin API is never close to it.
inline constexpr std::size_t max_api_blob = std::size_t{256} * 1024;

/// The largest number an instance, a port, a link, a sequence or a time
/// can be: every whole number up to it is exact in any JSON reader.
inline constexpr std::uint64_t max_api_number = max_message_id;

/// The longest name of a place, a section, a rule, a kind or another name that
/// a message gives to a thing, in bytes.
inline constexpr std::size_t max_api_name = 128;

/// The longest text of an extension point message that is for a person.
inline constexpr std::size_t max_api_text = 200;

/// The most a delay may be: one minute, as `max_link_delay_ms` is.
inline constexpr std::uint64_t max_api_delay_us = 60'000'000;

/// Reads the typed message `M` from `message`.
///
/// `api_errc::bad_request` when the message is not one of the plugin API's
/// own bodies (it is not an `other_message`), its "op" is not `M::op()`, its
/// "id" and "re" are not what `M::role()` needs, or a member is missing, has
/// the wrong type, is out of range, is too long or is not plain text.
/// `detail`, when given, receives one line for the log; it names the member
/// and never repeats what the plugin sent.
///
/// Defined for each message struct of the api_*.hpp headers.
template <class M>
result<M> decode_as(const api_message& message, std::string* detail = nullptr);

/// Writes `body` as a message.  `number` is the "id" of a request, the "re"
/// of an answer, and 0 for a notification.
///
/// `api_errc::bad_request` when what is written would not read back: a value
/// out of range, a text or list over its limit, a number that does not
/// match the role.
///
/// Defined for each message struct of the api_*.hpp headers.
template <class M>
result<api_message> encode_as(const M& body, std::uint64_t number = 0);

/// What the host and the tools know about one message of an extension point.
struct api_op
{
    std::string_view op;
    api_side from = api_side::host;
    api_role role = api_role::notification;
    /// Reads `message` as this op's struct and reports why it cannot.
    boost::system::error_code (*check)(const api_message& message,
                                       std::string* detail) = nullptr;
    /// Reads `message` and writes what was read.  The result has the same
    /// meaning as `message` and is in the compact form this library writes.
    result<api_message> (*recode)(const api_message& message) = nullptr;
};

/// Every message of the extension points that this library defines, in the
/// order of docs/plugin-api-reference.md.
std::span<const api_op> api_ops() noexcept;

/// The entry for `op`, or null when this library does not define it.
const api_op* find_api_op(std::string_view op) noexcept;

} // namespace vss::plugin
