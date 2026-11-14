//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The root public keys this program is built with.
//
// There are none yet.  The owner makes the root key offline and places its
// public half here (docs/plugins.md, section 8.9): the current root and the
// next one, so that a release can replace the root itself.  Only public
// keys ever go in this file.  While it is empty no official key list is
// accepted, so no plugin counts as official.
//

#include <vss/plugin/signature.hpp>

namespace vss::plugin
{

std::span<const root_key> builtin_root_keys() noexcept
{
    return {};
}

} // namespace vss::plugin
