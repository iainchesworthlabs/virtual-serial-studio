//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss::plugin::host::plugin_process` starting a real program: the test
// plugin (VSS_TEST_PLUGIN_PROGRAM), which does what its arguments say.  The
// tests look at what the program is given (arguments, environment, working
// directory, handles), at what the host gets back (output, channel, exit
// status) and at what is left behind when a plugin is killed or ends.  The
// last tests are of the command line and the environment helpers, which are
// built on every system.
//

#include <vss/plugin/error.hpp>
#include <vss/plugin/host/process.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/policy.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "native_io.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using vss::plugin::sandbox_level;
using vss::plugin::host::all_sandbox_parts;
using vss::plugin::host::available_sandbox;
using vss::plugin::host::exit_status;
using vss::plugin::host::native_handle;
using vss::plugin::host::no_handle;
using vss::plugin::host::plugin_environment;
using vss::plugin::host::plugin_process;
using vss::plugin::host::process_request;
using vss::plugin::host::sandbox_part;
using vss::plugin::host::sandbox_profile;
using vss::plugin::host::sandbox_report;
using vss_test_support::scratch_directory;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;
namespace io = vss_test_support;

using steady = std::chrono::steady_clock;

/// How long any wait lasts before the test gives up.
constexpr std::chrono::milliseconds long_limit{20000};

/// The request to run the test plugin with `arguments`, in the scratch
/// directory and with the environment of a plugin.
process_request request_for(const scratch_directory& scratch,
                            std::vector<std::string> arguments)
{
    process_request request;
    request.program = fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM));
    request.arguments = std::move(arguments);
    request.working_directory = scratch.path();
    request.environment =
        plugin_environment("com.example.test", scratch.path());
    return request;
}

plugin_process start_process(const process_request& request)
{
    auto started = plugin_process::start(request);
    if (started.has_error())
    {
        throw vss_test::failure("the plugin did not start: " +
                                started.error().message());
    }
    return std::move(started).value();
}

/// How the process ended, waiting up to the long limit.
exit_status wait_for_end(plugin_process& process)
{
    const std::optional<exit_status> status = process.wait(long_limit);
    REQUIRE(status.has_value());
    return *status;
}

/// Reads from the handle until `done(text)` holds, the stream ends or the
/// long limit passes.  Everything read is returned.
template <class Done>
std::string read_while_not(native_handle handle, Done done)
{
    const auto deadline = steady::now() + long_limit;
    std::string text;
    while (!done(text))
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - steady::now());
        if (left.count() <= 0)
            break;
        const std::optional<std::string> part = io::read_some(handle, left);
        if (!part.has_value() || part->empty())
            break;
        text += *part;
    }
    return text;
}

std::size_t count_of(const std::string& text, std::string_view part)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos;
         at = text.find(part, at + part.size()))
    {
        ++count;
    }
    return count;
}

/// The lines of `text` that end with a newline, without the line ending.  A
/// last line that has not ended yet is left out.
std::vector<std::string> complete_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(current);
            current.clear();
        }
        else if (c != '\r')
        {
            current += c;
        }
    }
    return lines;
}

/// The lines of `text`, a last line that has not ended included.
std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> lines = complete_lines(text);
    const std::size_t last = text.find_last_of('\n');
    const std::string rest =
        last == std::string::npos ? text : text.substr(last + 1);
    if (!rest.empty())
        lines.push_back(rest);
    return lines;
}

bool has_line(const std::vector<std::string>& lines, const std::string& line)
{
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

std::optional<std::uint64_t> to_number(std::string_view text)
{
    std::uint64_t value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return std::nullopt;
    return value;
}

/// The numbers after `prefix` on the complete lines that start with it.
std::vector<std::uint64_t> numbers_after(const std::string& text,
                                         const std::string& prefix)
{
    std::vector<std::uint64_t> numbers;
    for (const std::string& line : complete_lines(text))
    {
        if (line.rfind(prefix, 0) != 0)
            continue;
        const auto number =
            to_number(std::string_view(line).substr(prefix.size()));
        if (number.has_value())
            numbers.push_back(*number);
    }
    return numbers;
}

/// Waits until no process has the number, up to the long limit.
bool wait_until_gone(std::uint64_t pid)
{
    const auto deadline = steady::now() + long_limit;
    while (io::process_exists(pid))
    {
        if (steady::now() >= deadline)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

std::string hex_of(const std::string& bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    for (const char letter : bytes)
    {
        const auto byte = static_cast<unsigned char>(letter);
        text += digits[byte >> 4];
        text += digits[byte & 0x0f];
    }
    return text;
}

/// A number the test holds open and that is not a number the plugin's own
/// handles are likely to have: the plugin's standard streams and channel take
/// the first four descriptors.
class held_handles
{
public:
    held_handles() = default;
    ~held_handles()
    {
        for (const std::intptr_t handle : handles_)
            io::close_handle(handle);
    }

    held_handles(const held_handles&) = delete;
    held_handles& operator=(const held_handles&) = delete;

    /// A handle made inheritable whose number is above 3.
    std::intptr_t open()
    {
        for (int tries = 0; tries < 8; ++tries)
        {
            const std::intptr_t handle = io::open_inheritable_handle();
            REQUIRE(handle != -1);
            handles_.push_back(handle);
            if (handle > 3)
                return handle;
        }
        throw vss_test::failure("no handle above 3 could be opened");
    }

private:
    std::vector<std::intptr_t> handles_;
};

/// The text as narrow characters, for a failure message.
std::string narrow(const std::wstring& text)
{
    std::string out;
    for (const wchar_t letter : text)
        out += letter < 128 ? static_cast<char>(letter) : '?';
    return out;
}

void check_command_line(const std::vector<std::wstring>& words,
                        const std::wstring& expected)
{
    const std::wstring made =
        vss::plugin::host::detail::windows_command_line(words);
    if (made != expected)
    {
        throw vss_test::failure("the command line is [" + narrow(made) +
                                "] and should be [" + narrow(expected) + "]");
    }
}

void expect_refused(const process_request& request)
{
    const auto started = plugin_process::start(request);
    REQUIRE(started.has_error());
    REQUIRE(started.error() == vss::plugin::make_error_code(
                                   vss::plugin::plugin_errc::start_failed));
}

} // namespace

TEST_CASE("plugin process: a plugin that prints and exits")
{
    const scratch_directory scratch;
    plugin_process process = start_process(request_for(
        scratch, {"--stdout=printed-text", "--stderr=error-text", "--exit=7"}));
    REQUIRE(process.output() != no_handle);

    const std::string output = io::read_to_end(process.output(), long_limit);
    const std::vector<std::string> lines = lines_of(output);
    REQUIRE(has_line(lines, "printed-text"));
    REQUIRE(has_line(lines, "error-text"));

    const exit_status status = wait_for_end(process);
    REQUIRE(!status.abnormal);
    REQUIRE_EQ(status.code, 7);

    const std::optional<exit_status> again = process.poll();
    REQUIRE(again.has_value());
    REQUIRE(!again->abnormal);
    REQUIRE_EQ(again->code, 7);
}

TEST_CASE("plugin process: arguments arrive exactly as written")
{
    const std::vector<std::string> awkward = {
        "",           "a b",      "\"quoted\"",   "back\\slash",
        "trailing\\", "a\\\"b",   "\\\\\"",       "%PATH%",
        "$HOME",      "^&|<>",    "tab\there",    "it's",
        "--x=y z",    "ends\\\\", "with space\\", "two  spaces",
        "\"",         "x y\\\"z",
    };
    std::vector<std::string> arguments = {"--print-arguments"};
    arguments.insert(arguments.end(), awkward.begin(), awkward.end());

    const scratch_directory scratch;
    plugin_process process = start_process(request_for(scratch, arguments));
    const std::string output = io::read_to_end(process.output(), long_limit);
    const std::vector<std::string> lines = lines_of(output);

    // The test plugin counts every argument after its own name, the one that
    // asks for the list included, and exits after printing.
    REQUIRE_EQ(lines.size(), arguments.size() + 1);
    REQUIRE_EQ(lines[0], "argc=" + std::to_string(arguments.size()));
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string expected = "arg " + std::to_string(index + 1) + ": " +
                                     hex_of(arguments[index]);
        REQUIRE_EQ(lines[index + 1], expected);
    }

    const exit_status status = wait_for_end(process);
    REQUIRE(!status.abnormal);
    REQUIRE_EQ(status.code, 0);
}

TEST_CASE("plugin process: the environment is the one given and the channel")
{
    const scratch_directory scratch;
    process_request request = request_for(scratch, {"--print-environment"});
    request.environment.emplace_back("VSS_TEST_MARK", "yes");

    plugin_process process = start_process(request);
    const std::string output = io::read_to_end(process.output(), long_limit);
    const std::vector<std::string> lines = lines_of(output);
    REQUIRE(!lines.empty());
    REQUIRE_EQ(lines.back(), "env-end");
    REQUIRE(has_line(lines, "env VSS_TEST_MARK=yes"));
    REQUIRE(has_line(lines, "env VSS_PLUGIN_ID=com.example.test"));

    std::vector<std::pair<std::string, std::string>> seen;
    for (const std::string& line : lines)
    {
        if (line.rfind("env ", 0) != 0)
            continue;
        const std::string entry = line.substr(4);
        const std::size_t equals = entry.find('=');
        REQUIRE(equals != std::string::npos);
        seen.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
    }

    // Everything that was asked for arrived with its value.
    for (const auto& [name, value] : request.environment)
    {
        const auto found =
            std::find(seen.begin(), seen.end(), std::make_pair(name, value));
        if (found == seen.end())
            throw vss_test::failure("the variable did not arrive: " + name);
    }

    // Nothing else did, except the channel and what macOS adds itself.
    std::size_t channels = 0;
    for (const auto& variable : seen)
    {
        const std::string& name = variable.first;
        if (name == "VSS_PLUGIN_CHANNEL")
        {
            ++channels;
            REQUIRE(to_number(variable.second).has_value());
            continue;
        }
        bool asked = false;
        for (const auto& wanted : request.environment)
            asked = asked || wanted.first == name;
        const bool added_by_system = name.rfind("__CF", 0) == 0;
        if (!asked && !added_by_system)
            throw vss_test::failure("an unexpected variable: " + name);
    }
    REQUIRE_EQ(channels, std::size_t{1});

    static_cast<void>(wait_for_end(process));
}

TEST_CASE("plugin process: the working directory is the one asked for")
{
    const scratch_directory scratch;
    plugin_process process =
        start_process(request_for(scratch, {"--print-directory"}));
    const std::string output = io::read_to_end(process.output(), long_limit);

    std::optional<std::string> printed;
    for (const std::string& line : lines_of(output))
    {
        if (line.rfind("cwd ", 0) == 0)
            printed = line.substr(4);
    }
    REQUIRE(printed.has_value());

    std::error_code ec;
    const bool same = fs::equivalent(fs::path(*printed), scratch.path(), ec);
    if (ec)
    {
        throw vss_test::failure("the directories cannot be compared: " +
                                ec.message() + " for " + *printed);
    }
    if (!same)
    {
        throw vss_test::failure("the plugin started in " + *printed +
                                " and not in " + scratch.path().string());
    }
    static_cast<void>(wait_for_end(process));
}

TEST_CASE("plugin process: standard input is at its end")
{
    const scratch_directory scratch;
    plugin_process process =
        start_process(request_for(scratch, {"--read-stdin"}));
    const std::string output = io::read_to_end(process.output(), long_limit);
    REQUIRE(has_line(lines_of(output), "stdin 0"));

    const exit_status status = wait_for_end(process);
    REQUIRE(!status.abnormal);
    REQUIRE_EQ(status.code, 0);
}

TEST_CASE("plugin process: the channel carries bytes both ways")
{
    const scratch_directory scratch;
    plugin_process process = start_process(
        request_for(scratch, {"--channel-say=hello", "--channel-echo"}));
    const native_handle channel = process.channel();
    REQUIRE(channel != no_handle);

    const std::string said = io::read_until(channel, "hello", long_limit);
    REQUIRE(said.find("hello") != std::string::npos);

    REQUIRE(io::write_all(channel, "ping-1", long_limit));
    const std::string echoed = io::read_until(channel, "ping-1", long_limit);
    REQUIRE(echoed.find("ping-1") != std::string::npos);

    // Closing the host's end is the end of the stream for the plugin.
    io::close_handle(process.take_channel());
    REQUIRE(process.channel() == no_handle);
    const std::string output = io::read_to_end(process.output(), long_limit);
    REQUIRE(has_line(lines_of(output), "channel-end"));

    const exit_status status = wait_for_end(process);
    REQUIRE(!status.abnormal);
    REQUIRE_EQ(status.code, 0);
}

TEST_CASE("plugin process: no handle of the host reaches the plugin")
{
    held_handles held;
    const std::intptr_t mine = held.open();

    const scratch_directory scratch;
    plugin_process process = start_process(
        request_for(scratch, {"--probe-handle=" + std::to_string(mine),
                              "--list-descriptors"}));
    const std::string output = io::read_to_end(process.output(), long_limit);
    const std::vector<std::string> lines = lines_of(output);

    REQUIRE(has_line(lines, "handle " + std::to_string(mine) + " closed"));

    // Where the plugin can list its descriptors, it holds the three standard
    // ones and the channel and nothing else.
    std::optional<std::string> listed;
    for (const std::string& line : lines)
    {
        if (line.rfind("descriptors ", 0) == 0)
            listed = line;
    }
    REQUIRE(listed.has_value());
    if (*listed != "descriptors unsupported")
        REQUIRE_EQ(*listed, "descriptors 0,1,2,3");

    static_cast<void>(wait_for_end(process));
}

TEST_CASE("plugin process: a crash is reported as abnormal")
{
    const scratch_directory scratch;
    plugin_process process = start_process(request_for(scratch, {"--crash"}));
    const exit_status status = wait_for_end(process);
    REQUIRE(status.abnormal);

    static_cast<void>(io::read_to_end(process.output(), long_limit));
    const std::optional<std::string> end =
        io::read_some(process.output(), 1000ms);
    REQUIRE(end.has_value());
    REQUIRE(end->empty());
}

TEST_CASE("plugin process: kill ends a plugin that hangs")
{
    const scratch_directory scratch;
    plugin_process process = start_process(request_for(scratch, {"--hang"}));
    const std::string output =
        io::read_until(process.output(), "hanging\n", long_limit);
    REQUIRE(output.find("hanging\n") != std::string::npos);
    REQUIRE(!process.poll().has_value());

    process.kill();
    const exit_status status = wait_for_end(process);
    REQUIRE(status.abnormal);

    // Killing what is already gone does nothing.
    process.kill();
    process.kill();
}

TEST_CASE("plugin process: kill ends the children of a plugin too")
{
    const scratch_directory scratch;
    plugin_process process =
        start_process(request_for(scratch, {"--spawn-children=2", "--hang"}));

    // Two children and the plugin itself say "hanging".
    const std::string output =
        read_while_not(process.output(), [](const std::string& text)
                       { return count_of(text, "hanging\n") >= 3; });
    const std::vector<std::uint64_t> children = numbers_after(output, "child ");
    REQUIRE_EQ(children.size(), std::size_t{2});
    for (const std::uint64_t pid : children)
        REQUIRE(io::process_exists(pid));

    process.kill();
    const exit_status status = wait_for_end(process);
    REQUIRE(status.abnormal);

    for (const std::uint64_t pid : children)
        REQUIRE(wait_until_gone(pid));
}

TEST_CASE("plugin process: children left behind by a plugin that exits end")
{
    const scratch_directory scratch;
    plugin_process process =
        start_process(request_for(scratch, {"--spawn-children=1", "--exit=0"}));

    const std::string output =
        read_while_not(process.output(), [](const std::string& text)
                       { return !numbers_after(text, "child ").empty(); });
    const std::vector<std::uint64_t> children = numbers_after(output, "child ");
    REQUIRE_EQ(children.size(), std::size_t{1});

    const exit_status status = wait_for_end(process);
    REQUIRE(!status.abnormal);
    REQUIRE_EQ(status.code, 0);

    REQUIRE(wait_until_gone(children[0]));

    // With the child gone nothing holds the output pipe, so it has an end.
    static_cast<void>(io::read_to_end(process.output(), long_limit));
    const std::optional<std::string> end =
        io::read_some(process.output(), 1000ms);
    REQUIRE(end.has_value());
    REQUIRE(end->empty());
}

TEST_CASE("plugin process: the destructor ends a plugin")
{
    const scratch_directory scratch;
    std::uint64_t pid = 0;
    {
        plugin_process process =
            start_process(request_for(scratch, {"--pid", "--hang"}));
        const std::string output =
            io::read_until(process.output(), "hanging\n", long_limit);
        const std::vector<std::uint64_t> numbers =
            numbers_after(output, "pid ");
        REQUIRE_EQ(numbers.size(), std::size_t{1});
        pid = numbers[0];
        REQUIRE_EQ(process.id(), pid);
        REQUIRE(io::process_exists(pid));
    }
    REQUIRE(wait_until_gone(pid));
}

TEST_CASE("plugin process: a program that does not exist does not start")
{
    const scratch_directory scratch;
    process_request request = request_for(scratch, {});
    request.program = scratch.path() / "no-such-program";

    const auto started = plugin_process::start(request);
    REQUIRE(started.has_error());
}

TEST_CASE("plugin process: a request that breaks a rule is refused")
{
    const scratch_directory scratch;
    const process_request good = request_for(scratch, {"--exit=0"});

    {
        process_request request = good;
        request.program = fs::path("relative") / "program";
        expect_refused(request);
    }
    {
        process_request request = good;
        request.arguments.push_back(std::string("a\0b", 3));
        expect_refused(request);
    }
    {
        process_request request = good;
        request.environment.emplace_back("NAME=WITH", "value");
        expect_refused(request);
    }
    {
        process_request request = good;
        request.environment.emplace_back("", "value");
        expect_refused(request);
    }
    {
        process_request request = good;
        request.environment.emplace_back(std::string("NA\0ME", 5), "value");
        expect_refused(request);
    }
    {
        process_request request = good;
        request.environment.emplace_back("VALUE_WITH_ZERO",
                                         std::string("a\0b", 3));
        expect_refused(request);
    }
    {
        process_request request = good;
        request.environment.emplace_back("TWICE", "one");
        request.environment.emplace_back("TWICE", "two");
        expect_refused(request);
    }
    {
        process_request request = good;
        request.environment.emplace_back("VSS_PLUGIN_CHANNEL", "3");
        expect_refused(request);
    }
    {
        process_request request = good;
        request.working_directory = fs::path("relative-directory");
        expect_refused(request);
    }

    // The same request with no fault starts, so the refusals were for the
    // faults.
    plugin_process process = start_process(good);
    const exit_status status = wait_for_end(process);
    REQUIRE(!status.abnormal);
    REQUIRE_EQ(status.code, 0);
}

TEST_CASE("plugin process: a large output is neither blocked nor lost")
{
    const scratch_directory scratch;
    plugin_process process = start_process(
        request_for(scratch, {"--flood-stdout=1000000", "--stdout=done"}));
    const std::string output = io::read_to_end(process.output(), long_limit);

    const std::size_t crosses =
        static_cast<std::size_t>(std::count(output.begin(), output.end(), 'x'));
    REQUIRE(crosses >= std::size_t{1000000});
    REQUIRE(has_line(lines_of(output), "done"));

    const exit_status status = wait_for_end(process);
    REQUIRE(!status.abnormal);
    REQUIRE_EQ(status.code, 0);
}

TEST_CASE("plugin process: the Windows command line quotes what needs it")
{
    // A word with nothing in it that needs quotes is written as it is.
    check_command_line({L"prog"}, L"prog");
    check_command_line({L"prog", L"plain", L"back\\slash"},
                       L"prog plain back\\slash");

    // A space makes quotes, and so does an empty word.
    check_command_line({L"prog", L"a b"}, L"prog \"a b\"");
    check_command_line({L"prog", L""}, L"prog \"\"");

    // A tab, a newline and a vertical tab make quotes as well.
    check_command_line({L"prog", L"a\tb"}, L"prog \"a\tb\"");
    check_command_line({L"prog", L"a\nb"}, L"prog \"a\nb\"");
    check_command_line({L"prog", L"a\vb"}, L"prog \"a\vb\"");

    // A double quote makes quotes and is escaped: the one backslash before it
    // becomes three (2 * 1 + 1) and the quote itself follows.
    check_command_line({L"prog", L"a\\\"b"},
                       L"prog \"a" + std::wstring(3, L'\\') + L"\"b\"");

    // Two backslashes and a quote: 2 * 2 + 1 backslashes, then the quote.
    check_command_line({L"prog", L"\\\\\""},
                       L"prog \"" + std::wstring(5, L'\\') + L"\"\"");

    // A quote in the middle of a word with a space.
    check_command_line({L"prog", L"x y\\\"z"},
                       L"prog \"x y" + std::wstring(3, L'\\') + L"\"z\"");

    // Backslashes at the end of a word are left as they are when it is not
    // quoted and doubled when it is, so that they do not escape the closing
    // quote.
    check_command_line({L"prog", L"trailing\\"}, L"prog trailing\\");
    check_command_line({L"prog", L"trail ing\\"},
                       L"prog \"trail ing" + std::wstring(2, L'\\') + L"\"");
    check_command_line({L"prog", L"a b\\\\"},
                       L"prog \"a b" + std::wstring(4, L'\\') + L"\"");

    // Backslashes that are not before a quote stay as they are.
    check_command_line({L"prog", L"a b\\c"}, L"prog \"a b\\c\"");
}

TEST_CASE("plugin process: the environment of a plugin")
{
    const scratch_directory scratch;
    const auto environment =
        plugin_environment("com.example.test", scratch.path());

    const auto value_of =
        [&environment](const std::string& name) -> std::optional<std::string>
    {
        for (const auto& [key, value] : environment)
        {
            if (key == name)
                return value;
        }
        return std::nullopt;
    };

    const std::optional<std::string> id = value_of("VSS_PLUGIN_ID");
    REQUIRE(id.has_value());
    REQUIRE_EQ(*id, "com.example.test");

    const std::optional<std::string> data = value_of("VSS_PLUGIN_DATA");
    REQUIRE(data.has_value());
    REQUIRE_EQ(*data, scratch.path().string());

    const std::optional<std::string> home = value_of("HOME");
    REQUIRE(home.has_value());
    REQUIRE_EQ(*home, scratch.path().string());

    const std::optional<std::string> path = value_of("PATH");
    REQUIRE(path.has_value());
    REQUIRE(!path->empty());

    REQUIRE(value_of("LANG").has_value());

    // The channel is added when the plugin starts, never by the caller.
    REQUIRE(!value_of("VSS_PLUGIN_CHANNEL").has_value());

    for (std::size_t first = 0; first < environment.size(); ++first)
    {
        REQUIRE(!environment[first].first.empty());
        for (std::size_t second = first + 1; second < environment.size();
             ++second)
        {
            if (environment[first].first == environment[second].first)
            {
                throw vss_test::failure("a name comes twice: " +
                                        environment[first].first);
            }
        }
    }
}

namespace
{

sandbox_profile profile_for(const scratch_directory& scratch)
{
    sandbox_profile profile;
    profile.name = "com.example.test";
    profile.plugin_directory = scratch.path() / "plugin";
    profile.data_directory = scratch.path() / "data";
    return profile;
}

/// A request for the test plugin as an installed plugin is laid out: the
/// program copied into a directory of its own, a data directory beside it,
/// and a profile that names the two.
process_request confined_request(const scratch_directory& scratch,
                                 std::vector<std::string> arguments)
{
    const sandbox_profile profile = profile_for(scratch);
    std::error_code ec;
    std::filesystem::create_directories(profile.plugin_directory, ec);
    std::filesystem::create_directories(profile.data_directory, ec);
    const std::filesystem::path program =
        profile.plugin_directory /
        std::filesystem::path(VSS_TEST_PLUGIN_PROGRAM).filename();
    std::filesystem::copy_file(
        VSS_TEST_PLUGIN_PROGRAM, program,
        std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE(!ec);

    process_request request;
    request.program = program;
    request.arguments = std::move(arguments);
    request.working_directory = profile.data_directory;
    request.environment =
        plugin_environment("com.example.test", profile.data_directory);
    request.sandbox = profile;
    return request;
}

} // namespace

TEST_CASE("plugin process: the level of a sandbox follows what was applied")
{
    sandbox_report report;
    REQUIRE(report.level() == sandbox_level::none);
    REQUIRE_EQ(report.not_applied().size(), std::size(all_sandbox_parts));

    // Limits, the user interface and containment do not keep a plugin from
    // the files of the user, so they are no sandbox.
    report.applied = {sandbox_part::containment, sandbox_part::limits,
                      sandbox_part::user_interface};
    REQUIRE(report.level() == sandbox_level::none);

    // A closed file system without containment is none either.
    report.applied = {sandbox_part::file_system, sandbox_part::network,
                      sandbox_part::limits};
    REQUIRE(report.level() == sandbox_level::none);

    report.applied = {sandbox_part::containment, sandbox_part::file_system};
    REQUIRE(report.level() == sandbox_level::partial);
    REQUIRE(!report.has(sandbox_part::network));
    REQUIRE_EQ(report.not_applied().size(), std::size_t{5});
    REQUIRE(report.not_applied().front() == sandbox_part::network);

    report.applied.assign(std::begin(all_sandbox_parts),
                          std::end(all_sandbox_parts));
    REQUIRE(report.level() == sandbox_level::full);
    REQUIRE(report.not_applied().empty());

    for (const sandbox_part part : all_sandbox_parts)
        REQUIRE(to_string(part) != "unknown");
}

TEST_CASE("plugin process: a start reports what confines the plugin")
{
    const scratch_directory scratch;
    process_request request = request_for(scratch, {"--exit=0"});

    // Without a profile nothing is claimed beyond what always holds.
    plugin_process plain = start_process(request);
    REQUIRE(!plain.sandbox().has(sandbox_part::file_system));
    REQUIRE(!plain.sandbox().has(sandbox_part::limits));
    REQUIRE(plain.sandbox().level() == sandbox_level::none);
    static_cast<void>(wait_for_end(plain));

    // With one, a start applies what this computer says it can.
    plugin_process confined =
        start_process(confined_request(scratch, {"--exit=0"}));
    REQUIRE(!confined.sandbox().failure);
    const sandbox_report can = available_sandbox();
    for (const sandbox_part part : all_sandbox_parts)
        REQUIRE_EQ(confined.sandbox().has(part), can.has(part));
    REQUIRE(confined.sandbox().level() == can.level());
    const exit_status status = wait_for_end(confined);
    REQUIRE(!status.abnormal);
    REQUIRE_EQ(status.code, 0);
}

TEST_CASE("plugin process: a plugin that may not start programs cannot")
{
    if (!available_sandbox().has(sandbox_part::limits))
        VSS_SKIP("this system does not limit the processes of a plugin yet");
    const scratch_directory scratch;
    process_request request =
        confined_request(scratch, {"--spawn-children=1", "--stdout=after"});
    plugin_process process = start_process(request);
    REQUIRE(process.sandbox().has(sandbox_part::limits));

    const std::string output = io::read_to_end(process.output(), long_limit);
    REQUIRE(numbers_after(output, "child ").empty());
    REQUIRE(count_of(output, "a child could not be started") == 1);
    const exit_status status = wait_for_end(process);
    REQUIRE(!status.abnormal);
    REQUIRE_EQ(status.code, 5);
}

TEST_CASE("plugin process: a plugin that may start programs is held to its "
          "number of processes")
{
    if (!available_sandbox().has(sandbox_part::limits))
        VSS_SKIP("this system does not limit the processes of a plugin yet");
    const scratch_directory scratch;
    process_request request =
        confined_request(scratch, {"--spawn-children=5", "--hang"});
    request.sandbox->start_programs = true;
    request.sandbox->limits.processes = 3;
    plugin_process process = start_process(request);

    // The plugin and two children fit; the third child does not, and the
    // plugin exits.
    const std::string output =
        read_while_not(process.output(), [](const std::string& text)
                       { return count_of(text, "could not be started") >= 1; });
    REQUIRE_EQ(numbers_after(output, "child ").size(), std::size_t{2});
    const exit_status status = wait_for_end(process);
    REQUIRE_EQ(status.code, 5);
}

TEST_CASE("plugin process: a plugin is held to its memory")
{
    if (!available_sandbox().has(sandbox_part::limits))
        VSS_SKIP("this system does not limit the memory of a plugin yet");
    const scratch_directory scratch;
    process_request request =
        confined_request(scratch, {"--allocate=32", "--allocate=512"});
    request.sandbox->limits.memory_bytes = 128ULL * 1024 * 1024;
    plugin_process process = start_process(request);

    // 32 MiB fit below 128 MiB.  512 more do not: the plugin is refused
    // the memory, or is ended, before it has them.
    const std::string output = io::read_to_end(process.output(), long_limit);
    REQUIRE(count_of(output, "allocated 32\n") == 1);
    REQUIRE(count_of(output, "allocated 512\n") == 0);
    static_cast<void>(wait_for_end(process));
}

TEST_CASE("plugin process: a confined plugin reaches its own directories "
          "and no other")
{
    if (!available_sandbox().has(sandbox_part::file_system))
        VSS_SKIP("this system does not close the file system to a plugin yet");
    const scratch_directory scratch;
    // A file of the user that no profile names.
    const std::filesystem::path secret = scratch.path() / "secret.txt";
    {
        std::ofstream out(secret, std::ios::binary);
        out << "not for a plugin";
    }
    const std::filesystem::path inside = scratch.path() / "data" / "kept.txt";
    const std::filesystem::path outside = scratch.path() / "made.txt";

    process_request request = confined_request(
        scratch,
        {"--read-file=" + secret.string(), "--write-file=" + outside.string(),
         "--write-file=" + inside.string(), "--read-file=" + inside.string(),
         "--stdout=done"});
    request.least = sandbox_level::partial;
    plugin_process process = start_process(request);
    REQUIRE(process.sandbox().has(sandbox_part::file_system));
    REQUIRE(process.sandbox().level() >= sandbox_level::partial);

    const std::string output = io::read_to_end(process.output(), long_limit);
    const std::vector<std::string> lines = lines_of(output);
    REQUIRE(has_line(lines, "done"));
    // The file outside was not read and no file was made outside.
    REQUIRE_EQ(count_of(output, "read-failed\n"), std::size_t{1});
    REQUIRE_EQ(count_of(output, "write-failed\n"), std::size_t{1});
    REQUIRE(!std::filesystem::exists(outside));
    // Its own data directory works.
    REQUIRE_EQ(count_of(output, "written\n"), std::size_t{1});
    REQUIRE(has_line(lines, "read 26"));
    REQUIRE(std::filesystem::exists(inside));
    const exit_status status = wait_for_end(process);
    REQUIRE(!status.abnormal);
}

TEST_CASE("plugin process: the same plugin without a profile reads the file")
{
    // The check of the test above: what it refuses is refused by the
    // sandbox and not by the way the test is written.
    const scratch_directory scratch;
    const std::filesystem::path secret = scratch.path() / "secret.txt";
    {
        std::ofstream out(secret, std::ios::binary);
        out << "not for a plugin";
    }
    plugin_process process =
        start_process(request_for(scratch, {"--read-file=" + secret.string()}));
    const std::string output = io::read_to_end(process.output(), long_limit);
    REQUIRE(has_line(lines_of(output), "read 16"));
    static_cast<void>(wait_for_end(process));
}

TEST_CASE("plugin process: a start that cannot reach the level asked for "
          "starts nothing")
{
    const scratch_directory scratch;
    process_request request = confined_request(
        scratch,
        {"--write-file=" + (scratch.path() / "data" / "ran.txt").string()});
    request.least = sandbox_level::full;
    auto started = plugin_process::start(request);
    if (available_sandbox().level() == sandbox_level::full)
    {
        REQUIRE(started.has_value());
        REQUIRE(started.value().sandbox().level() == sandbox_level::full);
        return;
    }
    REQUIRE(started.has_error());
    REQUIRE(started.error() ==
            make_error_code(vss::plugin::plugin_errc::sandbox_insufficient));
    REQUIRE(!std::filesystem::exists(scratch.path() / "data" / "ran.txt"));
}

TEST_CASE("plugin process: a confined plugin has no network and no other "
          "process")
{
    // Something on this computer that listens, and this process itself.
    boost::asio::io_context context;
    boost::asio::ip::tcp::acceptor listener(
        context, boost::asio::ip::tcp::endpoint(
                     boost::asio::ip::address_v4::loopback(), 0));
    const std::string connect =
        "--connect=" + std::to_string(listener.local_endpoint().port());
    const std::string open =
        "--open-process=" + std::to_string(io::own_process_id());

    // Without a profile the plugin reaches both, which shows that the
    // test asks for something real.
    {
        const scratch_directory scratch;
        plugin_process process =
            start_process(request_for(scratch, {connect, open}));
        const std::vector<std::string> lines =
            lines_of(io::read_to_end(process.output(), long_limit));
        REQUIRE(has_line(lines, "connected"));
        REQUIRE(has_line(lines, "process-opened"));
        static_cast<void>(wait_for_end(process));
    }

    const sandbox_report can = available_sandbox();
    if (!can.has(sandbox_part::network) ||
        !can.has(sandbox_part::other_processes))
        VSS_SKIP("this system does not close the network to a plugin yet");
    const scratch_directory scratch;
    plugin_process process =
        start_process(confined_request(scratch, {connect, open}));
    REQUIRE(process.sandbox().has(sandbox_part::network));
    REQUIRE(process.sandbox().has(sandbox_part::other_processes));
    const std::vector<std::string> lines =
        lines_of(io::read_to_end(process.output(), long_limit));
    REQUIRE(has_line(lines, "connect-failed"));
    REQUIRE(has_line(lines, "process-refused"));
    static_cast<void>(wait_for_end(process));
}
