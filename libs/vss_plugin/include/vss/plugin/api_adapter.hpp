//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages of an automation adapter: a plugin that lets another tool drive
// Studio through the agent proxy.  docs/plugins.md, section 7.7.
//
// An adapter has no instance in these messages.  The plugin sends `agent.call`
// and the host answers `agent.result`, `ok` or an error (`not_granted` for an
// operation group the user did not grant).  `agent.subscribe` asks for the
// agent's events; the design names no message that carries them, so none is
// defined here.  When the plugin is not running, the host answers a request of
// an adapter with `plugin_unavailable`; `make_plugin_unavailable` is that
// answer.
//
// The names of the operations of `agent.call` are the agent protocol's own.
// This header only checks that the name is a token; the proxy checks it
// against the set that was granted.
//

#pragma once

#include <vss/plugin/api_message.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vss::plugin
{

/// The longest name of an operation of the agent protocol, in bytes.
inline constexpr std::size_t max_agent_op = 64;
/// The most places one `agent.subscribe` names.  A choice: the design gives no
/// limit.
inline constexpr std::size_t max_subscribe_places = 256;
/// The longest address in `adapter.state`, in bytes.  A choice: the design
/// gives no limit.
inline constexpr std::size_t max_adapter_address = 256;
/// The longest protocol name in `adapter.state`, in bytes.  A choice: the
/// design gives no limit.
inline constexpr std::size_t max_adapter_protocol = 64;

/// One call of the agent proxy.  The host answers `agent.result`, "ok" or an
/// error.
struct agent_call_message
{
    static constexpr std::string_view op = "agent.call";
    static constexpr api_side from = api_side::plugin;
    static constexpr api_role role = api_role::request;

    /// The operation of the agent protocol, as 1 to `max_agent_op` lower-case
    /// letters, digits, dots, hyphens and underscores.  Its name in the
    /// header is "agent_op", as "op" is the name of the message.
    std::string agent_op;
    /// The arguments as one JSON object.  "{}" for none.
    std::string args = "{}";
    /// 0 to `max_api_blob` bytes, in the blob, for an operation that writes
    /// bytes.
    byte_buffer data;

    friend bool operator==(const agent_call_message&,
                           const agent_call_message&) = default;
};

/// The answer to `agent.call`.
struct agent_result_message
{
    static constexpr std::string_view op = "agent.result";
    static constexpr api_side from = api_side::host;
    static constexpr api_role role = api_role::answer;

    /// What the operation returned, as one JSON object.  "{}" for nothing.
    std::string result = "{}";
    /// 0 to `max_api_blob` bytes, in the blob.
    byte_buffer data;

    friend bool operator==(const agent_result_message&,
                           const agent_result_message&) = default;
};

/// Asks for the agent's events.  The host answers "ok" or an error.
struct agent_subscribe_message
{
    static constexpr std::string_view op = "agent.subscribe";
    static constexpr api_side from = api_side::plugin;
    static constexpr api_role role = api_role::request;

    /// False is the events without traffic bytes.  True asks for the bytes
    /// too, and needs `traffic.tap`.  These are not the bytes of the blob:
    /// this message has no blob.
    bool data = false;
    /// 0 to `max_subscribe_places` names, each 1 to `max_api_name` bytes of
    /// plain text.  None is every place the grant covers.
    std::vector<std::string> places;

    friend bool operator==(const agent_subscribe_message&,
                           const agent_subscribe_message&) = default;
};

/// What the adapter listens on, for the plugin's page.
struct adapter_state_message
{
    static constexpr std::string_view op = "adapter.state";
    static constexpr api_side from = api_side::plugin;
    static constexpr api_role role = api_role::notification;

    bool listening = false;
    /// The exact address, 1 to `max_adapter_address` bytes of plain text.
    /// Given when `listening` is true, and not given when it is false.
    std::string address;
    /// The name of the protocol, 1 to `max_adapter_protocol` bytes of plain
    /// text.
    std::string protocol;

    friend bool operator==(const adapter_state_message&,
                           const adapter_state_message&) = default;
};

/// The "error" answer to the request `re` of an adapter whose plugin is not
/// running.
api_message make_plugin_unavailable(std::uint64_t re);

template <>
result<agent_call_message> decode_as<agent_call_message>(const api_message&,
                                                         std::string*);
template <>
result<api_message> encode_as<agent_call_message>(const agent_call_message&,
                                                  std::uint64_t);
template <>
result<agent_result_message> decode_as<agent_result_message>(const api_message&,
                                                             std::string*);
template <>
result<api_message> encode_as<agent_result_message>(const agent_result_message&,
                                                    std::uint64_t);
template <>
result<agent_subscribe_message>
decode_as<agent_subscribe_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<agent_subscribe_message>(const agent_subscribe_message&,
                                   std::uint64_t);
template <>
result<adapter_state_message>
decode_as<adapter_state_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<adapter_state_message>(const adapter_state_message&, std::uint64_t);

} // namespace vss::plugin
