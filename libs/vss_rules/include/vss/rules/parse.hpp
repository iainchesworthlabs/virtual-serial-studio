//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads a rule set from its JSON document.  docs/response-timing.md has the
// format.  The reader only reads: whether the set makes sense is compile()'s
// business, so a value that is well formed and wrong (a delay with its
// minimum above its maximum) is reported there, with the same kind of path.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/rules/compile.hpp>
#include <vss/rules/model.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace vss::rules {

/// The largest document read, and the deepest nesting.
inline constexpr std::size_t max_document_bytes = std::size_t{1} << 20;
inline constexpr std::size_t max_document_depth = 16;

/// Reads `text`.  On failure the error is `errc::config_error` and
/// `problems`, when given, says what is wrong and where.  An unknown key is
/// an error, so a misspelt `delay` is found and not ignored.
result<rule_set_source> parse_rule_set(
    std::string_view text, std::vector<problem>* problems = nullptr);

/// #parse_rule_set and then #compile.
result<std::shared_ptr<const compiled_rule_set>> compile_rule_set(
    std::string_view text, std::vector<problem>* problems = nullptr);

} // namespace vss::rules
