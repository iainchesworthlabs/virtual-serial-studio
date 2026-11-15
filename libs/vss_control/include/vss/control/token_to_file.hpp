//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/control/client.hpp>
#include <vss/control/private_file.hpp>
#include <vss/control/proto/message.hpp>
#include <vss/core/error.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vss::control
{

/// What `create_token_to_file` did, when the gateway made the token.
struct token_to_file_outcome
{
    /// The record of the token.  The secret is not here: it went to the file,
    /// and nowhere else.
    token_entry entry;
    /// Why the token could not be written to the file, or no error when it was.
    error_code write_error;
    /// True when the token could not be written and was revoked again, so that
    /// no token is left that nobody holds.  False with a `write_error` means
    /// the revoking failed too, and the token with this id is still good.
    bool revoked = false;
};

/// Makes a token at the gateway (`administration` is a client of the
/// administration endpoint) and writes it, with a line ending, to `file`,
/// which the caller made first with `claim_private_file`.  The secret is
/// written to the file and goes nowhere else.
///
/// The error is the gateway's when it refused to make the token.  When the
/// token was made and the write failed, the token is revoked and the outcome
/// says so; the caller then drops the claim, which removes the file.  On
/// success the caller calls `keep` on the claim.
result<token_to_file_outcome> create_token_to_file(
    client& administration, const std::string& label,
    const std::vector<std::string>& scopes, std::uint64_t life_s,
    const std::vector<std::string>& transports, private_file_claim& file);

} // namespace vss::control
