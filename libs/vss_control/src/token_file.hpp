//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The token store on disk: one JSON document in a file only this user can
// read.  It holds hashes, never tokens.
//
//     {"version": 1,
//      "latest_seen": 1794562200123,
//      "tokens": [
//        {"id": "abcdefghijklm", "label": "Lab rig 3",
//         "scopes": ["monitor", "monitor.data"], "transports": ["local"],
//         "created": 1794562200123, "expires": 1797154200123,
//         "last_used": null, "revoked": false,
//         "hash": "<64 hex digits>"}]}
//
// Times are milliseconds since 1970-01-01 UTC.
//

#pragma once

#include <vss/control/token_store.hpp>
#include <vss/core/error.hpp>

#include <filesystem>
#include <vector>

namespace vss::control
{

struct token_file_content
{
    std::vector<token_record> tokens;
    /// The latest time the store has seen.
    wall_clock::time_point latest_seen;

    friend bool operator==(const token_file_content&,
                           const token_file_content&) = default;
};

/// Reads the file.  A file that does not exist is an empty store.
/// `errc::config_error` for a document that is not exactly what
/// `save_token_file` writes: another version, a field missing or of the
/// wrong type, a scope or transport name this build does not know, an id or
/// hash of the wrong form, two records with one id, more than `max_tokens`
/// records.  `errc::permission_denied` when the file is not private to this
/// user.  Nothing is guessed and nothing is repaired: the caller fails closed.
result<token_file_content> load_token_file(const std::filesystem::path& file);

/// Writes the file so that a reader sees the old document or the new one and
/// never a part of either, and nobody but this user can read it.  Its
/// directory is made private when it is missing.
error_code save_token_file(const std::filesystem::path& file,
                           const token_file_content& content);

} // namespace vss::control
