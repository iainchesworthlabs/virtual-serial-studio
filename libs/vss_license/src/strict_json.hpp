//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// JSON reading for signed files: stricter than a plain parse, and with the
// typed field readers both layers use. Internal to vss_license.
//

#pragma once

#include <vss/license/error.hpp>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vss::license::detail
{

/// Parses standard JSON and also refuses an object that has the same key
/// twice, so that no two readers can disagree about what a file says.
/// Comments, trailing commas and invalid UTF-8 are refused as well.
result<boost::json::value> parse_strict(std::string_view text);

/// The field readers return `errc::malformed` for a missing field or one of
/// the wrong type or size.
result<std::string> required_string(const boost::json::object& object,
                                    std::string_view key,
                                    std::size_t max_length);

/// An absent field or a JSON null is the empty string.
result<std::string> optional_string(const boost::json::object& object,
                                    std::string_view key,
                                    std::size_t max_length);

/// A whole number in [minimum, maximum]. A number written with a fraction or
/// an exponent is refused.
result<std::int64_t> required_integer(const boost::json::object& object,
                                      std::string_view key,
                                      std::int64_t minimum,
                                      std::int64_t maximum);

/// An array of strings. An absent field is an empty list.
result<std::vector<std::string>> string_list(const boost::json::object& object,
                                             std::string_view key,
                                             std::size_t max_entries,
                                             std::size_t max_length);

/// Whether `text` is 1 to `max_length` characters, each a lower-case letter,
/// a digit or one of `extra`.
bool is_token(std::string_view text, std::size_t max_length,
              std::string_view extra);

} // namespace vss::license::detail
