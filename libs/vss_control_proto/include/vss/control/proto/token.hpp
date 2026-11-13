//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The form of a token (docs/remote-control.md, section 4):
//
//     vsst1_<id>_<secret>
//
//     vsst1    the format, and a fixed start that a scanner for leaked
//              secrets can look for
//     id       8 random bytes as 13 characters of base32.  Public: it names
//              the token in lists and in the audit log
//     secret   32 random bytes as 52 characters of base32.  Never stored and
//              never logged
//
// Base32 here is the alphabet `a-z2-7`, lower case, without padding.  A token
// carries no claims: what it may do and when it ends is kept by the gateway.
// Nothing here decodes base32: the id is looked up as text and the secret is
// hashed as text, so two spellings can never mean the same token.
//

#pragma once

#include <vss/control/proto/secret.hpp>
#include <vss/core/bytes.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vss::control
{

inline constexpr std::string_view token_prefix = "vsst1";
inline constexpr std::size_t token_id_bytes = 8;
inline constexpr std::size_t token_secret_bytes = 32;
inline constexpr std::size_t token_id_size = 13;
inline constexpr std::size_t token_secret_size = 52;
/// `vsst1` + `_` + id + `_` + secret.
inline constexpr std::size_t token_size =
    token_prefix.size() + 1 + token_id_size + 1 + token_secret_size;

/// Bytes as base32 text: lower case, no padding.
std::string base32_encode(bytes_view data);

/// True for 13 characters of the base32 alphabet.
bool valid_token_id(std::string_view id) noexcept;

/// The two parts of a token.
struct token_parts
{
    std::string id;
    secret_string secret;
};

/// Takes a token apart.  Nothing unless it is exactly the form above: the
/// right start, the right lengths, only characters of the alphabet, nothing
/// before and nothing after.
std::optional<token_parts> parse_token(const secret_string& token);

/// Puts a token together from an id and a secret that have the right form.
secret_string make_token(std::string_view id, const secret_string& secret);

/// What the gateway keeps of a secret.
using token_digest = std::array<std::uint8_t, 32>;

/// SHA-256 of `vsst1-token` followed by the secret as text.  The secret has
/// 256 bits of entropy, so a slow hash would add nothing against guessing and
/// would give a caller a way to make the gateway work hard.
token_digest hash_token_secret(const secret_string& secret);

/// Compares two digests in constant time.
bool same_digest(const token_digest& first,
                 const token_digest& second) noexcept;

} // namespace vss::control
