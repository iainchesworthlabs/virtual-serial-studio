//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Looking at a plugin folder before it runs, putting a plugin on a computer
// and taking it off again.  docs/plugins.md, sections 5.2, 5.3 and 8, are the
// design.
//
// Nothing here runs a plugin, prints, or reads the clock or the computer's
// settings on its own.  `inspect_plugin` is given everything it needs in an
// `inspect_context`; `context_for_this_computer` is the one function that
// fills a context from the computer.  `install_plugin` and `remove_plugin`
// change files, and only inside the folder they are given.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/license/license.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/licence.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/revocations.hpp>
#include <vss/plugin/signature.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin
{

/// What the caller knows about the computer and the day.  A test fills it
/// from memory.
struct inspect_context
{
    /// The later of the system's date and the latest date ever seen.
    std::chrono::sys_days today;
    /// The plugin API this host speaks.
    api_version host_api{1, 0};
    /// The engine capabilities of this build, by name.
    std::vector<std::string> capabilities;
    /// How far this computer can keep a plugin apart.  No sandbox is built
    /// yet, so a real computer says `none`.
    sandbox_level sandbox = sandbox_level::none;
    install_kind install = install_kind::for_one_user;

    /// The administrator's policy, or nothing when there is no policy file.
    /// A file that cannot be used is `official_only_policy()`.
    std::optional<policy> rules;
    /// The official key list, or nothing when none was accepted.
    std::optional<key_list> official;
    /// The keys of the policy's `allow_keys`.
    std::vector<approved_signer> administrator_keys;
    /// The keys the user approved (`trust_store::keys`).
    std::vector<approved_signer> user_keys;
    std::optional<revocation_list> revocations;
    /// The user's decisions.  Only `run_without_sandbox` is read from it.
    std::optional<trust_store> trust;

    /// The licence on this computer.  Used only when `licence_known`.
    license::status licence = license::no_licence();
    /// Whether `licence` was read.  When it was not, the answer to "does
    /// the licence cover this plugin" is "not checked".
    bool licence_known = false;
};

/// Everything known about one plugin folder before it runs.
struct inspection
{
    std::filesystem::path directory;

    /// Why the folder cannot be used, or success.  The first of: the
    /// reading of `plugin.json` (`layout_invalid`, `manifest_too_large`,
    /// `manifest_unsupported_version`, `manifest_invalid`); what
    /// `measure_directory` refused; what `check_files` refused
    /// (`file_missing`, `unlisted_file`, `signature_invalid` for a file that
    /// is not what the manifest lists, `layout_invalid`); a `plugin.sig`
    /// that is a link or too large.
    error_code problem;
    std::string detail;

    std::optional<manifest> declared;
    std::string manifest_bytes;
    std::vector<file_entry> files;
    /// Set when `files` is not empty.
    plugin_hash hash;

    /// The signature check, made whenever the manifest was read.  Its state
    /// is `absent` for a folder with no `plugin.sig`.
    signature_finding signature;
    std::string signature_detail;

    /// The trust level `verify` gave.  Nothing when `problem` is set or the
    /// signature is `invalid`.
    std::optional<trust_level> level;
    std::string key_id;

    /// Whether the plugin may run, as `verify` decides: success, or the
    /// reason code.  Set only when `level` is.  The plugin does not run
    /// whatever this says: nothing in this build starts a plugin.
    error_code run;
    std::string run_detail;
    std::optional<verify_result> verified;

    /// Whether the licence was asked: the manifest names an edition, the
    /// plugin is official, and the context has the licence.
    bool licence_checked = false;
    licence_answer licence;
    error_code licence_problem;
};

/// Looks at a plugin folder and decides nothing about it beyond what the
/// existing checks decide.  It reads the folder and writes nothing.
///
/// The steps are the ones of `discover_plugins` and `verify`: `plugin.json`
/// is read and parsed, the folder is measured and hashed, the files are
/// checked against the manifest, `plugin.sig` is read and checked
/// (`check_signature`), `verify` is run, and `check_licence` when the
/// manifest names an edition.  Nothing escapes as an exception.
///
/// The bytes of `plugin.json` and `plugin.sig` that were parsed and checked
/// are held to the measurement of the folder: when the measured digest of
/// either is not the digest of those bytes, or `plugin.sig` was found by one
/// and not the other, the folder changed while it was being looked at and
/// `problem` is `changed`.
inspection inspect_plugin(const std::filesystem::path& directory,
                          const inspect_context& context);

/// The problems that stop an install: the folder is not a plugin whose
/// files are in order (`inspection::problem`), its signature does not hold,
/// or the plugin could never run on any computer (`reserved_id`, `revoked`).
/// The others (expired, policy, no sandbox, licence) are reported and do
/// not stop an install: they depend on the computer and change.
bool blocks_install(const inspection& found) noexcept;

/// Why a plugin folder would not be installed, or success: what
/// `blocks_install` says; `layout_invalid` for an id that cannot be the name
/// of a folder; `signature_missing` for a plugin with no signature and
/// `key_unknown` for one signed with a key nobody approved, both unless
/// `allow_unsigned` (an unknown key proves nothing about who made the
/// plugin, so it counts as no signature).  `detail`, when given, receives
/// one line for a person.
error_code install_refusal(const inspection& found, bool allow_unsigned,
                           std::string* detail = nullptr);

/// What `context_for_this_computer` was asked to use.
struct computer_options
{
    /// The folder of the programs, for the kind of install.
    std::filesystem::path program_directory;
    /// The administrator's policy file.  Empty: there is none to read.
    std::filesystem::path policy_file;
    /// The user's `trust.json`.  Empty: nothing is approved.
    std::filesystem::path trust_file;
    /// The day to use.  Nothing: the system's date.
    std::optional<std::chrono::sys_days> today;
};

/// The official key list built into this program (`official-keys.json`,
/// read with the built-in root keys).
result<key_list> builtin_key_list(std::string* detail = nullptr);

/// A context filled from this computer: the official key list built in,
/// the policy file, the trust store, the kind of install and the date.  The
/// engine capabilities, the licence and the revocation list are not known
/// to a command line yet: the first is empty, `licence_known` is false and
/// the third is empty.  `sandbox` is `none`.  Anything that cannot be read
/// leaves the safe value (a policy that cannot be used is the official-only
/// policy) and one line of text in `notes`, when given.
inspect_context context_for_this_computer(const computer_options& options,
                                          std::vector<std::string>* notes =
                                              nullptr);

// -- files ------------------------------------------------------------------

/// The two operations whose failure between them matters.  A test gives its
/// own to make one fail; `default_file_operations` is the real thing.
struct file_operations
{
    std::function<error_code(const std::filesystem::path& from,
                             const std::filesystem::path& to)>
        rename;
    std::function<error_code(const std::filesystem::path& path)> remove_all;
    /// Called, when set, after the source was inspected and before the first
    /// file is copied.  Only a test sets it, to change the source at that
    /// moment.
    std::function<void(const std::filesystem::path& source)> inspected;
};

file_operations default_file_operations();

/// Whether `id` may be used as the name of a folder: a plugin id that the
/// manifest reader accepts, and on Windows not the name of a device (`con`,
/// `nul`, `com1`, and so on, with or without an extension).
bool usable_as_folder_name(std::string_view id) noexcept;

struct install_options
{
    /// Install a plugin that has no `plugin.sig`.
    bool allow_unsigned = false;
    /// Replace a plugin of the same id that is already installed.
    bool replace = false;
    /// The target is the machine folder: it must not be writable by anyone
    /// but an administrator, and the install is refused when it is.  A
    /// machine folder that is not there is made only when the nearest folder
    /// above it that is there passes the same test.
    bool for_everyone = false;
    inspect_context context;
    /// Null: the real file operations.
    const file_operations* operations = nullptr;
};

struct install_report
{
    /// What was found in the source folder.
    inspection found;
    /// The new plugin's folder: `<target_root>/<plugin id>`.
    std::filesystem::path directory;
    bool replaced = false;
    /// One line for a person, or empty.
    std::string warning;
};

/// Copies a plugin folder into `<target_root>/<plugin id>`.
///
/// The source is inspected first and refused when `blocks_install` says so.
/// A plugin with no `plugin.sig` is refused with `signature_missing` unless
/// `allow_unsigned`.  A plugin whose id cannot be a folder name is refused
/// with `layout_invalid`.  Links and anything but plain files and folders
/// are refused (`layout_invalid`).  For the machine folder, `permission_denied`
/// when someone but an administrator can write to it.
///
/// The folder is measured (every file's size and digest); then each file is
/// opened again and copied, with its digest taken as it is copied, and a
/// file that is not what the measurement found is refused with `changed`.
/// The bytes of `plugin.json` and `plugin.sig` that were judged are held to
/// the measurement as well (see `inspect_plugin`), so the files copied are
/// the files judged.  Each copy keeps the owner's read and execute bits of
/// its source and the read and execute bits for others, always has owner
/// read and write, never has group or other write, and the program the
/// manifest names to run is executable for its owner.
///
/// The copy goes to a work folder, `<target_root>/.vss-work/<unique>/`, that
/// discovery never reads (it skips every name that starts with a dot), and
/// the folder it makes is measured again before it is renamed to the
/// plugin's id.  Work folders left by an install that was cut off are
/// removed when the next install starts.  No work folder is left behind on
/// a failure, except the one that a failed removal names.
///
/// A plugin with this id that is already there is refused with
/// `errc::already_exists` unless `replace` is set; then the old folder is
/// moved into the work folder, the new one renamed in, and the work folder
/// removed.  When the second rename fails the old one is put back.  When
/// the work folder cannot be removed the new plugin is in place and the
/// result is an `errc::io_error` whose detail names what is left over.
///
/// `target_root` is made when it is not there.  `detail`, when given,
/// receives one line for a person.
result<install_report> install_plugin(const std::filesystem::path& source,
                                      const std::filesystem::path& target_root,
                                      const install_options& options,
                                      std::string* detail = nullptr);

struct remove_options
{
    /// The root is the machine folder: the removal is refused with
    /// `errc::permission_denied` unless only administrators can write to the
    /// folder, or to the nearest folder above it that is there.
    bool for_everyone = false;
    /// Null: the real file operations.
    const file_operations* operations = nullptr;
};

/// Removes `<target_root>/<plugin_id>`: the plugin's folder, never its data
/// directory.  `errc::invalid_argument` for an id that is not a plugin id
/// or cannot be a folder name; `errc::not_found` when there is no such
/// folder; `layout_invalid` when it is a link, a junction or has no
/// `plugin.json`.  Nothing is removed then.
result<void> remove_plugin(const std::filesystem::path& target_root,
                           std::string_view plugin_id,
                           const remove_options& options = {},
                           std::string* detail = nullptr);

/// Where the user's decisions about plugin authors are kept: `trust.json` in
/// `machine::user_folder()`.  Studio and every command read the same file.
/// Empty when the user's folder cannot be worked out.
std::filesystem::path trust_file_of_this_user();

} // namespace vss::plugin
