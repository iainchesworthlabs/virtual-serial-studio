//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/ports/permissions.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace vss::detail
{

/// Implemented once for each operating system in `src/platform/<os>/`.

/// "windows", "linux" or "macos".
std::string_view platform_host_name() noexcept;

/// Reads what the host says. Never changes anything. A question the platform
/// cannot answer stays `unknown`.
permissions::observation platform_observe(const permissions::context& ask);

/// Full path of the running program, or empty when it cannot be found.
std::string platform_this_program();

/// True when the platform has a firewall rule step
/// (`platform_apply_firewall_rule`).
bool platform_can_manage_firewall_rules() noexcept;

/// Adds or removes the one rule of `spec`. The caller has validated the spec
/// and the process is expected to be elevated; the function checks and says so.
permissions::step_result
platform_apply_firewall_rule(const permissions::firewall_rule_spec& spec,
                             bool remove);

/// True when the platform can start a program elevated through its own prompt.
bool platform_can_run_elevated() noexcept;

/// Starts `executable` elevated, waits, and maps its exit code to a status
/// (`permissions::exit_*`).
permissions::step_result
platform_run_elevated(const std::string& executable,
                      const std::vector<std::string>& arguments,
                      int wait_seconds);

/// True when `pkexec` and `usermod` are installed, so a group can be changed
/// through polkit.
bool platform_can_add_user_to_group() noexcept;

/// Runs `pkexec usermod -aG <group> <user>` and waits. The caller validated the
/// spec; the function checks that `user` is the user running this program.
permissions::step_result
platform_add_user_to_group(const permissions::group_membership_spec& spec);

} // namespace vss::detail
