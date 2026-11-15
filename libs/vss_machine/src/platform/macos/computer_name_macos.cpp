//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This computer's name on macOS: the name in System Settings, Sharing
// ("Local hostname", without the ".local"), which does not change with the
// network.  `gethostname` gives the DHCP or Bonjour name, which does, so it
// is used only when the first gives nothing.
//

#include "platform/machine_platform.hpp"
#include "platform/posix/posix_system.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>

namespace vss::machine::platform
{

namespace
{

using string_type = std::remove_pointer_t<CFStringRef>;

struct release_string
{
    void operator()(CFStringRef text) const noexcept { CFRelease(text); }
};

/// The local host name from the system configuration, in UTF-8, or an empty
/// string when there is none.
std::string local_host_name()
{
    const std::unique_ptr<string_type, release_string> name(
        SCDynamicStoreCopyLocalHostName(nullptr));
    if (name == nullptr)
        return {};
    const CFIndex units = CFStringGetLength(name.get());
    const CFIndex most =
        CFStringGetMaximumSizeForEncoding(units, kCFStringEncodingUTF8);
    if (most < 0)
        return {};
    // One more for the end of the text.
    std::string out(static_cast<std::size_t>(most) + 1, '\0');
    if (CFStringGetCString(name.get(), out.data(),
                           static_cast<CFIndex>(out.size()),
                           kCFStringEncodingUTF8) == 0)
        return {};
    out.resize(out.find('\0'));
    return out;
}

} // namespace

std::string full_computer_name()
{
    std::string name = local_host_name();
    if (name.empty())
        name = posix_host_name();
    return name;
}

} // namespace vss::machine::platform
