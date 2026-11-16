//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence for a plugin that checks it itself: the messages
// `licence.get`, `licence.file` and `licence.report`
// (vss/plugin/api_licence.hpp).  docs/plugins.md, section 8.7, and
// docs/licensing.md are the design.
//
// Studio is free software and can be changed, so an official plugin never
// takes the host's word for the licence.  The host hands it the bytes of
// the licence file and nothing else: it does not judge them for the plugin,
// and the plugin treats them as untrusted until it has verified them.  The
// plugin then says what it found by the name of a reason, and Studio shows
// its own sentence for that reason.
//
// - An official plugin is always answered.  A plugin from anyone else is
//   answered only when it was granted `licence.read`, because the file
//   holds the user's name; without it the answer is `not_granted`.
// - The file is the one Studio itself uses.  The plugin cannot ask for
//   another.  With no licence on the computer, or a file that is larger
//   than a licence may be, the answer says there is none.
// - A report changes nothing the host does.  It is kept for the screen.
// - The program that offers this names the engine capability
//   `licence_file` (`licence_file_capability`) in the capabilities it
//   gives the service, so that a plugin can tell.
//
// The host does not tell a plugin that the licence changed.  A plugin asks
// again each time it checks, and the user can start it again.
//

#pragma once

#include <vss/core/bytes.hpp>
#include <vss/plugin/host/extension.hpp>

#include <functional>
#include <memory>
#include <optional>

namespace vss::plugin::host
{

/// The bytes of the licence file Studio uses, exactly as on disk, or
/// nothing when there is none.  Called on the service's strand, so it must
/// return at once: it hands out bytes that were read before.
using licence_file_source = std::function<std::optional<byte_buffer>()>;

/// The extension point for one plugin.  With no `source` every plugin is
/// told there is no licence.
std::unique_ptr<extension_handler>
make_licence_point(const extension_context& context,
                   licence_file_source source);

} // namespace vss::plugin::host
