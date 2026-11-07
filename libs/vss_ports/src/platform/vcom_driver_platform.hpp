//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_PORTS_VCOM_DRIVER_PLATFORM_HPP
#define VSS_PORTS_VCOM_DRIVER_PLATFORM_HPP

#include <vss/core/error.hpp>
#include <vss/ports/driver_setup.hpp>
#include <vss/ports/vcom_driver.hpp>
#include <vss/ports/virtual_port.hpp>

#include <optional>
#include <string>

namespace vss::detail
{

/// Implemented once for each operating system in `src/platform/<os>/`.
/// True when the driver is installed and has at least one port.
bool platform_driver_available();

/// The installed driver version, or an empty string.
std::string platform_driver_version();

/// Opens the hub end of a free driver port. `errc::unsupported` when there is
/// no driver, `errc::device_busy` when every port is taken.
/// `com_number` is 0 or a number in range; the caller has checked.
result<virtual_port_result>
platform_create_driver_port(const transport::executor_type& executor,
                            const std::string& name, unsigned com_number);

/// Changes the COM number of a port made by `platform_create_driver_port`.
result<std::string> platform_set_driver_com_number(transport& port,
                                                   unsigned com_number);

/// Takes a driver port back from the program that has it open.
result<std::uint32_t> platform_revoke_driver_port(transport& port);

/// What the driver last said about the program on the COM port `com_name`.
std::optional<driver_peer_info> platform_driver_peer(const std::string& com_name);

/// Asks each piece of the driver set-up on its own. Empty where no driver is
/// needed. Makes no port.
std::optional<driver_setup_observation> platform_observe_driver_setup();

/// The last failed step of making a port in this process.
driver_failure platform_last_driver_failure();

} // namespace vss::detail

#endif // VSS_PORTS_VCOM_DRIVER_PLATFORM_HPP
