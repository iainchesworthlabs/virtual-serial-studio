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

#include "platform/private_storage_platform.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace control = vss::control;
namespace platform = vss::control::platform;

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

// -- whole files and random bytes --------------------------------------------

namespace
{

/// Text of every byte value in turn, `size` bytes long: it holds zero bytes and
/// bytes above 127, and is long enough to need more than one read and write.
std::string patterned_bytes(std::size_t size)
{
    std::string bytes(size, '\0');
    for (std::size_t position = 0; position < size; ++position)
        bytes[position] = static_cast<char>(position % 256);
    return bytes;
}

/// The content of a file, read with an ordinary stream.
std::string contents_of(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    REQUIRE(static_cast<bool>(in));
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool all_zero(std::span<const std::uint8_t> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

/// The names of the entries of a directory.
std::vector<std::string> names_in(const std::filesystem::path& directory)
{
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        names.push_back(entry.path().filename().string());
    return names;
}

} // namespace

TEST_CASE("control storage: a file is written and read back")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";

    const std::string brief = "{\"version\": 1}";
    REQUIRE(!platform::write_private_file(file, brief));
    const auto read_brief = platform::read_private_file(file, 1024);
    REQUIRE(read_brief);
    REQUIRE_EQ(*read_brief, brief);

    // Bytes of every value, more than one read and one write long.
    const std::string large = patterned_bytes(300 * 1000 + 17);
    REQUIRE(!platform::write_private_file(file, large));
    const auto read_large = platform::read_private_file(file, large.size());
    REQUIRE(read_large);
    REQUIRE(*read_large == large);

    // An empty file is a file.
    REQUIRE(!platform::write_private_file(file, ""));
    const auto read_empty = platform::read_private_file(file, 0);
    REQUIRE(read_empty);
    REQUIRE(read_empty->empty());
}

TEST_CASE("control storage: a file that is written has mode 0600")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";

    REQUIRE(!platform::write_private_file(file, "first"));
    const auto first = status_of(file);
    REQUIRE(first.has_value());
    REQUIRE(S_ISREG(first->st_mode));
    REQUIRE_EQ(first->st_uid, ::geteuid());
    REQUIRE_EQ(permission_bits(*first), 0600U);

    // The file that replaces it is private too.
    REQUIRE(!platform::write_private_file(file, "second"));
    const auto second = status_of(file);
    REQUIRE(second.has_value());
    REQUIRE_EQ(permission_bits(*second), 0600U);
}

TEST_CASE("control storage: a second write replaces the file")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";

    REQUIRE(!platform::write_private_file(file, "the first, and the longer"));
    REQUIRE(!platform::write_private_file(file, "the second"));
    const auto content = platform::read_private_file(file, 1024);
    REQUIRE(content);
    REQUIRE_EQ(*content, std::string("the second"));

    // One file is left, and no temporary one.
    const std::vector<std::string> names = names_in(scratch.path());
    REQUIRE_EQ(names.size(), std::size_t{1});
    REQUIRE_EQ(names[0], "state.json");
    REQUIRE(!std::filesystem::exists(scratch.path() / "state.json.tmp"));
}

TEST_CASE("control storage: a file that is not there is not found")
{
    const vss_test_support::scratch_directory scratch;
    const auto not_found = vss::make_error_code(vss::errc::not_found);

    const auto no_file = platform::read_private_file(scratch.path() / "no", 10);
    REQUIRE(!no_file);
    REQUIRE_EQ(no_file.error(), not_found);

    // The directory is missing too.
    const auto no_directory =
        platform::read_private_file(scratch.path() / "no" / "file", 10);
    REQUIRE(!no_directory);
    REQUIRE_EQ(no_directory.error(), not_found);
}

TEST_CASE("control storage: a whole file that is open to others is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";
    REQUIRE(!platform::write_private_file(file, "bytes"));

    for (const mode_t open_mode :
         {mode_t{0644}, mode_t{0640}, mode_t{0604}, mode_t{0660}, mode_t{0666}})
    {
        REQUIRE_EQ(::chmod(file.c_str(), open_mode), 0);
        const auto refused = platform::read_private_file(file, 1024);
        REQUIRE(!refused);
        REQUIRE_EQ(refused.error(), denied());
    }

    // Closed again, it is read.
    REQUIRE_EQ(::chmod(file.c_str(), 0600), 0);
    const auto content = platform::read_private_file(file, 1024);
    REQUIRE(content);
    REQUIRE_EQ(*content, std::string("bytes"));
}

TEST_CASE("control storage: a whole file is not read through a link")
{
    const vss_test_support::scratch_directory scratch;
    const auto real = scratch.path() / "real.json";
    REQUIRE(!platform::write_private_file(real, "bytes"));

    const auto link = scratch.path() / "link.json";
    std::filesystem::create_symlink(real, link);
    const auto refused = platform::read_private_file(link, 1024);
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), denied());
}

TEST_CASE("control storage: a link in place of the temporary file is kept out")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";
    const auto temporary = scratch.path() / "state.json.tmp";
    const auto target = scratch.path() / "elsewhere.txt";
    {
        std::ofstream out(target, std::ios::binary);
        REQUIRE(static_cast<bool>(out));
        out << "keep";
    }
    std::filesystem::create_symlink(target, temporary);

    // The write fails or replaces the link; it never writes through it.
    const vss::error_code first = platform::write_private_file(file, "new");
    REQUIRE_EQ(contents_of(target), "keep");
    if (!first)
    {
        const auto written = platform::read_private_file(file, 1024);
        REQUIRE(written);
        REQUIRE_EQ(*written, std::string("new"));
    }

    // Nothing is left under the temporary name, and the next write works.
    REQUIRE(std::filesystem::symlink_status(temporary).type() ==
            std::filesystem::file_type::not_found);
    REQUIRE(!platform::write_private_file(file, "newer"));
    REQUIRE_EQ(contents_of(target), "keep");
    const auto content = platform::read_private_file(file, 1024);
    REQUIRE(content);
    REQUIRE_EQ(*content, std::string("newer"));
}

TEST_CASE("control storage: a temporary file left open to others is removed")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";
    const auto temporary = scratch.path() / "state.json.tmp";
    {
        std::ofstream out(temporary, std::ios::binary);
        REQUIRE(static_cast<bool>(out));
        out << "left behind";
    }
    REQUIRE_EQ(::chmod(temporary.c_str(), 0644), 0);

    const vss::error_code refused = platform::write_private_file(file, "new");
    REQUIRE(refused);
    REQUIRE_EQ(refused, denied());
    REQUIRE(!std::filesystem::exists(file));
    REQUIRE(!std::filesystem::exists(temporary));

    REQUIRE(!platform::write_private_file(file, "new"));
    const auto content = platform::read_private_file(file, 1024);
    REQUIRE(content);
    REQUIRE_EQ(*content, std::string("new"));
}

TEST_CASE("control storage: a file longer than the limit is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";
    const std::string bytes(100, 'x');
    REQUIRE(!platform::write_private_file(file, bytes));

    const auto too_long = platform::read_private_file(file, 99);
    REQUIRE(!too_long);
    REQUIRE_EQ(too_long.error(),
               vss::make_error_code(vss::errc::invalid_argument));

    const auto just_right = platform::read_private_file(file, 100);
    REQUIRE(just_right);
    REQUIRE_EQ(*just_right, bytes);
}

TEST_CASE("control storage: a write into a directory that is missing fails")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "no" / "state.json";

    REQUIRE(platform::write_private_file(file, "bytes"));
    REQUIRE(!std::filesystem::exists(scratch.path() / "no"));
}

TEST_CASE("control storage: random bytes fill a buffer and differ each time")
{
    std::array<std::uint8_t, 64> first{};
    std::array<std::uint8_t, 64> second{};
    REQUIRE(!platform::random_bytes(first));
    REQUIRE(!platform::random_bytes(second));

    // 64 random bytes are all zero, or equal to another 64, with a probability
    // of 2 to the power of -512.
    REQUIRE(!all_zero(first));
    REQUIRE(!all_zero(second));
    REQUIRE(first != second);
}

TEST_CASE("control storage: random bytes fill a long buffer to its end")
{
    std::vector<std::uint8_t> bytes(3 * 1000 * 1000 + 5, 0);
    REQUIRE(!platform::random_bytes(bytes));
    const std::span<const std::uint8_t> all(bytes);
    REQUIRE(!all_zero(all.first(64)));
    REQUIRE(!all_zero(all.subspan(all.size() / 2, 64)));
    REQUIRE(!all_zero(all.last(64)));
}

TEST_CASE("control storage: random bytes of no length succeed")
{
    REQUIRE(!platform::random_bytes(std::span<std::uint8_t>()));

    std::array<std::uint8_t, 8> untouched{};
    REQUIRE(
        !platform::random_bytes(std::span<std::uint8_t>(untouched).first(0)));
    REQUIRE(all_zero(untouched));
}
