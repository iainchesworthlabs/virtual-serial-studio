//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the fuzz replay driver and vss_perf turn a path given on the command line
// into the file they read or write.
//

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include "perf_common.hpp"
#include "replay_inputs.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using vss_test_support::scratch_directory;

void write_text(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

} // namespace

TEST_CASE("fuzz replay: a file or a directory is collected once, resolved")
{
    scratch_directory scratch;
    fs::create_directories(scratch.path() / "corpus" / "nested");
    write_text(scratch.path() / "corpus" / "one", "1");
    write_text(scratch.path() / "corpus" / "nested" / "two", "22");
    write_text(scratch.path() / "corpus" / ".gitkeep", "");

    std::vector<fs::path> found;
    REQUIRE(vss_fuzz_replay::collect(scratch.path() / "corpus", found));
    REQUIRE_EQ(found.size(), std::size_t{2});

    // A path that wanders through `..` leads to the same files, and each
    // collected path is the resolved one.
    std::vector<fs::path> wandering;
    REQUIRE(vss_fuzz_replay::collect(
        scratch.path() / "corpus" / "nested" / ".." / "one", wandering));
    REQUIRE_EQ(wandering.size(), std::size_t{1});
    REQUIRE_EQ(wandering.front(),
               fs::canonical(scratch.path() / "corpus" / "one"));
    REQUIRE(!wandering.front().string().contains(".."));
}

TEST_CASE("fuzz replay: a path that does not exist is refused")
{
    scratch_directory scratch;
    std::vector<fs::path> found;
    REQUIRE(!vss_fuzz_replay::collect(scratch.path() / "missing", found));
    REQUIRE(found.empty());
}

TEST_CASE("fuzz replay: a file is read whole and an unreadable one is refused")
{
    scratch_directory scratch;
    write_text(scratch.path() / "input", "abc");
    const auto bytes = vss_fuzz_replay::read_file(scratch.path() / "input");
    REQUIRE(bytes.has_value());
    REQUIRE_EQ(bytes->size(), std::size_t{3});
    REQUIRE(!vss_fuzz_replay::read_file(scratch.path() / "missing"));
}

TEST_CASE("vss_perf: the output path is resolved before it is written")
{
    scratch_directory scratch;
    std::string reason;

    // The file need not exist, and `..` is removed.
    const auto fresh = vss_perf::resolve_output_path(
        (scratch.path() / "sub" / ".." / "results.json").string(), reason);
    REQUIRE(fresh.has_value());
    REQUIRE_EQ(fresh->filename().string(), std::string("results.json"));
    REQUIRE_EQ(*fresh, fs::weakly_canonical(scratch.path() / "results.json"));
    REQUIRE(!fresh->string().contains(".."));

    write_text(scratch.path() / "old.json", "[]");
    REQUIRE(vss_perf::resolve_output_path(
                (scratch.path() / "old.json").string(), reason)
                .has_value());
}

TEST_CASE("vss_perf: an output path that cannot be written is refused")
{
    scratch_directory scratch;
    std::string reason;

    REQUIRE(!vss_perf::resolve_output_path("", reason));
    REQUIRE(!reason.empty());

    reason.clear();
    REQUIRE(!vss_perf::resolve_output_path(scratch.path().string(), reason));
    REQUIRE(!reason.empty());

    reason.clear();
    REQUIRE(!vss_perf::resolve_output_path(
        (scratch.path() / "no-such-directory" / "results.json").string(),
        reason));
    REQUIRE(!reason.empty());
}
