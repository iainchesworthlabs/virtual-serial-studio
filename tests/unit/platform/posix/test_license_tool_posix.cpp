//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "license_tool_fixture.hpp"

#include <filesystem>
#include <string>

using namespace vss_test_support::license_tool;

TEST_CASE("licence tool: the private key file is for its owner only")
{
    scratch_directory scratch;
    const std::string key = path_in(scratch, "throwaway.key");
    REQUIRE_EQ(
        run_tool({"keygen", "--key-id", "lic-unit-a", "--out-private", key})
            .status,
        vss::license::tool::exit_ok);

    const auto permissions =
        std::filesystem::status(std::filesystem::path(key)).permissions();
    REQUIRE(permissions == (std::filesystem::perms::owner_read |
                            std::filesystem::perms::owner_write));
}
