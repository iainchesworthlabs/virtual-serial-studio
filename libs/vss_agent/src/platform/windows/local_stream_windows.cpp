//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent's transport on Windows: a named pipe that only the user who runs
// the agent can open.
//
//   name     \\.\pipe\vss-agent-<SID of the user>[-<instance>], with
//            `control` or `control_admin` in place of `agent` for the
//            gateway's two endpoints
//   access   D:P(A;;GA;;;<SID>): the user and nobody else, not even
//            administrators or SYSTEM, and nothing inherited
//   remote   PIPE_REJECT_REMOTE_CLIENTS: this computer only
//   one      FILE_FLAG_FIRST_PIPE_INSTANCE on the first instance: a second
//            agent, or a program that made the name first, is found out
//
// The name can be guessed, so a client does not trust it: it asks Windows
// which process serves the pipe and checks that the process runs as the same
// user (see `connect_local`).
//

#include "platform/local_stream_platform.hpp"

#include <vss/agent/proto/error.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/windows/overlapped_ptr.hpp>
#include <boost/asio/windows/stream_handle.hpp>

#include <windows.h>

#include <sddl.h>

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vss::agent::platform
{
namespace
{

struct handle_closer
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
};
using handle_owner = std::unique_ptr<void, handle_closer>;

struct local_releaser
{
    void operator()(void* memory) const noexcept
    {
        if (memory != nullptr)
            ::LocalFree(memory);
    }
};
using local_owner = std::unique_ptr<void, local_releaser>;

error_code system_error(DWORD value)
{
    return {static_cast<int>(value), boost::system::system_category()};
}

std::string to_utf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), out.data(), size,
                          nullptr, nullptr);
    return out;
}

/// The user of a token, as the bytes of a TOKEN_USER; empty when unreadable.
std::vector<unsigned char> token_user(HANDLE token)
{
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0)
        return {};
    std::vector<unsigned char> buffer(size);
    if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) ==
        0)
        return {};
    return buffer;
}

std::vector<unsigned char> process_user(HANDLE process)
{
    HANDLE raw = nullptr;
    if (::OpenProcessToken(process, TOKEN_QUERY, &raw) == 0)
        return {};
    const handle_owner token(raw);
    return token_user(token.get());
}

PSID sid_of(const std::vector<unsigned char>& user)
{
    return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
}

/// The SID of the user who runs this process, as text (`S-1-5-21-...`).
result<std::wstring> own_sid_text()
{
    const auto user = process_user(::GetCurrentProcess());
    if (user.empty())
        return system_error(::GetLastError());
    LPWSTR raw = nullptr;
    if (::ConvertSidToStringSidW(sid_of(user), &raw) == 0)
        return system_error(::GetLastError());
    const local_owner text(raw);
    return std::wstring(raw);
}

result<std::wstring> pipe_name(local_endpoint endpoint,
                               std::string_view instance)
{
    const auto sid = own_sid_text();
    if (!sid)
        return sid.error();
    std::wstring name = L"\\\\.\\pipe\\vss-";
    // The word of an endpoint is ASCII.
    for (const char letter : endpoint_word(endpoint))
        name += static_cast<wchar_t>(letter);
    name += L'-';
    name += *sid;
    if (!instance.empty())
    {
        name += L'-';
        // An instance name is ASCII letters, digits, `_` and `-`.
        for (const char letter : instance)
            name += static_cast<wchar_t>(letter);
    }
    return name;
}

/// The SID in `user` as text; empty when it cannot be written.
std::string sid_text(const std::vector<unsigned char>& user)
{
    LPWSTR raw = nullptr;
    if (::ConvertSidToStringSidW(sid_of(user), &raw) == 0)
        return {};
    const local_owner text(raw);
    return to_utf8(raw);
}

/// The path of the program that process `process` runs; empty when it cannot
/// be read.  For a log only: the number may belong to another process by now.
std::string process_program(DWORD process)
{
    if (process == 0)
        return {};
    const handle_owner opened(
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process));
    if (!opened)
        return {};
    constexpr std::size_t longest_path = 32768;
    std::wstring path(longest_path, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (::QueryFullProcessImageNameW(opened.get(), 0, path.data(), &size) == 0)
        return {};
    path.resize(size);
    return to_utf8(path);
}

/// The user who opened the client end of the pipe behind `pipe`, read from
/// the security context Windows recorded when the client opened it.  The
/// thread acts as the client only for the one call that reads the token, and
/// a client opens the pipe at identification level, so even that moment
/// gives this process nothing but the client's name.
result<std::vector<unsigned char>> client_user(HANDLE pipe)
{
    if (::ImpersonateNamedPipeClient(pipe) == 0)
        return system_error(::GetLastError());
    HANDLE raw = nullptr;
    const BOOL opened =
        ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &raw);
    const DWORD failed = ::GetLastError();
    // A thread that cannot stop acting as someone else must not run on.
    if (::RevertToSelf() == 0)
        std::terminate();
    if (opened == 0)
        return system_error(failed);
    const handle_owner token(raw);
    auto user = token_user(token.get());
    if (user.empty())
        return make_error_code(errc::permission_denied);
    return user;
}

/// True when the process that serves the pipe behind `pipe` runs as the user
/// who runs this one.
bool served_by_own_user(HANDLE pipe)
{
    ULONG server = 0;
    if (::GetNamedPipeServerProcessId(pipe, &server) == 0)
        return false;
    const handle_owner process(
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server));
    if (!process)
        return false;
    const auto theirs = process_user(process.get());
    const auto ours = process_user(::GetCurrentProcess());
    if (theirs.empty() || ours.empty())
        return false;
    return ::EqualSid(sid_of(theirs), sid_of(ours)) != 0;
}

/// Opens the client end of the pipe, or says why not.
result<handle_owner> open_client(const std::wstring& name)
{
    constexpr int attempts = 5;
    constexpr DWORD busy_wait_ms = 1000;
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        // SECURITY_IDENTIFICATION: whatever serves the pipe may learn who
        // this is and may not act as this user.
        handle_owner pipe(
            ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                              SECURITY_IDENTIFICATION,
                          nullptr));
        if (pipe.get() != INVALID_HANDLE_VALUE)
            return pipe;
        static_cast<void>(pipe.release());
        const DWORD failed = ::GetLastError();
        if (failed == ERROR_FILE_NOT_FOUND)
            return make_error_code(protocol_errc::not_running);
        if (failed == ERROR_ACCESS_DENIED)
            return make_error_code(protocol_errc::not_same_user);
        if (failed != ERROR_PIPE_BUSY)
            return system_error(failed);
        // Every instance is taken for a moment; the agent makes another.
        ::WaitNamedPipeW(name.c_str(), busy_wait_ms);
    }
    return make_error_code(protocol_errc::busy);
}

class pipe_stream final : public local_stream
{
public:
    /// `server_end` says which end of the pipe this is: the one a listener
    /// made, or the one a client opened.
    pipe_stream(const boost::asio::any_io_executor& executor, bool server_end)
        : handle_(executor), server_end_(server_end)
    {
    }

    /// Takes the handle.  On failure the handle is closed.
    error_code adopt(handle_owner pipe)
    {
        boost::system::error_code failed;
        handle_.assign(pipe.get(), failed);
        if (!failed)
            static_cast<void>(pipe.release());
        return failed;
    }

    HANDLE native() { return handle_.native_handle(); }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override
    {
        handle_.async_read_some(buffer, std::move(handler));
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        handle_.async_write_some(buffer, std::move(handler));
    }

    void close() override
    {
        boost::system::error_code ignored;
        handle_.close(ignored);
    }

    result<peer_identity> peer() override
    {
        const HANDLE pipe = handle_.native_handle();
        const auto ours = process_user(::GetCurrentProcess());
        if (ours.empty())
            return system_error(::GetLastError());

        peer_identity out;
        ULONG process = 0;
        std::vector<unsigned char> theirs;
        if (server_end_)
        {
            auto user = client_user(pipe);
            if (!user)
                return user.error();
            theirs = std::move(*user);
            if (::GetNamedPipeClientProcessId(pipe, &process) == 0)
                process = 0;
        }
        else
        {
            if (::GetNamedPipeServerProcessId(pipe, &process) == 0)
                return system_error(::GetLastError());
            const handle_owner server(::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process));
            if (!server)
                return system_error(::GetLastError());
            theirs = process_user(server.get());
            if (theirs.empty())
                return make_error_code(errc::permission_denied);
        }
        out.user = sid_text(theirs);
        out.same_user = ::EqualSid(sid_of(theirs), sid_of(ours)) != 0;
        out.process_id = process;
        out.program = process_program(process);
        return out;
    }

private:
    boost::asio::windows::stream_handle handle_;
    const bool server_end_;
};

/// What the listener and an accept that is still waiting share, so the
/// completion of the wait is safe after the listener has gone.
struct listener_state
{
    boost::asio::any_io_executor executor;
    std::wstring name;
    local_owner descriptor;
    /// The instance that waits for the next client.
    std::unique_ptr<pipe_stream> waiting;
    bool closed = false;
};

/// Makes one more instance of the pipe.
result<std::unique_ptr<pipe_stream>> make_instance(const listener_state& state,
                                                   bool first)
{
    constexpr DWORD buffer_size = 64 * 1024;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = state.descriptor.get();
    attributes.bInheritHandle = FALSE;
    const DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                            (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : DWORD{0});
    handle_owner pipe(::CreateNamedPipeW(
        state.name.c_str(), open_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, buffer_size, buffer_size, 0, &attributes));
    if (pipe.get() == INVALID_HANDLE_VALUE)
    {
        static_cast<void>(pipe.release());
        return system_error(::GetLastError());
    }
    auto stream = std::make_unique<pipe_stream>(state.executor, true);
    if (const error_code failed = stream->adopt(std::move(pipe)))
        return failed;
    return stream;
}

class pipe_listener final : public local_listener
{
public:
    explicit pipe_listener(std::shared_ptr<listener_state> state)
        : state_(std::move(state))
    {
    }

    ~pipe_listener() override { stop(); }

    pipe_listener(const pipe_listener&) = delete;
    pipe_listener& operator=(const pipe_listener&) = delete;

    void async_accept(accept_handler handler) override
    {
        auto state = state_;
        if (!state->closed && !state->waiting)
        {
            auto made = make_instance(*state, false);
            if (!made)
            {
                boost::asio::post(
                    state->executor,
                    [handler = std::move(handler), failed = made.error()]
                    { handler(failed, nullptr); });
                return;
            }
            state->waiting = std::move(*made);
        }
        if (state->closed)
        {
            boost::asio::post(
                state->executor, [handler = std::move(handler)]
                { handler(boost::asio::error::operation_aborted, nullptr); });
            return;
        }

        boost::asio::windows::overlapped_ptr operation(
            state->executor,
            [state, handler = std::move(handler)](
                boost::system::error_code failed, std::size_t)
            {
                std::unique_ptr<pipe_stream> connected =
                    std::move(state->waiting);
                if (state->closed)
                {
                    handler(boost::asio::error::operation_aborted, nullptr);
                    return;
                }
                // The next instance is made before this one is handed on or
                // dropped, so the name never stops existing while the agent
                // runs and nobody else can take it.
                if (auto next = make_instance(*state, false))
                    state->waiting = std::move(*next);
                if (failed)
                    handler(failed, nullptr);
                else
                    handler({}, std::move(connected));
            });
        const BOOL connected =
            ::ConnectNamedPipe(state->waiting->native(), operation.get());
        const DWORD last = ::GetLastError();
        if (connected == 0 && last == ERROR_IO_PENDING)
            operation.release();
        else if (connected != 0 || last == ERROR_PIPE_CONNECTED)
            operation.complete({}, 0);
        else
            operation.complete(system_error(last), 0);
    }

    void close() override { stop(); }

    std::string address() const override { return to_utf8(state_->name); }

private:
    void stop() noexcept
    {
        state_->closed = true;
        // Closing the instance ends a wait on it; the completion drops it.
        if (state_->waiting)
            state_->waiting->close();
    }

    std::shared_ptr<listener_state> state_;
};

} // namespace

result<std::string> local_address(local_endpoint endpoint,
                                  std::string_view instance)
{
    const auto name = pipe_name(endpoint, instance);
    if (!name)
        return name.error();
    return to_utf8(*name);
}

result<std::unique_ptr<local_listener>>
listen_local(const boost::asio::any_io_executor& executor,
             local_endpoint endpoint, std::string_view instance)
{
    const auto sid = own_sid_text();
    const auto name = pipe_name(endpoint, instance);
    if (!sid || !name)
        return sid ? name.error() : sid.error();

    auto state = std::make_shared<listener_state>();
    state->executor = executor;
    state->name = *name;

    // Protected, and one entry: everything for this user.
    const std::wstring sddl = L"D:P(A;;GA;;;" + *sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == 0)
        return system_error(::GetLastError());
    state->descriptor.reset(descriptor);

    auto first = make_instance(*state, true);
    if (!first)
    {
        // The name exists.  It is this user's agent when its pipe can be
        // opened and this user serves it; anything else made the name first.
        const auto probe = open_client(*name);
        if ((probe && served_by_own_user(probe->get())) ||
            (!probe && probe.error() == protocol_errc::busy))
            return make_error_code(protocol_errc::already_running);
        if (probe || probe.error() == protocol_errc::not_same_user)
            return make_error_code(errc::permission_denied);
        return first.error();
    }
    state->waiting = std::move(*first);
    return std::unique_ptr<local_listener>(
        std::make_unique<pipe_listener>(std::move(state)));
}

result<std::unique_ptr<local_stream>>
connect_local(const boost::asio::any_io_executor& executor,
              local_endpoint endpoint, std::string_view instance)
{
    const auto name = pipe_name(endpoint, instance);
    if (!name)
        return name.error();
    auto pipe = open_client(*name);
    if (!pipe)
        return pipe.error();
    if (!served_by_own_user(pipe->get()))
        return make_error_code(protocol_errc::not_same_user);
    auto stream = std::make_unique<pipe_stream>(executor, false);
    if (const error_code failed = stream->adopt(std::move(*pipe)))
        return failed;
    return std::unique_ptr<local_stream>(std::move(stream));
}

} // namespace vss::agent::platform
