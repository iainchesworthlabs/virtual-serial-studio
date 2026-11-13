//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages of the control API, version 1 (docs/remote-control.md,
// section 9).  Unlike the agent's protocol this one is a public contract:
// within a major version a message only gains optional fields, so a decoder
// here ignores a field it does not know.
//
// A message travels in the agent's frame (a length, a JSON header, a blob for
// bytes):
//
//     client   {"id": 7, "op": "ports.list"}
//     gateway  {"id": 7, "reply": "ports", "revision": 3, "ports": [...]}
//     gateway  {"id": 7, "reply": "error",
//               "error": {"code": "forbidden", "message": "not allowed",
//                         "retryable": false}}
//

#pragma once

#include <vss/agent/proto/frame.hpp>
#include <vss/control/proto/error.hpp>
#include <vss/control/proto/policy.hpp>
#include <vss/control/proto/secret.hpp>
#include <vss/core/error.hpp>

#include <vss/core/bytes.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vss::control
{

using agent::frame;

/// The major version of the API.  A `hello` that asks for another one is
/// refused.
inline constexpr std::uint32_t api_version = 1;

// -- limits ------------------------------------------------------------------

/// The longest token a `hello` may carry, in bytes.  A real one is shorter.
inline constexpr std::size_t max_token_text_size = 128;
/// The most bytes a frame may have before the connection has said `hello`.
inline constexpr std::size_t max_hello_frame_size = 4096;
/// The longest name, path or other text in a message, in bytes.
inline constexpr std::size_t max_text_size = 1024;
/// The most ports, links, link members, scopes or features in a message.
inline constexpr std::size_t max_list_size = 4096;
/// How deep the JSON of a header may nest.
inline constexpr std::size_t max_json_depth = 8;
/// The most ports a subscription may name.
inline constexpr std::size_t max_port_filter_size = 64;
/// The shortest, the longest and the usual time between two batches of a
/// stream, in milliseconds.  The gateway brings a time outside the range to
/// its nearest end and says in its reply what it took.
inline constexpr std::uint32_t min_stream_interval_ms = 10;
inline constexpr std::uint32_t max_stream_interval_ms = 10'000;
inline constexpr std::uint32_t default_stream_interval_ms = 250;

// -- requests ----------------------------------------------------------------

/// The first request of every connection.
struct hello_request
{
    std::uint32_t api = api_version;
    /// The program that connects, for the audit log (`pytest 8.3`).
    std::string client;
    /// A token, for a connection that wants to be the token and not the
    /// local user.  Empty for none.
    secret_string token{};

    friend bool operator==(const hello_request&,
                           const hello_request&) = default;
};

/// Asks what the gateway is and whether the agent behind it runs.
struct status_request
{
    friend bool operator==(const status_request&,
                           const status_request&) = default;
};

/// Asks for the ports and the links.
struct ports_list_request
{
    friend bool operator==(const ports_list_request&,
                           const ports_list_request&) = default;
};

/// Asks the gateway to close every session and exit.  The ports are the
/// agent's and stay.
struct endpoint_stop_request
{
    friend bool operator==(const endpoint_stop_request&,
                           const endpoint_stop_request&) = default;
};

/// Asks for a new token.  Only the administration endpoint may.
struct tokens_create_request
{
    /// What the person calls it: "Lab rig 3".
    std::string label;
    /// The names of the scopes it holds.  `admin` is refused.
    std::vector<std::string> scopes;
    /// How long it lives, in seconds; 0 for the usual 30 days.
    std::uint64_t life_s = 0;

    friend bool operator==(const tokens_create_request&,
                           const tokens_create_request&) = default;
};

struct tokens_list_request
{
    friend bool operator==(const tokens_list_request&,
                           const tokens_list_request&) = default;
};

/// Revokes the token with this id and closes its sessions.
struct tokens_revoke_request
{
    std::string id;

    friend bool operator==(const tokens_revoke_request&,
                           const tokens_revoke_request&) = default;
};

/// What a subscription asks for.
struct stream_options
{
    /// The time between two batches.  One connection has one stream, so the
    /// last subscription says the time for both kinds.
    std::uint32_t interval_ms = default_stream_interval_ms;
    /// The ids of the ports to hear about (`p_3`); empty for every port.
    /// With ports named, what is about no port is left out.
    std::vector<std::string> ports;

    friend bool operator==(const stream_options&,
                           const stream_options&) = default;
};

/// Starts the stream of what happens to the ports: states, control lines,
/// errors, and that bytes moved and how many.  No bytes.  Asking again
/// changes the ports and the time.
struct events_subscribe_request
{
    stream_options options;

    friend bool operator==(const events_subscribe_request&,
                           const events_subscribe_request&) = default;
};

struct events_unsubscribe_request
{
    friend bool operator==(const events_unsubscribe_request&,
                           const events_unsubscribe_request&) = default;
};

/// Starts the stream of the bytes themselves: a read-only tap.
struct traffic_subscribe_request
{
    stream_options options;

    friend bool operator==(const traffic_subscribe_request&,
                           const traffic_subscribe_request&) = default;
};

struct traffic_unsubscribe_request
{
    friend bool operator==(const traffic_unsubscribe_request&,
                           const traffic_unsubscribe_request&) = default;
};

/// Asks who is connected.  Only the administration endpoint may.
struct sessions_list_request
{
    friend bool operator==(const sessions_list_request&,
                           const sessions_list_request&) = default;
};

/// Closes the session with this number.
struct sessions_kick_request
{
    std::uint64_t session = 0;

    friend bool operator==(const sessions_kick_request&,
                           const sessions_kick_request&) = default;
};

using request_body =
    std::variant<hello_request, status_request, ports_list_request,
                 endpoint_stop_request, tokens_create_request,
                 tokens_list_request, tokens_revoke_request,
                 events_subscribe_request, events_unsubscribe_request,
                 traffic_subscribe_request, traffic_unsubscribe_request,
                 sessions_list_request, sessions_kick_request>;

struct request
{
    /// Chosen by the client and returned in the reply.  Never 0.
    std::uint64_t id = 0;
    request_body body;

    friend bool operator==(const request&, const request&) = default;
};

/// The operation a request asks for.
operation operation_of(const request_body& body) noexcept;

// -- replies -----------------------------------------------------------------

struct done_reply
{
    friend bool operator==(const done_reply&, const done_reply&) = default;
};

struct hello_reply
{
    std::uint32_t api = api_version;
    /// The version of the gateway program, for a person to read.
    std::string version;
    /// Who the gateway takes this connection to be: `local:<user>`.
    std::string principal;
    /// The names of the scopes this connection holds.
    std::vector<std::string> scopes;
    /// The names of what this gateway can do.  A feature that is not listed
    /// is off.
    std::vector<std::string> features;

    friend bool operator==(const hello_reply&, const hello_reply&) = default;
};

struct status_reply
{
    std::uint64_t uptime_ms = 0;
    /// Sessions that have said `hello`, this one included.
    std::uint64_t sessions = 0;
    bool agent_running = false;
    /// Empty when the agent is not running.
    std::string agent_version;

    friend bool operator==(const status_reply&, const status_reply&) = default;
};

/// One port.  `id` is an opaque text that stays the same for the life of the
/// port; it is never a COM number or a path.
struct port_entry
{
    std::string id;
    std::string name;
    std::string path;
    /// The engine's name of the kind and of the state.
    std::string kind;
    std::string state;
    bool open = false;
    std::uint64_t bytes_rx = 0;
    std::uint64_t bytes_tx = 0;

    friend bool operator==(const port_entry&, const port_entry&) = default;
};

struct link_entry
{
    std::string id;
    std::string mode;
    /// The ids of the ports on each side.
    std::vector<std::string> sources;
    std::vector<std::string> targets;
    std::uint64_t bytes = 0;

    friend bool operator==(const link_entry&, const link_entry&) = default;
};

struct ports_reply
{
    /// Counts the changes to the set of ports and links.
    std::uint64_t revision = 0;
    std::vector<port_entry> ports;
    std::vector<link_entry> links;

    friend bool operator==(const ports_reply&, const ports_reply&) = default;
};

/// What the gateway keeps about a token.  Times are milliseconds since
/// 1970-01-01 UTC.
struct token_entry
{
    std::string id;
    std::string label;
    std::vector<std::string> scopes;
    std::vector<std::string> transports;
    std::uint64_t created_ms = 0;
    std::uint64_t expires_ms = 0;
    /// 0 when it was never used.
    std::uint64_t last_used_ms = 0;
    bool revoked = false;

    friend bool operator==(const token_entry&, const token_entry&) = default;
};

/// A token that was just made.  This reply is the one time the token itself
/// is sent; the gateway does not keep it.
struct token_created_reply
{
    secret_string token{};
    token_entry entry;

    friend bool operator==(const token_created_reply&,
                           const token_created_reply&) = default;
};

struct tokens_reply
{
    std::vector<token_entry> tokens;

    friend bool operator==(const tokens_reply&, const tokens_reply&) = default;
};

/// A stream was started or changed.
struct subscribed_reply
{
    /// The time between two batches that the gateway took.
    std::uint32_t interval_ms = default_stream_interval_ms;
    /// The number the next batch of this connection will have.
    std::uint64_t next_seq = 1;

    friend bool operator==(const subscribed_reply&,
                           const subscribed_reply&) = default;
};

/// One session, as `sessions.list` tells it.
struct session_entry
{
    /// The number the audit log knows the session by.
    std::uint64_t number = 0;
    std::string principal;
    std::string transport;
    /// What the program called itself in `hello`, and what the operating
    /// system says it is.
    std::string client;
    std::string program;
    std::uint64_t process = 0;
    /// When it said `hello`: milliseconds since 1970-01-01 UTC.
    std::uint64_t opened_ms = 0;
    std::uint64_t requests = 0;
    /// Which streams it has now.
    bool events = false;
    bool traffic = false;
    /// What its streams have carried and lost so far.
    std::uint64_t events_sent = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t events_dropped = 0;

    friend bool operator==(const session_entry&,
                           const session_entry&) = default;
};

struct sessions_reply
{
    std::vector<session_entry> sessions;

    friend bool operator==(const sessions_reply&,
                           const sessions_reply&) = default;
};

struct error_reply
{
    api_error error;

    friend bool operator==(const error_reply&, const error_reply&) = default;
};

using reply_body = std::variant<done_reply, hello_reply, status_reply,
                                ports_reply, token_created_reply, tokens_reply,
                                subscribed_reply, sessions_reply, error_reply>;

struct reply
{
    /// The id of the request, or 0 for an error about the connection itself.
    std::uint64_t id = 0;
    reply_body body;

    friend bool operator==(const reply&, const reply&) = default;
};

// -- events -----------------------------------------------------------------

/// One thing that happened to a port.
struct event_entry
{
    /// The engine's name of the kind: `started`, `stopped`, `data`, `lines`,
    /// `error`, `stats`, `log_message`, `bus_fault`.  A client ignores a
    /// kind it does not know.
    std::string kind;
    /// The id of the port (`p_3`); empty for an event about no port.
    std::string port;
    std::string name;
    /// For `data`: `rx` or `tx`, as the port saw it.
    std::string direction;
    /// How many bytes moved.
    std::uint64_t size = 0;
    /// The bytes, for a connection with the traffic stream.  Empty
    /// otherwise, and `size` still says how many there were.
    byte_buffer data;
    /// The control lines, as bits.
    std::uint32_t lines = 0;
    std::string message;
    /// For `error`: the name of the engine's error (`engine.not_open`).
    std::string error;
    /// When the engine made the event, in nanoseconds on the steady clock
    /// of the computer the ports are on.  0 when not known.
    std::uint64_t mono_ns = 0;

    friend bool operator==(const event_entry&, const event_entry&) = default;
};

/// The events of one interval.  It is sent by the gateway on its own, not
/// in answer to a request, and has no `id`.
struct event_batch
{
    /// Counts the batches of this connection from 1, without gaps.
    std::uint64_t seq = 0;
    /// As `ports_reply::revision`: when it changes, the ports or links did.
    std::uint64_t revision = 0;
    /// Events of this connection's streams that were not sent since the
    /// batch before, because they came faster than they could be passed on.
    std::uint64_t dropped = 0;
    /// The steady clock and the wall clock of the gateway's computer, read
    /// together when the batch was made.  They turn the `mono_ns` of an
    /// event into a time of day: `wall_ms - (mono_ns - event.mono_ns) / 1e6`.
    std::uint64_t mono_ns = 0;
    std::uint64_t wall_ms = 0;
    std::vector<event_entry> events;

    friend bool operator==(const event_batch&, const event_batch&) = default;
};

/// What the gateway sends: an answer, or a batch nobody asked for by id.
using server_message = std::variant<reply, event_batch>;

// -- wire form ---------------------------------------------------------------

frame encode(const request& value);
frame encode(const reply& value);
/// The bytes of the events go into the frame's blob, in order.
frame encode(const event_batch& value);

/// What a client sent.  `api_errc::invalid_argument` for a header that is not
/// what a request looks like, and `api_errc::unsupported` for an operation
/// name this build does not have.
result<request> decode_request(const frame& value);

/// What the gateway sent in answer to a request.
/// `api_errc::invalid_argument` for anything else, a batch included.
result<reply> decode_reply(const frame& value);

/// What the gateway sent, of either kind.
result<server_message> decode_server_message(const frame& value);

/// The id of a request whose body could not be decoded, so the error can
/// still be sent to the right caller.  0 when the header has none.
std::uint64_t peek_request_id(const frame& value) noexcept;

/// The public id of a port (`p_3`) and of a link (`l_2`).
std::string port_id_text(std::uint64_t endpoint);
std::string link_id_text(std::uint64_t link);

/// The port a public id names, or nothing for a text that is not one.  Only
/// the form `port_id_text` writes is taken: no sign, no space, no leading
/// zero, and a number that fits.
std::optional<std::uint64_t> parse_port_id(std::string_view text) noexcept;

} // namespace vss::control
