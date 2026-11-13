//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/license/sign.hpp>

#include <monocypher-ed25519.h>
#include <monocypher.h>

namespace vss::license::signing
{

namespace
{

/// The 64 byte key Monocypher signs with, wiped when it goes out of scope.
struct expanded_key
{
    std::array<std::uint8_t, 64> secret{};
    signed_file::public_key public_half{};

    explicit expanded_key(const seed& private_key)
    {
        // crypto_ed25519_key_pair wipes the seed it is given, so it gets a
        // copy.
        seed copy = private_key;
        crypto_ed25519_key_pair(secret.data(), public_half.data(), copy.data());
    }
    expanded_key(const expanded_key&) = delete;
    expanded_key& operator=(const expanded_key&) = delete;
    ~expanded_key() { crypto_wipe(secret.data(), secret.size()); }
};

} // namespace

signed_file::public_key public_key_of(const seed& private_key)
{
    const expanded_key key(private_key);
    return key.public_half;
}

signed_file::envelope sign(std::string_view purpose, std::string_view key_id,
                           std::span<const std::uint8_t> payload,
                           const seed& private_key)
{
    signed_file::envelope file;
    file.purpose = purpose;
    file.key_id = key_id;
    file.payload.assign(payload.begin(), payload.end());

    const auto message = signed_file::signed_message(purpose, payload);
    const expanded_key key(private_key);
    crypto_ed25519_sign(file.sig.data(), key.secret.data(), message.data(),
                        message.size());
    return file;
}

} // namespace vss::license::signing
