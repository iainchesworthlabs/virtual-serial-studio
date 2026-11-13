//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/event.hpp>
#include <vss/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace vss::agent
{

// -- limits ------------------------------------------------------------------
//
// A decoder refuses anything over these, so a frame cannot ask the reader for
// more memory than the frame itself holds.

/// The longest name, path, message or other text in a message, in bytes.
inline constexpr std::size_t max_text_size = 1024;
/// The most ports, links, link members, capabilities or events in a message.
inline constexpr std::size_t max_list_size = 4096;
/// How deep the JSON of a header may nest.
inline constexpr std::size_t max_json_depth = 8;
/// The shortest and longest time between two event batches, and the time the
/// window uses today.  A `subscribe` always says a time; the agent, not the
/// decoder, brings one outside the range to its nearest end.
inline constexpr std::uint32_t min_event_interval_ms = 10;
inline constexpr std::uint32_t max_event_interval_ms = 10'000;
inline constexpr std::uint32_t default_event_interval_ms = 250;

// -- requests ----------------------------------------------------------------

/// The first request of every connection.
struct hello_request
{
    std::uint32_t protocol = protocol_version;
    /// The program that connects, for the agent's log (`vss_cli 0.1.0`).
    std::string client;
    /// The connection only looks: it may list and subscribe, and the agent
    /// refuses everything else it asks for.  It cannot be taken back.
    bool read_only = false;

    friend bool operator==(const hello_request&,
                           const hello_request&) = default;
};

/// Asks for the ports and the links.
struct list_request
{
    friend bool operator==(const list_request&, const list_request&) = default;
};

/// Makes a port.  `kind` is one of the names in `hello_reply::port_kinds`.
struct create_port_request
{
    std::string name;
    std::string kind;

    friend bool operator==(const create_port_request&,
                           const create_port_request&) = default;
};

struct remove_port_request
{
    endpoint_id endpoint = 0;

    friend bool operator==(const remove_port_request&,
                           const remove_port_request&) = default;
};

/// Links two ports: both ways, or from `first` to `second` only.
struct connect_request
{
    endpoint_id first = 0;
    endpoint_id second = 0;
    bool one_way = false;

    friend bool operator==(const connect_request&,
                           const connect_request&) = default;
};

/// Writes bytes to a port, as the Send pane does.  The bytes are the frame's
/// blob.
struct write_request
{
    endpoint_id endpoint = 0;
    byte_buffer data;

    friend bool operator==(const write_request&,
                           const write_request&) = default;
};

/// Starts the event stream of this connection, or changes it.
struct subscribe_request
{
    /// Send the bytes of traffic too (a read-only tap), not only that traffic
    /// happened.
    bool data = false;
    /// The time between two batches.  See `min_event_interval_ms`.
    std::uint32_t interval_ms = default_event_interval_ms;

    friend bool operator==(const subscribe_request&,
                           const subscribe_request&) = default;
};

/// Asks the agent to close every port and exit.
struct shutdown_request
{
    friend bool operator==(const shutdown_request&,
                           const shutdown_request&) = default;
};

using request_body =
    std::variant<hello_request, list_request, create_port_request,
                 remove_port_request, connect_request, write_request,
                 subscribe_request, shutdown_request>;

struct request
{
    /// Chosen by the client and returned in the reply.  Never 0.
    std::uint64_t id = 0;
    request_body body;

    friend bool operator==(const request&, const request&) = default;
};

// -- replies -----------------------------------------------------------------

struct port_summary
{
    endpoint_id id = 0;
    std::string name;
    std::string path;
    endpoint_kind kind = endpoint_kind::virtual_port;
    endpoint_state state = endpoint_state::created;
    bool open = false;
    std::uint64_t bytes_rx = 0;
    std::uint64_t bytes_tx = 0;

    friend bool operator==(const port_summary&, const port_summary&) = default;
};

struct link_summary
{
    link_id id = 0;
    link_mode mode = link_mode::duplex;
    std::vector<endpoint_id> sources;
    std::vector<endpoint_id> targets;
    std::uint64_t bytes = 0;

    friend bool operator==(const link_summary&, const link_summary&) = default;
};

/// A request that succeeded and has nothing to say.
struct done_reply
{
    friend bool operator==(const done_reply&, const done_reply&) = default;
};

struct hello_reply
{
    std::uint32_t protocol = protocol_version;
    /// The version of the program, such as `0.1.0`, for a person to read.
    std::string agent_version;
    std::uint64_t process_id = 0;
    /// The kinds of port `create_port` takes in this agent.
    std::vector<std::string> port_kinds;
    /// What the `hello` asked for, said back: a client that asked for a
    /// connection that only looks can see that it has one.
    bool read_only = false;

    friend bool operator==(const hello_reply&, const hello_reply&) = default;
};

struct list_reply
{
    /// Counts the changes to the set of ports and links, by any client.
    std::uint64_t revision = 0;
    std::vector<port_summary> ports;
    std::vector<link_summary> links;

    friend bool operator==(const list_reply&, const list_reply&) = default;
};

struct create_port_reply
{
    endpoint_id endpoint = 0;

    friend bool operator==(const create_port_reply&,
                           const create_port_reply&) = default;
};

struct connect_reply
{
    link_id link = 0;

    friend bool operator==(const connect_reply&,
                           const connect_reply&) = default;
};

struct error_reply
{
    wire_error error;

    friend bool operator==(const error_reply&, const error_reply&) = default;
};

using reply_body = std::variant<done_reply, hello_reply, list_reply,
                                create_port_reply, connect_reply, error_reply>;

struct reply
{
    /// The id of the request, or 0 for an error about a frame that had none.
    std::uint64_t id = 0;
    reply_body body;

    friend bool operator==(const reply&, const reply&) = default;
};

// -- events ------------------------------------------------------------------

/// One engine event.  The fields are those of `vss::endpoint_event`.
struct event_record
{
    event_kind kind = event_kind::log_message;
    endpoint_id endpoint = 0;
    std::string name;
    data_direction direction = data_direction::rx;
    /// How many bytes moved, for a data event.
    std::uint64_t size = 0;
    /// The bytes themselves, when the client asked for them.  Empty otherwise,
    /// and `size` still says how many there were.
    byte_buffer data;
    /// `vss::control_lines::bits`.
    std::uint32_t lines = 0;
    std::string message;
    /// The error of an error event, or a default one.
    wire_error error;
    /// When the engine made the event: nanoseconds on the steady clock of
    /// this computer, the clock that stamps all traffic.  0 when not known.
    std::uint64_t when_ns = 0;

    friend bool operator==(const event_record&, const event_record&) = default;
};

/// The events of one interval, sent as one frame.  The agent never sends an
/// event by itself.
struct event_batch
{
    /// As `list_reply::revision`: a client that sees it change asks for the
    /// list again.
    std::uint64_t revision = 0;
    /// Events this client did not get since the batch before, because it read
    /// slower than they arrived.  Counted, never queued without bound.
    std::uint64_t dropped = 0;
    std::vector<event_record> events;

    friend bool operator==(const event_batch&, const event_batch&) = default;
};

using server_message = std::variant<reply, event_batch>;

// -- wire form ---------------------------------------------------------------

frame encode(const request& value);
frame encode(const reply& value);
frame encode(const event_batch& value);

/// What a client sent.  `protocol_errc::malformed_message` for a header that
/// is not what a request looks like and `unknown_operation` for an operation
/// name this build does not have.
result<request> decode_request(const frame& value);

/// What the agent sent: a reply or a batch of events.
result<server_message> decode_server_message(const frame& value);

/// The id of a request whose body could not be decoded, so the error can
/// still be sent to the right caller.  0 when the header has none.
std::uint64_t peek_request_id(const frame& value) noexcept;

} // namespace vss::agent
