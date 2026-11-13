//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/policy.hpp>

#include "reader.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace vss::plugin
{

namespace
{

namespace json = boost::json;
using namespace std::string_view_literals;

constexpr std::array allow_names{
    std::pair{"official_only"sv, allow_level::official_only},
    std::pair{"signed"sv, allow_level::signed_only},
    std::pair{"any"sv, allow_level::any}};

constexpr std::array sandbox_names{
    std::pair{"none"sv, sandbox_level::none},
    std::pair{"partial"sv, sandbox_level::partial},
    std::pair{"full"sv, sandbox_level::full}};

constexpr std::array known_keys{
    "policy_version"sv, "allow"sv,           "allow_keys"sv,
    "deny_plugins"sv,   "min_sandbox"sv,     "allow_no_sandbox"sv,
    "user_plugins"sv,   "on_timeout"sv,      "permissions_denied"sv};

std::string_view view_of(const json::string& text) noexcept
{
    return {text.data(), text.size()};
}

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

std::optional<std::string_view> word(reading& state, const json::value& value,
                                     std::string_view where)
{
    if (!value.is_string())
    {
        state.fail(where, "must be a string");
        return std::nullopt;
    }
    return view_of(value.get_string());
}

bool flag(reading& state, const json::value& value, std::string_view where,
          bool fallback)
{
    if (!value.is_bool())
    {
        state.fail(where, "must be true or false");
        return fallback;
    }
    return value.get_bool();
}

/// A list of strings with no entry twice, each accepted by `valid`.
template <class Valid>
std::vector<std::string> names(reading& state, const json::value& value,
                               std::string_view where, std::size_t limit,
                               Valid valid)
{
    std::vector<std::string> out;
    if (!value.is_array())
    {
        state.fail(where, "must be a list");
        return out;
    }
    const json::array& list = value.get_array();
    if (list.size() > limit)
    {
        state.fail(where, "has more than " + std::to_string(limit) +
                              " entries");
        return out;
    }
    for (const json::value& item : list)
    {
        const auto text = word(state, item, where);
        if (!text)
            return out;
        if (!valid(*text))
        {
            state.fail(where, "cannot hold " + detail::quoted(*text));
            return out;
        }
        if (std::ranges::find(out, *text) != out.end())
        {
            state.fail(where, "holds " + detail::quoted(*text) + " twice");
            return out;
        }
        out.emplace_back(*text);
    }
    return out;
}

} // namespace

std::string_view to_string(allow_level value) noexcept
{
    for (const auto& [name, level] : allow_names)
    {
        if (level == value)
            return name;
    }
    return {};
}

std::string_view to_string(sandbox_level value) noexcept
{
    for (const auto& [name, level] : sandbox_names)
    {
        if (level == value)
            return name;
    }
    return {};
}

bool is_key_id(std::string_view text) noexcept
{
    if (text.empty() || text.size() > max_key_id_length)
        return false;
    return std::ranges::all_of(text, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    });
}

policy official_only_policy()
{
    policy out;
    out.allow = allow_level::official_only;
    out.min_sandbox = sandbox_level::full;
    out.allow_no_sandbox = false;
    out.user_plugins = false;
    return out;
}

result<policy> parse_policy(std::string_view text, std::string* explain)
{
    reading state;
    const auto failure = [&]() -> result<policy> {
        if (explain != nullptr)
            *explain = state.problem;
        return make_error_code(errc::config_error);
    };
    if (text.size() > max_policy_size)
    {
        state.fail("policy", "is larger than " +
                                 std::to_string(max_policy_size) + " bytes");
        return failure();
    }
    std::string what;
    const std::optional<json::value> root =
        detail::parse_strict(text, max_policy_depth, what);
    if (!root)
    {
        state.fail("policy", what);
        return failure();
    }
    if (!root->is_object())
    {
        state.fail("policy", "must be an object");
        return failure();
    }
    const json::object& object = root->get_object();
    for (const auto& entry : object)
    {
        const std::string_view key{entry.key().data(), entry.key().size()};
        if (std::ranges::find(known_keys, key) == known_keys.end())
        {
            state.fail("policy", "unknown key " + detail::quoted(key));
            return failure();
        }
    }

    // The version is needed, so that a file meant for a later reader is
    // never half understood by this one.
    const json::value* const version = object.if_contains("policy_version");
    if (version == nullptr)
    {
        state.fail("policy_version", "is required");
        return failure();
    }
    if (!version->is_int64() ||
        version->get_int64() != std::int64_t{policy_format_version})
    {
        state.fail("policy_version",
                   "must be " + std::to_string(policy_format_version));
        return failure();
    }

    policy out;
    if (const json::value* const value = object.if_contains("allow"))
    {
        if (const auto text_value = word(state, *value, "allow"))
        {
            const auto found = std::ranges::find(
                allow_names, *text_value,
                &std::pair<std::string_view, allow_level>::first);
            if (found == allow_names.end())
                state.fail("allow",
                           "unknown value " + detail::quoted(*text_value));
            else
                out.allow = found->second;
        }
    }
    if (const json::value* const value = object.if_contains("allow_keys"))
    {
        out.allow_keys =
            names(state, *value, "allow_keys", max_policy_keys, is_key_id);
    }
    if (const json::value* const value = object.if_contains("deny_plugins"))
    {
        out.deny_plugins = names(state, *value, "deny_plugins",
                                 max_policy_plugins, detail::valid_plugin_id);
    }
    if (const json::value* const value = object.if_contains("min_sandbox"))
    {
        if (const auto text_value = word(state, *value, "min_sandbox"))
        {
            const auto found = std::ranges::find(
                sandbox_names, *text_value,
                &std::pair<std::string_view, sandbox_level>::first);
            if (found == sandbox_names.end())
                state.fail("min_sandbox",
                           "unknown value " + detail::quoted(*text_value));
            else
                out.min_sandbox = found->second;
        }
    }
    if (const json::value* const value =
            object.if_contains("allow_no_sandbox"))
    {
        out.allow_no_sandbox =
            flag(state, *value, "allow_no_sandbox", out.allow_no_sandbox);
    }
    if (const json::value* const value = object.if_contains("user_plugins"))
    {
        out.user_plugins =
            flag(state, *value, "user_plugins", out.user_plugins);
    }
    if (const json::value* const value = object.if_contains("on_timeout");
        value != nullptr && !value->is_null())
    {
        if (const auto text_value = word(state, *value, "on_timeout"))
        {
            if (*text_value == "fail_open")
                out.on_timeout = timeout_action::fail_open;
            else if (*text_value == "fail_closed")
                out.on_timeout = timeout_action::fail_closed;
            else
                state.fail("on_timeout",
                           "unknown value " + detail::quoted(*text_value));
        }
    }
    if (const json::value* const value =
            object.if_contains("permissions_denied"))
    {
        const std::vector<std::string> listed = names(
            state, *value, "permissions_denied", max_permissions,
            [](std::string_view name) {
                return parse_permission(name).has_value();
            });
        for (const std::string& name : listed)
        {
            if (const auto parsed = parse_permission(name))
                out.permissions_denied.push_back(*parsed);
        }
    }

    if (state.failed())
        return failure();
    if (explain != nullptr)
        explain->clear();
    return out;
}

} // namespace vss::plugin
