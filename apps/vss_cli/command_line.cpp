//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "command_line.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <system_error>

namespace vss_cli
{

namespace
{

constexpr std::size_t no_limit = static_cast<std::size_t>(-1);

constexpr option_spec opt_seconds{"--seconds", value_kind::positive_whole};
constexpr option_spec opt_uri{"--uri", value_kind::text};
constexpr option_spec opt_port{"--port", value_kind::text};
constexpr option_spec opt_kind{"--kind", value_kind::text};

/// Every command takes these.
constexpr std::array<option_spec, 4> common_options{{
    {"--verbose", value_kind::none},
    {"--quiet", value_kind::none},
    {"--help", value_kind::none},
    {"--version", value_kind::none},
}};

constexpr std::array<option_spec, 1> devices_options{
    {{"--json", value_kind::none}}};
constexpr std::array<option_spec, 2> share_options{
    {{"--listeners", value_kind::positive_whole}, opt_seconds}};
constexpr std::array<option_spec, 2> join_options{
    {{"--target", value_kind::text}, opt_seconds}};
constexpr std::array<option_spec, 4> bus_options{
    {{"--virtual", value_kind::whole},
     {"--tap", value_kind::text},
     opt_kind,
     opt_seconds}};
constexpr std::array<option_spec, 1> seconds_only{{opt_seconds}};
constexpr std::array<option_spec, 3> virtual_options{
    {opt_kind, {"--com", value_kind::text}, opt_seconds}};
constexpr std::array<option_spec, 4> vport_options{
    {{"--name", value_kind::text},
     {"--link", value_kind::text},
     {"--stable-dir", value_kind::text},
     opt_seconds}};
constexpr std::array<option_spec, 2> run_options{
    {opt_seconds, {"--reconnect", value_kind::none}}};
constexpr std::array<option_spec, 5> capture_options{
    {opt_uri,
     {"--out", value_kind::text},
     {"--format", value_kind::text},
     opt_port,
     opt_seconds}};
constexpr std::array<option_spec, 7> play_options{
    {opt_uri,
     opt_port,
     {"--speed", value_kind::positive_real},
     {"--loop", value_kind::none},
     {"--direction", value_kind::text},
     {"--from", value_kind::text},
     opt_seconds}};
constexpr std::array<option_spec, 3> respond_options{
    {opt_uri, opt_port, opt_seconds}};
constexpr std::array<option_spec, 4> bench_options{
    {{"--from", value_kind::text},
     {"--to", value_kind::text},
     {"--bytes", value_kind::text},
     {"--chunk", value_kind::text}}};

constexpr std::array<command_spec, 14> commands{{
    {"devices", devices_options, 0},
    {"share", share_options, 1},
    {"join", join_options, no_limit},
    {"bus", bus_options, no_limit},
    {"bridge", seconds_only, 2},
    {"loopback", seconds_only, 0},
    {"virtual", virtual_options, 0},
    {"vport", vport_options, 0},
    {"run", run_options, 1},
    {"capture", capture_options, 1},
    {"play", play_options, 2},
    {"respond", respond_options, 2},
    {"bench", bench_options, 0},
    {"selftest", {}, 0},
}};

const option_spec* find_option(const command_spec& spec, std::string_view name)
{
    for (const auto& option : spec.options)
    {
        if (option.name == name)
            return &option;
    }
    for (const auto& option : common_options)
    {
        if (option.name == name)
            return &option;
    }
    return nullptr;
}

/// What is wrong with `value` for an option of this kind, or nothing.
std::string value_problem(const option_spec& option, std::string_view value)
{
    const std::string name(option.name);
    const std::string shown = " (not '" + std::string(value) + "')";
    switch (option.kind)
    {
    case value_kind::none:
    case value_kind::text:
        return {};
    case value_kind::whole:
    {
        const auto number = parse_whole_number(value);
        if (!number || *number < 0 || *number > INT_MAX)
            return name + " wants a whole number, 0 or more" + shown;
        return {};
    }
    case value_kind::positive_whole:
    {
        const auto number = parse_whole_number(value);
        if (!number || *number < 1 || *number > INT_MAX)
            return name + " wants a whole number, 1 or more" + shown;
        return {};
    }
    case value_kind::positive_real:
        if (!parse_positive_number(value))
            return name + " wants a number above zero" + shown;
        return {};
    }
    return {};
}

} // namespace

bool arguments::has(const std::string& flag) const
{
    return std::find(flags.begin(), flags.end(), flag) != flags.end();
}

std::string arguments::get(const std::string& key,
                           const std::string& fallback) const
{
    const auto it = options.find(key);
    return it == options.end() ? fallback : it->second;
}

std::vector<std::string> arguments::all(const std::string& key) const
{
    const auto it = repeated.find(key);
    return it == repeated.end() ? std::vector<std::string>{} : it->second;
}

int arguments::number(const std::string& key, int fallback) const
{
    const auto it = options.find(key);
    if (it == options.end())
        return fallback;
    const auto parsed = parse_whole_number(it->second);
    return parsed && *parsed >= INT_MIN && *parsed <= INT_MAX
               ? static_cast<int>(*parsed)
               : fallback;
}

double arguments::real(const std::string& key, double fallback) const
{
    const auto it = options.find(key);
    if (it == options.end())
        return fallback;
    return parse_positive_number(it->second).value_or(fallback);
}

const command_spec* find_command_spec(std::string_view name)
{
    const auto found = std::find_if(commands.begin(), commands.end(),
                                    [&](const command_spec& each)
                                    { return each.name == name; });
    return found == commands.end() ? nullptr : &*found;
}

bool takes_a_value(std::string_view name)
{
    for (const auto& command : commands)
    {
        for (const auto& option : command.options)
        {
            if (option.name == name && option.kind != value_kind::none)
                return true;
        }
    }
    return false;
}

std::optional<std::size_t>
find_command_word(const std::vector<std::string>& words)
{
    for (std::size_t index = 0; index < words.size(); ++index)
    {
        const std::string& word = words[index];
        if (word == "--")
        {
            if (index + 1 < words.size())
                return index + 1;
            return std::nullopt;
        }
        if (word.size() < 2 || word[0] != '-')
            return index;
        // `--seconds 5 run lab.json`: 5 belongs to --seconds, as it does in
        // the parser.
        if (word[1] == '-' && word.find('=') == std::string::npos &&
            takes_a_value(word) && index + 1 < words.size() &&
            !words[index + 1].starts_with("--"))
            ++index;
    }
    return std::nullopt;
}

std::optional<long long> parse_whole_number(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    long long value = 0;
    const char* const end = text.data() + text.size();
    // The end pointer carries the size, so the data() call is bounded.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || stop != end)
        return std::nullopt;
    return value;
}

std::optional<double> parse_positive_number(std::string_view text)
{
    // Only the characters of a decimal number: strtod would also take white
    // space, a hexadecimal number, "inf" and "nan".
    constexpr std::string_view allowed = "0123456789.eE+-";
    if (text.empty() ||
        text.find_first_not_of(allowed) != std::string_view::npos)
        return std::nullopt;

    const std::string copy(text);
    char* stop = nullptr;
    errno = 0;
    const double value = std::strtod(copy.c_str(), &stop);
    if (errno != 0 || stop != copy.c_str() + copy.size() ||
        !std::isfinite(value) || !(value > 0.0))
        return std::nullopt;
    return value;
}

parse_result parse_arguments(const command_spec& spec,
                             const std::vector<std::string>& words)
{
    parse_result result;
    arguments& parsed = result.args;
    bool only_arguments = false;

    for (std::size_t index = 0; index < words.size(); ++index)
    {
        const std::string& word = words[index];
        if (only_arguments || word.size() < 2 || word[0] != '-')
        {
            parsed.positional.push_back(word);
            continue;
        }
        if (word == "--")
        {
            only_arguments = true;
            continue;
        }
        if (word[1] != '-')
        {
            result.error = "unknown option " + word;
            return result;
        }

        const auto equals = word.find('=');
        const std::string name = word.substr(0, equals);
        const option_spec* option = find_option(spec, name);
        if (option == nullptr)
        {
            result.error = "unknown option " + name;
            return result;
        }

        if (option->kind == value_kind::none)
        {
            if (equals != std::string::npos)
            {
                result.error = name + " takes no value";
                return result;
            }
            parsed.flags.push_back(name);
            continue;
        }

        std::string value;
        if (equals != std::string::npos)
        {
            value = word.substr(equals + 1);
        }
        else
        {
            // The value is the next word, which may not look like an option:
            // `--out --seconds` is a missing value, not a file called that.
            const bool present =
                index + 1 < words.size() && !words[index + 1].starts_with("--");
            if (!present)
            {
                result.error = name + " needs a value";
                return result;
            }
            value = words[++index];
        }
        if (auto problem = value_problem(*option, value); !problem.empty())
        {
            result.error = std::move(problem);
            return result;
        }
        parsed.options[name] = value;
        parsed.repeated[name].push_back(std::move(value));
    }

    // The command word is the first of the positional arguments.
    if (!parsed.positional.empty() && spec.max_positional != no_limit &&
        parsed.positional.size() - 1 > spec.max_positional)
    {
        result.error =
            "unexpected argument " + parsed.positional[spec.max_positional + 1];
    }
    return result;
}

} // namespace vss_cli
