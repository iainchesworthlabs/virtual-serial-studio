//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a plugin's process is confined to, and what of that this computer
// really applied.  docs/plugins.md, section 9, is the design.
//
// The host asks for a profile: the directories the plugin may read and
// write, whether it may use the network or start programs, and its limits.
// The system applies what it can and reports each thing it could not.  The
// report is what Studio shows and what the run decision uses, never the
// profile that was asked for.
//
// The level is worked out from the report by one rule, the same on every
// system:
//
// - `full`: everything in the list below was applied.
// - `partial`: the file system is closed to the plugin (it can read its
//   own directory, write its data directory and reach nothing else of the
//   user's), and its process cannot outlive or escape the host's kill; one
//   or more of the other things were not applied.
// - `none`: anything less.  Limits on memory or on the number of processes
//   alone do not make a sandbox: a plugin that can read the user's files is
//   not kept apart.
//
// The sandbox is defence in depth.  The boundary is the trust decision.
//

#pragma once

#include <vss/plugin/policy.hpp>

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace vss::plugin::host
{

/// The limits of section 9.3.  The host has already bounded what a
/// manifest asked for.
struct sandbox_limits
{
    /// The memory the plugin and what it starts may commit, in bytes.
    std::uint64_t memory_bytes = 512ULL * 1024 * 1024;
    /// How many processes there may be, the plugin itself included.  A
    /// plugin that may not start programs gets one, whatever this says.
    std::uint32_t processes = 8;
    /// How many files a process may have open.
    std::uint32_t open_files = 256;
    /// The share of all processors the plugin may use, in percent, from 1
    /// to 100.
    std::uint32_t cpu_percent = 50;
};

struct sandbox_profile
{
    /// The plugin's own directory: read, and run programs from.
    std::filesystem::path plugin_directory;
    /// The plugin's data directory: read and write.
    std::filesystem::path data_directory;
    /// Directories the user chose for `fs.read` and `fs.write`.
    std::vector<std::filesystem::path> read_directories;
    std::vector<std::filesystem::path> write_directories;
    /// `net.listen` and `net.outbound` were granted.
    bool network_listen = false;
    bool network_outbound = false;
    /// `process.spawn` was granted.
    bool start_programs = false;
    sandbox_limits limits;
};

/// One thing a sandbox does.
enum class sandbox_part
{
    /// The plugin and all it starts end when the host says so.
    containment,
    /// It reaches no file but those of its profile.
    file_system,
    /// It has no network unless granted.
    network,
    /// It cannot open, signal or watch other processes.
    other_processes,
    /// System calls a plugin has no use for are refused.
    system_calls,
    /// It has no window, clipboard or desktop.
    user_interface,
    /// The limits on memory, processes, files and processor time.
    limits
};

/// Every part, in the order a report lists them.
inline constexpr sandbox_part all_sandbox_parts[] = {
    sandbox_part::containment,  sandbox_part::file_system,
    sandbox_part::network,      sandbox_part::other_processes,
    sandbox_part::system_calls, sandbox_part::user_interface,
    sandbox_part::limits};

/// The part as Studio's log and `vss_cli` name it, such as `file_system`.
std::string_view to_string(sandbox_part part) noexcept;

/// What was applied to one process, or what this computer can apply.
struct sandbox_report
{
    /// The parts that were applied, each once.
    std::vector<sandbox_part> applied;

    bool has(sandbox_part part) const noexcept;
    /// The parts that were not applied, in the order of
    /// `all_sandbox_parts`.
    std::vector<sandbox_part> not_applied() const;
    /// The level, by the rule at the top of this file.
    sandbox_level level() const noexcept;
};

/// What this computer can apply to a plugin, without starting one.  It is
/// what the run decision is given before a start; the report of the start
/// itself is what counts afterwards.
sandbox_report available_sandbox();

} // namespace vss::plugin::host
