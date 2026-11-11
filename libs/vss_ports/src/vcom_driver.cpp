//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>
#include <vss/ports/vcom_driver.hpp>

#include "platform/vcom_driver_platform.hpp"

namespace vss
{

namespace
{

bool valid_com_number(unsigned number) noexcept
{
    return number >= min_driver_com_number && number <= max_driver_com_number;
}

} // namespace

bool driver_available()
{
    VSS_PROFILE_ZONE("ports.vcom.available");
    return detail::platform_driver_available();
}

std::string driver_version()
{
    VSS_PROFILE_ZONE("ports.vcom.version");
    return detail::platform_driver_version();
}

result<virtual_port_result>
create_driver_port(const transport::executor_type& executor,
                   const std::string& name, unsigned com_number)
{
    VSS_PROFILE_ZONE_NAMED(zone, "ports.vcom.create_port");
    if (zone.active())
        zone.text(name);
    // The same answer on every system and with or without a driver, so that a
    // wrong number in a saved topology is caught where it is read.
    if (com_number != 0 && !valid_com_number(com_number))
        return make_error_code(errc::invalid_argument);
    return detail::platform_create_driver_port(executor, name, com_number);
}

result<std::string> set_driver_com_number(transport& port, unsigned com_number)
{
    VSS_PROFILE_ZONE("ports.vcom.set_com_number");
    if (!valid_com_number(com_number))
        return make_error_code(errc::invalid_argument);
    return detail::platform_set_driver_com_number(port, com_number);
}

result<std::uint32_t> revoke_driver_port(transport& port)
{
    VSS_PROFILE_ZONE("ports.vcom.revoke");
    return detail::platform_revoke_driver_port(port);
}

result<void> raise_driver_line_error(transport& port, driver_line_error error,
                                     std::optional<std::uint8_t> byte)
{
    VSS_PROFILE_ZONE("ports.vcom.line_error");
    return detail::platform_raise_driver_line_error(port, error, byte);
}

std::optional<driver_peer_info> driver_peer(const std::string& com_name)
{
    VSS_PROFILE_ZONE("ports.vcom.peer");
    return detail::platform_driver_peer(com_name);
}

} // namespace vss
