//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the readers of this library share: the strict reading of a JSON
// document and the rules for an id, a text, a version and a date.  They are
// defined in manifest.cpp, beside the manifest reader that they were written
// for.  This header is not installed.
//

#pragma once

#include <vss/plugin/manifest.hpp>

#include <boost/json/value.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace vss::plugin::detail
{

/// Reads one JSON document that nests at most `max_depth` deep.  Refused:
/// text that is not JSON, anything but white space after the document, and a
/// key given twice in one object.  On a refusal `what` says why, as the end
/// of a sentence ("is not JSON: ...").
std::optional<boost::json::value>
parse_strict(std::string_view text, std::size_t max_depth, std::string& what);

/// Whether `text` may be the id of a plugin.
bool valid_plugin_id(std::string_view text) noexcept;

/// Whether `text` is plain text: valid UTF-8 with no control character and
/// no mark that changes the direction of the text.
bool clean_text(std::string_view text) noexcept;

std::optional<plugin_version> version_from_text(std::string_view text) noexcept;

/// `YYYY-MM-DD`, a day that exists.
std::optional<std::chrono::year_month_day>
date_from_text(std::string_view text) noexcept;

/// `text` in quotes for a message to a person: cut short, and with anything
/// that is not plain ASCII replaced.
std::string quoted(std::string_view text);

} // namespace vss::plugin::detail
