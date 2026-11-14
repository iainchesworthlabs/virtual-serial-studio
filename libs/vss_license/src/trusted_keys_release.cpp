//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The public keys this program trusts. Public halves only: a private key is
// never in this repository.
//
// The owner made these keys on their own computer (docs/licensing.md, "Where
// the private keys live"). `lic-2026-a` signs. `lic-2026-b` is the standby:
// its private half has never been on an online system, and it is trusted
// here already so that it can take over at once if the first key leaks.
//
// A key verifies one purpose, so each key is listed for licences and for
// revocation lists. Neither has a window or a revocation yet; a later release
// sets `not_after` when a key is rotated out and `revoked_from` if one leaks
// (docs/licensing.md, "Rotation").
//

#include <vss/license/license.hpp>
#include <vss/license/signed_file.hpp>

#include <array>
#include <string>
#include <string_view>

namespace vss::license::signed_file
{

namespace
{

// base64url 7yaM1JZlHf0zkoch3l1F_HBlo2Ha52GsaWVoqTITQDU
constexpr public_key lic_2026_a{0xef, 0x26, 0x8c, 0xd4, 0x96, 0x65, 0x1d, 0xfd,
                                0x33, 0x92, 0x87, 0x21, 0xde, 0x5d, 0x45, 0xfc,
                                0x70, 0x65, 0xa3, 0x61, 0xda, 0xe7, 0x61, 0xac,
                                0x69, 0x65, 0x68, 0xa9, 0x32, 0x13, 0x40, 0x35};

// base64url JZxSM-Dqrdsz-SSSltoDVefb3Ky79bEAzVXL_aBHhd0
constexpr public_key lic_2026_b{0x25, 0x9c, 0x52, 0x33, 0xe0, 0xea, 0xad, 0xdb,
                                0x33, 0xf9, 0x24, 0x92, 0x96, 0xda, 0x03, 0x55,
                                0xe7, 0xdb, 0xdc, 0xac, 0xbb, 0xf5, 0xb1, 0x00,
                                0xcd, 0x55, 0xcb, 0xfd, 0xa0, 0x47, 0x85, 0xdd};

trusted_key make(std::string_view key_id, std::string_view purpose,
                 const public_key& key)
{
    trusted_key entry;
    entry.key_id = std::string(key_id);
    entry.purpose = std::string(purpose);
    entry.key = key;
    return entry;
}

} // namespace

std::span<const trusted_key> builtin_keys()
{
    static const std::array<trusted_key, 4> keys{
        make("lic-2026-a", licence_purpose, lic_2026_a),
        make("lic-2026-a", revocations_purpose, lic_2026_a),
        make("lic-2026-b", licence_purpose, lic_2026_b),
        make("lic-2026-b", revocations_purpose, lic_2026_b)};
    return keys;
}

} // namespace vss::license::signed_file
