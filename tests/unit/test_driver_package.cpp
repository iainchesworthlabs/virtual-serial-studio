//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The version of a driver package: reading DriverVer, the way Windows
// compares two (date first, then the four parts) and the bytes of the driver
// store. All plain text and numbers, so it runs on every system.
//

#include "test_framework.hpp"

#include <vss/ports/driver_package.hpp>
#include <vss/ports/driver_setup.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using namespace vss;

namespace
{

driver_package_version ver(const char* text)
{
    const auto parsed = parse_driver_ver(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

} // namespace

TEST_CASE("driver package: DriverVer is read as a date and four parts")
{
    const auto version = ver("11/18/2026,1.5.1.0");
    REQUIRE_EQ(driver_parts_text(version), std::string("1.5.1.0"));
    REQUIRE_EQ(driver_date_text(version), std::string("2026-11-18"));
    REQUIRE_EQ(describe_driver_package(version),
               std::string("1.5.1.0 (2026-11-18)"));
    // Spaces around the parts are allowed, as in an INF file.
    REQUIRE(parse_driver_ver("  1/2/2026 , 1.2.3.4 ").has_value());
    REQUIRE_EQ(driver_date_text(ver("1/2/2026,1.2.3.4")),
               std::string("2026-01-02"));
}

TEST_CASE("driver package: text that is not a DriverVer is refused")
{
    REQUIRE(!parse_driver_ver("").has_value());
    REQUIRE(!parse_driver_ver("11/18/2026").has_value());
    REQUIRE(!parse_driver_ver("11/18/2026,1.5.1").has_value());
    REQUIRE(!parse_driver_ver("11/18/2026,1.5.1.0.9").has_value());
    REQUIRE(!parse_driver_ver("13/18/2026,1.5.1.0").has_value());
    REQUIRE(!parse_driver_ver("0/18/2026,1.5.1.0").has_value());
    REQUIRE(!parse_driver_ver("11/18/2026,1.5.1.70000").has_value());
    REQUIRE(!parse_driver_ver("11/18/2026,1.5.x.0").has_value());
}

TEST_CASE("driver package: the DriverVer line is found in an INF file")
{
    const std::string inf =
        "; a comment\r\n[Version]\r\nSignature=\"$WINDOWS NT$\"\r\n"
        "  DriverVer = 11/18/2026,1.5.1.0\r\nPnpLockdown=1\r\n";
    const auto version = parse_inf_driver_ver(inf);
    REQUIRE(version.has_value());
    REQUIRE_EQ(describe_driver_package(*version),
               std::string("1.5.1.0 (2026-11-18)"));
    REQUIRE(!parse_inf_driver_ver("[Version]\nSignature=x\n").has_value());
    REQUIRE(!parse_inf_driver_ver("DriverVer=nonsense\n").has_value());
    REQUIRE(!read_inf_driver_version("no-such-folder/no-such.inf").has_value());
}

TEST_CASE("driver package: the date decides first and the version second")
{
    const auto base = ver("11/18/2026,1.5.1.0");
    REQUIRE(compare_driver_packages(base, base) == driver_package_order::same);
    REQUIRE(compare_driver_packages(ver("11/18/2026,1.5.0.0"), base) ==
            driver_package_order::older);
    REQUIRE(compare_driver_packages(ver("11/18/2026,1.5.2.0"), base) ==
            driver_package_order::newer);
    REQUIRE(compare_driver_packages(ver("11/18/2026,1.6.0.0"), base) ==
            driver_package_order::newer);
    // Numbers, not text: 1.5.10.0 is above 1.5.9.0.
    REQUIRE(compare_driver_packages(ver("11/18/2026,1.5.10.0"),
                                    ver("11/18/2026,1.5.9.0")) ==
            driver_package_order::newer);
    // A later date wins over a lower version, as Windows does it.
    REQUIRE(compare_driver_packages(ver("11/19/2026,1.0.0.0"), base) ==
            driver_package_order::newer);
    REQUIRE(compare_driver_packages(ver("11/17/2026,9.9.9.9"), base) ==
            driver_package_order::older);
    REQUIRE(compare_driver_packages(ver("1/1/2027,1.0.0.0"), base) ==
            driver_package_order::newer);
}

TEST_CASE("driver package: the driver store's bytes are read like Windows "
          "writes them")
{
    // The Version value of a package in SYSTEM\DriverDatabase\DriverPackages:
    // 8 bytes, the class GUID, the date as a FILETIME, the version in 64 bits.
    // 2026-11-18 00:00 UTC is 134774 + 20775 days after 1601-01-01.
    constexpr std::uint64_t ticks_per_day = 864000000000ULL;
    const std::uint64_t filetime = (134774ULL + 20775ULL) * ticks_per_day;
    const std::uint64_t packed = (1ULL << 48) | (5ULL << 32) | (1ULL << 16);
    std::array<unsigned char, 48> bytes{};
    std::memcpy(bytes.data() + 24, &filetime, sizeof(filetime));
    std::memcpy(bytes.data() + 32, &packed, sizeof(packed));
    const auto version =
        driver_package_from_store_value(bytes.data(), bytes.size());
    REQUIRE(version.has_value());
    REQUIRE_EQ(describe_driver_package(*version),
               std::string("1.5.1.0 (2026-11-18)"));
    REQUIRE(*version == ver("11/18/2026,1.5.1.0"));
    REQUIRE(!driver_package_from_store_value(bytes.data(), 39).has_value());
    REQUIRE(!driver_package_from_store_value(nullptr, 48).has_value());
}

TEST_CASE("driver package: the setup details say which version the store holds")
{
    driver_setup_observation seen;
    seen.driver_installed = driver_fact::yes;
    seen.service = helper_service_state::running;
    seen.pipe = helper_probe::ok;
    REQUIRE(describe_driver_setup(seen).find("driver store") ==
            std::string::npos);
    seen.installed_package = ver("11/12/2026,1.1.0.0");
    REQUIRE(describe_driver_setup(seen).find(
                "driver store: version 1.1.0.0 (2026-11-12)") !=
            std::string::npos);
}
