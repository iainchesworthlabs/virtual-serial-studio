//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_CORE_EVENT_HPP
#define VSS_CORE_EVENT_HPP

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/line_break.hpp>
#include <vss/core/stats.hpp>
#include <vss/core/types.hpp>

#include <boost/system/error_code.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace vss {

enum class event_kind
{
    started,
    stopped,
    data,
    lines,
    error,
    stats,
    log_message,
    /// A chunk sent on a half-duplex bus was not a clean transmission.  The
    /// event is the sender's; `fault` says what happened, `link` names the
    /// bus and `message` says it in words.
    bus_fault,
    /// A serial break, in order with the `data` events around it.  `phase`,
    /// `start`, `duration` and `measured` say which part and how long;
    /// `direction` is `rx` for a break the endpoint received and `tx` for
    /// one it sent out.  `when` is the item's own time (`line_break::when`).
    line_break
};

struct endpoint_event
{
    event_kind kind = event_kind::log_message;
    endpoint_id endpoint = 0;
    std::string name;
    data_direction direction = data_direction::rx;
    std::shared_ptr<const byte_buffer> data;
    control_lines lines;
    boost::system::error_code error;
    std::string message;
    time_point when{};
    /// For `bus_fault`: what happened, and on which bus.
    bus_fault fault = bus_fault::none;
    link_id link = 0;
    /// For `line_break`: the part of the break this is, when it began, how
    /// long it lasted (zero for a `begin`) and whether the length was
    /// measured.
    break_phase phase = break_phase::complete;
    time_point start{};
    std::chrono::nanoseconds duration{0};
    bool measured = false;
};

using event_sink = std::function<void(const endpoint_event&)>;

/// Anything callable with a const #endpoint_event, which is what
/// `hub::subscribe` accepts.  Constraining it means a mistyped handler is a
/// concept error naming the handler, not a wall of `std::function` template
/// diagnostics.
template <class Sink>
concept EventSink = std::invocable<Sink&, const endpoint_event&>;

static_assert(EventSink<event_sink>);

/// Declarative subscription filter.  An empty optional matches everything.
struct event_filter
{
    std::optional<endpoint_id> endpoint;
    std::optional<event_kind> kind;

    bool matches(const endpoint_event& event) const noexcept
    {
        if (endpoint && *endpoint != event.endpoint)
            return false;
        if (kind && *kind != event.kind)
            return false;
        return true;
    }
};

const char* to_string(event_kind value) noexcept;

} // namespace vss

#endif // VSS_CORE_EVENT_HPP
