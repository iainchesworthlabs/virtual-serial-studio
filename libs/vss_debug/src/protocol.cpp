//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/debug/protocol.hpp>

#include <array>

namespace vss::debug
{
namespace
{

struct operation_row
{
    std::string_view name;
    scope needs;
};

constexpr std::array<operation_row, 13> operations{{
    {"debug.status", scope::read},
    {"debug.snapshot", scope::read},
    {"debug.trace", scope::read},
    {"debug.trace.subscribe", scope::read},
    {"debug.trace.unsubscribe", scope::read},
    {"debug.com_numbers", scope::read},
    {"debug.driver", scope::read},
    {"debug.plugins", scope::read},
    {"debug.traffic", scope::data},
    {"debug.run", scope::act},
    {"debug.gui.state", scope::gui},
    {"debug.gui.screenshot", scope::gui},
    {"debug.audit.verify", scope::read},
}};

} // namespace

std::string_view scope_name(scope value) noexcept
{
    switch (value)
    {
    case scope::read:
        return "debug.read";
    case scope::act:
        return "debug.act";
    case scope::gui:
        return "debug.gui";
    case scope::data:
        return "debug.data";
    }
    return {};
}

std::optional<scope> scope_from_name(std::string_view name) noexcept
{
    for (const scope candidate :
         {scope::read, scope::act, scope::gui, scope::data})
    {
        if (scope_name(candidate) == name)
            return candidate;
    }
    return std::nullopt;
}

std::optional<scope> scope_of_operation(std::string_view operation) noexcept
{
    for (const auto& row : operations)
    {
        if (row.name == operation)
            return row.needs;
    }
    return std::nullopt;
}

} // namespace vss::debug
