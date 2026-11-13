//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/control/proto/policy.hpp>
#include <vss/control/proto/secret.hpp>
#include <vss/control/proto/token.hpp>
#include <vss/core/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::control
{

using wall_clock = std::chrono::system_clock;

/// The longest and the usual life of a token.  There is no token that never
/// ends.
inline constexpr std::chrono::hours longest_token_life{24 * 365};
inline constexpr std::chrono::hours usual_token_life{24 * 30};
/// The longest label, in bytes, and the most tokens kept at once.
inline constexpr std::size_t max_token_label_size = 128;
inline constexpr std::size_t max_tokens = 256;

/// What the gateway keeps about a token.  The token itself is not in it.
struct token_record
{
    std::string id;
    /// What the person who made it called it: "Lab rig 3".
    std::string label;
    scope_set scopes;
    /// The transports it may be used on.
    std::vector<transport> transports;
    wall_clock::time_point created;
    wall_clock::time_point expires;
    std::optional<wall_clock::time_point> last_used;
    bool revoked = false;
    /// `hash_token_secret` of its secret.
    token_digest hash{};

    friend bool operator==(const token_record&, const token_record&) = default;
};

/// What a person asks for when they make a token.
struct token_request
{
    std::string label;
    scope_set scopes;
    std::chrono::seconds life = usual_token_life;
    std::vector<transport> transports{transport::local};
};

/// A token that was just made.  `token` is shown to the person once and
/// cannot be had again.
struct new_token
{
    secret_string token;
    token_record record;
};

/// Why a presented token was or was not accepted.  The caller of the gateway
/// never learns which of the refusals it was; the audit log does.
enum class token_check
{
    accepted,
    /// Not the form of a token.
    malformed,
    /// No token has this id.
    unknown,
    wrong_secret,
    revoked,
    expired,
    /// Good, but not for the transport it came over.
    wrong_transport
};

/// The name of a check for the audit log: `wrong_secret`.
std::string_view token_check_name(token_check value) noexcept;

struct token_verdict
{
    token_check outcome = token_check::malformed;
    /// The id that was presented, when it had the form of one.  An id is
    /// public and may be logged.
    std::string id;
    /// For an accepted token: what it may do and when it ends.
    scope_set scopes;
    wall_clock::time_point expires;
};

/// The tokens of this user's gateway (docs/remote-control.md, section 4).
///
/// A token is made here, on the receiving side, and only its SHA-256 is
/// kept, in a file only this user can read.  Whoever reads the file gets
/// nothing that can be presented as a token.
///
/// Verifying does the same work whatever is wrong: the presented secret is
/// always hashed once and one digest is always compared in constant time,
/// against a dummy when there is no record to compare with.
///
/// Time: the store remembers the latest time it has seen, in its file, and
/// takes that when the system clock says something earlier, so setting the
/// clock back does not bring an expired token back.
///
/// Safe for use from any thread.
class token_store
{
public:
    using time_source = std::function<wall_clock::time_point()>;

    /// Reads `file`, which need not exist yet; its directory is made private
    /// when it is missing.  A file that cannot be understood, or is not
    /// private to this user, is an error: the gateway does not start with a
    /// token file it cannot trust, and never replaces one it cannot read.
    static result<std::unique_ptr<token_store>> open(std::filesystem::path file,
                                                     time_source now = {});

    ~token_store();
    token_store(const token_store&) = delete;
    token_store& operator=(const token_store&) = delete;

    /// Makes a token and saves its record.  `errc::invalid_argument` for a
    /// request no token may have: no scopes, the `admin` scope, no
    /// transport, a life of nothing or of more than `longest_token_life`, a
    /// label that is too long or not text.  `errc::queue_full` when
    /// `max_tokens` are kept already.
    result<new_token> create(const token_request& request);

    /// Checks a token that came over `via`.
    token_verdict verify(const secret_string& presented, transport via);

    /// Whether a session that was opened with token `id` may go on:
    /// `accepted`, `revoked`, `expired` or `unknown`.
    token_check standing(std::string_view id);

    /// Revokes a token, now and for good.  `errc::not_found` for an id no
    /// token has.  Revoking twice is not an error.
    result<void> revoke(std::string_view id);

    /// Every record, oldest first, revoked ones included until they expire.
    std::vector<token_record> list();

    /// Secrets hashed since the store was opened, for the test that every
    /// refusal costs the same.
    std::uint64_t hashes_computed() const noexcept;

private:
    class impl;
    explicit token_store(std::unique_ptr<impl> state);
    std::unique_ptr<impl> impl_;
};

} // namespace vss::control
