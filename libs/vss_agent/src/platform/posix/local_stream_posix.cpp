//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent's transport on Linux and macOS: a Unix domain socket that only the
// user who runs the agent can open.
//
//   directory  $XDG_RUNTIME_DIR/virtual-serial-studio, or without
//              XDG_RUNTIME_DIR <tmp>/virtual-serial-studio-<uid> where <tmp>
//              is $TMPDIR or /tmp.  It is made with mode 0700 and checked on
//              every use: a directory that is a link, belongs to someone
//              else or is open to others is refused.  The uid is in the name
//              of the fallback only, so that another user's directory in a
//              shared /tmp cannot keep this user out.
//   socket     agent.sock, mode 0600, or agent-<instance>.sock
//   lock       agent.lock or agent-<instance>.lock, held with flock for as
//              long as the listener lives: this is the rule that only one
//              agent runs.  A crash drops the lock, so a socket file left by
//              a crashed agent is known to be stale and is replaced.
//   peer       a connection from any other user is closed unseen (see
//              `peer_user`)
//
// The server does not trust the directory alone, and the client does not trust
// the socket's name: each asks the kernel who is at the other end.
//

#include "platform/local_stream_platform.hpp"
#include "platform/posix/peer_user.hpp"

#include <vss/agent/proto/error.hpp>

#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace vss::agent::platform
{
namespace
{

using stream_protocol = boost::asio::local::stream_protocol;

constexpr std::size_t longest_socket_path = sizeof(sockaddr_un::sun_path) - 1;

error_code system_error(int value)
{
    return {value, boost::system::system_category()};
}

/// A file descriptor that is closed with its owner.
class descriptor_owner
{
public:
    descriptor_owner() = default;

    explicit descriptor_owner(int descriptor) noexcept : descriptor_(descriptor)
    {
    }

    descriptor_owner(descriptor_owner&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1))
    {
    }

    ~descriptor_owner()
    {
        if (descriptor_ >= 0)
            static_cast<void>(::close(descriptor_));
    }

    descriptor_owner(const descriptor_owner&) = delete;
    descriptor_owner& operator=(const descriptor_owner&) = delete;
    descriptor_owner& operator=(descriptor_owner&&) = delete;

    int get() const noexcept { return descriptor_; }

private:
    int descriptor_ = -1;
};

std::string environment(const char* name)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

std::filesystem::path runtime_directory()
{
    const std::string runtime = environment("XDG_RUNTIME_DIR");
    if (!runtime.empty())
        return std::filesystem::path(runtime) / "virtual-serial-studio";
    std::string base = environment("TMPDIR");
    if (base.empty())
        base = "/tmp";
    return std::filesystem::path(base) /
           ("virtual-serial-studio-" +
            std::to_string(static_cast<std::uint32_t>(::geteuid())));
}

/// `agent<extension>` or `agent-<instance><extension>` in `directory`.
std::string file_path(const std::filesystem::path& directory,
                      std::string_view instance, std::string_view extension)
{
    std::string name = "agent";
    if (!instance.empty())
    {
        name += '-';
        name += instance;
    }
    name += extension;
    return (directory / name).string();
}

/// Says whether `directory` is a real directory of this user that nobody else
/// can enter, read or change.  `lstat`, not `stat`: a link in place of the
/// directory is refused.
error_code check_private_directory(const std::filesystem::path& directory)
{
    const std::string text = directory.string();
    struct stat info{};
    if (::lstat(text.c_str(), &info) != 0)
        return system_error(errno);
    if (!S_ISDIR(info.st_mode))
        return make_error_code(errc::permission_denied);
    if (info.st_uid != ::geteuid())
        return make_error_code(errc::permission_denied);
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return make_error_code(errc::permission_denied);
    return {};
}

/// Makes the directory when it is not there, then checks it.
error_code ensure_private_directory(const std::filesystem::path& directory)
{
    const std::string text = directory.string();

    // Each missing parent gets the default mode; the last component is closed
    // to everyone but the owner.
    std::error_code ignored;
    if (directory.has_parent_path())
        std::filesystem::create_directories(directory.parent_path(), ignored);

    if (::mkdir(text.c_str(), S_IRWXU) != 0 && errno != EEXIST)
        return system_error(errno);
    return check_private_directory(directory);
}

error_code close_on_exec(int descriptor)
{
    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0)
        return system_error(errno);
    if (::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
        return system_error(errno);
    return {};
}

/// True when the process at the other end of the socket runs as this user.
bool is_own_user(int descriptor)
{
    const auto user = peer_user(descriptor);
    return user && *user == static_cast<std::uint32_t>(::geteuid());
}

class socket_stream final : public local_stream
{
public:
    explicit socket_stream(stream_protocol::socket socket)
        : socket_(std::move(socket))
    {
    }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override
    {
        socket_.async_read_some(buffer, std::move(handler));
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        socket_.async_write_some(buffer, std::move(handler));
    }

    void close() override
    {
        boost::system::error_code ignored;
        socket_.close(ignored);
    }

private:
    stream_protocol::socket socket_;
};

/// What the listener and an accept that is still waiting share, so the
/// completion of the wait is safe after the listener has gone.
struct listener_state
{
    listener_state(const boost::asio::any_io_executor& executor,
                   std::string socket_path)
        : acceptor(executor), path(std::move(socket_path))
    {
    }

    stream_protocol::acceptor acceptor;
    std::string path;
    /// The socket file exists and is this listener's.
    bool bound = false;
    bool closed = false;
};

/// Stops listening and removes the socket file, once.  A waiting accept
/// completes with `operation_aborted`.
void shut_down(listener_state& state) noexcept
{
    if (state.closed)
        return;
    state.closed = true;
    boost::system::error_code ignored;
    state.acceptor.close(ignored);
    if (state.bound)
    {
        static_cast<void>(::unlink(state.path.c_str()));
        state.bound = false;
    }
}

/// Replaces what a crashed agent left behind and starts listening.  The
/// caller holds the lock, so no other agent owns the file.
error_code start(listener_state& state)
{
    if (::unlink(state.path.c_str()) != 0)
    {
        const int failed = errno;
        if (failed != ENOENT)
            return system_error(failed);
    }

    boost::system::error_code failed;
    const stream_protocol::endpoint endpoint(state.path);
    state.acceptor.open(endpoint.protocol(), failed);
    if (failed)
        return failed;
    if (const error_code marked = close_on_exec(state.acceptor.native_handle()))
        return marked;
    state.acceptor.bind(endpoint, failed);
    if (failed)
        return failed;
    state.bound = true;
    state.acceptor.listen(stream_protocol::acceptor::max_listen_connections,
                          failed);
    if (failed)
        return failed;
    if (::chmod(state.path.c_str(), S_IRUSR | S_IWUSR) != 0)
        return system_error(errno);
    return {};
}

/// Waits for one client of this user.  A connection of anyone else is closed
/// and the wait goes on, without telling the handler.
void accept_next(const std::shared_ptr<listener_state>& state,
                 local_listener::accept_handler handler)
{
    if (state->closed)
    {
        boost::asio::post(
            state->acceptor.get_executor(), [handler = std::move(handler)]
            { handler(boost::asio::error::operation_aborted, nullptr); });
        return;
    }
    state->acceptor.async_accept(
        [state,
         handler = std::move(handler)](boost::system::error_code failed,
                                       stream_protocol::socket peer) mutable
        {
            if (state->closed)
            {
                handler(boost::asio::error::operation_aborted, nullptr);
                return;
            }
            if (failed)
            {
                handler(failed, nullptr);
                return;
            }
            const int descriptor = peer.native_handle();
            if (close_on_exec(descriptor) || !is_own_user(descriptor))
            {
                boost::system::error_code ignored;
                peer.close(ignored);
                accept_next(state, std::move(handler));
                return;
            }
            handler({}, std::make_unique<socket_stream>(std::move(peer)));
        });
}

class socket_listener final : public local_listener
{
public:
    socket_listener(std::shared_ptr<listener_state> state,
                    descriptor_owner lock)
        : state_(std::move(state)), lock_(std::move(lock))
    {
    }

    // The socket file goes before the lock does, so that an agent that takes
    // the lock next never has its own file removed by this one.
    ~socket_listener() override { shut_down(*state_); }

    socket_listener(const socket_listener&) = delete;
    socket_listener& operator=(const socket_listener&) = delete;

    void async_accept(accept_handler handler) override
    {
        accept_next(state_, std::move(handler));
    }

    void close() override { shut_down(*state_); }

    std::string address() const override { return state_->path; }

private:
    std::shared_ptr<listener_state> state_;
    /// Held for the life of the listener, also after `close`.
    descriptor_owner lock_;
};

/// Takes the lock file.  `protocol_errc::already_running` when another agent
/// holds it.
result<descriptor_owner> take_lock(const std::string& path)
{
    descriptor_owner file(::open(path.c_str(),
                                 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                 S_IRUSR | S_IWUSR));
    if (file.get() < 0)
        return system_error(errno);
    if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0)
    {
        const int failed = errno;
        if (failed == EWOULDBLOCK)
            return make_error_code(protocol_errc::already_running);
        return system_error(failed);
    }
    return file;
}

} // namespace

result<std::string> local_address(std::string_view instance)
{
    return file_path(runtime_directory(), instance, ".sock");
}

result<std::unique_ptr<local_listener>>
listen_local(const boost::asio::any_io_executor& executor,
             std::string_view instance)
{
    const std::filesystem::path directory = runtime_directory();
    if (const error_code failed = ensure_private_directory(directory))
        return failed;

    std::string socket_path = file_path(directory, instance, ".sock");
    if (socket_path.size() > longest_socket_path)
        return make_error_code(errc::invalid_argument);

    auto lock = take_lock(file_path(directory, instance, ".lock"));
    if (!lock)
        return lock.error();

    auto state =
        std::make_shared<listener_state>(executor, std::move(socket_path));
    if (const error_code failed = start(*state))
    {
        shut_down(*state);
        return failed;
    }
    return std::unique_ptr<local_listener>(
        std::make_unique<socket_listener>(std::move(state), std::move(*lock)));
}

result<std::unique_ptr<local_stream>>
connect_local(const boost::asio::any_io_executor& executor,
              std::string_view instance)
{
    const std::filesystem::path directory = runtime_directory();
    if (const error_code checked = check_private_directory(directory))
    {
        if (checked.category() == boost::system::system_category() &&
            checked.value() == ENOENT)
            return make_error_code(protocol_errc::not_running);
        return make_error_code(protocol_errc::not_same_user);
    }

    const std::string socket_path = file_path(directory, instance, ".sock");
    if (socket_path.size() > longest_socket_path)
        return make_error_code(errc::invalid_argument);

    const stream_protocol::endpoint endpoint(socket_path);
    stream_protocol::socket socket(executor);
    boost::system::error_code failed;
    socket.open(endpoint.protocol(), failed);
    if (failed)
        return failed;
    if (const error_code marked = close_on_exec(socket.native_handle()))
        return marked;
    socket.connect(endpoint, failed);
    if (failed)
    {
        if (failed == boost::system::errc::no_such_file_or_directory ||
            failed == boost::system::errc::connection_refused)
            return make_error_code(protocol_errc::not_running);
        return failed;
    }
    if (!is_own_user(socket.native_handle()))
        return make_error_code(protocol_errc::not_same_user);
    return std::unique_ptr<local_stream>(
        std::make_unique<socket_stream>(std::move(socket)));
}

} // namespace vss::agent::platform
