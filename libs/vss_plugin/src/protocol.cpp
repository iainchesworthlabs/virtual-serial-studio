//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/protocol.hpp>

#include "reader.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace vss::plugin
{

namespace
{

namespace json = boost::json;
using namespace std::string_view_literals;

// -- characters ---------------------------------------------------------------

constexpr bool is_lower(char c) noexcept
{
    return c >= 'a' && c <= 'z';
}

constexpr bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// 1 to `most` bytes, each one `allowed`.
template <class Allowed>
bool is_token(std::string_view text, std::size_t most, Allowed allowed) noexcept
{
    if (text.empty() || text.size() > most)
        return false;
    return std::ranges::all_of(text, allowed);
}

/// The name of a capability flag, such as `engine_rules`.
bool is_capability(std::string_view text) noexcept
{
    return is_token(text, 64, [](char c) {
        return is_lower(c) || is_digit(c) || c == '_';
    });
}

/// The category of an "error", such as `vss.plugin.api`.
bool is_error_category(std::string_view text) noexcept
{
    return is_token(text, 64, [](char c) {
        return is_lower(c) || is_digit(c) || c == '.' || c == '_';
    });
}

/// The value of an "error", such as `unknown_op`.
bool is_error_value(std::string_view text) noexcept
{
    return is_token(text, 64, [](char c) {
        return is_lower(c) || is_digit(c) || c == '_';
    });
}

/// A "code" of a "status": a token, or nothing.
bool is_status_code(std::string_view text) noexcept
{
    return text.empty() || is_token(text, 64, [](char c) {
               return is_lower(c) || is_digit(c) || c == '.' || c == '-' ||
                      c == '_';
           });
}

/// One number of a version: digits, no leading zero, at most 65535.
std::optional<std::uint16_t> version_part(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 5)
        return std::nullopt;
    if (text.size() > 1 && text.front() == '0')
        return std::nullopt;
    std::uint32_t number = 0;
    for (const char c : text)
    {
        if (!is_digit(c))
            return std::nullopt;
        number = number * 10u + static_cast<std::uint32_t>(c - '0');
    }
    if (number > 65535u)
        return std::nullopt;
    return static_cast<std::uint16_t>(number);
}

/// `major.minor` and nothing else.
std::optional<api_version> api_version_from_text(std::string_view text) noexcept
{
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos)
        return std::nullopt;
    const auto major = version_part(text.substr(0, dot));
    const auto minor = version_part(text.substr(dot + 1));
    if (!major || !minor)
        return std::nullopt;
    return api_version{*major, *minor};
}

std::string version_to_text(api_version version)
{
    std::string out = std::to_string(static_cast<unsigned>(version.major));
    out += '.';
    out += std::to_string(static_cast<unsigned>(version.minor));
    return out;
}

// -- UTF-8 --------------------------------------------------------------------

struct code_point
{
    std::uint32_t value = 0;
    /// Bytes it takes; 0 when the bytes at the place are not UTF-8.
    std::size_t length = 0;
};

/// The character that starts at `at`.  Overlong forms, surrogates and
/// anything past U+10FFFF are refused, as the JSON reader refuses them.
code_point read_utf8(std::string_view text, std::size_t at) noexcept
{
    const auto byte_at = [&](std::size_t index) {
        return static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(text[index]));
    };
    const std::uint32_t first = byte_at(at);
    if (first < 0x80)
        return {first, 1};
    std::size_t length = 0;
    std::uint32_t value = 0;
    std::uint32_t least = 0;
    if (first >= 0xc2 && first <= 0xdf)
    {
        length = 2;
        value = first & 0x1fu;
        least = 0x80;
    }
    else if (first >= 0xe0 && first <= 0xef)
    {
        length = 3;
        value = first & 0x0fu;
        least = 0x800;
    }
    else if (first >= 0xf0 && first <= 0xf4)
    {
        length = 4;
        value = first & 0x07u;
        least = 0x10000;
    }
    else
        return {};
    if (text.size() - at < length)
        return {};
    for (std::size_t i = 1; i < length; ++i)
    {
        const std::uint32_t next = byte_at(at + i);
        if ((next & 0xc0u) != 0x80u)
            return {};
        value = (value << 6) | (next & 0x3fu);
    }
    if (value < least || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
        return {};
    return {value, length};
}

/// The characters `detail::clean_text` refuses, by code point.
bool is_plain(std::uint32_t value) noexcept
{
    if (value < 0x20 || (value >= 0x7f && value <= 0x9f))
        return false;
    // The Arabic letter mark, the direction marks, the line and paragraph
    // separators, the embeddings and overrides, the isolates and the byte
    // order mark.
    return value != 0x61c && value != 0x200e && value != 0x200f &&
           !(value >= 0x2028 && value <= 0x202e) &&
           !(value >= 0x2066 && value <= 0x2069) && value != 0xfeff;
}

/// `text` cut to at most `most` bytes at the edge of a character, with each
/// character `detail::clean_text` would refuse, and each byte that is not
/// UTF-8, replaced by a space.  What it returns always passes the check.
std::string plain_prefix(std::string_view text, std::size_t most)
{
    std::string out;
    out.reserve(std::min(text.size(), most));
    std::size_t at = 0;
    while (at < text.size())
    {
        const code_point point = read_utf8(text, at);
        const bool keep = point.length != 0 && is_plain(point.value);
        const std::size_t used = point.length == 0 ? 1 : point.length;
        if (out.size() + (keep ? used : 1) > most)
            break;
        if (keep)
            out.append(text.substr(at, used));
        else
            out += ' ';
        at += used;
    }
    return out;
}

// -- reading ------------------------------------------------------------------

/// The reader stops at the first thing that is wrong and keeps the reason.
struct reading
{
    std::string problem;

    bool failed() const noexcept { return !problem.empty(); }

    void fail(std::string_view where, std::string_view what)
    {
        if (failed())
            return;
        problem.assign(where);
        problem += ": ";
        problem += what;
    }
};

std::string_view view_of(const json::string& text) noexcept
{
    return {text.data(), text.size()};
}

/// `message.key`, or `key` alone for the members every message has.
std::string member(std::string_view where, std::string_view key)
{
    std::string out(where);
    if (!out.empty())
        out += '.';
    out += key;
    return out;
}

std::string bytes_text(std::size_t count)
{
    std::string out = std::to_string(count);
    out += " bytes";
    return out;
}

/// The member as a string.  Null when it is absent or something else; an
/// absent member is an error only when `required`.
const json::string* string_of(reading& state, const json::object& object,
                              std::string_view where, std::string_view key,
                              bool required)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr)
    {
        if (required)
            state.fail(member(where, key), "is required");
        return nullptr;
    }
    if (!value->is_string())
    {
        state.fail(member(where, key), "must be a string");
        return nullptr;
    }
    return &value->get_string();
}

/// A whole number of 0 or more.
std::optional<std::uint64_t> whole_number(const json::value& value) noexcept
{
    if (value.is_uint64())
        return value.get_uint64();
    if (value.is_int64() && value.get_int64() >= 0)
        return static_cast<std::uint64_t>(value.get_int64());
    return std::nullopt;
}

/// The member as a whole number from `least` to `most`.  Nothing when it is
/// absent or not one; an absent member is an error only when `required`.
std::optional<std::uint64_t> number_of(reading& state,
                                       const json::object& object,
                                       std::string_view where,
                                       std::string_view key,
                                       std::uint64_t least, std::uint64_t most,
                                       bool required)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr)
    {
        if (required)
            state.fail(member(where, key), "is required");
        return std::nullopt;
    }
    const std::optional<std::uint64_t> number = whole_number(*value);
    if (!number || *number < least || *number > most)
    {
        state.fail(member(where, key),
                   "must be a whole number from " + std::to_string(least) +
                       " to " + std::to_string(most));
        return std::nullopt;
    }
    return number;
}

/// The member as a list of at most `max_message_list` entries.  Null when it
/// is absent or something else.
const json::array* list_of(reading& state, const json::object& object,
                           std::string_view where, std::string_view key)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr)
    {
        state.fail(member(where, key), "is required");
        return nullptr;
    }
    if (!value->is_array())
    {
        state.fail(member(where, key), "must be a list");
        return nullptr;
    }
    if (value->get_array().size() > max_message_list)
    {
        state.fail(member(where, key),
                   "has more than " + std::to_string(max_message_list) +
                       " entries");
        return nullptr;
    }
    return &value->get_array();
}

/// A text of at most `most` bytes, plain.  It may be empty.
std::string text_of(reading& state, const json::object& object,
                    std::string_view where, std::string_view key,
                    std::size_t most, bool required)
{
    const json::string* const value =
        string_of(state, object, where, key, required);
    if (value == nullptr)
        return {};
    const std::string_view text = view_of(*value);
    if (text.size() > most)
        state.fail(member(where, key),
                   "is longer than " + bytes_text(most));
    else if (!detail::clean_text(text))
        state.fail(member(where, key),
                   "must be plain text, with no control character or "
                   "direction mark");
    else
        return std::string(text);
    return {};
}

api_version version_of(reading& state, const json::object& object,
                       std::string_view where)
{
    const json::string* const value =
        string_of(state, object, where, "api", true);
    if (value == nullptr)
        return {};
    const std::optional<api_version> version =
        api_version_from_text(view_of(*value));
    if (!version)
    {
        state.fail(member(where, "api"), "must be a version such as 1.0");
        return {};
    }
    return *version;
}

/// Whether a number is anywhere in `value` that JSON cannot write.
bool all_finite(const json::value& value) noexcept
{
    if (value.is_double())
        return std::isfinite(value.get_double());
    if (value.is_array())
    {
        for (const json::value& item : value.get_array())
        {
            if (!all_finite(item))
                return false;
        }
    }
    else if (value.is_object())
    {
        for (const auto& entry : value.get_object())
        {
            if (!all_finite(entry.value()))
                return false;
        }
    }
    return true;
}

message_body read_hello(reading& state, const json::object& object)
{
    constexpr auto where = "hello"sv;
    hello_message out;
    out.api = version_of(state, object, where);

    const json::string* const id =
        string_of(state, object, where, "plugin_id", true);
    if (id != nullptr)
    {
        if (detail::valid_plugin_id(view_of(*id)))
            out.plugin_id = std::string(view_of(*id));
        else
            state.fail(member(where, "plugin_id"),
                       "must be a plugin id such as com.example.tool");
    }

    const json::string* const version =
        string_of(state, object, where, "version", true);
    if (version != nullptr)
    {
        const std::string_view text = view_of(*version);
        if (text.empty() || text.size() > 64)
            state.fail(member(where, "version"),
                       "must be 1 to 64 bytes");
        else if (!detail::clean_text(text))
            state.fail(member(where, "version"),
                       "must be plain text, with no control character or "
                       "direction mark");
        else
            out.version = std::string(text);
    }

    out.pid = number_of(state, object, where, "pid", 0, max_message_id, false)
                  .value_or(0);
    return out;
}

void read_granted(reading& state, const json::object& object,
                  hello_ack_message& out)
{
    constexpr auto where = "hello_ack"sv;
    const json::array* const list = list_of(state, object, where, "granted");
    if (list == nullptr)
        return;
    for (const json::value& item : *list)
    {
        const std::optional<permission> parsed =
            item.is_string() ? parse_permission(view_of(item.get_string()))
                             : std::nullopt;
        if (!parsed)
        {
            state.fail(member(where, "granted"),
                       "holds something that is not a permission this "
                       "version knows");
            return;
        }
        if (std::ranges::find(out.granted, *parsed) != out.granted.end())
        {
            state.fail(member(where, "granted"),
                       "holds a permission twice");
            return;
        }
        out.granted.push_back(*parsed);
    }
}

void read_capabilities(reading& state, const json::object& object,
                       hello_ack_message& out)
{
    constexpr auto where = "hello_ack"sv;
    const json::array* const list =
        list_of(state, object, where, "capabilities");
    if (list == nullptr)
        return;
    for (const json::value& item : *list)
    {
        if (!item.is_string() || !is_capability(view_of(item.get_string())))
        {
            state.fail(member(where, "capabilities"),
                       "holds something that is not 1 to 64 lower-case "
                       "letters, digits and underscores");
            return;
        }
        const std::string_view name = view_of(item.get_string());
        if (std::ranges::find(out.capabilities, name) !=
            out.capabilities.end())
        {
            state.fail(member(where, "capabilities"),
                       "holds a name twice");
            return;
        }
        out.capabilities.emplace_back(name);
    }
}

struct limit_field
{
    std::string_view key;
    std::uint32_t api_limits::*field = nullptr;
};

constexpr std::array limit_fields{
    limit_field{"frames_per_second"sv, &api_limits::frames_per_second},
    limit_field{"frames_burst"sv, &api_limits::frames_burst},
    limit_field{"bytes_per_second"sv, &api_limits::bytes_per_second},
    limit_field{"outstanding_requests"sv, &api_limits::outstanding_requests},
    limit_field{"log_lines_per_second"sv, &api_limits::log_lines_per_second},
    limit_field{"instances"sv, &api_limits::instances}};

void read_limits(reading& state, const json::object& object,
                 hello_ack_message& out)
{
    constexpr auto where = "hello_ack"sv;
    const json::value* const value = object.if_contains("limits");
    if (value == nullptr)
        return;
    if (!value->is_object())
    {
        state.fail(member(where, "limits"), "must be an object");
        return;
    }
    const std::string place = member(where, "limits");
    for (const limit_field& entry : limit_fields)
    {
        const auto number = number_of(state, value->get_object(), place,
                                      entry.key, 1, 4294967295u, false);
        if (number)
            out.limits.*(entry.field) = static_cast<std::uint32_t>(*number);
    }
}

void read_settings(reading& state, const json::object& object,
                   hello_ack_message& out)
{
    constexpr auto where = "hello_ack"sv;
    const json::value* const value = object.if_contains("settings");
    if (value == nullptr)
        return;
    if (!value->is_object())
    {
        state.fail(member(where, "settings"), "must be a JSON object");
        return;
    }
    if (!all_finite(*value))
    {
        state.fail(member(where, "settings"),
                   "holds a number that JSON cannot write");
        return;
    }
    std::string text = json::serialize(*value);
    // What is written must read back as the same text.  A number the JSON
    // reader rounds differently the second time is not accepted, so that
    // writing a message and reading it again never changes its settings.
    boost::system::error_code failed;
    const json::value again = json::parse(text, failed);
    if (failed || json::serialize(again) != text)
    {
        state.fail(member(where, "settings"),
                   "holds a number that does not read back the same");
        return;
    }
    out.settings = std::move(text);
}

message_body read_hello_ack(reading& state, const json::object& object)
{
    constexpr auto where = "hello_ack"sv;
    hello_ack_message out;
    out.api = version_of(state, object, where);
    read_granted(state, object, out);
    read_capabilities(state, object, out);
    read_limits(state, object, out);
    out.data_directory = text_of(state, object, where, "data_directory",
                                 max_message_text, true);
    read_settings(state, object, out);
    return out;
}

message_body read_ping(reading&, const json::object&)
{
    return ping_message{};
}

message_body read_pong(reading&, const json::object&)
{
    return pong_message{};
}

message_body read_bye(reading&, const json::object&)
{
    return bye_message{};
}

message_body read_ok(reading&, const json::object&)
{
    return ok_message{};
}

message_body read_status(reading& state, const json::object& object)
{
    constexpr auto where = "status"sv;
    status_message out;
    const json::string* const level =
        string_of(state, object, where, "level", true);
    if (level != nullptr)
    {
        constexpr std::array levels{status_level::ok, status_level::warning,
                                    status_level::error};
        const auto found = std::ranges::find_if(levels, [&](status_level one) {
            return to_string(one) == view_of(*level);
        });
        if (found == levels.end())
            state.fail(member(where, "level"),
                       "must be ok, warning or error");
        else
            out.level = *found;
    }
    out.text = text_of(state, object, where, "text", max_status_text, false);
    const json::string* const code =
        string_of(state, object, where, "code", false);
    if (code != nullptr)
    {
        if (is_status_code(view_of(*code)))
            out.code = std::string(view_of(*code));
        else
            state.fail(member(where, "code"),
                       "must be at most 64 lower-case letters, digits, dots, "
                       "hyphens and underscores");
    }
    return out;
}

message_body read_log(reading& state, const json::object& object)
{
    constexpr auto where = "log"sv;
    log_message out;
    const json::string* const level =
        string_of(state, object, where, "level", true);
    if (level != nullptr)
    {
        constexpr std::array levels{log_level::debug, log_level::info,
                                    log_level::warning, log_level::error};
        const auto found = std::ranges::find_if(levels, [&](log_level one) {
            return to_string(one) == view_of(*level);
        });
        if (found == levels.end())
            state.fail(member(where, "level"),
                       "must be debug, info, warning or error");
        else
            out.level = *found;
    }
    out.text = text_of(state, object, where, "text", max_message_text, true);
    return out;
}

message_body read_shutdown(reading& state, const json::object& object)
{
    shutdown_message out;
    out.reason = text_of(state, object, "shutdown", "reason", 64, false);
    return out;
}

message_body read_error(reading& state, const json::object& object)
{
    constexpr auto where = "error"sv;
    error_message out;
    const json::string* const category =
        string_of(state, object, where, "category", true);
    if (category != nullptr)
    {
        if (is_error_category(view_of(*category)))
            out.category = std::string(view_of(*category));
        else
            state.fail(member(where, "category"),
                       "must be 1 to 64 lower-case letters, digits, dots and "
                       "underscores");
    }
    const json::string* const value =
        string_of(state, object, where, "value", true);
    if (value != nullptr)
    {
        if (is_error_value(view_of(*value)))
            out.value = std::string(view_of(*value));
        else
            state.fail(member(where, "value"),
                       "must be 1 to 64 lower-case letters, digits and "
                       "underscores");
    }
    out.message =
        text_of(state, object, where, "message", max_message_text, false);
    return out;
}

using body_reader = message_body (*)(reading&, const json::object&);

struct known_op
{
    std::string_view name;
    body_reader read = nullptr;
};

/// In the order of `message_body`, so that `op_of` can use the index.
constexpr std::array known_ops{
    known_op{"hello"sv, &read_hello},
    known_op{"hello_ack"sv, &read_hello_ack},
    known_op{"ping"sv, &read_ping},
    known_op{"pong"sv, &read_pong},
    known_op{"status"sv, &read_status},
    known_op{"log"sv, &read_log},
    known_op{"shutdown"sv, &read_shutdown},
    known_op{"bye"sv, &read_bye},
    known_op{"ok"sv, &read_ok},
    known_op{"error"sv, &read_error}};

static_assert(known_ops.size() + 1 == std::variant_size_v<message_body>);

const known_op* find_known(std::string_view op) noexcept
{
    const auto found = std::ranges::find(known_ops, op, &known_op::name);
    return found == known_ops.end() ? nullptr : &*found;
}

/// What a failed `parse_strict` says, without anything the plugin sent: it
/// quotes a repeated key.
std::string_view why_not_json(const std::string& what) noexcept
{
    if (what.find("is given twice") != std::string::npos)
        return "has a key given twice in one object";
    return what;
}

/// The member "id" or "re": absent is 0.
std::uint64_t reference_of(reading& state, const json::object& object,
                           std::string_view key)
{
    return number_of(state, object, {}, key, 1, max_message_id, false)
        .value_or(0);
}

api_message read_message(reading& state, const agent::frame& frame)
{
    api_message out;
    // The size is checked before anything is parsed, so that the work a
    // header can ask for is bounded by a number that is known.
    if (frame.header.size() > max_header_size)
    {
        state.fail("header", "is larger than " +
                                 std::to_string(max_header_size) + " bytes");
        return out;
    }
    std::string what;
    const std::optional<json::value> root =
        detail::parse_strict(frame.header, max_header_depth, what);
    if (!root)
    {
        state.fail("header", why_not_json(what));
        return out;
    }
    if (!root->is_object())
    {
        state.fail("header", "must be a JSON object");
        return out;
    }
    const json::object& object = root->get_object();

    const json::string* const op = string_of(state, object, {}, "op", true);
    if (op == nullptr)
        return out;
    if (!is_op_name(view_of(*op)))
    {
        state.fail("op", "must be a message name such as hello or "
                         "generator.start");
        return out;
    }
    out.op = std::string(view_of(*op));
    out.id = reference_of(state, object, "id");
    out.re = reference_of(state, object, "re");
    if (state.failed())
        return out;
    if (object.contains("id") && object.contains("re"))
    {
        state.fail("id", "a message has an id or a re, not both");
        return out;
    }

    if (const known_op* const known = find_known(out.op))
        out.body = known->read(state, object);
    else
        out.body = other_message{frame.header};
    out.blob = frame.blob;
    return out;
}

// -- writing ------------------------------------------------------------------

json::string text_value(std::string_view text)
{
    return json::string(text.data(), text.size());
}

json::array list_of_texts(const std::vector<std::string>& items)
{
    json::array out;
    out.reserve(items.size());
    for (const std::string& item : items)
        out.emplace_back(text_value(item));
    return out;
}

/// A member that is left out when the text is empty.
void put_optional(json::object& header, std::string_view key,
                  const std::string& text)
{
    if (!text.empty())
        header.emplace(key, text_value(text));
}

bool write_body(json::object& header, const hello_message& body)
{
    header.emplace("api", text_value(version_to_text(body.api)));
    header.emplace("plugin_id", text_value(body.plugin_id));
    header.emplace("version", text_value(body.version));
    if (body.pid != 0)
        header.emplace("pid", body.pid);
    return true;
}

bool write_body(json::object& header, const hello_ack_message& body)
{
    header.emplace("api", text_value(version_to_text(body.api)));
    json::array granted;
    for (const permission item : body.granted)
        granted.emplace_back(text_value(to_string(item)));
    header.emplace("granted", std::move(granted));
    header.emplace("capabilities", list_of_texts(body.capabilities));
    json::object limits;
    for (const limit_field& entry : limit_fields)
        limits.emplace(entry.key, std::uint64_t{body.limits.*(entry.field)});
    header.emplace("limits", std::move(limits));
    header.emplace("data_directory", text_value(body.data_directory));

    std::string what;
    std::optional<json::value> settings =
        detail::parse_strict(body.settings, max_header_depth - 1, what);
    if (!settings || !settings->is_object())
        return false;
    if (!settings->get_object().empty())
        header.emplace("settings", std::move(*settings));
    return true;
}

bool write_body(json::object&, const ping_message&)
{
    return true;
}

bool write_body(json::object&, const pong_message&)
{
    return true;
}

bool write_body(json::object& header, const status_message& body)
{
    header.emplace("level", text_value(to_string(body.level)));
    put_optional(header, "text", body.text);
    put_optional(header, "code", body.code);
    return true;
}

bool write_body(json::object& header, const log_message& body)
{
    header.emplace("level", text_value(to_string(body.level)));
    header.emplace("text", text_value(body.text));
    return true;
}

bool write_body(json::object& header, const shutdown_message& body)
{
    put_optional(header, "reason", body.reason);
    return true;
}

bool write_body(json::object&, const bye_message&)
{
    return true;
}

bool write_body(json::object&, const ok_message&)
{
    return true;
}

bool write_body(json::object& header, const error_message& body)
{
    header.emplace("category", text_value(body.category));
    header.emplace("value", text_value(body.value));
    put_optional(header, "message", body.message);
    return true;
}

/// An `other_message` is not written member by member.
bool write_body(json::object&, const other_message&)
{
    return false;
}

/// The header of a message whose body this file knows.  Nothing when `op`
/// names another message, or the body cannot be written.
std::optional<std::string> write_header(const api_message& message)
{
    const std::string_view name = op_of(message.body);
    if (!message.op.empty() && message.op != name)
        return std::nullopt;
    if (message.id != 0 && message.re != 0)
        return std::nullopt;

    json::object header;
    header.emplace("op", text_value(name));
    if (message.id != 0)
        header.emplace("id", message.id);
    if (message.re != 0)
        header.emplace("re", message.re);
    const bool written = std::visit(
        [&](const auto& body) { return write_body(header, body); },
        message.body);
    if (!written)
        return std::nullopt;
    return json::serialize(header);
}

/// Whether `body.header` is a header for `message`: one JSON object whose
/// "op", "id" and "re" are the message's, and whose op this file does not
/// know, since it would be read as another body.
bool matches_header(const api_message& message, const other_message& body)
{
    if (!is_op_name(message.op) || find_known(message.op) != nullptr)
        return false;
    std::string what;
    const std::optional<json::value> root =
        detail::parse_strict(body.header, max_header_depth, what);
    if (!root || !root->is_object())
        return false;
    const json::object& object = root->get_object();
    const json::value* const op = object.if_contains("op");
    if (op == nullptr || !op->is_string() ||
        view_of(op->get_string()) != message.op)
        return false;
    const auto matches = [&](std::string_view key, std::uint64_t expected) {
        const json::value* const value = object.if_contains(key);
        if (value == nullptr)
            return expected == 0;
        const std::optional<std::uint64_t> number = whole_number(*value);
        return number && *number == expected;
    };
    return matches("id", message.id) && matches("re", message.re);
}

} // namespace

// -- errors -------------------------------------------------------------------

namespace
{

class api_category_impl final : public boost::system::error_category
{
public:
    const char* name() const noexcept override { return "vss.plugin.api"; }

    std::string message(int value) const override
    {
        switch (static_cast<api_errc>(value))
        {
        case api_errc::success:
            return "success";
        case api_errc::unknown_op:
            return "the operation is not known";
        case api_errc::bad_request:
            return "the message is not as the plugin API describes it";
        case api_errc::refused:
            return "the request was refused";
        case api_errc::not_granted:
            return "the plugin was not granted the permission the operation "
                   "needs";
        case api_errc::not_declared:
            return "the plugin's manifest does not declare what the "
                   "operation uses";
        case api_errc::timeout:
            return "the answer did not come in time";
        case api_errc::busy:
            return "too many requests are waiting for an answer";
        case api_errc::unsupported:
            return "this version does not have the operation or the option";
        case api_errc::plugin_unavailable:
            return "the plugin is not running";
        }
        return "unknown plugin API error";
    }
};

} // namespace

const boost::system::error_category& api_category() noexcept
{
    static const api_category_impl instance;
    return instance;
}

boost::system::error_code make_error_code(api_errc value) noexcept
{
    return {std::to_underlying(value), api_category()};
}

std::string_view to_string(api_errc value) noexcept
{
    switch (value)
    {
    case api_errc::success:
        return "success";
    case api_errc::unknown_op:
        return "unknown_op";
    case api_errc::bad_request:
        return "bad_request";
    case api_errc::refused:
        return "refused";
    case api_errc::not_granted:
        return "not_granted";
    case api_errc::not_declared:
        return "not_declared";
    case api_errc::timeout:
        return "timeout";
    case api_errc::busy:
        return "busy";
    case api_errc::unsupported:
        return "unsupported";
    case api_errc::plugin_unavailable:
        return "plugin_unavailable";
    }
    return "unknown";
}

std::string_view to_string(status_level value) noexcept
{
    switch (value)
    {
    case status_level::ok:
        return "ok";
    case status_level::warning:
        return "warning";
    case status_level::error:
        return "error";
    }
    return "unknown";
}

std::string_view to_string(log_level value) noexcept
{
    switch (value)
    {
    case log_level::debug:
        return "debug";
    case log_level::info:
        return "info";
    case log_level::warning:
        return "warning";
    case log_level::error:
        return "error";
    }
    return "unknown";
}

// -- the messages -------------------------------------------------------------

bool is_op_name(std::string_view text) noexcept
{
    if (text.empty() || text.size() > max_op_length || !is_lower(text.front()))
        return false;
    char previous = '\0';
    for (const char c : text)
    {
        if (!is_lower(c) && !is_digit(c) && c != '_' && c != '.')
            return false;
        if (c == '.' && previous == '.')
            return false;
        previous = c;
    }
    return previous != '.';
}

std::string_view op_of(const message_body& body) noexcept
{
    const std::size_t index = body.index();
    if (index >= known_ops.size())
        return {};
    return known_ops[index].name;
}

result<api_message> decode_message(const agent::frame& frame,
                                   std::string* explain)
{
    try
    {
        reading state;
        api_message message = read_message(state, frame);
        if (state.failed())
        {
            if (explain != nullptr)
                *explain = state.problem;
            return make_error_code(api_errc::bad_request);
        }
        if (explain != nullptr)
            explain->clear();
        return message;
    }
    catch (...)
    {
        // Only running out of memory gets here.  The frame was not wrong.
        return make_error_code(errc::internal);
    }
}

result<agent::frame> encode_message(const api_message& message)
{
    try
    {
        agent::frame out;
        out.blob = message.blob;
        if (const auto* const other =
                std::get_if<other_message>(&message.body))
        {
            if (!matches_header(message, *other))
                return make_error_code(api_errc::bad_request);
            out.header = other->header;
        }
        else
        {
            std::optional<std::string> header = write_header(message);
            if (!header)
                return make_error_code(api_errc::bad_request);
            out.header = std::move(*header);
        }
        // Whatever the reader would refuse is refused here: a text that is
        // not plain or not UTF-8, a list or a text over its limit, a number
        // over its range and a document that nests too deep.
        if (decode_message(out).has_error())
            return make_error_code(api_errc::bad_request);
        return out;
    }
    catch (...)
    {
        return make_error_code(errc::internal);
    }
}

api_message make_error(std::uint64_t re, api_errc value,
                       std::string_view message)
{
    api_message out;
    out.op = "error";
    out.re = re;
    error_message body;
    body.category = std::string(api_category().name());
    body.value = std::string(to_string(value));
    body.message = plain_prefix(message, max_message_text);
    out.body = std::move(body);
    return out;
}

} // namespace vss::plugin
