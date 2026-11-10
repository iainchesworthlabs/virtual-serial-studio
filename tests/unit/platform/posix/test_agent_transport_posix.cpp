//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent's socket on Linux and macOS: the modes of the socket and of its
// directory, the lock that keeps one agent for each name, and what happens to
// a file that is in the way.  Refusing a connection of another user needs a
// second account and is for a guest machine.
//

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/core/error.hpp>

#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>

#include <sys/stat.h>
#include <sys/types.h>
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
