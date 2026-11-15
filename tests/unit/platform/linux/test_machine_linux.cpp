//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The product's folders on Linux, written out in full, and the rules for
// $XDG_DATA_HOME and $HOME.
//

#include <vss/machine/folders.hpp>

#include "scoped_environment_posix.hpp"
#include "test_framework.hpp"

#include <filesystem>

using namespace vss::machine;
using vss_test_support::scoped_environment;

namespace
{

namespace fs = std::filesystem;

} // namespace

TEST_CASE("machine folders on Linux: the machine folder")
{
    REQUIRE(machine_folder() == fs::path("/etc/virtual-serial-studio"));
}

TEST_CASE("machine folders on Linux: $XDG_DATA_HOME is used when absolute")
{
    const scoped_environment home("HOME", "/home/tester");
    const scoped_environment data("XDG_DATA_HOME", "/srv/data");
    REQUIRE(user_folder() == fs::path("/srv/data/virtual-serial-studio"));
}

TEST_CASE("machine folders on Linux: $HOME is the fallback")
{
    const scoped_environment home("HOME", "/home/tester");
    const fs::path expected = "/home/tester/.local/share/virtual-serial-studio";

    {
        // Unset.
        const scoped_environment data("XDG_DATA_HOME", nullptr);
        REQUIRE(user_folder() == expected);
    }
    {
        // Empty.
        const scoped_environment data("XDG_DATA_HOME", "");
        REQUIRE(user_folder() == expected);
    }
    {
        // Not an absolute path.
        const scoped_environment data("XDG_DATA_HOME", "data/relative");
        REQUIRE(user_folder() == expected);
    }
}

TEST_CASE("machine folders on Linux: no usable variable gives no user folder")
{
    {
        const scoped_environment home("HOME", nullptr);
        const scoped_environment data("XDG_DATA_HOME", nullptr);
        REQUIRE(user_folder().empty());
    }
    {
        const scoped_environment home("HOME", "");
        const scoped_environment data("XDG_DATA_HOME", "data/relative");
        REQUIRE(user_folder().empty());
    }
    {
        // A $HOME that is not absolute is not used either.
        const scoped_environment home("HOME", "tester/home");
        const scoped_environment data("XDG_DATA_HOME", nullptr);
        REQUIRE(user_folder().empty());
    }
}

TEST_CASE("machine folders on Linux: the machine folder does not use the "
          "environment")
{
    const scoped_environment home("HOME", nullptr);
    const scoped_environment data("XDG_DATA_HOME", "/srv/data");
    REQUIRE(machine_folder() == fs::path("/etc/virtual-serial-studio"));
}
