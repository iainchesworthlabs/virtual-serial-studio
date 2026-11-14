//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/license/test_keys.hpp>

#include <array>
#include <string>

namespace vss::license::test_keys
{

namespace
{

// THROWAWAY TEST KEY: RFC 8032, section 7.1, test 1. Published in the RFC.
constexpr signing::seed rfc8032_test1_seed{
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
    0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
    0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};

constexpr signed_file::public_key rfc8032_test1_public{
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
    0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
    0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};

signed_file::trusted_key make(std::string_view purpose)
{
    signed_file::trusted_key key;
    key.key_id = std::string(key_id);
    key.purpose = std::string(purpose);
    key.key = rfc8032_test1_public;
    return key;
}

} // namespace

signing::seed private_key()
{
    return rfc8032_test1_seed;
}

std::span<const signed_file::trusted_key> trusted()
{
    static const std::array<signed_file::trusted_key, 2> keys{
        make("licence"), make("revocations")};
    return keys;
}

} // namespace vss::license::test_keys
