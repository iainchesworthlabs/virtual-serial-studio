//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The version of a Windows driver package, and how two of them compare.
//
// A package carries `DriverVer=<month>/<day>/<year>,<a>.<b>.<c>.<d>` in its
// .inf file. Windows decides which of two packages of one driver is newer by
// the date first and the four-part version second, so this does the same.
// drivers/windows/driver-version.json is where the number comes from, and
// tools/ci/check_driver_version.py keeps it moving with the driver.
//
// Everything here is plain code over text, so it is tested on every system.
// `installed_driver_package()` is the one function that asks the host.
//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace vss
{

struct driver_package_version
{
    /// Days since 1970-01-01 of the package date.
    std::int64_t day = 0;
    /// The four parts, most significant first.
    std::array<unsigned, 4> parts{};

    bool operator==(const driver_package_version&) const = default;
};

enum class driver_package_order
{
    older,
    same,
    newer
};

/// The four parts as `1.5.1.0`.
std::string driver_parts_text(const driver_package_version& version);

/// The date as `2026-11-18`.
std::string driver_date_text(const driver_package_version& version);

/// Both as `1.5.1.0 (2026-11-18)`.
std::string describe_driver_package(const driver_package_version& version);

/// Reads `11/18/2026,1.5.1.0`. Nothing when the text is not that.
std::optional<driver_package_version> parse_driver_ver(std::string_view text);

/// Finds the DriverVer line in the text of an .inf file.
std::optional<driver_package_version>
parse_inf_driver_ver(std::string_view inf);

/// The DriverVer of an .inf file on disk. Nothing when it cannot be read.
std::optional<driver_package_version>
read_inf_driver_version(const std::filesystem::path& inf);

/// How `have` compares with `wanted`: the date first, then the parts.
driver_package_order
compare_driver_packages(const driver_package_version& have,
                        const driver_package_version& wanted) noexcept;

/// Turns the bytes Windows keeps for a package
/// (`DriverPackages\<name>\Version`) into a version. Nothing when the value is
/// too short. Pure, for tests.
std::optional<driver_package_version>
driver_package_from_store_value(const unsigned char* bytes, std::size_t size);

/// The newest `vsscom_um.inf` package in the Windows driver store, or nothing
/// when there is none or it cannot be read. Reads the registry only. Nothing
/// on other systems.
std::optional<driver_package_version> installed_driver_package();

} // namespace vss
