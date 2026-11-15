//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "platform.hpp"

#include <cstdlib>
#include <limits>

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>

namespace vss::license::tool::platform
{

bool random_bytes(std::span<std::uint8_t> out)
{
    if (out.size() > std::numeric_limits<ULONG>::max())
        return false;
    const NTSTATUS status =
        ::BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0;
}

bool write_private_file(const std::filesystem::path& path,
                        std::string_view text)
{
    if (text.size() > std::numeric_limits<DWORD>::max())
        return false;

    // A protected access list with one entry: the file's owner, full access.
    // Nothing is inherited from the folder, so nobody else is let in.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
        return false;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    // CREATE_NEW fails if the name exists.
    const auto file =
        ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    ::LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD count = 0;
    bool written =
        ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &count,
                    nullptr) != 0 &&
        count == text.size();
    if (written && ::FlushFileBuffers(file) == 0)
        written = false;
    if (::CloseHandle(file) == 0)
        written = false;
    if (!written)
        ::DeleteFileW(path.c_str());
    return written;
}

std::optional<std::size_t> read_file_into(const std::filesystem::path& path,
                                          std::span<char> out)
{
    const auto file =
        ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::nullopt;

    std::size_t total = 0;
    bool failed = false;
    while (total < out.size())
    {
        constexpr std::size_t most = std::numeric_limits<DWORD>::max();
        const std::size_t want =
            out.size() - total < most ? out.size() - total : most;
        DWORD count = 0;
        if (::ReadFile(file, out.data() + total, static_cast<DWORD>(want),
                       &count, nullptr) == 0)
        {
            failed = true;
            break;
        }
        if (count == 0)
            break;
        total += count;
    }
    ::CloseHandle(file);
    if (failed)
        return std::nullopt;
    return total;
}

std::optional<std::string> read_variable(const std::string& name)
{
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name.c_str()) != 0 || buffer == nullptr)
        return std::nullopt;
    std::string value(buffer);
    // The value may be a key, so the runtime's copy is cleared before it is
    // given back.
    ::SecureZeroMemory(buffer, size);
    std::free(buffer);
    return value;
}

} // namespace vss::license::tool::platform
