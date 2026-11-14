//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The access check on Windows: the owner and the access list of one file or
// folder.
//
// Who may do what is read from the access list as it is written, and not by
// asking Windows what some account could do.  So the answer does not depend
// on who runs Studio, and an entry this code does not understand is taken
// as "others too".
//

#include "platform/access_platform.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>

namespace vss::plugin::platform
{

namespace
{

struct local_free
{
    void operator()(void* memory) const noexcept
    {
        if (memory != nullptr)
            ::LocalFree(memory);
    }
};

/// The service that owns the files of Windows itself.
constexpr std::wstring_view trusted_installer =
    L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

bool is_well_known(PSID sid, WELL_KNOWN_SID_TYPE type) noexcept
{
    std::array<BYTE, SECURITY_MAX_SID_SIZE> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    return ::CreateWellKnownSid(type, nullptr, buffer.data(), &size) != 0 &&
           ::EqualSid(sid, buffer.data()) != 0;
}

/// Whether `sid` is one of the accounts that administer the computer:
/// the Administrators group, SYSTEM or TrustedInstaller.
bool is_administrator(PSID sid) noexcept
{
    if (sid == nullptr || ::IsValidSid(sid) == 0)
        return false;
    if (is_well_known(sid, WinBuiltinAdministratorsSid) ||
        is_well_known(sid, WinLocalSystemSid))
        return true;
    LPWSTR text = nullptr;
    if (::ConvertSidToStringSidW(sid, &text) == 0)
        return false;
    const std::unique_ptr<void, local_free> keep(text);
    return std::wstring_view(text) == trusted_installer;
}

/// The rights that let their holder change the file or folder itself.
constexpr DWORD change_it =
    FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD | DELETE |
    WRITE_DAC | WRITE_OWNER;

/// The rights on a folder above that let their holder replace or remove
/// what is inside it.  Adding a new file or folder beside the others is
/// not one of them: every user may make a folder in the root of a drive.
constexpr DWORD replace_inside =
    FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;

/// The rights an entry gives, with the four generic rights turned into the
/// file rights they stand for.
DWORD file_rights(DWORD mask) noexcept
{
    GENERIC_MAPPING mapping{};
    mapping.GenericRead = FILE_GENERIC_READ;
    mapping.GenericWrite = FILE_GENERIC_WRITE;
    mapping.GenericExecute = FILE_GENERIC_EXECUTE;
    mapping.GenericAll = FILE_ALL_ACCESS;
    ::MapGenericMask(&mask, &mapping);
    return mask;
}

} // namespace

result<access_finding> examine(const std::filesystem::path& path, bool last)
{
    PSID owner = nullptr;
    PACL list = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(
        path.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
        nullptr, &list, nullptr, &descriptor);
    if (status != ERROR_SUCCESS)
        return make_error_code(errc::io_error);
    const std::unique_ptr<void, local_free> keep(descriptor);

    access_finding found;
    // The owner may always rewrite the access list.
    if (!is_administrator(owner))
    {
        found.others = true;
        found.why = "does not belong to an administrator";
        return found;
    }
    if (list == nullptr)
    {
        // No access list at all gives everyone every right.
        found.others = true;
        found.why = "has no access list";
        return found;
    }

    ACL_SIZE_INFORMATION size{};
    if (::GetAclInformation(list, &size, sizeof(size),
                             AclSizeInformation) == 0)
        return make_error_code(errc::io_error);

    const DWORD dangerous = last ? change_it : replace_inside;
    for (DWORD index = 0; index < size.AceCount; ++index)
    {
        void* entry = nullptr;
        if (::GetAce(list, index, &entry) == 0)
            return make_error_code(errc::io_error);
        const auto* const header = static_cast<const ACE_HEADER*>(entry);

        // An entry that only passes rights on to what is inside says
        // nothing about this file or folder.
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0)
            continue;
        // An entry that denies can only take rights away.
        if (header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
        {
            found.others = true;
            found.why = "has an access entry of a kind that is not known";
            return found;
        }

        // The layout of an entry that allows: a header, the rights, and
        // the account, which starts where `SidStart` is.
        const auto* const allowed =
            static_cast<const ACCESS_ALLOWED_ACE*>(entry);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        PSID account = const_cast<DWORD*>(&allowed->SidStart);
        if ((file_rights(allowed->Mask) & dangerous) == 0)
            continue;
        if (is_administrator(account))
            continue;
        found.others = true;
        found.why = last ? "can be changed by an account that is not an "
                           "administrator"
                         : "lets an account that is not an administrator "
                           "replace what is in it";
        return found;
    }
    return found;
}

} // namespace vss::plugin::platform
