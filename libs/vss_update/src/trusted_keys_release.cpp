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

#include <vss/update/feed.hpp>

namespace vss::update
{

std::span<const vss::license::signed_file::trusted_key> builtin_feed_keys()
{
    return {};
}

} // namespace vss::update
