//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The JSON `vss_cli devices --json` prints.
//

#pragma once

#include <vss/ports/com0com.hpp>
#include <vss/ports/serial_enumeration.hpp>

#include <string>
#include <vector>

namespace vss_cli
{

/// The devices and the com0com installation as one JSON document on one line,
/// ended by a newline.  A name with a quotation mark, a backslash or a control
/// character in it is escaped, and a byte that is not part of a UTF-8 sequence
/// is replaced by U+FFFD, so the text is valid JSON whatever the names hold.
std::string devices_json(const std::vector<vss::serial_device_info>& devices,
                         const vss::com0com_installation& installation);

} // namespace vss_cli
