//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The token store on disk (see token_file.hpp for the document).  Loading is
// strict: a document that is not exactly what `save_token_file` writes is
// refused as a whole, and nothing in it is repaired or guessed.  An unknown key
// is the one thing that is let through, so that a later version of the
// program can add a field without making an older file unreadable.
//

#include "token_file.hpp"

#include "platform/private_storage_platform.hpp"

#include <vss/control/proto/policy.hpp>
#include <vss/control/proto/token.hpp>

#include <boost/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace vss::control
{
namespace json = boost::json;

namespace
{

constexpr std::size_t max_file_size = std::size_t{4} * 1024 * 1024;
constexpr std::size_t max_depth = 8;
constexpr std::uint64_t file_version = 1;
constexpr std::size_t digest_text_size = 2 * std::tuple_size_v<token_digest>;

constexpr transport known_transports[] = {transport::local,
                                          transport::local_admin,
                                          transport::loopback, transport::tls};

error_code bad_document()
{
    return make_error_code(errc::config_error);
}

// -- reading -----------------------------------------------------------------

/// The text of a member, if there is one and it is a string.
std::optional<std::string_view> as_text(const json::value* item)
{
    if (item == nullptr || !item->is_string())
        return std::nullopt;
    const json::string& text = item->get_string();
    return std::string_view(text.data(), text.size());
}

/// A whole number of zero or more, written without a fraction or an exponent.
std::optional<std::uint64_t> as_unsigned(const json::value* item)
{
    if (item == nullptr)
        return std::nullopt;
    if (item->is_uint64())
        return item->get_uint64();
    if (item->is_int64() && item->get_int64() >= 0)
        return static_cast<std::uint64_t>(item->get_int64());
    return std::nullopt;
}

/// Milliseconds since the epoch, as long as the system clock can hold them.
std::optional<wall_clock::time_point> as_time(const json::value* item)
{
    const std::optional<std::uint64_t> number = as_unsigned(item);
    if (!number)
        return std::nullopt;
    const auto latest = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            wall_clock::time_point::max().time_since_epoch())
            .count());
    if (*number > latest)
        return std::nullopt;
    const std::chrono::milliseconds millis{
        static_cast<std::chrono::milliseconds::rep>(*number)};
    return wall_clock::time_point(
        std::chrono::duration_cast<wall_clock::duration>(millis));
}

int hex_value(char letter) noexcept
{
    if (letter >= '0' && letter <= '9')
        return letter - '0';
    // Lower case only: the file is written by us and has one spelling.
    if (letter >= 'a' && letter <= 'f')
        return letter - 'a' + 10;
    return -1;
}

/// 64 lower-case hex digits.
std::optional<token_digest> as_digest(const json::value* item)
{
    const std::optional<std::string_view> text = as_text(item);
    if (!text || text->size() != digest_text_size)
        return std::nullopt;
    token_digest digest{};
    for (std::size_t position = 0; position < digest.size(); ++position)
    {
        const int high = hex_value((*text)[2 * position]);
        const int low = hex_value((*text)[2 * position + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        digest[position] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return digest;
}

/// A scope list: at least one name, each one a scope a token may hold, none
/// twice.  `admin` is never in a token.
std::optional<scope_set> as_scopes(const json::value* item)
{
    if (item == nullptr || !item->is_array())
        return std::nullopt;
    const json::array& names = item->get_array();
    if (names.empty())
        return std::nullopt;
    scope_set members;
    for (const json::value& entry : names)
    {
        const std::optional<std::string_view> text = as_text(&entry);
        if (!text)
            return std::nullopt;
        const std::optional<scope> parsed = parse_scope(*text);
        if (!parsed || *parsed == scope::admin || members.contains(*parsed))
            return std::nullopt;
        members = members | scope_set{*parsed};
    }
    return members;
}

std::optional<transport> parse_transport(std::string_view name) noexcept
{
    for (const transport candidate : known_transports)
    {
        if (transport_name(candidate) == name)
            return candidate;
    }
    return std::nullopt;
}

/// A transport list: at least one name, each one known, none twice.
std::optional<std::vector<transport>> as_transports(const json::value* item)
{
    if (item == nullptr || !item->is_array())
        return std::nullopt;
    const json::array& names = item->get_array();
    if (names.empty())
        return std::nullopt;
    std::vector<transport> found;
    for (const json::value& entry : names)
    {
        const std::optional<std::string_view> text = as_text(&entry);
        if (!text)
            return std::nullopt;
        const std::optional<transport> parsed = parse_transport(*text);
        if (!parsed)
            return std::nullopt;
        for (const transport earlier : found)
        {
            if (earlier == *parsed)
                return std::nullopt;
        }
        found.push_back(*parsed);
    }
    return found;
}

std::optional<token_record> as_record(const json::value& item)
{
    if (!item.is_object())
        return std::nullopt;
    const json::object& object = item.get_object();

    const auto id = as_text(object.if_contains("id"));
    const auto label = as_text(object.if_contains("label"));
    auto scopes = as_scopes(object.if_contains("scopes"));
    auto transports = as_transports(object.if_contains("transports"));
    const auto created = as_time(object.if_contains("created"));
    const auto expires = as_time(object.if_contains("expires"));
    const auto digest = as_digest(object.if_contains("hash"));
    const json::value* const revoked = object.if_contains("revoked");
    const json::value* const last_used = object.if_contains("last_used");

    if (!id || !valid_token_id(*id))
        return std::nullopt;
    if (!label || label->size() > max_token_label_size)
        return std::nullopt;
    if (!scopes || !transports || !created || !expires || !digest)
        return std::nullopt;
    if (revoked == nullptr || !revoked->is_bool())
        return std::nullopt;
    if (last_used == nullptr)
        return std::nullopt;

    token_record record;
    record.id = std::string(*id);
    record.label = std::string(*label);
    record.scopes = *scopes;
    record.transports = std::move(*transports);
    record.created = *created;
    record.expires = *expires;
    record.revoked = revoked->get_bool();
    record.hash = *digest;
    if (!last_used->is_null())
    {
        const auto used = as_time(last_used);
        if (!used)
            return std::nullopt;
        record.last_used = *used;
    }
    return record;
}

result<token_file_content> parse_document(std::string_view text)
{
    json::parse_options options;
    options.max_depth = max_depth;
    boost::system::error_code failed;
    const json::value document =
        json::parse(text, failed, json::storage_ptr(), options);
    if (failed || !document.is_object())
        return bad_document();
    const json::object& root = document.get_object();

    const std::optional<std::uint64_t> version =
        as_unsigned(root.if_contains("version"));
    if (!version || *version != file_version)
        return bad_document();

    token_file_content content;
    const auto latest_seen = as_time(root.if_contains("latest_seen"));
    if (!latest_seen)
        return bad_document();
    content.latest_seen = *latest_seen;

    const json::value* const list = root.if_contains("tokens");
    if (list == nullptr || !list->is_array())
        return bad_document();
    const json::array& records = list->get_array();
    if (records.size() > max_tokens)
        return bad_document();

    std::set<std::string> seen;
    content.tokens.reserve(records.size());
    for (const json::value& item : records)
    {
        std::optional<token_record> record = as_record(item);
        if (!record || !seen.insert(record->id).second)
            return bad_document();
        content.tokens.push_back(std::move(*record));
    }
    return content;
}

// -- writing -----------------------------------------------------------------

json::value text_value(std::string_view text)
{
    return json::value(json::string(text.data(), text.size()));
}

/// Milliseconds since the epoch, cut off (not rounded) below the millisecond;
/// nothing for a time before the epoch.
std::optional<std::uint64_t> to_millis(wall_clock::time_point point)
{
    const auto count =
        std::chrono::time_point_cast<std::chrono::milliseconds>(point)
            .time_since_epoch()
            .count();
    if (count < 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(count);
}

std::string hex_text(const token_digest& digest)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(digest_text_size);
    for (const std::uint8_t byte : digest)
    {
        out.push_back(digits[static_cast<std::size_t>(byte >> 4)]);
        out.push_back(digits[static_cast<std::size_t>(byte & 0x0fu)]);
    }
    return out;
}

/// The JSON object of one record, or nothing when a time is before the epoch.
std::optional<json::object> record_object(const token_record& record)
{
    const std::optional<std::uint64_t> created = to_millis(record.created);
    const std::optional<std::uint64_t> expires = to_millis(record.expires);
    if (!created || !expires)
        return std::nullopt;
    json::value last_used(nullptr);
    if (record.last_used)
    {
        const std::optional<std::uint64_t> used = to_millis(*record.last_used);
        if (!used)
            return std::nullopt;
        last_used = json::value(*used);
    }

    json::array scopes;
    for (const std::string& name : scope_names(record.scopes))
        scopes.push_back(text_value(name));
    json::array transports;
    for (const transport via : record.transports)
        transports.push_back(text_value(transport_name(via)));

    json::object object;
    object["id"] = text_value(record.id);
    object["label"] = text_value(record.label);
    object["scopes"] = std::move(scopes);
    object["transports"] = std::move(transports);
    object["created"] = json::value(*created);
    object["expires"] = json::value(*expires);
    object["last_used"] = std::move(last_used);
    object["revoked"] = json::value(record.revoked);
    object["hash"] = text_value(hex_text(record.hash));
    return object;
}

result<std::string> render_document(const token_file_content& content)
{
    const std::optional<std::uint64_t> latest_seen =
        to_millis(content.latest_seen);
    if (!latest_seen)
        return make_error_code(errc::invalid_argument);

    json::array records;
    for (const token_record& record : content.tokens)
    {
        std::optional<json::object> object = record_object(record);
        if (!object)
            return make_error_code(errc::invalid_argument);
        records.push_back(std::move(*object));
    }

    json::object root;
    root["version"] = json::value(file_version);
    root["latest_seen"] = json::value(*latest_seen);
    root["tokens"] = std::move(records);
    return json::serialize(root);
}

} // namespace

result<token_file_content> load_token_file(const std::filesystem::path& file)
{
    const result<std::string> text =
        platform::read_private_file(file, max_file_size);
    if (!text)
    {
        if (text.error() == make_error_code(errc::not_found))
            return token_file_content{};
        // A file longer than any store can be is a file that is not one.
        if (text.error() == make_error_code(errc::invalid_argument))
            return bad_document();
        return text.error();
    }
    return parse_document(*text);
}

error_code save_token_file(const std::filesystem::path& file,
                           const token_file_content& content)
{
    const result<std::string> text = render_document(content);
    if (!text)
        return text.error();
    if (file.has_parent_path())
    {
        if (const error_code failed =
                platform::ensure_private_directory(file.parent_path()))
            return failed;
    }
    return platform::write_private_file(file, *text);
}

} // namespace vss::control
