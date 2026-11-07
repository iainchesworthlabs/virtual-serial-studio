//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What Linux says about serial device access, confinement and the firewall, and
// the one thing Studio may do about it once the user agrees: ask polkit
// (`pkexec`) to put the user in the group that owns the serial devices.
//
// Everything read here is readable by an ordinary user: the process's groups,
// `stat` and `access` of a device, /proc/self/attr/current (the AppArmor
// profile or SELinux context), /sys/fs/selinux/enforce and /etc/ufw/ufw.conf.
// Firewall rules need root, so they are not read, and the fact says so.
//

#include <vss/core/profile.hpp>

#include "../permissions_platform.hpp"
#include "../posix/permissions_posix.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace vss::detail
{
namespace
{

using namespace permissions;
using posix::path_exists;

std::string find_usermod()
{
    for (const char* candidate :
         {"/usr/sbin/usermod", "/sbin/usermod", "/usr/bin/usermod"})
    {
        if (path_exists(candidate))
            return candidate;
    }
    return {};
}

tri ufw_enabled()
{
    if (!path_exists("/etc/ufw/ufw.conf"))
        return tri::no;
    std::ifstream file("/etc/ufw/ufw.conf");
    if (!file)
        return tri::unknown;
    std::string line;
    while (std::getline(file, line))
    {
        std::string compact;
        for (const char c : line)
        {
            if (c != ' ' && c != '\t' && c != '\r')
                compact += static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
        }
        if (compact == "enabled=yes")
            return tri::yes;
    }
    return tri::no;
}

} // namespace

std::string_view platform_host_name() noexcept
{
    return "linux";
}

permissions::observation platform_observe(const permissions::context& ask)
{
    VSS_PROFILE_ZONE("ports.permissions.observe_linux");
    observation seen;
    linux_observation& out = seen.linux_host;

    out.user = posix::user_name();
    out.root = ::geteuid() == 0 ? tri::yes : tri::no;

    const auto groups = posix::process_groups();
    for (const std::string& path : ask.device_paths)
        out.devices.push_back(posix::observe_device(path, out.user, groups));

    out.label =
        parse_security_label(posix::read_small_file("/proc/self/attr/current"));
    if (path_exists("/sys/fs/selinux"))
    {
        const std::string enforce =
            posix::read_small_file("/sys/fs/selinux/enforce");
        if (!enforce.empty())
            out.selinux_mode =
                enforce.front() == '1' ? "enforcing" : "permissive";
    }
    else
    {
        out.selinux_mode = "disabled";
    }

    if (const std::string id = posix::environment("FLATPAK_ID");
        !id.empty() || path_exists("/.flatpak-info"))
    {
        out.sandbox = "flatpak";
        out.sandbox_name = id;
    }
    else if (!posix::environment("SNAP").empty() &&
             !posix::environment("SNAP_NAME").empty())
    {
        out.sandbox = "snap";
        out.sandbox_name = posix::environment("SNAP_NAME");
    }

    out.ufw_active = ufw_enabled();
    out.firewalld_active =
        path_exists("/run/firewalld.pid") || path_exists("/run/firewalld")
            ? tri::yes
            : tri::no;
    out.can_ask_polkit =
        path_exists("/usr/bin/pkexec") && !find_usermod().empty() ? tri::yes
                                                                  : tri::no;
    return seen;
}

std::string platform_this_program()
{
    char buffer[4096];
    const ssize_t length =
        ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0)
        return {};
    return std::string(buffer, static_cast<std::size_t>(length));
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
            "this platform has no generic elevated step"};
}

bool platform_can_add_user_to_group() noexcept
{
    return path_exists("/usr/bin/pkexec") && !find_usermod().empty();
}

permissions::step_result
platform_add_user_to_group(const permissions::group_membership_spec& spec)
{
    // Only the user running Studio can be put in a group by this step.
    if (spec.user != posix::user_name())
        return {step_status::invalid, "the step changes the current user only"};
    const std::string usermod = find_usermod();
    if (usermod.empty() || !path_exists("/usr/bin/pkexec"))
        return {step_status::unsupported, "pkexec or usermod is not installed"};

    std::vector<std::string> words = {"/usr/bin/pkexec", usermod, "-aG",
                                      spec.group, spec.user};
    std::vector<char*> argv;
    argv.reserve(words.size() + 1);
    for (std::string& word : words)
        argv.push_back(word.data());
    argv.push_back(nullptr);

    pid_t child = 0;
    if (::posix_spawn(&child, "/usr/bin/pkexec", nullptr, nullptr, argv.data(),
                      environ) != 0)
        return {step_status::failed, "pkexec could not be started"};

    int status = 0;
    while (::waitpid(child, &status, 0) < 0)
    {
        if (errno != EINTR)
            return {step_status::failed, "the helper did not finish"};
    }
    if (!WIFEXITED(status))
        return {step_status::failed, "the helper ended abnormally"};
    switch (WEXITSTATUS(status))
    {
    case 0:
        return {step_status::done, "the user was added to the group"};
    case 126:
        // pkexec: the authentication dialog was dismissed.
        return {step_status::declined,
                "the authentication prompt was dismissed"};
    case 127:
        // pkexec: not authorised, or authentication failed.
        return {step_status::not_elevated, "polkit did not authorise the step"};
    default:
        break;
    }
    return {step_status::failed, "usermod did not succeed"};
}

} // namespace vss::detail
