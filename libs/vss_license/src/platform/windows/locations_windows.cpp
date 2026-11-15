//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where licences are on Windows:
//
//   machine   <ProgramData>\Virtual Serial Studio\licences
//   user      <LocalAppData>\Virtual Serial Studio\licences
//
// The two folders are asked of Windows itself (the known folders), not read
// from the environment variables of the same names. Anyone can set a variable
// for a program they start, and a `ProgramData` that points at a folder of the
// user's own would move the administrator's licence folder there.
//

#include <vss/license/store.hpp>

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
// clang-format on

#include <filesystem>
#include <memory>

namespace vss::license
{

namespace
{

namespace fs = std::filesystem;

struct task_memory_free
{
    void operator()(wchar_t* text) const noexcept { ::CoTaskMemFree(text); }
};

/// The path of a known folder, or an empty path when Windows does not give
/// one. The folder is not made when it is not there.
fs::path known_folder(const KNOWNFOLDERID& id)
{
    PWSTR text = nullptr;
    const HRESULT got =
        ::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &text);
    const std::unique_ptr<wchar_t, task_memory_free> keep(text);
    if (FAILED(got) || text == nullptr)
        return {};
    const fs::path path(text);
    return path.is_absolute() ? path : fs::path{};
}

fs::path licence_folder(const KNOWNFOLDERID& id)
{
    const fs::path base = known_folder(id);
    if (base.empty())
        return {};
    return base / L"Virtual Serial Studio" / L"licences";
}

} // namespace

fs::path machine_licence_directory()
{
    return licence_folder(FOLDERID_ProgramData);
}

fs::path user_licence_directory()
{
    return licence_folder(FOLDERID_LocalAppData);
}

} // namespace vss::license
