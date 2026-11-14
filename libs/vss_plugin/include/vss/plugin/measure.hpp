//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading a plugin's directory from disk: which files are in it, how large
// each is and what its SHA-512 is.  The result is what `hash_plugin` and
// `check_files` (hash.hpp) take.  docs/plugins.md, sections 5.2, 5.3 and
// 8.3, are the design.
//
// A plugin's directory was written by someone else.  Nothing in it is
// trusted here: a link of any kind is refused and never followed, so that a
// plugin cannot name a file outside itself; only plain files and plain
// directories are accepted; and everything has a limit.  No file is opened
// for writing and nothing is run.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/manifest.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vss::plugin
{

/// How deep a file may lie inside a plugin: `a/b/c.bin` is at depth 3.
inline constexpr std::size_t max_plugin_depth = 16;

/// The most files a plugin's directory may hold: as many as a manifest may
/// list, and `plugin.json` and `plugin.sig`, which a manifest never lists.
/// It is the number `check_files` accepts.
inline constexpr std::size_t max_measured_files = max_files + 2;

/// The most bytes all the files of one plugin may have together.
inline constexpr std::uintmax_t max_plugin_bytes =
    std::uintmax_t{1024} * 1024 * 1024;

/// Measures every file in a plugin's directory.
///
/// Returns one `file_entry` for each file, `plugin.json` and `plugin.sig`
/// included, sorted by path as bytes.  A path is relative to `directory`,
/// with `/` between its names, as a manifest writes it.  The size is the
/// number of bytes that were read and hashed, not what the file system
/// said beforehand.  Directories are not entries; an empty directory
/// leaves no trace.
///
/// `errc::not_found` when `directory` is not there.
/// `plugin_errc::layout_invalid` when `directory` is itself a link or is
/// not a directory; when anything inside it is a link (a symbolic link, a
/// junction or any other reparse point) or is neither a plain file nor a
/// plain directory; when a path breaks the rules of `check_relative_path`
/// (a name outside printable ASCII, a reserved name, and so on); when a
/// file or a directory lies deeper than `max_plugin_depth`; when there are
/// more than `max_measured_files` files; or when the files have more than
/// `max_plugin_bytes` together.  `errc::io_error` when the directory or a
/// file cannot be read.
///
/// `detail`, when given, receives one line that names the file.
result<std::vector<file_entry>>
measure_directory(const std::filesystem::path& directory,
                  std::string* detail = nullptr);

/// Reads one small file of a plugin whole: its `plugin.json` or its
/// `plugin.sig`.
///
/// `plugin_errc::file_missing` when the file is not there.
/// `plugin_errc::layout_invalid` when it is a link or not a plain file.
/// `plugin_errc::manifest_too_large` when it has more than `max_size`
/// bytes; the file is then not read to its end.  `errc::io_error` when it
/// cannot be read.
///
/// `detail`, when given, receives one line.
result<std::string> read_plugin_file(const std::filesystem::path& file,
                                     std::size_t max_size,
                                     std::string* detail = nullptr);

} // namespace vss::plugin
