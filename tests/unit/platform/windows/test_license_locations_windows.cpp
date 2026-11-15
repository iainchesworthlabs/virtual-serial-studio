//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where licences are looked for on Windows. The folders are the ones Windows
// itself names, and an environment variable does not move them.
//

#include <vss/license/store.hpp>

#include "test_framework.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
// clang-format on

#include <filesystem>
#include <optional>
#include <string>

namespace lic = vss::license;

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

} // namespace

TEST_CASE("licence locations on Windows: the machine folder")
{
    REQUIRE(lic::machine_licence_directory() ==
            known_folder(FOLDERID_ProgramData) / L"Virtual Serial Studio" /
                L"licences");
}

TEST_CASE("licence locations on Windows: the user's folder")
{
    const fs::path expected = known_folder(FOLDERID_LocalAppData) /
                              L"Virtual Serial Studio" / L"licences";
    REQUIRE(lic::user_licence_directory() == expected);
    REQUIRE(lic::default_store_options().clock_file ==
            expected.parent_path() / L"licence-clock.json");
}

TEST_CASE("licence locations on Windows: an environment variable does not "
          "move the folders")
{
    // Anyone can set a variable for a program they start. Were the folders
    // read from the variables, this would move the administrator's licence
    // folder into a folder of the user's.
    const fs::path machine = lic::machine_licence_directory();
    const fs::path user = lic::user_licence_directory();

    const fs::path other = fs::temp_directory_path();
    const scoped_variable data(L"ProgramData", other.c_str());
    const scoped_variable local(L"LOCALAPPDATA", other.c_str());

    REQUIRE(lic::machine_licence_directory() == machine);
    REQUIRE(lic::user_licence_directory() == user);
}

TEST_CASE("licence locations on Windows: the computer name is the DNS host "
          "name")
{
    std::wstring buffer(256, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    REQUIRE(::GetComputerNameExW(ComputerNameDnsHostname, buffer.data(),
                                 &size) != 0);
    buffer.resize(size);
    REQUIRE(fs::path(lic::computer_name()).wstring() == buffer);
}
