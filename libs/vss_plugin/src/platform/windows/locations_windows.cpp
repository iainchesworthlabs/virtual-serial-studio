//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where plugins and the policy are on Windows:
//
//   machine   <ProgramData>\VirtualSerialStudio\plugins
//   user      <LocalAppData>\VirtualSerialStudio\plugins
//   policy    <ProgramData>\VirtualSerialStudio\policy.json
//
// The two folders are asked of Windows itself (the known folders), not read
// from the environment variables of the same names.  Anyone can set a
// variable for a program they start, and a `ProgramData` that points at a
// folder of the user's own would move the machine's plugin folder and the
// administrator's policy there.
//

#include "platform/locations_platform.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
// clang-format on

#include <filesystem>
#include <memory>

namespace vss::plugin::platform
{

namespace
{

namespace fs = std::filesystem;

struct task_memory_free
{
    void operator()(wchar_t* text) const noexcept { ::CoTaskMemFree(text); }
};

/// The path of a known folder, or an empty path when Windows does not give
/// one.  The folder is not made when it is not there.
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

fs::path product_folder(const KNOWNFOLDERID& id)
{
    const fs::path base = known_folder(id);
    if (base.empty())
        return {};
    return base / L"VirtualSerialStudio";
}

} // namespace

fs::path machine_plugin_folder()
{
    const fs::path folder = product_folder(FOLDERID_ProgramData);
    if (folder.empty())
        return {};
    return folder / L"plugins";
}

fs::path user_plugin_folder()
{
    const fs::path folder = product_folder(FOLDERID_LocalAppData);
    if (folder.empty())
        return {};
    return folder / L"plugins";
}

fs::path policy_file_path()
{
    const fs::path folder = product_folder(FOLDERID_ProgramData);
    if (folder.empty())
        return {};
    return folder / L"policy.json";
}

} // namespace vss::plugin::platform
