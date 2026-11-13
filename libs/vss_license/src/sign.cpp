//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/license/sign.hpp>

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <algorithm>

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

result<seed> parse_seed(std::string_view text)
{
    constexpr std::string_view space = " \t\r\n";
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos)
        return errc::malformed;
    const auto last = text.find_last_not_of(space);
    auto bytes =
        signed_file::base64url_decode(text.substr(first, last - first + 1));
    if (!bytes)
        return errc::malformed;
    seed value{};
    const bool right_size = bytes->size() == value.size();
    if (right_size)
        std::copy(bytes->begin(), bytes->end(), value.begin());
    crypto_wipe(bytes->data(), bytes->size());
    if (!right_size)
        return errc::malformed;
    return value;
}

std::string format_seed(const seed& private_key)
{
    return signed_file::base64url_encode(private_key);
}

void wipe(seed& private_key) noexcept
{
    crypto_wipe(private_key.data(), private_key.size());
}

void wipe(std::string& text) noexcept
{
    crypto_wipe(text.data(), text.size());
    text.clear();
}

} // namespace vss::license::signing
