//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/manifest.hpp>

#include "reader.hpp"

#include <boost/json.hpp>
#include <boost/json/basic_parser_impl.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace vss::plugin
{
namespace
{

namespace json = boost::json;

// -- characters -------------------------------------------------------------

constexpr bool is_lower(char c) noexcept
{
    return c >= 'a' && c <= 'z';
}

constexpr bool is_upper(char c) noexcept
{
    return c >= 'A' && c <= 'Z';
}

constexpr bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

constexpr bool is_alpha(char c) noexcept
{
    return is_lower(c) || is_upper(c);
}

constexpr char to_lower(char c) noexcept
{
    return is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr char to_upper(char c) noexcept
{
    return is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c;
}

constexpr bool is_printable_ascii(char c) noexcept
{
    return c >= 0x20 && c <= 0x7e;
}

constexpr std::uint8_t byte_at(std::string_view text,
                               std::size_t index) noexcept
{
    return static_cast<std::uint8_t>(text[index]);
}

/// Whether a text may be shown to a person as it is.  The JSON reader has
/// already refused bytes that are not UTF-8.  Refused here: control
/// characters, line and paragraph separators, a byte order mark, and the
/// marks that change the direction of the text around them, which can make
/// one name read as another.
bool is_clean_text(std::string_view text) noexcept
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const std::uint8_t first = byte_at(text, i);
        if (first < 0x20 || first == 0x7f)
            return false;
        const std::uint8_t second =
            i + 1 < text.size() ? byte_at(text, i + 1) : std::uint8_t{0};
        const std::uint8_t third =
            i + 2 < text.size() ? byte_at(text, i + 2) : std::uint8_t{0};
        // U+0080 to U+009F: the second set of control characters.
        if (first == 0xc2 && second >= 0x80 && second <= 0x9f)
            return false;
        // U+061C: Arabic letter mark.
        if (first == 0xd8 && second == 0x9c)
            return false;
        if (first == 0xe2 && second == 0x80)
        {
            // U+200E, U+200F: direction marks.
            if (third == 0x8e || third == 0x8f)
                return false;
            // U+2028, U+2029: line and paragraph separators.
            if (third == 0xa8 || third == 0xa9)
                return false;
            // U+202A to U+202E: embeddings and overrides.
            if (third >= 0xaa && third <= 0xae)
                return false;
        }
        // U+2066 to U+2069: isolates.
        if (first == 0xe2 && second == 0x81 && third >= 0xa6 && third <= 0xa9)
            return false;
        // U+FEFF: byte order mark.
        if (first == 0xef && second == 0xbb && third == 0xbf)
            return false;
    }
    return true;
}

/// `[a-z][a-z0-9_-]*`, at most 64 characters: the ids inside a manifest.
bool is_local_id(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 64 || !is_lower(text.front()))
        return false;
    return std::ranges::all_of(text, [](char c) {
        return is_lower(c) || is_digit(c) || c == '_' || c == '-';
    });
}

/// A plugin id: lower-case names of letters and digits joined by `.` or `-`,
/// with at least one dot, such as `com.example.modbus-fixture`.
bool is_plugin_id(std::string_view text) noexcept
{
    if (text.size() < 3 || text.size() > max_id_length)
        return false;
    bool dot = false;
    bool after_separator = true;
    for (const char c : text)
    {
        if (is_lower(c) || is_digit(c))
        {
            after_separator = false;
            continue;
        }
        if ((c != '.' && c != '-') || after_separator)
            return false;
        dot = dot || c == '.';
        after_separator = true;
    }
    return dot && !after_separator;
}

/// `[a-z][a-z0-9_]*`, at most 64 characters: the name of a capability flag.
bool is_capability_name(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 64 || !is_lower(text.front()))
        return false;
    return std::ranges::all_of(
        text, [](char c) { return is_lower(c) || is_digit(c) || c == '_'; });
}

/// The name of a program of the computer, such as `python3` or `dotnet-8.0`.
bool is_runtime_name(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 32)
        return false;
    if (!is_lower(text.front()) && !is_digit(text.front()))
        return false;
    return std::ranges::all_of(text, [](char c) {
        return is_lower(c) || is_digit(c) || c == '.' || c == '_' ||
               c == '+' || c == '-';
    });
}

/// A language tag such as `en`, `de` or `en-AU`.
bool is_language_tag(std::string_view text) noexcept
{
    if (text.size() < 2 || text.size() > 16)
        return false;
    std::size_t part = 0;
    bool first = true;
    for (const char c : text)
    {
        if (c == '-')
        {
            if (part < 2 || part > 8 || (first && part > 3))
                return false;
            part = 0;
            first = false;
            continue;
        }
        if (!is_alpha(c) && !(is_digit(c) && !first))
            return false;
        ++part;
    }
    return part >= 2 && part <= 8 && !(first && part > 3);
}

/// Printable ASCII with no space: an address, a host or a URL.
bool is_ascii_token(std::string_view text) noexcept
{
    return !text.empty() && std::ranges::all_of(text, [](char c) {
        return is_printable_ascii(c) && c != ' ';
    });
}

// -- numbers and dates ------------------------------------------------------

/// The value of a run of digits with no sign and no needless leading zero,
/// when it is at most `limit`.
std::optional<std::uint64_t> parse_digits(std::string_view text,
                                          std::uint64_t limit) noexcept
{
    if (text.empty() || text.size() > 19)
        return std::nullopt;
    if (text.size() > 1 && text.front() == '0')
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : text)
    {
        if (!is_digit(c))
            return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > limit)
        return std::nullopt;
    return value;
}

std::optional<plugin_version> parse_version(std::string_view text) noexcept
{
    plugin_version out;
    std::string_view numbers = text;
    if (const std::size_t hyphen = text.find('-');
        hyphen != std::string_view::npos)
    {
        numbers = text.substr(0, hyphen);
        const std::string_view pre = text.substr(hyphen + 1);
        if (pre.empty() || pre.size() > 32 || pre.front() == '.' ||
            pre.back() == '.' || pre.find("..") != std::string_view::npos)
            return std::nullopt;
        if (!std::ranges::all_of(pre, [](char c) {
                return is_alpha(c) || is_digit(c) || c == '.' || c == '-';
            }))
            return std::nullopt;
        out.pre_release = std::string(pre);
    }
    std::uint32_t* const parts[] = {&out.major, &out.minor, &out.patch};
    for (std::size_t i = 0; i < std::size(parts); ++i)
    {
        const std::size_t dot = numbers.find('.');
        const bool last = i + 1 == std::size(parts);
        if (last != (dot == std::string_view::npos))
            return std::nullopt;
        const auto value = parse_digits(numbers.substr(0, dot), 999'999);
        if (!value)
            return std::nullopt;
        *parts[i] = static_cast<std::uint32_t>(*value);
        if (!last)
            numbers.remove_prefix(dot + 1);
    }
    return out;
}

std::optional<api_version> parse_api_version(std::string_view text) noexcept
{
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos)
        return std::nullopt;
    const auto major = parse_digits(text.substr(0, dot), 65'535);
    const auto minor = parse_digits(text.substr(dot + 1), 65'535);
    if (!major || !minor)
        return std::nullopt;
    return api_version{static_cast<std::uint16_t>(*major),
                       static_cast<std::uint16_t>(*minor)};
}

/// `YYYY-MM-DD`, a day that exists.
std::optional<std::chrono::year_month_day>
parse_date(std::string_view text) noexcept
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-')
        return std::nullopt;
    const auto number = [](std::string_view digits) -> std::optional<unsigned> {
        unsigned value = 0;
        for (const char c : digits)
        {
            if (!is_digit(c))
                return std::nullopt;
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        return value;
    };
    const auto year = number(text.substr(0, 4));
    const auto month = number(text.substr(5, 2));
    const auto day = number(text.substr(8, 2));
    if (!year || !month || !day)
        return std::nullopt;
    const std::chrono::year_month_day date{
        std::chrono::year{static_cast<int>(*year)},
        std::chrono::month{*month}, std::chrono::day{*day}};
    if (!date.ok() || *year < 2000)
        return std::nullopt;
    return date;
}

/// The longest duration a parameter may hold: one day.
constexpr std::int64_t max_duration_ns = 86'400'000'000'000;

/// A whole number and a unit, such as `250us`, as nanoseconds.
std::optional<std::int64_t> parse_duration(std::string_view text) noexcept
{
    std::size_t digits = 0;
    while (digits < text.size() && is_digit(text[digits]))
        ++digits;
    const auto count = parse_digits(text.substr(0, digits), 86'400'000'000'000);
    if (!count)
        return std::nullopt;
    const std::string_view unit = text.substr(digits);
    std::uint64_t scale = 0;
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
    if (*count > static_cast<std::uint64_t>(max_duration_ns) / scale)
        return std::nullopt;
    return static_cast<std::int64_t>(*count * scale);
}

std::optional<std::uint8_t> hex_digit(char c) noexcept
{
    if (is_digit(c))
        return static_cast<std::uint8_t>(c - '0');
    const char lower = to_lower(c);
    if (lower >= 'a' && lower <= 'f')
        return static_cast<std::uint8_t>(lower - 'a' + 10);
    return std::nullopt;
}

/// Pairs of hexadecimal digits, with single spaces allowed between pairs:
/// `02 31 03` or `023103`.
std::optional<std::string> parse_hex_bytes(std::string_view text)
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

// -- names ------------------------------------------------------------------

template <class Enum, std::size_t N>
std::optional<Enum>
find_named(const std::array<std::pair<std::string_view, Enum>, N>& names,
           std::string_view text) noexcept
{
    for (const auto& [name, value] : names)
    {
        if (name == text)
            return value;
    }
    return std::nullopt;
}

template <class Enum, std::size_t N>
std::string_view
name_of(const std::array<std::pair<std::string_view, Enum>, N>& names,
        Enum value) noexcept
{
    for (const auto& [name, candidate] : names)
    {
        if (candidate == value)
            return name;
    }
    return "unknown";
}

using std::string_view_literals::operator""sv;

constexpr std::array permission_names{
    std::pair{"ports.read"sv, permission::ports_read},
    std::pair{"traffic.tap"sv, permission::traffic_tap},
    std::pair{"ports.write"sv, permission::ports_write},
    std::pair{"topology.modify"sv, permission::topology_modify},
    std::pair{"modifier.inline"sv, permission::modifier_inline},
    std::pair{"faults"sv, permission::faults},
    std::pair{"serial.devices"sv, permission::serial_devices},
    std::pair{"fs.read"sv, permission::fs_read},
    std::pair{"fs.write"sv, permission::fs_write},
    std::pair{"data.private"sv, permission::data_private},
    std::pair{"net.listen"sv, permission::net_listen},
    std::pair{"net.outbound"sv, permission::net_outbound},
    std::pair{"process.spawn"sv, permission::process_spawn},
    std::pair{"bundle.section"sv, permission::bundle_section}};

constexpr std::array parameter_type_names{
    std::pair{"bool"sv, parameter_type::boolean},
    std::pair{"int"sv, parameter_type::integer},
    std::pair{"number"sv, parameter_type::number},
    std::pair{"enum"sv, parameter_type::choice},
    std::pair{"string"sv, parameter_type::string},
    std::pair{"bytes"sv, parameter_type::bytes},
    std::pair{"duration"sv, parameter_type::duration},
    std::pair{"port"sv, parameter_type::port},
    std::pair{"link"sv, parameter_type::link},
    std::pair{"path"sv, parameter_type::path},
    std::pair{"secret"sv, parameter_type::secret}};

constexpr std::array extension_kind_names{
    std::pair{"generator"sv, extension_kind::generator},
    std::pair{"fixture"sv, extension_kind::fixture},
    std::pair{"modifier"sv, extension_kind::modifier},
    std::pair{"adapter"sv, extension_kind::adapter},
    std::pair{"decoder"sv, extension_kind::decoder}};

constexpr std::array timing_class_names{
    std::pair{"rules"sv, timing_class::rules},
    std::pair{"inline"sv, timing_class::in_line},
    std::pair{"async"sv, timing_class::async}};

constexpr std::array timeout_action_names{
    std::pair{"fail_open"sv, timeout_action::fail_open},
    std::pair{"fail_closed"sv, timeout_action::fail_closed}};

constexpr std::array path_kind_names{
    std::pair{"file"sv, path_kind::file},
    std::pair{"directory"sv, path_kind::directory}};

constexpr std::array path_access_names{
    std::pair{"read"sv, path_access::read},
    std::pair{"write"sv, path_access::write}};

constexpr std::array platform_names{
    "windows-x64"sv, "windows-arm64"sv, "linux-x64"sv,
    "linux-arm64"sv, "macos-x64"sv,     "macos-arm64"sv};

constexpr std::array edition_names{"pro"sv, "enterprise"sv};

/// The names Windows keeps for devices, whatever follows a dot.
constexpr std::array reserved_device_names{
    "CON"sv,  "PRN"sv,  "AUX"sv,  "NUL"sv,    "CONIN$"sv, "CONOUT$"sv,
    "COM0"sv, "COM1"sv, "COM2"sv, "COM3"sv,   "COM4"sv,   "COM5"sv,
    "COM6"sv, "COM7"sv, "COM8"sv, "COM9"sv,   "LPT0"sv,   "LPT1"sv,
    "LPT2"sv, "LPT3"sv, "LPT4"sv, "LPT5"sv,   "LPT6"sv,   "LPT7"sv,
    "LPT8"sv, "LPT9"sv};

// -- a key given twice ------------------------------------------------------

/// Boost.JSON keeps the last of two members with the same key and says
/// nothing.  Two readers that disagree on which one counts would read one
/// signed manifest as two different plugins, so a repeated key is an error.
/// This handler follows the document once and only watches the keys.
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

    /// The first key that was given twice in one object; empty when none.
    const std::string& repeated() const noexcept { return repeated_; }
    bool found() const noexcept { return found_; }

    bool on_document_begin(error_code&) { return true; }
    bool on_document_end(error_code&) { return true; }
    bool on_array_begin(error_code&) { return true; }
    bool on_array_end(std::size_t, error_code&) { return true; }

    bool on_object_begin(error_code&)
    {
        objects_.emplace_back();
        return true;
    }

    bool on_object_end(std::size_t, error_code&)
    {
        objects_.pop_back();
        return true;
    }

    bool on_key_part(json::string_view part, std::size_t, error_code&)
    {
        key_.append(part.data(), part.size());
        return true;
    }

    bool on_key(json::string_view part, std::size_t, error_code& error)
    {
        key_.append(part.data(), part.size());
        std::vector<std::string>& keys = objects_.back();
        if (std::ranges::find(keys, key_) != keys.end())
        {
            found_ = true;
            repeated_ = std::move(key_);
            error = make_error_code(plugin_errc::manifest_invalid);
            return false;
        }
        keys.push_back(std::move(key_));
        key_.clear();
        return true;
    }

    bool on_string_part(json::string_view, std::size_t, error_code&)
    {
        return true;
    }
    bool on_string(json::string_view, std::size_t, error_code&) { return true; }
    bool on_number_part(json::string_view, error_code&) { return true; }
    bool on_int64(std::int64_t, json::string_view, error_code&) { return true; }
    bool on_uint64(std::uint64_t, json::string_view, error_code&)
    {
        return true;
    }
    bool on_double(double, json::string_view, error_code&) { return true; }
    bool on_bool(bool, error_code&) { return true; }
    bool on_null(error_code&) { return true; }
    bool on_comment_part(json::string_view, error_code&) { return true; }
    bool on_comment(json::string_view, error_code&) { return true; }

private:
    std::vector<std::vector<std::string>> objects_;
    std::string key_;
    std::string repeated_;
    bool found_ = false;
};

// -- the reader -------------------------------------------------------------

/// What went wrong first.  Every reading function goes on after an error and
/// returns something harmless, and the caller looks here before it uses the
/// result, so no function has to pass a failure up by hand.
struct context
{
    error_code error;
    std::string detail;

    bool failed() const noexcept { return error.failed(); }

    void fail(plugin_errc code, std::string_view where, std::string_view what)
    {
        if (failed())
            return;
        error = make_error_code(code);
        detail.assign(where.empty() ? "manifest" : where);
        detail += ": ";
        detail += what;
    }

    void invalid(std::string_view where, std::string_view what)
    {
        fail(plugin_errc::manifest_invalid, where, what);
    }
};

std::string member(std::string_view where, std::string_view key)
{
    std::string out(where);
    if (!out.empty())
        out += '.';
    out += key;
    return out;
}

std::string element(std::string_view where, std::size_t index)
{
    std::string out(where);
    out += '[';
    out += std::to_string(index);
    out += ']';
    return out;
}

std::string in_quotes(std::string_view text)
{
    // What is quoted came from the manifest and goes to a person, so it is
    // cut short and anything that is not plain ASCII is replaced.
    std::string out = "\"";
    for (const char c : text.substr(0, 48))
        out += is_printable_ascii(c) && c != '"' ? c : '?';
    if (text.size() > 48)
        out += "...";
    out += '"';
    return out;
}

std::string_view view_of(const json::string& text) noexcept
{
    return {text.data(), text.size()};
}

/// The members of one object.  Each member the format knows is taken by
/// name; `finish` then refuses whatever was not taken, so a misspelt key is
/// found and not ignored.
class object_reader
{
public:
    object_reader(context& ctx, const json::value& value, std::string where)
        : ctx_(ctx), where_(std::move(where))
    {
        if (value.is_object())
            object_ = &value.get_object();
        else
            ctx_.invalid(where_, "must be an object");
    }

    const std::string& where() const noexcept { return where_; }
    std::string at(std::string_view key) const { return member(where_, key); }

    /// The member, or null when the object does not have it.
    const json::value* take(std::string_view key)
    {
        if (object_ == nullptr)
            return nullptr;
        taken_.push_back(key);
        return object_->if_contains(key);
    }

    /// The member; an error when the object does not have it.
    const json::value* need(std::string_view key)
    {
        const json::value* const value = take(key);
        if (value == nullptr && object_ != nullptr)
            ctx_.invalid(at(key), "is required");
        return value;
    }

    void finish()
    {
        if (object_ == nullptr || ctx_.failed())
            return;
        for (const auto& entry : *object_)
        {
            const std::string_view key{entry.key().data(), entry.key().size()};
            if (std::ranges::find(taken_, key) == taken_.end())
            {
                ctx_.invalid(where_, "unknown key " + in_quotes(key));
                return;
            }
        }
    }

private:
    context& ctx_;
    std::string where_;
    const json::object* object_ = nullptr;
    std::vector<std::string_view> taken_;
};

/// A list of at most `limit` items; null after an error.
const json::array* read_array(context& ctx, const json::value& value,
                              std::string_view where, std::size_t least,
                              std::size_t limit)
{
    if (!value.is_array())
    {
        ctx.invalid(where, "must be a list");
        return nullptr;
    }
    const json::array& items = value.get_array();
    if (items.size() < least)
    {
        ctx.invalid(where, "must not be empty");
        return nullptr;
    }
    if (items.size() > limit)
    {
        ctx.invalid(where, "has more than " + std::to_string(limit) + " items");
        return nullptr;
    }
    return &items;
}

/// A text a person will read: clean, not empty, with no space at either end
/// and at most `limit` bytes.
std::string read_text(context& ctx, const json::value& value,
                      std::string_view where, std::size_t limit)
{
    if (!value.is_string())
    {
        ctx.invalid(where, "must be a string");
        return {};
    }
    const std::string_view text = view_of(value.get_string());
    if (text.empty())
        ctx.invalid(where, "must not be empty");
    else if (text.size() > limit)
        ctx.invalid(where,
                    "is longer than " + std::to_string(limit) + " bytes");
    else if (!is_clean_text(text))
        ctx.invalid(where, "has a control character or a direction mark");
    else if (text.front() == ' ' || text.back() == ' ')
        ctx.invalid(where, "must not begin or end with a space");
    else
        return std::string(text);
    return {};
}

/// A string that is a name of the format itself.  Nothing is checked here
/// but that it is a string; the caller decides whether it knows the name.
std::string_view read_word(context& ctx, const json::value& value,
                           std::string_view where)
{
    if (!value.is_string())
    {
        ctx.invalid(where, "must be a string");
        return {};
    }
    return view_of(value.get_string());
}

bool read_bool(context& ctx, const json::value& value, std::string_view where)
{
    if (!value.is_bool())
    {
        ctx.invalid(where, "must be true or false");
        return false;
    }
    return value.get_bool();
}

/// A whole number from `least` to `most`.
std::uint64_t read_unsigned(context& ctx, const json::value& value,
                            std::string_view where, std::uint64_t least,
                            std::uint64_t most)
{
    std::uint64_t number = 0;
    if (value.is_uint64())
        number = value.get_uint64();
    else if (value.is_int64() && value.get_int64() >= 0)
        number = static_cast<std::uint64_t>(value.get_int64());
    else
    {
        ctx.invalid(where, "must be a whole number that is not negative");
        return least;
    }
    if (number < least || number > most)
    {
        ctx.invalid(where, "must be from " + std::to_string(least) + " to " +
                               std::to_string(most));
        return least;
    }
    return number;
}

/// The largest whole number a double holds exactly, which is also the
/// largest every JSON reader agrees on.
constexpr std::int64_t max_exact_integer = 9'007'199'254'740'992;

std::optional<std::int64_t> read_integer(context& ctx,
                                         const json::value& value,
                                         std::string_view where)
{
    std::int64_t number = 0;
    if (value.is_int64())
        number = value.get_int64();
    else if (value.is_uint64() && value.get_uint64() <=
                                      static_cast<std::uint64_t>(
                                          max_exact_integer))
        number = static_cast<std::int64_t>(value.get_uint64());
    else
    {
        ctx.invalid(where, "must be a whole number");
        return std::nullopt;
    }
    if (number < -max_exact_integer || number > max_exact_integer)
    {
        ctx.invalid(where, "is too large");
        return std::nullopt;
    }
    return number;
}

std::optional<double> read_number(context& ctx, const json::value& value,
                                  std::string_view where)
{
    double number = 0.0;
    if (value.is_double())
        number = value.get_double();
    else if (value.is_int64())
        number = static_cast<double>(value.get_int64());
    else if (value.is_uint64())
        number = static_cast<double>(value.get_uint64());
    else
    {
        ctx.invalid(where, "must be a number");
        return std::nullopt;
    }
    if (!std::isfinite(number))
    {
        ctx.invalid(where, "is too large");
        return std::nullopt;
    }
    return number;
}

std::optional<std::int64_t> read_duration(context& ctx,
                                          const json::value& value,
                                          std::string_view where)
{
    const std::string_view text = read_word(ctx, value, where);
    if (ctx.failed())
        return std::nullopt;
    const auto duration = parse_duration(text);
    if (!duration)
        ctx.invalid(where, "must be a whole number and a unit (ns, us, ms, "
                           "s), one day at most, such as \"250us\"");
    return duration;
}

localized_text read_localized(context& ctx, const json::value& value,
                              std::string_view where, std::size_t limit)
{
    localized_text out;
    if (value.is_string())
    {
        out.entries.emplace_back(std::string{},
                                 read_text(ctx, value, where, limit));
        return out;
    }
    if (!value.is_object())
    {
        ctx.invalid(where, "must be a string, or an object of language to "
                           "string");
        return out;
    }
    const json::object& object = value.get_object();
    if (object.empty())
        ctx.invalid(where, "must not be empty");
    if (object.size() > max_languages)
        ctx.invalid(where, "has more than " + std::to_string(max_languages) +
                               " languages");
    for (const auto& entry : object)
    {
        if (ctx.failed())
            break;
        const std::string_view language{entry.key().data(),
                                        entry.key().size()};
        if (!is_language_tag(language))
        {
            ctx.invalid(where, in_quotes(language) + " is not a language tag");
            break;
        }
        out.entries.emplace_back(
            std::string(language),
            read_text(ctx, entry.value(), member(where, language), limit));
    }
    return out;
}

/// A list of strings that each pass `accept`, with none given twice.
template <class Accept>
std::vector<std::string> read_names(context& ctx, const json::value& value,
                                    std::string_view where, std::size_t least,
                                    std::size_t limit, std::string_view kind,
                                    Accept accept)
{
    std::vector<std::string> out;
    const json::array* const items =
        read_array(ctx, value, where, least, limit);
    if (items == nullptr)
        return out;
    for (std::size_t i = 0; i < items->size() && !ctx.failed(); ++i)
    {
        const std::string at = element(where, i);
        const std::string_view name = read_word(ctx, (*items)[i], at);
        if (ctx.failed())
            break;
        if (!accept(name))
            ctx.invalid(at, in_quotes(name) + " is not " + std::string(kind));
        else if (std::ranges::any_of(out, [&](const std::string& other) {
                     return other == name;
                 }))
            ctx.invalid(at, in_quotes(name) + " is given twice");
        else
            out.emplace_back(name);
    }
    return out;
}

/// A path inside the plugin.
std::string read_path(context& ctx, const json::value& value,
                      std::string_view where)
{
    const std::string_view path = read_word(ctx, value, where);
    if (ctx.failed())
        return {};
    if (const auto problem = check_relative_path(path))
    {
        ctx.fail(plugin_errc::layout_invalid, where, *problem);
        return {};
    }
    return std::string(path);
}

// -- parameters -------------------------------------------------------------

/// Checks `min <= default <= max` and `min <= max` for one ordered type.
template <class T>
void check_range(context& ctx, const parameter& out, std::string_view where)
{
    const T* const least = std::get_if<T>(&out.min);
    const T* const most = std::get_if<T>(&out.max);
    const T* const chosen = std::get_if<T>(&out.default_value);
    if (least != nullptr && most != nullptr && *least > *most)
        ctx.invalid(member(where, "min"), "is above max");
    if (chosen != nullptr && least != nullptr && *chosen < *least)
        ctx.invalid(member(where, "default"), "is below min");
    if (chosen != nullptr && most != nullptr && *chosen > *most)
        ctx.invalid(member(where, "default"), "is above max");
}

void read_integer_parameter(context& ctx, object_reader& reader,
                            parameter& out)
{
    const auto read = [&](std::string_view key, parameter_value& into) {
        if (const json::value* const value = reader.take(key))
        {
            if (const auto number = read_integer(ctx, *value, reader.at(key)))
                into = *number;
        }
    };
    read("default", out.default_value);
    read("min", out.min);
    read("max", out.max);
    read("step", out.step);
    if (const auto* const step = std::get_if<std::int64_t>(&out.step);
        step != nullptr && *step <= 0)
        ctx.invalid(reader.at("step"), "must be above zero");
    check_range<std::int64_t>(ctx, out, reader.where());
}

void read_number_parameter(context& ctx, object_reader& reader, parameter& out)
{
    const auto read = [&](std::string_view key, parameter_value& into) {
        if (const json::value* const value = reader.take(key))
        {
            if (const auto number = read_number(ctx, *value, reader.at(key)))
                into = *number;
        }
    };
    read("default", out.default_value);
    read("min", out.min);
    read("max", out.max);
    read("step", out.step);
    if (const auto* const step = std::get_if<double>(&out.step);
        step != nullptr && !(*step > 0.0))
        ctx.invalid(reader.at("step"), "must be above zero");
    check_range<double>(ctx, out, reader.where());
}

void read_duration_parameter(context& ctx, object_reader& reader,
                             parameter& out)
{
    const auto read = [&](std::string_view key, parameter_value& into) {
        if (const json::value* const value = reader.take(key))
        {
            if (const auto duration =
                    read_duration(ctx, *value, reader.at(key)))
                into = *duration;
        }
    };
    read("default", out.default_value);
    read("min", out.min);
    read("max", out.max);
    check_range<std::int64_t>(ctx, out, reader.where());
}

void read_choice_parameter(context& ctx, object_reader& reader, parameter& out)
{
    const json::value* const options = reader.need("options");
    if (options == nullptr)
        return;
    const std::string where = reader.at("options");
    const json::array* const items =
        read_array(ctx, *options, where, 1, max_choices);
    if (items == nullptr)
        return;
    for (std::size_t i = 0; i < items->size() && !ctx.failed(); ++i)
    {
        object_reader option(ctx, (*items)[i], element(where, i));
        parameter_choice choice;
        if (const json::value* const value = option.need("value"))
            choice.value =
                read_text(ctx, *value, option.at("value"), max_name_length);
        if (const json::value* const label = option.need("label"))
            choice.label = read_localized(ctx, *label, option.at("label"),
                                          max_name_length);
        option.finish();
        if (ctx.failed())
            return;
        const bool twice =
            std::ranges::any_of(out.choices, [&](const parameter_choice& c) {
                return c.value == choice.value;
            });
        if (twice)
            ctx.invalid(option.at("value"),
                        in_quotes(choice.value) + " is given twice");
        out.choices.push_back(std::move(choice));
    }
    if (const json::value* const chosen = reader.take("default"))
    {
        const std::string_view value =
            read_word(ctx, *chosen, reader.at("default"));
        const bool known =
            std::ranges::any_of(out.choices, [&](const parameter_choice& c) {
                return c.value == value;
            });
        if (!ctx.failed() && !known)
            ctx.invalid(reader.at("default"), "is not one of the options");
        out.default_value = std::string(value);
    }
}

/// `max_length`, from 1 to `max_text_parameter_length`; `fallback` when the
/// parameter does not say.
std::uint32_t read_max_length(context& ctx, object_reader& reader,
                              std::uint32_t fallback)
{
    const json::value* const value = reader.take("max_length");
    if (value == nullptr)
        return fallback;
    return static_cast<std::uint32_t>(
        read_unsigned(ctx, *value, reader.at("max_length"), 1,
                      max_text_parameter_length));
}

void read_string_parameter(context& ctx, object_reader& reader, parameter& out)
{
    out.max_length = read_max_length(ctx, reader, 256);
    const json::value* const chosen = reader.take("default");
    if (chosen == nullptr || ctx.failed())
        return;
    out.default_value =
        read_text(ctx, *chosen, reader.at("default"), out.max_length);
}

void read_bytes_parameter(context& ctx, object_reader& reader, parameter& out)
{
    out.max_length = read_max_length(ctx, reader, 256);
    const json::value* const chosen = reader.take("default");
    if (chosen == nullptr || ctx.failed())
        return;
    const std::string where = reader.at("default");
    const std::string_view text = read_word(ctx, *chosen, where);
    if (ctx.failed())
        return;
    auto bytes = parse_hex_bytes(text);
    if (!bytes)
        ctx.invalid(where, "must be hexadecimal bytes, such as \"02 31 03\"");
    else if (bytes->size() > out.max_length)
        ctx.invalid(where, "is longer than max_length");
    else
        out.default_value = std::move(*bytes);
}

void read_path_parameter(context& ctx, object_reader& reader, parameter& out)
{
    if (const json::value* const kind = reader.take("kind"))
    {
        const auto known = find_named(
            path_kind_names, read_word(ctx, *kind, reader.at("kind")));
        if (known)
            out.kind = *known;
        else
            ctx.invalid(reader.at("kind"),
                        "must be \"file\" or \"directory\"");
    }
    if (const json::value* const access = reader.take("access"))
    {
        const auto known = find_named(
            path_access_names, read_word(ctx, *access, reader.at("access")));
        if (known)
            out.access = *known;
        else
            ctx.invalid(reader.at("access"), "must be \"read\" or \"write\"");
    }
    if (const json::value* const must_exist = reader.take("must_exist"))
        out.must_exist = read_bool(ctx, *must_exist, reader.at("must_exist"));
}

parameter read_parameter(context& ctx, const json::value& value,
                         const std::string& where)
{
    parameter out;
    object_reader reader(ctx, value, where);
    if (const json::value* const id = reader.need("id"))
    {
        const std::string_view text = read_word(ctx, *id, reader.at("id"));
        if (!ctx.failed() && !is_local_id(text))
            ctx.invalid(reader.at("id"), in_quotes(text) + " is not an id");
        out.id = std::string(text);
    }
    if (const json::value* const label = reader.need("label"))
        out.label =
            read_localized(ctx, *label, reader.at("label"), max_name_length);
    if (const json::value* const help = reader.take("help"))
        out.help =
            read_localized(ctx, *help, reader.at("help"), max_help_length);
    const json::value* const type = reader.need("type");
    if (type == nullptr || ctx.failed())
        return out;
    const std::string_view type_name =
        read_word(ctx, *type, reader.at("type"));
    const auto known = find_named(parameter_type_names, type_name);
    if (!known)
    {
        ctx.invalid(reader.at("type"), "unknown type " + in_quotes(type_name));
        return out;
    }
    out.type = *known;

    switch (out.type)
    {
    case parameter_type::boolean:
        if (const json::value* const chosen = reader.take("default"))
            out.default_value =
                read_bool(ctx, *chosen, reader.at("default"));
        break;
    case parameter_type::integer:
        read_integer_parameter(ctx, reader, out);
        break;
    case parameter_type::number:
        read_number_parameter(ctx, reader, out);
        break;
    case parameter_type::choice:
        read_choice_parameter(ctx, reader, out);
        break;
    case parameter_type::string:
        read_string_parameter(ctx, reader, out);
        break;
    case parameter_type::bytes:
        read_bytes_parameter(ctx, reader, out);
        break;
    case parameter_type::duration:
        read_duration_parameter(ctx, reader, out);
        break;
    case parameter_type::path:
        read_path_parameter(ctx, reader, out);
        break;
    case parameter_type::secret:
        // A secret has no default: a manifest is not a place for one.
        out.max_length = read_max_length(ctx, reader, 256);
        break;
    case parameter_type::port:
    case parameter_type::link:
        break;
    }
    if (out.type == parameter_type::integer ||
        out.type == parameter_type::number)
    {
        if (const json::value* const unit = reader.take("unit"))
            out.unit = read_text(ctx, *unit, reader.at("unit"), 16);
    }
    reader.finish();
    return out;
}

std::vector<parameter> read_parameters(context& ctx, const json::value& value,
                                       const std::string& where)
{
    std::vector<parameter> out;
    const json::array* const items =
        read_array(ctx, value, where, 0, max_parameters);
    if (items == nullptr)
        return out;
    for (std::size_t i = 0; i < items->size() && !ctx.failed(); ++i)
    {
        const std::string at = element(where, i);
        parameter item = read_parameter(ctx, (*items)[i], at);
        if (ctx.failed())
            break;
        const bool twice = std::ranges::any_of(
            out, [&](const parameter& p) { return p.id == item.id; });
        if (twice)
            ctx.invalid(member(at, "id"),
                        in_quotes(item.id) + " is given twice");
        out.push_back(std::move(item));
    }
    return out;
}

// -- the parts of a manifest ------------------------------------------------

plugin_author read_author(context& ctx, const json::value& value,
                          const std::string& where)
{
    plugin_author out;
    object_reader reader(ctx, value, where);
    if (const json::value* const name = reader.need("name"))
        out.name = read_text(ctx, *name, reader.at("name"), max_name_length);
    if (const json::value* const url = reader.take("url"))
    {
        const std::string_view text = read_word(ctx, *url, reader.at("url"));
        if (!ctx.failed() &&
            (text.size() > max_url_length || !is_ascii_token(text) ||
             !text.starts_with("https://") || text.size() == 8))
            ctx.invalid(reader.at("url"), "must be an https address");
        out.url = std::string(text);
    }
    reader.finish();
    return out;
}

api_range read_api_range(context& ctx, const json::value& value,
                         const std::string& where)
{
    api_range out;
    object_reader reader(ctx, value, where);
    if (const json::value* const min = reader.need("min"))
    {
        const auto version =
            parse_api_version(read_word(ctx, *min, reader.at("min")));
        if (version)
            out.min = *version;
        else
            ctx.invalid(reader.at("min"),
                        "must be a version such as \"1.0\"");
    }
    if (const json::value* const max = reader.need("max"))
    {
        const std::string_view text = read_word(ctx, *max, reader.at("max"));
        const std::size_t dot = text.find('.');
        const auto major = parse_digits(text.substr(0, dot), 65'535);
        if (dot != std::string_view::npos && major &&
            text.substr(dot + 1) == "x")
        {
            out.max_major = static_cast<std::uint16_t>(*major);
        }
        else if (const auto version = parse_api_version(text))
        {
            out.max_major = version->major;
            out.max_minor = version->minor;
        }
        else
        {
            ctx.invalid(reader.at("max"),
                        "must be a version such as \"1.4\" or \"1.x\"");
        }
    }
    reader.finish();
    if (!ctx.failed())
    {
        const bool below =
            out.max_major < out.min.major ||
            (out.max_major == out.min.major && out.max_minor &&
             *out.max_minor < out.min.minor);
        if (below)
            ctx.invalid(reader.at("max"), "is below min");
    }
    return out;
}

plugin_needs read_needs(context& ctx, const json::value& value,
                        const std::string& where)
{
    plugin_needs out;
    object_reader reader(ctx, value, where);
    if (const json::value* const api = reader.need("api"))
        out.api = read_api_range(ctx, *api, reader.at("api"));
    if (const json::value* const capabilities = reader.take("capabilities"))
        out.capabilities =
            read_names(ctx, *capabilities, reader.at("capabilities"), 0,
                       max_capabilities, "the name of a capability",
                       is_capability_name);
    if (const json::value* const edition = reader.take("edition"))
    {
        const std::string_view text =
            read_word(ctx, *edition, reader.at("edition"));
        if (!ctx.failed() &&
            std::ranges::find(edition_names, text) == edition_names.end())
            ctx.invalid(reader.at("edition"),
                        "must be \"pro\" or \"enterprise\"");
        out.edition = std::string(text);
    }
    if (const json::value* const runtime = reader.take("runtime"))
        out.runtime = read_names(ctx, *runtime, reader.at("runtime"), 0,
                                 max_runtimes, "the name of a program",
                                 is_runtime_name);
    reader.finish();
    return out;
}

run_spec read_run(context& ctx, const json::value& value,
                  const std::string& where)
{
    run_spec out;
    object_reader reader(ctx, value, where);
    if (const json::value* const executable = reader.need("executable"))
        out.executable = read_path(ctx, *executable, reader.at("executable"));
    if (const json::value* const args = reader.take("args"))
    {
        const std::string at = reader.at("args");
        if (const json::array* const items =
                read_array(ctx, *args, at, 0, max_arguments))
        {
            for (std::size_t i = 0; i < items->size() && !ctx.failed(); ++i)
                out.args.push_back(read_text(ctx, (*items)[i], element(at, i),
                                             max_argument_length));
        }
    }
    if (const json::value* const platforms = reader.need("platforms"))
        out.platforms = read_names(
            ctx, *platforms, reader.at("platforms"), 1, platform_names.size(),
            "a platform this version knows", [](std::string_view name) {
                return std::ranges::find(platform_names, name) !=
                       platform_names.end();
            });
    reader.finish();
    return out;
}

bool takes_scope(permission name) noexcept
{
    return name == permission::net_listen || name == permission::net_outbound;
}

permission_request read_permission(context& ctx, const json::value& value,
                                   const std::string& where)
{
    permission_request out;
    object_reader reader(ctx, value, where);
    if (const json::value* const name = reader.need("name"))
    {
        const std::string_view text =
            read_word(ctx, *name, reader.at("name"));
        if (const auto known = parse_permission(text))
            out.name = *known;
        else if (!ctx.failed())
            ctx.invalid(reader.at("name"),
                        "unknown permission " + in_quotes(text));
    }
    if (const json::value* const required = reader.take("required"))
        out.required = read_bool(ctx, *required, reader.at("required"));
    if (const json::value* const reason = reader.take("reason"))
        out.reason =
            read_text(ctx, *reason, reader.at("reason"), max_reason_length);
    const json::value* const scope = reader.take("scope");
    reader.finish();
    if (ctx.failed())
        return out;
    if (!takes_scope(out.name))
    {
        if (scope != nullptr)
            ctx.invalid(reader.at("scope"),
                        "is only for net.listen and net.outbound");
        return out;
    }
    // A plugin that uses the network says where, so the user is told.
    if (scope == nullptr)
    {
        ctx.invalid(reader.at("scope"), "is required");
        return out;
    }
    out.scope = read_names(ctx, *scope, reader.at("scope"), 1, max_scopes,
                           "an address", [](std::string_view text) {
                               return text.size() <= max_scope_length &&
                                      is_ascii_token(text);
                           });
    return out;
}

timing_spec read_timing(context& ctx, const json::value& value,
                        const std::string& where)
{
    timing_spec out;
    object_reader reader(ctx, value, where);
    if (const json::value* const type = reader.need("class"))
    {
        const std::string_view text =
            read_word(ctx, *type, reader.at("class"));
        if (const auto known = find_named(timing_class_names, text))
            out.type = *known;
        else if (!ctx.failed())
            ctx.invalid(reader.at("class"), "unknown value " + in_quotes(text));
    }
    const json::value* const budget = reader.take("budget_us");
    const json::value* const on_timeout = reader.take("on_timeout");
    reader.finish();
    if (ctx.failed())
        return out;
    if (out.type != timing_class::in_line)
    {
        if (budget != nullptr)
            ctx.invalid(reader.at("budget_us"),
                        "is only for the class \"inline\"");
        if (on_timeout != nullptr)
            ctx.invalid(reader.at("on_timeout"),
                        "is only for the class \"inline\"");
        return out;
    }
    if (budget == nullptr)
    {
        ctx.invalid(reader.at("budget_us"), "is required");
        return out;
    }
    out.budget_us = static_cast<std::uint32_t>(read_unsigned(
        ctx, *budget, reader.at("budget_us"), min_budget_us, max_budget_us));
    if (on_timeout != nullptr)
    {
        const auto known = find_named(
            timeout_action_names,
            read_word(ctx, *on_timeout, reader.at("on_timeout")));
        if (known)
            out.on_timeout = *known;
        else
            ctx.invalid(reader.at("on_timeout"),
                        "must be \"fail_open\" or \"fail_closed\"");
    }
    return out;
}

/// Which timing classes a kind of extension point may declare.
bool timing_allowed(extension_kind kind, timing_class type) noexcept
{
    switch (kind)
    {
    case extension_kind::generator:
    case extension_kind::fixture:
        return type != timing_class::in_line;
    case extension_kind::modifier:
        return type != timing_class::rules;
    case extension_kind::adapter:
    case extension_kind::decoder:
        return type == timing_class::async;
    }
    return false;
}

extension_point read_extension_point(context& ctx, const json::value& value,
                                     const std::string& where)
{
    extension_point out;
    object_reader reader(ctx, value, where);
    if (const json::value* const kind = reader.need("kind"))
    {
        const std::string_view text =
            read_word(ctx, *kind, reader.at("kind"));
        if (const auto known = find_named(extension_kind_names, text))
            out.kind = *known;
        else if (!ctx.failed())
            ctx.invalid(reader.at("kind"), "unknown kind " + in_quotes(text));
    }
    if (const json::value* const id = reader.need("id"))
    {
        const std::string_view text = read_word(ctx, *id, reader.at("id"));
        if (!ctx.failed() && !is_local_id(text))
            ctx.invalid(reader.at("id"), in_quotes(text) + " is not an id");
        out.id = std::string(text);
    }
    if (const json::value* const name = reader.need("name"))
        out.name =
            read_localized(ctx, *name, reader.at("name"), max_name_length);
    if (const json::value* const description = reader.take("description"))
        out.description =
            read_localized(ctx, *description, reader.at("description"),
                           max_description_length);
    const json::value* const timing = reader.take("timing");
    if (timing != nullptr)
        out.timing = read_timing(ctx, *timing, reader.at("timing"));
    if (const json::value* const instances = reader.take("max_instances"))
        out.max_instances = static_cast<std::uint32_t>(
            read_unsigned(ctx, *instances, reader.at("max_instances"), 1,
                          max_instances));
    if (const json::value* const parameters = reader.take("parameters"))
        out.parameters =
            read_parameters(ctx, *parameters, reader.at("parameters"));
    reader.finish();
    if (ctx.failed())
        return out;
    // A modifier sits on a link, so it says how: nothing is assumed for it.
    if (out.kind == extension_kind::modifier && timing == nullptr)
        ctx.invalid(reader.at("timing"), "is required for a modifier");
    else if (!timing_allowed(out.kind, out.timing.type))
        ctx.invalid(reader.at("timing.class"),
                    in_quotes(to_string(out.timing.type)) +
                        " is not allowed for the kind " +
                        std::string(to_string(out.kind)));
    return out;
}

bundle_section read_bundle_section(context& ctx, const json::value& value,
                                   const std::string& where)
{
    bundle_section out;
    object_reader reader(ctx, value, where);
    if (const json::value* const id = reader.need("id"))
    {
        const std::string_view text = read_word(ctx, *id, reader.at("id"));
        if (!ctx.failed() && !is_local_id(text))
            ctx.invalid(reader.at("id"), in_quotes(text) + " is not an id");
        out.id = std::string(text);
    }
    if (const json::value* const title = reader.need("title"))
        out.title =
            read_localized(ctx, *title, reader.at("title"), max_name_length);
    if (const json::value* const sensitive = reader.take("sensitive"))
        out.sensitive = read_bool(ctx, *sensitive, reader.at("sensitive"));
    if (const json::value* const parameters = reader.take("parameters"))
        out.parameters =
            read_parameters(ctx, *parameters, reader.at("parameters"));
    reader.finish();
    return out;
}

file_entry read_file(context& ctx, const json::value& value,
                     const std::string& where)
{
    file_entry out;
    object_reader reader(ctx, value, where);
    if (const json::value* const path = reader.need("path"))
        out.path = read_path(ctx, *path, reader.at("path"));
    if (const json::value* const size = reader.need("size"))
        out.size = read_unsigned(ctx, *size, reader.at("size"), 0,
                                 std::uint64_t{1} << 32);
    if (const json::value* const digest = reader.need("sha512"))
    {
        const std::string at = reader.at("sha512");
        const std::string_view text = read_word(ctx, *digest, at);
        bool good = text.size() == out.sha512.size() * 2;
        for (std::size_t i = 0; good && i < out.sha512.size(); ++i)
        {
            const auto high = hex_digit(text[i * 2]);
            const auto low = hex_digit(text[i * 2 + 1]);
            good = high && low;
            if (good)
                out.sha512[i] = static_cast<std::uint8_t>((*high << 4) | *low);
        }
        if (!good && !ctx.failed())
            ctx.invalid(at, "must be 128 hexadecimal digits");
    }
    reader.finish();
    return out;
}

/// Reads each item of a list with `read`, and refuses two with the same id.
template <class Item, class Read>
std::vector<Item> read_items(context& ctx, const json::value& value,
                             const std::string& where, std::size_t limit,
                             Read read)
{
    std::vector<Item> out;
    const json::array* const items = read_array(ctx, value, where, 0, limit);
    if (items == nullptr)
        return out;
    for (std::size_t i = 0; i < items->size() && !ctx.failed(); ++i)
    {
        const std::string at = element(where, i);
        Item item = read(ctx, (*items)[i], at);
        if (ctx.failed())
            break;
        const bool twice = std::ranges::any_of(
            out, [&](const Item& other) { return other.id == item.id; });
        if (twice)
            ctx.invalid(member(at, "id"),
                        in_quotes(item.id) + " is given twice");
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<permission_request>
read_permissions(context& ctx, const json::value& value,
                 const std::string& where)
{
    std::vector<permission_request> out;
    const json::array* const items =
        read_array(ctx, value, where, 0, max_permissions);
    if (items == nullptr)
        return out;
    for (std::size_t i = 0; i < items->size() && !ctx.failed(); ++i)
    {
        const std::string at = element(where, i);
        permission_request item = read_permission(ctx, (*items)[i], at);
        if (ctx.failed())
            break;
        const bool twice =
            std::ranges::any_of(out, [&](const permission_request& other) {
                return other.name == item.name;
            });
        if (twice)
            ctx.invalid(member(at, "name"),
                        in_quotes(to_string(item.name)) + " is given twice");
        out.push_back(std::move(item));
    }
    return out;
}

/// The files of a plugin must be the same files on every system: no two
/// paths that differ only in case, and no path that is both a file and a
/// folder of another.
void check_files(context& ctx, const manifest& out, std::string_view where)
{
    std::vector<std::string> folded;
    folded.reserve(out.files.size());
    for (const file_entry& file : out.files)
        folded.push_back(fold_path(file.path));
    std::vector<std::string> sorted = folded;
    std::ranges::sort(sorted);

    for (std::size_t i = 0; i < folded.size(); ++i)
    {
        const std::string at = member(element(where, i), "path");
        const std::string& path = folded[i];
        if (path == "plugin.json" || path == "plugin.sig")
        {
            ctx.fail(plugin_errc::layout_invalid, at,
                     "the manifest and its signature are not listed");
            return;
        }
        const auto range = std::ranges::equal_range(sorted, path);
        if (range.size() > 1)
        {
            ctx.fail(plugin_errc::layout_invalid, at,
                     in_quotes(out.files[i].path) +
                         " is listed twice, or differs from another path "
                         "only in case");
            return;
        }
        for (std::size_t slash = path.find('/');
             slash != std::string::npos; slash = path.find('/', slash + 1))
        {
            if (std::ranges::binary_search(sorted, path.substr(0, slash)))
            {
                ctx.fail(plugin_errc::layout_invalid, at,
                         in_quotes(out.files[i].path) +
                             " is inside a path that is listed as a file");
                return;
            }
        }
    }
    // By its exact name: on a system that tells case apart, a path that
    // differs in case is another file.
    const bool listed =
        std::ranges::any_of(out.files, [&](const file_entry& file) {
            return file.path == out.run.executable;
        });
    if (!listed)
        ctx.fail(plugin_errc::layout_invalid, "run.executable",
                 "is not one of the listed files");
}

/// The checks that span more than one part of the manifest.
void check_whole(context& ctx, const manifest& out)
{
    if (out.extension_points.empty() && out.bundle_sections.empty())
    {
        ctx.invalid("extension_points",
                    "the plugin must add at least one thing");
        return;
    }
    for (std::size_t i = 0; i < out.extension_points.size(); ++i)
    {
        const extension_point& point = out.extension_points[i];
        if (point.timing.type == timing_class::in_line)
        {
            const permission_request* const held =
                out.find(permission::modifier_inline);
            if (held == nullptr || !held->required)
            {
                ctx.invalid(
                    member(element("extension_points", i), "timing.class"),
                    "needs the permission \"modifier.inline\", as required");
                return;
            }
        }
        // A decoder reads the traffic of the places it is given, so it
        // cannot work without the tap.
        if (point.kind == extension_kind::decoder)
        {
            const permission_request* const held =
                out.find(permission::traffic_tap);
            if (held == nullptr || !held->required)
            {
                ctx.invalid(member(element("extension_points", i), "kind"),
                            "needs the permission \"traffic.tap\", as "
                            "required");
                return;
            }
        }
    }
    if (!out.bundle_sections.empty())
    {
        const permission_request* const held =
            out.find(permission::bundle_section);
        if (held == nullptr)
        {
            ctx.invalid("contributes.bundle_sections",
                        "needs the permission \"bundle.section\"");
            return;
        }
    }
    if (!out.files.empty())
        check_files(ctx, out, "files");
}

manifest read_manifest(context& ctx, const json::value& root)
{
    manifest out;
    object_reader reader(ctx, root, {});

    // The version comes first: a newer format has keys this reader would
    // call unknown, and the right answer for it is "needs a newer version".
    if (const json::value* const version = reader.need("manifest_version"))
    {
        const std::uint64_t number =
            read_unsigned(ctx, *version, "manifest_version", 1,
                          std::numeric_limits<std::uint32_t>::max());
        if (!ctx.failed() && number > manifest_format_version)
            ctx.fail(plugin_errc::manifest_unsupported_version,
                     "manifest_version",
                     "this version reads format " +
                         std::to_string(manifest_format_version) +
                         " and the manifest is format " +
                         std::to_string(number));
    }
    if (ctx.failed())
        return out;

    if (const json::value* const id = reader.need("id"))
    {
        const std::string_view text = read_word(ctx, *id, "id");
        if (!ctx.failed() && !is_plugin_id(text))
            ctx.invalid("id", in_quotes(text) + " is not a plugin id, such as "
                                             "\"com.example.my-plugin\"");
        out.id = std::string(text);
    }
    if (const json::value* const name = reader.need("name"))
        out.name = read_text(ctx, *name, "name", max_name_length);
    if (const json::value* const version = reader.need("version"))
    {
        const auto parsed = parse_version(read_word(ctx, *version, "version"));
        if (parsed)
            out.version = *parsed;
        else
            ctx.invalid("version", "must be a version such as \"1.2.0\"");
    }
    if (const json::value* const author = reader.take("author"))
        out.author = read_author(ctx, *author, "author");
    if (const json::value* const description = reader.take("description"))
        out.description =
            read_text(ctx, *description, "description", max_description_length);
    if (const json::value* const license = reader.take("license"))
        out.license = read_text(ctx, *license, "license", max_name_length);
    const auto read_day =
        [&](std::string_view key,
            std::optional<std::chrono::year_month_day>& into) {
            const json::value* const value = reader.take(key);
            if (value == nullptr)
                return;
            into = parse_date(read_word(ctx, *value, key));
            if (!into)
                ctx.invalid(key, "must be a date such as \"2027-12-31\"");
        };
    read_day("not_after", out.not_after);
    read_day("released", out.released);
    if (const json::value* const needs = reader.need("requires"))
        out.needs = read_needs(ctx, *needs, "requires");
    if (const json::value* const run = reader.need("run"))
        out.run = read_run(ctx, *run, "run");
    if (const json::value* const permissions = reader.take("permissions"))
        out.permissions = read_permissions(ctx, *permissions, "permissions");
    if (const json::value* const points = reader.take("extension_points"))
        out.extension_points = read_items<extension_point>(
            ctx, *points, "extension_points", max_extension_points,
            read_extension_point);
    if (const json::value* const contributes = reader.take("contributes"))
    {
        object_reader inner(ctx, *contributes, "contributes");
        if (const json::value* const sections = inner.take("bundle_sections"))
            out.bundle_sections = read_items<bundle_section>(
                ctx, *sections, "contributes.bundle_sections",
                max_bundle_sections, read_bundle_section);
        inner.finish();
    }
    if (const json::value* const files = reader.take("files"))
    {
        if (const json::array* const items =
                read_array(ctx, *files, "files", 1, max_files))
        {
            out.files.reserve(items->size());
            for (std::size_t i = 0; i < items->size() && !ctx.failed(); ++i)
                out.files.push_back(
                    read_file(ctx, (*items)[i], element("files", i)));
        }
    }
    reader.finish();
    if (!ctx.failed())
        check_whole(ctx, out);
    return out;
}

} // namespace

// -- public -----------------------------------------------------------------

std::string_view localized_text::best(std::string_view language) const noexcept
{
    const auto find = [&](std::string_view wanted) -> const std::string* {
        for (const auto& [tag, text] : entries)
        {
            if (tag == wanted)
                return &text;
        }
        return nullptr;
    };
    if (entries.empty())
        return {};
    if (const std::string* const exact = find(language))
        return *exact;
    if (const std::string* const base =
            find(language.substr(0, language.find('-'))))
        return *base;
    if (const std::string* const english = find("en"))
        return *english;
    if (const std::string* const plain = find({}))
        return *plain;
    return entries.front().second;
}

std::string to_string(const plugin_version& value)
{
    std::string out = std::to_string(value.major) + '.' +
                      std::to_string(value.minor) + '.' +
                      std::to_string(value.patch);
    if (!value.pre_release.empty())
    {
        out += '-';
        out += value.pre_release;
    }
    return out;
}

bool api_range::contains(api_version version) const noexcept
{
    if (version < min)
        return false;
    if (version.major != max_major)
        return version.major < max_major;
    return !max_minor || version.minor <= *max_minor;
}

std::string_view to_string(permission value) noexcept
{
    return name_of(permission_names, value);
}

std::optional<permission> parse_permission(std::string_view text) noexcept
{
    return find_named(permission_names, text);
}

std::string_view to_string(parameter_type value) noexcept
{
    return name_of(parameter_type_names, value);
}

std::string_view to_string(extension_kind value) noexcept
{
    return name_of(extension_kind_names, value);
}

std::string_view to_string(timing_class value) noexcept
{
    return name_of(timing_class_names, value);
}

const permission_request*
manifest::find(permission wanted) const noexcept
{
    for (const permission_request& request : permissions)
    {
        if (request.name == wanted)
            return &request;
    }
    return nullptr;
}

std::optional<std::string> check_relative_path(std::string_view path)
{
    if (path.empty())
        return "is empty";
    if (path.size() > max_path_length)
        return "is longer than " + std::to_string(max_path_length) +
               " characters";
    for (const char c : path)
    {
        if (!is_printable_ascii(c))
            return "has a character that is not plain ASCII";
        if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            return std::string("has the character ") + c;
    }
    if (path.front() == '/')
        return "must be relative";

    std::string_view rest = path;
    while (true)
    {
        const std::size_t slash = rest.find('/');
        const std::string_view name = rest.substr(0, slash);
        if (name.empty())
            return "has an empty name";
        if (name == "." || name == "..")
            return "has the name " + std::string(name);
        if (name.back() == '.' || name.back() == ' ' || name.front() == ' ')
            return "has a name that begins with a space, or ends in a dot or "
                   "a space";
        // Windows reserves a device name with any extension, and with
        // spaces before the dot: NUL, nul.txt and "nul .txt" are all the
        // device.
        std::string_view stem = name.substr(0, name.find('.'));
        while (!stem.empty() && stem.back() == ' ')
            stem.remove_suffix(1);
        std::string upper(stem);
        std::ranges::transform(upper, upper.begin(), to_upper);
        if (std::ranges::find(reserved_device_names,
                              std::string_view(upper)) !=
            reserved_device_names.end())
            return "has the reserved name " + std::string(stem);
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    return std::nullopt;
}

std::string fold_path(std::string_view path)
{
    std::string out(path);
    std::ranges::transform(out, out.begin(), to_lower);
    return out;
}

result<manifest> parse_manifest(std::string_view text, std::string* detail)
{
    context ctx;
    const auto failure = [&]() -> result<manifest> {
        if (detail != nullptr)
            *detail = ctx.detail;
        return ctx.error;
    };
    if (text.size() > max_manifest_size)
    {
        ctx.fail(plugin_errc::manifest_too_large, {},
                 "is larger than " + std::to_string(max_manifest_size) +
                     " bytes");
        return failure();
    }

    std::string what;
    const std::optional<json::value> root =
        ::vss::plugin::detail::parse_strict(text, max_manifest_depth,
                                            what);
    if (!root)
    {
        ctx.invalid({}, what);
        return failure();
    }
    manifest out = read_manifest(ctx, *root);
    if (ctx.failed())
        return failure();
    if (detail != nullptr)
        detail->clear();
    return out;
}

std::strong_ordering compare(const plugin_version& left,
                             const plugin_version& right) noexcept
{
    if (const auto order = left.major <=> right.major; order != 0)
        return order;
    if (const auto order = left.minor <=> right.minor; order != 0)
        return order;
    if (const auto order = left.patch <=> right.patch; order != 0)
        return order;
    // A version with a pre-release comes before the same version without.
    if (left.pre_release.empty() || right.pre_release.empty())
        return left.pre_release.empty() <=> right.pre_release.empty();

    std::string_view a = left.pre_release;
    std::string_view b = right.pre_release;
    while (true)
    {
        const std::size_t a_dot = a.find('.');
        const std::size_t b_dot = b.find('.');
        const std::string_view a_part = a.substr(0, a_dot);
        const std::string_view b_part = b.substr(0, b_dot);
        const bool a_number =
            !a_part.empty() && std::ranges::all_of(a_part, is_digit);
        const bool b_number =
            !b_part.empty() && std::ranges::all_of(b_part, is_digit);
        if (a_number != b_number)
        {
            // A number comes before a word.
            return a_number ? std::strong_ordering::less
                            : std::strong_ordering::greater;
        }
        if (a_number)
        {
            // Without leading zeros the longer number is the larger one.
            const std::string_view a_digits =
                a_part.substr(std::min(a_part.find_first_not_of('0'),
                                       a_part.size() - 1));
            const std::string_view b_digits =
                b_part.substr(std::min(b_part.find_first_not_of('0'),
                                       b_part.size() - 1));
            if (const auto order = a_digits.size() <=> b_digits.size();
                order != 0)
                return order;
            if (const auto order = a_digits <=> b_digits; order != 0)
                return order;
        }
        else if (const auto order = a_part <=> b_part; order != 0)
        {
            return order;
        }
        const bool a_last = a_dot == std::string_view::npos;
        const bool b_last = b_dot == std::string_view::npos;
        if (a_last || b_last)
        {
            // The one with more parts comes later.
            return b_last <=> a_last;
        }
        a.remove_prefix(a_dot + 1);
        b.remove_prefix(b_dot + 1);
    }
}

namespace detail
{

std::optional<json::value>
parse_strict(std::string_view text, std::size_t max_depth, std::string& what)
{
    json::parse_options options;
    options.max_depth = max_depth;

    // Once for the keys, and once for the values.  The first pass also finds
    // text that is not JSON, so the second cannot fail in a new way.
    {
        json::basic_parser<duplicate_key_finder> scanner(options);
        error_code failed;
        const std::size_t used =
            scanner.write_some(false, text.data(), text.size(), failed);
        if (scanner.handler().found())
        {
            what = "the key " + in_quotes(scanner.handler().repeated()) +
                   " is given twice in one object";
            return std::nullopt;
        }
        if (!failed && used != text.size())
        {
            // Only white space may follow the document.
            const std::string_view tail = text.substr(used);
            if (tail.find_first_not_of(" \t\r\n") != std::string_view::npos)
            {
                what = "has text after the JSON document";
                return std::nullopt;
            }
        }
        if (failed)
        {
            what = "is not JSON: " + failed.message();
            return std::nullopt;
        }
    }

    error_code failed;
    json::value root = json::parse(text, failed, {}, options);
    if (failed)
    {
        what = "is not JSON: " + failed.message();
        return std::nullopt;
    }
    return root;
}

bool valid_plugin_id(std::string_view text) noexcept
{
    return is_plugin_id(text);
}

bool clean_text(std::string_view text) noexcept
{
    return is_clean_text(text);
}

std::optional<plugin_version> version_from_text(std::string_view text) noexcept
{
    return parse_version(text);
}

std::optional<std::chrono::year_month_day>
date_from_text(std::string_view text) noexcept
{
    return parse_date(text);
}

std::string quoted(std::string_view text)
{
    return in_quotes(text);
}

} // namespace detail

} // namespace vss::plugin
