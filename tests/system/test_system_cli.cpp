//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli` as a separate process, the way a script uses it: what it prints on
// standard output and on standard error, its exit codes, the options it
// refuses, and the commands that stay until they are stopped.  The command line
// parser and the JSON of `devices --json` are also called directly.
//

#include <vss/ports/virtual_port.hpp>

#include "command_line.hpp"
#include "devices_json.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/json.hpp>

#include "child_process.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using vss_test_support::child_process;
using vss_test_support::scratch_directory;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr std::chrono::milliseconds long_limit{30000};

/// How a command ended, and what it printed where.
struct cli_result
{
    std::optional<int> code;
    std::string out;
    std::string err;
};

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, std::string_view content)
{
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    REQUIRE(!out.fail());
}

/// Runs `vss_cli` with `words` to its end.  `err` is its standard error and
/// `out` its standard output; they are not mixed.
cli_result run_cli(const std::vector<std::string>& words,
                   std::chrono::milliseconds limit = long_limit)
{
    scratch_directory scratch;
    const fs::path error_file = scratch.path() / "stderr.txt";
    child_process child(VSS_CLI_PROGRAM, words, error_file.string());
    cli_result result;
    result.out = child.read_all(limit);
    result.code = child.wait(limit);
    result.err = read_file(error_file);
    return result;
}

std::size_t line_count(const std::string& text)
{
    std::size_t lines = 0;
    for (const char c : text)
        lines += c == '\n' ? 1 : 0;
    return lines;
}

bool contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

/// The topology that `run` keeps: two loopback ports and a link.
void write_topology(const fs::path& path)
{
    write_file(
        path,
        R"({"version":1,"ports":[)"
        R"({"name":"a","kind":"virtual","virtual_kind":"loopback"},)"
        R"({"name":"b","kind":"virtual","virtual_kind":"loopback"}],)"
        R"("links":[{"mode":"duplex","sources":["a"],"targets":["b"]}]})");
}

/// A command that must still be running after a while, and that ends cleanly
/// with the exit code 0 when it is told to stop.
void require_alive_until_stopped(const std::vector<std::string>& words,
                                 const std::string& first_line)
{
    child_process child(VSS_CLI_PROGRAM, words);
    const auto line = child.read_line(long_limit);
    REQUIRE(line.has_value());
    REQUIRE(contains(*line, first_line));

    // Without --seconds these used to return as soon as they had printed
    // what they made.
    std::this_thread::sleep_for(1500ms);
    REQUIRE(!child.wait(0ms).has_value());

    if (!child.request_stop())
    {
        child.kill();
        VSS_SKIP("the stop signal could not be delivered; on Windows this "
                 "test needs a console to send CTRL_BREAK to");
    }
    // A second press, a moment later, must not end it before its ports are
    // down: Windows used to end the process then.
    std::this_thread::sleep_for(20ms);
    static_cast<void>(child.request_stop());
    const auto code = child.wait(long_limit);
    REQUIRE(code.has_value());
    REQUIRE_EQ(*code, 0);
}

} // namespace

TEST_CASE("cli: --help and -h print the help on standard output and exit 0")
{
    for (const std::vector<std::string>& words :
         {std::vector<std::string>{"--help"}, std::vector<std::string>{"-h"},
          std::vector<std::string>{"devices", "--help"},
          std::vector<std::string>{"--help", "--nonsense"}})
    {
        const auto result = run_cli(words);
        REQUIRE(result.code.has_value());
        REQUIRE_EQ(*result.code, 0);
        REQUIRE(contains(result.out, "Virtual Serial Studio command line"));
        REQUIRE_EQ(result.err, std::string());
    }
}

TEST_CASE("cli: no command and an unknown command are errors on standard error")
{
    const auto bare = run_cli({});
    REQUIRE(bare.code.has_value());
    REQUIRE_EQ(*bare.code, 2);
    REQUIRE_EQ(bare.out, std::string());
    REQUIRE(contains(bare.err, "Virtual Serial Studio command line"));

    const auto unknown = run_cli({"nonsense"});
    REQUIRE(unknown.code.has_value());
    REQUIRE_EQ(*unknown.code, 2);
    REQUIRE_EQ(unknown.out, std::string());
    REQUIRE_EQ(line_count(unknown.err), std::size_t{1});
    REQUIRE(contains(unknown.err, "nonsense"));
}

TEST_CASE("cli: --version prints the program and its version")
{
    const auto result = run_cli({"--version"});
    REQUIRE(result.code.has_value());
    REQUIRE_EQ(*result.code, 0);
    REQUIRE_EQ(result.err, std::string());
    REQUIRE_EQ(line_count(result.out), std::size_t{1});
    // The program, a space and the build as `vss_gui --version` writes it:
    // v0.1.0-rc.3+build.6083-3de4617, or v0.1.0-dev when git is not there.
    REQUIRE(result.out.starts_with("vss_cli v"));
    const std::string version = result.out.substr(9);
    REQUIRE(version.size() > 2);
    REQUIRE(version[0] >= '0' && version[0] <= '9');
}

TEST_CASE("cli: a mistake in the command line is one line on standard error "
          "and exit code 2")
{
    const std::vector<std::vector<std::string>> mistakes{
        // An option the command does not have, as a typo of --seconds.
        {"run", "lab.json", "--second=60"},
        {"loopback", "--second=60"},
        {"capture", "lab.json", "--out", "run.cap", "--prot", "master"},
        {"devices", "-x"},
        // A value that is not a number where one is needed.
        {"virtual", "--seconds=abc"},
        {"virtual", "--seconds=1.5"},
        {"virtual", "--seconds=-5"},
        {"virtual", "--seconds="},
        {"loopback", "--seconds", "soon"},
        {"share", "serial://COM1", "--listeners=0"},
        {"bus", "--virtual", "many"},
        {"play", "x.cap", "--uri=tcp://127.0.0.1:1", "--speed=fast"},
        {"play", "x.cap", "--uri=tcp://127.0.0.1:1", "--speed=0"},
        // An option that needs a value, and has none.
        {"virtual", "--kind"},
        {"capture", "--out"},
        // A flag that was given a value.
        {"devices", "--json=yes"},
        // A stray word.
        {"loopback", "stray"},
        {"share", "serial://COM1", "stray"},
        {"run", "a.json", "b.json"},
        {"selftest", "now"},
        // Zero seconds is not a time to stay.
        {"loopback", "--seconds=0"},
        {"run", "lab.json", "--seconds", "0"},
        // No command, though there are options.
        {"--second=5"},
        {"--quiet"},
        {"--seconds", "1"},
        // An unknown command is one, whatever else is on the line.
        {"nonsense", "--help"},
        {"nonsense", "--version"},
    };
    for (const auto& words : mistakes)
    {
        const auto result = run_cli(words);
        REQUIRE(result.code.has_value());
        REQUIRE_EQ(*result.code, 2);
        REQUIRE_EQ(result.out, std::string());
        REQUIRE_EQ(line_count(result.err), std::size_t{1});
    }
}

TEST_CASE(
    "cli: --help and --version count only before -- and for a command that "
    "exists")
{
    scratch_directory scratch;

    // After --, --help is a word like another: here the name of a file.
    const auto after = run_cli({"run", "--", "--help"});
    REQUIRE(after.code.has_value());
    REQUIRE_EQ(*after.code, 1);
    REQUIRE_EQ(after.out, std::string());
    REQUIRE(contains(after.err, "--help"));

    // An option before the command, with its value after a space.
    const fs::path topology = scratch.path() / "lab.json";
    write_topology(topology);
    const auto spaced = run_cli({"--seconds", "1", "run", topology.string()});
    REQUIRE(spaced.code.has_value());
    REQUIRE_EQ(*spaced.code, 0);
    REQUIRE(contains(spaced.out, "ports: 2"));
}

TEST_CASE("cli: an option takes its value after a space as well as after an "
          "equals sign")
{
    for (const std::vector<std::string>& words :
         {std::vector<std::string>{"bus", "--virtual", "2", "--kind",
                                   "loopback", "--seconds", "1"},
          std::vector<std::string>{"bus", "--virtual=2", "--kind=loopback",
                                   "--seconds=1"}})
    {
        const auto result = run_cli(words);
        REQUIRE(result.code.has_value());
        REQUIRE_EQ(*result.code, 0);
        REQUIRE(contains(result.out, "bus of 2 members"));
        REQUIRE_EQ(result.err, std::string());
    }
}

TEST_CASE("cli parser: every command reads --name value and --name=value")
{
    const auto* capture = vss_cli::find_command_spec("capture");
    REQUIRE(capture != nullptr);
    const auto spaced = vss_cli::parse_arguments(
        *capture, {"capture", "lab.json", "--out", "run.cap", "--port",
                   "master", "--uri", "serial://COM5", "--seconds", "30"});
    REQUIRE(spaced.ok());
    REQUIRE_EQ(spaced.args.positional.size(), std::size_t{2});
    REQUIRE_EQ(spaced.args.get("--out"), std::string("run.cap"));
    REQUIRE_EQ(spaced.args.get("--port"), std::string("master"));
    REQUIRE_EQ(spaced.args.all("--uri").size(), std::size_t{1});
    REQUIRE_EQ(spaced.args.number("--seconds", 0), 30);

    const auto joined = vss_cli::parse_arguments(
        *capture, {"capture", "lab.json", "--out=run.cap", "--port=master",
                   "--uri=serial://COM5", "--seconds=30"});
    REQUIRE(joined.ok());
    REQUIRE_EQ(joined.args.options, spaced.args.options);

    // An option as the next word is a missing value, not the value.
    const auto missing =
        vss_cli::parse_arguments(*capture, {"capture", "--out", "--seconds=5"});
    REQUIRE(!missing.ok());

    // Words after -- are arguments, whatever they look like.
    const auto after = vss_cli::parse_arguments(
        *vss_cli::find_command_spec("run"), {"run", "--", "--odd.json"});
    REQUIRE(after.ok());
    REQUIRE_EQ(after.args.positional.size(), std::size_t{2});
    REQUIRE_EQ(after.args.positional[1], std::string("--odd.json"));
}

TEST_CASE("cli parser: numbers are read whole or refused")
{
    REQUIRE_EQ(vss_cli::parse_whole_number("60").value_or(-1), 60LL);
    REQUIRE_EQ(vss_cli::parse_whole_number("-5").value_or(0), -5LL);
    REQUIRE(!vss_cli::parse_whole_number("").has_value());
    REQUIRE(!vss_cli::parse_whole_number("abc").has_value());
    REQUIRE(!vss_cli::parse_whole_number("12abc").has_value());
    REQUIRE(!vss_cli::parse_whole_number("1.5").has_value());
    REQUIRE(!vss_cli::parse_whole_number(" 7").has_value());
    REQUIRE(!vss_cli::parse_whole_number("99999999999999999999").has_value());

    REQUIRE(vss_cli::parse_positive_number("4").has_value());
    REQUIRE(vss_cli::parse_positive_number("0.5").has_value());
    REQUIRE(vss_cli::parse_positive_number("2e1").has_value());
    REQUIRE(!vss_cli::parse_positive_number("0").has_value());
    REQUIRE(!vss_cli::parse_positive_number("-1").has_value());
    REQUIRE(!vss_cli::parse_positive_number("fast").has_value());
    REQUIRE(!vss_cli::parse_positive_number("nan").has_value());
    REQUIRE(!vss_cli::parse_positive_number("inf").has_value());
    REQUIRE(!vss_cli::parse_positive_number("0x10").has_value());
    REQUIRE(!vss_cli::parse_positive_number("1e999").has_value());
    REQUIRE(!vss_cli::parse_positive_number("2x").has_value());
}

TEST_CASE("cli: devices --json is one valid JSON document")
{
    const auto result = run_cli({"devices", "--json"});
    REQUIRE(result.code.has_value());
    REQUIRE_EQ(*result.code, 0);
    REQUIRE_EQ(result.err, std::string());

    boost::system::error_code error;
    const json::value document = json::parse(result.out, error);
    REQUIRE(!error);
    REQUIRE(document.is_object());
    REQUIRE(document.as_object().at("devices").is_array());
    REQUIRE(
        document.as_object().at("com0com").as_object().at("found").is_bool());
    for (const auto& device : document.as_object().at("devices").as_array())
    {
        const auto& entry = device.as_object();
        REQUIRE(entry.at("path").is_string());
        REQUIRE(entry.at("description").is_string());
        REQUIRE(entry.at("hardware_id").is_string());
        REQUIRE(entry.at("virtual").is_bool());
    }
}

TEST_CASE("cli: devices --json escapes the names it prints")
{
    vss::serial_device_info device;
    device.path = "COM9";
    device.description =
        "ACME \"Pro\" Serial (COM9) C:\\Drivers\tnew\nline\x01";
    device.hardware_id = "USB\\VID_1234&PID_5678";
    device.virtual_device = true;
    device.com0com = true;
    device.com0com_pair = 3;
    device.com0com_side = "B";

    vss::com0com_installation installation;
    installation.found = true;
    installation.pairs.resize(2);

    const std::string text = vss_cli::devices_json({device}, installation);
    REQUIRE_EQ(line_count(text), std::size_t{1});

    boost::system::error_code error;
    const json::value document = json::parse(text, error);
    REQUIRE(!error);
    const auto& entry =
        document.as_object().at("devices").as_array().at(0).as_object();
    REQUIRE_EQ(std::string(entry.at("description").as_string()),
               device.description);
    REQUIRE_EQ(std::string(entry.at("hardware_id").as_string()),
               device.hardware_id);
    REQUIRE_EQ(entry.at("pair").as_int64(), std::int64_t{3});
    REQUIRE(entry.at("com0com").as_bool());
    REQUIRE_EQ(document.as_object()
                   .at("com0com")
                   .as_object()
                   .at("pairs")
                   .to_number<int>(),
               2);
}

TEST_CASE("cli: loopback stays until it is stopped")
{
    require_alive_until_stopped({"loopback"}, "virtual port");
}

TEST_CASE("cli: run stays until it is stopped")
{
    scratch_directory scratch;
    const fs::path topology = scratch.path() / "lab.json";
    write_topology(topology);
    require_alive_until_stopped({"run", topology.string()}, "ports: 2");
}

TEST_CASE("cli: run ends by itself after --seconds")
{
    scratch_directory scratch;
    const fs::path topology = scratch.path() / "lab.json";
    write_topology(topology);

    const auto result = run_cli({"run", topology.string(), "--seconds=1"});
    REQUIRE(result.code.has_value());
    REQUIRE_EQ(*result.code, 0);
    REQUIRE(contains(result.out, "ports: 2"));
}

TEST_CASE("cli: virtual --com is an error where the driver is not there")
{
    // A COM number is the whole point of the option.  Where the driver is
    // missing, a port with another name would be no answer, so the command
    // fails.  A machine with the driver would make a real port: not here.
    if (vss::supports(vss::virtual_port_kind::windows_driver))
        VSS_SKIP("the Windows driver is installed; it would make a real port");

    const auto result = run_cli({"virtual", "--com=7", "--seconds=1"});
    REQUIRE(result.code.has_value());
    REQUIRE_EQ(*result.code, 1);
    REQUIRE(contains(result.err, "COM numbers need the Virtual Serial Studio "
                                 "driver"));
    REQUIRE(!contains(result.out, "open this from your application"));

    const auto explicit_kind =
        run_cli({"virtual", "--kind=windows-driver", "--com=7", "--seconds=1"});
    REQUIRE(explicit_kind.code.has_value());
    REQUIRE_EQ(*explicit_kind.code, 1);
}

TEST_CASE("cli: virtual --com refuses what cannot be a COM number")
{
    for (const char* option : {"--com=0", "--com=256", "--com=abc", "--com="})
    {
        const auto result = run_cli({"virtual", option, "--seconds=1"});
        REQUIRE(result.code.has_value());
        REQUIRE_EQ(*result.code, 2);
        REQUIRE_EQ(result.out, std::string());
    }
    const auto pty = run_cli({"virtual", "--kind=pty", "--com=7"});
    REQUIRE(pty.code.has_value());
    REQUIRE_EQ(*pty.code, 2);
}

TEST_CASE("cli: devices --json replaces bytes that are not UTF-8")
{
    const std::string replacement = "\xEF\xBF\xBD";

    vss::serial_device_info device;
    device.path = "/dev/ttyS1";
    // A stray byte, a cut sequence, an overlong form, a surrogate, and a
    // letter that is well formed.
    device.description = "bad \xff byte, cut \xe2\x82, overlong \xc0\xaf, "
                         "surrogate \xed\xa0\x80 and fine \xc3\xa9";

    const std::string text =
        vss_cli::devices_json({device}, vss::com0com_installation{});

    boost::system::error_code error;
    const json::value document = json::parse(text, error);
    REQUIRE(!error);
    const std::string expected =
        "bad " + replacement + " byte, cut " + replacement + replacement +
        ", overlong " + replacement + replacement + ", surrogate " +
        replacement + replacement + replacement + " and fine \xc3\xa9";
    REQUIRE_EQ(std::string(document.as_object()
                               .at("devices")
                               .as_array()
                               .at(0)
                               .as_object()
                               .at("description")
                               .as_string()),
               expected);
}

namespace
{

/// A TCP server on the loopback address that takes one connection and then
/// runs `work` with it.  It waits for the connection for a limited time, so a
/// program that never connects makes a test fail and not wait for ever, and it
/// is stopped from its own thread, not by closing the acceptor from another.
class one_connection_server
{
public:
    using behaviour = std::function<void(boost::asio::ip::tcp::socket&,
                                         const std::atomic<bool>&)>;

    one_connection_server(std::size_t receive_buffer, behaviour work)
        : work_(std::move(work))
    {
        boost::system::error_code error;
        acceptor_.open(boost::asio::ip::tcp::v4(), error);
        REQUIRE(!error);
        // A small window keeps the kernel from taking the whole capture.
        acceptor_.set_option(boost::asio::socket_base::receive_buffer_size(
                                 static_cast<int>(receive_buffer)),
                             error);
        acceptor_.bind(boost::asio::ip::tcp::endpoint(
                           boost::asio::ip::address_v4::loopback(), 0),
                       error);
        REQUIRE(!error);
        acceptor_.listen(1, error);
        REQUIRE(!error);
        acceptor_.non_blocking(true, error);
        REQUIRE(!error);
        port_ = acceptor_.local_endpoint().port();
        thread_ = std::thread([this] { serve(); });
    }

    ~one_connection_server()
    {
        stop_.store(true);
        if (thread_.joinable())
            thread_.join();
    }

    one_connection_server(const one_connection_server&) = delete;
    one_connection_server& operator=(const one_connection_server&) = delete;

    std::uint16_t port() const { return port_; }

    /// Waits for the connection to end.
    void finish()
    {
        if (thread_.joinable())
            thread_.join();
    }

private:
    void serve()
    {
        boost::asio::ip::tcp::socket socket(io_);
        const auto give_up = std::chrono::steady_clock::now() + 20s;
        for (;;)
        {
            boost::system::error_code error;
            acceptor_.accept(socket, error);
            if (!error)
                break;
            if (error != boost::asio::error::would_block &&
                error != boost::asio::error::try_again)
                return;
            if (stop_.load() || std::chrono::steady_clock::now() >= give_up)
                return;
            std::this_thread::sleep_for(10ms);
        }
        work_(socket, stop_);
    }

    behaviour work_;
    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_{io_};
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/// What a reader that starts late has taken in: all the bytes, and how many of
/// them were `mark`.
struct reader_counts
{
    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> marked{0};
};

/// Waits `delay`, then reads until the client closes the connection.
one_connection_server::behaviour slow_reader(std::chrono::milliseconds delay,
                                             reader_counts& counts, char mark)
{
    return [delay, &counts, mark](boost::asio::ip::tcp::socket& socket,
                                  const std::atomic<bool>& stop)
    {
        const auto start = std::chrono::steady_clock::now();
        while (!stop.load() && std::chrono::steady_clock::now() - start < delay)
            std::this_thread::sleep_for(10ms);
        std::vector<char> buffer(64 * 1024);
        boost::system::error_code error;
        while (!stop.load())
        {
            const std::size_t got = socket.read_some(
                boost::asio::buffer(buffer.data(), buffer.size()), error);
            counts.total.fetch_add(got);
            for (std::size_t index = 0; index < got; ++index)
            {
                if (buffer[index] == mark)
                    counts.marked.fetch_add(1);
            }
            if (error)
                break;
        }
    };
}

/// Sends `size` bytes of `mark` every 50 ms until the connection ends.
one_connection_server::behaviour chatty_sender(std::size_t size, char mark)
{
    return [size, mark](boost::asio::ip::tcp::socket& socket,
                        const std::atomic<bool>& stop)
    {
        const std::vector<char> chunk(size, mark);
        while (!stop.load())
        {
            boost::system::error_code error;
            boost::asio::write(
                socket, boost::asio::buffer(chunk.data(), chunk.size()), error);
            if (error)
                return;
            std::this_thread::sleep_for(50ms);
        }
    };
}

/// A capture of `records` records of `size` bytes `mark`, `step` ms apart.
std::string capture_text(std::size_t records, std::size_t size, char mark,
                         int step)
{
    char hex[8];
    std::snprintf(hex, sizeof hex, "0x%02X", static_cast<unsigned>(mark));
    std::string body;
    body.reserve(size * 5);
    for (std::size_t index = 0; index < size; ++index)
    {
        if (index != 0)
            body += '|';
        body += hex;
    }

    std::string text = "# a capture of the test\n";
    for (std::size_t index = 0; index < records; ++index)
    {
        text += "[" + std::to_string(static_cast<int>(index) * step) + "] R ";
        text += body;
        text += '\n';
    }
    return text;
}

} // namespace

TEST_CASE("cli: play waits until the port has written what was queued")
{
    constexpr std::size_t records = 4;
    constexpr std::size_t size = 512 * 1024;
    scratch_directory scratch;
    const fs::path capture = scratch.path() / "big.cap";
    write_file(capture, capture_text(records, size, 'A', 0));

    reader_counts counts;
    one_connection_server server(4096, slow_reader(2000ms, counts, 'A'));
    const fs::path error_file = scratch.path() / "stderr.txt";
    child_process player(
        VSS_CLI_PROGRAM,
        {"play", capture.string(),
         "--uri=tcp://127.0.0.1:" + std::to_string(server.port())},
        error_file.string());
    const std::string out = player.read_all(long_limit * 2);
    const auto code = player.wait(long_limit * 2);
    server.finish();

    // Every byte the command says it sent reached the other end, and the
    // command did not end before they had.
    REQUIRE(code.has_value());
    REQUIRE_EQ(*code, 0);
    REQUIRE_EQ(read_file(error_file), std::string());
    REQUIRE_EQ(counts.total.load(), std::uint64_t{records * size});
    REQUIRE(contains(out, "sent 4 record(s), " +
                              std::to_string(records * size) + " byte(s)"));
}

TEST_CASE("cli: play counts its own writes when a link feeds the same port")
{
    constexpr std::size_t records = 4;
    constexpr std::size_t size = 512 * 1024;
    scratch_directory scratch;
    const fs::path capture = scratch.path() / "mixed.cap";
    write_file(capture, capture_text(records, size, 'A', 300));

    // The port `t` is written by the player, and by the link from `u`, whose
    // far end sends 'B' all the time.  The far end of `t` reads late.
    reader_counts counts;
    one_connection_server target(4096, slow_reader(2000ms, counts, 'A'));
    one_connection_server feeder(65536, chatty_sender(16 * 1024, 'B'));
    const fs::path topology = scratch.path() / "mixed.json";
    write_file(topology,
               R"({"version":1,"ports":[)"
               R"({"name":"t","kind":"tcp","uri":"tcp://127.0.0.1:)" +
                   std::to_string(target.port()) +
                   R"("},)"
                   R"({"name":"u","kind":"tcp","uri":"tcp://127.0.0.1:)" +
                   std::to_string(feeder.port()) +
                   R"("}],)"
                   R"("links":[{"mode":"redirect","sources":["u"],)"
                   R"("targets":["t"]}]})");

    const fs::path error_file = scratch.path() / "stderr.txt";
    child_process player(
        VSS_CLI_PROGRAM,
        {"play", capture.string(), topology.string(), "--port=t"},
        error_file.string());
    const std::string out = player.read_all(long_limit * 2);
    const auto code = player.wait(long_limit * 2);
    target.finish();

    // The far end of `t` has every byte of the capture, though the bytes of
    // the link went out between them, and the command says so.
    REQUIRE(code.has_value());
    REQUIRE_EQ(*code, 0);
    REQUIRE_EQ(counts.marked.load(), std::uint64_t{records * size});
    REQUIRE(contains(out, "sent 4 record(s), " +
                              std::to_string(records * size) + " byte(s)"));
}

TEST_CASE("cli: play says so, and exits 1, when the port refuses what it was "
          "given")
{
    // Twenty records of half a megabyte at once are more than the port's
    // queue of eight megabytes; what it refuses is not sent.
    constexpr std::size_t records = 20;
    constexpr std::size_t size = 512 * 1024;
    scratch_directory scratch;
    const fs::path capture = scratch.path() / "flood.cap";
    write_file(capture, capture_text(records, size, 'A', 0));

    reader_counts counts;
    one_connection_server server(4096, slow_reader(1000ms, counts, 'A'));
    const fs::path error_file = scratch.path() / "stderr.txt";
    child_process player(
        VSS_CLI_PROGRAM,
        {"play", capture.string(),
         "--uri=tcp://127.0.0.1:" + std::to_string(server.port())},
        error_file.string());
    // The capture is read before this line; the time counts from here.
    const auto playing = player.read_line(long_limit);
    REQUIRE(playing.has_value());
    const auto begin = std::chrono::steady_clock::now();
    const std::string out =
        *playing + std::string(1, '\n') + player.read_all(long_limit * 2);
    const auto code = player.wait(long_limit * 2);
    server.finish();
    const auto took = std::chrono::steady_clock::now() - begin;

    REQUIRE(code.has_value());
    REQUIRE_EQ(*code, 1);
    REQUIRE(contains(out, "sent "));
    REQUIRE(contains(out, " of " + std::to_string(records * size) +
                              " queued byte(s)"));
    REQUIRE(contains(read_file(error_file), "write(s) failed"));
    // Refused bytes end their wait at once, and the rest is written as the
    // reader takes it: the command does not idle for the stall limit.
    REQUIRE(took < 25s);
    REQUIRE(counts.total.load() < records * size);
}

TEST_CASE("cli: a command whose output has gone away still ends cleanly")
{
    // The reader of a pipe that has gone makes the command's next write fail;
    // it prints its table once more after the stop, and must still end with
    // its ports down and the exit code 0, not die on the signal that a
    // write to a closed pipe raises on Linux and macOS.
    child_process child(VSS_CLI_PROGRAM, {"loopback"});
    const auto line = child.read_line(long_limit);
    REQUIRE(line.has_value());
    child.close_output();
    std::this_thread::sleep_for(300ms);

    if (!child.request_stop())
    {
        child.kill();
        VSS_SKIP("the stop signal could not be delivered; on Windows this "
                 "test needs a console to send CTRL_BREAK to");
    }
    const auto code = child.wait(long_limit);
    REQUIRE(code.has_value());
    REQUIRE_EQ(*code, 0);
}

TEST_CASE("cli: --seconds also limits the wait for a port that takes nothing")
{
    // Half a megabyte at once into a connection nobody reads: the port takes
    // part of it, and --seconds=2 ends the command at about two seconds with
    // what was written, not after the stall limit of five.
    scratch_directory scratch;
    const fs::path capture = scratch.path() / "stuck.cap";
    write_file(capture, capture_text(32, 64 * 1024, 'A', 0));

    reader_counts counts;
    one_connection_server server(4096, slow_reader(60s, counts, 'A'));
    const fs::path error_file = scratch.path() / "stderr.txt";
    child_process player(
        VSS_CLI_PROGRAM,
        {"play", capture.string(),
         "--uri=tcp://127.0.0.1:" + std::to_string(server.port()),
         "--seconds=2"},
        error_file.string());
    const auto playing = player.read_line(long_limit);
    REQUIRE(playing.has_value());
    const auto begin = std::chrono::steady_clock::now();
    const std::string out =
        *playing + std::string(1, '\n') + player.read_all(long_limit);
    const auto code = player.wait(long_limit);
    const auto took = std::chrono::steady_clock::now() - begin;

    REQUIRE(code.has_value());
    REQUIRE_EQ(*code, 0);
    REQUIRE(took < 4500ms);
    REQUIRE(contains(out, " of "));
}
