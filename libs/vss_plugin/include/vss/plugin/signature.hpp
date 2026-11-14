//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Signatures: the official key list, the signature of a plugin and the
// signature of a revocation list.  docs/plugins.md, sections 8.2, 8.5 and
// 8.9, are the design.
//
// Every signed file here is the envelope that licence files use too
// (`vss::license::signed_file`): Ed25519 over a message that starts with the
// file's purpose, so a signature made for one kind of file never passes as
// another.  Three purposes belong to plugins:
//
//   key-list            the official key list, signed by a root key
//   plugin              a plugin's `plugin.sig`
//   plugin-revocations  the revocation list
//
// The chain is short.  A root public key is compiled into Studio.  It signs
// the key list.  A key of the list signs official plugins or revocation
// lists, each key for one of the two and only between its two dates.
// Nothing here makes, holds or reads a private key.
//
// A public key is never taken from the file being checked.  It comes from
// the key list, from the administrator's policy or from the keys the user
// approved.  A signature that names a key found in none of them cannot be
// checked, and is reported as exactly that.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/revocations.hpp>
#include <vss/plugin/verify.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin
{

inline constexpr std::string_view key_list_purpose = "key-list";
inline constexpr std::string_view plugin_purpose = "plugin";
inline constexpr std::string_view revocations_purpose = "plugin-revocations";

/// The format of the key list this version reads.
inline constexpr std::uint32_t key_list_format_version = 1;

inline constexpr std::size_t max_official_keys = 64;
inline constexpr std::size_t max_revoked_keys = 256;

/// The 32 bytes of an Ed25519 public key.
using key_bytes = std::array<std::uint8_t, 32>;

/// A root key: the one key Studio is built with.  It signs key lists and
/// nothing else.
struct root_key
{
    std::string key_id;
    key_bytes public_key{};
};

/// The root keys built into this program.  Empty until the owner adds the
/// real public keys; while it is empty no key list is accepted, so no plugin
/// is official.
std::span<const root_key> builtin_root_keys() noexcept;

/// One key of the official key list.
struct official_key
{
    std::string key_id;
    key_bytes public_key{};
    /// `plugin` or `plugin-revocations`.  A key signs for its purpose only.
    std::string purpose;
    /// Who the key stands for, shown to the user.
    std::string name;
    /// The first and the last day the key counts on.
    std::chrono::year_month_day valid_from{};
    std::chrono::year_month_day valid_to{};

    friend bool operator==(const official_key&, const official_key&) = default;
};

struct key_list
{
    /// A list replaces another only when its serial is higher.
    std::uint64_t serial = 0;
    std::chrono::year_month_day issued{};
    std::vector<official_key> keys;
    /// Ids of keys that were withdrawn.  A withdrawn key signs for nothing,
    /// whatever its dates say, and stays withdrawn in every later list.
    std::vector<std::string> revoked;

    /// The key with this id and purpose, or null.  The dates and the
    /// withdrawn list are not looked at.
    const official_key* find(std::string_view key_id,
                             std::string_view purpose) const noexcept;

    /// Whether `key_id` was withdrawn.
    bool is_revoked(std::string_view key_id) const noexcept;

    friend bool operator==(const key_list&, const key_list&) = default;
};

/// Whether `day` is one of the days `key` counts on.
bool counts_on(const official_key& key, std::chrono::sys_days day) noexcept;

/// Reads the payload of a key list: the bytes its signature covers.
///
/// `errc::config_error` for anything that is not a key list this version
/// reads: text that is not JSON, a key the format does not have or one
/// given twice, a wrong type, a `key_list_version` other than
/// `key_list_format_version`, a serial of zero or above
/// `max_revocations_serial`, a key with a purpose other than `plugin` and
/// `plugin-revocations`, a key id listed twice, a `valid_to` before its
/// `valid_from`, or a list over its limit.  `detail`, when given, receives
/// one line for a person.
result<key_list> parse_key_list(std::string_view payload,
                                std::string* detail = nullptr);

/// Reads an official key list as it travels: `official-keys.json`, an
/// envelope with the purpose `key-list`.
///
/// `plugin_errc::signature_invalid` when the text is not an envelope, has
/// another purpose, names a key that is not one of `roots`, or its
/// signature does not verify.  Then, for the payload, the errors of
/// `parse_key_list`.  The caller keeps the list only when its serial is
/// higher than any it has seen (`trust_store::note_key_list_serial`).
result<key_list> read_key_list(std::string_view file_text,
                               std::span<const root_key> roots,
                               std::string* detail = nullptr);

/// Reads a revocation list as it travels: `revocations.json`, an envelope
/// with the purpose `plugin-revocations`.
///
/// `plugin_errc::signature_invalid` when the text is not an envelope, has
/// another purpose, names a key that `official` does not hold for that
/// purpose or that was withdrawn, when its signature does not verify, or
/// when the key does not count on the day the list says it was issued.
/// Then, for the payload, the errors of `parse_revocations`.  The caller
/// applies the list only when its serial is higher than any it has seen
/// (`trust_store::note_revocation_serial`).
result<revocation_list> read_revocations(std::string_view file_text,
                                         const key_list& official,
                                         std::string* detail = nullptr);

/// A signing key that someone on this computer approved: the
/// administrator, in the policy's `allow_keys`, or the user, in the trust
/// store.
struct approved_signer
{
    std::string key_id;
    key_bytes public_key{};
};

struct signature_input
{
    /// The exact bytes of `plugin.json`.
    std::string_view manifest_bytes;
    /// The manifest read from those bytes.
    const manifest* declared = nullptr;
    /// The exact bytes of `plugin.sig`, or nothing when the plugin has none.
    std::optional<std::string_view> signature_text;
    /// The official key list, or null when none was accepted.
    const key_list* official = nullptr;
    std::span<const approved_signer> administrator_keys;
    std::span<const approved_signer> user_keys;
    /// The day to judge a key's dates by: the later of the system's date
    /// and the latest date ever seen.
    std::chrono::sys_days today;
};

/// Checks a plugin's signature and says what was found.
///
/// The signed payload names the plugin by id, version and the SHA-512 of
/// the exact bytes of `plugin.json`.  The files of the plugin are covered
/// through the manifest, which lists each with its hash; `check_files`
/// holds the directory to that list.
///
/// - `absent`: there is no `plugin.sig`.
/// - `invalid`: the file is not an envelope or has another purpose; the
///   payload is not as described, or names another plugin id, another
///   version or other manifest bytes; the key it names is an official key
///   that was withdrawn or does not count today; or the signature does not
///   verify with the key it names.  Also when `declared` is null.
/// - `valid`: it verifies.  `origin` says where the key is known from.
///   The key is looked for by its id in the official list (purpose
///   `plugin`) first, then among the administrator's keys, then among the
///   user's.  The first place that has the id is the one that counts: a
///   signature that fails with that key is `invalid`, and no other place
///   is tried.
/// - `unknown_key`: the envelope and the payload are in order, and no
///   place has a key with the id it names.  The signature was not checked.
///
/// `key_id` is set for `valid` and `unknown_key`.  `detail`, when given,
/// receives one line for a person about `invalid`.
signature_finding check_signature(const signature_input& input,
                                  std::string* detail = nullptr);

} // namespace vss::plugin
