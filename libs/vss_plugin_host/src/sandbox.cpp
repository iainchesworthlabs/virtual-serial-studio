//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/sandbox.hpp>

#include <vss/plugin/policy.hpp>

#include "platform/process_platform.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

namespace vss::plugin::host
{

std::string_view to_string(sandbox_part part) noexcept
{
    switch (part)
    {
    case sandbox_part::containment:
        return "containment";
    case sandbox_part::file_system:
        return "file_system";
    case sandbox_part::network:
        return "network";
    case sandbox_part::other_processes:
        return "other_processes";
    case sandbox_part::system_calls:
        return "system_calls";
    case sandbox_part::user_interface:
        return "user_interface";
    case sandbox_part::limits:
        return "limits";
    }
    return "unknown";
}

bool sandbox_report::has(sandbox_part part) const noexcept
{
    return std::ranges::find(applied, part) != applied.end();
}

std::vector<sandbox_part> sandbox_report::not_applied() const
{
    std::vector<sandbox_part> missing;
    for (const sandbox_part part : all_sandbox_parts)
    {
        if (!has(part))
            missing.push_back(part);
    }
    return missing;
}

sandbox_level sandbox_report::level() const noexcept
{
    if (!has(sandbox_part::file_system) || !has(sandbox_part::containment))
        return sandbox_level::none;
    return std::ranges::all_of(all_sandbox_parts,
                               [this](sandbox_part part) { return has(part); })
               ? sandbox_level::full
               : sandbox_level::partial;
}

sandbox_report available_sandbox()
{
    return platform::available_sandbox();
}

} // namespace vss::plugin::host
