//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who may do what through the control endpoint: the scopes, the table of
// operations and the one function that decides.  Nothing here does I/O, so
// every pair of caller and operation can be tested as a table
// (docs/remote-control.md, sections 5 and 7).
//

#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss::control
{

// -- scopes ------------------------------------------------------------------

/// A class of actions.  No scope implies another.
enum class scope : std::uint32_t
{
    /// Read ports, links, state and counters.  No bytes of traffic.
    monitor = 1u << 0,
    /// Read the bytes of traffic.
    monitor_data = 1u << 1,
    /// Make, remove, link and configure ports.
    control_ports = 1u << 2,
    /// Send bytes and set scripted replies.
    control_data = 1u << 3,
    /// Inject faults and set response timing.
    control_faults = 1u << 4,
    /// Apply a whole topology document.
    control_apply = 1u << 5,
    /// Start, stop and fetch recordings.
    record = 1u << 6,
    /// Change who may connect, and stop the gateway.  Held only by a
    /// connection to the administration endpoint.
    admin = 1u << 7
};

/// Every scope, in the order of the enumeration.
inline constexpr scope all_scope_values[] = {
    scope::monitor,      scope::monitor_data,   scope::control_ports,
    scope::control_data, scope::control_faults, scope::control_apply,
    scope::record,       scope::admin};

/// A set of scopes.
class scope_set
{
public:
    constexpr scope_set() noexcept = default;

    constexpr scope_set(std::initializer_list<scope> members) noexcept
    {
        for (const scope member : members)
            bits_ |= static_cast<std::uint32_t>(member);
    }

    constexpr bool contains(scope member) const noexcept
    {
        return (bits_ & static_cast<std::uint32_t>(member)) != 0;
    }

    constexpr bool contains_all(scope_set other) const noexcept
    {
        return (bits_ & other.bits_) == other.bits_;
    }

    constexpr bool empty() const noexcept { return bits_ == 0; }

    constexpr scope_set without(scope member) const noexcept
    {
        scope_set out;
        out.bits_ = bits_ & ~static_cast<std::uint32_t>(member);
        return out;
    }

    friend constexpr scope_set operator|(scope_set first,
                                         scope_set second) noexcept
    {
        scope_set out;
        out.bits_ = first.bits_ | second.bits_;
        return out;
    }

    friend constexpr scope_set operator&(scope_set first,
                                         scope_set second) noexcept
    {
        scope_set out;
        out.bits_ = first.bits_ & second.bits_;
        return out;
    }

    friend constexpr bool operator==(scope_set, scope_set) noexcept = default;

private:
    std::uint32_t bits_ = 0;
};

/// The name of a scope on the wire and in a dialog: `monitor.data`.
std::string_view scope_name(scope value) noexcept;
std::optional<scope> parse_scope(std::string_view name) noexcept;
/// The names of the members, in the order of the enumeration.
std::vector<std::string> scope_names(scope_set set);

/// The named bundles a person chooses from.
constexpr scope_set role_monitor() noexcept
{
    return {scope::monitor};
}
constexpr scope_set role_control() noexcept
{
    return {scope::monitor,      scope::monitor_data,   scope::control_ports,
            scope::control_data, scope::control_faults, scope::record};
}
constexpr scope_set role_control_apply() noexcept
{
    return role_control() | scope_set{scope::control_apply};
}
constexpr scope_set every_scope() noexcept
{
    return role_control_apply() | scope_set{scope::admin};
}

// -- callers -----------------------------------------------------------------

/// How a connection reached the gateway.
enum class transport
{
    /// The public local pipe or socket.
    local,
    /// The administration pipe or socket.
    local_admin,
    /// TCP on the loopback interface.
    loopback,
    /// TLS from another computer.
    tls
};

std::string_view transport_name(transport value) noexcept;

/// Who a connection is, once it has been authenticated.  It is made once and
/// never changed; a connection whose rights change is closed.
struct principal
{
    /// `local:<user>` for a connection the kernel vouched for.
    std::string id;
    control::transport via = transport::local;
    /// What this caller may do, already cut down by `effective_scopes`.
    scope_set scopes;
};

/// The scopes a caller really gets: what it was granted, cut down to what
/// its transport allows.  `admin` survives only on the administration
/// endpoint, whatever was granted and whatever the ceiling says.
constexpr scope_set effective_scopes(scope_set granted, transport via,
                                     scope_set ceiling) noexcept
{
    const scope_set allowed = granted & ceiling;
    return via == transport::local_admin ? allowed
                                         : allowed.without(scope::admin);
}

/// True when a caller with these scopes can only look: it holds nothing but
/// `monitor` and `monitor.data`.  The gateway opens such a caller's
/// connection to the agent read-only, so the agent refuses a change even if
/// the gateway were wrong about one.  Written as what is allowed, so a scope
/// that is added later counts as one that changes things.
constexpr bool looks_only(scope_set scopes) noexcept
{
    return scopes.without(scope::monitor).without(scope::monitor_data).empty();
}

// -- operations --------------------------------------------------------------

/// Every operation of the API.  An operation that is not here does not exist.
enum class operation
{
    hello,
    status,
    ports_list,
    endpoint_stop,
    tokens_create,
    tokens_list,
    tokens_revoke,
    events_subscribe,
    events_unsubscribe,
    traffic_subscribe,
    traffic_unsubscribe,
    sessions_list,
    sessions_kick
};

struct operation_info
{
    operation id;
    /// The name on the wire: `ports.list`.
    std::string_view name;
    /// The scope a caller needs.  Nothing only for an operation that every
    /// authenticated caller may use.
    std::optional<scope> required;
    /// True when the operation changes something, so each use is audited.
    bool mutating;
};

/// The table of operations.  It is the one place that says which scope an
/// operation needs; a handler does no checking of its own.
std::span<const operation_info> operations() noexcept;

/// The row of an operation.
const operation_info& info_of(operation id) noexcept;

/// The row for a wire name, or null for a name that is not an operation.
const operation_info* find_operation(std::string_view name) noexcept;

/// The answer of `authorise`.
struct decision
{
    bool allowed = false;
    /// The scope the caller lacks, when it was refused for lack of one.
    std::optional<scope> missing;
};

/// Decides one request.  Deny is the default: an operation is allowed only
/// when its row names no scope, or names one the caller holds.  An operation
/// that needs `admin` is refused on every transport but the administration
/// endpoint, even to a caller whose scopes say otherwise.
decision authorise(const principal& caller,
                   const operation_info& wanted) noexcept;

} // namespace vss::control
