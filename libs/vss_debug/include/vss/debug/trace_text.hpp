//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/trace/recorder.hpp>

#include <chrono>
#include <string>

namespace vss::debug
{

/// One entry as a line of text: the time, the number, the call and its
/// arguments, the operation and port it belonged to, and for a failure the
/// raw code, its category and message and the entry it came from.
std::string entry_line(const trace::entry& value);

/// The text of the support bundle's call trace: every failed call that the
/// recorder still holds, with the calls made in the `around` before and after
/// it, oldest first.  A recorder with no failure says so and lists the last
/// calls instead, so the file is never empty.  No bytes of traffic are in it,
/// because the recorder holds none.
std::string
failure_context_text(const trace::recorder& source,
                     std::chrono::seconds around = std::chrono::seconds(30));

} // namespace vss::debug
