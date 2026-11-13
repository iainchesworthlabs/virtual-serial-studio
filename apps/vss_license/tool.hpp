//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The commands of vss_license (docs/licensing.md, "The tools"). They are a
// function, not a program, so the tests run them in the test process.
//

#pragma once

#include <vss/license/license.hpp>

#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>

namespace vss::license::tool
{

/// Exit statuses.
inline constexpr int exit_ok = 0;
/// The file is not valid, or the command could not do what was asked.
inline constexpr int exit_failed = 1;
inline constexpr int exit_usage = 2;

/// What the commands use from outside. The tests pass their own.
struct surroundings
{
    std::ostream& out;
    std::ostream& err;
    /// Reads an environment variable. Used for `--key-env` only.
    std::function<std::optional<std::string>(const std::string&)> variable;
    /// The time `verify` judges at when `--at` is not given.
    time_point now;
};

/// Runs one command. `arguments` are the words after the program name.
int run(std::span<const std::string> arguments, surroundings& where);

/// The text `vss_license` prints when it is run with no command.
std::string_view usage();

} // namespace vss::license::tool
