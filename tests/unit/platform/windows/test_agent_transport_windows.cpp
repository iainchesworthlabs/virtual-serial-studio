//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent's pipe on Windows: its name, and the access list that lets this
// user in and nobody else.  The access list is read back from a client handle
// of the pipe, which is what another program would be able to see.  Refusing a
// second real user needs a second account and is for a guest machine.
//

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/core/error.hpp>

#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on

#include <atomic>
#include <cstddef>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agent = vss::agent;

namespace
{

vss::error_code code(agent::protocol_errc value)
{
    return agent::make_error_code(value);
}

/// A name no other agent has.
std::string unique_instance_name()
{
    static const unsigned random = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    std::ostringstream out;
    out << "tw-" << std::hex << random << '-' << std::dec
        << counter.fetch_add(1);
    return out.str();
}

struct handle_closer
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
};
using handle_owner = std::unique_ptr<void, handle_closer>;

/// Memory that Windows allocated and `LocalFree` returns.
struct local_releaser
{
    void operator()(void* memory) const noexcept
    {
        if (memory != nullptr)
            ::LocalFree(memory);
    }
};
using local_owner = std::unique_ptr<void, local_releaser>;

std::wstring to_wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int length = static_cast<int>(text.size());
    const int size =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    static_cast<void>(::MultiByteToWideChar(CP_UTF8, 0, text.data(), length,
                                            out.data(), size));
    return out;
}

/// The user of this process, as the bytes of a TOKEN_USER.
std::vector<unsigned char> current_user()
{
    HANDLE raw = nullptr;
    REQUIRE(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw) != 0);
    const handle_owner token(raw);
    DWORD size = 0;
    static_cast<void>(
        ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size));
    REQUIRE(size > 0);
    std::vector<unsigned char> buffer(size);
    REQUIRE(::GetTokenInformation(token.get(), TokenUser, buffer.data(), size,
                                  &size) != 0);
    return buffer;
}

PSID sid_of(const std::vector<unsigned char>& user)
{
    return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
}

std::wstring sid_text(PSID sid)
{
    LPWSTR raw = nullptr;
    REQUIRE(::ConvertSidToStringSidW(sid, &raw) != 0);
    const local_owner release(raw);
    return std::wstring(raw);
}

} // namespace

TEST_CASE("agent transport: the pipe is named for the user and the instance")
{
    const std::string instance = unique_instance_name();
    boost::asio::io_context io;
    auto listener = agent::listen_local(io.get_executor(), instance);
    REQUIRE(listener);

    const std::string address = (*listener)->address();
    REQUIRE(
        std::string_view(address).starts_with(R"(\\.\pipe\vss-agent-S-1-)"));
    REQUIRE(std::string_view(address).ends_with("-" + instance));

    // Between the two is the SID of the user who runs this process.
    const auto user = current_user();
    const std::wstring expected = L"\\\\.\\pipe\\vss-agent-" +
                                  sid_text(sid_of(user)) + L"-" +
                                  to_wide(instance);
    REQUIRE(to_wide(address) == expected);
}

namespace
{

/// The pipe of `endpoint` lets this user in and nobody else.
void require_private_pipe(agent::local_endpoint endpoint)
{
    const std::string instance = unique_instance_name();
    boost::asio::io_context io;
    auto listener = agent::listen_local(io.get_executor(), endpoint, instance);
    REQUIRE(listener);

    // What another program would see: open the pipe as a client, with the
    // right to read its security, and read it.
    const std::wstring name = to_wide((*listener)->address());
    const handle_owner pipe(::CreateFileW(
        name.c_str(), READ_CONTROL | GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr));
    REQUIRE(pipe.get() != INVALID_HANDLE_VALUE);

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetSecurityInfo(
        pipe.get(), SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, &owner, nullptr,
        &dacl, nullptr, &descriptor);
    const local_owner release(descriptor);
    REQUIRE_EQ(status, DWORD{0});
    REQUIRE(descriptor != nullptr);
    REQUIRE(owner != nullptr);
    REQUIRE(dacl != nullptr);

    // One entry that allows, and it names this user.
    ACL_SIZE_INFORMATION entries{};
    REQUIRE(::GetAclInformation(dacl, &entries, sizeof(entries),
                                AclSizeInformation) != 0);
    REQUIRE_EQ(entries.AceCount, DWORD{1});
    void* found = nullptr;
    REQUIRE(::GetAce(dacl, 0, &found) != 0);
    REQUIRE(found != nullptr);
    const auto* header = static_cast<const ACE_HEADER*>(found);
    REQUIRE_EQ(header->AceType, static_cast<BYTE>(ACCESS_ALLOWED_ACE_TYPE));
    auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(found);
    REQUIRE(allowed->Mask != 0);
    PSID entry_sid = &allowed->SidStart;
    const auto user = current_user();
    REQUIRE(::EqualSid(entry_sid, sid_of(user)) != 0);

    // The list is protected: nothing is inherited into it.
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    REQUIRE(::GetSecurityDescriptorControl(descriptor, &control, &revision) !=
            0);
    REQUIRE((control & SE_DACL_PROTECTED) != 0);
}

} // namespace

TEST_CASE("agent transport: the pipe is open to this user alone")
{
    require_private_pipe(agent::local_endpoint::agent);
}

TEST_CASE("control transport: both pipes of the gateway are open to this "
          "user alone")
{
    require_private_pipe(agent::local_endpoint::control);
    require_private_pipe(agent::local_endpoint::control_admin);
}

TEST_CASE("control transport: the pipes are named for the endpoint, the user "
          "and the instance")
{
    const std::string instance = unique_instance_name();
    const auto user = current_user();
    const std::wstring sid = sid_text(sid_of(user));
    const auto control =
        agent::local_address(agent::local_endpoint::control, instance);
    const auto admin =
        agent::local_address(agent::local_endpoint::control_admin, instance);
    REQUIRE(control);
    REQUIRE(admin);
    REQUIRE(to_wide(*control) == L"\\\\.\\pipe\\vss-control-" + sid + L"-" +
                                     to_wide(instance));
    REQUIRE(to_wide(*admin) == L"\\\\.\\pipe\\vss-control_admin-" + sid +
                                   L"-" + to_wide(instance));
}

TEST_CASE("agent transport: a second listener on a name in use is refused")
{
    const std::string instance = unique_instance_name();
    boost::asio::io_context io;
    auto first = agent::listen_local(io.get_executor(), instance);
    REQUIRE(first);

    const auto second = agent::listen_local(io.get_executor(), instance);
    REQUIRE(!second);
    REQUIRE_EQ(second.error(), code(agent::protocol_errc::already_running));

    // A name that is free is not refused.
    const auto other =
        agent::listen_local(io.get_executor(), unique_instance_name());
    REQUIRE(other);
}
