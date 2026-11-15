//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence folders on macOS, written out in full.
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

TEST_CASE("licence locations on macOS: the machine folder")
{
    REQUIRE(lic::machine_licence_directory() ==
            fs::path(
                "/Library/Application Support/Virtual Serial Studio/licences"));
}

TEST_CASE("licence locations on macOS: the user's folder is in the library of "
          "$HOME")
{
    const scoped_environment home("HOME", "/Users/tester");
    REQUIRE(lic::user_licence_directory() ==
            fs::path("/Users/tester/Library/Application Support/Virtual "
                     "Serial Studio/licences"));
    REQUIRE(lic::default_store_options().clock_file ==
            fs::path("/Users/tester/Library/Application Support/Virtual "
                     "Serial Studio/licence-clock.json"));
}
