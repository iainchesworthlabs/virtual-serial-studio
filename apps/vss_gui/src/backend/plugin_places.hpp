//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// The places a plugin may record, as the setup on the screen has them: a port
// by its name, a link by the names of its ends ("COM3 - COM4"). Built on the
// GUI thread from the backend's own lists and handed to the plugin service as
// an immutable table, so the service never reads a GUI object.
//

#pragma once

#include "backend/service_plugin_backend.hpp"
#include "backend/studio_backend.hpp"

#include <vss/core/hub.hpp>

#include <memory>

namespace vss::gui
{

std::shared_ptr<const plugin_place_table>
make_plugin_place_table(const StudioBackend& backend);

/// The ports of the setup, as the endpoints of `source`, by name: what a
/// plugin that replays data writes to.
std::shared_ptr<const plugin_endpoint_table>
make_plugin_endpoint_table(const StudioBackend& backend,
                           const vss::hub& source);

} // namespace vss::gui
