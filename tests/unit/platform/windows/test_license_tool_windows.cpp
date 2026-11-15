//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The private key file that `vss_license keygen` writes, read back the way
// another program would see it: its access list must be protected, and hold
// one entry that allows, for the owner of the file and nobody else.
//

#include "license_tool_fixture.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on

#include <filesystem>
#include <memory>
#include <string>

using namespace vss_test_support::license_tool;

namespace
{

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

} // namespace

TEST_CASE("licence tool: the private key file is for its owner only")
{
    scratch_directory scratch;
    const std::string key = path_in(scratch, "throwaway.key");
    REQUIRE_EQ(
        run_tool({"keygen", "--key-id", "lic-unit-a", "--out-private", key})
            .status,
        vss::license::tool::exit_ok);

    const std::wstring name = std::filesystem::path(key).wstring();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(
        name.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
        nullptr, &dacl, nullptr, &descriptor);
    const local_owner release(descriptor);
    REQUIRE_EQ(status, DWORD{0});
    REQUIRE(descriptor != nullptr);
    REQUIRE(dacl != nullptr);

    // One entry, and it allows.
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

    // The entry is for "owner rights", the owner of the file whoever that is,
    // which is what the tool asks for.
    PSID owner_rights = nullptr;
    REQUIRE(::ConvertStringSidToSidW(L"S-1-3-4", &owner_rights) != 0);
    const local_owner release_sid(owner_rights);
    PSID entry_sid = &allowed->SidStart;
    REQUIRE(::EqualSid(entry_sid, owner_rights) != 0);

    // The list is protected: nothing is inherited from the folder.
    SECURITY_DESCRIPTOR_CONTROL control_bits = 0;
    DWORD revision = 0;
    REQUIRE(::GetSecurityDescriptorControl(descriptor, &control_bits,
                                           &revision) != 0);
    REQUIRE((control_bits & SE_DACL_PROTECTED) != 0);
}
