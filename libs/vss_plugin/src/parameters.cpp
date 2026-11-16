//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/parameters.hpp>

#include "reader.hpp"

#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vss::plugin
{

namespace
{

namespace json = boost::json;
namespace fs = std::filesystem;

// -- words for a person ---------------------------------------------------------

/// `"id" <rest>.`
std::string line_for(std::string_view id, std::string_view rest)
{
    std::string out = detail::quoted(id);
    out.push_back(' ');
    out.append(rest);
    out.push_back('.');
    return out;
}

parameter_error refuse(std::string_view id, parameter_errc code,
                       std::string_view rest)
{
    parameter_error out;
    out.id = std::string(id);
    out.code = code;
    out.line = line_for(id, rest);
    return out;
}

std::string number_text(double value)
{
    return json::serialize(json::value(value));
}

// -- text -----------------------------------------------------------------------

/// Whether `text` is well formed UTF-8: no overlong form, no surrogate and
/// nothing above U+10FFFF.  `clean_text` leaves this to the caller.
bool valid_utf8(std::string_view text) noexcept
{
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto first = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if (first < 0x80)
        {
            ++i;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf)
        {
            extra = 1;
            code = first & 0x1fU;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            extra = 2;
            code = first & 0x0fU;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            extra = 3;
            code = first & 0x07U;
        }
        else
        {
            return false;
        }
        if (i + extra >= text.size())
            return false;
        for (std::size_t k = 1; k <= extra; ++k)
        {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            code = (code << 6) | (next & 0x3fU);
        }
        const bool overlong = (extra == 2 && code < 0x800) ||
                              (extra == 3 && code < 0x10000);
        if (overlong || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
        i += extra + 1;
    }
    return true;
}

/// Plain text: well formed UTF-8 with no control character and no mark that
/// changes the direction of the text.
bool plain_text(std::string_view text) noexcept
{
    return valid_utf8(text) && detail::clean_text(text);
}

// -- durations ------------------------------------------------------------------

/// Whole units of ns, us, ms or s, as nanoseconds.  The digits are 1 to 14,
/// which cannot overflow before the check against one day.
std::optional<std::int64_t> parse_duration(std::string_view text) noexcept
{
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9')
        ++digits;
    if (digits == 0 || digits > 14)
        return std::nullopt;
    std::int64_t count = 0;
    for (std::size_t i = 0; i < digits; ++i)
        count = count * 10 + (text[i] - '0');
    const std::string_view unit = text.substr(digits);
    std::int64_t scale = 0;
    if (unit == "ns")
        scale = 1;
    else if (unit == "us")
        scale = 1'000;
    else if (unit == "ms")
        scale = 1'000'000;
    else if (unit == "s")
        scale = 1'000'000'000;
    else
        return std::nullopt;
    if (count > max_parameter_duration_ns / scale)
        return std::nullopt;
    return count * scale;
}

std::string duration_text(std::int64_t nanoseconds)
{
    struct unit_scale
    {
        std::int64_t scale;
        std::string_view name;
    };
    constexpr std::array units{
        unit_scale{1'000'000'000, "s"}, unit_scale{1'000'000, "ms"},
        unit_scale{1'000, "us"}};
    if (nanoseconds == 0)
        return "0s";
    for (const unit_scale& unit : units)
    {
        if (nanoseconds % unit.scale == 0)
        {
            std::string out = std::to_string(nanoseconds / unit.scale);
            out.append(unit.name);
            return out;
        }
    }
    std::string out = std::to_string(nanoseconds);
    out.append("ns");
    return out;
}

// -- bytes ----------------------------------------------------------------------

std::optional<int> hex_digit(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return std::nullopt;
}

/// Pairs of hex digits, with one space allowed between two pairs: the form
/// the manifest reader takes for a default.
std::optional<std::string> parse_hex(std::string_view text)
{
    std::string out;
    std::size_t i = 0;
    while (i < text.size())
    {
        if (i + 1 >= text.size())
            return std::nullopt;
        const auto high = hex_digit(text[i]);
        const auto low = hex_digit(text[i + 1]);
        if (!high || !low)
            return std::nullopt;
        out.push_back(static_cast<char>((*high << 4) | *low));
        i += 2;
        if (i < text.size() && text[i] == ' ')
        {
            ++i;
            if (i == text.size())
                return std::nullopt;
        }
    }
    return out;
}

std::string hex_text(std::string_view bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes)
    {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

// -- paths ----------------------------------------------------------------------

bool is_separator(char c) noexcept
{
    return c == '/' || c == '\\';
}

std::string to_text(const fs::path& path)
{
    const std::u8string wide = path.u8string();
    std::string out;
    out.reserve(wide.size());
    for (const char8_t c : wide)
        out.push_back(static_cast<char>(c));
    return out;
}

fs::path from_text(std::string_view text)
{
    std::u8string wide;
    wide.reserve(text.size());
    for (const char c : text)
        wide.push_back(static_cast<char8_t>(static_cast<unsigned char>(c)));
    return fs::path(wide);
}

struct path_result
{
    std::string path;
    /// Empty when the path is good.
    std::string why;
};

path_result check_path(std::string_view text, path_kind kind)
{
    path_result out;
    if (text.empty() || text.size() > max_path_parameter_length)
    {
        out.why = "is empty or too long";
        return out;
    }
    if (!plain_text(text))
    {
        out.why = "is not plain text";
        return out;
    }
    if (text.size() >= 2 && is_separator(text[0]) && is_separator(text[1]))
    {
        out.why = "is a network or device path";
        return out;
    }
    const fs::path given = from_text(text);
    if (!given.is_absolute())
    {
        out.why = "is not a full path";
        return out;
    }
    // A colon after the drive names a stream of a file, not a file.
    const std::size_t root_name = to_text(given.root_name()).size();
    if (root_name != 0 && text.substr(root_name).find(':') !=
                              std::string_view::npos)
    {
        out.why = "has a colon after the drive";
        return out;
    }
    // Any `..` is refused as given.  Folding it away first would let the
    // path reach somewhere the user did not name.
    for (const fs::path& part : given)
    {
        if (part == "..")
        {
            out.why = "has a \"..\" in it";
            return out;
        }
    }
    fs::path normal = given.lexically_normal();
    if (normal.relative_path().empty())
    {
        out.why = "is a root or a drive";
        return out;
    }
    if (!normal.has_filename())
    {
        if (kind == path_kind::file)
        {
            out.why = "names a folder, not a file";
            return out;
        }
        normal = normal.parent_path();
    }
    std::string result = to_text(normal);
    if (result.size() > max_path_parameter_length)
    {
        out.why = "is empty or too long";
        return out;
    }
    out.path = std::move(result);
    return out;
}

// -- one value ------------------------------------------------------------------

template <class T>
std::optional<parameter_error>
check_limits(const parameter& declared, const T& value,
             std::string (*show)(T))
{
    const T* const least = std::get_if<T>(&declared.min);
    const T* const most = std::get_if<T>(&declared.max);
    if (least != nullptr && value < *least)
    {
        std::string rest = "must be at least ";
        rest.append(show(*least));
        return refuse(declared.id, parameter_errc::below_min, rest);
    }
    if (most != nullptr && value > *most)
    {
        std::string rest = "must be at most ";
        rest.append(show(*most));
        return refuse(declared.id, parameter_errc::above_max, rest);
    }
    return std::nullopt;
}

std::string integer_text(std::int64_t value)
{
    return std::to_string(value);
}

std::optional<parameter_error> check_integer(const parameter& declared,
                                             const raw_value& given,
                                             checked_parameter& out)
{
    const auto* const value = std::get_if<std::int64_t>(&given);
    if (value == nullptr)
        return refuse(declared.id, parameter_errc::wrong_type,
                      "must be a whole number");
    if (auto error = check_limits<std::int64_t>(declared, *value,
                                                integer_text))
        return error;
    out.value = *value;
    return std::nullopt;
}

std::optional<parameter_error> check_number(const parameter& declared,
                                            const raw_value& given,
                                            checked_parameter& out)
{
    double value = 0.0;
    if (const auto* const real = std::get_if<double>(&given))
    {
        value = *real;
    }
    else if (const auto* const whole = std::get_if<std::int64_t>(&given))
    {
        value = static_cast<double>(*whole);
        // Above 2^53 a whole number may not be held exactly.
        if (*whole > (std::int64_t{1} << 53) ||
            *whole < -(std::int64_t{1} << 53))
            return refuse(declared.id, parameter_errc::bad_format,
                          "must be a number that fits exactly");
    }
    else
    {
        return refuse(declared.id, parameter_errc::wrong_type,
                      "must be a number");
    }
    if (!(value >= std::numeric_limits<double>::lowest() &&
          value <= std::numeric_limits<double>::max()))
        return refuse(declared.id, parameter_errc::bad_format,
                      "must be a finite number");
    if (auto error = check_limits<double>(declared, value, number_text))
        return error;
    out.value = value;
    return std::nullopt;
}

std::optional<parameter_error> check_duration(const parameter& declared,
                                              const raw_value& given,
                                              checked_parameter& out)
{
    const auto* const text = std::get_if<std::string>(&given);
    if (text == nullptr)
        return refuse(declared.id, parameter_errc::wrong_type,
                      "must be a duration such as 3ms");
    const auto nanoseconds = parse_duration(*text);
    if (!nanoseconds)
        return refuse(declared.id, parameter_errc::bad_format,
                      "must be whole units of ns, us, ms or s, up to 1 day");
    if (auto error = check_limits<std::int64_t>(declared, *nanoseconds,
                                                duration_text))
        return error;
    out.value = *nanoseconds;
    return std::nullopt;
}

std::optional<parameter_error> check_text(const parameter& declared,
                                          const raw_value& given,
                                          checked_parameter& out)
{
    const auto* const text = std::get_if<std::string>(&given);
    if (text == nullptr)
        return refuse(declared.id, parameter_errc::wrong_type,
                      "must be text");
    if (text->size() > declared.max_length)
        return refuse(declared.id, parameter_errc::too_long,
                      "is too long");
    if (!plain_text(*text))
        return refuse(declared.id, parameter_errc::bad_format,
                      "must be plain text");
    out.value = *text;
    return std::nullopt;
}

std::optional<parameter_error> check_bytes(const parameter& declared,
                                           const raw_value& given,
                                           checked_parameter& out)
{
    const auto* const text = std::get_if<std::string>(&given);
    if (text == nullptr)
        return refuse(declared.id, parameter_errc::wrong_type,
                      "must be hexadecimal text such as 02 31 03");
    if (text->size() > max_parameter_text_size)
        return refuse(declared.id, parameter_errc::too_long,
                      "is too long");
    auto bytes = parse_hex(*text);
    if (!bytes)
        return refuse(declared.id, parameter_errc::bad_format,
                      "must be pairs of hexadecimal digits");
    if (bytes->size() > declared.max_length)
        return refuse(declared.id, parameter_errc::too_long,
                      "has too many bytes");
    out.value = std::move(*bytes);
    return std::nullopt;
}

std::optional<parameter_error> check_choice(const parameter& declared,
                                            const raw_value& given,
                                            checked_parameter& out)
{
    const auto* const text = std::get_if<std::string>(&given);
    if (text == nullptr)
        return refuse(declared.id, parameter_errc::wrong_type,
                      "must be text");
    const bool known = std::ranges::any_of(
        declared.choices,
        [&](const parameter_choice& c) { return c.value == *text; });
    if (!known)
        return refuse(declared.id, parameter_errc::not_an_option,
                      "is not one of the options");
    out.value = *text;
    return std::nullopt;
}

std::optional<parameter_error> check_name(const parameter& declared,
                                          const raw_value& given,
                                          checked_parameter& out)
{
    const auto* const text = std::get_if<std::string>(&given);
    if (text == nullptr)
        return refuse(declared.id, parameter_errc::wrong_type,
                      "must be a name");
    if (text->empty() || text->size() > max_name_length)
        return refuse(declared.id, parameter_errc::too_long,
                      "must be a name of 1 to 100 bytes");
    if (!plain_text(*text))
        return refuse(declared.id, parameter_errc::bad_format,
                      "must be plain text");
    out.value = *text;
    return std::nullopt;
}

std::optional<parameter_error> check_path_value(const parameter& declared,
                                                const raw_value& given,
                                                checked_parameter& out)
{
    const auto* const text = std::get_if<std::string>(&given);
    if (text == nullptr)
        return refuse(declared.id, parameter_errc::wrong_type,
                      "must be a path");
    path_result checked = check_path(*text, declared.kind);
    if (!checked.why.empty())
    {
        std::string rest = "is not a path that can be used: it ";
        rest.append(checked.why);
        return refuse(declared.id, parameter_errc::bad_path, rest);
    }
    path_scope scope;
    scope.path = checked.path;
    scope.kind = declared.kind;
    scope.access = declared.access;
    scope.must_exist = declared.must_exist;
    out.value = std::move(checked.path);
    out.scope = std::move(scope);
    return std::nullopt;
}

std::optional<parameter_error> check_one(const parameter& declared,
                                         const raw_value& given,
                                         value_source source,
                                         checked_parameter& out)
{
    switch (declared.type)
    {
    case parameter_type::boolean:
    {
        const auto* const value = std::get_if<bool>(&given);
        if (value == nullptr)
            return refuse(declared.id, parameter_errc::wrong_type,
                          "must be true or false");
        out.value = *value;
        return std::nullopt;
    }
    case parameter_type::integer:
        return check_integer(declared, given, out);
    case parameter_type::number:
        return check_number(declared, given, out);
    case parameter_type::duration:
        return check_duration(declared, given, out);
    case parameter_type::choice:
        return check_choice(declared, given, out);
    case parameter_type::string:
        return check_text(declared, given, out);
    case parameter_type::secret:
        if (source == value_source::setup)
            return refuse(declared.id, parameter_errc::stored_secret,
                          "is a secret, which a setup does not keep");
        return check_text(declared, given, out);
    case parameter_type::bytes:
        return check_bytes(declared, given, out);
    case parameter_type::port:
    case parameter_type::link:
        return check_name(declared, given, out);
    case parameter_type::path:
        return check_path_value(declared, given, out);
    }
    return refuse(declared.id, parameter_errc::wrong_type,
                  "has a type this version does not know");
}

// -- text -----------------------------------------------------------------------

json::value json_of(const checked_parameter& item)
{
    switch (item.type)
    {
    case parameter_type::duration:
        if (const auto* const value = std::get_if<std::int64_t>(&item.value))
            return json::value(json::string(duration_text(*value)));
        break;
    case parameter_type::bytes:
        if (const auto* const value = std::get_if<std::string>(&item.value))
            return json::value(json::string(hex_text(*value)));
        break;
    default:
        break;
    }
    if (const auto* const flag = std::get_if<bool>(&item.value))
        return json::value(*flag);
    if (const auto* const whole = std::get_if<std::int64_t>(&item.value))
        return json::value(*whole);
    if (const auto* const real = std::get_if<double>(&item.value))
        return json::value(*real);
    if (const auto* const text = std::get_if<std::string>(&item.value))
        return json::value(json::string(*text));
    return json::value(nullptr);
}

} // namespace

std::string_view to_string(parameter_errc value) noexcept
{
    switch (value)
    {
    case parameter_errc::missing_required:
        return "missing_required";
    case parameter_errc::unknown_name:
        return "unknown_name";
    case parameter_errc::repeated_name:
        return "repeated_name";
    case parameter_errc::wrong_type:
        return "wrong_type";
    case parameter_errc::below_min:
        return "below_min";
    case parameter_errc::above_max:
        return "above_max";
    case parameter_errc::too_long:
        return "too_long";
    case parameter_errc::bad_format:
        return "bad_format";
    case parameter_errc::not_an_option:
        return "not_an_option";
    case parameter_errc::bad_path:
        return "bad_path";
    case parameter_errc::stored_secret:
        return "stored_secret";
    }
    return "unknown";
}

validation validate(std::span<const parameter> declared,
                    std::span<const raw_parameter> given, value_source source)
{
    validation out;

    // Names that were given: the first of each is used, a second is refused,
    // and one nobody declared is refused.
    std::vector<const raw_parameter*> taken(declared.size(), nullptr);
    std::set<std::string_view> seen;
    for (const raw_parameter& item : given)
    {
        if (!seen.insert(item.id).second)
        {
            out.errors.push_back(refuse(item.id, parameter_errc::repeated_name,
                                        "is given more than once"));
            continue;
        }
        const auto at = std::ranges::find_if(
            declared, [&](const parameter& p) { return p.id == item.id; });
        if (at == declared.end())
        {
            out.errors.push_back(
                refuse(item.id, parameter_errc::unknown_name,
                       "is not a setting of this plugin"));
            continue;
        }
        taken[static_cast<std::size_t>(at - declared.begin())] = &item;
    }

    for (std::size_t i = 0; i < declared.size(); ++i)
    {
        const parameter& wanted = declared[i];
        const bool is_secret = wanted.type == parameter_type::secret;
        checked_parameter item;
        item.id = wanted.id;
        item.type = wanted.type;
        if (taken[i] != nullptr)
        {
            if (auto error = check_one(wanted, taken[i]->value, source, item))
            {
                out.errors.push_back(std::move(*error));
                continue;
            }
        }
        else if (is_secret && source == value_source::setup)
        {
            // A setup holds no secret, so there is nothing to ask for.
            continue;
        }
        else if (!std::holds_alternative<std::monostate>(wanted.default_value))
        {
            item.value = wanted.default_value;
            item.defaulted = true;
        }
        else
        {
            out.errors.push_back(refuse(wanted.id,
                                        parameter_errc::missing_required,
                                        "needs a value"));
            continue;
        }
        out.values.push_back(std::move(item));
    }
    if (!out.errors.empty())
        out.values.clear();
    return out;
}

std::string to_json(std::span<const checked_parameter> values,
                    value_source source)
{
    std::vector<const checked_parameter*> order;
    order.reserve(values.size());
    for (const checked_parameter& item : values)
    {
        if (item.type == parameter_type::secret &&
            source == value_source::setup)
            continue;
        order.push_back(&item);
    }
    std::ranges::sort(order,
                      [](const checked_parameter* a,
                         const checked_parameter* b) { return a->id < b->id; });
    json::object object;
    for (const checked_parameter* item : order)
        object.insert_or_assign(item->id, json_of(*item));
    return json::serialize(object);
}

std::optional<raw_parameters> from_json(std::string_view text,
                                        std::string& problem)
{
    if (text.size() > max_parameters_json_size)
    {
        problem = "is larger than 64 KiB";
        return std::nullopt;
    }
    const std::optional<json::value> root =
        detail::parse_strict(text, max_parameters_json_depth, problem);
    if (!root)
        return std::nullopt;
    if (!root->is_object())
    {
        problem = "is not one JSON object";
        return std::nullopt;
    }
    const json::object& object = root->as_object();
    if (object.size() > max_parameters)
    {
        problem = "has more settings than a plugin may declare";
        return std::nullopt;
    }
    raw_parameters out;
    out.reserve(object.size());
    for (const auto& member : object)
    {
        const std::string_view key = member.key();
        if (key.empty() || key.size() > max_id_length ||
            !plain_text(key))
        {
            problem = "has a setting name that is empty, too long or not "
                      "plain text";
            return std::nullopt;
        }
        raw_parameter item;
        item.id = std::string(key);
        const json::value& value = member.value();
        if (value.is_bool())
        {
            item.value = value.as_bool();
        }
        else if (value.is_int64())
        {
            item.value = value.as_int64();
        }
        else if (value.is_uint64())
        {
            const std::uint64_t big = value.as_uint64();
            if (big >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
            {
                problem = "has a whole number that is too large";
                return std::nullopt;
            }
            item.value = static_cast<std::int64_t>(big);
        }
        else if (value.is_double())
        {
            const double real = value.as_double();
            if (!(real >= std::numeric_limits<double>::lowest() &&
                  real <= std::numeric_limits<double>::max()))
            {
                problem = "has a number that is too large";
                return std::nullopt;
            }
            item.value = real;
        }
        else if (value.is_string())
        {
            if (value.as_string().size() > max_parameter_text_size)
            {
                problem = "has a text that is too long";
                return std::nullopt;
            }
            item.value = std::string(value.as_string());
        }
        else
        {
            problem = "has a setting that is not true or false, a number or "
                      "text";
            return std::nullopt;
        }
        out.push_back(std::move(item));
    }
    return out;
}

std::optional<raw_parameters> from_binding_params(std::string_view params,
                                                  std::string& problem)
{
    if (params.find_first_not_of(" \t\r\n") == std::string_view::npos)
        return raw_parameters{};
    return from_json(params, problem);
}

std::string to_binding_params(std::span<const checked_parameter> values)
{
    const bool any = std::ranges::any_of(
        values, [](const checked_parameter& item) {
            return item.type != parameter_type::secret;
        });
    if (!any)
        return {};
    return to_json(values, value_source::setup);
}

std::string stable_text(std::span<const checked_parameter> values)
{
    return to_json(values, value_source::setup);
}

} // namespace vss::plugin
