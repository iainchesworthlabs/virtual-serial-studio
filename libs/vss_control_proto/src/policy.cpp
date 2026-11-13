//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/proto/policy.hpp>

#include <array>
#include <exception>

namespace vss::control
{
namespace
{

struct scope_row
{
    scope value;
    std::string_view name;
};

constexpr std::array scope_rows{
    scope_row{scope::monitor, "monitor"},
    scope_row{scope::monitor_data, "monitor.data"},
    scope_row{scope::control_ports, "control.ports"},
    scope_row{scope::control_data, "control.data"},
    scope_row{scope::control_faults, "control.faults"},
    scope_row{scope::control_apply, "control.apply"},
    scope_row{scope::record, "record"},
    scope_row{scope::admin, "admin"}};

// The table of section 9 of docs/remote-control.md.  `hello` is how a caller
// becomes a principal, so it names no scope; the gateway's state machine, not
// this table, says when it may be sent.
constexpr std::array operation_rows{
    operation_info{operation::hello, "hello", std::nullopt, false},
    operation_info{operation::status, "status", scope::monitor, false},
    operation_info{operation::ports_list, "ports.list", scope::monitor, false},
    operation_info{operation::endpoint_stop, "endpoint.stop", scope::admin,
                   true},
    operation_info{operation::tokens_create, "tokens.create", scope::admin,
                   true},
    operation_info{operation::tokens_list, "tokens.list", scope::admin, false},
    operation_info{operation::tokens_revoke, "tokens.revoke", scope::admin,
                   true},
    // Looking is not a change, so none of the four is "mutating"; the
    // gateway audits when a stream starts and ends all the same.
    operation_info{operation::events_subscribe, "events.subscribe",
                   scope::monitor, false},
    operation_info{operation::events_unsubscribe, "events.unsubscribe",
                   scope::monitor, false},
    operation_info{operation::traffic_subscribe, "traffic.subscribe",
                   scope::monitor_data, false},
    operation_info{operation::traffic_unsubscribe, "traffic.unsubscribe",
                   scope::monitor_data, false},
    operation_info{operation::sessions_list, "sessions.list", scope::admin,
                   false},
    operation_info{operation::sessions_kick, "sessions.kick", scope::admin,
                   true},
    // What changes the ports.  Sending bytes has a scope of its own: a
    // caller that may arrange ports is not thereby allowed to speak on them.
    operation_info{operation::ports_create, "ports.create",
                   scope::control_ports, true},
    operation_info{operation::ports_remove, "ports.remove",
                   scope::control_ports, true},
    operation_info{operation::links_connect, "links.connect",
                   scope::control_ports, true},
    operation_info{operation::ports_send, "ports.send", scope::control_data,
                   true},
    // Waiting for bytes is reading them.  When the request also sends, the
    // gateway asks for `ports.send` as well, by this same table.
    operation_info{operation::expect_bytes, "expect.bytes", scope::monitor_data,
                   false},
    // A run changes nothing of the ports: it is a list the gateway keeps of
    // what a harness checked, so a caller that may look may keep one.
    operation_info{operation::runs_begin, "runs.begin", scope::monitor, true},
    operation_info{operation::runs_end, "runs.end", scope::monitor, true},
    operation_info{operation::runs_record, "runs.record", scope::monitor, true},
    operation_info{operation::report_junit, "report.junit", scope::monitor,
                   false}};

} // namespace

std::string_view scope_name(scope value) noexcept
{
    for (const scope_row& row : scope_rows)
    {
        if (row.value == value)
            return row.name;
    }
    return "unknown";
}

std::optional<scope> parse_scope(std::string_view name) noexcept
{
    for (const scope_row& row : scope_rows)
    {
        if (row.name == name)
            return row.value;
    }
    return std::nullopt;
}

std::vector<std::string> scope_names(scope_set set)
{
    std::vector<std::string> out;
    for (const scope_row& row : scope_rows)
    {
        if (set.contains(row.value))
            out.emplace_back(row.name);
    }
    return out;
}

std::string_view transport_name(transport value) noexcept
{
    switch (value)
    {
    case transport::local:
        return "local";
    case transport::local_admin:
        return "local-admin";
    case transport::loopback:
        return "loopback";
    case transport::tls:
        return "tls";
    }
    return "unknown";
}

std::span<const operation_info> operations() noexcept
{
    return operation_rows;
}

const operation_info& info_of(operation id) noexcept
{
    for (const operation_info& row : operation_rows)
    {
        if (row.id == id)
            return row;
    }
    // Every enumerator has a row; the tests walk the table to prove it.
    std::terminate();
}

const operation_info* find_operation(std::string_view name) noexcept
{
    for (const operation_info& row : operation_rows)
    {
        if (row.name == name)
            return &row;
    }
    return nullptr;
}

decision authorise(const principal& caller,
                   const operation_info& wanted) noexcept
{
    if (!wanted.required)
        return {true, std::nullopt};
    const scope needed = *wanted.required;
    if (needed == scope::admin && caller.via != transport::local_admin)
        return {false, needed};
    if (!caller.scopes.contains(needed))
        return {false, needed};
    return {true, std::nullopt};
}

} // namespace vss::control
