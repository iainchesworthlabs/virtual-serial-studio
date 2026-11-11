//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_CORE_EVENT_HPP
#define VSS_CORE_EVENT_HPP

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
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
    bus_fault
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
