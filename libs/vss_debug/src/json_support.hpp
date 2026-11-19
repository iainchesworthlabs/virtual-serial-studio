//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/debug/redact.hpp>
#include <vss/trace/recorder.hpp>

#include <boost/json.hpp>

#include <cstdint>
#include <string>

namespace vss::debug
{

/// `2026-11-18T09:30:00.123Z` for milliseconds since the Unix epoch.
std::string time_text(std::int64_t wall_ms);

/// One recorder entry as the debug API shows it.
boost::json::object entry_object(const trace::entry& value);

/// Applies `redact` to every string in `value`, keys included.
void redact_value(boost::json::value& value, const redactor& redact);

} // namespace vss::debug
