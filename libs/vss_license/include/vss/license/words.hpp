//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plain words for a licence's state and for the reason behind it, as the
// command line tools print them (`vss_license verify` and `vss_cli licence`).
// The GUI has its own, translated, sentences.
//

#pragma once

#include <vss/license/license.hpp>

#include <string_view>

namespace vss::license
{

/// "none", "valid", "expiring", "grace", "expired", "not yet valid",
/// "clock suspect" or "invalid".
std::string_view state_word(state value) noexcept;

/// The name of a state for a program to read: `none`, `valid`, `expiring`,
/// `grace`, `expired`, `not_yet_valid`, `clock_suspect` or `invalid`. One
/// word each, the same in `vss_cli licence` and in the control gateway's
/// status.
std::string_view state_name(state value) noexcept;

/// The edition a status amounts to, for a program to read: `community`,
/// `pro` or `enterprise`. A licence that is not in force is `community`.
std::string_view edition_name(const status& current) noexcept;

/// One lower-case clause that says why a licence is in its state, such as "the
/// licence has ended"; "ok" for `reason::ok`.
std::string_view reason_sentence(reason value) noexcept;

} // namespace vss::license
