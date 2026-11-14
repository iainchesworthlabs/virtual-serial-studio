//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The access check on Windows, against the folder of Windows itself, and the
// refusal of a junction in a plugin's folder.  A junction is the link an
// ordinary user can make on Windows without any privilege, so it is the one
// that matters most there.
//

#include <vss/core/error.hpp>
#include <vss/plugin/access.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/measure.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/verify.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

// windows.h first: the other header needs its types.
// clang-format off
#include <windows.h>
#include <winioctl.h>
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace vss;
using namespace vss::plugin;
using vss_test_support::scratch_directory;

namespace
{

namespace fs = std::filesystem;

void write_file(const fs::path& path, std::string_view text)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file.good());
}

/// The folder of Windows' own programs, System32.
fs::path system_folder()
{
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = ::GetSystemDirectoryW(
        buffer.data(), static_cast<UINT>(buffer.size()));
    REQUIRE(length > 0 && length < buffer.size());
    return fs::path(std::wstring(buffer.data(), length));
}

/// Appends the bytes of `value` to `out`, as Windows lays them out.
template <class T>
void append(std::vector<std::byte>& out, const T& value)
{
    const auto* const bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void append(std::vector<std::byte>& out, const std::wstring& text)
{
    for (const wchar_t unit : text)
        append(out, unit);
    append(out, wchar_t{0});
}

/// A junction that is taken away again when the test ends.  It is removed
/// by its own name and before the scratch folder is, so that nothing ever
/// walks through it into the folder it leads to.
class junction
{
public:
    /// Makes a junction at `link` that leads to the folder `target`, the
    /// way Windows itself does: an empty folder that is given a mount point
    /// as its reparse data.  No program is run.
    junction(const fs::path& link, const fs::path& target) : link_(link)
    {
        std::error_code failed;
        if (!fs::create_directory(link, failed))
            return;
        const std::wstring print = fs::absolute(target).wstring();
        const std::wstring substitute = L"\\??\\" + print;
        const auto substitute_bytes =
            static_cast<std::uint16_t>(substitute.size() * sizeof(wchar_t));
        const auto print_bytes =
            static_cast<std::uint16_t>(print.size() * sizeof(wchar_t));

        // The reparse data of a mount point: the tag, the length of what
        // follows the first eight bytes, a reserved field, where the two
        // names are in the text, and the two names with a zero after each.
        std::vector<std::byte> data;
        append(data, std::uint32_t{IO_REPARSE_TAG_MOUNT_POINT});
        append(data, static_cast<std::uint16_t>(8 + substitute_bytes + 2 +
                                                print_bytes + 2));
        append(data, std::uint16_t{0});
        append(data, std::uint16_t{0});
        append(data, substitute_bytes);
        append(data, static_cast<std::uint16_t>(substitute_bytes + 2));
        append(data, print_bytes);
        append(data, substitute);
        append(data, print);

        HANDLE folder = ::CreateFileW(
            link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (folder == INVALID_HANDLE_VALUE)
            return;
        DWORD returned = 0;
        made_ = ::DeviceIoControl(folder, FSCTL_SET_REPARSE_POINT,
                                  data.data(),
                                  static_cast<DWORD>(data.size()), nullptr, 0,
                                  &returned, nullptr) != 0;
        ::CloseHandle(folder);
    }

    ~junction()
    {
        // Removing a junction by name removes the junction, not what it
        // leads to.  When it was never made this removes the empty folder.
        ::RemoveDirectoryW(link_.c_str());
    }

    junction(const junction&) = delete;
    junction& operator=(const junction&) = delete;

    /// False when the system would not make it.
    bool made() const noexcept { return made_; }

private:
    fs::path link_;
    bool made_ = false;
};

} // namespace

TEST_CASE("plugin access: the folder of Windows can be changed by "
          "administrators only")
{
    const fs::path folder = system_folder();
    std::string detail = "old";
    const auto access = who_can_write(folder, &detail);
    REQUIRE(access.has_value());
    REQUIRE(*access == write_access::administrators_only);
    REQUIRE(detail.empty());

    // A copy of Studio in such a folder is installed for everyone.
    const auto kind = kind_of_install(folder);
    REQUIRE(kind.has_value());
    REQUIRE(*kind == install_kind::for_everyone);
}

TEST_CASE("plugin access: a file of Windows that is not a policy is refused "
          "for what it says")
{
    // It gets past the question of who could have written it and fails at
    // reading, and the answer is still "official plugins only".
    const fs::path file = system_folder() / "drivers" / "etc" / "hosts";
    const auto access = who_can_write(file);
    if (!access.has_value() || *access != write_access::administrators_only)
        VSS_SKIP("the hosts file is not the administrators' alone here");

    const loaded_policy loaded = load_policy(file);
    REQUIRE(loaded.problem == errc::config_error);
    REQUIRE(!loaded.detail.empty());
    REQUIRE(loaded.rules.has_value());
    REQUIRE(loaded.rules.value() == official_only_policy());
}

TEST_CASE("plugin access: a junction to the folder of Windows is judged by "
          "where it leads")
{
    // Resolved, the junction is the folder it leads to; the folder that
    // holds the junction is a user's, which is what tells the two apart.
    const scratch_directory scratch;
    const fs::path link = scratch.path() / "link";
    const junction made(link, system_folder());
    if (!made.made())
        VSS_SKIP("junctions cannot be made here");

    const auto through = who_can_write(link);
    REQUIRE(through.has_value());
    REQUIRE(*through == write_access::administrators_only);
    const auto holder = who_can_write(scratch.path());
    REQUIRE(holder.has_value());
    REQUIRE(*holder == write_access::others_too);
}

TEST_CASE("plugin measure: a junction in a plugin's folder is refused")
{
    const scratch_directory scratch;
    const fs::path outside = scratch.path() / "outside";
    write_file(outside / "secret.txt", "not the plugin's");
    const fs::path plugin = scratch.path() / "plugin";
    write_file(plugin / "plugin.json", "{}");
    const junction made(plugin / "data", outside);
    if (!made.made())
        VSS_SKIP("junctions cannot be made here");

    std::string detail;
    const auto measured = measure_directory(plugin, &detail);
    REQUIRE(measured.has_error());
    REQUIRE(measured.error() == plugin_errc::layout_invalid);
    REQUIRE(detail.find("data") != std::string::npos);
}

TEST_CASE("plugin measure: a plugin's folder that is a junction is refused")
{
    const scratch_directory scratch;
    const fs::path real = scratch.path() / "real";
    write_file(real / "plugin.json", "{}");
    const fs::path link = scratch.path() / "link";
    const junction made(link, real);
    if (!made.made())
        VSS_SKIP("junctions cannot be made here");

    const auto measured = measure_directory(link);
    REQUIRE(measured.has_error());
    REQUIRE(measured.error() == plugin_errc::layout_invalid);
}

TEST_CASE("plugin discover: a junction in a plugin folder root is not "
          "followed")
{
    const scratch_directory scratch;
    const fs::path elsewhere = scratch.path() / "elsewhere";
    write_file(elsewhere / "plugin.json", "{}");
    const fs::path root = scratch.path() / "root";
    fs::create_directories(root);
    const junction made(root / "linked", elsewhere);
    if (!made.made())
        VSS_SKIP("junctions cannot be made here");

    const std::vector<plugin_root> roots = {
        plugin_root{root, plugin_scope::for_user}};
    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    REQUIRE(found.plugins[0].problem == plugin_errc::layout_invalid);
    // Nothing behind the junction was read.
    REQUIRE(found.plugins[0].manifest_bytes.empty());
    REQUIRE(!found.plugins[0].declared.has_value());
}

TEST_CASE("plugin discover: a plugin folder root that is a junction is "
          "reported and not entered")
{
    const scratch_directory scratch;
    const fs::path real = scratch.path() / "real";
    write_file(real / "one" / "plugin.json", "{}");
    const fs::path link = scratch.path() / "link";
    const junction made(link, real);
    if (!made.made())
        VSS_SKIP("junctions cannot be made here");

    const std::vector<plugin_root> roots = {
        plugin_root{link, plugin_scope::for_everyone}};
    const discovery found = discover_plugins(roots);
    REQUIRE(found.plugins.empty());
    REQUIRE_EQ(found.roots.size(), std::size_t{1});
    REQUIRE(found.roots[0].problem == plugin_errc::layout_invalid);
}
