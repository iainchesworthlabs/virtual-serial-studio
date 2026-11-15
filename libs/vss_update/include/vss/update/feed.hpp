//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The update feed: the small signed file that says what is current
// (docs/updates.md, section 4). It is a `vss::license::signed_file` envelope
// with the purpose `update-feed`. Nothing here reads a file or opens a
// connection: the caller hands over the text.
//

#pragma once

#include <vss/license/signed_file.hpp>
#include <vss/update/error.hpp>
#include <vss/update/version.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss::update
{

/// The purpose inside the signed envelope. A feed key signs nothing else.
inline constexpr std::string_view feed_purpose = "update-feed";
/// The payload format this version reads (`feed_version`).
inline constexpr std::uint32_t feed_format_version = 1;

inline constexpr std::size_t max_studio_releases = 2;
inline constexpr std::size_t max_files = 16;
inline constexpr std::size_t max_plugins = 64;
inline constexpr std::size_t max_summary_length = 2000;
inline constexpr std::size_t max_url_length = 2048;
inline constexpr std::size_t max_name_length = 255;
inline constexpr std::size_t max_plugin_id_length = 128;
inline constexpr std::uint64_t max_file_size =
    std::uint64_t{4} * 1024 * 1024 * 1024;
/// The largest `serial`: 2^53, the largest whole number every JSON reader
/// keeps exactly.
inline constexpr std::uint64_t max_serial = std::uint64_t{1} << 53;
inline constexpr std::string_view official_plugin_prefix =
    "com.virtualserialstudio.";

enum class channel
{
    stable,
    preview
};

enum class file_kind
{
    windows_installer,
    windows_zip,
    macos_dmg,
    macos_archive,
    linux_deb,
    linux_rpm,
    linux_archive
};

enum class architecture
{
    x86_64,
    arm64
};

enum class operating_system
{
    windows,
    macos,
    linux
};

/// The SHA-512 of a file's bytes.
using sha512_digest = std::array<std::uint8_t, 64>;

/// One file of a Studio release: an installer, a package or an archive.
struct studio_file
{
    file_kind kind = file_kind::windows_installer;
    architecture arch = architecture::x86_64;
    /// A file name and nothing that can be a path.
    std::string name;
    /// `https` only.
    std::string url;
    std::uint64_t size = 0;
    sha512_digest sha512{};
};

struct studio_release
{
    update::channel channel = update::channel::stable;
    update::version version;
    std::chrono::year_month_day released{};
    /// Plain text; may be empty.
    std::string summary;
    std::string notes_url;
    std::vector<studio_file> files;
};

struct plugin_file
{
    operating_system os = operating_system::windows;
    architecture arch = architecture::x86_64;
    std::string name;
    std::string url;
    std::uint64_t size = 0;
    sha512_digest sha512{};
};

struct plugin_release
{
    std::string id;
    update::version version;
    std::chrono::year_month_day released{};
    /// The oldest Studio this plugin version runs with.
    update::version requires_studio;
    std::string summary;
    std::string notes_url;
    std::vector<plugin_file> files;
};

struct feed
{
    /// Higher in every feed that is published.
    std::uint64_t serial = 0;
    std::chrono::sys_seconds issued_at{};
    std::vector<studio_release> studio;
    std::vector<plugin_release> plugins;
};

/// The names the feed writes, in both directions. `*_from` is empty for a
/// name the feed does not use.
std::string_view to_string(channel value) noexcept;
std::string_view to_string(file_kind value) noexcept;
std::string_view to_string(architecture value) noexcept;
std::string_view to_string(operating_system value) noexcept;
std::optional<channel> channel_from(std::string_view text) noexcept;
std::optional<file_kind> file_kind_from(std::string_view text) noexcept;
std::optional<architecture> architecture_from(std::string_view text) noexcept;
std::optional<operating_system>
operating_system_from(std::string_view text) noexcept;

/// Reads the payload: the bytes the signature covers. Pure. Every rule of
/// docs/updates.md, section 4.3, is enforced here and a feed that breaks one
/// is refused as a whole with `feed_malformed`, except `feed_version` is a
/// whole number other than 1, which is `feed_version_unsupported`. `detail`,
/// when given, receives one line for a person ("studio[0].version: ...") and
/// is empty on success.
result<feed> parse_feed_payload(std::string_view payload,
                                std::string* detail = nullptr);

/// Reads a feed as it travels. In this order: an empty `keys` is
/// `no_update_keys`; the envelope is parsed; it is verified for `feed_purpose`
/// with `keys`; only then is the payload parsed; the key's window is checked
/// against `issued_at`; and `serial` is compared with `highest_serial_seen`
/// (equal is accepted, because it is the same feed fetched again). Nothing of
/// the payload is read before the signature verified. Every fault of the
/// envelope, the key or the signature is `signature_invalid`.
result<feed>
read_feed(std::string_view file_text,
          std::span<const vss::license::signed_file::trusted_key> keys,
          std::uint64_t highest_serial_seen, std::string* detail = nullptr);

/// The feed keys built into this program, without any test key. Empty until
/// the owner adds the public halves.
std::vector<vss::license::signed_file::trusted_key> product_feed_keys();

/// The keys compiled in (`trusted_keys_release.cpp`). Use `product_feed_keys`
/// instead; this is exposed for its test.
std::span<const vss::license::signed_file::trusted_key> builtin_feed_keys();

} // namespace vss::update
