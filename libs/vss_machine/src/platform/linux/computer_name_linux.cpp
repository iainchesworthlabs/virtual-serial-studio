//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This computer's name on Linux: what `gethostname` gives.
//

#include "platform/machine_platform.hpp"
#include "platform/posix/posix_system.hpp"

#include <string>

namespace vss::machine::platform
{

std::string full_computer_name()
{
    return posix_host_name();
}

} // namespace vss::machine::platform
