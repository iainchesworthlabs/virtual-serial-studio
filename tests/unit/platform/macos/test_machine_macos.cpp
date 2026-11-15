//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The product's folders on macOS, written out in full.
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

TEST_CASE("machine folders on macOS: the machine folder")
{
    REQUIRE(machine_folder() ==
            fs::path("/Library/Application Support/Virtual Serial Studio"));
}

TEST_CASE("machine folders on macOS: the user's folder is under $HOME")
{
    // The XDG variable means nothing on macOS.
    const scoped_environment data("XDG_DATA_HOME", "/srv/data");
    const scoped_environment home("HOME", "/Users/tester");
    REQUIRE(user_folder() ==
            fs::path("/Users/tester/Library/Application Support/"
                     "Virtual Serial Studio"));
}

TEST_CASE("machine folders on macOS: no usable $HOME gives no user folder")
{
    {
        const scoped_environment home("HOME", nullptr);
        REQUIRE(user_folder().empty());
    }
    {
        const scoped_environment home("HOME", "");
        REQUIRE(user_folder().empty());
    }
    {
        const scoped_environment home("HOME", "tester/home");
        REQUIRE(user_folder().empty());
    }
}

TEST_CASE("machine folders on macOS: the machine folder does not use $HOME")
{
    const scoped_environment home("HOME", nullptr);
    REQUIRE(machine_folder() ==
            fs::path("/Library/Application Support/Virtual Serial Studio"));
}
