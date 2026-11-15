//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "api_codec.hpp"

#include <algorithm>
#include <cmath>

namespace vss::plugin
{

std::string_view to_string(api_side value) noexcept
{
    switch (value)
    {
    case api_side::host:
        return "host";
    case api_side::plugin:
        return "plugin";
    }
    return "unknown";
}

std::string_view to_string(api_role value) noexcept
{
    switch (value)
    {
    case api_role::request:
        return "request";
    case api_role::notification:
        return "notification";
    case api_role::answer:
        return "answer";
    }
    return "unknown";
}

namespace detail
{

namespace
{

constexpr bool is_lower(char c) noexcept
{
    return c >= 'a' && c <= 'z';
}

constexpr bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

std::string bytes_text(std::size_t count)
{
    std::string out = std::to_string(count);
    out += " bytes";
    return out;
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
        for (const auto& item : value.get_object())
        {
            if (!all_finite(item.value()))
                return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> whole_number(const json::value& value) noexcept
{
    if (value.is_uint64())
        return value.get_uint64();
    if (value.is_int64() && value.get_int64() >= 0)
        return static_cast<std::uint64_t>(value.get_int64());
    return std::nullopt;
}

/// The member, or null.  An absent member is an error only when `required`.
const json::value* find_member(reading& state, const json::object& object,
                               std::string_view where, std::string_view key,
                               bool required)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr && required)
        state.fail(member(where, key), "is required");
    return value;
}

std::string why_not_json(const std::string& what)
{
    if (what.find("is given twice") != std::string::npos)
        return "has a key given twice in one object";
    return what;
}

char hex_digit(unsigned value) noexcept
{
    return "0123456789abcdef"[value & 0xfu];
}

} // namespace

std::string_view view_of(const json::string& text) noexcept
{
    return {text.data(), text.size()};
}

std::string member(std::string_view where, std::string_view key)
{
    std::string out(where);
    if (!out.empty())
        out += '.';
    out += key;
    return out;
}

std::string entry(std::string_view where, std::size_t index)
{
    std::string out(where);
    out += '[';
    out += std::to_string(index);
    out += ']';
    return out;
}

std::optional<std::uint64_t> number_of(reading& state,
                                       const json::object& object,
                                       std::string_view where,
                                       std::string_view key,
                                       std::uint64_t least, std::uint64_t most,
                                       bool required)
{
    const json::value* const value =
        find_member(state, object, where, key, required);
    if (value == nullptr)
        return std::nullopt;
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

std::optional<double> real_of(reading& state, const json::object& object,
                              std::string_view where, std::string_view key,
                              double least, double most, bool required)
{
    const json::value* const value =
        find_member(state, object, where, key, required);
    if (value == nullptr)
        return std::nullopt;
    double number = 0;
    if (value->is_double())
        number = value->get_double();
    else if (value->is_int64())
        number = static_cast<double>(value->get_int64());
    else if (value->is_uint64())
        number = static_cast<double>(value->get_uint64());
    else
        number = std::nan("");
    if (!std::isfinite(number) || number < least || number > most)
    {
        state.fail(member(where, key), "must be a number from " +
                                           std::to_string(least) + " to " +
                                           std::to_string(most));
        return std::nullopt;
    }
    return number;
}

std::optional<bool> bool_of(reading& state, const json::object& object,
                            std::string_view where, std::string_view key,
                            bool required)
{
    const json::value* const value =
        find_member(state, object, where, key, required);
    if (value == nullptr)
        return std::nullopt;
    if (!value->is_bool())
    {
        state.fail(member(where, key), "must be true or false");
        return std::nullopt;
    }
    return value->get_bool();
}

std::string text_of(reading& state, const json::object& object,
                    std::string_view where, std::string_view key,
                    std::size_t most, bool required, bool not_empty)
{
    const json::value* const value =
        find_member(state, object, where, key, required);
    if (value == nullptr)
        return {};
    if (!value->is_string())
    {
        state.fail(member(where, key), "must be a string");
        return {};
    }
    const std::string_view text = view_of(value->get_string());
    if (text.size() > most)
        state.fail(member(where, key), "is longer than " + bytes_text(most));
    else if (not_empty && text.empty())
        state.fail(member(where, key), "must not be empty");
    else if (!clean_text(text))
        state.fail(member(where, key),
                   "must be plain text, with no control character or "
                   "direction mark");
    else
        return std::string(text);
    return {};
}

std::string token_of(reading& state, const json::object& object,
                     std::string_view where, std::string_view key,
                     std::size_t most, bool required)
{
    const json::value* const value =
        find_member(state, object, where, key, required);
    if (value == nullptr)
        return {};
    if (!value->is_string())
    {
        state.fail(member(where, key), "must be a string");
        return {};
    }
    const std::string_view text = view_of(value->get_string());
    const bool good =
        !text.empty() && text.size() <= most &&
        std::ranges::all_of(text, [](char c) {
            return is_lower(c) || is_digit(c) || c == '.' || c == '-' ||
                   c == '_';
        });
    if (!good)
    {
        state.fail(member(where, key),
                   "must be 1 to " + std::to_string(most) +
                       " lower-case letters, digits, dots, hyphens and "
                       "underscores");
        return {};
    }
    return std::string(text);
}

const json::array* list_of(reading& state, const json::object& object,
                           std::string_view where, std::string_view key,
                           std::size_t most, bool required)
{
    const json::value* const value =
        find_member(state, object, where, key, required);
    if (value == nullptr)
        return nullptr;
    if (!value->is_array())
    {
        state.fail(member(where, key), "must be a list");
        return nullptr;
    }
    if (value->get_array().size() > most)
    {
        state.fail(member(where, key),
                   "has more than " + std::to_string(most) + " entries");
        return nullptr;
    }
    return &value->get_array();
}

const json::object* object_of(reading& state, const json::object& object,
                              std::string_view where, std::string_view key,
                              bool required)
{
    const json::value* const value =
        find_member(state, object, where, key, required);
    if (value == nullptr)
        return nullptr;
    if (!value->is_object())
    {
        state.fail(member(where, key), "must be an object");
        return nullptr;
    }
    return &value->get_object();
}

const json::object* entry_object(reading& state, const json::value& value,
                                 std::string_view where)
{
    if (!value.is_object())
    {
        state.fail(where, "must be an object");
        return nullptr;
    }
    return &value.get_object();
}

std::string object_text_of(reading& state, const json::object& object,
                           std::string_view where, std::string_view key,
                           bool required)
{
    const json::value* const value =
        find_member(state, object, where, key, required);
    if (value == nullptr)
        return "{}";
    if (!value->is_object())
    {
        state.fail(member(where, key), "must be a JSON object");
        return "{}";
    }
    if (!all_finite(*value))
    {
        state.fail(member(where, key),
                   "holds a number that JSON cannot write");
        return "{}";
    }
    std::string text = json::serialize(*value);
    // What is written must read back as the same text, so that writing a
    // message and reading it again never changes it.
    boost::system::error_code failed;
    const json::value again = json::parse(text, failed);
    if (failed || json::serialize(again) != text)
    {
        state.fail(member(where, key),
                   "holds a number that does not read back the same");
        return "{}";
    }
    return text;
}

byte_buffer hex_of(reading& state, const json::object& object,
                   std::string_view where, std::string_view key,
                   std::size_t most, bool required)
{
    const json::value* const value =
        find_member(state, object, where, key, required);
    if (value == nullptr)
        return {};
    if (!value->is_string())
    {
        state.fail(member(where, key), "must be a string");
        return {};
    }
    const std::string_view text = view_of(value->get_string());
    // The length is checked before anything is allocated for it.
    if (text.empty() || text.size() % 2 != 0 || text.size() / 2 > most)
    {
        state.fail(member(where, key),
                   "must be 1 to " + bytes_text(most) +
                       " written as an even number of hex characters");
        return {};
    }
    byte_buffer out(text.size() / 2);
    if (!hex_to_bytes(text, out))
    {
        state.fail(member(where, key),
                   "must be written in lower-case hex characters");
        return {};
    }
    return out;
}

void no_blob(reading& state, std::string_view where, const byte_buffer& blob)
{
    if (!blob.empty())
        state.fail(where, "has no bytes, and the frame has a blob");
}

void blob_between(reading& state, std::string_view where,
                  const byte_buffer& blob, std::size_t least,
                  std::size_t most)
{
    if (blob.size() < least || blob.size() > most)
        state.fail(where, "has a blob of " + bytes_text(blob.size()) +
                              ", and it must be " + std::to_string(least) +
                              " to " + bytes_text(most));
}

json::string text_value(std::string_view text)
{
    return json::string(text.data(), text.size());
}

void put_optional(json::object& header, std::string_view key,
                  const std::string& text)
{
    if (!text.empty())
        header.emplace(key, text_value(text));
}

bool put_object_text(json::object& header, std::string_view key,
                     const std::string& text)
{
    std::string what;
    std::optional<json::value> value =
        parse_strict(text, max_header_depth - 1, what);
    if (!value || !value->is_object())
        return false;
    header.emplace(key, std::move(*value));
    return true;
}

std::string hex_text(const byte_buffer& bytes)
{
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes)
    {
        out += hex_digit(byte >> 4);
        out += hex_digit(byte);
    }
    return out;
}

std::optional<json::value> open_message(reading& state,
                                        const api_message& message,
                                        std::string_view op, api_role role)
{
    const auto* const other = std::get_if<other_message>(&message.body);
    if (other == nullptr || message.op != op)
    {
        state.fail(op, "is not the message in this frame");
        return std::nullopt;
    }
    switch (role)
    {
    case api_role::request:
        if (message.id == 0 || message.re != 0)
            state.fail(op, "is a request and must have an id and no re");
        break;
    case api_role::notification:
        if (message.id != 0 || message.re != 0)
            state.fail(op, "is a notification and has neither an id nor a re");
        break;
    case api_role::answer:
        if (message.id != 0 || message.re == 0)
            state.fail(op, "is an answer and must have a re and no id");
        break;
    }
    if (state.failed())
        return std::nullopt;
    std::string what;
    std::optional<json::value> root =
        parse_strict(other->header, max_header_depth, what);
    if (!root)
    {
        state.fail("header", why_not_json(what));
        return std::nullopt;
    }
    if (!root->is_object())
    {
        state.fail("header", "must be a JSON object");
        return std::nullopt;
    }
    return root;
}

result<api_message> close_message(std::string_view op, api_role role,
                                  std::uint64_t number, json::object header,
                                  byte_buffer blob)
{
    api_message out;
    out.op = std::string(op);
    json::object full;
    full.emplace("op", text_value(op));
    if (role == api_role::request)
    {
        out.id = number;
        full.emplace("id", number);
    }
    else if (role == api_role::answer)
    {
        out.re = number;
        full.emplace("re", number);
    }
    if (number > max_message_id)
        return make_error_code(api_errc::bad_request);
    for (auto& item : header)
        full.insert_or_assign(item.key(), std::move(item.value()));
    out.body = other_message{json::serialize(full)};
    out.blob = std::move(blob);
    return out;
}

} // namespace detail

} // namespace vss::plugin
