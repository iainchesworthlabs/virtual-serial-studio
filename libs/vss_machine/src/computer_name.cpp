//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/machine/computer_name.hpp>

#include "platform/machine_platform.hpp"

#include <exception>
#include <string>
#include <string_view>

namespace vss::machine
{

std::string short_computer_name(std::string_view full)
{
    const std::string_view::size_type dot = full.find('.');
    return std::string(dot == std::string_view::npos ? full
                                                     : full.substr(0, dot));
}

std::string computer_name()
{
    try
    {
        return short_computer_name(platform::full_computer_name());
    }
    catch (const std::exception&)
    {
        // Out of memory: there is no name to give.
        return {};
    }
}

} // namespace vss::machine
