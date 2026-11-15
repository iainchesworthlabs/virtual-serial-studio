//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The public keys this program trusts to sign the update feed. Public halves
// only: a private key is never in this repository.
//
// The list is empty. The owner makes `upd-<year>-a` and `upd-<year>-b` on
// their own computer, as the licence keys were made (docs/licensing.md,
// "Where the private keys live"), and adds the two public halves here, each
// with the purpose `update-feed`. `upd-<year>-a` signs. `upd-<year>-b` is the
// standby: its private half is never on an online system, and it is trusted
// here already so that it can take over at once if the first key leaks.
// Until then a build cannot check for updates (`no_update_keys`).
//
// Neither key has a window or a revocation yet; a later release sets
// `not_after` when a key is rotated out and `revoked_from` if one leaks, as
// for the licence keys (docs/updates.md, section 5).
//
// This file is also read as text by `tools/update_feed.py` (`keys` and
// `verify --keys-from`), which the release workflow and the website check use
// to sign and verify the feed with the keys that are listed here. So a filled
// list keeps this form, and the tool refuses a file that it cannot read
// exactly (see `read_feed_keys` in the tool):
//
//   - Each key is an array of exactly 32 bytes, each written as `0x` and two
//     hex digits, separated by commas, with no comma after the last:
//         constexpr public_key upd_2026_a{0x00, 0x01, ..., 0x1f};
//   - Each key is listed once, in the order that signs first, as an entry
//     with its id, `feed_purpose` and the name of its array, inside the one
//     `std::array<trusted_key, N>` that the function returns (N is the number
//     of entries):
//         make("upd-2026-a", feed_purpose, upd_2026_a)
//   - With no key the function is `return {};`, as it is now.
//   - The names `not_before`, `not_after` and `revoked_from` are not allowed
//     in this file until the tool reads them. The tool cannot tell what a key
//     with a window may sign, so it refuses the file.
//

#include <vss/update/feed.hpp>

namespace vss::update
{

std::span<const vss::license::signed_file::trusted_key> builtin_feed_keys()
{
    return {};
}

} // namespace vss::update
