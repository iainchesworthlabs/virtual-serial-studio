//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli vport` as a separate process, the way a script uses it: start it,
// read the names it prints, open them from outside, stop it with a signal.
// POSIX only; CMake builds this file on Linux and macOS.
//

#include <vss/studio/studio.hpp>
#include <vss/testkit/harness.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

using namespace vss;
using namespace std::chrono_literals;
using vss_test_support::scratch_directory;
namespace fs = std::filesystem;

namespace
{

/// `vss_cli` as a child process with its standard output captured.
class cli_process
{
public:
    explicit cli_process(const std::vector<std::string>& arguments)
    {
        // CMake names the program this test was built with; VSS_CLI overrides
        // it for a binary somewhere else.
        std::string program = testkit::environment("VSS_CLI");
        if (program.empty())
            program = VSS_CLI_PATH;

        int pipes[2] = {-1, -1};
        if (::pipe(pipes) != 0)
            throw vss_test::failure("pipe failed");

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipes[0]);
        posix_spawn_file_actions_addclose(&actions, pipes[1]);

        std::vector<std::string> storage{program};
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& item : storage)
            argv.push_back(item.data());
        argv.push_back(nullptr);

        const int rc = ::posix_spawn(&pid_, program.c_str(), &actions, nullptr,
                                     argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipes[1]);
        if (rc != 0)
        {
            ::close(pipes[0]);
            throw vss_test::failure("posix_spawn failed");
        }
        output_ = pipes[0];
    }

    ~cli_process()
    {
        if (pid_ > 0 && !reaped_)
        {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
        if (output_ >= 0)
            ::close(output_);
    }

    cli_process(const cli_process&) = delete;
    cli_process& operator=(const cli_process&) = delete;

    /// One line of standard output, without its newline; empty on timeout or
    /// at the end of the output.
    std::string read_line(std::chrono::milliseconds limit = 10000ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (;;)
        {
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos)
            {
                std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                return line;
            }

            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                return {};
            pollfd watch{output_, POLLIN, 0};
            if (::poll(&watch, 1, static_cast<int>(left.count())) <= 0)
                return {};
            char chunk[256];
            const auto got = ::read(output_, chunk, sizeof chunk);
            if (got <= 0)
                return {};
            buffer_.append(chunk, static_cast<std::size_t>(got));
        }
    }

    void signal(int number) { ::kill(pid_, number); }

    /// Waits for the child to end.  Returns its exit code, 128 plus the signal
    /// number when a signal killed it, or -1 when it was still running.
    int wait(std::chrono::milliseconds limit = 10000ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (;;)
        {
            int status = 0;
            const pid_t done = ::waitpid(pid_, &status, WNOHANG);
            if (done == pid_)
            {
                reaped_ = true;
                const int signal_number = status & 0x7f;
                return signal_number == 0 ? (status >> 8) & 0xff
                                          : 128 + signal_number;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return -1;
            std::this_thread::sleep_for(20ms);
        }
    }

private:
    pid_t pid_ = -1;
    int output_ = -1;
    bool reaped_ = false;
    std::string buffer_;
};

/// The part of a "<link> -> <slave>" line before the arrow.
std::string link_of(const std::string& line)
{
    const auto arrow = line.find(" -> ");
    return arrow == std::string::npos ? std::string{} : line.substr(0, arrow);
}

bool wait_for_rx(endpoint& port, std::uint64_t bytes)
{
    return testkit::wait_until([&] { return port.stats().bytes_rx >= bytes; },
                               5000ms);
}

std::string stable_dir_argument(const fs::path& directory)
{
    return "--stable-dir=" + directory.string();
}

} // namespace

TEST_CASE("vport: two linked ports carry data between outside programs")
{
    scratch_directory scratch;
    const auto directory = scratch.path() / "links";

    cli_process cli({"vport", "--name=vss-a,vss-b", "--link=vss-a:vss-b",
                     stable_dir_argument(directory)});
    const std::string first = cli.read_line();
    const std::string second = cli.read_line();
    REQUIRE_EQ(link_of(first), (directory / "vss-a").string());
    REQUIRE_EQ(link_of(second), (directory / "vss-b").string());
    REQUIRE_EQ(cli.read_line(), std::string("linked vss-a:vss-b"));

    {
        // This program opens the two names the way any other would.
        studio outside;
        const auto a = outside.attach_serial("a", "serial://" + link_of(first) +
                                                      "?baud=115200");
        const auto b = outside.attach_serial(
            "b", "serial://" + link_of(second) + "?baud=115200");
        REQUIRE(a);
        REQUIRE(b);

        REQUIRE((*a)->write_sync(as_bytes("PING"), 3000ms));
        REQUIRE(wait_for_rx(**b, 4));
        REQUIRE((*b)->write_sync(as_bytes("PONG"), 3000ms));
        REQUIRE(wait_for_rx(**a, 4));
    }

    // A signal ends it cleanly and takes the names with it.
    cli.signal(SIGTERM);
    REQUIRE_EQ(cli.wait(), 0);
    REQUIRE(!fs::exists(fs::symlink_status(directory / "vss-a")));
    REQUIRE(!fs::exists(fs::symlink_status(directory / "vss-b")));
    REQUIRE(fs::is_empty(directory));
}

TEST_CASE("vport: SIGINT also stops it and removes the name")
{
    scratch_directory scratch;
    const auto directory = scratch.path() / "links";

    cli_process cli(
        {"vport", "--name=vss-int", stable_dir_argument(directory)});
    REQUIRE_EQ(link_of(cli.read_line()), (directory / "vss-int").string());
    REQUIRE(fs::is_symlink(directory / "vss-int"));

    cli.signal(SIGINT);
    REQUIRE_EQ(cli.wait(), 0);
    REQUIRE(!fs::exists(fs::symlink_status(directory / "vss-int")));
}

TEST_CASE("vport: SIGHUP, which a closing terminal sends, removes the name too")
{
    scratch_directory scratch;
    const auto directory = scratch.path() / "links";

    cli_process cli(
        {"vport", "--name=vss-hup", stable_dir_argument(directory)});
    REQUIRE_EQ(link_of(cli.read_line()), (directory / "vss-hup").string());
    REQUIRE(fs::is_symlink(directory / "vss-hup"));

    cli.signal(SIGHUP);
    REQUIRE_EQ(cli.wait(), 0);
    REQUIRE(!fs::exists(fs::symlink_status(directory / "vss-hup")));
}

TEST_CASE("vport: the same name works again after the process was killed")
{
    scratch_directory scratch;
    const auto directory = scratch.path() / "links";

    {
        cli_process cli(
            {"vport", "--name=vss-kill", stable_dir_argument(directory)});
        REQUIRE_EQ(link_of(cli.read_line()), (directory / "vss-kill").string());
        cli.signal(SIGKILL);
        REQUIRE_EQ(cli.wait(), 128 + SIGKILL);
    }

    // Nothing cleaned up: the name is still there, pointing at nothing.
    REQUIRE(fs::is_symlink(directory / "vss-kill"));

    cli_process again(
        {"vport", "--name=vss-kill", stable_dir_argument(directory)});
    REQUIRE_EQ(link_of(again.read_line()), (directory / "vss-kill").string());
    again.signal(SIGTERM);
    REQUIRE_EQ(again.wait(), 0);
    REQUIRE(!fs::exists(fs::symlink_status(directory / "vss-kill")));
}

TEST_CASE("vport: a name held by a running process is not taken over")
{
    scratch_directory scratch;
    const auto directory = scratch.path() / "links";

    cli_process running(
        {"vport", "--name=vss-held", stable_dir_argument(directory)});
    REQUIRE_EQ(link_of(running.read_line()), (directory / "vss-held").string());

    cli_process second(
        {"vport", "--name=vss-held", stable_dir_argument(directory)});
    REQUIRE_EQ(second.wait(), 1);

    // The first process still owns the name.
    REQUIRE(fs::is_symlink(directory / "vss-held"));
    running.signal(SIGTERM);
    REQUIRE_EQ(running.wait(), 0);
}

TEST_CASE("vport: bad arguments and an unsafe directory fail without output")
{
    scratch_directory scratch;

    cli_process nameless({"vport"});
    REQUIRE_EQ(nameless.wait(), 2);

    const auto open = scratch.path() / "open";
    fs::create_directory(open);
    fs::permissions(open, fs::perms::all, fs::perm_options::replace);
    cli_process unsafe({"vport", "--name=x", stable_dir_argument(open)});
    REQUIRE_EQ(unsafe.wait(), 1);
    REQUIRE(fs::is_empty(open));

    cli_process badlink({"vport", "--name=vss-one", "--link=vss-one",
                         stable_dir_argument(scratch.path() / "links")});
    REQUIRE_EQ(badlink.wait(), 2);
}

TEST_CASE("virtual: stays alive until it is signalled")
{
    cli_process cli({"virtual", "--kind=pty"});
    REQUIRE_EQ(cli.read_line(), std::string("virtual port virtual"));
    // It used to exit at once, which closed the port it had just announced.
    std::this_thread::sleep_for(300ms);
    REQUIRE_EQ(cli.wait(100ms), -1);
    cli.signal(SIGTERM);
    REQUIRE_EQ(cli.wait(), 0);
}
