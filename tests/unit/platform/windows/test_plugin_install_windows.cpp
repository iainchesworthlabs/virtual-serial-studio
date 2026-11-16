//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installing and removing where a folder is a junction.  A junction can be
// made without any privilege (a symbolic link cannot), so these run on every
// Windows computer.  A junction is never followed: as the source, inside the
// source and as the folder of a plugin to remove.
//

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/install.hpp>

#include "plugin_fixtures.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <windows.h>
#include <winioctl.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace vss;
using namespace vss::plugin;
using namespace vss_test_support;

namespace
{

namespace fs = std::filesystem;

/// The part of a mount point reparse buffer that follows its header.
struct mount_point_header
{
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
};

/// Makes `link` a junction to the folder `target`.  A junction needs no
/// privilege.  False when it could not be made.
bool make_junction(const fs::path& link, const fs::path& target)
{
    std::error_code ec;
    if (!fs::create_directory(link, ec) || ec)
        return false;
    const std::wstring print = fs::absolute(target).wstring();
    const std::wstring substitute = L"\\??\\" + print;
    const std::size_t path_bytes =
        (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    std::vector<char> buffer(sizeof(mount_point_header) + path_bytes);
    mount_point_header header{};
    header.tag = IO_REPARSE_TAG_MOUNT_POINT;
    header.data_length = static_cast<USHORT>(
        sizeof(mount_point_header) - 8 + path_bytes);
    header.substitute_offset = 0;
    header.substitute_length =
        static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    header.print_offset = static_cast<USHORT>(
        (substitute.size() + 1) * sizeof(wchar_t));
    header.print_length = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitute.c_str(),
                (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(header) +
                    (substitute.size() + 1) * sizeof(wchar_t),
                print.c_str(), (print.size() + 1) * sizeof(wchar_t));

    HANDLE handle = CreateFileW(
        link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD returned = 0;
    const BOOL done = DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    return done != FALSE;
}

} // namespace

TEST_CASE("plugin install windows: a source folder that is a junction is "
          "refused")
{
    const scratch_directory scratch;
    const fs::path real = make_unsigned_plugin(scratch.path(), "real");
    const fs::path junction = scratch.path() / "junction";
    REQUIRE(make_junction(junction, real));

    std::string detail;
    const fs::path root = scratch.path() / "installed";
    const auto done =
        install_plugin(junction, root, unsigned_install_options(), &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE(!detail.empty());
    REQUIRE(names_in_folder(root).empty());
    REQUIRE(fs::exists(real / "plugin.json"));
}

TEST_CASE("plugin install windows: a junction inside the source is refused "
          "and not followed")
{
    const scratch_directory scratch;
    const fs::path source = make_unsigned_plugin(scratch.path(), "source");
    write_plugin_file(scratch.path() / "outside" / "secret.txt", "secret");
    REQUIRE(make_junction(source / "bin" / "out", scratch.path() / "outside"));

    std::string detail;
    const fs::path root = scratch.path() / "installed";
    const auto done =
        install_plugin(source, root, unsigned_install_options(), &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE(!detail.empty());
    REQUIRE(names_in_folder(root).empty());
    REQUIRE_EQ(read_plugin_file(scratch.path() / "outside" / "secret.txt"),
               std::string("secret"));
}

TEST_CASE("plugin remove windows: a plugin folder that is a junction is not "
          "followed and not removed")
{
    const scratch_directory scratch;
    const fs::path real = make_unsigned_plugin(scratch.path(), "real");
    const fs::path root = scratch.path() / "installed";
    std::error_code ec;
    fs::create_directories(root, ec);
    REQUIRE(make_junction(root / "com.example.modbus", real));

    std::string detail;
    const auto done = remove_plugin(root, "com.example.modbus", {}, &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE(!detail.empty());
    REQUIRE(fs::exists(real / "plugin.json"));
    REQUIRE(fs::exists(real / "bin" / "prog"));
    REQUIRE(fs::exists(root / "com.example.modbus"));
}

TEST_CASE("plugin install windows: a replace does not touch a junction in the "
          "plugin's place")
{
    const scratch_directory scratch;
    const fs::path real = make_unsigned_plugin(scratch.path(), "real");
    const fs::path source = make_unsigned_plugin(scratch.path(), "source",
                                                 "com.example.modbus",
                                                 "new program");
    const fs::path root = scratch.path() / "installed";
    std::error_code ec;
    fs::create_directories(root, ec);
    REQUIRE(make_junction(root / "com.example.modbus", real));

    install_options options = unsigned_install_options();
    options.replace = true;
    const auto done = install_plugin(source, root, options);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE_EQ(read_plugin_file(real / "bin" / "prog"),
               std::string("program bytes"));
}
