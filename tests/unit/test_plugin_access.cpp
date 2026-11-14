//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The access check where the answer is the same on every system: a folder a
// test made itself is one that someone other than an administrator can
// change.  What only an administrator can change is in the platform tests.
//

#include <vss/core/error.hpp>
#include <vss/plugin/access.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/verify.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace vss;
using namespace vss::plugin;
using vss_test_support::scratch_directory;

namespace
{

namespace fs = std::filesystem;

void write_file(const fs::path& path, std::string_view text)
{
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file.good());
}

} // namespace

TEST_CASE("plugin access: the two answers have names")
{
    REQUIRE(to_string(write_access::administrators_only) ==
            "administrators_only");
    REQUIRE(to_string(write_access::others_too) == "others_too");
}

TEST_CASE("plugin access: a path that is not there is an error")
{
    const scratch_directory scratch;
    std::string detail;
    const auto access = who_can_write(scratch.path() / "missing", &detail);
    REQUIRE(access.has_error());
    REQUIRE(access.error() == errc::not_found);
    REQUIRE(detail.find("is not there") != std::string::npos);

    const auto kind = kind_of_install(scratch.path() / "missing");
    REQUIRE(kind.has_error());
    REQUIRE(kind.error() == errc::not_found);
}

TEST_CASE("plugin access: what a test made can be changed by others")
{
    const scratch_directory scratch;
    write_file(scratch.path() / "file.txt", "text");
    fs::create_directories(scratch.path() / "bin");

    for (const fs::path& path :
         {scratch.path(), scratch.path() / "file.txt", scratch.path() / "bin"})
    {
        std::string detail;
        const auto access = who_can_write(path, &detail);
        REQUIRE(access.has_value());
        REQUIRE(*access == write_access::others_too);
        // The line says which file or folder gave the answer.
        REQUIRE(!detail.empty());
    }

    // A copy of Studio in such a folder is one user's.
    const auto kind = kind_of_install(scratch.path() / "bin");
    REQUIRE(kind.has_value());
    REQUIRE(*kind == install_kind::for_one_user);
}

TEST_CASE("plugin access: no policy file is no policy")
{
    const scratch_directory scratch;
    const loaded_policy loaded = load_policy(scratch.path() / "policy.json");
    REQUIRE(!loaded.rules.has_value());
    REQUIRE(!loaded.problem.failed());
    REQUIRE(loaded.detail.empty());
}

TEST_CASE("plugin access: a policy file a user could have written is not "
          "used")
{
    const scratch_directory scratch;
    const fs::path file = scratch.path() / "policy.json";
    // It asks for everything to be allowed.  It must not get that.
    write_file(file, R"({ "policy_version": 1, "allow": "any",
                          "min_sandbox": "none", "allow_no_sandbox": true })");

    const loaded_policy loaded = load_policy(file);
    REQUIRE(loaded.problem == errc::permission_denied);
    REQUIRE(loaded.detail.find("not an administrator") != std::string::npos);
    REQUIRE(loaded.rules.has_value());
    const policy& rules = loaded.rules.value();
    REQUIRE(rules == official_only_policy());
    REQUIRE(rules.allow == allow_level::official_only);
    REQUIRE(!rules.allow_no_sandbox);
}

TEST_CASE("plugin access: a policy file that is not a plain file is not used")
{
    const scratch_directory scratch;
    const fs::path file = scratch.path() / "policy.json";
    fs::create_directories(file);

    const loaded_policy loaded = load_policy(file);
    REQUIRE(loaded.problem == plugin_errc::layout_invalid);
    REQUIRE(!loaded.detail.empty());
    REQUIRE(loaded.rules.has_value());
    REQUIRE(loaded.rules.value() == official_only_policy());
}

TEST_CASE("plugin access: a policy file that is too large is not used")
{
    const scratch_directory scratch;
    const fs::path file = scratch.path() / "policy.json";
    write_file(file, std::string(max_policy_size + 1, ' '));

    const loaded_policy loaded = load_policy(file);
    REQUIRE(loaded.problem == errc::config_error);
    REQUIRE(loaded.rules.has_value());
    REQUIRE(loaded.rules.value() == official_only_policy());
}
