//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "strict_json.hpp"

#include <boost/json/basic_parser_impl.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace vss::license::detail
{

namespace
{

namespace json = boost::json;

/// Deep enough for an envelope and a payload, and no deeper.
constexpr std::size_t max_depth = 8;

/// A handler for `basic_parser` that keeps nothing but the keys of the
/// objects that are open, and stops at the first key seen twice.
// The member names are those basic_parser calls.
// NOLINTBEGIN(readability-identifier-naming,readability-convert-member-functions-to-static)
struct duplicate_key_handler
{
    static constexpr std::size_t max_array_size =
        std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t max_object_size =
        std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t max_string_size =
        std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t max_key_size =
        std::numeric_limits<std::size_t>::max();

    std::vector<std::vector<std::string>> open_objects;
    std::string key_so_far;

    bool on_document_begin(boost::system::error_code&) { return true; }
    bool on_document_end(boost::system::error_code&) { return true; }
    bool on_array_begin(boost::system::error_code&) { return true; }
    bool on_array_end(std::size_t, boost::system::error_code&) { return true; }
    bool on_object_begin(boost::system::error_code&)
    {
        open_objects.emplace_back();
        return true;
    }
    bool on_object_end(std::size_t, boost::system::error_code&)
    {
        open_objects.pop_back();
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
    bool on_key_part(json::string_view part, std::size_t,
                     boost::system::error_code&)
    {
        key_so_far.append(part.data(), part.size());
        return true;
    }
    bool on_key(json::string_view part, std::size_t,
                boost::system::error_code& ec)
    {
        key_so_far.append(part.data(), part.size());
        auto& keys = open_objects.back();
        if (std::ranges::find(keys, key_so_far) != keys.end())
        {
            ec = json::make_error_code(json::error::syntax);
            return false;
        }
        keys.push_back(std::move(key_so_far));
        key_so_far.clear();
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
};
// NOLINTEND(readability-identifier-naming,readability-convert-member-functions-to-static)

json::parse_options strict_options()
{
    json::parse_options options;
    options.max_depth = max_depth;
    return options;
}

bool has_duplicate_key(std::string_view text)
{
    json::basic_parser<duplicate_key_handler> parser(strict_options());
    boost::system::error_code ec;
    parser.write_some(false, text.data(), text.size(), ec);
    return static_cast<bool>(ec);
}

const json::value* find(const json::object& object, std::string_view key)
{
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->value();
}

} // namespace

result<boost::json::value> parse_strict(std::string_view text)
{
    boost::system::error_code ec;
    json::value value = json::parse(text, ec, {}, strict_options());
    if (ec || has_duplicate_key(text))
    {
        return errc::malformed;
    }
    return value;
}

result<std::string> required_string(const boost::json::object& object,
                                    std::string_view key,
                                    std::size_t max_length)
{
    const json::value* value = find(object, key);
    if (value == nullptr || !value->is_string())
    {
        return errc::malformed;
    }
    const json::string& text = value->get_string();
    if (text.empty() || text.size() > max_length)
    {
        return errc::malformed;
    }
    return std::string(text.data(), text.size());
}

result<std::string> optional_string(const boost::json::object& object,
                                    std::string_view key,
                                    std::size_t max_length)
{
    const json::value* value = find(object, key);
    if (value == nullptr || value->is_null())
    {
        return std::string();
    }
    return required_string(object, key, max_length);
}

result<std::int64_t> required_integer(const boost::json::object& object,
                                      std::string_view key,
                                      std::int64_t minimum,
                                      std::int64_t maximum)
{
    const json::value* value = find(object, key);
    if (value == nullptr)
    {
        return errc::malformed;
    }
    std::int64_t number = 0;
    if (value->is_int64())
    {
        number = value->get_int64();
    }
    else if (value->is_uint64() &&
             value->get_uint64() <=
                 static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max()))
    {
        number = static_cast<std::int64_t>(value->get_uint64());
    }
    else
    {
        return errc::malformed;
    }
    if (number < minimum || number > maximum)
    {
        return errc::malformed;
    }
    return number;
}

result<std::vector<std::string>> string_list(const boost::json::object& object,
                                             std::string_view key,
                                             std::size_t max_entries,
                                             std::size_t max_length)
{
    std::vector<std::string> list;
    const json::value* value = find(object, key);
    if (value == nullptr)
    {
        return list;
    }
    if (!value->is_array() || value->get_array().size() > max_entries)
    {
        return errc::malformed;
    }
    for (const json::value& entry : value->get_array())
    {
        if (!entry.is_string() || entry.get_string().empty() ||
            entry.get_string().size() > max_length)
        {
            return errc::malformed;
        }
        list.emplace_back(entry.get_string().data(), entry.get_string().size());
    }
    return list;
}

bool is_token(std::string_view text, std::size_t max_length,
              std::string_view extra)
{
    if (text.empty() || text.size() > max_length)
    {
        return false;
    }
    return std::ranges::all_of(text,
                               [extra](char c)
                               {
                                   return (c >= 'a' && c <= 'z') ||
                                          (c >= '0' && c <= '9') ||
                                          extra.find(c) !=
                                              std::string_view::npos;
                               });
}

} // namespace vss::license::detail
