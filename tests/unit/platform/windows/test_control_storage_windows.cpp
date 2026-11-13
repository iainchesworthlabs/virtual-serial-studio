//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audit log's storage on Windows: the access list of the directory is read
// back, which is what another program would see.  It must be protected and
// hold one entry that allows, naming the user who runs the test.  A directory
// in place of the file is refused.
//

#include <vss/control/audit_log.hpp>
#include <vss/core/error.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace control = vss::control;

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

vss::result<std::unique_ptr<control::audit_log>>
try_open(const std::filesystem::path& directory)
{
    control::audit_options options;
    options.directory = directory;
    return control::audit_log::open(std::move(options));
}

} // namespace

TEST_CASE("control storage: the directory is open to this user alone")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    const auto opened = try_open(directory);
    REQUIRE(opened);

    // What another program would see: the owner and the access list.
    const std::wstring name = directory.wstring();
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(
        name.c_str(), SE_FILE_OBJECT,
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
    SECURITY_DESCRIPTOR_CONTROL control_bits = 0;
    DWORD revision = 0;
    REQUIRE(::GetSecurityDescriptorControl(descriptor, &control_bits,
                                           &revision) != 0);
    REQUIRE((control_bits & SE_DACL_PROTECTED) != 0);
}

TEST_CASE("control storage: a directory in place of the file is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    REQUIRE(try_open(directory));

    const auto file = directory / "audit.jsonl";
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);

    const auto opened = try_open(directory);
    REQUIRE(!opened);
}

TEST_CASE("control storage: the directory can be opened again")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    {
        const auto first = try_open(directory);
        REQUIRE(first);
        (*first)->record("test.event");
    }
    const auto again = try_open(directory);
    REQUIRE(again);
    (*again)->record("test.event");
    REQUIRE_EQ((*again)->failed_records(), std::uint64_t{0});
    const auto verdict = control::verify_audit_log(directory);
    REQUIRE(verdict);
    REQUIRE(verdict->intact);
    REQUIRE_EQ(verdict->records, std::uint64_t{2});
}

TEST_CASE("control storage: the default directory is named control")
{
    const auto directory = control::default_audit_directory();
    REQUIRE(directory);
    REQUIRE(directory->is_absolute());
    REQUIRE_EQ(directory->filename().string(), "control");
    REQUIRE_EQ(directory->parent_path().filename().string(),
               "Virtual Serial Studio");
}
