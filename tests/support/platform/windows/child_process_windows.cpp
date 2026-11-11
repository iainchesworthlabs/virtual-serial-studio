//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `child_process` on Windows: CreateProcessW with an anonymous pipe for
// standard output and standard error, PeekNamedPipe for timed reads,
// WaitForSingleObject for the exit code.  Only the pipe's write end and a NUL
// standard input are inherited by the child.
//

#include "child_process.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vss_test_support
{

namespace
{

using steady = std::chrono::steady_clock;

/// The longest any wait lasts, so that a huge limit cannot overflow a clock.
constexpr std::chrono::milliseconds longest_wait{std::chrono::hours{24}};

/// How long a timed read sleeps between two looks at the pipe.
constexpr int poll_step_ms = 10;

/// How long `kill` waits for a terminated process to be gone.
constexpr DWORD kill_wait_ms = 10000;

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

std::string error_text(DWORD code)
{
    std::array<char, 256> message;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        code, 0, message.data(), static_cast<DWORD>(message.size()), nullptr);
    std::string text = "Windows error " + std::to_string(code);
    if (length == 0)
        return text;
    std::string detail(message.data(), static_cast<std::size_t>(length));
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r' ||
                               detail.back() == ' '))
    {
        detail.pop_back();
    }
    return text + " (" + detail + ")";
}

[[noreturn]] void fail(const std::string& what)
{
    throw std::runtime_error(what + ": " + error_text(::GetLastError()));
}

/// A Windows handle that closes itself.  Both null and INVALID_HANDLE_VALUE
/// mean "no handle".
class handle
{
public:
    handle() = default;
    explicit handle(HANDLE value) noexcept
        : value_(value == INVALID_HANDLE_VALUE ? nullptr : value)
    {
    }
    ~handle() { reset(); }

    handle(handle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr))
    {
    }

    handle& operator=(handle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    HANDLE get() const noexcept { return value_; }
    bool is_open() const noexcept { return value_ != nullptr; }

    void reset() noexcept
    {
        if (value_ != nullptr)
            static_cast<void>(::CloseHandle(value_));
        value_ = nullptr;
    }

private:
    HANDLE value_ = nullptr;
};

/// A process attribute list that restricts which handles the child inherits.
class attribute_list
{
public:
    attribute_list() = default;
    ~attribute_list()
    {
        if (ready_)
            ::DeleteProcThreadAttributeList(list_);
    }

    attribute_list(const attribute_list&) = delete;
    attribute_list& operator=(const attribute_list&) = delete;

    /// `inherited` must outlive the CreateProcessW call that uses the list.
    void restrict_to(std::vector<HANDLE>& inherited)
    {
        SIZE_T size = 0;
        static_cast<void>(
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size));
        if (size == 0)
            fail("cannot size the child's attribute list");
        storage_.resize(size);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!::InitializeProcThreadAttributeList(list_, 1, 0, &size))
            fail("cannot create the child's attribute list");
        ready_ = true;
        if (!::UpdateProcThreadAttribute(
                list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                static_cast<void*>(inherited.data()),
                inherited.size() * sizeof(HANDLE), nullptr, nullptr))
        {
            fail("cannot restrict the child's inherited handles");
        }
    }

    LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
    bool ready_ = false;
};

std::wstring widen(const std::string& text)
{
    if (text.empty())
        return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("argument too long");
    const int size = static_cast<int>(text.size());
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             text.data(), size, nullptr, 0);
    if (needed <= 0)
        fail("cannot convert '" + text + "' from UTF-8");
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size,
                              wide.data(), needed) != needed)
    {
        fail("cannot convert '" + text + "' from UTF-8");
    }
    return wide;
}

/// Appends `argument` so that CommandLineToArgvW (and the C runtime) reads it
/// back unchanged: quote it when needed, double the backslashes that precede
/// a quotation mark, and escape the quotation marks.
void append_argument(std::wstring& line, const std::wstring& argument)
{
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        line += argument;
        return;
    }

    line += L'"';
    std::size_t backslashes = 0;
    for (const wchar_t letter : argument)
    {
        if (letter == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (letter == L'"')
            line.append(backslashes * 2 + 1, L'\\');
        else
            line.append(backslashes, L'\\');
        line += letter;
        backslashes = 0;
    }
    line.append(backslashes * 2, L'\\');
    line += L'"';
}

} // namespace

class child_process::impl
{
public:
    impl() = default;
    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    ~impl() { terminate(); }

    /// Reads the exit code of a process that has been seen to end.
    bool collect_exit() noexcept
    {
        DWORD code = 0;
        if (!::GetExitCodeProcess(process_.get(), &code))
            return false;
        exit_code_ = static_cast<int>(code);
        exited_ = true;
        return true;
    }

    /// TerminateProcess, then wait.
    void terminate() noexcept
    {
        if (exited_ || !process_.is_open())
            return;
        static_cast<void>(::TerminateProcess(process_.get(), 1));
        if (::WaitForSingleObject(process_.get(), kill_wait_ms) ==
            WAIT_OBJECT_0)
        {
            static_cast<void>(collect_exit());
        }
    }

    /// Waits for more output until `deadline`.  True when bytes were appended
    /// to `buffer_`; false on timeout or when the output has ended.
    bool fill(steady::time_point deadline)
    {
        // Output written before the child exited stays in the pipe, so once
        // the process is seen to be gone the pipe is only drained.  Waiting on
        // the process handle gives a bounded sleep that ends early on exit.
        bool process_gone = exited_;
        while (output_.is_open())
        {
            DWORD available = 0;
            if (!::PeekNamedPipe(output_.get(), nullptr, 0, nullptr, &available,
                                 nullptr))
            {
                // Broken pipe: every writer has gone and nothing is left.
                output_.reset();
                return false;
            }
            if (available > 0)
            {
                std::array<char, 4096> chunk;
                const DWORD wanted = std::min<DWORD>(
                    available, static_cast<DWORD>(chunk.size()));
                DWORD got = 0;
                if (!::ReadFile(output_.get(), chunk.data(), wanted, &got,
                                nullptr) ||
                    got == 0)
                {
                    output_.reset();
                    return false;
                }
                buffer_.append(chunk.data(), static_cast<std::size_t>(got));
                return true;
            }

            const DWORD step =
                static_cast<DWORD>(milliseconds_until(deadline, poll_step_ms));
            if (step == 0)
                return false;
            if (process_gone)
            {
                // Something else (a grandchild) still holds the pipe open.
                ::Sleep(step);
            }
            else
            {
                const DWORD state = ::WaitForSingleObject(process_.get(), step);
                process_gone = state == WAIT_OBJECT_0;
            }
        }
        return false;
    }

private:
    friend class child_process;

    handle process_;
    handle output_;
    DWORD id_ = 0;
    bool exited_ = false;
    int exit_code_ = 0;
    std::string buffer_;
};

child_process::child_process(const std::string& program,
                             const std::vector<std::string>& arguments)
    : impl_(std::make_unique<impl>())
{
    std::wstring command_line;
    append_argument(command_line, widen(program));
    for (const auto& argument : arguments)
    {
        command_line += L' ';
        append_argument(command_line, widen(argument));
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = static_cast<DWORD>(sizeof inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE read_raw = nullptr;
    HANDLE write_raw = nullptr;
    if (!::CreatePipe(&read_raw, &write_raw, &inheritable, 0))
        fail("cannot create a pipe");
    handle read_end(read_raw);
    handle write_end(write_raw);
    // The parent's end must not leak into the child.
    if (!::SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0))
        fail("cannot configure a pipe");

    handle null_input(::CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!null_input.is_open())
        fail("cannot open NUL");

    std::vector<HANDLE> inherited{null_input.get(), write_end.get()};
    attribute_list attributes;
    attributes.restrict_to(inherited);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = static_cast<DWORD>(sizeof startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.get();
    startup.StartupInfo.hStdOutput = write_end.get();
    startup.StartupInfo.hStdError = write_end.get();
    startup.lpAttributeList = attributes.get();

    // A new process group lets GenerateConsoleCtrlEvent address the child.
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP;
    PROCESS_INFORMATION started{};
    if (!::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                          flags, nullptr, nullptr, &startup.StartupInfo,
                          &started))
    {
        fail("cannot start '" + program + "'");
    }

    handle process(started.hProcess);
    handle thread(started.hThread);
    thread.reset();
    write_end.reset();
    null_input.reset();

    impl_->id_ = started.dwProcessId;
    impl_->process_ = std::move(process);
    impl_->output_ = std::move(read_end);
}

child_process::~child_process()
{
    if (impl_)
        impl_->terminate();
}

std::optional<int> child_process::wait(std::chrono::milliseconds limit)
{
    if (impl_->exited_)
        return impl_->exit_code_;
    const DWORD milliseconds = static_cast<DWORD>(
        std::clamp<std::int64_t>(limit.count(), 0, 0x7fffffff));
    if (::WaitForSingleObject(impl_->process_.get(), milliseconds) ==
            WAIT_OBJECT_0 &&
        impl_->collect_exit())
    {
        return impl_->exit_code_;
    }
    return std::nullopt;
}

void child_process::kill()
{
    impl_->terminate();
}

bool child_process::request_stop()
{
    if (impl_->exited_)
        return false;
    return ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, impl_->id_) != 0;
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
    return static_cast<std::uint64_t>(impl_->id_);
}

} // namespace vss_test_support
