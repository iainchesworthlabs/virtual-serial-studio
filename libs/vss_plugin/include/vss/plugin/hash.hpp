//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hash of a plugin, and the check of a plugin's files against the list in
// its manifest.  docs/plugins.md, sections 5.3 and 8.3, are the design.
//
// Nothing here reads a file.  The caller measures the files of a plugin's
// directory (path, size and SHA-512 of each) and hands over the list, so the
// same functions run on files, on open handles and on a test's own bytes.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/manifest.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace vss::plugin
{

/// The two files of a plugin that its manifest never lists.
inline constexpr std::string_view manifest_file_name = "plugin.json";
inline constexpr std::string_view signature_file_name = "plugin.sig";

/// An id that starts with this belongs to official plugins only.
inline constexpr std::string_view reserved_id_prefix =
    "com.virtualserialstudio.";

/// SHA-512 of bytes that arrive in parts.
class sha512_hasher
{
public:
    sha512_hasher();
    ~sha512_hasher();
    sha512_hasher(const sha512_hasher&) = delete;
    sha512_hasher& operator=(const sha512_hasher&) = delete;

    void update(std::span<const std::byte> bytes);
    void update(std::string_view text);

    /// The digest of everything given so far.  Call it once, last.
    sha512_digest finish();

private:
    struct state;
    std::unique_ptr<state> state_;
};

sha512_digest sha512_of(std::span<const std::byte> bytes);
sha512_digest sha512_of(std::string_view text);

/// The digest as 128 lower-case hex characters.
std::string to_hex(const sha512_digest& digest);

/// What a decision about a plugin is tied to.  Any change to any file of the
/// plugin, a rename, an added file or a removed one gives a different hash.
struct plugin_hash
{
    sha512_digest bytes{};

    /// The first 16 hex characters: the fingerprint a person is shown.
    std::string short_text() const;

    friend bool operator==(const plugin_hash&, const plugin_hash&) = default;
};

/// The hash of a plugin from every file in its directory, `plugin.json` and
/// `plugin.sig` included, in any order.
///
/// It is the SHA-512 of one line for each file, the lines sorted by path as
/// bytes: the path, a zero byte, the size in decimal, a zero byte, the file's
/// SHA-512 as 128 lower-case hex characters, and a line feed.
plugin_hash hash_plugin(std::span<const file_entry> files);

/// Checks the files measured in a plugin's directory against its manifest.
///
/// `plugin_errc::layout_invalid` when `plugin.json` is not among them, when
/// there are more than the limit, or when a path breaks the rules of
/// `check_relative_path`, is there twice or clashes with another when case
/// is ignored.  `plugin_errc::file_missing` when the program the manifest
/// runs, or a file it lists, is not there.  For a manifest that lists its
/// files: `plugin_errc::signature_invalid` when a file's size or digest is
/// not the listed one, and `plugin_errc::unlisted_file` when the directory
/// holds a file that is not listed, other than `plugin.json` and
/// `plugin.sig`.
///
/// `detail`, when given, receives one line that names the file.
result<void> check_files(const manifest& declared,
                         std::span<const file_entry> measured,
                         std::string* detail = nullptr);

/// Whether only an official plugin may use `id`.
bool is_reserved_id(std::string_view id) noexcept;

} // namespace vss::plugin
