//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The revocation list this release carries (docs/licensing.md,
// "Revocation"). It holds licence ids and key ids only, never names.
//
// The list is empty and has sequence 0, so any signed list a customer is
// sent is newer. When a licence or a key is withdrawn, the owner adds its id
// here and raises the sequence number to that of the signed list of the same
// day, so that the release and the file say the same thing.
//

#include <vss/license/license.hpp>

namespace vss::license
{

revocation_list builtin_revocations()
{
    return {};
}

} // namespace vss::license
