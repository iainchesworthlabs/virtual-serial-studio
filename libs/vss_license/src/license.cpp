//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/license/license.hpp>

#include "strict_json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <tuple>
#include <utility>

namespace vss::license
{

namespace
{

namespace json = boost::json;
using std::chrono::days;

constexpr std::size_t max_id_length = 64;
constexpr std::size_t max_name_length = 200;
constexpr std::size_t max_feature_length = 64;
constexpr std::size_t max_features = 64;
constexpr std::size_t max_machine_length = 64;
constexpr std::size_t max_machines = 1024;
constexpr std::size_t max_revoked = 100'000;
constexpr std::size_t max_applied_resets = 32;

/// The payload fields this version understands. A name in `critical` that
/// is not here makes the licence one for a newer version.
constexpr std::array<std::string_view, 15> known_licence_fields{
    "licence_id", "kind",          "edition",    "licensee",    "issued_at",
    "starts_at",  "expires_at",    "grace_days", "seats",       "features",
    "machines",   "updates_until", "replaces",   "clock_reset", "critical"};

constexpr std::array<std::string_view, 5> known_revocation_fields{
    "sequence", "issued_at", "licences", "keys", "critical"};

bool is_upper_token(std::string_view text, std::size_t max_length)
{
    if (text.empty() || text.size() > max_length)
    {
        return false;
    }
    return std::ranges::all_of(text,
                               [](char c)
                               {
                                   return (c >= 'A' && c <= 'Z') ||
                                          (c >= '0' && c <= '9') || c == '-';
                               });
}

bool is_feature_entry(std::string_view text)
{
    if (text.ends_with(".*"))
    {
        text.remove_suffix(2);
    }
    return detail::is_token(text, max_feature_length, ".-") &&
           text.front() != '.' && text.back() != '.';
}

bool is_machine_entry(std::string_view text)
{
    if (text.ends_with('*'))
    {
        text.remove_suffix(1);
    }
    if (text.empty() || text.size() > max_machine_length)
    {
        return false;
    }
    return std::ranges::all_of(text,
                               [](char c)
                               {
                                   return (c >= 'a' && c <= 'z') ||
                                          (c >= 'A' && c <= 'Z') ||
                                          (c >= '0' && c <= '9') || c == '-' ||
                                          c == '_' || c == '.';
                               });
}

/// A name a person reads: no control characters.
bool is_display_text(std::string_view text)
{
    return std::ranges::none_of(text,
                                [](char c)
                                {
                                    const auto byte =
                                        static_cast<unsigned char>(c);
                                    return byte < 0x20 || byte == 0x7f;
                                });
}

char ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool digits(std::string_view text)
{
    return std::ranges::all_of(text,
                               [](char c) { return c >= '0' && c <= '9'; });
}

int number(std::string_view text)
{
    int value = 0;
    for (const char c : text)
    {
        value = value * 10 + (c - '0');
    }
    return value;
}

result<time_point> required_time(const json::object& object,
                                 std::string_view key)
{
    const auto text = detail::required_string(object, key, 20);
    if (!text)
    {
        return errc::malformed;
    }
    return parse_time(*text);
}

/// `critical` may only name fields this version knows.
result<void> check_critical(const json::object& object,
                            std::span<const std::string_view> known)
{
    const auto critical = detail::string_list(object, "critical", 64, 64);
    if (!critical)
    {
        return errc::malformed;
    }
    for (const std::string& name : *critical)
    {
        if (std::ranges::find(known, name) == known.end())
        {
            return errc::newer_version;
        }
    }
    return {};
}

result<json::object> payload_object(std::span<const std::uint8_t> payload)
{
    const std::string_view text(reinterpret_cast<const char*>(payload.data()),
                                payload.size());
    auto document = detail::parse_strict(text);
    if (!document || !document->is_object())
    {
        return errc::malformed;
    }
    // The object is taken out of the document, which is not a local of its
    // own type, so it is moved into one and that is returned.
    json::object object = std::move(document->get_object());
    return object;
}

std::span<const std::uint8_t> as_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

reason reason_for(const boost::system::error_code& ec)
{
    if (ec == errc::newer_version || ec == errc::unknown_key)
    {
        return reason::newer_version;
    }
    if (ec == errc::key_not_valid)
    {
        return reason::revoked;
    }
    if (ec == errc::bad_signature)
    {
        return reason::bad_signature;
    }
    return reason::malformed;
}

status invalid(reason why)
{
    status result;
    result.state = state::invalid;
    result.reason = why;
    return result;
}

bool revoked(const licence& value, const signed_file::trusted_key* signed_by,
             const revocation_list& list)
{
    if (std::ranges::find(list.licence_ids, value.licence_id) !=
        list.licence_ids.end())
    {
        return true;
    }
    if (signed_by == nullptr)
    {
        return false;
    }
    return std::ranges::any_of(list.keys,
                               [&](const revoked_key& key)
                               {
                                   return key.key_id == signed_by->key_id &&
                                          (!key.from ||
                                           value.issued_at >= *key.from);
                               });
}

int whole_days(time_point from, time_point until)
{
    return static_cast<int>(std::chrono::floor<days>(until - from).count());
}

int state_rank(state value)
{
    switch (value)
    {
    case state::valid:
    case state::expiring:
        return 6;
    case state::grace:
        return 5;
    case state::not_yet_valid:
        return 4;
    case state::clock_suspect:
        return 3;
    case state::expired:
        return 2;
    case state::invalid:
        return 1;
    case state::none:
        return 0;
    }
    return 0;
}

} // namespace

result<time_point> parse_time(std::string_view text)
{
    // 2026-11-13T02:10:00Z
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        text[19] != 'Z' || !digits(text.substr(0, 4)) ||
        !digits(text.substr(5, 2)) || !digits(text.substr(8, 2)) ||
        !digits(text.substr(11, 2)) || !digits(text.substr(14, 2)) ||
        !digits(text.substr(17, 2)))
    {
        return errc::malformed;
    }
    const int year = number(text.substr(0, 4));
    const int hour = number(text.substr(11, 2));
    const int minute = number(text.substr(14, 2));
    const int second = number(text.substr(17, 2));
    const std::chrono::year_month_day date{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(number(text.substr(5, 2)))},
        std::chrono::day{static_cast<unsigned>(number(text.substr(8, 2)))}};
    if (year < 2000 || !date.ok() || hour > 23 || minute > 59 || second > 59)
    {
        return errc::malformed;
    }
    return time_point{std::chrono::sys_days{date}} + std::chrono::hours{hour} +
           std::chrono::minutes{minute} + std::chrono::seconds{second};
}

std::string format_time(time_point value)
{
    const auto day = std::chrono::floor<days>(value);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{value - day};
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%04d-%02u-%02uT%02d:%02d:%02dZ",
                  static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()),
                  static_cast<unsigned>(date.day()),
                  static_cast<int>(time.hours().count()),
                  static_cast<int>(time.minutes().count()),
                  static_cast<int>(time.seconds().count()));
    return text.data();
}

result<licence> parse_payload(std::span<const std::uint8_t> payload)
{
    const auto object = payload_object(payload);
    if (!object)
    {
        return errc::malformed;
    }
    if (const auto critical = check_critical(*object, known_licence_fields);
        !critical)
    {
        return critical.error();
    }

    licence value;

    const auto id =
        detail::required_string(*object, "licence_id", max_id_length);
    const auto kind = detail::required_string(*object, "kind", 16);
    const auto edition_text = detail::required_string(*object, "edition", 16);
    if (!id || !kind || !edition_text || !is_upper_token(*id, max_id_length))
    {
        return errc::malformed;
    }
    value.licence_id = *id;

    if (*kind == "trial")
    {
        value.kind = licence_kind::trial;
    }
    else if (*kind == "annual")
    {
        value.kind = licence_kind::annual;
    }
    else if (*kind == "enterprise")
    {
        value.kind = licence_kind::enterprise;
    }
    else
    {
        return errc::malformed;
    }

    if (*edition_text == "pro")
    {
        value.edition = edition::pro;
    }
    else if (*edition_text == "enterprise")
    {
        value.edition = edition::enterprise;
    }
    else
    {
        return errc::malformed;
    }

    const auto licensee = object->find("licensee");
    if (licensee == object->end() || !licensee->value().is_object())
    {
        return errc::malformed;
    }
    const auto name = detail::required_string(licensee->value().get_object(),
                                              "name", max_name_length);
    const auto contact = detail::optional_string(licensee->value().get_object(),
                                                 "contact", max_name_length);
    if (!name || !contact || !is_display_text(*name) ||
        !is_display_text(*contact))
    {
        return errc::malformed;
    }
    value.licensee_name = *name;
    value.licensee_contact = *contact;

    const auto issued = required_time(*object, "issued_at");
    const auto starts = required_time(*object, "starts_at");
    const auto expires = required_time(*object, "expires_at");
    if (!issued || !starts || !expires || *expires < *starts)
    {
        return errc::malformed;
    }
    value.issued_at = *issued;
    value.starts_at = *starts;
    value.expires_at = *expires;

    const auto grace =
        detail::required_integer(*object, "grace_days", 0, max_grace_days);
    const auto seats = detail::required_integer(*object, "seats", 0, max_seats);
    if (!grace || !seats)
    {
        return errc::malformed;
    }
    value.grace_days = static_cast<int>(*grace);
    value.seats = static_cast<int>(*seats);

    auto features = detail::string_list(*object, "features", max_features,
                                        max_feature_length + 2);
    auto machines = detail::string_list(*object, "machines", max_machines,
                                        max_machine_length + 1);
    if (!features || !machines ||
        !std::ranges::all_of(*features, is_feature_entry) ||
        !std::ranges::all_of(*machines, is_machine_entry))
    {
        return errc::malformed;
    }
    value.features = std::move(*features);
    value.machines = std::move(*machines);

    const auto updates = detail::optional_string(*object, "updates_until", 20);
    if (!updates)
    {
        return errc::malformed;
    }
    if (!updates->empty())
    {
        const auto until = parse_time(*updates);
        if (!until)
        {
            return errc::malformed;
        }
        value.updates_until = *until;
    }

    const auto replaces =
        detail::optional_string(*object, "replaces", max_id_length);
    const auto reset =
        detail::optional_string(*object, "clock_reset", max_id_length);
    if (!replaces || !reset ||
        (!replaces->empty() && !is_upper_token(*replaces, max_id_length)) ||
        (!reset->empty() && !detail::is_token(*reset, max_id_length, "-")))
    {
        return errc::malformed;
    }
    value.replaces = *replaces;
    value.clock_reset = *reset;
    return value;
}

result<std::string> write_payload(const licence& value)
{
    json::object licensee;
    licensee["name"] = value.licensee_name;
    if (!value.licensee_contact.empty())
    {
        licensee["contact"] = value.licensee_contact;
    }

    json::object object;
    object["licence_id"] = value.licence_id;
    object["kind"] = value.kind == licence_kind::trial    ? "trial"
                     : value.kind == licence_kind::annual ? "annual"
                                                          : "enterprise";
    object["edition"] = value.edition == edition::pro ? "pro" : "enterprise";
    object["licensee"] = std::move(licensee);
    object["issued_at"] = format_time(value.issued_at);
    object["starts_at"] = format_time(value.starts_at);
    object["expires_at"] = format_time(value.expires_at);
    object["grace_days"] = value.grace_days;
    object["seats"] = value.seats;
    object["features"] =
        json::array(value.features.begin(), value.features.end());
    object["machines"] =
        json::array(value.machines.begin(), value.machines.end());
    if (value.updates_until)
    {
        object["updates_until"] = format_time(*value.updates_until);
    }
    if (!value.replaces.empty())
    {
        object["replaces"] = value.replaces;
    }
    if (!value.clock_reset.empty())
    {
        object["clock_reset"] = value.clock_reset;
    }
    object["critical"] = json::array();

    std::string text = json::serialize(object);
    const auto read_back = parse_payload(as_bytes(text));
    if (!read_back || *read_back != value)
    {
        return errc::malformed;
    }
    return text;
}

result<revocation_list> parse_revocations(std::span<const std::uint8_t> payload)
{
    const auto object = payload_object(payload);
    if (!object)
    {
        return errc::malformed;
    }
    if (const auto critical = check_critical(*object, known_revocation_fields);
        !critical)
    {
        return critical.error();
    }

    revocation_list list;
    const auto sequence = detail::required_integer(
        *object, "sequence", 0, std::numeric_limits<std::int64_t>::max());
    const auto issued = required_time(*object, "issued_at");
    auto ids =
        detail::string_list(*object, "licences", max_revoked, max_id_length);
    if (!sequence || !issued || !ids ||
        !std::ranges::all_of(*ids, [](const std::string& id)
                             { return is_upper_token(id, max_id_length); }))
    {
        return errc::malformed;
    }
    list.sequence = static_cast<std::uint64_t>(*sequence);
    list.issued_at = *issued;
    list.licence_ids = std::move(*ids);

    if (const auto keys = object->find("keys"); keys != object->end())
    {
        if (!keys->value().is_array() ||
            keys->value().get_array().size() > max_revoked)
        {
            return errc::malformed;
        }
        for (const json::value& entry : keys->value().get_array())
        {
            if (!entry.is_object())
            {
                return errc::malformed;
            }
            const auto key_id = detail::required_string(
                entry.get_object(), "key_id", max_id_length);
            const auto from =
                detail::optional_string(entry.get_object(), "from", 20);
            if (!key_id || !from ||
                !detail::is_token(*key_id, max_id_length, "-"))
            {
                return errc::malformed;
            }
            revoked_key key;
            key.key_id = *key_id;
            if (!from->empty())
            {
                const auto time = parse_time(*from);
                if (!time)
                {
                    return errc::malformed;
                }
                key.from = *time;
            }
            list.keys.push_back(std::move(key));
        }
    }
    return list;
}

result<std::string> write_revocations(const revocation_list& value)
{
    json::array keys;
    for (const revoked_key& key : value.keys)
    {
        json::object entry;
        entry["key_id"] = key.key_id;
        if (key.from)
        {
            entry["from"] = format_time(*key.from);
        }
        keys.push_back(std::move(entry));
    }

    json::object object;
    object["sequence"] = value.sequence;
    object["issued_at"] = format_time(value.issued_at);
    object["licences"] =
        json::array(value.licence_ids.begin(), value.licence_ids.end());
    object["keys"] = std::move(keys);
    object["critical"] = json::array();

    std::string text = json::serialize(object);
    const auto read_back = parse_revocations(as_bytes(text));
    if (!read_back || *read_back != value)
    {
        return errc::malformed;
    }
    return text;
}

result<revocation_list>
read_revocations(std::string_view file_text,
                 std::span<const signed_file::trusted_key> keys)
{
    const auto file = signed_file::parse(file_text);
    if (!file)
    {
        return file.error();
    }
    const auto key = signed_file::verify(*file, revocations_purpose, keys);
    if (!key)
    {
        return key.error();
    }
    auto list = parse_revocations(file->payload);
    if (!list)
    {
        return list.error();
    }
    if (const auto window =
            signed_file::check_key_window(**key, list->issued_at);
        !window)
    {
        return window.error();
    }
    return list;
}

revocation_list newer(const revocation_list& current,
                      const revocation_list& candidate)
{
    return candidate.sequence > current.sequence ? candidate : current;
}

bool status::active() const noexcept
{
    return state == license::state::valid ||
           state == license::state::expiring || state == license::state::grace;
}

status no_licence()
{
    return {};
}

status evaluate(std::string_view file_text, const context& where)
{
    const auto file = signed_file::parse(file_text);
    if (!file)
    {
        return invalid(reason_for(file.error()));
    }
    const auto key = signed_file::verify(*file, licence_purpose, where.keys);
    if (!key)
    {
        return invalid(reason_for(key.error()));
    }
    const auto value = parse_payload(file->payload);
    if (!value)
    {
        return invalid(reason_for(value.error()));
    }
    return evaluate(*value, *key, where);
}

status evaluate(const licence& value, const signed_file::trusted_key* signed_by,
                const context& where)
{
    status result;
    result.value = value;
    result.grace_ends = value.expires_at + days{value.grace_days};
    result.state = state::invalid;
    if (signed_by != nullptr)
    {
        result.key_id = signed_by->key_id;
        if (!signed_file::check_key_window(*signed_by, value.issued_at))
        {
            result.reason = reason::revoked;
            return result;
        }
    }
    if (where.revocations != nullptr &&
        revoked(value, signed_by, *where.revocations))
    {
        result.reason = reason::revoked;
        return result;
    }
    if (!value.machines.empty() &&
        std::ranges::none_of(
            value.machines, [&](const std::string& entry)
            { return machine_matches(entry, where.computer_name); }))
    {
        result.reason = reason::wrong_computer;
        return result;
    }

    // Times that have certainly passed. The clock may not read well before
    // the latest of them.
    const time_point passed =
        std::max({where.latest_seen, where.build_time, value.issued_at});
    if (where.now + clock_tolerance < passed)
    {
        result.state = state::clock_suspect;
        result.reason = reason::clock_suspect;
        return result;
    }
    if (where.now < value.starts_at)
    {
        result.state = state::not_yet_valid;
        result.reason = reason::not_yet_valid;
        return result;
    }
    if (where.now <= value.expires_at)
    {
        result.days_left = whole_days(where.now, value.expires_at);
        const int threshold = value.kind == licence_kind::trial
                                  ? expiring_days_trial
                                  : expiring_days;
        result.state =
            result.days_left <= threshold ? state::expiring : state::valid;
        result.reason = reason::ok;
        return result;
    }
    if (where.now <= result.grace_ends)
    {
        result.days_left = whole_days(where.now, result.grace_ends);
        result.state = state::grace;
        result.reason = reason::ok;
        return result;
    }
    result.state = state::expired;
    result.reason = reason::expired;
    return result;
}

status best(std::span<const status> candidates)
{
    const auto rank = [](const status& s)
    {
        return std::tuple{state_rank(s.state),
                          s.value && s.value->edition == edition::enterprise,
                          s.value ? s.value->expires_at : time_point::min()};
    };
    status chosen = no_licence();
    for (const status& candidate : candidates)
    {
        if (rank(candidate) > rank(chosen))
        {
            chosen = candidate;
        }
    }
    return chosen;
}

entitlement check(const status& current, std::string_view feature)
{
    entitlement answer;
    answer.reason = current.reason;
    answer.days_left = current.days_left;
    if (current.value)
    {
        answer.licence_id = current.value->licence_id;
    }
    if (!current.active() || !current.value)
    {
        return answer;
    }
    answer.granted = std::ranges::any_of(
        current.value->features, [feature](const std::string& entry)
        { return feature_matches(entry, feature); });
    answer.reason = answer.granted ? reason::ok : reason::not_in_licence;
    return answer;
}

bool feature_matches(std::string_view entry, std::string_view feature)
{
    if (entry.ends_with(".*"))
    {
        // "plugins.*" covers "plugins.x" and not "plugins" or "pluginsx".
        entry.remove_suffix(1);
        return feature.size() > entry.size() && feature.starts_with(entry);
    }
    return entry == feature;
}

bool machine_matches(std::string_view entry, std::string_view computer_name)
{
    const bool prefix = entry.ends_with('*');
    if (prefix)
    {
        entry.remove_suffix(1);
    }
    if (computer_name.size() < entry.size() ||
        (!prefix && computer_name.size() != entry.size()))
    {
        return false;
    }
    return std::ranges::equal(entry, computer_name.substr(0, entry.size()),
                              [](char a, char b)
                              { return ascii_lower(a) == ascii_lower(b); });
}

clock_state observe(clock_state stored, time_point now)
{
    stored.latest_seen = std::max(stored.latest_seen, now);
    return stored;
}

clock_state apply_reset(clock_state stored, const licence& imported)
{
    if (imported.clock_reset.empty() ||
        std::ranges::find(stored.applied_resets, imported.clock_reset) !=
            stored.applied_resets.end())
    {
        return stored;
    }
    stored.latest_seen = imported.issued_at;
    stored.applied_resets.push_back(imported.clock_reset);
    if (stored.applied_resets.size() > max_applied_resets)
    {
        stored.applied_resets.erase(stored.applied_resets.begin());
    }
    return stored;
}

} // namespace vss::license
