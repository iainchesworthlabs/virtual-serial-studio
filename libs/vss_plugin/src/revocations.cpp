//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/revocations.hpp>

#include <vss/plugin/policy.hpp>

#include "reader.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace vss::plugin
{

namespace
{

namespace json = boost::json;
using namespace std::string_view_literals;

constexpr std::array top_keys{"revocations_version"sv, "serial"sv,
                              "issued_at"sv,           "key_ids"sv,
                              "plugin_hashes"sv,       "plugins"sv};

constexpr std::array plugin_keys{"id"sv, "max_version"sv};

std::string_view view_of(const json::string& text) noexcept
{
    return {text.data(), text.size()};
}

std::optional<std::uint8_t> hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<std::uint8_t>(c - 'a' + 10);
    return std::nullopt;
}

/// 128 lower-case hex characters.  Upper case is refused, so one hash has
/// one spelling.
std::optional<plugin_hash> hash_from_text(std::string_view text) noexcept
{
    plugin_hash out;
    if (text.size() != out.bytes.size() * 2)
        return std::nullopt;
    for (std::size_t i = 0; i < out.bytes.size(); ++i)
    {
        const auto high = hex_value(text[i * 2]);
        const auto low = hex_value(text[i * 2 + 1]);
        if (!high || !low)
            return std::nullopt;
        out.bytes[i] = static_cast<std::uint8_t>((*high << 4) | *low);
    }
    return out;
}

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
            state.fail(where, "unknown key " + detail::quoted(key));
            return nullptr;
        }
    }
    return &object;
}

/// The member as a string; an error when it is missing or something else.
std::optional<std::string_view> text_of(reading& state,
                                        const json::object& object,
                                        std::string_view key,
                                        std::string_view where)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr)
    {
        state.fail(where, "is required");
        return std::nullopt;
    }
    if (!value->is_string())
    {
        state.fail(where, "must be a string");
        return std::nullopt;
    }
    return view_of(value->get_string());
}

/// The member as a list of at most `max_revoked_entries`; an empty list
/// when it is missing.
std::span<const json::value> list_of(reading& state,
                                     const json::object& object,
                                     std::string_view key)
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
    if (list.size() > max_revoked_entries)
    {
        state.fail(key, "has more than " +
                            std::to_string(max_revoked_entries) + " entries");
        return {};
    }
    return {list.data(), list.size()};
}

} // namespace

bool revocation_list::revokes_key(std::string_view key_id) const noexcept
{
    return std::ranges::find(key_ids, key_id) != key_ids.end();
}

bool revocation_list::revokes_hash(const plugin_hash& hash) const noexcept
{
    return std::ranges::find(plugin_hashes, hash) != plugin_hashes.end();
}

bool revocation_list::revokes_plugin(
    std::string_view id, const plugin_version& version) const noexcept
{
    return std::ranges::any_of(plugins, [&](const revoked_plugin& entry) {
        return entry.id == id && compare(version, entry.max_version) <= 0;
    });
}

result<revocation_list> parse_revocations(std::string_view payload,
                                          std::string* explain)
{
    reading state;
    const auto failure = [&]() -> result<revocation_list> {
        if (explain != nullptr)
            *explain = state.problem;
        return make_error_code(errc::config_error);
    };
    if (payload.size() > max_revocations_size)
    {
        state.fail("revocations",
                   "is larger than " + std::to_string(max_revocations_size) +
                       " bytes");
        return failure();
    }
    std::string what;
    const std::optional<json::value> root =
        detail::parse_strict(payload, max_revocations_depth, what);
    if (!root)
    {
        state.fail("revocations", what);
        return failure();
    }
    const json::object* const object =
        object_of(state, *root, "revocations", top_keys);
    if (object == nullptr)
        return failure();

    const json::value* const version =
        object->if_contains("revocations_version");
    if (version == nullptr || !version->is_int64() ||
        version->get_int64() != std::int64_t{revocations_format_version})
    {
        state.fail("revocations_version",
                   "must be " + std::to_string(revocations_format_version));
        return failure();
    }

    revocation_list out;
    const json::value* const serial = object->if_contains("serial");
    if (serial == nullptr || !serial->is_int64() || serial->get_int64() < 1 ||
        static_cast<std::uint64_t>(serial->get_int64()) >
            max_revocations_serial)
    {
        state.fail("serial", "must be a whole number from 1 to " +
                                 std::to_string(max_revocations_serial));
        return failure();
    }
    out.serial = static_cast<std::uint64_t>(serial->get_int64());

    if (const auto text = text_of(state, *object, "issued_at", "issued_at"))
    {
        if (const auto date = detail::date_from_text(*text))
            out.issued_at = *date;
        else
            state.fail("issued_at", "must be a date such as 2026-11-13");
    }

    for (const json::value& item : list_of(state, *object, "key_ids"))
    {
        if (!item.is_string() || !is_key_id(view_of(item.get_string())))
        {
            state.fail("key_ids", "holds something that is not a key id");
            break;
        }
        out.key_ids.emplace_back(view_of(item.get_string()));
    }
    for (const json::value& item : list_of(state, *object, "plugin_hashes"))
    {
        const auto hash = item.is_string()
                              ? hash_from_text(view_of(item.get_string()))
                              : std::nullopt;
        if (!hash)
        {
            state.fail("plugin_hashes",
                       "holds something that is not 128 lower-case hex "
                       "characters");
            break;
        }
        out.plugin_hashes.push_back(*hash);
    }
    for (const json::value& item : list_of(state, *object, "plugins"))
    {
        const json::object* const entry =
            object_of(state, item, "plugins", plugin_keys);
        if (entry == nullptr)
            break;
        const auto id = text_of(state, *entry, "id", "plugins.id");
        const auto max_version =
            text_of(state, *entry, "max_version", "plugins.max_version");
        if (!id || !max_version)
            break;
        if (!detail::valid_plugin_id(*id))
        {
            state.fail("plugins.id",
                       "cannot be " + detail::quoted(*id));
            break;
        }
        const auto parsed = detail::version_from_text(*max_version);
        if (!parsed)
        {
            state.fail("plugins.max_version",
                       "cannot be " + detail::quoted(*max_version));
            break;
        }
        out.plugins.push_back(revoked_plugin{std::string(*id), *parsed});
    }

    if (state.failed())
        return failure();
    if (explain != nullptr)
        explain->clear();
    return out;
}

} // namespace vss::plugin
