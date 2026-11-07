//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What macOS says about serial devices, the App Sandbox and the Application
// Firewall. Everything is read without changing anything and without
// administrator rights: `stat` and `access` of each device, the App Sandbox
// environment variable, the kernel release (to know whether the Local Network
// permission exists), and the output of two read-only queries of the
// Application Firewall tool. Studio makes no change on macOS: the firewall
// answer is the user's, given in the prompt macOS shows, and the fix for a "no"
// is a command the user runs.
//

#include <vss/core/profile.hpp>

#include "../permissions_platform.hpp"
#include "../posix/permissions_posix.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <mach-o/dyld.h>
#include <sys/utsname.h>

namespace vss::detail
{
namespace
{

using namespace permissions;

constexpr const char* firewall_tool =
    "/usr/libexec/ApplicationFirewall/socketfilterfw";

} // namespace

std::string_view platform_host_name() noexcept
{
    return "macos";
}

permissions::observation platform_observe(const permissions::context& ask)
{
    VSS_PROFILE_ZONE("ports.permissions.observe_macos");
    observation seen;
    macos_observation& out = seen.macos_host;

    out.user = posix::user_name();
    const auto groups = posix::process_groups();
    for (const std::string& path : ask.device_paths)
        out.devices.push_back(posix::observe_device(path, out.user, groups));

    // The App Sandbox sets this for every sandboxed process.
    out.sandboxed = posix::environment("APP_SANDBOX_CONTAINER_ID").empty()
                        ? tri::no
                        : tri::yes;

    utsname name{};
    if (::uname(&name) == 0)
        out.darwin_major = darwin_major_from_release(name.release);

    // The Application Firewall matters only to a listener that other computers
    // reach, so it is asked only then.
    bool exposed = false;
    for (const listener& each : ask.listeners)
        exposed = exposed || !is_loopback_host(each.host);
    if (exposed && posix::path_exists(firewall_tool))
    {
        const auto timeout = std::chrono::milliseconds(3000);
        if (const auto state = posix::capture_command(
                {firewall_tool, "--getglobalstate"}, timeout))
        {
            parse_alf_global_state(*state, out.firewall_on, out.block_all);
        }
        if (out.firewall_on == tri::yes && out.block_all != tri::yes)
        {
            if (const auto block = posix::capture_command(
                    {firewall_tool, "--getblockall"}, timeout))
            {
                if (parse_alf_block_all(*block) == tri::yes)
                    out.block_all = tri::yes;
            }
        }
    }
    return seen;
}

std::string platform_this_program()
{
    std::uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    return buffer;
}

bool platform_can_manage_firewall_rules() noexcept
{
    return false;
}

permissions::step_result
platform_apply_firewall_rule(const permissions::firewall_rule_spec&, bool)
{
    return {permissions::step_status::unsupported,
            "this platform has no firewall rule step"};
}

bool platform_can_run_elevated() noexcept
{
    return false;
}

permissions::step_result platform_run_elevated(const std::string&,
                                               const std::vector<std::string>&,
                                               int)
{
    return {permissions::step_status::unsupported,
            "this platform has no elevated helper step"};
}

bool platform_can_add_user_to_group() noexcept
{
    return false;
}

permissions::step_result
platform_add_user_to_group(const permissions::group_membership_spec&)
{
    return {permissions::step_status::unsupported,
            "this platform has no group step"};
}

} // namespace vss::detail
