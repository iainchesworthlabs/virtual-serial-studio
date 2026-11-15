//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a snapshot of the plugin service becomes what the Plugins page shows:
// each plugin's run, its problem code, its decision and its sandbox are put
// through one function that gives it a state group, a reason and the numbers
// the sentences need. A pure function of its input, so every group can be
// made from a snapshot built by hand. It reads no file and starts nothing.
//

#pragma once

#include "backend/plugin_view.hpp"

#include <vss/license/license.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/host/service.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace vss::gui
{

/// What the mapping needs besides the snapshot itself. The service does not
/// carry these; the adapter reads them off the GUI thread.
struct service_mapping_inputs
{
    /// The manifest of each plugin, by `manifest_key`. A plugin whose
    /// manifest could not be read has none.
    std::map<std::string, vss::plugin::manifest> manifests;
    /// The user's decisions and approved keys.
    vss::plugin::trust_store trust;
    /// The administrator's policy, or nothing.
    std::optional<vss::plugin::policy> rules;
    vss::plugin::install_kind install = vss::plugin::install_kind::for_one_user;
    vss::license::status licence = vss::license::no_licence();
    /// What this computer can apply to a plugin that is not running.
    vss::plugin::host::sandbox_report available;
    std::string studio_version;
    QString user_folder;
    QString machine_folder;
};

/// The key of a plugin's manifest in `service_mapping_inputs::manifests`: its
/// folder and its hash.
std::string manifest_key(const vss::plugin::host::service_plugin& plugin);

/// The page's view of the service. Plugin text is kept as it was given; the
/// page cleans it before it shows any of it.
plugin_snapshot
map_service_snapshot(const vss::plugin::host::service_snapshot& snapshot,
                     const service_mapping_inputs& inputs,
                     std::chrono::steady_clock::time_point now =
                         std::chrono::steady_clock::now());

/// Fills the identity and what a manifest says of what the plugin adds, how it
/// is timed, what it asks for and its expiry.
void fill_view_from_manifest(plugin_view& view,
                             const vss::plugin::manifest& declared);

/// "no network isolation, ..." for the parts that were not applied.
QString sandbox_missing_text(const vss::plugin::host::sandbox_report& report);

} // namespace vss::gui
