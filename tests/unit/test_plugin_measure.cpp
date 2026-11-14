//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/measure.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace vss;
using namespace vss::plugin;

namespace
{

namespace fs = std::filesystem;

using vss_test_support::scratch_directory;

/// The SHA-512 of no bytes.
constexpr std::string_view empty_digest =
    "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
    "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e";

/// Writes a file, and the directories above it when they are not there.
void write_file(const fs::path& path, std::string_view content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    REQUIRE(!out.fail());
}

/// The bytes a file holds now.
std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// Bytes of every value that do not repeat within a chunk.
std::string pattern(std::size_t size)
{
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
    {
        out[i] = static_cast<char>((i * 131U + 7U) % 251U);
    }
    return out;
}

/// A file as a test expects it to be measured.
file_entry entry(std::string path, std::string_view content)
{
    file_entry out;
    out.path = std::move(path);
    out.size = content.size();
    out.sha512 = sha512_of(content);
    return out;
}

bool make_file_link(const fs::path& target, const fs::path& link_path)
{
    std::error_code ec;
    fs::create_symlink(target, link_path, ec);
    return !ec;
}

bool make_directory_link(const fs::path& target, const fs::path& link_path)
{
    std::error_code ec;
    fs::create_directory_symlink(target, link_path, ec);
    return !ec;
}

void require_refused(const fs::path& directory, plugin_errc expected,
                     std::string_view part)
{
    std::string detail = "old";
    const auto measured = measure_directory(directory, &detail);
    REQUIRE(measured.has_error());
    REQUIRE(measured.error() == expected);
    REQUIRE(detail.find(part) != std::string::npos);
}

void require_refused(const fs::path& directory, errc expected,
                     std::string_view part)
{
    std::string detail = "old";
    const auto measured = measure_directory(directory, &detail);
    REQUIRE(measured.has_error());
    REQUIRE(measured.error() == expected);
    REQUIRE(detail.find(part) != std::string::npos);
}

/// `directories` directories one inside the other, each named `d`, and then
/// a file named `f.bin` in the innermost: the file is at depth
/// `directories + 1`.
fs::path nested_file(const fs::path& root, std::size_t directories)
{
    fs::path out = root;
    for (std::size_t i = 0; i < directories; ++i)
    {
        out /= "d";
    }
    return out / "f.bin";
}

std::string nested_name(std::size_t directories)
{
    std::string out;
    for (std::size_t i = 0; i < directories; ++i)
    {
        out += "d/";
    }
    out += "f.bin";
    return out;
}

} // namespace

TEST_CASE("plugin measure: an empty directory has no entries")
{
    const scratch_directory scratch;
    std::string detail = "old";
    const auto measured = measure_directory(scratch.path(), &detail);
    REQUIRE(measured.has_value());
    REQUIRE_EQ(measured->size(), std::size_t{0});
    REQUIRE(detail.empty());

    // A directory with only directories in it leaves no trace either.
    fs::create_directories(scratch.path() / "a" / "b");
    const auto nested = measure_directory(scratch.path());
    REQUIRE(nested.has_value());
    REQUIRE_EQ(nested->size(), std::size_t{0});
}

TEST_CASE("plugin measure: files at several depths are listed in order")
{
    const scratch_directory scratch;
    const fs::path& root = scratch.path();
    write_file(root / "plugin.sig", "signature");
    write_file(root / "plugin.json", "{}");
    write_file(root / "bin" / "plugin", "program");
    write_file(root / "data" / "sub" / "deep.bin", "deep");
    write_file(root / "a.txt", "text");
    write_file(root / "Zebra", "capital");
    fs::create_directories(root / "empty");

    const auto measured = measure_directory(root);
    REQUIRE(measured.has_value());

    // Paths are compared as bytes, so a capital letter comes first.
    const std::vector<file_entry> expected = {
        entry("Zebra", "capital"),
        entry("a.txt", "text"),
        entry("bin/plugin", "program"),
        entry("data/sub/deep.bin", "deep"),
        entry("plugin.json", "{}"),
        entry("plugin.sig", "signature")};
    REQUIRE(*measured == expected);
    REQUIRE_EQ(measured->at(3).size, std::uint64_t{4});
    REQUIRE_EQ(measured->at(3).path, "data/sub/deep.bin");
}

TEST_CASE("plugin measure: the entries feed hash_plugin and check_files")
{
    const scratch_directory scratch;
    const fs::path& root = scratch.path();
    write_file(root / "plugin.json", "{}");
    write_file(root / "plugin.sig", "signature");
    write_file(root / "bin" / "plugin", "program");
    write_file(root / "data" / "table.bin", "table");

    manifest declared;
    declared.id = "com.example.plugin";
    declared.run.executable = "bin/plugin";
    declared.files = {entry("bin/plugin", "program"),
                      entry("data/table.bin", "table")};

    const auto measured = measure_directory(root);
    REQUIRE(measured.has_value());
    std::string detail;
    REQUIRE(check_files(declared, *measured, &detail).has_value());
    const plugin_hash before = hash_plugin(*measured);
    REQUIRE(before ==
            hash_plugin(std::vector<file_entry>{
                entry("bin/plugin", "program"),
                entry("data/table.bin", "table"), entry("plugin.json", "{}"),
                entry("plugin.sig", "signature")}));

    // One byte of one file, with the size unchanged.
    write_file(root / "data" / "table.bin", "tablf");
    const auto changed = measure_directory(root);
    REQUIRE(changed.has_value());
    const auto checked = check_files(declared, *changed, &detail);
    REQUIRE(checked.has_error());
    REQUIRE(checked.error() == plugin_errc::signature_invalid);
    REQUIRE(detail.find("data/table.bin") != std::string::npos);
    REQUIRE(hash_plugin(*changed) != before);
}

TEST_CASE("plugin measure: a file larger than one buffer is hashed whole")
{
    const scratch_directory scratch;
    const std::size_t chunk = std::size_t{64} * 1024;
    // Not a multiple of the buffer, a multiple of it, and one byte over.
    const std::string odd = pattern(chunk * 4 + 123);
    const std::string exact = pattern(chunk * 2);
    const std::string over = pattern(chunk + 1);
    write_file(scratch.path() / "odd.bin", odd);
    write_file(scratch.path() / "exact.bin", exact);
    write_file(scratch.path() / "over.bin", over);

    const auto measured = measure_directory(scratch.path());
    REQUIRE(measured.has_value());
    REQUIRE_EQ(measured->size(), std::size_t{3});
    REQUIRE((*measured)[0] == entry("exact.bin", exact));
    REQUIRE((*measured)[1] == entry("odd.bin", odd));
    REQUIRE((*measured)[2] == entry("over.bin", over));
    REQUIRE_EQ((*measured)[1].size, std::uint64_t{odd.size()});
}

TEST_CASE("plugin measure: an empty file has size zero")
{
    const scratch_directory scratch;
    write_file(scratch.path() / "empty", "");

    const auto measured = measure_directory(scratch.path());
    REQUIRE(measured.has_value());
    REQUIRE_EQ(measured->size(), std::size_t{1});
    REQUIRE_EQ((*measured)[0].path, "empty");
    REQUIRE_EQ((*measured)[0].size, std::uint64_t{0});
    REQUIRE_EQ(to_hex((*measured)[0].sha512), empty_digest);
}

TEST_CASE("plugin measure: a directory that is not there or not a directory")
{
    const scratch_directory scratch;
    const fs::path missing = scratch.path() / "missing";
    require_refused(missing, errc::not_found, "missing");

    write_file(scratch.path() / "file.txt", "text");
    require_refused(scratch.path() / "file.txt", plugin_errc::layout_invalid,
                    "file.txt");
}

TEST_CASE("plugin measure: a name that breaks the rules is refused")
{
    {
        const scratch_directory scratch;
        write_file(scratch.path() / "ok.txt", "fine");
        write_file(scratch.path() / "sub" / fs::path(u8"caf\u00e9.txt"),
                   "text");
        require_refused(scratch.path(), plugin_errc::layout_invalid,
                        "not plain ASCII");
    }
    {
        const scratch_directory scratch;
        write_file(scratch.path() / "ok.txt", "fine");
        write_file(scratch.path() / "sub" / "NUL.txt", "text");
        // On Windows the name opens the NUL device and no file is made.
        if (fs::is_regular_file(scratch.path() / "sub" / "NUL.txt"))
        {
            require_refused(scratch.path(), plugin_errc::layout_invalid,
                            "sub/NUL.txt");
            require_refused(scratch.path(), plugin_errc::layout_invalid,
                            "reserved name");
        }
    }
}

TEST_CASE("plugin measure: the depth limit is exact")
{
    {
        const scratch_directory scratch;
        write_file(nested_file(scratch.path(), max_plugin_depth - 1), "deep");
        const auto measured = measure_directory(scratch.path());
        REQUIRE(measured.has_value());
        REQUIRE_EQ(measured->size(), std::size_t{1});
        REQUIRE_EQ((*measured)[0].path, nested_name(max_plugin_depth - 1));
    }
    {
        const scratch_directory scratch;
        write_file(nested_file(scratch.path(), max_plugin_depth), "deep");
        require_refused(scratch.path(), plugin_errc::layout_invalid, "deeper");
        require_refused(scratch.path(), plugin_errc::layout_invalid,
                        "f.bin");
    }
    {
        // A directory below the limit cannot hold an allowed file, so it is
        // refused even when it is empty.
        const scratch_directory scratch;
        fs::create_directories(
            nested_file(scratch.path(), max_plugin_depth + 1).parent_path());
        require_refused(scratch.path(), plugin_errc::layout_invalid, "deeper");
    }
}

TEST_CASE("plugin measure: more files than a plugin may have are refused")
{
    // As many as a manifest may list, and the two files it never lists.
    REQUIRE_EQ(max_measured_files, max_files + 2);
    const scratch_directory scratch;
    for (std::size_t i = 0; i < max_measured_files; ++i)
    {
        std::string name = "f";
        name += std::to_string(i);
        write_file(scratch.path() / name, "");
    }
    const auto at_limit = measure_directory(scratch.path());
    REQUIRE(at_limit.has_value());
    REQUIRE_EQ(at_limit->size(), max_measured_files);

    write_file(scratch.path() / "one-more", "");
    require_refused(scratch.path(), plugin_errc::layout_invalid,
                    "more files than the limit");
}

TEST_CASE("plugin measure: a link to a file inside is refused")
{
    const scratch_directory scratch;
    write_file(scratch.path() / "real.txt", "text");
    if (!make_file_link(scratch.path() / "real.txt",
                        scratch.path() / "link.txt"))
    {
        VSS_SKIP("symbolic links cannot be made here");
    }
    require_refused(scratch.path(), plugin_errc::layout_invalid, "link.txt");
    require_refused(scratch.path(), plugin_errc::layout_invalid, "is a link");
}

TEST_CASE("plugin measure: a link to a file outside is refused")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "plugin";
    write_file(root / "real.txt", "text");
    write_file(scratch.path() / "outside.txt", "secret");
    if (!make_file_link(scratch.path() / "outside.txt",
                        root / "sub-link.txt"))
    {
        VSS_SKIP("symbolic links cannot be made here");
    }
    require_refused(root, plugin_errc::layout_invalid, "sub-link.txt");
}

TEST_CASE("plugin measure: a link to a directory is refused")
{
    {
        const scratch_directory scratch;
        write_file(scratch.path() / "real" / "file.txt", "text");
        if (!make_directory_link(scratch.path() / "real",
                                 scratch.path() / "link"))
        {
            VSS_SKIP("symbolic links cannot be made here");
        }
        require_refused(scratch.path(), plugin_errc::layout_invalid, "link");
        require_refused(scratch.path(), plugin_errc::layout_invalid,
                        "is a link");
    }
    {
        // A link to a directory outside, inside a directory of the plugin.
        const scratch_directory scratch;
        const fs::path root = scratch.path() / "plugin";
        write_file(root / "sub" / "file.txt", "text");
        write_file(scratch.path() / "outside" / "secret.txt", "secret");
        if (!make_directory_link(scratch.path() / "outside",
                                 root / "sub" / "out"))
        {
            VSS_SKIP("symbolic links cannot be made here");
        }
        require_refused(root, plugin_errc::layout_invalid, "sub/out");
    }
}

TEST_CASE("plugin measure: a plugin directory that is a link is refused")
{
    const scratch_directory scratch;
    write_file(scratch.path() / "real" / "plugin.json", "{}");
    const fs::path link_path = scratch.path() / "link";
    if (!make_directory_link(scratch.path() / "real", link_path))
    {
        VSS_SKIP("symbolic links cannot be made here");
    }
    require_refused(link_path, plugin_errc::layout_invalid, "link");

    // A trailing separator does not make the system follow it.
    require_refused(link_path / "", plugin_errc::layout_invalid, "link");
}

TEST_CASE("plugin measure: a directory given with a separator is measured")
{
    const scratch_directory scratch;
    write_file(scratch.path() / "plugin.json", "{}");
    const auto measured = measure_directory(scratch.path() / "");
    REQUIRE(measured.has_value());
    REQUIRE_EQ(measured->size(), std::size_t{1});
}

TEST_CASE("plugin measure: read_plugin_file reads a file whole")
{
    const scratch_directory scratch;
    const fs::path file = scratch.path() / "plugin.json";
    write_file(file, "{\"a\": 1}\n");

    std::string detail = "old";
    const auto text = read_plugin_file(file, 1024, &detail);
    REQUIRE(text.has_value());
    REQUIRE_EQ(*text, "{\"a\": 1}\n");
    REQUIRE(detail.empty());

    // Bytes are returned as they are, a zero byte and a CR LF included.
    write_file(file, std::string_view("a\0b\r\n", 5));
    const auto bytes = read_plugin_file(file, 1024);
    REQUIRE(bytes.has_value());
    REQUIRE(*bytes == std::string("a\0b\r\n", 5));

    write_file(file, "");
    const auto none = read_plugin_file(file, 0);
    REQUIRE(none.has_value());
    REQUIRE(none->empty());
}

TEST_CASE("plugin measure: read_plugin_file refuses what is not a file")
{
    const scratch_directory scratch;
    std::string detail = "old";
    const auto missing =
        read_plugin_file(scratch.path() / "plugin.sig", 1024, &detail);
    REQUIRE(missing.has_error());
    REQUIRE(missing.error() == plugin_errc::file_missing);
    REQUIRE(detail.find("plugin.sig") != std::string::npos);

    fs::create_directories(scratch.path() / "plugin.json");
    detail = "old";
    const auto directory =
        read_plugin_file(scratch.path() / "plugin.json", 1024, &detail);
    REQUIRE(directory.has_error());
    REQUIRE(directory.error() == plugin_errc::layout_invalid);
    REQUIRE(detail.find("plugin.json") != std::string::npos);
}

TEST_CASE("plugin measure: read_plugin_file refuses a link")
{
    const scratch_directory scratch;
    write_file(scratch.path() / "real.json", "{}");
    if (!make_file_link(scratch.path() / "real.json",
                        scratch.path() / "plugin.json"))
    {
        VSS_SKIP("symbolic links cannot be made here");
    }
    std::string detail;
    const auto text =
        read_plugin_file(scratch.path() / "plugin.json", 1024, &detail);
    REQUIRE(text.has_error());
    REQUIRE(text.error() == plugin_errc::layout_invalid);
    REQUIRE(detail.find("plugin.json") != std::string::npos);
    REQUIRE(detail.find("is a link") != std::string::npos);
}

TEST_CASE("plugin measure: read_plugin_file stops at the size limit")
{
    const scratch_directory scratch;
    const fs::path file = scratch.path() / "plugin.json";

    write_file(file, "12345");
    const auto exact = read_plugin_file(file, 5);
    REQUIRE(exact.has_value());
    REQUIRE_EQ(*exact, "12345");

    std::string detail;
    const auto over = read_plugin_file(file, 4, &detail);
    REQUIRE(over.has_error());
    REQUIRE(over.error() == plugin_errc::manifest_too_large);
    REQUIRE(detail.find("plugin.json") != std::string::npos);

    // More than one chunk, so the limit is met in the middle of the loop.
    const std::string big = pattern(std::size_t{64} * 1024 + 500);
    write_file(file, big);
    const auto whole = read_plugin_file(file, big.size());
    REQUIRE(whole.has_value());
    REQUIRE(*whole == big);
    const auto short_by_one = read_plugin_file(file, big.size() - 1);
    REQUIRE(short_by_one.has_error());
    REQUIRE(short_by_one.error() == plugin_errc::manifest_too_large);
    const auto chunk_exact = read_plugin_file(file, std::size_t{64} * 1024);
    REQUIRE(chunk_exact.has_error());
    REQUIRE(chunk_exact.error() == plugin_errc::manifest_too_large);

    // No limit that can be named overflows.
    const auto unlimited =
        read_plugin_file(file, std::numeric_limits<std::size_t>::max());
    REQUIRE(unlimited.has_value());
    REQUIRE(*unlimited == big);
    REQUIRE(read_file(file) == big);
}
