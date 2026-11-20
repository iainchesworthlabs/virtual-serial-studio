//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A program that behaves as a hostile or broken plugin would, for the tests
// of `vss::plugin::host::plugin_process` and `plugin_runner`.  It does what
// its arguments say, in the order given, and then exits with 0 unless an
// argument says otherwise.  Only `--serve` uses the project (the plugin API,
// in serve.cpp).  Every line it prints ends with "\n" and standard output is
// flushed after each action.
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
//   --stdout-hex=<hex>    prints the bytes the hex digits stand for and a
//                         newline to standard output, so that a line can hold
//                         control characters or bytes that are not UTF-8
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
//   --allocate=<n>        takes n mebibytes of memory one at a time and
//                         writes to each; prints "allocated <n>", or
//                         "allocation-failed <m>" after m of them
//   --read-file=<path>    prints "read <n>" for the n bytes of the file,
//                         or "read-failed"
//   --write-file=<path>   writes a few bytes to the file; prints "written"
//                         or "write-failed"
//   --connect=<port>      tries a TCP connection to 127.0.0.1 at the port;
//                         prints "connected" or "connect-failed"
//   --open-process=<pid>  tries to open another process; prints
//                         "process-opened" or "process-refused"
//   --hang                prints "hanging" and never returns
//   --crash               ends the process the way a fault does
//   --exit=<code>         exits at once with the code
//   --serve               is a plugin: sends "hello" (a request with id 1)
//                         for the plugin id and version below, and then
//                         reads the channel.  It goes on with the next
//                         message on "hello_ack", answers "ping" with "pong",
//                         answers a request with an op it does not know with
//                         an "error" (unknown_op), and on "shutdown" sends
//                         "bye" and exits 0.  When the channel ends it exits
//                         2.  The modifiers below apply only to `--serve`
//                         and must come before it on the command line.
// Modifiers of `--serve`:
//   --hello-id=<text>     the plugin id its "hello" says (default
//                         com.example.test)
//   --hello-version=<text>  the version its "hello" says (default 1.2.0)
//   --no-hello            serves without ever sending "hello"
//   --hello-twice         sends "hello" a second time at once (id 2)
//   --exit-after-ready=<code>  exits with the code as soon as "hello_ack"
//                         arrives
//   --crash-after-ready   ends the way a fault does as soon as "hello_ack"
//                         arrives
//   --ignore-shutdown     goes on serving after "shutdown" and never sends
//                         "bye"
//   --ignore-ping         never answers "ping"
//   --status=<text>       after "hello_ack" sends a "status" with the level
//                         warning and the text
//   --log=<text>          after "hello_ack" sends a "log" message (level
//                         info) with the text
//   --send-op=<op>        after "hello_ack" sends a request with the op, an
//                         empty body and id 2; prints "answer <op of the
//                         answer>" when the answer arrives, and
//                         "answer-value <value>" as well when it is an
//                         "error"
//   --send-header=<text>  after "hello_ack" (and after --send-op) sends a
//                         request whose header is the text, a JSON object
//                         that holds an "op" and any other members; the
//                         plugin puts the "id" in (3, then 4 and so on).
//                         Prints "answer <op of the answer>" when the answer
//                         arrives, as --send-op does.  May be given more
//                         than once.  A manifest writes the text as a JSON
//                         string, so its quotes are escaped there.
//   --notify-header=<text>  after those, sends a notification whose header
//                         is the text, a JSON object that holds an "op" and
//                         no "id".  May be given more than once.
//   --echo-op=<op>        answers a request with this op with "ok" and
//                         prints "got <op>"; may be given more than once
//   --refuse-op=<op>      answers a request with this op with an "error"
//                         (refused) whose text is "refused by the test
//                         plugin", and prints "got <op>"; may be given more
//                         than once
//   --garbage-after-ready  after "hello_ack" writes 64 bytes that are not a
//                         frame
//   --flood-after-ready=<n>  after "hello_ack" sends n "log" messages as fast
//                         as it can
//   --stderr-each=<text>  after "hello_ack" prints the text once to
//                         standard error
// When "hello_ack" arrives the modifiers act in the order of the list above
// (exit, crash, stderr, status, log, send-op, send-header, garbage, flood).
// An argument it does not know makes it print "unknown argument" to standard
// error and exit with 2.
//

#include "platform.hpp"
#include "serve.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <new>
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

/// The bytes that pairs of hex digits stand for.  Nothing when the text has
/// an odd number of digits or a character that is not a digit.
std::optional<std::string> bytes_of_hex(std::string_view digits)
{
    if (digits.size() % 2 != 0)
        return std::nullopt;
    std::string bytes;
    for (std::size_t index = 0; index < digits.size(); index += 2)
    {
        unsigned value = 0;
        const auto result = std::from_chars(
            digits.data() + index, digits.data() + index + 2, value, 16);
        if (result.ec != std::errc{} || result.ptr != digits.data() + index + 2)
            return std::nullopt;
        bytes += static_cast<char>(value);
    }
    return bytes;
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
        text += '\n';
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

void allocate_memory(unsigned long long mebibytes)
{
    constexpr std::size_t chunk = std::size_t{1024} * 1024;
    // Kept until the process ends, so that the memory stays in use.
    static std::vector<std::unique_ptr<char[]>> kept;
    for (unsigned long long made = 0; made < mebibytes; ++made)
    {
        std::unique_ptr<char[]> block(new (std::nothrow) char[chunk]);
        if (!block)
        {
            say("allocation-failed " + std::to_string(made));
            return;
        }
        std::memset(block.get(), 0x5a, chunk);
        kept.push_back(std::move(block));
    }
    say("allocated " + std::to_string(mebibytes));
}

void read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        say("read-failed");
        return;
    }
    std::size_t total = 0;
    std::array<char, 4096> buffer{};
    while (
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
        in.gcount() > 0)
        total += static_cast<std::size_t>(in.gcount());
    say("read " + std::to_string(total));
}

void write_file(const std::string& path)
{
    std::ofstream out(path, std::ios::binary);
    out << "written by vss_test_plugin";
    out.close();
    say(out.fail() ? "write-failed" : "written");
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
/// code; nothing says to go on with the next argument.  The modifiers of
/// `--serve` are collected in `serving`.
std::optional<int> act(std::string_view argument, const char* self,
                       test_plugin::serve_options& serving)
{
    // Each argument is matched in turn. A flag with a value is read with
    // value_after, and a value that does not parse falls through to the
    // "unknown argument" answer at the end.

    // What the plugin can see of its own process.
    if (argument == "--print-environment")
    {
        print_environment();
        return std::nullopt;
    }
    if (argument == "--print-directory")
        return print_directory();
    // Output on the standard streams, as text or as bytes given in hex.
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
    if (const auto text = value_after(argument, "--stdout-hex="))
    {
        if (const auto bytes = bytes_of_hex(*text))
        {
            say(*bytes);
            return std::nullopt;
        }
    }
    // What the plugin has open: the standard input, the file descriptors and
    // one handle, which a test uses to see what a launch passed down.
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
    // Talk over the channel the host opened, then end with the exit code
    // that comes back.
    if (const auto text = value_after(argument, "--channel-say="))
    {
        return channel_say(*text);
    }
    if (argument == "--channel-echo")
    {
        return channel_echo();
    }
    // Child processes, a signal that is ignored, and the process id, for the
    // tests of stopping a plugin and all that it started.
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
    // Load: a flood of output and an allocation, for the limits on output
    // and memory.
    if (const auto text = value_after(argument, "--flood-stdout="))
    {
        if (const auto bytes = parse_number<unsigned long long>(*text))
        {
            flood_standard_output(*bytes);
            return std::nullopt;
        }
    }
    if (const auto text = value_after(argument, "--allocate="))
    {
        if (const auto count = parse_number<unsigned long long>(*text))
        {
            allocate_memory(*count);
            return std::nullopt;
        }
    }
    // Things a confined plugin may or may not do: read or write a file, open
    // a connection to the loopback address, and open another process.
    if (const auto text = value_after(argument, "--read-file="))
    {
        read_file(std::string(*text));
        return std::nullopt;
    }
    if (const auto text = value_after(argument, "--write-file="))
    {
        write_file(std::string(*text));
        return std::nullopt;
    }
    if (const auto text = value_after(argument, "--connect="))
    {
        if (const auto port = parse_number<unsigned short>(*text))
        {
            say(test_plugin::connect_loopback(*port) ? "connected"
                                                     : "connect-failed");
            return std::nullopt;
        }
    }
    if (const auto text = value_after(argument, "--open-process="))
    {
        if (const auto pid = parse_number<long long>(*text))
        {
            say(test_plugin::can_open_process(*pid) ? "process-opened"
                                                    : "process-refused");
            return std::nullopt;
        }
    }
    // Ways to end: never, by a crash, or with an exit code that was asked for.
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
        if (const std::optional<int> code = parse_number<int>(*text))
            return code;
    }
    // Serve mode runs the plugin as a server. The flags that change it are
    // kept in "serving" and apply when --serve itself is reached.
    if (argument == "--serve")
        return test_plugin::serve(serving);
    if (test_plugin::take_serve_modifier(argument, serving))
        return std::nullopt;

    // Nothing matched: say so and end with 2, so a typo in a test fails.
    emit(stderr, "unknown argument\n");
    return 2;
}

/// What `main` does, where it may throw.
int run(int argc, char** argv)
{
    test_plugin::binary_output();

    test_plugin::serve_options serving;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--print-arguments")
        {
            print_arguments(argc, argv);
            return 0;
        }
        if (const std::optional<int> code = act(argument, argv[0], serving))
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
