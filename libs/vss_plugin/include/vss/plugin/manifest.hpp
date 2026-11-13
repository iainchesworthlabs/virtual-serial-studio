//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The manifest of a plugin, `plugin.json`: what the plugin is, what it adds,
// what it asks for and which files it is made of.  docs/plugins.md, section
// 13, is the design.
//
// A manifest is written by someone else, so reading one is reading untrusted
// input.  The reader does no I/O and runs no plugin code; it either returns a
// manifest whose every value is inside its limits, or an error and a line
// that says where the text went wrong.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vss::plugin
{

// -- limits -----------------------------------------------------------------

/// The manifest format this version reads.
inline constexpr std::uint32_t manifest_format_version = 1;

/// `plugin.json` may be this many bytes and no more.
inline constexpr std::size_t max_manifest_size = 256 * 1024;
/// How deep the JSON of a manifest may nest.
inline constexpr std::size_t max_manifest_depth = 8;

inline constexpr std::size_t max_id_length = 128;
inline constexpr std::size_t max_name_length = 100;
inline constexpr std::size_t max_description_length = 1000;
inline constexpr std::size_t max_reason_length = 200;
inline constexpr std::size_t max_help_length = 500;
inline constexpr std::size_t max_url_length = 256;
inline constexpr std::size_t max_path_length = 240;
inline constexpr std::size_t max_argument_length = 256;
inline constexpr std::size_t max_scope_length = 256;
inline constexpr std::size_t max_text_parameter_length = 4096;

inline constexpr std::size_t max_languages = 32;
inline constexpr std::size_t max_capabilities = 32;
inline constexpr std::size_t max_runtimes = 8;
inline constexpr std::size_t max_arguments = 32;
inline constexpr std::size_t max_permissions = 32;
inline constexpr std::size_t max_scopes = 16;
inline constexpr std::size_t max_extension_points = 32;
inline constexpr std::size_t max_bundle_sections = 8;
inline constexpr std::size_t max_parameters = 64;
inline constexpr std::size_t max_choices = 64;
inline constexpr std::size_t max_files = 4096;

/// How many instances an extension point gets when it does not say.
inline constexpr std::uint32_t default_instances = 16;
inline constexpr std::uint32_t max_instances = 256;

/// The shortest and longest time the engine may be asked to wait for an
/// inline plugin, in microseconds.
inline constexpr std::uint32_t min_budget_us = 50;
inline constexpr std::uint32_t max_budget_us = 1'000'000;

// -- parts ------------------------------------------------------------------

/// A text in one or more languages.  A manifest gives it as a plain string
/// (stored with an empty language) or as an object of language tag to text.
/// It is plain text: never markup and never a format string.
struct localized_text
{
    std::vector<std::pair<std::string, std::string>> entries;

    bool empty() const noexcept { return entries.empty(); }

    /// The text for `language` ("de", "en-AU"), else for its first part
    /// ("en"), else English, else the plain string, else the first entry.
    /// Empty when there is none.
    std::string_view best(std::string_view language) const noexcept;

    friend bool operator==(const localized_text&,
                           const localized_text&) = default;
};

/// `major.minor.patch`, with an optional pre-release after a hyphen.
struct plugin_version
{
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    std::string pre_release;

    friend bool operator==(const plugin_version&,
                           const plugin_version&) = default;
};

std::string to_string(const plugin_version& value);

/// Which of two versions is the earlier, by the rules of Semantic
/// Versioning: the numbers first; then a version with a pre-release comes
/// before the same version without one; and two pre-releases are compared
/// part by part, a number before a word and fewer parts before more.
std::strong_ordering compare(const plugin_version& left,
                             const plugin_version& right) noexcept;

/// A version of the plugin API.
struct api_version
{
    std::uint16_t major = 0;
    std::uint16_t minor = 0;

    friend auto operator<=>(const api_version&, const api_version&) = default;
};

/// The versions of the plugin API a plugin works with: from `min` up to
/// `max_major.max_minor`, or up to any minor of `max_major` when `max_minor`
/// is unset (written "1.x").
struct api_range
{
    api_version min;
    std::uint16_t max_major = 0;
    std::optional<std::uint16_t> max_minor;

    bool contains(api_version version) const noexcept;

    friend bool operator==(const api_range&, const api_range&) = default;
};

struct plugin_author
{
    std::string name;
    std::string url;

    friend bool operator==(const plugin_author&,
                           const plugin_author&) = default;
};

/// What the plugin needs from the host before it can run.
struct plugin_needs
{
    api_range api;
    /// Engine capability flags by name, such as `engine_rules`.
    std::vector<std::string> capabilities;
    /// "pro" or "enterprise"; empty when no edition is needed.
    std::string edition;
    /// Programs of the computer the plugin runs with, such as `python3`.
    /// Their files are not covered by the plugin's signature.
    std::vector<std::string> runtime;

    friend bool operator==(const plugin_needs&, const plugin_needs&) = default;
};

/// How the plugin is started: one program with fixed arguments, never a
/// shell line.
struct run_spec
{
    /// A path inside the plugin, by the rules of `check_relative_path`.
    std::string executable;
    std::vector<std::string> args;
    /// Where it runs: `windows-x64`, `linux-arm64` and so on.  Never empty.
    std::vector<std::string> platforms;

    friend bool operator==(const run_spec&, const run_spec&) = default;
};

enum class permission
{
    ports_read,
    traffic_tap,
    ports_write,
    topology_modify,
    modifier_inline,
    faults,
    serial_devices,
    fs_read,
    fs_write,
    data_private,
    net_listen,
    net_outbound,
    process_spawn,
    bundle_section
};

/// The name a manifest uses, such as `traffic.tap`.
std::string_view to_string(permission value) noexcept;
std::optional<permission> parse_permission(std::string_view text) noexcept;

struct permission_request
{
    permission name = permission::ports_read;
    /// A plugin that is refused a required permission cannot start.
    bool required = true;
    /// The plugin's own words on why it asks.  Shown as a quotation.
    std::string reason;
    /// For `net.listen` the addresses it binds, for `net.outbound` the hosts
    /// it connects to.  Empty for every other permission.
    std::vector<std::string> scope;

    friend bool operator==(const permission_request&,
                           const permission_request&) = default;
};

enum class parameter_type
{
    boolean,
    integer,
    number,
    choice,
    string,
    bytes,
    duration,
    port,
    link,
    path,
    secret
};

std::string_view to_string(parameter_type value) noexcept;

/// The value of a parameter.  A boolean is `bool`; an integer `int64_t`; a
/// number `double`; a duration `int64_t` nanoseconds; a choice its value; a
/// string its text; bytes are the bytes themselves in a string.  A port, a
/// link, a path and a secret have no value in a manifest.
using parameter_value =
    std::variant<std::monostate, bool, std::int64_t, double, std::string>;

struct parameter_choice
{
    std::string value;
    localized_text label;

    friend bool operator==(const parameter_choice&,
                           const parameter_choice&) = default;
};

enum class path_kind { file, directory };
enum class path_access { read, write };

/// One setting of a plugin.  Studio draws the control from this; the plugin
/// never draws anything.
struct parameter
{
    std::string id;
    parameter_type type = parameter_type::boolean;
    localized_text label;
    localized_text help;
    /// A unit shown beside the control, such as "ms".  Integer and number.
    std::string unit;
    parameter_value default_value;
    /// Integer, number (as that type) and duration (nanoseconds).
    parameter_value min;
    parameter_value max;
    /// Integer and number.
    parameter_value step;
    /// Choice.
    std::vector<parameter_choice> choices;
    /// String and secret in characters' bytes, bytes in bytes.
    std::uint32_t max_length = 0;
    /// Path.
    path_kind kind = path_kind::file;
    path_access access = path_access::read;
    bool must_exist = false;

    friend bool operator==(const parameter&, const parameter&) = default;
};

enum class extension_kind { generator, fixture, modifier, adapter };

std::string_view to_string(extension_kind value) noexcept;

/// How fast the answers of an instance must be, and so where they are made.
/// docs/plugins.md, section 6.
enum class timing_class
{
    /// The engine carries out rules the plugin hands it.
    rules,
    /// The engine waits for the plugin, up to a budget.
    in_line,
    /// The plugin is never waited for.
    async
};

std::string_view to_string(timing_class value) noexcept;

enum class timeout_action { fail_open, fail_closed };

struct timing_spec
{
    timing_class type = timing_class::async;
    /// In-line only: the most the engine waits, in microseconds.
    std::uint32_t budget_us = 0;
    /// In-line only: what happens to the bytes when the budget passes.
    timeout_action on_timeout = timeout_action::fail_open;

    friend bool operator==(const timing_spec&, const timing_spec&) = default;
};

struct extension_point
{
    extension_kind kind = extension_kind::generator;
    std::string id;
    localized_text name;
    localized_text description;
    timing_spec timing;
    std::uint32_t max_instances = default_instances;
    std::vector<parameter> parameters;

    friend bool operator==(const extension_point&,
                           const extension_point&) = default;
};

/// A section the plugin can add to a support bundle.
struct bundle_section
{
    std::string id;
    localized_text title;
    /// A sensitive section is off until the user turns it on, every time.
    bool sensitive = true;
    std::vector<parameter> parameters;

    friend bool operator==(const bundle_section&,
                           const bundle_section&) = default;
};

using sha512_digest = std::array<std::uint8_t, 64>;

/// One file of the plugin, as the manifest of a signed plugin lists it.
struct file_entry
{
    std::string path;
    std::uint64_t size = 0;
    sha512_digest sha512{};

    friend bool operator==(const file_entry&, const file_entry&) = default;
};

/// A manifest that was read and found to be inside every limit.
struct manifest
{
    std::string id;
    std::string name;
    plugin_version version;
    plugin_author author;
    std::string description;
    /// An SPDX expression or a name; shown, never interpreted.
    std::string license;
    std::optional<std::chrono::year_month_day> not_after;
    std::optional<std::chrono::year_month_day> released;
    plugin_needs needs;
    run_spec run;
    std::vector<permission_request> permissions;
    std::vector<extension_point> extension_points;
    std::vector<bundle_section> bundle_sections;
    /// Empty for a plugin that does not list its files; such a plugin cannot
    /// be signed.
    std::vector<file_entry> files;

    /// The request for `name`, or null when the manifest does not ask for it.
    const permission_request* find(permission wanted) const noexcept;

    friend bool operator==(const manifest&, const manifest&) = default;
};

// -- reading ----------------------------------------------------------------

/// Reads the bytes of a `plugin.json`.
///
/// `plugin_errc::manifest_too_large` for more than `max_manifest_size` bytes,
/// `plugin_errc::manifest_unsupported_version` for a `manifest_version` above
/// `manifest_format_version`, `plugin_errc::layout_invalid` for a path that
/// breaks the rules of `check_relative_path`, and
/// `plugin_errc::manifest_invalid` for everything else: text that is not
/// JSON, a key the format does not have, a key given twice, a wrong type, a
/// value outside its limits, or a text with a control character or a
/// direction override in it.
///
/// `detail`, when given, receives one line for a person: where in the
/// document and what was wrong, such as
/// `extension_points[0].timing.class: unknown value "fast"`.
result<manifest> parse_manifest(std::string_view text,
                                std::string* detail = nullptr);

/// Whether `path` may name a file inside a plugin.  Returns what is wrong
/// with it, or nothing.
///
/// A path is one or more names joined by `/`.  It is refused when it is
/// empty or longer than `max_path_length`; has a character outside printable
/// ASCII, or one of `\ : * ? " < > |`; starts with `/`; has an empty name,
/// `.` or `..`; has a name that ends in a dot or a space; or has a name that
/// Windows reserves for a device (`CON`, `NUL`, `COM1`, `LPT1`, ...), with or
/// without an extension.  So the same path means the same file on every
/// system, and never one outside the plugin.
std::optional<std::string> check_relative_path(std::string_view path);

/// `path` in the form two paths are compared in: ASCII letters in lower
/// case.  Two files whose folded paths are equal clash on Windows and macOS.
std::string fold_path(std::string_view path);

} // namespace vss::plugin
