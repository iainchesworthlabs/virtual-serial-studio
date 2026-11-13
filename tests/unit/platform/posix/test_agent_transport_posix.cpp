//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent's socket on Linux and macOS: the modes of the socket and of its
// directory, the lock that keeps one agent for each name, what happens to a
// file that is in the way, and a connection of another user.  The last needs a
// second account: it uses `nobody` through `sudo` where that works without a
// password (the hosted runners) and is skipped elsewhere.
//

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/core/error.hpp>

#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>

#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

extern char** environ;

namespace agent = vss::agent;

namespace
{

using steady = std::chrono::steady_clock;

constexpr std::chrono::milliseconds long_wait{20000};

vss::error_code code(agent::protocol_errc value)
{
    return agent::make_error_code(value);
}

std::string unique_instance_name()
{
    static const unsigned random = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    std::ostringstream out;
    out << "tp-" << std::hex << random << '-' << std::dec
        << counter.fetch_add(1);
    return out.str();
}

/// A name no other agent has.  The lock file the transport leaves beside the
/// socket, and a file a test put at the socket's path, go with it.
class scoped_instance
{
public:
    scoped_instance() : name_(unique_instance_name()) {}

    ~scoped_instance()
    {
        try
        {
            const auto address = agent::local_address(name_);
            if (!address)
                return;
            std::filesystem::path path(*address);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            path.replace_extension(".lock");
            std::filesystem::remove(path, ignored);
        }
        catch (...)
        {
            // best effort: a file left behind fails no test, and a
            // destructor has nobody to tell.
        }
    }

    scoped_instance(const scoped_instance&) = delete;
    scoped_instance& operator=(const scoped_instance&) = delete;

    const std::string& name() const noexcept { return name_; }

private:
    std::string name_;
};

/// `lstat`: a link is looked at, not followed.
std::optional<struct stat> status_of(const std::filesystem::path& path)
{
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0)
        return std::nullopt;
    return info;
}

unsigned permission_bits(const struct stat& info)
{
    return static_cast<unsigned>(info.st_mode & 07777);
}

template <class Predicate>
bool run_until(boost::asio::io_context& io, Predicate done,
               std::chrono::milliseconds limit)
{
    const auto deadline = steady::now() + limit;
    while (!done())
    {
        const auto now = steady::now();
        if (now >= deadline)
            return false;
        io.restart();
        io.run_one_for(deadline - now);
    }
    return true;
}

struct accept_result
{
    bool done = false;
    boost::system::error_code failed;
    std::unique_ptr<agent::local_stream> stream;
};

std::shared_ptr<accept_result> accept_into(agent::local_listener& listener)
{
    auto result = std::make_shared<accept_result>();
    listener.async_accept(
        [result](boost::system::error_code failed,
                 std::unique_ptr<agent::local_stream> stream)
        {
            result->done = true;
            result->failed = failed;
            result->stream = std::move(stream);
        });
    return result;
}

/// Connects to the instance and waits until the listener hands the connection
/// over.
bool accepts_a_connection(boost::asio::io_context& io,
                          agent::local_listener& listener,
                          const std::string& instance)
{
    const auto accepted = accept_into(listener);
    const auto connected = agent::connect_local(io.get_executor(), instance);
    if (!connected)
        return false;
    if (!run_until(io, [&] { return accepted->done; }, long_wait))
        return false;
    return !accepted->failed && accepted->stream != nullptr;
}

/// Starts a program found on the PATH.  0 when it could not be started.
pid_t start_program(std::vector<std::string> words)
{
    std::vector<char*> arguments;
    arguments.reserve(words.size() + 1);
    for (std::string& word : words)
        arguments.push_back(word.data());
    arguments.push_back(nullptr);
    pid_t child = 0;
    if (::posix_spawnp(&child, arguments[0], nullptr, nullptr, arguments.data(),
                       environ) != 0)
        return 0;
    return child;
}

/// The exit code of a program that has ended, or nothing while it runs.  A
/// program that was ended by a signal counts as 255.
std::optional<int> exit_code_now(pid_t child)
{
    int status = 0;
    if (::waitpid(child, &status, WNOHANG) != child)
        return std::nullopt;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
}

int exit_code_of(std::vector<std::string> words)
{
    const pid_t child = start_program(std::move(words));
    if (child == 0)
        return 255;
    int status = 0;
    if (::waitpid(child, &status, 0) != child)
        return 255;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
}

/// The words that run `python3 -c <script> <arguments>` as the user `nobody`.
std::vector<std::string> as_nobody(std::string script,
                                   std::vector<std::string> arguments)
{
    std::vector<std::string> words{
        "sudo", "-n", "-u", "nobody", "python3", "-c", std::move(script)};
    for (std::string& argument : arguments)
        words.push_back(std::move(argument));
    return words;
}

/// What the stranger does: connect to the socket and wait to be told the
/// connection is over.  0 the agent closed it, 3 the system refused the
/// connection, 4 nothing happened, 5 the agent sent something.
constexpr const char* stranger_script =
    "import socket, sys\n"
    "s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n"
    "s.settimeout(15)\n"
    "try:\n"
    "    s.connect(sys.argv[1])\n"
    "except OSError:\n"
    "    sys.exit(3)\n"
    "try:\n"
    "    data = s.recv(1)\n"
    "except socket.timeout:\n"
    "    sys.exit(4)\n"
    "except OSError:\n"
    "    sys.exit(0)\n"
    "sys.exit(0 if data == b'' else 5)\n";

/// Points the transport at a directory of the test's own for as long as it
/// lives, and puts the environment back afterwards.  The usual directory is
/// under one only this user can enter, so no other user could reach a socket
/// in it whatever its mode.
class scoped_runtime_base
{
public:
    scoped_runtime_base()
    {
        // NOLINTBEGIN(concurrency-mt-unsafe)
        if (const char* value = ::getenv("XDG_RUNTIME_DIR"))
            runtime_ = value;
        if (const char* value = ::getenv("TMPDIR"))
            tmpdir_ = value;
        // NOLINTEND(concurrency-mt-unsafe)

        std::string made = "/tmp/vssxu.XXXXXX";
        if (::mkdtemp(made.data()) == nullptr)
            return;
        base_ = made;
        // Anybody may pass through; the transport's own directory inside it
        // is what is private.
        static_cast<void>(::chmod(base_.c_str(), 0755));
        static_cast<void>(::unsetenv("XDG_RUNTIME_DIR"));
        static_cast<void>(::setenv("TMPDIR", base_.c_str(), 1));
    }

    ~scoped_runtime_base()
    {
        if (runtime_)
            static_cast<void>(
                ::setenv("XDG_RUNTIME_DIR", runtime_->c_str(), 1));
        if (tmpdir_)
            static_cast<void>(::setenv("TMPDIR", tmpdir_->c_str(), 1));
        else
            static_cast<void>(::unsetenv("TMPDIR"));
        if (!base_.empty())
        {
            std::error_code ignored;
            std::filesystem::remove_all(base_, ignored);
        }
    }

    scoped_runtime_base(const scoped_runtime_base&) = delete;
    scoped_runtime_base& operator=(const scoped_runtime_base&) = delete;

    bool made() const noexcept { return !base_.empty(); }

private:
    std::optional<std::string> runtime_;
    std::optional<std::string> tmpdir_;
    std::string base_;
};

} // namespace

TEST_CASE("agent transport: the socket and its directory are private")
{
    const scoped_instance instance;
    boost::asio::io_context io;
    auto listener = agent::listen_local(io.get_executor(), instance.name());
    REQUIRE(listener);

    const std::filesystem::path socket((*listener)->address());
    const auto expected = agent::local_address(instance.name());
    REQUIRE(expected);
    REQUIRE_EQ(socket.string(), *expected);

    const auto socket_status = status_of(socket);
    REQUIRE(socket_status.has_value());
    REQUIRE(S_ISSOCK(socket_status->st_mode));
    REQUIRE_EQ(permission_bits(*socket_status), 0600U);

    const auto directory_status = status_of(socket.parent_path());
    REQUIRE(directory_status.has_value());
    REQUIRE(S_ISDIR(directory_status->st_mode));
    REQUIRE_EQ(directory_status->st_uid, ::geteuid());
    REQUIRE_EQ(permission_bits(*directory_status), 0700U);

    // The lock that keeps one agent for each name is beside the socket.
    std::filesystem::path lock = socket;
    lock.replace_extension(".lock");
    const auto lock_status = status_of(lock);
    REQUIRE(lock_status.has_value());
    REQUIRE(S_ISREG(lock_status->st_mode));
}

TEST_CASE("agent transport: a second listener on a name in use is refused")
{
    const scoped_instance instance;
    boost::asio::io_context io;
    auto first = agent::listen_local(io.get_executor(), instance.name());
    REQUIRE(first);
    const std::filesystem::path socket((*first)->address());

    const auto second = agent::listen_local(io.get_executor(), instance.name());
    REQUIRE(!second);
    REQUIRE_EQ(second.error(), code(agent::protocol_errc::already_running));

    // The first listener and its socket are not disturbed.
    const auto socket_status = status_of(socket);
    REQUIRE(socket_status.has_value());
    REQUIRE(S_ISSOCK(socket_status->st_mode));
    REQUIRE(accepts_a_connection(io, **first, instance.name()));
}

TEST_CASE("agent transport: the socket file goes with the listener")
{
    const scoped_instance instance;
    boost::asio::io_context io;
    std::filesystem::path socket;
    {
        auto listener = agent::listen_local(io.get_executor(), instance.name());
        REQUIRE(listener);
        socket = (*listener)->address();
        REQUIRE(status_of(socket).has_value());
    }
    REQUIRE(!status_of(socket).has_value());

    // The name is free again.
    auto again = agent::listen_local(io.get_executor(), instance.name());
    REQUIRE(again);
    REQUIRE(accepts_a_connection(io, **again, instance.name()));
}

TEST_CASE("agent transport: a file in the way of the socket is replaced")
{
    const scoped_instance instance;
    boost::asio::io_context io;
    const auto address = agent::local_address(instance.name());
    REQUIRE(address);
    const std::filesystem::path socket(*address);

    // A first listener makes the directory; its socket goes with it.
    {
        auto first = agent::listen_local(io.get_executor(), instance.name());
        REQUIRE(first);
    }
    REQUIRE(!status_of(socket).has_value());

    {
        std::ofstream stale(socket);
        stale << "left behind";
    }
    const auto before = status_of(socket);
    REQUIRE(before.has_value());
    REQUIRE(S_ISREG(before->st_mode));

    auto listener = agent::listen_local(io.get_executor(), instance.name());
    REQUIRE(listener);
    const auto after = status_of(socket);
    REQUIRE(after.has_value());
    REQUIRE(S_ISSOCK(after->st_mode));
    REQUIRE(accepts_a_connection(io, **listener, instance.name()));
}

TEST_CASE("agent transport: a connection of another user is closed unseen")
{
    if (exit_code_of(as_nobody("pass", {})) != 0)
        VSS_SKIP("needs sudo without a password, the user nobody and python3");

    const scoped_runtime_base base;
    REQUIRE(base.made());
    const scoped_instance instance;
    boost::asio::io_context io;
    auto listener = agent::listen_local(io.get_executor(), instance.name());
    REQUIRE(listener);
    const std::filesystem::path socket((*listener)->address());
    const std::filesystem::path directory = socket.parent_path();

    // Open the two doors the modes keep shut, so that the question the agent
    // asks about the other end is all that stands between the stranger and
    // the server.  Nothing but this test ever does this.
    REQUIRE_EQ(::chmod(directory.c_str(), 0755), 0);
    REQUIRE_EQ(::chmod(socket.c_str(), 0666), 0);

    const auto accepted = accept_into(**listener);
    const pid_t stranger =
        start_program(as_nobody(stranger_script, {socket.string()}));
    REQUIRE(stranger != 0);

    // The agent has to run for the connection to be looked at, so the wait
    // for the stranger to end is a wait on the io_context in short turns.
    std::optional<int> ended;
    const auto deadline = steady::now() + long_wait;
    while (!ended && steady::now() < deadline)
    {
        io.restart();
        io.run_for(std::chrono::milliseconds(20));
        ended = exit_code_now(stranger);
    }
    if (!ended)
    {
        static_cast<void>(::kill(stranger, SIGKILL));
        int status = 0;
        static_cast<void>(::waitpid(stranger, &status, 0));
    }
    REQUIRE(ended.has_value());
    // The system let the stranger in (3 would mean a mode still stopped it and
    // the test proved nothing), and the agent closed the connection without a
    // byte (4: left open, 5: answered).
    REQUIRE_EQ(*ended, 0);
    // The server was never told about it.
    REQUIRE(!accepted->done);

    // The wait went on: with the doors shut again, this user's own client is
    // the first connection the listener hands over.
    REQUIRE_EQ(::chmod(socket.c_str(), 0600), 0);
    REQUIRE_EQ(::chmod(directory.c_str(), 0700), 0);
    const auto own = agent::connect_local(io.get_executor(), instance.name());
    REQUIRE(own);
    REQUIRE(run_until(io, [&] { return accepted->done; }, long_wait));
    REQUIRE(!accepted->failed);
    REQUIRE(accepted->stream != nullptr);
}

TEST_CASE("control transport: both sockets of the gateway are private and "
          "have their own lock")
{
    const scoped_instance instance;
    boost::asio::io_context io;
    for (const agent::local_endpoint endpoint :
         {agent::local_endpoint::control, agent::local_endpoint::control_admin})
    {
        auto listener =
            agent::listen_local(io.get_executor(), endpoint, instance.name());
        REQUIRE(listener);
        const std::filesystem::path socket((*listener)->address());
        const auto expected = agent::local_address(endpoint, instance.name());
        REQUIRE(expected);
        REQUIRE_EQ(socket.string(), *expected);

        const auto socket_status = status_of(socket);
        REQUIRE(socket_status.has_value());
        REQUIRE(S_ISSOCK(socket_status->st_mode));
        REQUIRE_EQ(permission_bits(*socket_status), 0600U);

        const auto directory_status = status_of(socket.parent_path());
        REQUIRE(directory_status.has_value());
        REQUIRE_EQ(directory_status->st_uid, ::geteuid());
        REQUIRE_EQ(permission_bits(*directory_status), 0700U);

        // One gateway for each name: a second listener is refused.
        const auto second =
            agent::listen_local(io.get_executor(), endpoint, instance.name());
        REQUIRE(!second);
        REQUIRE_EQ(second.error(), code(agent::protocol_errc::already_running));

        std::filesystem::path lock = socket;
        lock.replace_extension(".lock");
        REQUIRE(status_of(lock).has_value());
        (*listener)->close();
        listener->reset();
        std::error_code ignored;
        std::filesystem::remove(lock, ignored);
    }

    // The agent's own name is not taken by either.
    auto own = agent::listen_local(io.get_executor(), instance.name());
    REQUIRE(own);
}
