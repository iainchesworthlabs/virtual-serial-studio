//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Signing a file. Only the issuing tool and the tests link this part
// (vss_license_sign); Studio and the plugins verify and never sign.
//

#pragma once

#include <vss/license/signed_file.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace vss::license::signing
{

/// An Ed25519 private key in its 32 byte seed form (RFC 8032).
using seed = std::array<std::uint8_t, 32>;

/// The public half of a private key.
signed_file::public_key public_key_of(const seed& private_key);

/// Signs `payload` for `purpose` and returns the envelope, ready to write
/// with `signed_file::write`. The expanded key is wiped before returning.
signed_file::envelope sign(std::string_view purpose, std::string_view key_id,
                           std::span<const std::uint8_t> payload,
                           const seed& private_key);

} // namespace vss::license::signing
