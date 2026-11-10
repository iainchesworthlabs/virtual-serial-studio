//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_PORTS_SERIAL_ENUMERATION_PLATFORM_HPP
#define VSS_PORTS_SERIAL_ENUMERATION_PLATFORM_HPP

#include <vss/ports/serial_enumeration.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::detail
{

/// Implemented once for each operating system in `src/platform/<os>/`.
/// Lists what the system reports; the caller normalises the list.
std::vector<serial_device_info> platform_enumerate_serial_devices();

/// The COM name database bitmap and the names that resolve, read for
/// `reserved_com_numbers`.  `nullopt` where there is none or it cannot be read.
std::optional<std::vector<unsigned>> platform_reserved_com_numbers();

/// The `COMn` name in the last pair of brackets of `text`, or empty.
std::string extract_com_name(std::string_view text);

/// Reads the pair number and side from a com0com `CNC<A|B><n>` instance id.
bool parse_com0com_instance(std::string_view instance, int& pair,
                            std::string& side);

} // namespace vss::detail

#endif // VSS_PORTS_SERIAL_ENUMERATION_PLATFORM_HPP
