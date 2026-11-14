//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/trust_store.hpp>

#include <vss/plugin/policy.hpp>
#include <vss/plugin/revocations.hpp>

#include "reader.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <span>
#include <system_error>
#include <utility>

namespace vss::plugin
{

namespace
{

namespace json = boost::json;
namespace fs = std::filesystem;
using namespace std::string_view_literals;

constexpr std::array top_keys{"trust_version"sv, "keys"sv, "plugins"sv,
                              "ledger"sv};

constexpr std::array key_keys{"key_id"sv, "public_key"sv, "name"sv,
                              "approved_on"sv};

constexpr std::array decision_keys{"plugin_id"sv,
                                   "plugin_hash"sv,
                                   "key_id"sv,
                                   "level"sv,
                                   "enabled"sv,
                                   "granted"sv,
                                   "run_without_sandbox"sv,
                                   "decided_on"sv};

constexpr std::array ledger_keys{"revocation_serial"sv, "key_list_serial"sv,
                                 "latest_date"sv};

constexpr std::array all_levels{
    trust_level::official, trust_level::third_party_signed,
    trust_level::signed_unknown_key, trust_level::unsigned_plugin};

/// The earliest and the last year a date in the store may have.  The text
/// of a date has four digits and the date reader refuses earlier years, so
/// a date outside this range could be written and never read back.
constexpr int first_year = 2000;
constexpr int last_year = 9999;

std::string_view view_of(const json::string& text) noexcept
{
    return {text.data(), text.size()};
}

/// The parts one after the other, for a message to a person.
std::string join(std::initializer_list<std::string_view> parts)
{
    std::string out;
    for (const std::string_view part : parts)
        out.append(part);
    return out;
}

bool valid_date(const std::chrono::year_month_day& date) noexcept
{
    if (!date.ok())
        return false;
    const int year = static_cast<int>(date.year());
    return year >= first_year && year <= last_year;
}

/// Whether the JSON reader takes `text` back as a string.  It does not for
/// bytes that are not UTF-8, which `detail::clean_text` does not look at.
bool reads_back(std::string_view text)
{
    boost::system::error_code failed;
    json::parse(json::serialize(json::string(text)), failed);
    return !failed;
}

bool valid_name(std::string_view name)
{
    return !name.empty() && name.size() <= max_name_length &&
           detail::clean_text(name) && reads_back(name);
}

bool known_level(trust_level level) noexcept
{
    return std::ranges::find(all_levels, level) != all_levels.end();
}

bool known_permission(permission name) noexcept
{
    return parse_permission(to_string(name)) == name;
}

bool has_repeats(const std::vector<permission>& list)
{
    std::vector<permission> sorted = list;
    std::ranges::sort(sorted);
    return std::ranges::adjacent_find(sorted) != sorted.end();
}

bool same_decision(const plugin_decision& decision, std::string_view plugin_id,
                   const plugin_hash& hash, std::string_view key_id) noexcept
{
    return decision.plugin_id == plugin_id && decision.hash == hash &&
           decision.key_id == key_id;
}

std::optional<std::uint8_t> hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<std::uint8_t>(c - 'a' + 10);
    return std::nullopt;
}

/// Fills `out` from twice as many lower-case hex characters.  Upper case is
/// refused, so one value has one spelling.
bool bytes_from_hex(std::string_view text, std::span<std::uint8_t> out) noexcept
{
    if (text.size() != out.size() * 2)
        return false;
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const auto high = hex_value(text[i * 2]);
        const auto low = hex_value(text[i * 2 + 1]);
        if (!high || !low)
            return false;
        out[i] = static_cast<std::uint8_t>((*high << 4) | *low);
    }
    return true;
}

std::string hex_of(std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes)
    {
        out += digits[static_cast<std::size_t>(byte >> 4)];
        out += digits[static_cast<std::size_t>(byte & 0x0f)];
    }
    return out;
}

void append_digits(std::string& out, unsigned value, std::size_t width)
{
    const std::string digits = std::to_string(value);
    if (digits.size() < width)
        out.append(width - digits.size(), '0');
    out += digits;
}

/// `YYYY-MM-DD` of a date that `valid_date` accepts.
std::string date_text(const std::chrono::year_month_day& date)
{
    std::string out;
    append_digits(out, static_cast<unsigned>(static_cast<int>(date.year())), 4);
    out += '-';
    append_digits(out, static_cast<unsigned>(date.month()), 2);
    out += '-';
    append_digits(out, static_cast<unsigned>(date.day()), 2);
    return out;
}

std::optional<trust_level> level_from_text(std::string_view text) noexcept
{
    for (const trust_level level : all_levels)
    {
        if (to_string(level) == text)
            return level;
    }
    return std::nullopt;
}

std::string too_large_text()
{
    return join({"trust store: is larger than ",
                 std::to_string(max_trust_store_size), " bytes"});
}

// -- reading ----------------------------------------------------------------

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

/// `where.name`, or `name` at the top.
std::string field(std::string_view where, std::string_view name)
{
    std::string out;
    if (!where.empty())
    {
        out.assign(where);
        out += '.';
    }
    out.append(name);
    return out;
}

/// `list[index]`.
std::string element(std::string_view list, std::size_t index)
{
    std::string out(list);
    out += '[';
    out += std::to_string(index);
    out += ']';
    return out;
}

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
            state.fail(where, join({"unknown key ", detail::quoted(key)}));
            return nullptr;
        }
    }
    return &object;
}

/// The member, or an error when it is missing.
const json::value* required(reading& state, const json::object& object,
                            std::string_view where, std::string_view key)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr)
        state.fail(field(where, key), "is required");
    return value;
}

/// The member as a string; an error when it is missing or something else.
std::optional<std::string_view> text_of(reading& state,
                                        const json::object& object,
                                        std::string_view where,
                                        std::string_view key)
{
    const json::value* const value = required(state, object, where, key);
    if (value == nullptr)
        return std::nullopt;
    if (!value->is_string())
    {
        state.fail(field(where, key), "must be a string");
        return std::nullopt;
    }
    return view_of(value->get_string());
}

std::optional<bool> flag_of(reading& state, const json::object& object,
                            std::string_view where, std::string_view key)
{
    const json::value* const value = required(state, object, where, key);
    if (value == nullptr)
        return std::nullopt;
    if (!value->is_bool())
    {
        state.fail(field(where, key), "must be true or false");
        return std::nullopt;
    }
    return value->get_bool();
}

std::optional<std::uint64_t> serial_of(reading& state,
                                       const json::object& object,
                                       std::string_view where,
                                       std::string_view key)
{
    const json::value* const value = required(state, object, where, key);
    if (value == nullptr)
        return std::nullopt;
    if (!value->is_int64() || value->get_int64() < 0 ||
        static_cast<std::uint64_t>(value->get_int64()) > max_revocations_serial)
    {
        state.fail(field(where, key),
                   join({"must be a whole number from 0 to ",
                         std::to_string(max_revocations_serial)}));
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value->get_int64());
}

std::optional<std::chrono::year_month_day> date_of(reading& state,
                                                   const json::object& object,
                                                   std::string_view where,
                                                   std::string_view key)
{
    const auto text = text_of(state, object, where, key);
    if (!text)
        return std::nullopt;
    const auto date = detail::date_from_text(*text);
    if (!date)
        state.fail(field(where, key), "must be a date such as 2026-11-14");
    return date;
}

/// The member as a list of at most `limit` entries; an empty list when it
/// is missing.
std::span<const json::value> list_of(reading& state, const json::object& object,
                                     std::string_view key, std::size_t limit)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr)
        return {};
    if (!value->is_array())
    {
        state.fail(key, "must be a list");
        return {};
    }
    const json::array& list = value->get_array();
    if (list.size() > limit)
    {
        state.fail(key,
                   join({"has more than ", std::to_string(limit), " entries"}));
        return {};
    }
    return {list.data(), list.size()};
}

void read_keys(reading& state, const json::object& root, trust_store& out)
{
    const auto list = list_of(state, root, "keys", max_approved_keys);
    for (std::size_t i = 0; i < list.size() && !state.failed(); ++i)
    {
        const std::string where = element("keys", i);
        const json::object* const entry =
            object_of(state, list[i], where, key_keys);
        if (entry == nullptr)
            return;
        const auto id = text_of(state, *entry, where, "key_id");
        const auto public_text = text_of(state, *entry, where, "public_key");
        const auto name = text_of(state, *entry, where, "name");
        const auto approved = date_of(state, *entry, where, "approved_on");
        if (!id || !public_text || !name || !approved)
            return;

        if (!is_key_id(*id))
        {
            state.fail(field(where, "key_id"),
                       join({"cannot be ", detail::quoted(*id)}));
            return;
        }
        approved_key key;
        key.key_id = std::string(*id);
        if (!bytes_from_hex(*public_text, key.public_key))
        {
            state.fail(field(where, "public_key"),
                       "must be 64 lower-case hex characters");
            return;
        }
        if (!valid_name(*name))
        {
            state.fail(field(where, "name"),
                       join({"must be 1 to ", std::to_string(max_name_length),
                             " bytes of plain text"}));
            return;
        }
        key.name = std::string(*name);
        key.approved_on = *approved;

        const std::size_t before = out.keys().size();
        const result<void> added = out.approve_key(std::move(key));
        if (added.has_error())
        {
            state.fail(where, added.error() == errc::already_exists
                                  ? join({"key_id ", detail::quoted(*id),
                                          " has another public key"})
                                  : std::string("is not accepted"));
            return;
        }
        if (out.keys().size() != before + 1)
        {
            state.fail(where, join({"key_id ", detail::quoted(*id),
                                    " is given twice"}));
            return;
        }
    }
}

std::optional<std::vector<permission>>
granted_of(reading& state, const json::object& object, std::string_view where)
{
    const json::value* const value = required(state, object, where, "granted");
    if (value == nullptr)
        return std::nullopt;
    const std::string name = field(where, "granted");
    if (!value->is_array())
    {
        state.fail(name, "must be a list");
        return std::nullopt;
    }
    const json::array& list = value->get_array();
    if (list.size() > max_permissions)
    {
        state.fail(name, join({"has more than ",
                               std::to_string(max_permissions), " entries"}));
        return std::nullopt;
    }
    std::vector<permission> granted;
    for (const json::value& item : list)
    {
        if (!item.is_string())
        {
            state.fail(name, "must hold only strings");
            return std::nullopt;
        }
        const std::string_view text = view_of(item.get_string());
        const auto parsed = parse_permission(text);
        if (!parsed)
        {
            state.fail(name,
                       join({"unknown permission ", detail::quoted(text)}));
            return std::nullopt;
        }
        if (std::ranges::find(granted, *parsed) != granted.end())
        {
            state.fail(name, join({"holds ", detail::quoted(text), " twice"}));
            return std::nullopt;
        }
        granted.push_back(*parsed);
    }
    return granted;
}

void read_decisions(reading& state, const json::object& root, trust_store& out)
{
    const auto list = list_of(state, root, "plugins", max_decisions);
    for (std::size_t i = 0; i < list.size() && !state.failed(); ++i)
    {
        const std::string where = element("plugins", i);
        const json::object* const entry =
            object_of(state, list[i], where, decision_keys);
        if (entry == nullptr)
            return;
        const auto id = text_of(state, *entry, where, "plugin_id");
        const auto hash_text = text_of(state, *entry, where, "plugin_hash");
        const auto level_text = text_of(state, *entry, where, "level");
        const auto enabled = flag_of(state, *entry, where, "enabled");
        const auto granted = granted_of(state, *entry, where);
        const auto sandbox =
            flag_of(state, *entry, where, "run_without_sandbox");
        const auto decided = date_of(state, *entry, where, "decided_on");
        if (!id || !hash_text || !level_text || !enabled || !granted ||
            !sandbox || !decided)
            return;

        plugin_decision decision;
        if (!detail::valid_plugin_id(*id))
        {
            state.fail(field(where, "plugin_id"),
                       join({"cannot be ", detail::quoted(*id)}));
            return;
        }
        decision.plugin_id = std::string(*id);
        if (!bytes_from_hex(*hash_text, decision.hash.bytes))
        {
            state.fail(field(where, "plugin_hash"),
                       "must be 128 lower-case hex characters");
            return;
        }
        // A signing key is optional: an unsigned plugin has none.
        if (const json::value* const key = entry->if_contains("key_id"))
        {
            if (!key->is_string())
            {
                state.fail(field(where, "key_id"), "must be a string");
                return;
            }
            const std::string_view key_text = view_of(key->get_string());
            if (!is_key_id(key_text))
            {
                state.fail(field(where, "key_id"),
                           join({"cannot be ", detail::quoted(key_text)}));
                return;
            }
            decision.key_id = std::string(key_text);
        }
        const auto level = level_from_text(*level_text);
        if (!level)
        {
            state.fail(field(where, "level"),
                       join({"unknown value ", detail::quoted(*level_text)}));
            return;
        }
        if ((*level == trust_level::unsigned_plugin) != decision.key_id.empty())
        {
            state.fail(field(where, "level"),
                       "does not fit the key_id: unsigned exactly when "
                       "there is no key_id");
            return;
        }
        decision.level = *level;
        decision.enabled = *enabled;
        decision.granted = *granted;
        decision.run_without_sandbox = *sandbox;
        decision.decided_on = *decided;

        // `decide` replaces a decision it already has and drops the oldest
        // of too many.  A file must not rely on either, so both are errors
        // here.
        if (out.find(decision.plugin_id, decision.hash, decision.key_id) !=
            nullptr)
        {
            state.fail(where, "a decision for this plugin_id, plugin_hash and "
                              "key_id is given twice");
            return;
        }
        const auto same_id = std::ranges::count(
            out.decisions(), decision.plugin_id, &plugin_decision::plugin_id);
        if (static_cast<std::size_t>(same_id) >= max_decisions_per_plugin)
        {
            state.fail(
                where,
                join({"more than ", std::to_string(max_decisions_per_plugin),
                      " decisions for the plugin_id ", detail::quoted(*id)}));
            return;
        }

        const std::size_t before = out.decisions().size();
        const result<void> added = out.decide(std::move(decision));
        if (added.has_error())
        {
            state.fail(where, "is not accepted");
            return;
        }
        if (out.decisions().size() != before + 1)
        {
            state.fail(where, "was not added to the end of the list");
            return;
        }
    }
}

trust_ledger read_ledger(reading& state, const json::object& root)
{
    trust_ledger ledger;
    const json::value* const value = root.if_contains("ledger");
    if (value == nullptr)
        return ledger;
    const json::object* const object =
        object_of(state, *value, "ledger", ledger_keys);
    if (object == nullptr)
        return ledger;
    if (const auto serial =
            serial_of(state, *object, "ledger", "revocation_serial"))
        ledger.revocation_serial = *serial;
    if (const auto serial =
            serial_of(state, *object, "ledger", "key_list_serial"))
        ledger.key_list_serial = *serial;
    if (object->contains("latest_date"))
        ledger.latest_date = date_of(state, *object, "ledger", "latest_date");
    return ledger;
}

// -- writing ----------------------------------------------------------------

void indent(std::string& out, std::size_t level)
{
    out.append(level * 2, ' ');
}

/// The end of a line that holds one member or one list entry.
void end_line(std::string& out, bool last)
{
    if (!last)
        out += ',';
    out += '\n';
}

/// A string with the quotes and the escapes JSON needs.
void append_string(std::string& out, std::string_view text)
{
    out += json::serialize(json::string(text));
}

/// `"name": ` and nothing after it.
void member(std::string& out, std::size_t level, std::string_view name)
{
    indent(out, level);
    append_string(out, name);
    out += ": ";
}

void text_member(std::string& out, std::size_t level, std::string_view name,
                 std::string_view value, bool last)
{
    member(out, level, name);
    append_string(out, value);
    end_line(out, last);
}

void number_member(std::string& out, std::size_t level, std::string_view name,
                   std::uint64_t value, bool last)
{
    member(out, level, name);
    out += std::to_string(value);
    end_line(out, last);
}

void flag_member(std::string& out, std::size_t level, std::string_view name,
                 bool value, bool last)
{
    member(out, level, name);
    out += value ? "true" : "false";
    end_line(out, last);
}

void write_key(std::string& out, const approved_key& key, bool last)
{
    indent(out, 2);
    out += "{\n";
    text_member(out, 3, "key_id", key.key_id, false);
    text_member(out, 3, "public_key", hex_of(key.public_key), false);
    text_member(out, 3, "name", key.name, false);
    text_member(out, 3, "approved_on", date_text(key.approved_on), true);
    indent(out, 2);
    out += '}';
    end_line(out, last);
}

void write_decision(std::string& out, const plugin_decision& decision,
                    bool last)
{
    indent(out, 2);
    out += "{\n";
    text_member(out, 3, "plugin_id", decision.plugin_id, false);
    text_member(out, 3, "plugin_hash", hex_of(decision.hash.bytes), false);
    if (!decision.key_id.empty())
        text_member(out, 3, "key_id", decision.key_id, false);
    text_member(out, 3, "level", to_string(decision.level), false);
    flag_member(out, 3, "enabled", decision.enabled, false);
    member(out, 3, "granted");
    if (decision.granted.empty())
    {
        out += "[]";
    }
    else
    {
        out += "[\n";
        for (std::size_t i = 0; i < decision.granted.size(); ++i)
        {
            indent(out, 4);
            append_string(out, to_string(decision.granted[i]));
            end_line(out, i + 1 == decision.granted.size());
        }
        indent(out, 3);
        out += ']';
    }
    end_line(out, false);
    flag_member(out, 3, "run_without_sandbox", decision.run_without_sandbox,
                false);
    text_member(out, 3, "decided_on", date_text(decision.decided_on), true);
    indent(out, 2);
    out += '}';
    end_line(out, last);
}

/// Writes the text to a new file.  False when any part of it failed.
bool write_file(const fs::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    file.close();
    return !file.fail();
}

} // namespace

// -- the store --------------------------------------------------------------

const approved_key*
trust_store::find_key(std::string_view key_id) const noexcept
{
    const auto found = std::ranges::find(keys_, key_id, &approved_key::key_id);
    return found == keys_.end() ? nullptr : &*found;
}

const plugin_decision* trust_store::find(std::string_view plugin_id,
                                         const plugin_hash& hash,
                                         std::string_view key_id) const noexcept
{
    const auto found =
        std::ranges::find_if(decisions_, [&](const plugin_decision& decision) {
            return same_decision(decision, plugin_id, hash, key_id);
        });
    return found == decisions_.end() ? nullptr : &*found;
}

const plugin_decision*
trust_store::previous(std::string_view plugin_id, const plugin_hash& hash,
                      std::string_view key_id) const noexcept
{
    // The newest is the last one, so look from the end.
    for (auto it = decisions_.rbegin(); it != decisions_.rend(); ++it)
    {
        if (it->plugin_id == plugin_id &&
            !same_decision(*it, plugin_id, hash, key_id))
            return &*it;
    }
    return nullptr;
}

result<void> trust_store::approve_key(approved_key key)
{
    if (!is_key_id(key.key_id) || !valid_name(key.name) ||
        !valid_date(key.approved_on))
        return make_error_code(errc::invalid_argument);

    const auto existing =
        std::ranges::find(keys_, key.key_id, &approved_key::key_id);
    if (existing != keys_.end())
    {
        if (existing->public_key != key.public_key)
            return make_error_code(errc::already_exists);
        existing->name = std::move(key.name);
        existing->approved_on = key.approved_on;
        return {};
    }
    if (keys_.size() >= max_approved_keys)
        return make_error_code(errc::queue_full);
    keys_.push_back(std::move(key));
    return {};
}

bool trust_store::forget_key(std::string_view key_id)
{
    const auto existing =
        std::ranges::find(keys_, key_id, &approved_key::key_id);
    if (existing == keys_.end())
        return false;
    keys_.erase(existing);
    std::erase_if(decisions_, [&](const plugin_decision& decision) {
        return decision.key_id == key_id;
    });
    return true;
}

result<void> trust_store::decide(plugin_decision decision)
{
    const bool signed_plugin = !decision.key_id.empty();
    if (!detail::valid_plugin_id(decision.plugin_id) ||
        (signed_plugin && !is_key_id(decision.key_id)) ||
        !known_level(decision.level) ||
        (decision.level == trust_level::unsigned_plugin) == signed_plugin ||
        has_repeats(decision.granted) ||
        !std::ranges::all_of(decision.granted, known_permission) ||
        !valid_date(decision.decided_on))
        return make_error_code(errc::invalid_argument);

    std::erase_if(decisions_, [&](const plugin_decision& old) {
        return same_decision(old, decision.plugin_id, decision.hash,
                             decision.key_id);
    });
    const std::string plugin_id = decision.plugin_id;
    decisions_.push_back(std::move(decision));

    // Too many for one plugin id: the oldest of that id go.
    auto count = static_cast<std::size_t>(
        std::ranges::count(decisions_, plugin_id, &plugin_decision::plugin_id));
    for (auto it = decisions_.begin();
         count > max_decisions_per_plugin && it != decisions_.end();)
    {
        if (it->plugin_id == plugin_id)
        {
            it = decisions_.erase(it);
            --count;
        }
        else
        {
            ++it;
        }
    }
    // Too many in all: the oldest of all go.
    if (decisions_.size() > max_decisions)
    {
        const auto extra = decisions_.size() - max_decisions;
        decisions_.erase(decisions_.begin(),
                         decisions_.begin() +
                             static_cast<std::ptrdiff_t>(extra));
    }
    return {};
}

bool trust_store::forget(std::string_view plugin_id, const plugin_hash& hash,
                         std::string_view key_id)
{
    return std::erase_if(decisions_, [&](const plugin_decision& decision) {
               return same_decision(decision, plugin_id, hash, key_id);
           }) != 0;
}

bool trust_store::note_revocation_serial(std::uint64_t serial) noexcept
{
    // A serial above the largest is refused too: the file could not be read
    // back with it in.
    if (serial <= ledger_.revocation_serial || serial > max_revocations_serial)
        return false;
    ledger_.revocation_serial = serial;
    return true;
}

bool trust_store::note_key_list_serial(std::uint64_t serial) noexcept
{
    if (serial <= ledger_.key_list_serial || serial > max_revocations_serial)
        return false;
    ledger_.key_list_serial = serial;
    return true;
}

std::chrono::sys_days
trust_store::note_date(std::chrono::sys_days seen) noexcept
{
    if (ledger_.latest_date)
    {
        const std::chrono::sys_days known{*ledger_.latest_date};
        if (seen <= known)
            return known;
    }
    // A date the file could not hold (a clock that was never set, say) is
    // returned but not kept.
    const std::chrono::year_month_day day{seen};
    if (valid_date(day))
        ledger_.latest_date = day;
    return seen;
}

// -- the file ---------------------------------------------------------------

result<trust_store> parse_trust_store(std::string_view text,
                                      std::string* explain)
{
    reading state;
    const auto failure = [&]() -> result<trust_store> {
        if (explain != nullptr)
            *explain = state.problem;
        return make_error_code(errc::config_error);
    };
    if (text.size() > max_trust_store_size)
    {
        state.fail("trust store",
                   join({"is larger than ",
                         std::to_string(max_trust_store_size), " bytes"}));
        return failure();
    }
    std::string what;
    const std::optional<json::value> root =
        detail::parse_strict(text, max_trust_store_depth, what);
    if (!root)
    {
        state.fail("trust store", what);
        return failure();
    }
    const json::object* const object =
        object_of(state, *root, "trust store", top_keys);
    if (object == nullptr)
        return failure();

    // The version is needed, so that a file meant for a later reader is
    // never half understood by this one.
    const json::value* const version = object->if_contains("trust_version");
    if (version == nullptr || !version->is_int64() ||
        version->get_int64() != std::int64_t{trust_store_format_version})
    {
        state.fail(
            "trust_version",
            join({"must be ", std::to_string(trust_store_format_version)}));
        return failure();
    }

    trust_store out;
    read_keys(state, *object, out);
    if (state.failed())
        return failure();
    read_decisions(state, *object, out);
    if (state.failed())
        return failure();
    out.ledger_ = read_ledger(state, *object);
    if (state.failed())
        return failure();

    if (explain != nullptr)
        explain->clear();
    return out;
}

std::string to_json(const trust_store& store)
{
    std::string out = "{\n";
    number_member(out, 1, "trust_version", trust_store_format_version, false);

    member(out, 1, "keys");
    if (store.keys().empty())
    {
        out += "[]";
    }
    else
    {
        out += "[\n";
        for (std::size_t i = 0; i < store.keys().size(); ++i)
            write_key(out, store.keys()[i], i + 1 == store.keys().size());
        indent(out, 1);
        out += ']';
    }
    end_line(out, false);

    member(out, 1, "plugins");
    if (store.decisions().empty())
    {
        out += "[]";
    }
    else
    {
        out += "[\n";
        for (std::size_t i = 0; i < store.decisions().size(); ++i)
            write_decision(out, store.decisions()[i],
                           i + 1 == store.decisions().size());
        indent(out, 1);
        out += ']';
    }
    end_line(out, false);

    const trust_ledger& ledger = store.ledger();
    member(out, 1, "ledger");
    out += "{\n";
    number_member(out, 2, "revocation_serial", ledger.revocation_serial, false);
    number_member(out, 2, "key_list_serial", ledger.key_list_serial,
                  !ledger.latest_date.has_value());
    if (ledger.latest_date)
        text_member(out, 2, "latest_date", date_text(*ledger.latest_date),
                    true);
    indent(out, 1);
    out += "}\n";
    out += "}\n";
    return out;
}

result<trust_store> load_trust_store(const fs::path& path, std::string* explain)
{
    const auto unreadable = [&](std::string_view why) -> result<trust_store> {
        if (explain != nullptr)
            *explain = join({"trust store: ", why});
        return make_error_code(errc::io_error);
    };

    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (status.type() == fs::file_type::not_found)
    {
        // Nobody has decided anything yet.
        if (explain != nullptr)
            explain->clear();
        return trust_store{};
    }
    if (ec || status.type() != fs::file_type::regular)
        return unreadable("cannot be read");

    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec)
        return unreadable("cannot be read");
    if (size > max_trust_store_size)
    {
        if (explain != nullptr)
            *explain = too_large_text();
        return make_error_code(errc::config_error);
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return unreadable("cannot be opened");
    std::string text(static_cast<std::size_t>(size), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (file.fail() || static_cast<std::size_t>(file.gcount()) != text.size())
        return unreadable("cannot be read");
    return parse_trust_store(text, explain);
}

result<void> save_trust_store(const fs::path& path, const trust_store& store)
{
    fs::path temp = path;
    temp += ".tmp";
    const std::string text = to_json(store);

    std::error_code ec;
    if (write_file(temp, text))
    {
        fs::rename(temp, path, ec);
        if (!ec)
            return {};
    }
    // The old file stays as it was; the new one is not left behind.
    std::error_code ignored;
    fs::remove(temp, ignored);
    return make_error_code(errc::io_error);
}

} // namespace vss::plugin
