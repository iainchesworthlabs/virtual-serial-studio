//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether the licence on this computer covers an official plugin.
// docs/plugins.md, section 8.7, and docs/licensing.md are the design.
//
// The licence library judges the licence; this asks it one question for one
// plugin and turns the answer into a plugin reason.  It never looks inside a
// licence file, and the licence code never sees a plugin's files.
//
// Only an official plugin that says it needs an edition is asked about.  A
// plugin from anyone else is never licensed by Virtual Serial Studio, so
// there is nothing to ask, and an official plugin that needs no edition is
// part of the Community edition.
//
// This check tells the user the truth about why a plugin does not run.  It
// is not the enforcement: Studio is free software and can be changed, so an
// official plugin checks the licence itself as well.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/license/license.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/verify.hpp>

#include <string>
#include <string_view>

namespace vss::plugin
{

/// The feature a licence names to cover every official plugin.
inline constexpr std::string_view all_official_plugins_feature =
    "plugins.official";

/// The feature a licence names to cover one plugin: `plugins.` and the
/// plugin's id, such as `plugins.com.virtualserialstudio.protocol`.
std::string plugin_feature(std::string_view plugin_id);

/// Whether a plugin of this level with this manifest is asked about at all:
/// it is official and its manifest names an edition (`requires.edition`).
bool needs_licence(trust_level level, const manifest& declared) noexcept;

/// What the licence said about one plugin.
struct licence_answer
{
    /// Whether the licence was asked.  When it was not, the plugin needs
    /// none and the other fields are as they are for "granted".
    bool asked = false;
    bool granted = true;
    /// Why not, in the licence library's own terms, so that the screens can
    /// show its sentence unchanged.  `ok` when granted or not asked.
    license::reason reason = license::reason::ok;
    /// The licence that was judged.  Empty when there is none that can be
    /// read, or when the licence was not asked.
    std::string licence_id;
    /// Days the licence still runs, as the licence library counts them.
    int days_left = 0;

    friend bool operator==(const licence_answer&,
                           const licence_answer&) = default;
};

/// Asks whether the licence covers the plugin.
///
/// A plugin that `needs_licence` says is not asked about is granted without
/// a question.  Otherwise the licence is asked for the plugin's own feature
/// (`plugin_feature`) first and, when that is not granted, for
/// `all_official_plugins_feature`; the plugin is granted when either is.
/// When neither is, the answer carries the licence library's reason: why
/// the licence does not work now, or `not_in_licence` for a licence that is
/// fine and names neither feature.
///
/// Returns success when granted and `plugin_errc::not_licensed` when not.
/// `answer`, when given, receives the details in both cases.
result<void> check_licence(trust_level level, const manifest& declared,
                           const license::status& current,
                           licence_answer* answer = nullptr);

} // namespace vss::plugin
