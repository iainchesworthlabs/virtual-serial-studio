//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/hash.hpp>
#include <vss/plugin/host/bundle_sections.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace vss::plugin;
using namespace vss::plugin::host;

namespace
{

namespace fs = std::filesystem;

using vss_test_support::scratch_directory;

void write_file(const fs::path& path, std::string_view content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

/// The entry for a file whose content is `content`.
offered_file entry(std::string path, std::string_view content)
{
    offered_file file;
    file.path = std::move(path);
    file.size = content.size();
    file.sha512 = to_hex(sha512_of(content));
    return file;
}

offered_section section(std::string id, std::vector<offered_file> files)
{
    offered_section out;
    out.id = std::move(id);
    out.title = "Recorded traffic";
    out.sensitive = true;
    out.files = std::move(files);
    return out;
}

section_rules rules()
{
    section_rules out;
    out.plugin_id = "com.example.diag";
    out.reserved_ids = {"traffic", "notes"};
    return out;
}

/// The problem found in a list alone, or nothing.
std::optional<bundle_problem> listed(const offered_section& value)
{
    const std::vector<offered_section> all = {value};
    const auto found = check_sections(all, rules());
    if (!found)
    {
        return std::nullopt;
    }
    return found->problem;
}

offered_section good()
{
    return section("traffic", {entry("traffic/1.jsonl", "line one\n"),
                               entry("traffic/recording.json", "{}")});
}

} // namespace

TEST_CASE("bundle sections: a good list is accepted")
{
    REQUIRE(!listed(good()));
    REQUIRE(!listed(section("traffic", {})));
    REQUIRE(!listed(section("com.example.diag.extra", {})));
    REQUIRE(!listed(section("notes", {entry("notes/notes.txt", "hi")})));
}

TEST_CASE("bundle sections: ids")
{
    REQUIRE(listed(section("", {})) == bundle_problem::id_bad);
    REQUIRE(listed(section("Traffic", {})) == bundle_problem::id_bad);
    REQUIRE(listed(section("a b", {})) == bundle_problem::id_bad);
    REQUIRE(listed(section("traffic/", {})) == bundle_problem::id_bad);
    REQUIRE(listed(section("traffic\n", {})) == bundle_problem::id_bad);

    // The edge is 128 bytes, and an id of the plugin's own can reach it.
    std::string edge = "com.example.diag.";
    edge.append(128 - edge.size(), 'x');
    REQUIRE(edge.size() == 128);
    REQUIRE(!listed(section(edge, {})));
    REQUIRE(listed(section(edge + "x", {})) == bundle_problem::id_bad);

    // Not the plugin's own and not reserved.
    REQUIRE(listed(section("logs", {})) == bundle_problem::id_not_allowed);
    REQUIRE(listed(section("com.example.diagnostics", {})) ==
            bundle_problem::id_not_allowed);
    REQUIRE(listed(section("com.example.diag", {})) ==
            bundle_problem::id_not_allowed);
    REQUIRE(listed(section("com.example.diag.", {})) ==
            bundle_problem::id_not_allowed);
    REQUIRE(listed(section("com.virtualserialstudio.x", {})) ==
            bundle_problem::id_not_allowed);

    const std::vector<offered_section> twice = {section("traffic", {}),
                                                section("notes", {}),
                                                section("traffic", {})};
    const auto found = check_sections(twice, rules());
    REQUIRE(found.has_value());
    REQUIRE(found->problem == bundle_problem::id_twice);
    REQUIRE_EQ(found->section, "traffic");

    std::vector<offered_section> many;
    for (std::size_t i = 0; i < max_offered_sections; ++i)
    {
        many.push_back(section("com.example.diag.s" + std::to_string(i), {}));
    }
    REQUIRE(!check_sections(many, rules()));
    many.push_back(section("com.example.diag.over", {}));
    REQUIRE(check_sections(many, rules())->problem ==
            bundle_problem::too_many_sections);
}

TEST_CASE("bundle sections: titles")
{
    offered_section value = good();
    value.title.assign(128, 't');
    REQUIRE(!listed(value));
    value.title.assign(129, 't');
    REQUIRE(listed(value) == bundle_problem::title_bad);
    value.title.clear();
    REQUIRE(listed(value) == bundle_problem::title_bad);
    value.title = "bad \xC0\x80";
    REQUIRE(listed(value) == bundle_problem::title_bad);
    value.title = "line\nbreak";
    REQUIRE(listed(value) == bundle_problem::title_bad);
    value.title = "gnp\xE2\x80\xAE";
    REQUIRE(listed(value) == bundle_problem::title_bad);
    // Markup is text.
    value.title = "<b>x</b> & \xD7\xA9";
    REQUIRE(!listed(value));
}

TEST_CASE("bundle sections: the files of a list")
{
    // 16 files, and one more.
    std::vector<offered_file> files;
    for (int i = 0; i < 16; ++i)
    {
        files.push_back(entry("traffic/" + std::to_string(i) + ".bin", "x"));
    }
    REQUIRE(!listed(section("traffic", files)));
    files.push_back(entry("traffic/16.bin", "x"));
    REQUIRE(listed(section("traffic", files)) == bundle_problem::too_many_files);

    // Paths that break the rules of check_relative_path.
    const std::vector<std::string> bad_paths = {
        "",
        "/etc/passwd",
        "../x",
        "traffic/../../x",
        "traffic/./x",
        "traffic//x",
        "traffic\\x",
        "C:\\x",
        "C:/x",
        "traffic/x.",
        "traffic/x ",
        "traffic/NUL",
        "traffic/com1.txt",
        "traffic/a:b",
        "traffic/a*b",
        "traffic/caf\xC3\xA9",
        "traffic/x\ny",
        "traffic/",
        std::string(241, 'a'),
    };
    for (const std::string& path : bad_paths)
    {
        REQUIRE(listed(section("traffic", {entry(path, "x")})) ==
                bundle_problem::path_bad);
    }
    REQUIRE(!listed(section("traffic", {entry(std::string(240, 'a'), "x")})));

    // The same file twice, also when the case is ignored.
    REQUIRE(listed(section("traffic", {entry("a/b", "x"), entry("a/b", "y")})) ==
            bundle_problem::path_twice);
    REQUIRE(listed(section("traffic", {entry("a/b", "x"), entry("A/B", "y")})) ==
            bundle_problem::path_twice);
}

TEST_CASE("bundle sections: sizes and hashes")
{
    offered_file file = entry("traffic/a", "x");
    file.size = max_section_bytes;
    REQUIRE(!listed(section("traffic", {file})));
    file.size = max_section_bytes + 1;
    REQUIRE(listed(section("traffic", {file})) ==
            bundle_problem::size_over_limit);

    offered_file half = entry("traffic/a", "x");
    half.size = max_section_bytes / 2;
    offered_file other = entry("traffic/b", "x");
    other.size = max_section_bytes / 2;
    REQUIRE(!listed(section("traffic", {half, other})));
    other.size += 1;
    REQUIRE(listed(section("traffic", {half, other})) ==
            bundle_problem::section_over_limit);

    const std::string good_hash = entry("a", "x").sha512;
    const std::vector<std::string> bad_hashes = {
        "",
        good_hash.substr(1),
        good_hash + "0",
        std::string(128, 'G'),
        std::string(128, 'A'),  // upper case
        std::string(127, '0') + "-",
    };
    for (const std::string& hash : bad_hashes)
    {
        offered_file value = entry("traffic/a", "x");
        value.sha512 = hash;
        REQUIRE(listed(section("traffic", {value})) == bundle_problem::hash_bad);
    }
}

namespace
{

/// A directory with the files of `good()` written, and the result of
/// verifying `value` against it.
struct world
{
    scratch_directory directory;

    world()
    {
        write_file(directory.path() / "traffic" / "1.jsonl", "line one\n");
        write_file(directory.path() / "traffic" / "recording.json", "{}");
    }

    boost::system::result<std::vector<verified_section>, bundle_failure>
    verify(const offered_section& value) const
    {
        const std::vector<offered_section> all = {value};
        return verify_sections(directory.path(), all, rules());
    }
};

bundle_problem problem_of(
    const boost::system::result<std::vector<verified_section>,
                                bundle_failure>& verdict)
{
    REQUIRE(verdict.has_error());
    return verdict.error().problem;
}

} // namespace

TEST_CASE("bundle sections: good files are verified from their handles")
{
    world here;
    const auto verdict = here.verify(good());
    REQUIRE(verdict.has_value());
    REQUIRE_EQ(verdict->size(), std::size_t{1});
    const verified_section& done = (*verdict)[0];
    REQUIRE_EQ(done.id, "traffic");
    REQUIRE_EQ(done.title, "Recorded traffic");
    REQUIRE(done.sensitive);
    REQUIRE_EQ(done.bytes, std::uint64_t{9 + 2});
    REQUIRE_EQ(done.files.size(), std::size_t{2});
    REQUIRE_EQ(done.files[0].path, "traffic/1.jsonl");
    REQUIRE_EQ(done.files[0].size, std::uint64_t{9});
    REQUIRE_EQ(done.files[0].sha512, entry("x", "line one\n").sha512);
    REQUIRE(done.files[0].full_path ==
            here.directory.path() / "traffic" / "1.jsonl");

    // An empty file, and a section with no files.
    write_file(here.directory.path() / "traffic" / "empty", "");
    REQUIRE(here.verify(section("traffic", {entry("traffic/empty", "")}))
                .has_value());
    REQUIRE(here.verify(section("traffic", {})).has_value());
}

TEST_CASE("bundle sections: a bad list is refused before any file is opened")
{
    world here;
    const std::vector<offered_section> all = {
        section("traffic", {entry("../x", "x")})};
    const auto verdict = verify_sections(here.directory.path(), all, rules());
    REQUIRE(problem_of(verdict) == bundle_problem::path_bad);
    REQUIRE_EQ(verdict.error().path, "../x");
    REQUIRE_EQ(verdict.error().section, "traffic");
}

TEST_CASE("bundle sections: files that are missing, changed or not plain")
{
    world here;

    // Not there.
    REQUIRE(problem_of(here.verify(
                section("traffic", {entry("traffic/none", "x")}))) ==
            bundle_problem::file_missing);
    REQUIRE(problem_of(here.verify(
                section("traffic", {entry("nodir/x", "x")}))) ==
            bundle_problem::file_missing);

    // The first failure is the one given: the first file is fine.
    const auto second = here.verify(section(
        "traffic", {entry("traffic/1.jsonl", "line one\n"),
                    entry("traffic/none", "x")}));
    REQUIRE(problem_of(second) == bundle_problem::file_missing);
    REQUIRE_EQ(second.error().path, "traffic/none");

    // The size changed between the list and the check, larger and smaller.
    offered_file listed_size = entry("traffic/1.jsonl", "line one\n");
    listed_size.size = 8;
    REQUIRE(problem_of(here.verify(section("traffic", {listed_size}))) ==
            bundle_problem::size_changed);
    listed_size.size = 10;
    REQUIRE(problem_of(here.verify(section("traffic", {listed_size}))) ==
            bundle_problem::size_changed);

    // The content changed and the size did not.
    write_file(here.directory.path() / "traffic" / "1.jsonl", "line two\n");
    REQUIRE(problem_of(here.verify(
                section("traffic", {entry("traffic/1.jsonl", "line one\n")}))) ==
            bundle_problem::hash_changed);

    // A wrong hash that is well formed.
    offered_file wrong = entry("traffic/recording.json", "{}");
    wrong.sha512 = std::string(128, '0');
    REQUIRE(problem_of(here.verify(section("traffic", {wrong}))) ==
            bundle_problem::hash_changed);

    // A folder where a file is listed, and a file where a folder is.
    fs::create_directories(here.directory.path() / "traffic" / "folder");
    REQUIRE(problem_of(here.verify(
                section("traffic", {entry("traffic/folder", "x")}))) ==
            bundle_problem::file_not_plain);
    REQUIRE(problem_of(here.verify(section(
                "traffic", {entry("traffic/recording.json/x", "x")}))) ==
            bundle_problem::file_not_plain);
}

TEST_CASE("bundle sections: a file over the limit is refused by its size")
{
    world here;
    offered_file big = entry("traffic/1.jsonl", "line one\n");
    big.size = max_section_bytes + 1;
    REQUIRE(problem_of(here.verify(section("traffic", {big}))) ==
            bundle_problem::size_over_limit);
}

TEST_CASE("bundle sections: a link is never followed")
{
    world here;
    const fs::path outside_dir = here.directory.path() / "outside";
    write_file(outside_dir / "secret.txt", "secret");
    const fs::path data = here.directory.path() / "data";
    write_file(data / "traffic" / "1.jsonl", "line one\n");

    std::error_code ec;
    fs::create_symlink(outside_dir / "secret.txt",
                       data / "traffic" / "link.txt", ec);
    if (ec)
    {
        VSS_SKIP("symbolic links cannot be made here");
    }
    fs::create_directory_symlink(outside_dir, data / "folder_link", ec);
    REQUIRE(!ec);

    const auto check = [&](const offered_section& value) {
        const std::vector<offered_section> all = {value};
        return verify_sections(data, all, rules());
    };
    REQUIRE(problem_of(check(
                section("traffic", {entry("traffic/link.txt", "secret")}))) ==
            bundle_problem::file_is_link);
    REQUIRE(problem_of(check(
                section("traffic", {entry("folder_link/secret.txt",
                                          "secret")}))) ==
            bundle_problem::file_is_link);
    REQUIRE(check(section("traffic",
                          {entry("traffic/1.jsonl", "line one\n")}))
                .has_value());
}

TEST_CASE("bundle sections: the data directory is checked")
{
    scratch_directory scratch;
    const std::vector<offered_section> none;
    REQUIRE(verify_sections(scratch.path(), none, rules()).has_value());

    const std::vector<offered_section> all = {section("traffic", {})};
    REQUIRE(verify_sections(scratch.path() / "absent", all, rules())
                .error()
                .problem == bundle_problem::directory_bad);

    // A file where the directory should be.
    write_file(scratch.path() / "file", "x");
    REQUIRE(verify_sections(scratch.path() / "file", all, rules())
                .error()
                .problem == bundle_problem::directory_bad);

    std::error_code ec;
    fs::create_directory(scratch.path() / "real");
    fs::create_directory_symlink(scratch.path() / "real",
                                 scratch.path() / "link", ec);
    if (!ec)
    {
        REQUIRE(verify_sections(scratch.path() / "link", all, rules())
                    .error()
                    .problem == bundle_problem::directory_bad);
    }
}

TEST_CASE("bundle sections: the problems have names")
{
    REQUIRE_EQ(to_string(bundle_problem::hash_changed), "hash_changed");
    REQUIRE_EQ(to_string(bundle_problem::file_is_link), "file_is_link");
}
