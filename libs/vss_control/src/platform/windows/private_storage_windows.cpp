//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Private storage on Windows: a directory and files that only the user who
// runs the gateway can open.
//
//   state      %LOCALAPPDATA%\Virtual Serial Studio, found with
//              SHGetKnownFolderPath
//   directory  made with the protected access list D:P(A;OICI;FA;;;<SID>): the
//              user and nobody else, nothing inherited from the parent, and
//              everything inside it inherits this list.  A directory that
//              exists is accepted when this user owns it, or when the
//              Administrators group does and this process runs with that
//              group enabled (an elevated process makes objects owned by the
//              group).  A path that is a file or a reparse point is refused.
//   file       opened for appending only, shared for reading and deleting, not
//              inherited by child processes.  A new file takes the access list
//              of its directory.  A directory or a reparse point in place of
//              the file is refused.
//
// This file needs shell32 (SHGetKnownFolderPath), ole32 (CoTaskMemFree) and
// advapi32 (the security functions).
//

#include "platform/private_storage_platform.hpp"

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss::control::platform
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

std::vector<unsigned char> own_user()
{
    HANDLE raw = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw) == 0)
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
    const auto user = own_user();
    if (user.empty())
        return system_error(::GetLastError());
    LPWSTR raw = nullptr;
    if (::ConvertSidToStringSidW(sid_of(user), &raw) == 0)
        return system_error(::GetLastError());
    const local_owner text(raw);
    return std::wstring(raw);
}

/// True when `owner` is the Administrators group and this process has that
/// group enabled in its token.  A process that runs without elevation has the
/// group only to deny access, which `CheckTokenMembership` does not count.
bool is_enabled_administrators(PSID owner)
{
    std::vector<unsigned char> group(SECURITY_MAX_SID_SIZE);
    DWORD size = static_cast<DWORD>(group.size());
    if (::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, group.data(),
                             &size) == 0)
        return false;
    if (::EqualSid(owner, group.data()) == 0)
        return false;
    BOOL member = FALSE;
    if (::CheckTokenMembership(nullptr, group.data(), &member) == 0)
        return false;
    return member != FALSE;
}

/// Checks a directory that was there before: a real directory, not a link, and
/// owned by this user (or by Administrators for an elevated process).
error_code check_existing_directory(const std::filesystem::path& directory,
                                    const std::vector<unsigned char>& user)
{
    const std::wstring text = directory.wstring();
    const DWORD attributes = ::GetFileAttributesW(text.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return system_error(::GetLastError());
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return make_error_code(errc::permission_denied);
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return make_error_code(errc::permission_denied);

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(
        text.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
        nullptr, nullptr, nullptr, &descriptor);
    const local_owner release(descriptor);
    if (status != ERROR_SUCCESS)
        return system_error(status);
    if (owner == nullptr)
        return make_error_code(errc::permission_denied);
    if (::EqualSid(owner, sid_of(user)) != 0)
        return {};
    if (is_enabled_administrators(owner))
        return {};
    return make_error_code(errc::permission_denied);
}

class windows_file final : public private_file
{
public:
    explicit windows_file(handle_owner handle) : handle_(std::move(handle)) {}

    error_code append(std::string_view bytes) override
    {
        constexpr std::size_t longest_write = std::size_t{1} << 20;
        std::size_t done = 0;
        while (done < bytes.size())
        {
            const std::size_t left = bytes.size() - done;
            const DWORD count =
                static_cast<DWORD>(left < longest_write ? left : longest_write);
            DWORD written = 0;
            if (::WriteFile(handle_.get(), bytes.data() + done, count, &written,
                            nullptr) == 0)
                return system_error(::GetLastError());
            if (written == 0)
                return make_error_code(errc::io_error);
            done += written;
        }
        return {};
    }

private:
    handle_owner handle_;
};

} // namespace

result<std::filesystem::path> state_directory()
{
    PWSTR raw = nullptr;
    const HRESULT status =
        ::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
    // The memory is returned to the system also when the call failed.
    struct task_memory_releaser
    {
        PWSTR memory;
        ~task_memory_releaser() { ::CoTaskMemFree(memory); }
    } const release{raw};
    if (FAILED(status) || raw == nullptr)
        return make_error_code(errc::not_found);
    return std::filesystem::path(raw) / L"Virtual Serial Studio";
}

error_code ensure_private_directory(const std::filesystem::path& directory)
{
    const auto user = own_user();
    if (user.empty())
        return system_error(::GetLastError());
    const auto sid = own_sid_text();
    if (!sid)
        return sid.error();

    // Each missing parent gets the access list it inherits; the last
    // component is closed to everyone but the owner.
    std::error_code ignored;
    if (directory.has_parent_path())
        std::filesystem::create_directories(directory.parent_path(), ignored);

    // Protected, and one entry: everything for this user, inherited by what
    // is made inside.
    const std::wstring sddl = L"D:P(A;OICI;FA;;;" + *sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == 0)
        return system_error(::GetLastError());
    const local_owner release(descriptor);

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    const std::wstring text = directory.wstring();
    if (::CreateDirectoryW(text.c_str(), &attributes) != 0)
        return {};
    const DWORD failed = ::GetLastError();
    if (failed != ERROR_ALREADY_EXISTS)
        return system_error(failed);
    return check_existing_directory(directory, user);
}

result<std::unique_ptr<private_file>>
open_private_append(const std::filesystem::path& path)
{
    const std::wstring text = path.wstring();
    // FILE_READ_ATTRIBUTES lets the handle be asked what it is, below.  Without
    // FILE_WRITE_DATA every write goes to the end of the file.
    handle_owner handle(::CreateFileW(
        text.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE)
    {
        static_cast<void>(handle.release());
        return system_error(::GetLastError());
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle.get(), &information) == 0)
        return system_error(::GetLastError());
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return make_error_code(errc::permission_denied);

    return std::unique_ptr<private_file>(
        std::make_unique<windows_file>(std::move(handle)));
}

} // namespace vss::control::platform
