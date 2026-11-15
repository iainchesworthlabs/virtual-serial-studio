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
#include <utility>

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
    /// The time `verify` judges at when `--at` is not given, and the time
    /// `issue` holds a request's `issued_at` against.
    time_point now;
    /// The revocation list built into this build, which `verify` always
    /// consults, as Studio does. The program passes `builtin_revocations()`;
    /// the tests pass their own.
    revocation_list builtin;

    surroundings(std::ostream& out_stream, std::ostream& err_stream,
                 std::function<std::optional<std::string>(const std::string&)>
                     variable_reader,
                 time_point time_now, revocation_list builtin_list)
        : out(out_stream)
        , err(err_stream)
        , variable(std::move(variable_reader))
        , now(time_now)
        , builtin(std::move(builtin_list))
    {
    }
};

/// Runs one command. `arguments` are the words after the program name.
int run(std::span<const std::string> arguments, surroundings& where);

/// The text `vss_license` prints when it is run with no command.
std::string_view usage();

} // namespace vss::license::tool
