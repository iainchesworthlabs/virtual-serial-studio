//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Licence files and licence folders for the tests of the licence store and of
// `vss_cli licence`. Every licence is signed with the published RFC 8032 test
// key, which the program itself never trusts.
//

#pragma once

#include <vss/license/license.hpp>
#include <vss/license/sign.hpp>
#include <vss/license/store.hpp>
#include <vss/license/test_keys.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss_test_support::licence_files
{

namespace fs = std::filesystem;
namespace lic = vss::license;

inline lic::time_point at(std::string_view text)
{
    return *lic::parse_time(text);
}

/// A Pro licence for one year from 13 November 2026, with three seats.
inline lic::licence annual(std::string id = "VSS-2026-000123")
{
    lic::licence value;
    value.licence_id = std::move(id);
    value.kind = lic::licence_kind::annual;
    value.edition = lic::edition::pro;
    value.licensee_name = "Example Lab Pty Ltd";
    value.issued_at = at("2026-11-13T02:00:00Z");
    value.starts_at = at("2026-11-13T00:00:00Z");
    value.expires_at = at("2027-11-13T00:00:00Z");
    value.grace_days = 28;
    value.seats = 3;
    value.features = {"plugins.official", "driver.signed", "support"};
    return value;
}

inline std::string signed_with_test_key(std::string_view purpose,
                                        const std::string& payload)
{
    const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    return lic::signed_file::write(lic::signing::sign(
        purpose, lic::test_keys::key_id, bytes, lic::test_keys::private_key()));
}

inline std::string licence_file(const lic::licence& value)
{
    return signed_with_test_key(lic::licence_purpose,
                                *lic::write_payload(value));
}

/// More bytes than a signed file may have (64 KiB), so too many to be a
/// licence or a revocation list.
inline std::string too_large_file()
{
    return std::string(std::size_t{70} * 1024, 'x');
}

/// Writes `bytes` to `folder/name`, making the folder first.
inline fs::path write_file(const fs::path& folder, const std::string& name,
                           const std::string& bytes)
{
    fs::create_directories(folder);
    const fs::path path = folder / name;
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    REQUIRE(!file.fail());
    return path;
}

inline std::string read_file(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

/// How many entries `folder` has, of any kind.
inline std::size_t entries_in(const fs::path& folder)
{
    return static_cast<std::size_t>(std::distance(
        fs::directory_iterator(folder), fs::directory_iterator{}));
}

/// Folders of a test and a clock the test moves. The computer is called
/// LAB-RIG-07, and the test key is the only one trusted.
struct fixture
{
    scratch_directory root;
    lic::time_point now = at("2027-06-01T00:00:00Z");

    fs::path user() const { return root.path() / "user" / "licences"; }
    fs::path machine() const { return root.path() / "machine" / "licences"; }
    fs::path inbox() const { return root.path() / "inbox"; }
    fs::path clock_file() const { return root.path() / "user" / "clock.json"; }

    lic::store_options options()
    {
        lic::store_options value;
        value.user_directory = user();
        value.machine_directory = machine();
        value.clock_file = clock_file();
        const auto keys = lic::test_keys::trusted();
        value.keys.assign(keys.begin(), keys.end());
        value.computer_name = "LAB-RIG-07";
        value.clock = [this] { return now; };
        return value;
    }
};

} // namespace vss_test_support::licence_files
