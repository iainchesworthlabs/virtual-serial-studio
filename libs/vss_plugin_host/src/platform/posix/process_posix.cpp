//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plugin's process on Linux and macOS.
//
// The process is made with fork and execve.  The host has several threads,
// so between the two the child calls only what may be called there: it
// makes a session of its own, puts signals back to their defaults, places
// its four descriptors and closes every other one, changes directory and
// runs the program.  Everything it needs for that (texts, lists, numbers)
// is prepared before the fork.
//
// The descriptors of the plugin: 0 reads /dev/null, 1 and 2 write the
// output pipe, 3 is the channel.
//
// Every other descriptor is closed in the child by number, up to the
// highest the host had open just before the fork and a margin above it.
// Another thread that opens more than that margin of descriptors in that
// moment, without close-on-exec, could pass one on.  The launcher of
// docs/plugins.md, section 9.5, has one thread and closes that gap.
//
// A failure after the fork (no such directory, no such program) comes back
// through a pipe that the execve closes when it succeeds.
//
// The host's end of the channel is a socket: write to it with send() and
// MSG_NOSIGNAL (or SO_NOSIGPIPE), never with write(), or a plugin that has
// gone raises SIGPIPE in the host.
//

#include "platform/process_platform.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/host/process.hpp>

#include <boost/system/error_code.hpp>
#include <boost/system/system_category.hpp>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss::plugin::host::platform
{

namespace
{

error_code system_error(int code) noexcept
{
    return {code, boost::system::system_category()};
}

/// A descriptor that is closed when it goes out of scope.
class owned_descriptor
{
public:
    owned_descriptor() = default;
    explicit owned_descriptor(int descriptor) noexcept : descriptor_(descriptor)
    {
    }
    ~owned_descriptor() { reset(); }
    owned_descriptor(owned_descriptor&& other) noexcept
        : descriptor_(other.release())
    {
    }
    owned_descriptor& operator=(owned_descriptor&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            descriptor_ = other.release();
        }
        return *this;
    }
    owned_descriptor(const owned_descriptor&) = delete;
    owned_descriptor& operator=(const owned_descriptor&) = delete;

    bool valid() const noexcept { return descriptor_ >= 0; }
    int get() const noexcept { return descriptor_; }
    int release() noexcept { return std::exchange(descriptor_, -1); }
    void reset() noexcept
    {
        if (descriptor_ >= 0)
            ::close(descriptor_);
        descriptor_ = -1;
    }

private:
    int descriptor_ = -1;
};

/// The descriptors a plugin has, by number.
constexpr int plugin_input = 0;
constexpr int plugin_output = 1;
constexpr int plugin_error = 2;
constexpr int plugin_channel = 3;

/// The lowest number the host's own copies of the plugin's descriptors
/// take, so that none of them sits where the child places one.
constexpr int first_spare = 10;

/// How far above the highest open descriptor the child still closes.
constexpr int close_margin = 256;

/// What is closed when the list of open descriptors cannot be read.
constexpr int close_fallback = 65536;

/// A copy of a descriptor at `first_spare` or above, closed on exec, in
/// place of the one given.
error_code lift(owned_descriptor& descriptor) noexcept
{
    const int copy = ::fcntl(descriptor.get(), F_DUPFD_CLOEXEC, first_spare);
    if (copy < 0)
        return system_error(errno);
    descriptor = owned_descriptor(copy);
    return {};
}

/// The highest descriptor this process has open now.
int highest_descriptor() noexcept
{
    DIR* const directory = ::opendir("/dev/fd");
    if (directory == nullptr)
    {
        struct rlimit limit{};
        if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 ||
            limit.rlim_cur > static_cast<rlim_t>(close_fallback))
            return close_fallback;
        return static_cast<int>(limit.rlim_cur);
    }
    int highest = plugin_channel;
    while (const dirent* const entry = ::readdir(directory))
    {
        const std::string_view name(entry->d_name);
        int number = 0;
        const auto parsed =
            std::from_chars(name.data(), name.data() + name.size(), number);
        if (parsed.ec == std::errc() && parsed.ptr == name.data() + name.size())
            highest = std::max(highest, number);
    }
    ::closedir(directory);
    return highest;
}

/// What the child needs, all of it made before the fork.
struct child_plan
{
    const char* program = nullptr;
    char* const* arguments = nullptr;
    char* const* environment = nullptr;
    const char* directory = nullptr;
    int input = -1;
    int output = -1;
    int channel = -1;
    int report = -1;
    int close_up_to = 0;
};

/// The child, between fork and execve.  Only calls that are safe there.
/// It does not return.
[[noreturn]] void run_child(const child_plan& plan) noexcept
{
    int failed = 0;
    // A session of its own: no terminal, and a process group whose number
    // is this process's, which is what the host later signals.
    if (::setsid() < 0)
        failed = errno;

    // Signals as a new program expects them: none ignored, none blocked.
    struct sigaction standard{};
    standard.sa_handler = SIG_DFL;
    ::sigemptyset(&standard.sa_mask);
    for (int signal_number = 1; signal_number < NSIG; ++signal_number)
        ::sigaction(signal_number, &standard, nullptr);
    sigset_t none;
    ::sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);

    if (failed == 0 && (::dup2(plan.input, plugin_input) < 0 ||
                        ::dup2(plan.output, plugin_output) < 0 ||
                        ::dup2(plan.output, plugin_error) < 0 ||
                        ::dup2(plan.channel, plugin_channel) < 0))
        failed = errno;

    for (int descriptor = plugin_channel + 1; descriptor <= plan.close_up_to;
         ++descriptor)
    {
        if (descriptor != plan.report)
            ::close(descriptor);
    }

    if (failed == 0 && ::chdir(plan.directory) != 0)
        failed = errno;
    if (failed == 0)
    {
        ::execve(plan.program, plan.arguments, plan.environment);
        failed = errno;
    }
    // Tell the host why, and go.  The write is small enough to be whole.
    const ssize_t written = ::write(plan.report, &failed, sizeof(failed));
    static_cast<void>(written);
    ::_exit(127);
}

/// The texts of a list as the pointers execve takes, ended by a null.
std::vector<char*> pointers_to(std::vector<std::string>& texts)
{
    std::vector<char*> pointers;
    pointers.reserve(texts.size() + 1);
    for (std::string& text : texts)
        pointers.push_back(text.data());
    pointers.push_back(nullptr);
    return pointers;
}

/// How long the destructor waits for a killed process to be gone.
constexpr std::chrono::milliseconds gone_limit{5000};

class posix_process final : public plugin_process::impl
{
public:
    posix_process(pid_t id, owned_descriptor channel,
                  owned_descriptor output) noexcept
        : id_(id), channel_(std::move(channel)), output_(std::move(output))
    {
    }

    ~posix_process() override
    {
        kill();
        wait(gone_limit);
    }

    std::uint64_t id() const noexcept override
    {
        return static_cast<std::uint64_t>(id_);
    }

    native_handle channel() const noexcept override
    {
        return channel_.valid() ? channel_.get() : no_handle;
    }

    native_handle output() const noexcept override
    {
        return output_.valid() ? output_.get() : no_handle;
    }

    native_handle take_channel() noexcept override
    {
        return channel_.valid() ? channel_.release() : no_handle;
    }

    native_handle take_output() noexcept override
    {
        return output_.valid() ? output_.release() : no_handle;
    }

    bool request_stop() noexcept override
    {
        // After the end was seen the number may belong to someone else.
        if (poll().has_value())
            return false;
        return ::kill(-id_, SIGTERM) == 0;
    }

    void kill() noexcept override
    {
        if (poll().has_value())
            return;
        ::kill(-id_, SIGKILL);
    }

    std::optional<exit_status> poll() noexcept override
    {
        if (ended_.has_value())
            return ended_;
        // Look without collecting: while the ended process is not
        // collected its number, and so its group's, cannot be given to
        // another, and the group can still be signalled safely.
        siginfo_t information{};
        if (::waitid(P_PID, static_cast<id_t>(id_), &information,
                     WEXITED | WNOHANG | WNOWAIT) != 0 ||
            information.si_pid == 0)
            return std::nullopt;
        // What the plugin started and left behind goes with it.
        ::kill(-id_, SIGKILL);
        int status = 0;
        pid_t collected = -1;
        do
        {
            collected = ::waitpid(id_, &status, 0);
        } while (collected < 0 && errno == EINTR);
        exit_status result;
        if (collected == id_ && WIFSIGNALED(status))
        {
            result.abnormal = true;
            result.code = WTERMSIG(status);
        }
        else if (collected == id_ && WIFEXITED(status))
        {
            result.code = WEXITSTATUS(status);
        }
        else
        {
            result.abnormal = true;
        }
        ended_ = result;
        return ended_;
    }

    std::optional<exit_status>
    wait(std::chrono::milliseconds limit) noexcept override
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (;;)
        {
            if (const auto status = poll())
                return status;
            if (std::chrono::steady_clock::now() >= deadline)
                return std::nullopt;
            const timespec pause{0, 2'000'000};
            ::nanosleep(&pause, nullptr);
        }
    }

private:
    pid_t id_ = -1;
    owned_descriptor channel_;
    owned_descriptor output_;
    std::optional<exit_status> ended_;
};

} // namespace

result<std::unique_ptr<plugin_process::impl>>
start_process(const process_request& request)
{
    // The texts the child uses.
    std::string program = request.program.string();
    std::string directory = request.working_directory.string();
    std::vector<std::string> argument_texts{program};
    argument_texts.insert(argument_texts.end(), request.arguments.begin(),
                          request.arguments.end());
    std::vector<std::string> environment_texts;
    environment_texts.reserve(request.environment.size() + 1);
    for (const auto& [name, value] : request.environment)
    {
        std::string text;
        text.reserve(name.size() + value.size() + 1);
        text.append(name).append("=").append(value);
        environment_texts.push_back(std::move(text));
    }
    environment_texts.push_back(std::string(channel_variable) + "=" +
                                std::to_string(plugin_channel));
    std::vector<char*> arguments = pointers_to(argument_texts);
    std::vector<char*> environment = pointers_to(environment_texts);

    // The channel, the output pipe, the report pipe and /dev/null.
    std::array<int, 2> pair{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0)
        return system_error(errno);
    owned_descriptor channel_host(pair[0]);
    owned_descriptor channel_plugin(pair[1]);
    if (::pipe(pair.data()) != 0)
        return system_error(errno);
    owned_descriptor output_host(pair[0]);
    owned_descriptor output_plugin(pair[1]);
    if (::pipe(pair.data()) != 0)
        return system_error(errno);
    owned_descriptor report_host(pair[0]);
    owned_descriptor report_plugin(pair[1]);
    owned_descriptor input(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (!input.valid())
        return system_error(errno);
    for (owned_descriptor* const descriptor :
         {&channel_host, &channel_plugin, &output_host, &output_plugin,
          &report_host, &report_plugin, &input})
    {
        if (const error_code failed = lift(*descriptor))
            return failed;
    }

    child_plan plan;
    plan.program = program.c_str();
    plan.arguments = arguments.data();
    plan.environment = environment.data();
    plan.directory = directory.c_str();
    plan.input = input.get();
    plan.output = output_plugin.get();
    plan.channel = channel_plugin.get();
    plan.report = report_plugin.get();
    plan.close_up_to = highest_descriptor() + close_margin;

    const pid_t child = ::fork();
    if (child < 0)
        return system_error(errno);
    if (child == 0)
        run_child(plan);

    // The plugin's ends are the plugin's alone now.
    channel_plugin.reset();
    output_plugin.reset();
    report_plugin.reset();
    input.reset();

    // The report pipe ends with nothing when the program runs, and holds
    // an error number when it does not.
    int failed = 0;
    ssize_t got = 0;
    do
    {
        got = ::read(report_host.get(), &failed, sizeof(failed));
    } while (got < 0 && errno == EINTR);
    if (got != 0)
    {
        int status = 0;
        pid_t collected = -1;
        do
        {
            collected = ::waitpid(child, &status, 0);
        } while (collected < 0 && errno == EINTR);
        return system_error(
            got == static_cast<ssize_t>(sizeof(failed)) && failed != 0 ? failed
                                                                       : EIO);
    }

    return std::unique_ptr<plugin_process::impl>(
        std::make_unique<posix_process>(child, std::move(channel_host),
                                        std::move(output_host)));
}

std::vector<std::pair<std::string, std::string>> system_environment()
{
    return {{"PATH", "/usr/bin:/bin"}};
}

} // namespace vss::plugin::host::platform
