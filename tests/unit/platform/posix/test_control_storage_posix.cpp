//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audit log's storage on Linux and macOS: the modes and the owner of the
// directory and of the file are read back with `lstat`, and a directory that
// is open to others, a link in place of the directory and a link in place of
// the file are refused.  Everything is reached through the public API.
//

#include <vss/control/audit_log.hpp>
#include <vss/core/error.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace control = vss::control;

namespace
{

/// `lstat`: a link is looked at, not followed.
std::optional<struct stat> status_of(const std::filesystem::path& path)
{
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0)
        return std::nullopt;
    return info;
}

unsigned permission_bits(const struct stat& info)
{
    return static_cast<unsigned>(info.st_mode & 07777);
}

vss::result<std::unique_ptr<control::audit_log>>
try_open(const std::filesystem::path& directory)
{
    control::audit_options options;
    options.directory = directory;
    return control::audit_log::open(std::move(options));
}

/// Makes the directory and its file, and closes them again.
void make_log_directory(const std::filesystem::path& directory)
{
    const auto opened = try_open(directory);
    REQUIRE(opened);
}

vss::error_code denied()
{
    return vss::make_error_code(vss::errc::permission_denied);
}

} // namespace

TEST_CASE("control storage: the directory and the file are private")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    const auto opened = try_open(directory);
    REQUIRE(opened);
    (*opened)->record("test.event");

    const auto directory_status = status_of(directory);
    REQUIRE(directory_status.has_value());
    REQUIRE(S_ISDIR(directory_status->st_mode));
    REQUIRE_EQ(directory_status->st_uid, ::geteuid());
    REQUIRE_EQ(permission_bits(*directory_status), 0700U);

    const auto file_status = status_of(directory / "audit.jsonl");
    REQUIRE(file_status.has_value());
    REQUIRE(S_ISREG(file_status->st_mode));
    REQUIRE_EQ(file_status->st_uid, ::geteuid());
    REQUIRE_EQ(permission_bits(*file_status), 0600U);
    REQUIRE(file_status->st_size > 0);
}

TEST_CASE("control storage: a directory that is open to others is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_log_directory(directory);

    REQUIRE_EQ(::chmod(directory.c_str(), 0755), 0);
    const auto opened = try_open(directory);
    REQUIRE(!opened);
    REQUIRE_EQ(opened.error(), denied());

    // Closed again, it is accepted.
    REQUIRE_EQ(::chmod(directory.c_str(), 0700), 0);
    REQUIRE(try_open(directory));
}

TEST_CASE("control storage: a link in place of the directory is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto real = scratch.path() / "real";
    make_log_directory(real);

    const auto link = scratch.path() / "link";
    std::filesystem::create_directory_symlink(real, link);
    const auto opened = try_open(link);
    REQUIRE(!opened);
    REQUIRE_EQ(opened.error(), denied());
}

TEST_CASE("control storage: a link in place of the file is not followed")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_log_directory(directory);

    const auto target = scratch.path() / "elsewhere.txt";
    {
        std::ofstream out(target);
        REQUIRE(static_cast<bool>(out));
    }
    const auto file = directory / "audit.jsonl";
    std::filesystem::remove(file);
    std::filesystem::create_symlink(target, file);

    const auto opened = try_open(directory);
    REQUIRE(!opened);
    REQUIRE_EQ(std::filesystem::file_size(target), std::uintmax_t{0});
}

TEST_CASE("control storage: a file that is open to others is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_log_directory(directory);

    const auto file = directory / "audit.jsonl";
    REQUIRE_EQ(::chmod(file.c_str(), 0644), 0);
    const auto opened = try_open(directory);
    REQUIRE(!opened);
    REQUIRE_EQ(opened.error(), denied());
}

TEST_CASE("control storage: the default directory is named control")
{
    const auto directory = control::default_audit_directory();
    if (!directory &&
        directory.error() == vss::make_error_code(vss::errc::not_found))
        VSS_SKIP("the environment names no home directory");
    REQUIRE(directory);
    REQUIRE(directory->is_absolute());
    REQUIRE_EQ(directory->filename().string(), "control");
}
