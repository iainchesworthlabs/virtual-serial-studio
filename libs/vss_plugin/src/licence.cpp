//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/licence.hpp>

#include <vss/plugin/error.hpp>

#include <string>
#include <string_view>

namespace vss::plugin
{

namespace
{

/// The answer for a plugin the licence is not asked about.
licence_answer not_asked()
{
    return licence_answer{};
}

/// The answer that carries what the licence library said.
licence_answer asked_about(const license::entitlement& said)
{
    licence_answer answer;
    answer.asked = true;
    answer.granted = said.granted;
    answer.reason = said.reason;
    answer.licence_id = said.licence_id;
    answer.days_left = said.days_left;
    return answer;
}

} // namespace

std::string plugin_feature(std::string_view plugin_id)
{
    std::string name{"plugins."};
    name += plugin_id;
    return name;
}

bool needs_licence(trust_level level, const manifest& declared) noexcept
{
    return level == trust_level::official && !declared.needs.edition.empty();
}

result<void> check_licence(trust_level level, const manifest& declared,
                           const license::status& current,
                           licence_answer* answer)
{
    licence_answer found = not_asked();
    if (needs_licence(level, declared))
    {
        // The plugin's own feature first, then the one that covers every
        // official plugin.  Both questions are put to the same status, so
        // the second answer says why neither held: when the licence is fine
        // and names neither, both say `not_in_licence`.
        license::entitlement said =
            license::check(current, plugin_feature(declared.id));
        if (!said.granted)
            said = license::check(current, all_official_plugins_feature);
        found = asked_about(said);
    }
    if (answer != nullptr)
        *answer = found;
    if (!found.granted)
        return make_error_code(plugin_errc::not_licensed);
    return result<void>{};
}

} // namespace vss::plugin
