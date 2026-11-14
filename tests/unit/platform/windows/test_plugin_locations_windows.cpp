//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where plugins and the policy are looked for on Windows.  The folders are
// the ones Windows itself names, and an environment variable does not move
// them.
//

#include <vss/plugin/locations.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
// clang-format on

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

using namespace vss::plugin;
using vss_test_support::scratch_directory;

namespace
{

namespace fs = std::filesystem;

/// An environment variable that a test sets, and that is put back as it was
/// when the test ends.
class scoped_variable
{
public:
    scoped_variable(const wchar_t* name, const wchar_t* value) : name_(name)
    {
        std::wstring buffer(32768, L'\0');
        const DWORD size = ::GetEnvironmentVariableW(
            name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size > 0 && size < buffer.size())
        {
            buffer.resize(size);
            old_ = buffer;
        }
        static_cast<void>(::SetEnvironmentVariableW(name, value));
    }

    ~scoped_variable()
    {
        // A null value removes the variable, which is how it was when it
        // had none.
        const wchar_t* value = nullptr;
        if (old_.has_value())
            value = old_->c_str();
        static_cast<void>(::SetEnvironmentVariableW(name_.c_str(), value));
    }

    scoped_variable(const scoped_variable&) = delete;
    scoped_variable& operator=(const scoped_variable&) = delete;

private:
    std::wstring name_;
    std::optional<std::wstring> old_;
};

/// The folder Windows names for `id`.
fs::path known_folder(const KNOWNFOLDERID& id)
{
    PWSTR text = nullptr;
    const HRESULT got =
        ::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &text);
    REQUIRE(SUCCEEDED(got));
    REQUIRE(text != nullptr);
    fs::path path(text);
    ::CoTaskMemFree(text);
    return path;
}

const plugin_root* find_root(const plugin_locations& found, plugin_scope scope)
{
    for (const plugin_root& root : found.roots)
    {
        if (root.scope == scope)
            return &root;
    }
    return nullptr;
}

} // namespace

TEST_CASE("plugin locations on Windows: the machine folder and the policy")
{
    const scratch_directory scratch;
    const plugin_locations found =
        plugin_locations_for(scratch.path() / "install" / "bin");
    const fs::path product =
        known_folder(FOLDERID_ProgramData) / L"VirtualSerialStudio";

    const plugin_root* const machine =
        find_root(found, plugin_scope::for_everyone);
    REQUIRE(machine != nullptr);
    REQUIRE(machine->directory == product / L"plugins");
    REQUIRE(found.policy_file == product / L"policy.json");
    REQUIRE(machine->directory.is_absolute());
}

TEST_CASE("plugin locations on Windows: the user's folder")
{
    const scratch_directory scratch;
    const plugin_locations found = plugin_locations_for(scratch.path() / "bin");

    const plugin_root* const user = find_root(found, plugin_scope::for_user);
    REQUIRE(user != nullptr);
    REQUIRE(user->directory == known_folder(FOLDERID_LocalAppData) /
                                   L"VirtualSerialStudio" / L"plugins");
    // The user's folder is the last.
    REQUIRE(&found.roots.back() == user);
}

TEST_CASE("plugin locations on Windows: an environment variable does not "
          "move the folders")
{
    // Anyone can set a variable for a program they start.  Were the folders
    // read from the variables, this would move the machine's plugin folder
    // and the administrator's policy into a folder of the user's.
    const scratch_directory scratch;
    const plugin_locations before =
        plugin_locations_for(scratch.path() / "bin");

    const scoped_variable data(L"ProgramData", scratch.path().c_str());
    const scoped_variable local(L"LOCALAPPDATA", scratch.path().c_str());
    const plugin_locations after =
        plugin_locations_for(scratch.path() / "bin");

    REQUIRE(after.policy_file == before.policy_file);
    REQUIRE_EQ(after.roots.size(), before.roots.size());
    for (std::size_t i = 0; i < after.roots.size(); ++i)
        REQUIRE(after.roots[i].directory == before.roots[i].directory);
    const plugin_root* const machine =
        find_root(after, plugin_scope::for_everyone);
    REQUIRE(machine != nullptr);
    REQUIRE(machine->directory.wstring().find(scratch.path().wstring()) ==
            std::wstring::npos);
}
