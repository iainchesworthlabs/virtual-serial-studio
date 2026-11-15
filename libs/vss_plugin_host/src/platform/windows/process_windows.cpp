//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plugin's process on Windows.
//
// What the plugin inherits is named handle by handle (the handle list of
// the process's start-up attributes), so no other inheritable handle of the
// host reaches it, whichever thread made one at the wrong moment.
//
// The channel and the output pipe are named pipes, because only a named
// pipe can be opened for overlapped I/O, which the host needs to read
// without a thread for each plugin.  The name is no way in:
//
// - it holds 128 random bits;
// - the pipe is made as the first and only instance of its name, so making
//   it fails when the name exists, and nobody can make a second;
// - only this user may open it, and no remote client;
// - this code opens the one client end itself, at once.  Had another
//   program got there first, the open here fails and the plugin is never
//   started.  Once both ends are open, nothing else can connect.
//
// The process is put in a job object before its first instruction runs.
// Closing or ending the job ends everything in it.
//

#include "platform/process_platform.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/host/process.hpp>

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>

#include <boost/system/error_code.hpp>
#include <boost/system/system_category.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss::plugin::host::platform
{

namespace
{

error_code system_error(DWORD code) noexcept
{
    return {static_cast<int>(code), boost::system::system_category()};
}

error_code last_error() noexcept
{
    return system_error(::GetLastError());
}

/// A handle that is closed when it goes out of scope.
class owned_handle
{
public:
    owned_handle() = default;
    explicit owned_handle(HANDLE handle) noexcept : handle_(handle) {}
    ~owned_handle() { reset(); }
    owned_handle(owned_handle&& other) noexcept : handle_(other.release()) {}
    owned_handle& operator=(owned_handle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            handle_ = other.release();
        }
        return *this;
    }
    owned_handle(const owned_handle&) = delete;
    owned_handle& operator=(const owned_handle&) = delete;

    bool valid() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    void reset() noexcept
    {
        if (valid())
            ::CloseHandle(handle_);
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

struct local_free
{
    void operator()(void* memory) const noexcept { ::LocalFree(memory); }
};

native_handle number_of(HANDLE handle) noexcept
{
    return reinterpret_cast<native_handle>(handle);
}

std::optional<std::wstring> wide(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::nullopt;
    const int size = static_cast<int>(text.size());
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             text.data(), size, nullptr, 0);
    if (needed <= 0)
        return std::nullopt;
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size,
                              result.data(), needed) != needed)
        return std::nullopt;
    return result;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int size = static_cast<int>(text.size());
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), size,
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string result(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), size, result.data(), needed,
                          nullptr, nullptr);
    return result;
}

/// The user this process runs as, as the text of a SID.
result<std::wstring> own_sid_text()
{
    HANDLE raw = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw) == 0)
        return last_error();
    const owned_handle token(raw);
    DWORD size = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (size == 0)
        return last_error();
    std::vector<std::byte> buffer(size);
    if (::GetTokenInformation(token.get(), TokenUser, buffer.data(), size,
                              &size) == 0)
        return last_error();
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR text = nullptr;
    if (::ConvertSidToStringSidW(user->User.Sid, &text) == 0)
        return last_error();
    const std::unique_ptr<wchar_t, local_free> owner(text);
    return std::wstring(text);
}

/// A name for a pipe that nobody can guess.
result<std::wstring> random_pipe_name()
{
    std::array<unsigned char, 16> random{};
    const NTSTATUS status = ::BCryptGenRandom(nullptr, random.data(),
                                              static_cast<ULONG>(random.size()),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0)
        return make_error_code(plugin_errc::start_failed);
    constexpr std::wstring_view digits = L"0123456789abcdef";
    std::wstring name = L"\\\\.\\pipe\\vss-plugin-";
    for (const unsigned char byte : random)
    {
        name.push_back(digits[byte >> 4U]);
        name.push_back(digits[byte & 0x0fU]);
    }
    return name;
}

/// The two ends of one pipe: the host's, opened for overlapped I/O and not
/// inherited by anything; and the plugin's, which the plugin inherits.
struct pipe_ends
{
    owned_handle host;
    owned_handle plugin;
};

/// `two_way` is the channel; otherwise the plugin writes and the host
/// reads.
result<pipe_ends> make_pipe(bool two_way, PSECURITY_DESCRIPTOR own_user_only)
{
    const auto name = random_pipe_name();
    if (!name)
        return name.error();

    SECURITY_ATTRIBUTES host_attributes{};
    host_attributes.nLength = sizeof(host_attributes);
    host_attributes.lpSecurityDescriptor = own_user_only;
    host_attributes.bInheritHandle = FALSE;
    const DWORD direction = two_way ? PIPE_ACCESS_DUPLEX : PIPE_ACCESS_INBOUND;
    constexpr DWORD buffer_size = 65536;
    pipe_ends ends;
    ends.host = owned_handle(::CreateNamedPipeW(
        name->c_str(),
        direction | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, buffer_size, buffer_size, 0, &host_attributes));
    if (!ends.host.valid())
        return last_error();

    SECURITY_ATTRIBUTES plugin_attributes{};
    plugin_attributes.nLength = sizeof(plugin_attributes);
    plugin_attributes.bInheritHandle = TRUE;
    // FILE_READ_ATTRIBUTES lets the plugin's runtime ask what kind of file
    // its standard output is.
    const DWORD access = two_way ? (GENERIC_READ | GENERIC_WRITE)
                                 : (GENERIC_WRITE | FILE_READ_ATTRIBUTES);
    ends.plugin = owned_handle(::CreateFileW(name->c_str(), access, 0,
                                             &plugin_attributes, OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!ends.plugin.valid())
        return last_error();
    return ends;
}

/// The environment as Windows takes it: "NAME=value" texts, each ended by
/// a zero, in the order of their names, and one more zero at the end.
std::optional<std::wstring> environment_block(
    const std::vector<std::pair<std::string, std::string>>& environment,
    HANDLE channel)
{
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    entries.reserve(environment.size() + 1);
    for (const auto& [name, value] : environment)
    {
        auto wide_name = wide(name);
        auto wide_value = wide(value);
        if (!wide_name || !wide_value)
            return std::nullopt;
        entries.emplace_back(std::move(*wide_name), std::move(*wide_value));
    }
    const auto channel_name = wide(std::string(channel_variable));
    if (!channel_name)
        return std::nullopt;
    entries.emplace_back(
        *channel_name,
        std::to_wstring(static_cast<std::uintptr_t>(number_of(channel))));

    std::ranges::sort(entries,
                      [](const auto& left, const auto& right)
                      {
                          return ::CompareStringOrdinal(left.first.c_str(), -1,
                                                        right.first.c_str(), -1,
                                                        TRUE) == CSTR_LESS_THAN;
                      });
    std::wstring block;
    for (const auto& [name, value] : entries)
    {
        block.append(name);
        block.push_back(L'=');
        block.append(value);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

/// The start-up attribute that names the handles a new process inherits.
class handle_list
{
public:
    explicit handle_list(std::vector<HANDLE> handles)
        : handles_(std::move(handles))
    {
    }
    ~handle_list()
    {
        if (ready_)
            ::DeleteProcThreadAttributeList(list());
    }
    handle_list(const handle_list&) = delete;
    handle_list& operator=(const handle_list&) = delete;

    error_code prepare()
    {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (size == 0)
            return last_error();
        storage_.resize(size);
        if (::InitializeProcThreadAttributeList(list(), 1, 0, &size) == 0)
            return last_error();
        ready_ = true;
        if (::UpdateProcThreadAttribute(
                list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                static_cast<void*>(handles_.data()),
                handles_.size() * sizeof(HANDLE), nullptr, nullptr) == 0)
            return last_error();
        return {};
    }

    LPPROC_THREAD_ATTRIBUTE_LIST list() noexcept
    {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    }

private:
    std::vector<HANDLE> handles_;
    std::vector<std::byte> storage_;
    bool ready_ = false;
};

/// A job and what it confines.
struct confined_job
{
    owned_handle job;
    sandbox_report report;
};

/// A job that ends everything in it when its last handle is closed, and
/// that shows no error window for a process that crashes.  With a profile
/// it also holds the plugin to its limits and takes the user interface
/// away from it.  A part that cannot be set is left out of the report and
/// the job is still made: whether that is enough is the caller's to say.
result<confined_job> make_job(const sandbox_profile* profile)
{
    confined_job made;
    made.job = owned_handle(::CreateJobObjectW(nullptr, nullptr));
    if (!made.job.valid())
        return last_error();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (::SetInformationJobObject(made.job.get(),
                                  JobObjectExtendedLimitInformation, &limits,
                                  sizeof(limits)) == 0)
        return last_error();
    // Everything in the job ends with it, and a process cannot leave a job.
    made.report.applied.push_back(sandbox_part::containment);
    if (profile == nullptr)
        return made;

    // Memory and the number of processes.  A plugin that may not start
    // programs is one process.  Windows has no limit on open files for a
    // job; the others stand for "limits".
    limits.BasicLimitInformation.LimitFlags |=
        JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.JobMemoryLimit = static_cast<SIZE_T>(std::min<std::uint64_t>(
        profile->limits.memory_bytes, std::numeric_limits<SIZE_T>::max()));
    limits.BasicLimitInformation.ActiveProcessLimit =
        profile->start_programs ? std::max<DWORD>(1, profile->limits.processes)
                                : 1;
    bool limited = ::SetInformationJobObject(made.job.get(),
                                             JobObjectExtendedLimitInformation,
                                             &limits, sizeof(limits)) != 0;
    // A hard cap on processor time, in hundredths of a percent of all
    // processors.
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
    rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE |
                        JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    rate.CpuRate = std::clamp<DWORD>(profile->limits.cpu_percent, 1, 100) * 100;
    limited = ::SetInformationJobObject(made.job.get(),
                                        JobObjectCpuRateControlInformation,
                                        &rate, sizeof(rate)) != 0 &&
              limited;
    if (limited)
        made.report.applied.push_back(sandbox_part::limits);

    // No desktop, clipboard, display settings, global atoms, handles of
    // windows outside the job, system parameters or shutting Windows down.
    JOBOBJECT_BASIC_UI_RESTRICTIONS restrictions{};
    restrictions.UIRestrictionsClass =
        JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
        JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
        JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
        JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (::SetInformationJobObject(made.job.get(), JobObjectBasicUIRestrictions,
                                  &restrictions, sizeof(restrictions)) != 0)
        made.report.applied.push_back(sandbox_part::user_interface);
    return made;
}

/// The exit code a process killed by the host is given.
constexpr UINT killed_code = 1;

/// How long the destructor waits for a killed process to be gone.
constexpr DWORD gone_limit_ms = 5000;

class windows_process final : public plugin_process::impl
{
public:
    windows_process(owned_handle process, confined_job job,
                    owned_handle channel, owned_handle output, DWORD id)
        : process_(std::move(process)), job_(std::move(job.job)),
          channel_(std::move(channel)), output_(std::move(output)), id_(id),
          report_(std::move(job.report))
    {
    }

    sandbox_report sandbox() const override { return report_; }

    ~windows_process() override
    {
        kill();
        job_.reset();
        ::WaitForSingleObject(process_.get(), gone_limit_ms);
    }

    std::uint64_t id() const noexcept override { return id_; }

    native_handle channel() const noexcept override
    {
        return channel_.valid() ? number_of(channel_.get()) : no_handle;
    }

    native_handle output() const noexcept override
    {
        return output_.valid() ? number_of(output_.get()) : no_handle;
    }

    native_handle take_channel() noexcept override
    {
        return channel_.valid() ? number_of(channel_.release()) : no_handle;
    }

    native_handle take_output() noexcept override
    {
        return output_.valid() ? number_of(output_.release()) : no_handle;
    }

    bool request_stop() noexcept override { return false; }

    void kill() noexcept override
    {
        // A process that ended by itself keeps the way it ended.
        if (poll().has_value())
            return;
        killed_ = true;
        ::TerminateJobObject(job_.get(), killed_code);
    }

    std::optional<exit_status> poll() noexcept override
    {
        return wait(std::chrono::milliseconds{0});
    }

    std::optional<exit_status>
    wait(std::chrono::milliseconds limit) noexcept override
    {
        if (ended_.has_value())
            return ended_;
        const auto most =
            std::chrono::milliseconds{std::numeric_limits<DWORD>::max() - 1};
        const auto bounded =
            std::clamp(limit, std::chrono::milliseconds{0}, most);
        if (::WaitForSingleObject(process_.get(),
                                  static_cast<DWORD>(bounded.count())) !=
            WAIT_OBJECT_0)
            return std::nullopt;
        DWORD code = 0;
        ::GetExitCodeProcess(process_.get(), &code);
        exit_status status;
        status.code = code;
        // An error NTSTATUS as the exit code is an exception that nothing
        // handled.
        status.abnormal = killed_ || code >= 0xC0000000UL;
        ended_ = status;
        // What the plugin started and left behind goes with it.
        ::TerminateJobObject(job_.get(), killed_code);
        return ended_;
    }

private:
    owned_handle process_;
    owned_handle job_;
    owned_handle channel_;
    owned_handle output_;
    DWORD id_ = 0;
    sandbox_report report_;
    bool killed_ = false;
    std::optional<exit_status> ended_;
};

/// A system directory as UTF-8, asked of one of the two calls below.
std::string directory_from(UINT(WINAPI* ask)(LPWSTR, UINT))
{
    std::wstring text(MAX_PATH, L'\0');
    const UINT size = ask(text.data(), static_cast<UINT>(text.size()));
    if (size == 0 || size >= text.size())
        return {};
    text.resize(size);
    return narrow(text);
}

} // namespace

result<std::unique_ptr<plugin_process::impl>>
start_process(const process_request& request)
{
    const auto sid = own_sid_text();
    if (!sid)
        return sid.error();
    // Protected, and one entry: everything for this user.
    const std::wstring sddl = L"D:P(A;;GA;;;" + *sid + L")";
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &raw_descriptor, nullptr) == 0)
        return last_error();
    const std::unique_ptr<void, local_free> descriptor(raw_descriptor);

    auto channel = make_pipe(true, descriptor.get());
    if (!channel)
        return channel.error();
    auto output = make_pipe(false, descriptor.get());
    if (!output)
        return output.error();

    // Standard input: a file that is always at its end.
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    const owned_handle input(
        ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input.valid())
        return last_error();

    std::vector<std::wstring> words{request.program.native()};
    for (const std::string& argument : request.arguments)
    {
        auto word = wide(argument);
        if (!word)
            return make_error_code(plugin_errc::start_failed);
        words.push_back(std::move(*word));
    }
    std::wstring command_line = detail::windows_command_line(words);
    // The longest command line CreateProcess takes.
    if (command_line.size() > 32766)
        return make_error_code(plugin_errc::start_failed);

    auto environment =
        environment_block(request.environment, channel->plugin.get());
    if (!environment)
        return make_error_code(plugin_errc::start_failed);

    handle_list inherited(
        {input.get(), output->plugin.get(), channel->plugin.get()});
    if (const error_code failed = inherited.prepare())
        return failed;

    auto job = make_job(request.sandbox ? &*request.sandbox : nullptr);
    if (!job)
        return job.error();

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = output->plugin.get();
    startup.StartupInfo.hStdError = output->plugin.get();
    startup.lpAttributeList = inherited.list();

    // Suspended, so that it is in the job before it runs anything.
    PROCESS_INFORMATION information{};
    if (::CreateProcessW(request.program.c_str(), command_line.data(), nullptr,
                         nullptr, TRUE,
                         CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                         environment->data(), request.working_directory.c_str(),
                         &startup.StartupInfo, &information) == 0)
        return last_error();
    owned_handle process(information.hProcess);
    const owned_handle thread(information.hThread);

    if (::AssignProcessToJobObject(job->job.get(), process.get()) == 0)
    {
        const error_code failed = last_error();
        ::TerminateProcess(process.get(), killed_code);
        ::WaitForSingleObject(process.get(), gone_limit_ms);
        return failed;
    }
    if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1))
    {
        const error_code failed = last_error();
        ::TerminateJobObject(job->job.get(), killed_code);
        ::WaitForSingleObject(process.get(), gone_limit_ms);
        return failed;
    }

    // The plugin's ends are the plugin's alone now.  They close here as
    // `channel`, `output` and `input` go out of scope, and that is what
    // lets the host see the end of the output when the plugin has gone.
    return std::unique_ptr<plugin_process::impl>(
        std::make_unique<windows_process>(
            std::move(process), std::move(*job), std::move(channel->host),
            std::move(output->host), information.dwProcessId));
}

sandbox_report available_sandbox()
{
    sandbox_report report;
    report.applied = {sandbox_part::containment, sandbox_part::user_interface,
                      sandbox_part::limits};
    return report;
}

std::vector<std::pair<std::string, std::string>> system_environment()
{
    const std::string system = directory_from(&::GetSystemDirectoryW);
    const std::string windows = directory_from(&::GetSystemWindowsDirectoryW);
    return {{"PATH", system + ";" + windows}, {"SystemRoot", windows}};
}

} // namespace vss::plugin::host::platform
