//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence folders on Linux, written out in full, and the rules for
// $XDG_DATA_HOME.
//

#include <vss/license/store.hpp>

#include "scoped_environment_posix.hpp"
#include "test_framework.hpp"

#include <filesystem>

namespace lic = vss::license;
using vss_test_support::scoped_environment;

namespace
{

namespace fs = std::filesystem;

} // namespace

TEST_CASE("licence locations on Linux: the machine folder")
{
    REQUIRE(lic::machine_licence_directory() ==
            fs::path("/etc/virtual-serial-studio/licences"));
}

TEST_CASE("licence locations on Linux: $XDG_DATA_HOME is used when absolute")
{
    const scoped_environment home("HOME", "/home/tester");
    const scoped_environment data("XDG_DATA_HOME", "/srv/data");

    REQUIRE(lic::user_licence_directory() ==
            fs::path("/srv/data/virtual-serial-studio/licences"));
    REQUIRE(lic::default_store_options().clock_file ==
            fs::path("/srv/data/virtual-serial-studio/licence-clock.json"));
}

TEST_CASE("licence locations on Linux: $HOME is the fallback")
{
    const scoped_environment home("HOME", "/home/tester");
    const fs::path expected =
        "/home/tester/.local/share/virtual-serial-studio/licences";

    {
        // Unset.
        const scoped_environment data("XDG_DATA_HOME", nullptr);
        REQUIRE(lic::user_licence_directory() == expected);
    }
    {
        // Empty.
        const scoped_environment data("XDG_DATA_HOME", "");
        REQUIRE(lic::user_licence_directory() == expected);
    }
    {
        // Not an absolute path.
        const scoped_environment data("XDG_DATA_HOME", "data/relative");
        REQUIRE(lic::user_licence_directory() == expected);
    }
}

TEST_CASE("licence locations on Linux: no usable variable gives no user "
          "folder")
{
    const scoped_environment home("HOME", nullptr);
    const scoped_environment data("XDG_DATA_HOME", "data/relative");
    REQUIRE(lic::user_licence_directory().empty());
}
