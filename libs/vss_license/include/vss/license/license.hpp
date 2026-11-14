//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Licence files: the payload, the state a licence is in and what it grants
// (docs/licensing.md). Nothing here reads a clock, a file or the computer's
// name: the caller passes them in, so every state can be tested.
//

#pragma once

#include <vss/license/error.hpp>
#include <vss/license/signed_file.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss::license
{

using time_point = signed_file::time_point;

inline constexpr std::string_view licence_purpose = "licence";
inline constexpr std::string_view revocations_purpose = "revocations";

/// A clock this far behind the latest time seen is still accepted. It covers
/// time zones, daylight saving and a dual-boot machine that keeps local time.
inline constexpr std::chrono::hours clock_tolerance{48};

/// A licence with this many days left, or fewer, is `expiring`.
inline constexpr int expiring_days = 30;
inline constexpr int expiring_days_trial = 3;

inline constexpr int max_grace_days = 366;
inline constexpr int max_seats = 1'000'000;

enum class licence_kind
{
    trial,
    annual,
    enterprise
};

enum class edition
{
    pro,
    enterprise
};

/// The payload of a licence file.
struct licence
{
    std::string licence_id;
    licence_kind kind = licence_kind::annual;
    license::edition edition = license::edition::pro;
    std::string licensee_name;
    /// Optional. A team or role, not a personal address.
    std::string licensee_contact;
    time_point issued_at;
    time_point starts_at;
    time_point expires_at;
    /// Days after `expires_at` during which the features still work.
    int grace_days = 0;
    /// 0 is a licence that is not counted by seat.
    int seats = 1;
    /// Exact names, or a prefix that ends in `.*`.
    std::vector<std::string> features;
    /// Computer names, where a trailing `*` matches a prefix. Empty means
    /// any computer.
    std::vector<std::string> machines;
    /// The version window; see `docs/licensing.md`.
    std::optional<time_point> updates_until;
    /// The licence this one renews or corrects. Empty when there is none.
    std::string replaces;
    /// Optional. A licence that carries a token the program has not applied
    /// before sets the stored latest time back to `issued_at`, once. The
    /// owner sends one to a customer whose clock once ran far ahead.
    std::string clock_reset;

    friend bool operator==(const licence&, const licence&) = default;
};

/// Reads a payload. Unknown fields are ignored; a field named in `critical`
/// that this version does not know gives `errc::newer_version`.
result<licence> parse_payload(std::span<const std::uint8_t> payload);

/// The payload text for a licence, as `parse_payload` reads it. Refuses a
/// licence `parse_payload` would refuse.
result<std::string> write_payload(const licence& value);

/// "2026-11-13T02:10:00Z", the only form a payload uses.
result<time_point> parse_time(std::string_view text);
std::string format_time(time_point value);

/// A key that a revocation list withdraws.
struct revoked_key
{
    std::string key_id;
    /// Files signed at or after this time are refused. Absent revokes the
    /// key outright.
    std::optional<time_point> from;

    friend bool operator==(const revoked_key&, const revoked_key&) = default;
};

/// The payload of a revocation list. It holds ids only, never names.
struct revocation_list
{
    /// The program keeps the list with the highest number it has seen.
    std::uint64_t sequence = 0;
    time_point issued_at;
    std::vector<std::string> licence_ids;
    std::vector<revoked_key> keys;

    friend bool operator==(const revocation_list&,
                           const revocation_list&) = default;
};

result<revocation_list>
parse_revocations(std::span<const std::uint8_t> payload);
result<std::string> write_revocations(const revocation_list& value);

/// Verifies and reads a signed revocation list file.
result<revocation_list>
read_revocations(std::string_view file_text,
                 std::span<const signed_file::trusted_key> keys);

/// The revocation list built into this release
/// (`revocations_release.cpp`). Empty, with sequence 0, until the owner
/// withdraws a licence or a key. A signed list with a higher sequence number
/// takes its place.
revocation_list builtin_revocations();

/// The newer of the two lists; `current` when `candidate` is not newer.
revocation_list newer(const revocation_list& current,
                      const revocation_list& candidate);

enum class state
{
    /// No licence file.
    none,
    valid,
    /// Valid, and close to its end.
    expiring,
    /// Past `expires_at`, within the grace period. The features still work.
    grace,
    /// Past the grace period.
    expired,
    not_yet_valid,
    /// The clock reads well before a time the program knows has passed.
    clock_suspect,
    invalid
};

/// Why a licence is in its state, or why a feature is not granted. The GUI
/// maps each to one sentence.
enum class reason
{
    ok,
    no_licence,
    expired,
    not_yet_valid,
    clock_suspect,
    /// Damaged, incomplete or not a licence at all.
    malformed,
    /// Needs a newer version: an unknown format, key or critical field.
    newer_version,
    bad_signature,
    /// The licence or its key was withdrawn.
    revoked,
    wrong_computer,
    /// The licence is fine and does not include the feature asked for.
    not_in_licence
};

/// What the program knows when it judges a licence. All of it is passed in.
struct context
{
    time_point now;
    /// The latest time the program has seen, from its settings. Leave at
    /// `time_point::min()` when there is none.
    time_point latest_seen = time_point::min();
    /// When this program was built. Another time that has certainly passed.
    time_point build_time = time_point::min();
    std::string computer_name;
    std::span<const signed_file::trusted_key> keys;
    /// May be null.
    const revocation_list* revocations = nullptr;
};

struct status
{
    license::state state = license::state::none;
    license::reason reason = license::reason::no_licence;
    /// Present when the signature verified and the payload was read, even
    /// for an expired or revoked licence, so the page can show whose it is.
    std::optional<licence> value;
    /// The key that signed it. Empty unless the signature verified.
    std::string key_id;
    /// Whole days until `expires_at` (`valid`, `expiring`) or until the end
    /// of the grace period (`grace`). 0 is "today". 0 in every other state.
    int days_left = 0;
    /// `expires_at` plus the grace period. Meaningful when `value` is set.
    time_point grace_ends;

    /// Whether the licence's features work now.
    bool active() const noexcept;
};

/// The status when there is no licence file.
status no_licence();

/// Judges the text of one licence file.
status evaluate(std::string_view file_text, const context& where);

/// Judges a licence whose signature was already checked. `signed_by` may be
/// null in a test; then the key checks are skipped.
status evaluate(const licence& value, const signed_file::trusted_key* signed_by,
                const context& where);

/// The status the program should use out of several files: active beats
/// inactive, valid beats grace, Enterprise beats Pro, then the later expiry.
/// `no_licence()` for an empty list.
status best(std::span<const status> candidates);

struct entitlement
{
    bool granted = false;
    license::reason reason = license::reason::no_licence;
    /// Empty when there is no readable licence.
    std::string licence_id;
    int days_left = 0;
};

/// Whether the licence behind `current` turns `feature` on, and why not.
entitlement check(const status& current, std::string_view feature);

/// Whether a `features` entry covers a feature name: the same name, or a
/// prefix ending in `.*`.
bool feature_matches(std::string_view entry, std::string_view feature);

/// Whether a `machines` entry names a computer. Letters compare without
/// regard to ASCII case; a trailing `*` matches any ending.
bool machine_matches(std::string_view entry, std::string_view computer_name);

/// What the program stores between runs for the clock check.
struct clock_state
{
    time_point latest_seen = time_point::min();
    /// The `clock_reset` tokens already used, so a file works once.
    std::vector<std::string> applied_resets;

    friend bool operator==(const clock_state&, const clock_state&) = default;
};

/// The stored state after the program has seen the time `now`: the latest
/// time only ever moves forward here.
clock_state observe(clock_state stored, time_point now);

/// The stored state after a verified licence is imported. A `clock_reset`
/// token that has not been applied sets the latest time to the licence's
/// `issued_at`. Anything else leaves the state as it was.
clock_state apply_reset(clock_state stored, const licence& imported);

} // namespace vss::license
