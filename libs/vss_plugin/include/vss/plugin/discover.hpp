//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Finding the plugins that are installed: looking through the plugin
// folders, reading each plugin's manifest and files, and saying which copy
// of a plugin is the one that counts.  docs/plugins.md, section 5.2, is the
// design.
//
// Discovery reads and never runs anything.  It does not look at
// signatures, at who owns a folder, at the policy or at the user's
// decisions: `verify` and the checks around it do that with what discovery
// hands over.  So a plugin that discovery reports without a problem is one
// whose files are in order, and nothing more.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin
{

/// The most plugins that are looked at, over all folders.  The ones after
/// that are reported as `too_many` and not read.
inline constexpr std::size_t max_plugins = 64;

/// A `plugin.sig` may be this many bytes and no more.
inline constexpr std::size_t max_signature_size = std::size_t{64} * 1024;

/// Who a plugin is installed for, from the folder it was found in.  The
/// values are in order: an earlier one is the wider scope and wins when
/// two folders hold a plugin with the same id.
enum class plugin_scope
{
    /// In Studio's own folder: it came with Studio.
    with_studio,
    /// In the machine plugin folder: an administrator installed it for
    /// everyone on this computer.
    for_everyone,
    /// In the user's own plugin folder.
    for_user
};

std::string_view to_string(plugin_scope value) noexcept;

/// One folder that holds plugins, each in a folder of its own.
struct plugin_root
{
    std::filesystem::path directory;
    plugin_scope scope = plugin_scope::for_user;
};

/// One plugin folder that was found.
struct found_plugin
{
    /// The plugin's own folder.
    std::filesystem::path directory;
    plugin_scope scope = plugin_scope::for_user;

    /// Why this plugin cannot be used, or success.  The first of:
    /// `too_many`; `layout_invalid`, `manifest_too_large`,
    /// `manifest_unsupported_version` or `manifest_invalid` from reading
    /// `plugin.json`; what `measure_directory` or `check_files` refused;
    /// `layout_invalid` or `manifest_too_large` for a `plugin.sig` that is
    /// a link or too large; and last `shadowed`.  A system error (a file
    /// that cannot be read) is kept as it is.
    error_code problem;
    /// One line for a person about `problem`, or empty.
    std::string detail;

    /// The manifest, when `plugin.json` could be read as one.  It is set
    /// for a shadowed plugin, and for one whose files are then found
    /// wrong, so that the plugin can be shown by name.
    std::optional<manifest> declared;
    /// The exact bytes of `plugin.json`, for the signature check.  Empty
    /// when the file could not be read.
    std::string manifest_bytes;
    /// The exact bytes of `plugin.sig`, or nothing when the plugin has
    /// none or it could not be read.
    std::optional<std::string> signature_bytes;
    /// Every file of the plugin as measured, and the hash of them.  Both
    /// are set when the directory could be measured, whatever
    /// `check_files` then said.
    std::vector<file_entry> files;
    plugin_hash hash;

    /// For a shadowed plugin: where in `discovery::plugins` the copy that
    /// counts is.
    std::optional<std::size_t> shadowed_by;
};

/// A plugin folder root that could not be looked through.
struct root_problem
{
    std::filesystem::path directory;
    plugin_scope scope = plugin_scope::for_user;
    error_code problem;
    std::string detail;
};

struct discovery
{
    /// In the order of the roots given, and inside a root by the name of
    /// the plugin's folder, as bytes.
    std::vector<found_plugin> plugins;
    std::vector<root_problem> roots;
};

/// Looks through the plugin folders.
///
/// The roots are looked through in the order given; the caller gives them
/// widest scope first.  For each root:
///
/// - A root that is not there is passed over without a word: most
///   computers have no machine plugin folder.
/// - A root that is a link, or is not a directory, or cannot be read, is
///   reported in `discovery::roots` (`plugin_errc::layout_invalid`, or the
///   system's error) and nothing in it is looked at.
/// - Each plain directory directly inside the root that holds a file named
///   `plugin.json` is a plugin, and gives one `found_plugin`.  A directory
///   without that file, and a plain file in the root, are passed over.  An
///   entry of the root that is a link is reported as a `found_plugin` with
///   `plugin_errc::layout_invalid` and is not followed.
///
/// For each plugin, in this order, stopping at the first problem:
/// `plugin.json` is read (`read_plugin_file`, at most `max_manifest_size`
/// bytes) and parsed (`parse_manifest`); the directory is measured
/// (`measure_directory`) and hashed (`hash_plugin`); the files are checked
/// against the manifest (`check_files`); `plugin.sig` is read when there
/// is one (at most `max_signature_size` bytes).
///
/// Then, over all plugins that were read as a manifest: when two have the
/// same id, the one found first counts and each later one gets
/// `plugin_errc::shadowed`, unless it already has a problem, and
/// `shadowed_by` in either case.  A plugin that has a problem of its own
/// still shadows a later one with its id: a broken copy for everyone is
/// never silently replaced by the user's own copy.
///
/// At most `max_plugins` plugins are read.  Every plugin folder after that
/// gives a `found_plugin` with `plugin_errc::too_many`, its directory and
/// its scope, and nothing else.
///
/// Nothing escapes as an exception.
discovery discover_plugins(std::span<const plugin_root> roots);

} // namespace vss::plugin
