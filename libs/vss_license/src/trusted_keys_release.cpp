//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The public keys this program trusts. Public halves only: a private key is
// never in this repository.
//
// The list is empty until the owner generates the licence keys
// (docs/licensing.md, "Where the private keys live") and adds their public
// halves here. With an empty list every licence is refused and Studio is the
// Community edition.
//

#include <vss/license/signed_file.hpp>

namespace vss::license::signed_file
{

std::span<const trusted_key> builtin_keys()
{
    return {};
}

} // namespace vss::license::signed_file
