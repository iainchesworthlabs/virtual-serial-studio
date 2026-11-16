//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: BSD-2-Clause
//
// A reader for Virtual Serial Studio licence files (`.vsslic`), for a plugin
// that checks the licence itself. This header and `licence_reader.cpp` are the
// whole reader. They need C++17, the standard library and Monocypher's
// Ed25519 (`monocypher-ed25519.h`), which the user supplies. See README.md.
//
// Nothing here reads a file, a clock or a key list of its own: the caller
// passes the bytes of the file, the time and the keys it trusts.
//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace vss_licence_reader
{

/// A file larger than this is refused before it is parsed.
inline constexpr std::size_t max_file_size = std::size_t{64} * 1024;

/// Every time here is a count of seconds since 1970-01-01T00:00:00Z.
using seconds = std::int64_t;

/// "No such time": a lower bound the caller does not have.
inline constexpr seconds no_time = std::numeric_limits<seconds>::min();

/// A clock this far behind a time that has certainly passed is still
/// accepted.
inline constexpr seconds clock_tolerance = 48 * 60 * 60;

/// A licence with this many whole days left, or fewer, is `expiring`.
inline constexpr int expiring_days = 30;
inline constexpr int expiring_days_trial = 3;

/// A public key the caller trusts to sign licences.
struct trusted_key
{
    /// The `key_id` of the files it signs.
    std::string id;
    std::array<std::uint8_t, 32> public_key{};
    /// The signing times (`issued_at`) the key is good for, both ends
    /// included.
    seconds not_before = std::numeric_limits<seconds>::min();
    seconds not_after = std::numeric_limits<seconds>::max();
};

/// What the caller knows when it asks about a licence.
struct question
{
    /// The keys the caller trusts. A file signed by any other key is
    /// refused.
    std::vector<trusted_key> keys;
    /// The time now.
    seconds now = 0;
    /// When the caller's program was built, or `no_time`.
    seconds build_time = no_time;
    /// The latest time the caller has seen, which it stores itself, or
    /// `no_time`.
    seconds latest_seen = no_time;
    /// The feature names to look for. The licence grants the answer when it
    /// names any one of them. A plugin asks for `plugins.official` and
    /// `plugins.<its id>`. Empty asks only whether the licence is in force.
    std::vector<std::string> features;
    /// Whether `released` is set.
    bool has_released = false;
    /// When the thing asked about was released (a plugin takes it from its
    /// own build, never from a file time). Used for the version window: a
    /// licence that sets `updates_until` keeps what was released on or
    /// before that date working after the licence has ended.
    seconds released = 0;
};

enum class state
{
    valid,
    /// Valid, and close to its end.
    expiring,
    /// Past `expires_at`, within the grace period. The features still work.
    grace,
    /// Past the grace period.
    expired,
    not_yet_valid,
    /// The clock reads well before a time that has certainly passed.
    clock_suspect,
    invalid
};

struct answer
{
    /// Whether the feature asked about may be used now.
    bool granted = false;
    /// Why, as one of: `ok`, `expired`, `not_yet_valid`, `clock_suspect`,
    /// `malformed`, `newer_version`, `bad_signature`, `revoked`,
    /// `not_in_licence`. A plugin sends it in `licence.report` unchanged.
    std::string reason = "malformed";
    /// Empty unless the signature verified and the payload was read.
    std::string licence_id;

    /// The state of the licence itself, whatever feature was asked about.
    state licence_state = state::invalid;
    /// The key that signed it. Empty unless the signature verified.
    std::string key_id;
    /// Whether the fields below were read from a verified payload.
    bool verified = false;
    seconds issued_at = 0;
    seconds starts_at = 0;
    seconds expires_at = 0;
    /// `expires_at` plus the grace period.
    seconds grace_ends = 0;
    /// Whole days until `expires_at` (`valid`, `expiring`) or until the end
    /// of the grace period (`grace`). 0 is "today". 0 in every other state.
    int days_left = 0;
    /// Granted by the version window: the licence has ended, and what was
    /// asked about was released on or before its `updates_until`.
    bool by_version_window = false;
    /// The licence's `clock_reset` token, or empty. See `apply_reset`.
    std::string clock_reset;
};

/// Reads and judges the text of one licence file. It checks, in this order:
/// the size; the envelope and its five members; the purpose; the key, by
/// `key_id`, in `asked.keys`; the Ed25519 signature; the payload; the key's
/// window against `issued_at`; the clock; the term; the features.
///
/// It does not look at a revocation list or at the licence's `machines`
/// list: a plugin has neither the list nor a use for the computer's name.
answer read_licence(std::string_view file_text, const question& asked);

/// What a caller stores between runs for the clock check.
struct clock_state
{
    seconds latest_seen = no_time;
    /// The `clock_reset` tokens already used, so a licence works once.
    std::vector<std::string> applied_resets;
};

/// The stored state after the caller has seen the time `now`: the latest
/// time only ever moves forward here.
clock_state observe(clock_state stored, seconds now);

/// The stored state after a licence that `read_licence` verified is first
/// seen. A `clock_reset` token that has not been applied sets the latest time
/// back to the licence's `issued_at`, once. Anything else leaves the state
/// as it was.
clock_state apply_reset(clock_state stored, const answer& verified_licence);

/// Reads "2026-11-13T02:10:00Z", the only form a licence uses. False when
/// `text` is not such a time.
bool parse_time(std::string_view text, seconds& value);

/// Whether a `features` entry of a licence covers a feature name: the same
/// name, or a prefix ending in `.*`.
bool feature_matches(std::string_view entry, std::string_view feature);

} // namespace vss_licence_reader
