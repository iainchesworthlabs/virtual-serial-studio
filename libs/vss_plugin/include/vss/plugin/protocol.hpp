//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages a plugin and its host send each other: the part of the plugin
// API that every plugin uses, whatever it adds.  docs/plugins.md, sections
// 7.1 to 7.3, are the design.  The plugin API is public: a plugin may be
// written in any language against what this file describes.
//
// A message is one frame.  The frame is the one the agent uses
// (`vss::agent::frame`): a JSON object, the header, and a blob of bytes.
// Bytes of traffic travel in the blob and never in JSON.  The header of a
// message of this API has:
//
//   "op"   the name of the message, such as "hello" or "generator.start"
//   "id"   a number chosen by the sender, when it wants an answer.  Never 0
//   "re"   in an answer: the "id" it answers
//
// and the members the message itself has.  An answer is a message with
// "re": "ok" when the request was carried out, "error" when it was not, or
// a message of its own where the API names one ("hello_ack" answers
// "hello", "pong" answers "ping", "bye" answers "shutdown").  A message
// with neither "id" nor "re" is a notification and gets no answer.
//
// Reading is strict about the three members above and about the members of
// the messages this file knows, and it ignores members it does not know:
// a newer plugin or host may add optional ones (a minor version).  An "op"
// this file does not know is not an error here.  It is returned as an
// `other_message` with its header text, for the extension point it belongs
// to, or for the host to answer with `unknown_op`.
//
// Everything from a plugin is untrusted input.  Nothing here does I/O,
// keeps state or trusts a size before it has checked it.
//

#pragma once

#include <vss/agent/proto/frame.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>
#include <vss/plugin/manifest.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vss::plugin
{

// -- limits ------------------------------------------------------------------

/// The version of the plugin API this code speaks.
inline constexpr api_version plugin_api_version{1, 0};

/// The most bytes a header may have.  The bytes of traffic are in the blob,
/// so a header is small; one over this is refused before it is read.
inline constexpr std::size_t max_header_size = std::size_t{64} * 1024;
/// The longest "op", in bytes.
inline constexpr std::size_t max_op_length = 64;
/// The longest text in a message of this file, in bytes.
inline constexpr std::size_t max_message_text = 1024;
/// The longest status text, in bytes.
inline constexpr std::size_t max_status_text = 200;
/// The most entries in a list of a message of this file.
inline constexpr std::size_t max_message_list = 256;
/// How deep the JSON of a header may nest.
inline constexpr std::size_t max_header_depth = 8;
/// The largest "id" and "re": every whole number up to it is exact in any
/// JSON reader.
inline constexpr std::uint64_t max_message_id = (std::uint64_t{1} << 53) - 1;

// -- errors ------------------------------------------------------------------

/// Why a request of the plugin API was not carried out.  These travel as
/// the "value" of an "error" message with the category "vss.plugin.api".
/// A value never changes its name once a release has carried it.
enum class api_errc
{
    success = 0,
    /// The "op" is not one the receiver knows.
    unknown_op,
    /// The message is not as the API describes it.
    bad_request,
    /// The receiver will not do it.
    refused,
    /// The plugin was not granted the permission the operation needs.
    not_granted,
    /// The plugin's manifest does not declare what the operation uses.
    not_declared,
    /// The answer did not come in time.
    timeout,
    /// Too many requests are waiting for an answer.
    busy,
    /// This version does not have the operation or the option.
    unsupported,
    /// The plugin is not running.
    plugin_unavailable
};

const boost::system::error_category& api_category() noexcept;
boost::system::error_code make_error_code(api_errc value) noexcept;

/// The name as it is written in an "error" message, such as `unknown_op`.
std::string_view to_string(api_errc value) noexcept;

// -- the messages ------------------------------------------------------------

/// The first message, from the plugin.  It is a request: it has an "id",
/// and "hello_ack" answers it.
struct hello_message
{
    /// The version of the plugin API the plugin speaks.
    api_version api;
    /// The id in the plugin's manifest.  The channel is the plugin's
    /// identity; this is checked against it and proves nothing by itself.
    std::string plugin_id;
    /// The plugin's version, as its manifest writes it.
    std::string version;
    /// The plugin's process id, for the log.
    std::uint64_t pid = 0;

    friend bool operator==(const hello_message&,
                           const hello_message&) = default;
};

/// What the host allows this plugin each second, and at once.
struct api_limits
{
    std::uint32_t frames_per_second = 2000;
    std::uint32_t frames_burst = 4000;
    std::uint32_t bytes_per_second = 8u * 1024u * 1024u;
    std::uint32_t outstanding_requests = 32;
    std::uint32_t log_lines_per_second = 100;
    std::uint32_t instances = 16;

    friend bool operator==(const api_limits&, const api_limits&) = default;
};

/// The host's answer to "hello".  After it the plugin is ready.
struct hello_ack_message
{
    /// The version both sides use from here on.
    api_version api;
    /// The permissions the user granted, by the names of the manifest.
    std::vector<permission> granted;
    /// The engine capabilities of this build, by name.
    std::vector<std::string> capabilities;
    api_limits limits;
    /// The folder the plugin may keep its own files in.  UTF-8.
    std::string data_directory;
    /// The plugin's own settings as one JSON object, already checked
    /// against its manifest.  "{}" when it has none.
    std::string settings = "{}";

    friend bool operator==(const hello_ack_message&,
                           const hello_ack_message&) = default;
};

/// Asks whether the other side is alive.  A request; "pong" answers it.
struct ping_message
{
    friend bool operator==(const ping_message&, const ping_message&) = default;
};

struct pong_message
{
    friend bool operator==(const pong_message&, const pong_message&) = default;
};

enum class status_level { ok, warning, error };

std::string_view to_string(status_level value) noexcept;

/// A plugin says how it is doing.  A notification.  It is shown to the
/// user as plain text and never changes what the host does.
struct status_message
{
    status_level level = status_level::ok;
    /// At most `max_status_text` bytes.  Plain text.
    std::string text;
    /// One of the codes the plugin's manifest declares, or empty.
    std::string code;

    friend bool operator==(const status_message&,
                           const status_message&) = default;
};

enum class log_level { debug, info, warning, error };

std::string_view to_string(log_level value) noexcept;

/// A line for the plugin's log.  A notification.
struct log_message
{
    log_level level = log_level::info;
    /// At most `max_message_text` bytes.  Plain text.
    std::string text;

    friend bool operator==(const log_message&, const log_message&) = default;
};

/// The host asks the plugin to stop.  A request; "bye" answers it, and the
/// plugin then exits.
struct shutdown_message
{
    /// Why, for the plugin's log: `disabled`, `quit`, `policy`, `update`
    /// or another short word.
    std::string reason;

    friend bool operator==(const shutdown_message&,
                           const shutdown_message&) = default;
};

struct bye_message
{
    friend bool operator==(const bye_message&, const bye_message&) = default;
};

/// The request was carried out and has nothing more to say.
struct ok_message
{
    friend bool operator==(const ok_message&, const ok_message&) = default;
};

/// The request was not carried out.
struct error_message
{
    /// `vss.plugin.api` for the values of `api_errc`, `vss` for an error of
    /// the engine that is passed on, or another category by name.
    std::string category;
    /// The name of the value in its category, such as `unknown_op`.
    std::string value;
    /// One line for a person.  Plain text.
    std::string message;

    friend bool operator==(const error_message&,
                           const error_message&) = default;
};

/// A message whose "op" this file does not know.  It belongs to an
/// extension point, or to a newer version.
struct other_message
{
    /// The header as it arrived: one JSON object within the limits of
    /// nesting and size, with a valid "op" and, when present, valid "id"
    /// and "re".  Nothing else in it was looked at.
    std::string header;

    friend bool operator==(const other_message&,
                           const other_message&) = default;
};

using message_body =
    std::variant<hello_message, hello_ack_message, ping_message, pong_message,
                 status_message, log_message, shutdown_message, bye_message,
                 ok_message, error_message, other_message>;

struct api_message
{
    /// The name of the message.  For the bodies this file knows it follows
    /// from the body; for `other_message` it is the name that arrived.
    std::string op;
    /// The sender wants an answer with this number.  0 for none.
    std::uint64_t id = 0;
    /// This answers the request with this number.  0 for none.
    std::uint64_t re = 0;
    message_body body;
    /// The bytes that came with the message.
    byte_buffer blob;

    friend bool operator==(const api_message&, const api_message&) = default;
};

/// Whether `text` may be an "op": 1 to `max_op_length` lower-case ASCII
/// letters, digits, underscores and dots, starting with a letter, with no
/// dot at either end and no two dots together.
bool is_op_name(std::string_view text) noexcept;

/// The name of the message a body is sent under, such as `hello_ack`.
/// Empty for `other_message`, whose name is in `api_message::op`.
std::string_view op_of(const message_body& body) noexcept;

/// Reads one frame as a message.
///
/// `api_errc::bad_request` when the header has more than
/// `max_header_size` bytes, is not one JSON object within
/// `max_header_depth`, has a key twice, has no valid "op", has an "id" or
/// a "re" that is not a whole number from 1 to `max_message_id`, has both,
/// or when a message this file knows lacks a member it needs, has one of
/// the wrong type, or has a text or a list over its limit.  A member this
/// file does not know is passed over.
///
/// `detail`, when given, receives one line for the log.  It names the
/// member and never repeats what the plugin sent.
result<api_message> decode_message(const agent::frame& frame,
                                   std::string* detail = nullptr);

/// Writes a message as a frame.
///
/// `api_errc::bad_request` when the message could not be read back: an
/// "op" that is not a name, an `other_message` whose header is not a JSON
/// object or whose "op" differs from `op`, both "id" and "re", a number
/// over `max_message_id`, or a text or list over its limit.  The frame's
/// own size limit is the agent's.
result<agent::frame> encode_message(const api_message& message);

/// The message with its "id" and "re" set to these.  For a message this
/// file knows that is the two fields of `api_message`.  For an
/// `other_message` the header text carries them too, so they are written
/// into it, in place of any it had; the rest of the header is kept, in
/// compact form.  `api_errc::bad_request` when the header of an
/// `other_message` is not one JSON object.
result<api_message> with_ids(api_message message, std::uint64_t id,
                             std::uint64_t re);

/// An "error" answer to the request `re`, for an `api_errc` value.
api_message make_error(std::uint64_t re, api_errc value,
                       std::string_view message);

} // namespace vss::plugin

namespace boost::system
{
template <>
struct is_error_code_enum<vss::plugin::api_errc> : std::true_type
{
};
} // namespace boost::system
