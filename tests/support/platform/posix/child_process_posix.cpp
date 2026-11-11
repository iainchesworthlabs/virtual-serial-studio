//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `child_process` on POSIX systems (Linux and macOS): posix_spawn with a pipe
// for standard output and standard error, poll for timed reads, waitpid for
// the exit code.
//

#include "child_process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

extern char** environ;

namespace vss_test_support
{

namespace
{

using steady = std::chrono::steady_clock;

/// The longest any wait lasts, so that a huge limit cannot overflow a clock.
constexpr std::chrono::milliseconds longest_wait{std::chrono::hours{24}};

/// How long `wait` sleeps between two looks at the child.
constexpr int poll_step_ms = 10;

steady::time_point deadline_after(std::chrono::milliseconds limit)
{
    return steady::now() +
           std::clamp(limit, std::chrono::milliseconds{0}, longest_wait);
}

/// Whole milliseconds from now until `deadline`, rounded up, within 0..cap.
int milliseconds_until(steady::time_point deadline, int cap)
{
    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - steady::now());
    return static_cast<int>(std::clamp<std::int64_t>(left.count(), 0, cap));
}

std::string error_text(int code)
{
    return std::generic_category().message(code);
}

/// A file descriptor that closes itself.
class descriptor
{
public:
    descriptor() = default;
    explicit descriptor(int value) noexcept : value_(value) {}
    ~descriptor() { reset(); }

    descriptor(descriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1))
    {
    }

    descriptor& operator=(descriptor&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    int get() const noexcept { return value_; }
    bool is_open() const noexcept { return value_ >= 0; }

    void reset() noexcept
    {
        if (value_ >= 0)
            static_cast<void>(::close(value_));
        value_ = -1;
    }

private:
    int value_ = -1;
};

/// A posix_spawn file action list that destroys itself.
class spawn_actions
{
public:
    spawn_actions()
    {
        const int rc = ::posix_spawn_file_actions_init(&value_);
        if (rc != 0)
            throw std::runtime_error("cannot prepare the child: " +
                                     error_text(rc));
    }

    ~spawn_actions()
    {
        static_cast<void>(::posix_spawn_file_actions_destroy(&value_));
    }

    spawn_actions(const spawn_actions&) = delete;
    spawn_actions& operator=(const spawn_actions&) = delete;

    posix_spawn_file_actions_t* get() noexcept { return &value_; }

    void check(int rc) const
    {
        if (rc != 0)
            throw std::runtime_error("cannot prepare the child: " +
                                     error_text(rc));
    }

private:
    posix_spawn_file_actions_t value_;
};

void close_on_exec(int fd)
{
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        throw std::runtime_error("cannot configure a pipe: " +
                                 error_text(errno));
}

} // namespace

class child_process::impl
{
public:
    impl() = default;
    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    ~impl() { terminate(); }

    /// Stores what `waitpid` reported.
    void record(int status) noexcept
    {
        reaped_ = true;
        const int signal_number = status & 0x7f;
        exit_code_ =
            signal_number == 0 ? (status >> 8) & 0xff : 128 + signal_number;
    }

    /// Collects the exit status if the child has ended.
    bool try_reap() noexcept
    {
        if (reaped_)
            return true;
        for (;;)
        {
            int status = 0;
            const pid_t done = ::waitpid(pid_, &status, WNOHANG);
            if (done == pid_)
            {
                record(status);
                return true;
            }
            if (done == 0)
                return false;
            if (errno == EINTR)
                continue;
            // The child is gone and its status is lost (SIGCHLD ignored).
            reaped_ = true;
            exit_code_ = -1;
            return true;
        }
    }

    /// SIGKILL, then wait.
    void terminate() noexcept
    {
        if (reaped_ || pid_ <= 0)
            return;
        static_cast<void>(::kill(pid_, SIGKILL));
        for (;;)
        {
            int status = 0;
            const pid_t done = ::waitpid(pid_, &status, 0);
            if (done == pid_)
            {
                record(status);
                return;
            }
            if (done < 0 && errno == EINTR)
                continue;
            reaped_ = true;
            exit_code_ = -1;
            return;
        }
    }

    /// Waits for more output until `deadline`.  True when bytes were appended
    /// to `buffer_`; false on timeout or when the output has ended.
    bool fill(steady::time_point deadline)
    {
        while (output_.is_open())
        {
            pollfd watch{output_.get(), POLLIN, 0};
            const int ready =
                ::poll(&watch, 1, milliseconds_until(deadline, 1000));
            if (ready < 0)
            {
                if (errno == EINTR)
                    continue;
                output_.reset();
                return false;
            }
            if (ready == 0)
            {
                if (steady::now() >= deadline)
                    return false;
                continue;
            }

            std::array<char, 4096> chunk;
            const auto got = ::read(output_.get(), chunk.data(), chunk.size());
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
            {
                output_.reset();
                return false;
            }
            buffer_.append(chunk.data(), static_cast<std::size_t>(got));
            return true;
        }
        return false;
    }

private:
    friend class child_process;

    pid_t pid_ = -1;
    descriptor output_;
    bool reaped_ = false;
    int exit_code_ = 0;
    std::string buffer_;
};

child_process::child_process(const std::string& program,
                             const std::vector<std::string>& arguments)
    : impl_(std::make_unique<impl>())
{
    std::array<int, 2> ends{-1, -1};
    if (::pipe(ends.data()) != 0)
        throw std::runtime_error("cannot create a pipe: " + error_text(errno));
    descriptor read_end(ends[0]);
    descriptor write_end(ends[1]);
    close_on_exec(read_end.get());
    close_on_exec(write_end.get());

    // The child gets the write end as standard output and standard error and
    // /dev/null as standard input; both original pipe ends are closed in it.
    spawn_actions actions;
    actions.check(::posix_spawn_file_actions_addopen(
        actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    actions.check(::posix_spawn_file_actions_adddup2(
        actions.get(), write_end.get(), STDOUT_FILENO));
    actions.check(::posix_spawn_file_actions_adddup2(
        actions.get(), write_end.get(), STDERR_FILENO));
    actions.check(
        ::posix_spawn_file_actions_addclose(actions.get(), read_end.get()));
    actions.check(
        ::posix_spawn_file_actions_addclose(actions.get(), write_end.get()));

    std::vector<std::string> storage{program};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& item : storage)
        argv.push_back(item.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, program.c_str(), actions.get(), nullptr,
                                  argv.data(), environ);
    if (rc != 0)
        throw std::runtime_error("cannot start '" + program +
                                 "': " + error_text(rc));

    write_end.reset();
    impl_->pid_ = pid;
    impl_->output_ = std::move(read_end);
}

child_process::~child_process()
{
    if (impl_)
        impl_->terminate();
}

std::optional<int> child_process::wait(std::chrono::milliseconds limit)
{
    const auto deadline = deadline_after(limit);
    for (;;)
    {
        if (impl_->try_reap())
            return impl_->exit_code_;
        const int left = milliseconds_until(deadline, poll_step_ms);
        if (left <= 0)
            return std::nullopt;
        static_cast<void>(::poll(nullptr, 0, left));
    }
}

void child_process::kill()
{
    impl_->terminate();
}

bool child_process::request_stop()
{
    if (impl_->reaped_ || impl_->pid_ <= 0)
        return false;
    return ::kill(impl_->pid_, SIGTERM) == 0;
}

std::optional<std::string>
child_process::read_line(std::chrono::milliseconds limit)
{
    const auto deadline = deadline_after(limit);
    std::string& buffer = impl_->buffer_;
    for (;;)
    {
        const auto newline = buffer.find('\n');
        if (newline != std::string::npos)
        {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return line;
        }
        if (!impl_->fill(deadline))
            break;
    }

    // A partial line is returned only once the output has ended.
    if (!impl_->output_.is_open() && !buffer.empty())
        return std::exchange(buffer, std::string{});
    return std::nullopt;
}

std::string child_process::read_all(std::chrono::milliseconds limit)
{
    const auto deadline = deadline_after(limit);
    while (impl_->fill(deadline))
    {
        if (steady::now() >= deadline)
            break;
    }
    return std::exchange(impl_->buffer_, std::string{});
}

std::uint64_t child_process::id() const noexcept
{
    return static_cast<std::uint64_t>(impl_->pid_);
}

} // namespace vss_test_support
