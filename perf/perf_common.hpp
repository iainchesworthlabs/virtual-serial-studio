//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared by the throughput benchmarks in perf_main.cpp and the per-component
// micro-benchmarks in component_benchmarks.cpp.
//

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace vss_perf
{

struct measurement
{
    std::string name;
    double value = 0.0;
    std::string unit;
    std::string note;
};

inline double seconds_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
}

/// The file a `--output` argument names, resolved to an absolute path with
/// every `.` and `..` removed and any link followed once, so the file that is
/// written is the one the argument really leads to. The file may not exist yet,
/// but its directory must, and the path must not name a directory. On failure
/// the result is empty and `reason` says why.
inline std::optional<std::filesystem::path>
resolve_output_path(const std::string& requested, std::string& reason)
{
    namespace fs = std::filesystem;
    if (requested.empty())
    {
        reason = "the output path is empty";
        return std::nullopt;
    }
    std::error_code ec;
    // Absolute first: weakly_canonical leaves a relative path relative when no
    // part of it exists yet, and a bare file name has no directory to check.
    fs::path resolved = fs::absolute(fs::path(requested), ec);
    if (!ec)
        resolved = fs::weakly_canonical(resolved, ec);
    if (ec)
    {
        reason = ec.message();
        return std::nullopt;
    }
    if (fs::is_directory(resolved, ec))
    {
        reason = "the output path is a directory";
        return std::nullopt;
    }
    if (!fs::is_directory(resolved.parent_path(), ec))
    {
        reason = "the output directory does not exist";
        return std::nullopt;
    }
    return resolved;
}

/// The per-component micro-benchmarks: one measurement per library component
/// worth watching (byte formatting, traffic log, configuration validation,
/// studio topology operations). Each runs inside a `bench.<name>` zone, so a
/// Tracy capture of `vss_perf` shows what the component's own zones cost.
std::vector<measurement> run_component_benchmarks(bool quick);

} // namespace vss_perf
