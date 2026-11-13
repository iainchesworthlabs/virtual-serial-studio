//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/proto/token.hpp>

#include <boost/hash2/sha2.hpp>

#include <algorithm>

namespace vss::control
{
namespace
{

constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz234567";
constexpr std::string_view hash_label = "vsst1-token";

bool in_alphabet(char letter) noexcept
{
    return (letter >= 'a' && letter <= 'z') || (letter >= '2' && letter <= '7');
}

bool all_in_alphabet(std::string_view text) noexcept
{
    return std::ranges::all_of(text, in_alphabet);
}

} // namespace

std::string base32_encode(bytes_view data)
{
    std::string out;
    out.reserve((data.size() * 8 + 4) / 5);
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    for (const byte value : data)
    {
        buffer = (buffer << 8) | static_cast<std::uint32_t>(value);
        bits += 8;
        while (bits >= 5)
        {
            bits -= 5;
            out.push_back(alphabet[(buffer >> bits) & 0x1fu]);
        }
        // Only the bits not yet written are kept.
        buffer &= (1u << bits) - 1u;
    }
    if (bits > 0)
        out.push_back(alphabet[(buffer << (5 - bits)) & 0x1fu]);
    return out;
}

bool valid_token_id(std::string_view id) noexcept
{
    return id.size() == token_id_size && all_in_alphabet(id);
}

std::optional<token_parts> parse_token(const secret_string& token)
{
    const std::string_view text = token.expose();
    if (text.size() != token_size)
        return std::nullopt;
    const std::size_t id_at = token_prefix.size() + 1;
    const std::size_t secret_at = id_at + token_id_size + 1;
    if (text.substr(0, token_prefix.size()) != token_prefix ||
        text[id_at - 1] != '_' || text[secret_at - 1] != '_')
        return std::nullopt;
    const std::string_view id = text.substr(id_at, token_id_size);
    const std::string_view secret = text.substr(secret_at);
    if (!all_in_alphabet(id) || !all_in_alphabet(secret))
        return std::nullopt;
    token_parts out;
    out.id = id;
    out.secret = secret_string(secret);
    return out;
}

secret_string make_token(std::string_view id, const secret_string& secret)
{
    std::string text;
    text.reserve(token_size);
    text += token_prefix;
    text += '_';
    text += id;
    text += '_';
    text += secret.expose();
    secret_string out(text);
    secure_wipe(text.data(), text.size());
    return out;
}

token_digest hash_token_secret(const secret_string& secret)
{
    boost::hash2::sha2_256 hash;
    hash.update(hash_label.data(), hash_label.size());
    const std::string_view text = secret.expose();
    hash.update(text.data(), text.size());
    const auto digest = hash.result();
    token_digest out{};
    std::copy(digest.begin(), digest.end(), out.begin());
    return out;
}

bool same_digest(const token_digest& first, const token_digest& second) noexcept
{
    return constant_time_equal(
        std::string_view(reinterpret_cast<const char*>(first.data()),
                         first.size()),
        std::string_view(reinterpret_cast<const char*>(second.data()),
                         second.size()));
}

} // namespace vss::control
