//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The access check on Linux and macOS, against files that every such system
// has and that belong to root.
//

#include <vss/core/error.hpp>
#include <vss/plugin/access.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/verify.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace vss;
using namespace vss::plugin;
using vss_test_support::scratch_directory;

namespace
{

namespace fs = std::filesystem;

/// A folder of the system that belongs to root and that only root may
/// write to, with every folder above it the same.  Skips the test where
/// there is none, such as a container that maps another user to root.
fs::path system_folder()
{
    for (const char* candidate : {"/usr/bin", "/usr", "/bin"})
    {
        const fs::path resolved = fs::weakly_canonical(candidate);
        bool good = true;
        for (fs::path part = resolved; good; part = part.parent_path())
        {
            struct stat info = {};
            good = ::lstat(part.c_str(), &info) == 0 && info.st_uid == 0 &&
                   (info.st_mode & (S_IWGRP | S_IWOTH)) == 0;
            if (part == part.root_path())
                break;
        }
        if (good)
            return resolved;
    }
    VSS_SKIP("no system folder here belongs to root alone");
}

} // namespace

TEST_CASE("plugin access: a system folder can be changed by root only")
{
    const fs::path folder = system_folder();
    std::string detail = "old";
    const auto access = who_can_write(folder, &detail);
    REQUIRE(access.has_value());
    REQUIRE(*access == write_access::administrators_only);
    REQUIRE(detail.empty());

    // A copy of Studio there is installed for everyone.
    const auto kind = kind_of_install(folder);
    REQUIRE(kind.has_value());
    REQUIRE(*kind == install_kind::for_everyone);
}

TEST_CASE("plugin access: a folder below a user's folder is the user's, "
          "whatever its own mode")
{
    // The folder itself lets nobody but its owner write, but the owner is
    // not root.
    const scratch_directory scratch;
    const fs::path inner = scratch.path() / "inner";
    fs::create_directories(inner);
    fs::permissions(inner, fs::perms::owner_all);

    std::string detail;
    const auto access = who_can_write(inner, &detail);
    REQUIRE(access.has_value());
    REQUIRE(*access == write_access::others_too);
    REQUIRE(!detail.empty());
}

TEST_CASE("plugin access: a link to a system folder is judged by where it "
          "lies too")
{
    // The link points at a folder only root can change, but the link is in
    // a folder a user can change, so the user can point it elsewhere.
    const fs::path folder = system_folder();
    const scratch_directory scratch;
    const fs::path link = scratch.path() / "link";
    std::error_code failed;
    fs::create_directory_symlink(folder, link, failed);
    if (failed)
        VSS_SKIP("symbolic links cannot be made here");

    // Resolved, the link is the system folder: that is what a program that
    // opens it gets.  The answer for the folder that holds the link is what
    // says the link cannot be trusted.
    const auto holder = who_can_write(scratch.path());
    REQUIRE(holder.has_value());
    REQUIRE(*holder == write_access::others_too);
}

TEST_CASE("plugin access: a root-owned file that is not a policy is refused "
          "for what it says")
{
    // /etc/passwd is on every such system, belongs to root and is not a
    // policy.  It gets past the question of who wrote it and fails at
    // reading, and the answer is still "official plugins only".
    const fs::path file = "/etc/passwd";
    const auto access = who_can_write(file);
    if (!access.has_value() ||
        *access != write_access::administrators_only)
        VSS_SKIP("/etc/passwd does not belong to root alone here");

    const loaded_policy loaded = load_policy(file);
    REQUIRE(loaded.problem == errc::config_error);
    REQUIRE(!loaded.detail.empty());
    REQUIRE(loaded.rules.has_value());
    REQUIRE(loaded.rules.value() == official_only_policy());
}
