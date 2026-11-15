//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The host's check of the sections a plugin offers for a support bundle.
// docs/plugins.md, section 19.5, is the design.
//
// A plugin answers `bundle.prepare` with the files it wrote into a folder of
// its data directory.  What it says is not trusted.  `check_sections` looks at
// the list alone: ids, titles, counts, paths and sizes.  `verify_sections`
// then opens each file, once, and reads its size and its hash from that one
// handle, so what Studio previews and saves are the bytes that were checked.
//
// Everything a plugin names is data: it is returned as it came and is shown
// as plain text, quoted.  Nothing here shows anything.
//

#pragma once

#include <boost/assert/source_location.hpp>
#include <boost/system/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin::host
{

/// Limits of section 19.5 and `bundle.result`.
inline constexpr std::size_t max_section_files = 16;
inline constexpr std::uint64_t max_section_bytes = 268'435'456;
inline constexpr std::size_t max_section_id = 128;
inline constexpr std::size_t max_section_title = 128;
/// The most sections a plugin may offer.  This is this code's choice.
inline constexpr std::size_t max_offered_sections = 16;

/// One file of a section, as the plugin lists it.
struct offered_file
{
    /// Relative to the plugin's data directory, `/` between names.  The
    /// rules of `check_relative_path` (manifest.hpp) apply.
    std::string path;
    std::uint64_t size = 0;
    /// The SHA-512 of its bytes as 128 lower-case hex characters.  The
    /// digest is `libs/vss_plugin`'s (hash.hpp), the one the plugin's own
    /// files are listed with.
    std::string sha512;

    friend bool operator==(const offered_file&, const offered_file&) = default;
};

/// One section, as the plugin lists it.
struct offered_section
{
    /// A token of 1 to 128 bytes: lower-case letters, digits, dots, hyphens
    /// and underscores.
    std::string id;
    /// 1 to 128 bytes, valid UTF-8, no control character and no mark that
    /// changes the direction of text.
    std::string title;
    bool sensitive = false;
    std::vector<offered_file> files;

    friend bool operator==(const offered_section&, const offered_section&) =
        default;
};

/// Which ids a plugin may use.
struct section_rules
{
    /// The plugin's id.  An id that starts with it and a dot is its own.
    std::string plugin_id;
    /// Ids reserved for this plugin by the host, such as `traffic`.
    std::vector<std::string> reserved_ids;
};

enum class bundle_problem
{
    too_many_sections,
    id_bad,
    id_not_allowed,
    id_twice,
    title_bad,
    too_many_files,
    path_bad,
    path_twice,
    size_over_limit,
    section_over_limit,
    hash_bad,
    /// From here on, found by opening the files.
    directory_bad,
    file_missing,
    file_is_link,
    file_not_plain,
    file_unreadable,
    size_changed,
    hash_changed
};

/// The name of a problem, for a log.
std::string_view to_string(bundle_problem problem) noexcept;

/// The first thing wrong, and where.  `section` and `path` are what the
/// plugin wrote, kept as they came, and are never to be put into a sentence
/// of Studio's own; `detail` is Studio's.
struct bundle_failure
{
    bundle_problem problem = bundle_problem::id_bad;
    std::string section;
    std::string path;
    std::string detail;
};

/// What `boost::system::result<T, bundle_failure>::value()` calls on a
/// failure: it throws `std::runtime_error` with the name of the problem.
[[noreturn]] void throw_exception_from_error(const bundle_failure& failure,
                                             const boost::source_location& at);

/// Checks the list alone.  Nothing when it is good.
std::optional<bundle_failure>
check_sections(std::span<const offered_section> sections,
               const section_rules& rules);

/// A file whose size and hash were read from one open handle.
struct verified_file
{
    std::string path;
    std::filesystem::path full_path;
    std::uint64_t size = 0;
    std::string sha512;
};

struct verified_section
{
    std::string id;
    std::string title;
    bool sensitive = false;
    std::uint64_t bytes = 0;
    std::vector<verified_file> files;
};

/// `check_sections`, then each file of each section: no part of its path is
/// a link or anything but a plain directory, it opens, its size from the
/// handle is the listed size, and what is read from the handle is that many
/// bytes with the listed hash.  At most the listed size plus one byte is
/// read from a file.  Returns the verified list, or the first failure.
///
/// `data_directory` is the plugin's data directory and is itself checked: it
/// must exist and must not be a link.
///
/// What this cannot do: a program that may write to the data directory can
/// replace a file after it was read.  The size and hash are of the handle
/// that was opened, not of the name.  The name is looked at again after the
/// read, which finds a link put in its place, and a reader that uses
/// `full_path` later must check what it reads again.
boost::system::result<std::vector<verified_section>, bundle_failure>
verify_sections(const std::filesystem::path& data_directory,
                std::span<const offered_section> sections,
                const section_rules& rules);

} // namespace vss::plugin::host
