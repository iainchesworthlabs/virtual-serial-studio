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
// Closing or ending the job ends everything in it.  With a sandbox profile
// the job also holds the limits and takes the user interface away, and
// the process runs in an AppContainer: an identity that can reach the
// plugin's own directory and its data directory, because this code grants
// it those, and no other file of the user, no network unless granted, and
// no other process.
//

#include "platform/process_platform.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/host/process.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/policy.hpp>

#include <windows.h>

#include <aclapi.h>
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
#include <filesystem>
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

/// The environment of a request, with the variables Windows insists on for
/// a process in an AppContainer added where the request does not have
/// them: without them it refuses to start the process.  They name the
/// plugin's data directory; nothing is taken from the host's environment.
std::vector<std::pair<std::string, std::string>>
with_container_variables(const process_request& request)
{
    auto environment = request.environment;
    if (!request.sandbox)
        return environment;
    const std::u8string wide_data = request.sandbox->data_directory.u8string();
    const std::string data(wide_data.begin(), wide_data.end());
    for (const std::string_view name :
         {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "TEMP", "TMP"})
    {
        const bool present = std::ranges::any_of(
            environment,
            [name](const auto& entry)
            {
                return ::_stricmp(entry.first.c_str(),
                                  std::string(name).c_str()) == 0;
            });
        if (!present)
            environment.emplace_back(std::string(name), data);
    }
    return environment;
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

/// The parts of Windows that an AppContainer needs came with Windows 8,
/// and this project is built against the interface of Windows 7.  So the
/// two functions are looked up when they are needed, and the two numbers
/// are written here as the Windows headers define them for later versions.
/// On a Windows without them nothing of a container is applied, and the
/// report says so.
struct container_functions
{
    using create_profile = HRESULT(WINAPI*)(PCWSTR, PCWSTR, PCWSTR,
                                            PSID_AND_ATTRIBUTES, DWORD, PSID*);
    using derive_sid = HRESULT(WINAPI*)(PCWSTR, PSID*);

    create_profile create = nullptr;
    derive_sid derive = nullptr;

    bool found() const noexcept
    {
        return create != nullptr && derive != nullptr;
    }
};

container_functions find_container_functions() noexcept
{
    container_functions functions;
    const HMODULE library =
        ::LoadLibraryExW(L"userenv.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library == nullptr)
        return functions;
    functions.create = reinterpret_cast<container_functions::create_profile>(
        ::GetProcAddress(library, "CreateAppContainerProfile"));
    functions.derive = reinterpret_cast<container_functions::derive_sid>(
        ::GetProcAddress(library, "DeriveAppContainerSidFromAppContainerName"));
    return functions;
}

/// PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES.
constexpr DWORD_PTR security_capabilities_attribute = 0x00020009;

/// PROCESS_CREATION_MITIGATION_POLICY_WIN32K_SYSTEM_CALL_DISABLE_ALWAYS_ON.
constexpr DWORD64 win32k_calls_off = DWORD64{1} << 28U;

struct sid_free
{
    void operator()(void* sid) const noexcept { ::FreeSid(sid); }
};

/// The AppContainer of one plugin: an identity of its own that Windows
/// gives no file, no network and no other process unless something grants
/// it.
struct app_container
{
    std::unique_ptr<void, sid_free> sid;
    /// The well-known SIDs of the capabilities granted, and the list that
    /// points at them.
    std::vector<std::vector<std::byte>> capability_sids;
    std::vector<SID_AND_ATTRIBUTES> capabilities;

    SECURITY_CAPABILITIES security() noexcept
    {
        SECURITY_CAPABILITIES result{};
        result.AppContainerSid = sid.get();
        result.Capabilities =
            capabilities.empty() ? nullptr : capabilities.data();
        result.CapabilityCount = static_cast<DWORD>(capabilities.size());
        return result;
    }
};

/// The name of a plugin's AppContainer: fixed letters and a hash of the
/// plugin's name, so that any name gives a name Windows takes, and the
/// same plugin gets the same container at every start.
std::wstring container_name(const std::string& name)
{
    const sha512_digest digest = sha512_of(std::string_view(name));
    constexpr std::wstring_view digits = L"0123456789abcdef";
    std::wstring text = L"vss.plugin.";
    for (std::size_t i = 0; i < 20; ++i)
    {
        const auto byte = static_cast<unsigned>(digest[i]);
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0fU]);
    }
    return text;
}

/// Lets the container reach a directory and everything in it.
error_code grant(const std::filesystem::path& directory, PSID sid, DWORD access)
{
    std::wstring path = directory.native();
    PACL old_list = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    DWORD status = ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION, nullptr,
                                           nullptr, &old_list, nullptr, &raw);
    if (status != ERROR_SUCCESS)
        return system_error(status);
    const std::unique_ptr<void, local_free> descriptor(raw);

    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = access;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    PACL new_list = nullptr;
    status = ::SetEntriesInAclW(1, &entry, old_list, &new_list);
    if (status != ERROR_SUCCESS)
        return system_error(status);
    const std::unique_ptr<void, local_free> owner(new_list);
    status = ::SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION, nullptr,
                                     nullptr, new_list, nullptr);
    return status == ERROR_SUCCESS ? error_code{} : system_error(status);
}

/// Adds a capability of the kind Windows has a fixed SID for.
error_code add_capability(app_container& container, WELL_KNOWN_SID_TYPE kind)
{
    std::vector<std::byte> sid(SECURITY_MAX_SID_SIZE);
    DWORD size = static_cast<DWORD>(sid.size());
    if (::CreateWellKnownSid(kind, nullptr, sid.data(), &size) == 0)
        return last_error();
    container.capability_sids.push_back(std::move(sid));
    SID_AND_ATTRIBUTES entry{};
    entry.Sid = container.capability_sids.back().data();
    entry.Attributes = SE_GROUP_ENABLED;
    container.capabilities.push_back(entry);
    return {};
}

/// Makes the container of a profile and lets it reach the directories of
/// the profile and nothing else.  The plugin's own directory can be read
/// and run from; its data directory and the directories the user chose
/// for writing can be changed.
result<app_container> prepare_container(const sandbox_profile& profile)
{
    if (profile.name.empty())
        return make_error_code(plugin_errc::start_failed);
    app_container container;
    PSID raw = nullptr;
    const container_functions functions = find_container_functions();
    if (!functions.found())
        return make_error_code(plugin_errc::sandbox_insufficient);
    // Windows starts a process in a container only when the container is
    // registered for this user.  It is registered once, under its name, and
    // found again at every later start.
    const std::wstring name = container_name(profile.name);
    HRESULT made = functions.create(name.c_str(), name.c_str(),
                                    L"A plugin of Virtual Serial Studio",
                                    nullptr, 0, &raw);
    if (made == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        made = functions.derive(name.c_str(), &raw);
    if (FAILED(made) || raw == nullptr)
        return make_error_code(plugin_errc::sandbox_insufficient);
    container.sid.reset(raw);

    // Read and run.  FILE_EXECUTE is the one right of "generic execute"
    // that "generic read" does not already hold.
    constexpr DWORD read = FILE_GENERIC_READ | FILE_EXECUTE;
    // Everything but changing who may reach the directory.
    constexpr DWORD change = FILE_ALL_ACCESS & ~(WRITE_DAC | WRITE_OWNER);
    if (const error_code failed =
            grant(profile.plugin_directory, container.sid.get(), read))
        return failed;
    if (const error_code failed =
            grant(profile.data_directory, container.sid.get(), change))
        return failed;
    for (const auto& directory : profile.read_directories)
    {
        if (const error_code failed =
                grant(directory, container.sid.get(), read))
            return failed;
    }
    for (const auto& directory : profile.write_directories)
    {
        if (const error_code failed =
                grant(directory, container.sid.get(), change))
            return failed;
    }

    // Without a capability an AppContainer has no network at all, the
    // computer's own addresses included.
    if (profile.network_outbound || profile.network_listen)
    {
        // The capability list must not move once entries point into it.
        container.capability_sids.reserve(3);
        const WELL_KNOWN_SID_TYPE reach =
            profile.network_listen ? WinCapabilityInternetClientServerSid
                                   : WinCapabilityInternetClientSid;
        if (const error_code failed = add_capability(container, reach))
            return failed;
        if (const error_code failed = add_capability(
                container, WinCapabilityPrivateNetworkClientServerSid))
            return failed;
    }
    return container;
}

/// The start-up attributes of a plugin's process: the handles it inherits,
/// and with a container the identity it runs as and the closing of the
/// window system's calls to it.
class start_attributes
{
public:
    start_attributes(std::vector<HANDLE> handles, app_container* container)
        : handles_(std::move(handles)), container_(container)
    {
    }
    ~start_attributes()
    {
        if (ready_)
            ::DeleteProcThreadAttributeList(list());
    }
    start_attributes(const start_attributes&) = delete;
    start_attributes& operator=(const start_attributes&) = delete;

    error_code prepare()
    {
        const DWORD count = container_ != nullptr ? 3 : 1;
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, count, 0, &size);
        if (size == 0)
            return last_error();
        storage_.resize(size);
        if (::InitializeProcThreadAttributeList(list(), count, 0, &size) == 0)
            return last_error();
        ready_ = true;
        if (::UpdateProcThreadAttribute(
                list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                static_cast<void*>(handles_.data()),
                handles_.size() * sizeof(HANDLE), nullptr, nullptr) == 0)
            return last_error();
        if (container_ == nullptr)
            return {};
        security_ = container_->security();
        if (::UpdateProcThreadAttribute(
                list(), 0, security_capabilities_attribute, &security_,
                sizeof(security_), nullptr, nullptr) == 0)
            return last_error();
        // A plugin has no window, so it has no use for the calls of the
        // window system, which are a large part of what the kernel offers.
        policy_ = win32k_calls_off;
        if (::UpdateProcThreadAttribute(
                list(), 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &policy_,
                sizeof(policy_), nullptr, nullptr) == 0)
            return last_error();
        return {};
    }

    LPPROC_THREAD_ATTRIBUTE_LIST list() noexcept
    {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    }

private:
    std::vector<HANDLE> handles_;
    app_container* container_ = nullptr;
    SECURITY_CAPABILITIES security_{};
    DWORD64 policy_ = 0;
    std::vector<std::byte> storage_;
    bool ready_ = false;
};

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

    auto environment = environment_block(with_container_variables(request),
                                         channel->plugin.get());
    if (!environment)
        return make_error_code(plugin_errc::start_failed);

    auto job = make_job(request.sandbox ? &*request.sandbox : nullptr);
    if (!job)
        return job.error();

    // The container, when a profile asks for one and it can be made.  If
    // it cannot, or the program does not start in it, the start goes on
    // without it and the report says so, unless that is less than the
    // caller will take.
    std::optional<app_container> container;
    if (request.sandbox)
    {
        auto made = prepare_container(*request.sandbox);
        if (made)
            container = std::move(*made);
        else
            job->report.failure = made.error();
    }

    PROCESS_INFORMATION information{};
    for (;;)
    {
        sandbox_report report = job->report;
        if (container)
        {
            for (const sandbox_part part :
                 {sandbox_part::file_system, sandbox_part::network,
                  sandbox_part::other_processes, sandbox_part::system_calls})
                report.applied.push_back(part);
        }
        if (report.level() < request.least)
            return make_error_code(plugin_errc::sandbox_insufficient);

        start_attributes attributes(
            {input.get(), output->plugin.get(), channel->plugin.get()},
            container ? &*container : nullptr);
        error_code failed = attributes.prepare();
        if (!failed)
        {
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = input.get();
            startup.StartupInfo.hStdOutput = output->plugin.get();
            startup.StartupInfo.hStdError = output->plugin.get();
            startup.lpAttributeList = attributes.list();

            // Suspended, so that it is in the job before it runs anything.
            std::wstring line = command_line;
            if (::CreateProcessW(
                    request.program.c_str(), line.data(), nullptr, nullptr,
                    TRUE,
                    CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                    environment->data(), request.working_directory.c_str(),
                    &startup.StartupInfo, &information) == 0)
                failed = last_error();
        }
        if (!failed)
        {
            job->report = std::move(report);
            break;
        }
        if (!container)
            return failed;
        job->report.failure = failed;
        container.reset();
    }
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
    if (find_container_functions().found())
    {
        report.applied.assign(std::begin(all_sandbox_parts),
                              std::end(all_sandbox_parts));
    }
    else
    {
        report.applied = {sandbox_part::containment,
                          sandbox_part::user_interface, sandbox_part::limits};
    }
    return report;
}

std::vector<std::pair<std::string, std::string>> system_environment()
{
    const std::string system = directory_from(&::GetSystemDirectoryW);
    const std::string windows = directory_from(&::GetSystemWindowsDirectoryW);
    return {{"PATH", system + ";" + windows}, {"SystemRoot", windows}};
}

} // namespace vss::plugin::host::platform
