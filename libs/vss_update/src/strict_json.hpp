//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strict reading of the feed's JSON: no key given twice, a depth limit, no
// text after the document. Internal to vss_update.
//

#pragma once

#include <boost/json/value.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace vss::update::detail
{

/// How deep the feed's JSON may nest: the document, the list of releases, a
/// release, its list of files and a file.
inline constexpr std::size_t max_json_depth = 6;

/// Parses one JSON document. Refused: text that is not JSON, invalid UTF-8,
/// comments, a trailing comma, anything but white space after the document,
/// nesting deeper than `max_json_depth` and a key given twice in one object,
/// so that no two readers can disagree about what a feed says. On a refusal
/// `what` says why, as the end of a sentence ("is not JSON: ...").
std::optional<boost::json::value> parse_strict(std::string_view text,
                                               std::string& what);

/// `text` in quotes for a message to a person: cut short, and with anything
/// that is not printable ASCII replaced.
std::string quoted(std::string_view text);

} // namespace vss::update::detail
