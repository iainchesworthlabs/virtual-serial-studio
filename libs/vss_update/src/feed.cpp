//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/update/feed.hpp>

#include "strict_json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss::update
{

namespace
{

namespace json = boost::json;
namespace sf = vss::license::signed_file;
using namespace std::string_view_literals;

constexpr std::array top_keys{"feed_version"sv, "serial"sv, "issued_at"sv,
                              "studio"sv, "plugins"sv};
constexpr std::array studio_keys{"channel"sv, "version"sv,   "released"sv,
                                 "summary"sv, "notes_url"sv, "files"sv};
constexpr std::array studio_file_keys{"kind"sv, "arch"sv, "name"sv,
                                      "url"sv,  "size"sv, "sha512"sv};
constexpr std::array plugin_keys{
    "id"sv,      "version"sv,   "released"sv, "requires_studio"sv,
    "summary"sv, "notes_url"sv, "files"sv};
constexpr std::array plugin_file_keys{"os"sv,  "arch"sv, "name"sv,
                                      "url"sv, "size"sv, "sha512"sv};

constexpr std::array all_channels{channel::stable, channel::preview};
constexpr std::array all_kinds{
    file_kind::windows_installer, file_kind::windows_zip, file_kind::macos_dmg,
    file_kind::macos_archive,     file_kind::linux_deb,   file_kind::linux_rpm,
    file_kind::linux_archive};
constexpr std::array all_architectures{architecture::x86_64,
                                       architecture::arm64};
constexpr std::array all_systems{operating_system::windows,
                                 operating_system::macos,
                                 operating_system::linux};

constexpr std::size_t sha512_text_length = std::tuple_size_v<sha512_digest> * 2;

// -- the words of the feed --------------------------------------------------

template <class Enum, std::size_t N>
std::optional<Enum> find_name(const std::array<Enum, N>& all,
                              std::string_view text) noexcept
{
    for (const Enum value : all)
    {
        if (to_string(value) == text)
            return value;
    }
    return std::nullopt;
}

// -- what a person reads when a feed is refused -----------------------------

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

void set_detail(std::string* detail, std::string text)
{
    if (detail != nullptr)
        *detail = std::move(text);
}

/// `where.key`, or `key` at the top.
std::string at(std::string_view where, std::string_view key)
{
    std::string out(where);
    if (!out.empty())
        out += '.';
    out += key;
    return out;
}

std::string at(std::string_view where, std::size_t index)
{
    std::string out(where);
    out += '[';
    out += std::to_string(index);
    out += ']';
    return out;
}

std::string_view view_of(const json::string& text) noexcept
{
    return {text.data(), text.size()};
}

// -- the rules of a value ---------------------------------------------------

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// The value of `count` digits of `text` from `pos`, or -1.
int digits_at(std::string_view text, std::size_t pos,
              std::size_t count) noexcept
{
    int value = 0;
    for (std::size_t i = pos; i < pos + count; ++i)
    {
        if (i >= text.size() || !is_digit(text[i]))
            return -1;
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

/// `YYYY-MM-DD`, a day that exists.
std::optional<std::chrono::year_month_day>
date_from_text(std::string_view text) noexcept
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-')
        return std::nullopt;
    const int year = digits_at(text, 0, 4);
    const int month = digits_at(text, 5, 2);
    const int day = digits_at(text, 8, 2);
    // Year 0 is refused: the tool that writes a feed has no such year.
    if (year < 1 || month < 0 || day < 0)
        return std::nullopt;
    const std::chrono::year_month_day date{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok())
        return std::nullopt;
    return date;
}

/// `YYYY-MM-DDTHH:MM:SSZ`, a day and a time that exist.
std::optional<std::chrono::sys_seconds>
time_from_text(std::string_view text) noexcept
{
    if (text.size() != 20 || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != 'Z')
        return std::nullopt;
    const auto date = date_from_text(text.substr(0, 10));
    const int hour = digits_at(text, 11, 2);
    const int minute = digits_at(text, 14, 2);
    const int second = digits_at(text, 17, 2);
    if (!date || hour < 0 || minute < 0 || second < 0 || hour > 23 ||
        minute > 59 || second > 59)
        return std::nullopt;
    return std::chrono::sys_days{*date} + std::chrono::hours{hour} +
           std::chrono::minutes{minute} + std::chrono::seconds{second};
}

/// Whether `stem`, the part of a name before its first dot, is a name that
/// Windows gives to a device whatever follows the dot: CON, PRN, AUX, NUL,
/// COM0 to COM9 and LPT0 to LPT9, in any case.
bool is_device_name(std::string_view stem) noexcept
{
    const auto upper = [](char c)
    { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
    if (stem.size() == 3)
    {
        const std::array<char, 3> word{upper(stem[0]), upper(stem[1]),
                                       upper(stem[2])};
        const std::string_view text{word.data(), word.size()};
        return text == "CON" || text == "PRN" || text == "AUX" || text == "NUL";
    }
    if (stem.size() == 4 && is_digit(stem[3]))
    {
        const std::array<char, 3> word{upper(stem[0]), upper(stem[1]),
                                       upper(stem[2])};
        const std::string_view text{word.data(), word.size()};
        return text == "COM" || text == "LPT";
    }
    return false;
}

/// 1 to 255 characters of `[A-Za-z0-9._+~-]`, not starting or ending with a
/// dot and not a device name of Windows: a file name on every system, and
/// nothing that can be a path.
bool valid_name(std::string_view text) noexcept
{
    if (text.empty() || text.size() > max_name_length || text.front() == '.' ||
        text.back() == '.' || is_device_name(text.substr(0, text.find('.'))))
        return false;
    return std::ranges::all_of(text,
                               [](char c)
                               {
                                   return is_digit(c) ||
                                          (c >= 'a' && c <= 'z') ||
                                          (c >= 'A' && c <= 'Z') || c == '.' ||
                                          c == '_' || c == '+' || c == '~' ||
                                          c == '-';
                               });
}

/// An `https` address with a host, in printable ASCII, with no user name or
/// password and no backslash.
bool valid_url(std::string_view text) noexcept
{
    constexpr std::string_view scheme = "https://";
    if (text.empty() || text.size() > max_url_length ||
        !text.starts_with(scheme))
        return false;
    const bool printable = std::ranges::all_of(
        text, [](char c) { return c >= 0x21 && c <= 0x7e && c != '\\'; });
    if (!printable)
        return false;
    // Everything up to the first slash after the scheme is the authority and
    // anything after it. An `@` there is a user name or a password.
    std::string_view authority = text.substr(scheme.size());
    authority = authority.substr(0, authority.find('/'));
    if (authority.find('@') != std::string_view::npos)
        return false;
    // The host runs up to a port, a query or a fragment, and is not empty.
    const std::string_view host =
        authority.substr(0, authority.find_first_of(":?#"));
    return !host.empty();
}

/// Valid UTF-8 with no control character but the new line: not the C0 set,
/// not DEL and not the C1 set.
bool valid_summary(std::string_view text) noexcept
{
    if (text.size() > max_summary_length)
        return false;
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<std::uint8_t>(text[i]);
        if (lead < 0x80)
        {
            if ((lead < 0x20 && lead != '\n') || lead == 0x7f)
                return false;
            ++i;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (lead >= 0xc2 && lead <= 0xdf)
        {
            length = 2;
            code = lead & 0x1fU;
        }
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            length = 3;
            code = lead & 0x0fU;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            length = 4;
            code = lead & 0x07U;
        }
        else
        {
            return false;
        }
        if (i + length > text.size())
            return false;
        for (std::size_t k = 1; k < length; ++k)
        {
            const auto next = static_cast<std::uint8_t>(text[i + k]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            code = (code << 6U) | (next & 0x3fU);
        }
        const bool overlong =
            (length == 3 && code < 0x800) || (length == 4 && code < 0x10000);
        const bool surrogate = code >= 0xd800 && code <= 0xdfff;
        const bool control = code >= 0x80 && code <= 0x9f;
        if (overlong || surrogate || control || code > 0x10ffff)
            return false;
        i += length;
    }
    return true;
}

/// The official prefix and at least one more character, 128 at most, each a
/// lower-case letter, a digit, a dot or a hyphen.
bool valid_plugin_id(std::string_view text) noexcept
{
    if (text.size() <= official_plugin_prefix.size() ||
        text.size() > max_plugin_id_length ||
        !text.starts_with(official_plugin_prefix))
        return false;
    return std::ranges::all_of(text,
                               [](char c)
                               {
                                   return is_digit(c) ||
                                          (c >= 'a' && c <= 'z') || c == '.' ||
                                          c == '-';
                               });
}

std::optional<std::uint8_t> hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<std::uint8_t>(c - 'a' + 10);
    return std::nullopt;
}

/// 128 lower-case hex characters. Upper case is refused, so one hash has one
/// spelling.
std::optional<sha512_digest> digest_from_text(std::string_view text) noexcept
{
    if (text.size() != sha512_text_length)
        return std::nullopt;
    sha512_digest out{};
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const auto high = hex_value(text[i * 2]);
        const auto low = hex_value(text[i * 2 + 1]);
        if (!high || !low)
            return std::nullopt;
        out[i] = static_cast<std::uint8_t>((*high << 4U) | *low);
    }
    return out;
}

// -- reading the members of an object ---------------------------------------

/// The object, when `value` is one and has no key outside `known`.
template <std::size_t N>
const json::object* object_of(reading& state, const json::value& value,
                              std::string_view where,
                              const std::array<std::string_view, N>& known)
{
    if (!value.is_object())
    {
        state.fail(where, "must be an object");
        return nullptr;
    }
    const json::object& object = value.get_object();
    for (const auto& entry : object)
    {
        const std::string_view key{entry.key().data(), entry.key().size()};
        if (std::ranges::find(known, key) == known.end())
        {
            state.fail(where, "has the key " + detail::quoted(key) +
                                  ", which this version does not read");
            return nullptr;
        }
    }
    return &object;
}

/// The member, or an error when it is missing.
const json::value* required_member(reading& state, const json::object& object,
                                   std::string_view where, std::string_view key)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr)
        state.fail(at(where, key), "is required");
    return value;
}

/// The member as a string; an error when it is missing or something else.
std::optional<std::string_view> text_member(reading& state,
                                            const json::object& object,
                                            std::string_view where,
                                            std::string_view key)
{
    const json::value* const value = required_member(state, object, where, key);
    if (value == nullptr)
        return std::nullopt;
    if (!value->is_string())
    {
        state.fail(at(where, key), "must be a string");
        return std::nullopt;
    }
    return view_of(value->get_string());
}

/// A whole number from `least` to `most`.
std::optional<std::uint64_t> number_of(reading& state, const json::value& value,
                                       std::string_view where,
                                       std::uint64_t least, std::uint64_t most)
{
    std::uint64_t number = 0;
    bool whole = false;
    if (value.is_uint64())
    {
        number = value.get_uint64();
        whole = true;
    }
    else if (value.is_int64() && value.get_int64() >= 0)
    {
        number = static_cast<std::uint64_t>(value.get_int64());
        whole = true;
    }
    if (!whole || number < least || number > most)
    {
        state.fail(where, "must be a whole number from " +
                              std::to_string(least) + " to " +
                              std::to_string(most));
        return std::nullopt;
    }
    return number;
}

std::optional<std::uint64_t>
number_member(reading& state, const json::object& object,
              std::string_view where, std::string_view key, std::uint64_t least,
              std::uint64_t most)
{
    const json::value* const value = required_member(state, object, where, key);
    if (value == nullptr)
        return std::nullopt;
    return number_of(state, *value, at(where, key), least, most);
}

std::optional<std::chrono::year_month_day>
date_member(reading& state, const json::object& object, std::string_view where,
            std::string_view key)
{
    const auto text = text_member(state, object, where, key);
    if (!text)
        return std::nullopt;
    const auto date = date_from_text(*text);
    if (!date)
        state.fail(at(where, key), "must be a date such as 2026-12-01");
    return date;
}

/// A version that no tag could not have produced: not a developer build.
std::optional<version> version_member(reading& state,
                                      const json::object& object,
                                      std::string_view where,
                                      std::string_view key)
{
    const auto text = text_member(state, object, where, key);
    if (!text)
        return std::nullopt;
    auto parsed = parse_version(*text);
    if (!parsed)
    {
        state.fail(at(where, key), "must be a version such as 0.2.0 or "
                                   "0.3.0-rc.1, not " +
                                       detail::quoted(*text));
        return std::nullopt;
    }
    if (is_developer_build(*parsed))
    {
        state.fail(at(where, key), "cannot be a developer version");
        return std::nullopt;
    }
    return parsed;
}

/// `summary` may be missing, and then it is empty.
std::optional<std::string> summary_member(reading& state,
                                          const json::object& object,
                                          std::string_view where)
{
    const json::value* const value = object.if_contains("summary");
    if (value == nullptr)
        return std::string();
    if (!value->is_string())
    {
        state.fail(at(where, "summary"), "must be a string");
        return std::nullopt;
    }
    const std::string_view text = view_of(value->get_string());
    if (!valid_summary(text))
    {
        state.fail(at(where, "summary"),
                   "must be plain text of at most " +
                       std::to_string(max_summary_length) +
                       " bytes, with no control character but a new line");
        return std::nullopt;
    }
    return std::string(text);
}

std::optional<std::string> url_member(reading& state,
                                      const json::object& object,
                                      std::string_view where,
                                      std::string_view key)
{
    const auto text = text_member(state, object, where, key);
    if (!text)
        return std::nullopt;
    if (!valid_url(*text))
    {
        state.fail(at(where, key),
                   "must be an https address of at most " +
                       std::to_string(max_url_length) +
                       " printable ASCII characters, with no user name or "
                       "password");
        return std::nullopt;
    }
    return std::string(*text);
}

std::optional<std::string>
name_member(reading& state, const json::object& object, std::string_view where)
{
    const auto text = text_member(state, object, where, "name");
    if (!text)
        return std::nullopt;
    if (!valid_name(*text))
    {
        state.fail(at(where, "name"),
                   "must be a file name of 1 to " +
                       std::to_string(max_name_length) +
                       " letters, digits and . _ + ~ -, not starting with "
                       "a dot");
        return std::nullopt;
    }
    return std::string(*text);
}

std::optional<sha512_digest> digest_member(reading& state,
                                           const json::object& object,
                                           std::string_view where)
{
    const auto text = text_member(state, object, where, "sha512");
    if (!text)
        return std::nullopt;
    const auto digest = digest_from_text(*text);
    if (!digest)
        state.fail(at(where, "sha512"),
                   "must be 128 lower-case hex characters");
    return digest;
}

/// The member as a list of at most `most` entries. A missing member is an
/// error unless `may_be_missing`, and then the list is empty.
std::optional<std::span<const json::value>>
list_member(reading& state, const json::object& object, std::string_view where,
            std::string_view key, std::size_t most, bool may_be_missing)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr)
    {
        if (may_be_missing)
            return std::span<const json::value>{};
        state.fail(at(where, key), "is required");
        return std::nullopt;
    }
    if (!value->is_array())
    {
        state.fail(at(where, key), "must be a list");
        return std::nullopt;
    }
    const json::array& list = value->get_array();
    if (list.size() > most)
    {
        state.fail(at(where, key),
                   "has more than " + std::to_string(most) + " entries");
        return std::nullopt;
    }
    return std::span<const json::value>{list.data(), list.size()};
}

// -- the entries ------------------------------------------------------------

template <class Enum, std::size_t N>
std::optional<Enum> word_member(reading& state, const json::object& object,
                                std::string_view where, std::string_view key,
                                const std::array<Enum, N>& all,
                                std::string_view choices)
{
    const auto text = text_member(state, object, where, key);
    if (!text)
        return std::nullopt;
    const auto value = find_name(all, *text);
    if (!value)
        state.fail(at(where, key), "must be " + std::string(choices) +
                                       ", not " + detail::quoted(*text));
    return value;
}

std::optional<studio_file> read_studio_file(reading& state,
                                            const json::value& value,
                                            const std::string& where)
{
    const json::object* const object =
        object_of(state, value, where, studio_file_keys);
    if (object == nullptr)
        return std::nullopt;
    const auto kind =
        word_member(state, *object, where, "kind", all_kinds,
                    "windows-installer, windows-zip, macos-dmg, "
                    "macos-archive, linux-deb, linux-rpm or linux-archive");
    const auto arch = word_member(state, *object, where, "arch",
                                  all_architectures, "x86_64 or arm64");
    auto name = name_member(state, *object, where);
    auto url = url_member(state, *object, where, "url");
    const auto size =
        number_member(state, *object, where, "size", 1, max_file_size);
    const auto digest = digest_member(state, *object, where);
    if (!kind || !arch || !name || !url || !size || !digest)
        return std::nullopt;
    return studio_file{*kind,           *arch, std::move(*name),
                       std::move(*url), *size, *digest};
}

std::optional<plugin_file> read_plugin_file(reading& state,
                                            const json::value& value,
                                            const std::string& where)
{
    const json::object* const object =
        object_of(state, value, where, plugin_file_keys);
    if (object == nullptr)
        return std::nullopt;
    const auto which_os = word_member(state, *object, where, "os", all_systems,
                                      "windows, macos or linux");
    const auto arch = word_member(state, *object, where, "arch",
                                  all_architectures, "x86_64 or arm64");
    auto name = name_member(state, *object, where);
    auto url = url_member(state, *object, where, "url");
    const auto size =
        number_member(state, *object, where, "size", 1, max_file_size);
    const auto digest = digest_member(state, *object, where);
    if (!which_os || !arch || !name || !url || !size || !digest)
        return std::nullopt;
    return plugin_file{*which_os,       *arch, std::move(*name),
                       std::move(*url), *size, *digest};
}

std::optional<studio_release> read_studio_release(reading& state,
                                                  const json::value& value,
                                                  const std::string& where)
{
    const json::object* const object =
        object_of(state, value, where, studio_keys);
    if (object == nullptr)
        return std::nullopt;

    const auto which = word_member(state, *object, where, "channel",
                                   all_channels, "stable or preview");
    auto release_version = version_member(state, *object, where, "version");
    const auto released = date_member(state, *object, where, "released");
    auto summary = summary_member(state, *object, where);
    auto notes_url = url_member(state, *object, where, "notes_url");
    const auto files =
        list_member(state, *object, where, "files", max_files, false);
    if (!which || !release_version || !released || !summary || !notes_url ||
        !files)
        return std::nullopt;

    // The channel says what kind of version belongs in it.
    if (*which == channel::stable && release_version->is_prerelease())
    {
        state.fail(at(where, "version"),
                   "cannot be a pre-release in the stable channel");
        return std::nullopt;
    }
    if (*which == channel::preview && !release_version->is_prerelease())
    {
        state.fail(at(where, "version"),
                   "must be a pre-release in the preview channel");
        return std::nullopt;
    }
    if (files->empty())
    {
        state.fail(at(where, "files"), "must have at least one entry");
        return std::nullopt;
    }

    studio_release out;
    out.channel = *which;
    out.version = std::move(*release_version);
    out.released = *released;
    out.summary = std::move(*summary);
    out.notes_url = std::move(*notes_url);
    for (std::size_t i = 0; i < files->size(); ++i)
    {
        const std::string file_where = at(at(where, "files"), i);
        auto file = read_studio_file(state, (*files)[i], file_where);
        if (!file)
            return std::nullopt;
        const bool repeated = std::ranges::any_of(
            out.files,
            [&](const studio_file& earlier)
            {
                return earlier.kind == file->kind && earlier.arch == file->arch;
            });
        if (repeated)
        {
            state.fail(file_where, "repeats the kind and arch of an earlier "
                                   "file");
            return std::nullopt;
        }
        out.files.push_back(std::move(*file));
    }
    return out;
}

std::optional<plugin_release> read_plugin_release(reading& state,
                                                  const json::value& value,
                                                  const std::string& where)
{
    const json::object* const object =
        object_of(state, value, where, plugin_keys);
    if (object == nullptr)
        return std::nullopt;

    const auto id = text_member(state, *object, where, "id");
    if (id && !valid_plugin_id(*id))
    {
        state.fail(at(where, "id"),
                   "must start with " + std::string(official_plugin_prefix) +
                       " and have at most " +
                       std::to_string(max_plugin_id_length) +
                       " lower-case letters, digits, dots and hyphens");
        return std::nullopt;
    }
    auto release_version = version_member(state, *object, where, "version");
    const auto released = date_member(state, *object, where, "released");
    auto requires_studio =
        version_member(state, *object, where, "requires_studio");
    auto summary = summary_member(state, *object, where);
    auto notes_url = url_member(state, *object, where, "notes_url");
    const auto files =
        list_member(state, *object, where, "files", max_files, false);
    if (!id || !release_version || !released || !requires_studio || !summary ||
        !notes_url || !files)
        return std::nullopt;

    plugin_release out;
    out.id = std::string(*id);
    out.version = std::move(*release_version);
    out.released = *released;
    out.requires_studio = std::move(*requires_studio);
    out.summary = std::move(*summary);
    out.notes_url = std::move(*notes_url);
    for (std::size_t i = 0; i < files->size(); ++i)
    {
        const std::string file_where = at(at(where, "files"), i);
        auto file = read_plugin_file(state, (*files)[i], file_where);
        if (!file)
            return std::nullopt;
        const bool repeated = std::ranges::any_of(
            out.files, [&](const plugin_file& earlier)
            { return earlier.os == file->os && earlier.arch == file->arch; });
        if (repeated)
        {
            state.fail(file_where, "repeats the os and arch of an earlier "
                                   "file");
            return std::nullopt;
        }
        out.files.push_back(std::move(*file));
    }
    return out;
}

/// The payload bytes as text.
std::string text_of(const std::vector<std::uint8_t>& bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (const std::uint8_t byte : bytes)
        out += static_cast<char>(byte);
    return out;
}

} // namespace

std::string_view to_string(channel value) noexcept
{
    switch (value)
    {
    case channel::stable:
        return "stable";
    case channel::preview:
        return "preview";
    }
    return {};
}

std::string_view to_string(file_kind value) noexcept
{
    switch (value)
    {
    case file_kind::windows_installer:
        return "windows-installer";
    case file_kind::windows_zip:
        return "windows-zip";
    case file_kind::macos_dmg:
        return "macos-dmg";
    case file_kind::macos_archive:
        return "macos-archive";
    case file_kind::linux_deb:
        return "linux-deb";
    case file_kind::linux_rpm:
        return "linux-rpm";
    case file_kind::linux_archive:
        return "linux-archive";
    }
    return {};
}

std::string_view to_string(architecture value) noexcept
{
    switch (value)
    {
    case architecture::x86_64:
        return "x86_64";
    case architecture::arm64:
        return "arm64";
    }
    return {};
}

std::string_view to_string(operating_system value) noexcept
{
    switch (value)
    {
    case operating_system::windows:
        return "windows";
    case operating_system::macos:
        return "macos";
    case operating_system::linux:
        return "linux";
    }
    return {};
}

std::optional<channel> channel_from(std::string_view text) noexcept
{
    return find_name(all_channels, text);
}

std::optional<file_kind> file_kind_from(std::string_view text) noexcept
{
    return find_name(all_kinds, text);
}

std::optional<architecture> architecture_from(std::string_view text) noexcept
{
    return find_name(all_architectures, text);
}

std::optional<operating_system>
operating_system_from(std::string_view text) noexcept
{
    return find_name(all_systems, text);
}

result<feed> parse_feed_payload(std::string_view payload, std::string* detail)
{
    // `state` collects the first problem. Every failure return goes through
    // `failure`, which copies that problem to the caller's detail string.
    reading state;
    const auto failure = [&](update_errc code) -> result<feed>
    {
        set_detail(detail, state.problem);
        return code;
    };

    // Parse strictly, and require a JSON object at the top.
    std::string what;
    const std::optional<json::value> root = detail::parse_strict(payload, what);
    if (!root)
    {
        state.fail("feed", what);
        return failure(update_errc::feed_malformed);
    }
    if (!root->is_object())
    {
        state.fail("feed", "must be an object");
        return failure(update_errc::feed_malformed);
    }
    const json::object& top = root->get_object();

    // The format comes first: a later format may have other keys, and must be
    // told apart from a feed that is broken.
    const json::value* const format = top.if_contains("feed_version");
    if (format == nullptr)
    {
        state.fail("feed_version", "is required");
        return failure(update_errc::feed_malformed);
    }
    if (!format->is_int64() && !format->is_uint64())
    {
        state.fail("feed_version", "must be a whole number");
        return failure(update_errc::feed_malformed);
    }
    // A whole number that is not the version this code reads is reported as
    // an unsupported version, not as a broken feed.
    if (!format->is_int64() ||
        format->get_int64() != std::int64_t{feed_format_version})
    {
        state.fail("feed_version", "this version reads only " +
                                       std::to_string(feed_format_version));
        return failure(update_errc::feed_version_unsupported);
    }

    // With the format known, check the top level keys against the allowed
    // set, then read the serial number and the issue time.
    if (object_of(state, *root, "feed", top_keys) == nullptr)
        return failure(update_errc::feed_malformed);

    feed out;
    const auto serial = number_member(state, top, "", "serial", 1, max_serial);
    const auto issued_text = text_member(state, top, "", "issued_at");
    if (issued_text)
    {
        if (const auto issued = time_from_text(*issued_text))
            out.issued_at = *issued;
        else
            state.fail("issued_at",
                       "must be a UTC time such as 2026-12-01T03:40:00Z");
    }
    // Both members are read before this check so that one pass reports
    // whichever of the two is wrong.
    if (serial)
        out.serial = *serial;
    if (state.failed())
        return failure(update_errc::feed_malformed);

    // The Studio releases. Each channel may appear only once.
    const auto studio_list =
        list_member(state, top, "", "studio", max_studio_releases, false);
    if (!studio_list)
        return failure(update_errc::feed_malformed);
    for (std::size_t i = 0; i < studio_list->size(); ++i)
    {
        auto release =
            read_studio_release(state, (*studio_list)[i], at("studio", i));
        if (!release)
            return failure(update_errc::feed_malformed);
        const bool repeated = std::ranges::any_of(
            out.studio, [&](const studio_release& earlier)
            { return earlier.channel == release->channel; });
        if (repeated)
        {
            state.fail(at("studio", i),
                       "repeats the channel " +
                           std::string(to_string(release->channel)));
            return failure(update_errc::feed_malformed);
        }
        out.studio.push_back(std::move(*release));
    }

    // The plugin releases. Each plugin id may appear only once.
    const auto plugin_list =
        list_member(state, top, "", "plugins", max_plugins, true);
    if (!plugin_list)
        return failure(update_errc::feed_malformed);
    for (std::size_t i = 0; i < plugin_list->size(); ++i)
    {
        auto release =
            read_plugin_release(state, (*plugin_list)[i], at("plugins", i));
        if (!release)
            return failure(update_errc::feed_malformed);
        const bool repeated =
            std::ranges::any_of(out.plugins, [&](const plugin_release& earlier)
                                { return earlier.id == release->id; });
        if (repeated)
        {
            state.fail(at("plugins", i),
                       "repeats the id " + detail::quoted(release->id));
            return failure(update_errc::feed_malformed);
        }
        out.plugins.push_back(std::move(*release));
    }

    // Everything read cleanly, so clear the caller's detail string.
    set_detail(detail, {});
    return out;
}

result<feed> read_feed(std::string_view file_text,
                       std::span<const sf::trusted_key> keys,
                       std::uint64_t highest_serial_seen, std::string* detail)
{
    const auto failure = [&](boost::system::error_code code,
                             std::string text) -> result<feed>
    {
        set_detail(detail, std::move(text));
        return code;
    };

    if (keys.empty())
        return failure(update_errc::no_update_keys,
                       "this build has no keys to check updates with");

    const auto envelope = sf::parse(file_text);
    if (!envelope)
        return failure(update_errc::signature_invalid,
                       "the update information is not a signed file: " +
                           envelope.error().message());

    // Nothing of the payload is read before this succeeds.
    const auto key = sf::verify(*envelope, feed_purpose, keys);
    if (!key)
        return failure(update_errc::signature_invalid,
                       "the update information could not be trusted: " +
                           key.error().message());

    std::string problem;
    auto parsed = parse_feed_payload(text_of(envelope->payload), &problem);
    if (!parsed)
        return failure(parsed.error(), std::move(problem));

    if (const auto window = sf::check_key_window(**key, parsed->issued_at);
        !window)
        return failure(update_errc::signature_invalid,
                       "the key " + (*key)->key_id +
                           " was not valid when the feed was issued");

    if (parsed->serial < highest_serial_seen)
        return failure(update_errc::feed_rolled_back,
                       "serial " + std::to_string(parsed->serial) +
                           " is lower than " +
                           std::to_string(highest_serial_seen) +
                           ", which was accepted before");

    set_detail(detail, {});
    return parsed;
}

std::vector<sf::trusted_key> product_feed_keys()
{
    return sf::without_test_keys(builtin_feed_keys());
}

} // namespace vss::update
