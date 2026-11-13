//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The signed envelope that licence files, revocation lists and plugin
// manifests share (docs/licensing.md, "The licence file").
//
//   { "format": 1, "purpose": "...", "key_id": "...",
//     "payload": "<base64url>", "signature": "<base64url>" }
//
// The payload travels as the exact bytes that were signed. A caller verifies
// first and parses the payload second, so no parser reads unsigned data
// beyond these five fields.
//

#pragma once

#include <vss/license/error.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss::license::signed_file
{

/// A file larger than this is refused before it is parsed.
inline constexpr std::size_t max_file_size = 64 * 1024;

/// The envelope format this version reads and writes.
inline constexpr int current_format = 1;

/// A key id with this prefix is a throwaway for tests. `product_trust_list`
/// never returns one.
inline constexpr std::string_view test_key_prefix = "test-";

using public_key = std::array<std::uint8_t, 32>;
using signature = std::array<std::uint8_t, 64>;
using time_point = std::chrono::sys_seconds;

/// A public key the program trusts, for one purpose only.
struct trusted_key
{
    std::string key_id;
    /// `licence`, `revocations`, or the plugin signer's own purpose.
    std::string purpose;
    public_key key{};
    /// The window of signing times (`issued_at`) the key may sign for.
    time_point not_before = time_point::min();
    time_point not_after = time_point::max();
    /// Set by a later release when the key leaked: files signed at or after
    /// this time are refused. `time_point::min()` revokes the key outright.
    std::optional<time_point> revoked_from;
};

struct envelope
{
    int format = current_format;
    std::string purpose;
    std::string key_id;
    std::vector<std::uint8_t> payload;
    signature sig{};
};

/// Reads an envelope. Checks the size, the JSON, the five fields and the
/// base64url. Does not check the signature.
result<envelope> parse(std::string_view text);

/// The text of an envelope, as `parse` reads it.
std::string write(const envelope& file);

/// The bytes a signature covers:
/// "VSS-SIGNED-FILE-1" 0x00 purpose 0x00 payload. The purpose is inside, so
/// a signature made for one kind of file never passes as another.
std::vector<std::uint8_t> signed_message(std::string_view purpose,
                                         std::span<const std::uint8_t> payload);

/// Ed25519 verification as RFC 8032 defines it. Monocypher does the
/// arithmetic. It accepts a point that is not written in its one canonical
/// form, which the RFC does not (section 5.1.3), so those encodings of the
/// key and of the first half of the signature are refused here first. No
/// honest signer produces one.
bool verify_ed25519(const signature& sig, const public_key& key,
                    std::span<const std::uint8_t> message);

/// Checks the envelope against the trusted keys: the purpose is the expected
/// one, a key with that id and purpose exists and is not revoked outright,
/// and the signature verifies. Returns the key, which the caller then gives
/// to `check_key_window` with the signing time it read from the payload.
result<const trusted_key*> verify(const envelope& file,
                                  std::string_view expected_purpose,
                                  std::span<const trusted_key> keys);

/// Whether `key` may have signed a file at `issued_at`.
result<void> check_key_window(const trusted_key& key, time_point issued_at);

/// The keys built into this program, without any test key. Empty until the
/// owner adds the real public keys. While it is empty every licence is refused
/// and Studio is the Community edition.
std::vector<trusted_key> product_trust_list();

/// The keys compiled in (`trusted_keys_release.cpp`). Use
/// `product_trust_list` instead; this is exposed for its test.
std::span<const trusted_key> builtin_keys();

/// `keys` without the test keys.
std::vector<trusted_key> without_test_keys(std::span<const trusted_key> keys);

/// base64url without padding (RFC 4648 section 5). `decode` refuses padding,
/// white space, other alphabets and spare bits that are not zero.
std::string base64url_encode(std::span<const std::uint8_t> data);
result<std::vector<std::uint8_t>> base64url_decode(std::string_view text);

} // namespace vss::license::signed_file
