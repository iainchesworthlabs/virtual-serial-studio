//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a user decided about plugins on this computer: the signing keys they
// approved, the plugins they chose to run, and the ledger that keeps an old
// revocation list or an old date from coming back.  docs/plugins.md, section
// 8.4, is the design.
//
// The store is one JSON file, `trust.json`, in the agent's private
// directory.  It is the user's own file, so it is not a secret and not
// protected from the user.  It is still read strictly: a store that cannot
// be read is an error, and the caller then starts with an empty store, in
// which nothing is approved.
//
// Only a person's answer to a prompt changes a decision.  Nothing in a
// plugin, in a plugin's directory or in a message from a plugin does.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/verify.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin
{

/// The format of `trust.json` this version reads and writes.
inline constexpr std::uint32_t trust_store_format_version = 1;

inline constexpr std::size_t max_trust_store_size = std::size_t{1024} * 1024;
inline constexpr std::size_t max_trust_store_depth = 6;
inline constexpr std::size_t max_approved_keys = 256;
inline constexpr std::size_t max_decisions = 1024;
/// How many decisions the store keeps for one plugin id.  The older ones
/// are what "this version asks for more than the last one" is told from.
inline constexpr std::size_t max_decisions_per_plugin = 8;

/// The 32 bytes of an Ed25519 public key.
using public_key_bytes = std::array<std::uint8_t, 32>;

/// A signing key the user approved.  Plugins signed with it count as
/// third-party signed.
struct approved_key
{
    /// By the rule of `is_key_id`.
    std::string key_id;
    public_key_bytes public_key{};
    /// The author's name as the user saw it when they approved the key.
    /// Plain text of at most `max_name_length` bytes.
    std::string name;
    std::chrono::year_month_day approved_on{};

    friend bool operator==(const approved_key&, const approved_key&) = default;
};

/// What the user decided about one plugin, as it was on one day: these
/// files (the hash), from this signer.  A plugin whose files change, or
/// that is signed by another key, has no decision and asks again.
struct plugin_decision
{
    std::string plugin_id;
    plugin_hash hash;
    /// The signing key at the time, or empty for an unsigned plugin.
    std::string key_id;
    /// The level the plugin had when the user decided.
    trust_level level = trust_level::unsigned_plugin;
    /// Whether the user switched the plugin on.
    bool enabled = false;
    /// The permissions the user granted, each at most once.  A subset of
    /// what the plugin declared; the store does not know the manifest and
    /// does not check that.
    std::vector<permission> granted;
    /// The user's choice to run this plugin below the minimum isolation.
    /// It counts only where `may_override_sandbox` holds.
    bool run_without_sandbox = false;
    std::chrono::year_month_day decided_on{};

    friend bool operator==(const plugin_decision&,
                           const plugin_decision&) = default;
};

/// What must only ever go forward.
struct trust_ledger
{
    /// The highest serial of a revocation list that was applied, or 0.
    std::uint64_t revocation_serial = 0;
    /// The highest serial of an official key list that was applied, or 0.
    std::uint64_t key_list_serial = 0;
    /// The latest date ever seen, or nothing before the first.
    std::optional<std::chrono::year_month_day> latest_date;

    friend bool operator==(const trust_ledger&, const trust_ledger&) = default;
};

class trust_store
{
public:
    const std::vector<approved_key>& keys() const noexcept { return keys_; }
    const std::vector<plugin_decision>& decisions() const noexcept
    {
        return decisions_;
    }
    const trust_ledger& ledger() const noexcept { return ledger_; }

    /// The approved key with this id, or null.
    const approved_key* find_key(std::string_view key_id) const noexcept;

    /// The decision for exactly this plugin id, hash and signing key
    /// (`key_id` empty for an unsigned plugin), or null.  A decision never
    /// carries over to other files or to another signer.
    const plugin_decision* find(std::string_view plugin_id,
                                const plugin_hash& hash,
                                std::string_view key_id) const noexcept;

    /// The newest decision for this plugin id other than the one `find`
    /// returns for `hash` and `key_id`: what the user last decided about
    /// another version or another signer of a plugin with this id.  Null
    /// when there is none.  "Newest" is the one added to the store last.
    const plugin_decision* previous(std::string_view plugin_id,
                                    const plugin_hash& hash,
                                    std::string_view key_id) const noexcept;

    /// Approves a signing key.
    ///
    /// `errc::invalid_argument` when the id is not a key id, the name is
    /// empty, too long or not plain text, or the date is not a day that
    /// exists.  `errc::already_exists` when a key with this id is approved
    /// with a different public key: an id never silently changes its key.
    /// Approving the same id and key again replaces the name and the date.
    /// `errc::queue_full` when `max_approved_keys` keys are approved.
    result<void> approve_key(approved_key key);

    /// Takes back the approval of a key and removes every decision about a
    /// plugin signed with it, so those plugins ask again.  Returns whether
    /// the key was approved.
    bool forget_key(std::string_view key_id);

    /// Stores a decision.
    ///
    /// `errc::invalid_argument` when the plugin id is not a plugin id, the
    /// key id is neither empty nor a key id, the level does not fit the key
    /// id (`unsigned_plugin` exactly when the key id is empty), a
    /// permission is granted twice, or the date is not a day that exists.
    /// A decision for the same plugin id, hash and key id is replaced and
    /// becomes the newest.  When the plugin id then has more than
    /// `max_decisions_per_plugin` decisions, its oldest are dropped; when
    /// the store has more than `max_decisions`, the oldest of all are.
    result<void> decide(plugin_decision decision);

    /// Removes one decision.  Returns whether there was one.
    bool forget(std::string_view plugin_id, const plugin_hash& hash,
                std::string_view key_id);

    /// Notes the serial of a revocation list that is about to be applied.
    /// Returns true, and keeps the serial, when it is higher than any seen;
    /// false, and changes nothing, when it is not.  A list is applied only
    /// on true.
    bool note_revocation_serial(std::uint64_t serial) noexcept;

    /// The same for an official key list.
    bool note_key_list_serial(std::uint64_t serial) noexcept;

    /// Notes a date that was seen (the system's, a list's) and returns the
    /// date to judge an expiry by: the later of `seen` and every date noted
    /// before.  Setting the clock back therefore revives nothing.
    std::chrono::sys_days note_date(std::chrono::sys_days seen) noexcept;

    friend bool operator==(const trust_store&, const trust_store&) = default;

private:
    friend result<trust_store> parse_trust_store(std::string_view text,
                                                 std::string* detail);

    std::vector<approved_key> keys_;
    /// Oldest first.
    std::vector<plugin_decision> decisions_;
    trust_ledger ledger_;
};

/// Reads the text of a `trust.json`.
///
/// `errc::config_error` for anything that is not a store this version
/// reads: more than `max_trust_store_size` bytes, text that is not JSON, a
/// key the format does not have or one given twice, a wrong type, a
/// `trust_version` other than `trust_store_format_version`, a list over its
/// limit, or an entry that `approve_key` or `decide` would refuse.
/// `detail`, when given, receives one line for a person.
result<trust_store> parse_trust_store(std::string_view text,
                                      std::string* detail = nullptr);

/// The store as the text of a `trust.json`: indented JSON that ends with a
/// line feed, the same text for the same store, and text that
/// `parse_trust_store` reads back to an equal store.
std::string to_json(const trust_store& store);

/// Reads the store from a file.  A file that is not there is an empty
/// store, which is how every user starts.  `errc::io_error` when the file
/// is there and cannot be read; otherwise as `parse_trust_store`.
result<trust_store> load_trust_store(const std::filesystem::path& path,
                                     std::string* detail = nullptr);

/// Writes the store to a file so that a reader sees the old file or the new
/// one and never part of one: the text goes to a new file beside `path`,
/// which then takes the place of `path`.  `errc::io_error` when that fails;
/// the old file is then unchanged and the new one is removed.
result<void> save_trust_store(const std::filesystem::path& path,
                              const trust_store& store);

} // namespace vss::plugin
