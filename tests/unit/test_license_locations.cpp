//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where licences are looked for and what this computer is called, in what
// holds on every system. The exact folders of each system are in the platform
// tests. Nothing here touches the real folders.
//

#include <vss/license/signed_file.hpp>
#include <vss/license/store.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace lic = vss::license;

namespace
{

namespace fs = std::filesystem;

} // namespace

TEST_CASE("licence locations: the machine folder is absolute and ends in "
          "licences")
{
    const fs::path machine = lic::machine_licence_directory();
    REQUIRE(!machine.empty());
    REQUIRE(machine.is_absolute());
    REQUIRE(machine.filename() == fs::path("licences"));
}

TEST_CASE("licence locations: the user's folder is absolute and ends in "
          "licences")
{
    // It is empty when the system cannot say where the user's data goes.
    const fs::path user = lic::user_licence_directory();
    if (user.empty())
        VSS_SKIP("this system gives no user folder");
    REQUIRE(user.is_absolute());
    REQUIRE(user.filename() == fs::path("licences"));
    REQUIRE(user != lic::machine_licence_directory());
}

TEST_CASE("licence locations: the computer has a name")
{
    const std::string name = lic::computer_name();
    REQUIRE(!name.empty());
    REQUIRE(name.size() <= 255);
    REQUIRE(std::ranges::none_of(
        name, [](char c) { return static_cast<unsigned char>(c) < 0x20; }));
}

TEST_CASE("licence locations: the default options name the real folders")
{
    const lic::store_options options = lic::default_store_options();
    REQUIRE(options.user_directory == lic::user_licence_directory());
    REQUIRE(options.machine_directory == lic::machine_licence_directory());
    REQUIRE_EQ(options.computer_name, lic::computer_name());
    // The clock file is beside the licence folder, not in it.
    if (!options.user_directory.empty())
    {
        REQUIRE(options.clock_file.parent_path() ==
                options.user_directory.parent_path());
        REQUIRE(options.clock_file.filename() ==
                fs::path("licence-clock.json"));
    }
    else
    {
        REQUIRE(options.clock_file.empty());
    }
    // The system clock, and no build time until the program sets one.
    REQUIRE(!options.clock);
    REQUIRE(options.build_time == lic::time_point::min());
}

TEST_CASE("licence locations: the default options carry the program's keys "
          "and no test key")
{
    const lic::store_options options = lic::default_store_options();
    const auto product = lic::signed_file::product_trust_list();
    REQUIRE_EQ(options.keys.size(), product.size());
    REQUIRE(!options.keys.empty());
    for (const lic::signed_file::trusted_key& key : options.keys)
        REQUIRE(!key.key_id.starts_with(lic::signed_file::test_key_prefix));
}
