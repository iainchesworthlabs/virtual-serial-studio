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
    const HANDLE file =
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
