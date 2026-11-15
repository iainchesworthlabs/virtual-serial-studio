//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The command line of vss_cli: which options each command has, and a strict
// parser that turns the words into those options.
//
// An option is written `--name=value` or `--name value`, for every option that
// takes a value.  A word that is not an option of the command, a value that
// does not fit, and an argument too many are errors, not something to ignore.
//

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss_cli
{

/// What an option takes after its name.
enum class value_kind
{
    /// A flag: `--loop`.
    none,
    /// Any text; the command checks it.
    text,
    /// A whole number, 0 or more.
    whole,
    /// A whole number, 1 or more.
    positive_whole,
    /// A number above zero, such as 0.5 or 4.
    positive_real,
};

struct option_spec
{
    std::string_view name;
    value_kind kind = value_kind::none;
};

/// One command: its options, and how many arguments follow its name.
struct command_spec
{
    std::string_view name;
    std::span<const option_spec> options;
    std::size_t max_positional = 0;
};

/// The words of a command line, sorted into what they are.
struct arguments
{
    /// The command first, then its arguments.
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    /// Every value an option was given, for the ones that may repeat.
    std::map<std::string, std::vector<std::string>> repeated;
    std::vector<std::string> flags;

    bool has(const std::string& flag) const;
    std::string get(const std::string& key,
                    const std::string& fallback = {}) const;
    std::vector<std::string> all(const std::string& key) const;
    /// A whole-number option.  The parser has already checked the text, so
    /// `fallback` is only for an option that was not given.
    int number(const std::string& key, int fallback) const;
    /// A real-number option, as `number`.
    double real(const std::string& key, double fallback) const;
};

struct parse_result
{
    arguments args;
    /// One line that says what is wrong, without the program or command name;
    /// empty when the command line is good.
    std::string error;

    bool ok() const { return error.empty(); }
};

/// The commands the program runs itself: not `agent`, `control`,
/// `permissions` and `plugin`, which read their own options.  Null for a word
/// that is not one of them.
const command_spec* find_command_spec(std::string_view name);

/// Sorts `words` (the command line without the program name) by the options
/// of `spec`.  The command word is the first one that does not start with a
/// dash, and may be preceded by options.
parse_result parse_arguments(const command_spec& spec,
                             const std::vector<std::string>& words);

/// The whole of `text` as a decimal number, or nothing when there is other
/// text, nothing at all, or a value that does not fit.  A leading minus sign
/// is allowed.
std::optional<long long> parse_whole_number(std::string_view text);

/// The whole of `text` as a number above zero that is not infinite or not a
/// number, or nothing.  Plain decimal notation, with an optional exponent.
std::optional<double> parse_positive_number(std::string_view text);

} // namespace vss_cli
