//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What this computer says on Windows.  The folders are the ones Windows
// itself names, and an environment variable does not move them.  Nothing here
// creates, changes or removes a folder under ProgramData or anywhere else
// that belongs to the computer.
//

#include <vss/machine/access.hpp>
#include <vss/machine/computer_name.hpp>
#include <vss/machine/folders.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
// clang-format on

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

using namespace vss::machine;
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

/// The folder of Windows' own programs, System32.
fs::path system_folder()
{
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length =
        ::GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    REQUIRE(length > 0 && length < buffer.size());
    return fs::path(std::wstring(buffer.data(), length));
}

} // namespace

TEST_CASE("machine folders on Windows: the folders Windows names")
{
    REQUIRE(machine_folder() ==
            known_folder(FOLDERID_ProgramData) / L"Virtual Serial Studio");
    REQUIRE(user_folder() ==
            known_folder(FOLDERID_LocalAppData) / L"Virtual Serial Studio");
    REQUIRE(machine_folder().filename() == fs::path(L"Virtual Serial Studio"));
    REQUIRE(user_folder().filename() == fs::path(L"Virtual Serial Studio"));
}

TEST_CASE("machine folders on Windows: an environment variable does not move "
          "them")
{
    // Anyone can set a variable for a program they start.  Were the folders
    // read from the variables, this would move the computer's folder into a
    // folder of the user's own.
    const scratch_directory scratch;
    const fs::path machine_before = machine_folder();
    const fs::path user_before = user_folder();
    REQUIRE(!machine_before.empty());
    REQUIRE(!user_before.empty());

    const scoped_variable data(L"ProgramData", scratch.path().c_str());
    const scoped_variable all_users(L"ALLUSERSPROFILE", scratch.path().c_str());
    const scoped_variable local(L"LOCALAPPDATA", scratch.path().c_str());
    const scoped_variable profile(L"USERPROFILE", scratch.path().c_str());

    REQUIRE(machine_folder() == machine_before);
    REQUIRE(user_folder() == user_before);
    REQUIRE(machine_folder().wstring().find(scratch.path().wstring()) ==
            std::wstring::npos);
    REQUIRE(user_folder().wstring().find(scratch.path().wstring()) ==
            std::wstring::npos);
}

TEST_CASE("machine folders on Windows: nothing is made")
{
    // The folders are worked out and never made, so asking changes nothing
    // whether or not the computer's folder is there.
    std::error_code ignored;
    const bool before = fs::exists(machine_folder(), ignored);
    static_cast<void>(machine_folder());
    static_cast<void>(user_folder());
    REQUIRE(fs::exists(machine_folder(), ignored) == before);
}

TEST_CASE("machine access on Windows: the folder of Windows can be changed by "
          "administrators only")
{
    const fs::path folder = system_folder();
    std::string detail = "old";
    const auto access = who_can_write(folder, &detail);
    REQUIRE(access.has_value());
    REQUIRE(*access == write_access::administrators_only);
    REQUIRE(detail.empty());
}

TEST_CASE("machine name on Windows: the DNS host name, cut at the first dot")
{
    std::array<wchar_t, 256> buffer{};
    auto size = static_cast<DWORD>(buffer.size());
    REQUIRE(::GetComputerNameExW(ComputerNameDnsHostname, buffer.data(),
                                 &size) != 0);
    const std::wstring dns(buffer.data(), size);
    REQUIRE(!dns.empty());

    const std::string name = computer_name();
    REQUIRE(!name.empty());
    // The same name as Windows gives, in UTF-8.
    const std::wstring expected = dns.substr(0, dns.find(L'.'));
    std::string expected_utf8;
    for (const char8_t unit : fs::path(expected).u8string())
        expected_utf8 += static_cast<char>(unit);
    REQUIRE(name == expected_utf8);
}
