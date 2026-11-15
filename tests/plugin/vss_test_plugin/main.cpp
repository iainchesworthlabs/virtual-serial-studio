//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A program that behaves as a hostile or broken plugin would, for the tests
// of `vss::plugin::host::plugin_process`.  It does what its arguments say, in
// the order given, and then exits with 0 unless an argument says otherwise.
// It links nothing of the project.  Every line it prints ends with "\n" and
// standard output is flushed after each action.
//
// Arguments:
//   --print-arguments     prints "argc=<n>" and then one line "arg <i>:
//                         <hex>" for each argument after the program's name
//                         (this one too), and exits 0 at once
//   --print-environment   prints "env NAME=value" for every variable, sorted
//                         by name, and then "env-end"
//   --print-directory     prints "cwd <the working directory>"
//   --stdout=<text>       prints the text and a newline to standard output
//   --stderr=<text>       prints the text and a newline to standard error
//   --read-stdin          reads standard input to its end, prints
//                         "stdin <bytes read>"
//   --list-descriptors    prints "descriptors <open descriptors>"
//   --probe-handle=<n>    prints "handle <n> open" or "handle <n> closed"
//   --channel-say=<text>  writes the bytes of the text to the channel; prints
//                         "no-channel" and exits 3 when there is no channel
//   --channel-echo        writes what the channel delivers straight back
//                         until it ends, then prints "channel-end"
//   --spawn-children=<n>  starts n idle children, prints "child <pid>" for
//                         each
//   --ignore-sigterm      ignores the request to stop, prints "ignoring"
//   --pid                 prints "pid <process id>"
//   --flood-stdout=<n>    writes n bytes 'x' to standard output, in chunks of
//                         4096, and a newline
//   --hang                prints "hanging" and never returns
//   --crash               ends the process the way a fault does
//   --exit=<code>         exits at once with the code
// An argument it does not know makes it print "unknown argument" to standard
// error and exit with 2.
//

#include "platform.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

/// Writes the text to the stream and flushes it.
void emit(std::FILE* stream, std::string_view text)
{
    static_cast<void>(std::fwrite(text.data(), 1, text.size(), stream));
    static_cast<void>(std::fflush(stream));
}

/// One line on standard output.
void say(const std::string& line)
{
    emit(stdout, line + "\n");
}

template <class Number>
std::optional<Number> parse_number(std::string_view text)
{
    Number value{};
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return std::nullopt;
    return value;
}

/// The part of `argument` after `name`, when it starts with `name`.
std::optional<std::string_view> value_after(std::string_view argument,
                                            std::string_view name)
{
    if (!argument.starts_with(name))
        return std::nullopt;
    return argument.substr(name.size());
}

std::string hex_of(std::string_view bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const char letter : bytes)
    {
        const auto byte = static_cast<unsigned char>(letter);
        text += digits[byte >> 4];
        text += digits[byte & 0x0f];
    }
    return text;
}

[[noreturn]] void sleep_forever()
{
    for (;;)
        std::this_thread::sleep_for(std::chrono::hours(1));
}

void print_arguments(int argc, char** argv)
{
    std::string text = "argc=" + std::to_string(argc - 1) + "\n";
    for (int index = 1; index < argc; ++index)
    {
        text += "arg ";
        text += std::to_string(index);
        text += ": ";
        text += hex_of(argv[index]);
        text += "\n";
    }
    emit(stdout, text);
}

void print_environment()
{
    std::vector<std::pair<std::string, std::string>> variables;
    for (const std::string& entry : test_plugin::environment())
    {
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos)
            continue;
        variables.emplace_back(entry.substr(0, equals),
                               entry.substr(equals + 1));
    }
    std::sort(variables.begin(), variables.end());
    for (const auto& [name, value] : variables)
    {
        std::string line;
        line.reserve(name.size() + value.size() + 5);
        line.append("env ").append(name).append("=").append(value);
        say(line);
    }
    say("env-end");
}

std::optional<int> print_directory()
{
    std::error_code ec;
    const std::filesystem::path here = std::filesystem::current_path(ec);
    if (ec)
    {
        emit(stderr, "no working directory: " + ec.message() + "\n");
        return 6;
    }
    say("cwd " + here.generic_string());
    return std::nullopt;
}

void read_standard_input()
{
    std::array<char, 4096> chunk{};
    unsigned long long total = 0;
    for (;;)
    {
        const std::size_t got =
            std::fread(chunk.data(), 1, chunk.size(), stdin);
        if (got == 0)
            break;
        total += got;
    }
    say("stdin " + std::to_string(total));
}

std::optional<int> channel_say(std::string_view text)
{
    const auto channel = test_plugin::channel_handle();
    if (!channel.has_value())
    {
        say("no-channel");
        return 3;
    }
    if (!test_plugin::write_handle(*channel, text.data(), text.size()))
    {
        emit(stderr, "the channel could not be written\n");
        return 4;
    }
    return std::nullopt;
}

std::optional<int> channel_echo()
{
    const auto channel = test_plugin::channel_handle();
    if (!channel.has_value())
    {
        say("no-channel");
        return 3;
    }
    std::array<char, 4096> chunk{};
    for (;;)
    {
        const long long got =
            test_plugin::read_handle(*channel, chunk.data(), chunk.size());
        if (got < 0)
        {
            emit(stderr, "the channel could not be read\n");
            return 4;
        }
        if (got == 0)
            break;
        if (!test_plugin::write_handle(*channel, chunk.data(),
                                       static_cast<std::size_t>(got)))
        {
            emit(stderr, "the channel could not be written\n");
            return 4;
        }
    }
    say("channel-end");
    return std::nullopt;
}

std::optional<int> spawn_children(unsigned long long count, const char* self)
{
    for (unsigned long long made = 0; made < count; ++made)
    {
        const long long pid = test_plugin::start_idle_child(self);
        if (pid < 0)
        {
            emit(stderr, "a child could not be started\n");
            return 5;
        }
        say("child " + std::to_string(pid));
    }
    return std::nullopt;
}

void flood_standard_output(unsigned long long bytes)
{
    const std::string chunk(4096, 'x');
    unsigned long long left = bytes;
    while (left > 0)
    {
        const std::size_t now = static_cast<std::size_t>(
            std::min<unsigned long long>(left, chunk.size()));
        emit(stdout, std::string_view(chunk).substr(0, now));
        left -= now;
    }
    emit(stdout, "\n");
}

/// Does what one argument says.  A value says the program ends with that exit
/// code; nothing says to go on with the next argument.
std::optional<int> act(std::string_view argument, const char* self)
{
    if (argument == "--print-environment")
    {
        print_environment();
        return std::nullopt;
    }
    if (argument == "--print-directory")
        return print_directory();
    if (const auto text = value_after(argument, "--stdout="))
    {
        say(std::string(*text));
        return std::nullopt;
    }
    if (const auto text = value_after(argument, "--stderr="))
    {
        emit(stderr, std::string(*text) + "\n");
        return std::nullopt;
    }
    if (argument == "--read-stdin")
    {
        read_standard_input();
        return std::nullopt;
    }
    if (argument == "--list-descriptors")
    {
        say("descriptors " + test_plugin::open_descriptors());
        return std::nullopt;
    }
    if (const auto text = value_after(argument, "--probe-handle="))
    {
        if (const auto number = parse_number<std::intptr_t>(*text))
        {
            const bool open = test_plugin::handle_is_open(*number);
            say("handle " + std::string(*text) + (open ? " open" : " closed"));
            return std::nullopt;
        }
    }
    if (const auto text = value_after(argument, "--channel-say="))
    {
        return channel_say(*text);
    }
    if (argument == "--channel-echo")
    {
        return channel_echo();
    }
    if (const auto text = value_after(argument, "--spawn-children="))
    {
        if (const auto count = parse_number<unsigned long long>(*text))
            return spawn_children(*count, self);
    }
    if (argument == "--ignore-sigterm")
    {
        test_plugin::ignore_stop_request();
        say("ignoring");
        return std::nullopt;
    }
    if (argument == "--pid")
    {
        say("pid " + std::to_string(test_plugin::process_id()));
        return std::nullopt;
    }
    if (const auto text = value_after(argument, "--flood-stdout="))
    {
        if (const auto bytes = parse_number<unsigned long long>(*text))
        {
            flood_standard_output(*bytes);
            return std::nullopt;
        }
    }
    if (argument == "--hang")
    {
        say("hanging");
        sleep_forever();
    }
    if (argument == "--crash")
    {
        test_plugin::crash();
        return 1;
    }
    if (const auto text = value_after(argument, "--exit="))
    {
        if (const auto code = parse_number<int>(*text))
            return *code;
    }

    emit(stderr, "unknown argument\n");
    return 2;
}

/// What `main` does, where it may throw.
int run(int argc, char** argv)
{
    test_plugin::binary_output();

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--print-arguments")
        {
            print_arguments(argc, argv);
            return 0;
        }
        if (const std::optional<int> code = act(argument, argv[0]))
            return *code;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        return run(argc, argv);
    }
    catch (...)
    {
        return 4;
    }
}
