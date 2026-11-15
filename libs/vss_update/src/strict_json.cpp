//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "strict_json.hpp"

#include <boost/json/basic_parser_impl.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace vss::update::detail
{

namespace
{

namespace json = boost::json;

/// A handler for `basic_parser` that keeps nothing but the keys of the
/// objects that are open, and stops at the first key seen twice.
class duplicate_key_finder
{
public:
    // Boost.JSON looks for these four by name.
    // NOLINTBEGIN(readability-identifier-naming)
    static constexpr std::size_t max_array_size =
        std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t max_object_size =
        std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t max_string_size =
        std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t max_key_size =
        std::numeric_limits<std::size_t>::max();
    // NOLINTEND(readability-identifier-naming)

    bool found() const noexcept { return found_; }
    /// The first key that was given twice in one object.
    const std::string& repeated() const noexcept { return repeated_; }

    bool on_document_begin(boost::system::error_code&) { return true; }
    bool on_document_end(boost::system::error_code&) { return true; }
    bool on_array_begin(boost::system::error_code&) { return true; }
    bool on_array_end(std::size_t, boost::system::error_code&) { return true; }

    bool on_object_begin(boost::system::error_code&)
    {
        objects_.emplace_back();
        return true;
    }

    bool on_object_end(std::size_t, boost::system::error_code&)
    {
        objects_.pop_back();
        return true;
    }

    bool on_key_part(json::string_view part, std::size_t,
                     boost::system::error_code&)
    {
        key_.append(part.data(), part.size());
        return true;
    }

    bool on_key(json::string_view part, std::size_t,
                boost::system::error_code& error)
    {
        key_.append(part.data(), part.size());
        // An ordered set, so that the cost of one key is the logarithm of
        // how many the object has, whatever the keys are.
        if (!objects_.back().insert(key_).second)
        {
            found_ = true;
            repeated_ = std::move(key_);
            error = json::make_error_code(json::error::syntax);
            return false;
        }
        key_.clear();
        return true;
    }

    bool on_string_part(json::string_view, std::size_t,
                        boost::system::error_code&)
    {
        return true;
    }
    bool on_string(json::string_view, std::size_t, boost::system::error_code&)
    {
        return true;
    }
    bool on_number_part(json::string_view, boost::system::error_code&)
    {
        return true;
    }
    bool on_int64(std::int64_t, json::string_view, boost::system::error_code&)
    {
        return true;
    }
    bool on_uint64(std::uint64_t, json::string_view, boost::system::error_code&)
    {
        return true;
    }
    bool on_double(double, json::string_view, boost::system::error_code&)
    {
        return true;
    }
    bool on_bool(bool, boost::system::error_code&) { return true; }
    bool on_null(boost::system::error_code&) { return true; }
    bool on_comment_part(json::string_view, boost::system::error_code&)
    {
        return true;
    }
    bool on_comment(json::string_view, boost::system::error_code&)
    {
        return true;
    }

private:
    /// The keys seen so far in each object that is open, outermost first.
    std::vector<std::set<std::string, std::less<>>> objects_;
    std::string key_;
    std::string repeated_;
    bool found_ = false;
};

} // namespace

std::optional<boost::json::value> parse_strict(std::string_view text,
                                               std::string& what)
{
    json::parse_options options;
    options.max_depth = max_json_depth;

    // Once for the keys, and once for the values. A text that is not JSON
    // fails the second pass with the parser's own reason.
    {
        json::basic_parser<duplicate_key_finder> scanner(options);
        boost::system::error_code ignored;
        scanner.write_some(false, text.data(), text.size(), ignored);
        if (scanner.handler().found())
        {
            what = "the key " + quoted(scanner.handler().repeated()) +
                   " is given twice in one object";
            return std::nullopt;
        }
    }

    boost::system::error_code failed;
    json::value root = json::parse(text, failed, {}, options);
    if (failed)
    {
        what = "is not JSON: " + failed.message();
        return std::nullopt;
    }
    return root;
}

std::string quoted(std::string_view text)
{
    constexpr std::size_t shown = 40;
    std::string out = "\"";
    for (std::size_t i = 0; i < text.size() && i < shown; ++i)
    {
        const char c = text[i];
        out += (c >= 0x20 && c <= 0x7e) ? c : '?';
    }
    if (text.size() > shown)
        out += "...";
    out += '"';
    return out;
}

} // namespace vss::update::detail
