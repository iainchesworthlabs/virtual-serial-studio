//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/license/signed_file.hpp>

#include "strict_json.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <monocypher-ed25519.h>

#include <algorithm>
#include <string>

namespace vss::license::signed_file
{

namespace
{

constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr std::string_view message_tag = "VSS-SIGNED-FILE-1";

constexpr std::size_t max_purpose_length = 32;
constexpr std::size_t max_key_id_length = 64;
/// base64url of `max_file_size` bytes is longer than any file that passes
/// the size check, so these only bound the two fields against each other.
constexpr std::size_t max_payload_text = max_file_size;
constexpr std::size_t signature_text_length = 86;

int sextet(char c)
{
    const auto at = alphabet.find(c);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

/// Whether 32 bytes are the canonical encoding of a curve point's
/// coordinates (RFC 8032, section 5.1.3): y below the field prime
/// 2^255 - 19, and no sign bit on a point whose x is zero. Only y = 1 and
/// y = -1 have x = 0. Whether the point is on the curve is Monocypher's
/// check. This looks at bytes only and does no arithmetic.
bool canonical_encoding(std::span<const std::uint8_t, 32> point)
{
    const std::uint8_t top = point[31] & 0x7fU;
    const bool sign = (point[31] & 0x80U) != 0;
    const auto middle = point.subspan(1, 30);
    const bool middle_ones = std::ranges::all_of(middle, [](std::uint8_t byte)
                                                 { return byte == 0xffU; });
    const bool middle_zero = std::ranges::all_of(middle, [](std::uint8_t byte)
                                                 { return byte == 0U; });

    // The prime is ed ff .. ff 7f, least significant byte first.
    if (top == 0x7fU && middle_ones && point[0] >= 0xedU)
    {
        return false;
    }
    const bool y_is_one = top == 0U && middle_zero && point[0] == 1U;
    const bool y_is_minus_one =
        top == 0x7fU && middle_ones && point[0] == 0xecU;
    return !(sign && (y_is_one || y_is_minus_one));
}

} // namespace

bool verify_ed25519(const signature& sig, const public_key& key,
                    std::span<const std::uint8_t> message)
{
    if (!canonical_encoding(std::span<const std::uint8_t, 32>(key)) ||
        !canonical_encoding(std::span(sig).first<32>()))
    {
        return false;
    }
    return crypto_ed25519_check(sig.data(), key.data(), message.data(),
                                message.size()) == 0;
}

std::string base64url_encode(std::span<const std::uint8_t> data)
{
    std::string text;
    text.reserve((data.size() * 4 + 2) / 3);
    std::uint32_t bits = 0;
    int count = 0;
    for (const std::uint8_t byte : data)
    {
        bits = (bits << 8U) | byte;
        count += 8;
        while (count >= 6)
        {
            count -= 6;
            text.push_back(
                alphabet[(bits >> static_cast<unsigned>(count)) & 0x3fU]);
        }
    }
    if (count > 0)
    {
        text.push_back(
            alphabet[(bits << static_cast<unsigned>(6 - count)) & 0x3fU]);
    }
    return text;
}

result<std::vector<std::uint8_t>> base64url_decode(std::string_view text)
{
    // A length of 4n+1 cannot come from whole bytes.
    if (text.size() % 4 == 1)
    {
        return errc::malformed;
    }
    std::vector<std::uint8_t> data;
    data.reserve(text.size() * 3 / 4);
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : text)
    {
        const int value = sextet(c);
        if (value < 0)
        {
            return errc::malformed;
        }
        bits = ((bits << 6U) | static_cast<std::uint32_t>(value)) & 0xffffU;
        count += 6;
        if (count >= 8)
        {
            count -= 8;
            data.push_back(static_cast<std::uint8_t>(
                (bits >> static_cast<unsigned>(count)) & 0xffU));
        }
    }
    // The bits left over must be zero, so each value has one spelling.
    if ((bits & ((1U << static_cast<unsigned>(count)) - 1U)) != 0)
    {
        return errc::malformed;
    }
    return data;
}

result<envelope> parse(std::string_view text)
{
    if (text.size() > max_file_size)
    {
        return errc::too_large;
    }
    const auto document = detail::parse_strict(text);
    if (!document || !document->is_object())
    {
        return errc::malformed;
    }
    const boost::json::object& object = document->get_object();

    // The format comes first: a later format may have other fields.
    const auto format = detail::required_integer(object, "format", 1, 1'000);
    if (!format)
    {
        return errc::malformed;
    }
    if (*format != current_format)
    {
        return errc::newer_version;
    }
    if (object.size() != 5)
    {
        return errc::malformed;
    }

    const auto purpose =
        detail::required_string(object, "purpose", max_purpose_length);
    const auto key_id =
        detail::required_string(object, "key_id", max_key_id_length);
    const auto payload_text =
        detail::required_string(object, "payload", max_payload_text);
    const auto signature_text =
        detail::required_string(object, "signature", signature_text_length);
    if (!purpose || !key_id || !payload_text || !signature_text ||
        !detail::is_token(*purpose, max_purpose_length, "-") ||
        !detail::is_token(*key_id, max_key_id_length, "-"))
    {
        return errc::malformed;
    }

    auto payload = base64url_decode(*payload_text);
    const auto sig = base64url_decode(*signature_text);
    if (!payload || !sig || sig->size() != signature{}.size())
    {
        return errc::malformed;
    }

    envelope file;
    file.format = static_cast<int>(*format);
    file.purpose = *purpose;
    file.key_id = *key_id;
    file.payload = std::move(*payload);
    std::ranges::copy(*sig, file.sig.begin());
    return file;
}

std::string write(const envelope& file)
{
    boost::json::object object;
    object["format"] = file.format;
    object["purpose"] = file.purpose;
    object["key_id"] = file.key_id;
    object["payload"] = base64url_encode(file.payload);
    object["signature"] = base64url_encode(file.sig);
    return boost::json::serialize(object) + "\n";
}

std::vector<std::uint8_t> signed_message(std::string_view purpose,
                                         std::span<const std::uint8_t> payload)
{
    std::vector<std::uint8_t> message;
    message.reserve(message_tag.size() + purpose.size() + payload.size() + 2);
    message.insert(message.end(), message_tag.begin(), message_tag.end());
    message.push_back(0);
    message.insert(message.end(), purpose.begin(), purpose.end());
    message.push_back(0);
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

result<const trusted_key*> verify(const envelope& file,
                                  std::string_view expected_purpose,
                                  std::span<const trusted_key> keys)
{
    if (file.format != current_format)
    {
        return errc::newer_version;
    }
    if (file.purpose != expected_purpose)
    {
        return errc::wrong_purpose;
    }
    const auto key = std::ranges::find_if(
        keys, [&](const trusted_key& k)
        { return k.key_id == file.key_id && k.purpose == file.purpose; });
    if (key == keys.end())
    {
        return errc::unknown_key;
    }
    // An optional equals a value only when it holds one.
    if (key->revoked_from == time_point::min())
    {
        return errc::key_not_valid;
    }
    const auto message = signed_message(file.purpose, file.payload);
    if (!verify_ed25519(file.sig, key->key, message))
    {
        return errc::bad_signature;
    }
    return &*key;
}

result<void> check_key_window(const trusted_key& key, time_point issued_at)
{
    if (issued_at < key.not_before || issued_at > key.not_after)
    {
        return errc::key_not_valid;
    }
    if (key.revoked_from && issued_at >= *key.revoked_from)
    {
        return errc::key_not_valid;
    }
    return {};
}

std::vector<trusted_key> without_test_keys(std::span<const trusted_key> keys)
{
    std::vector<trusted_key> kept;
    for (const trusted_key& key : keys)
    {
        if (!key.key_id.starts_with(test_key_prefix))
        {
            kept.push_back(key);
        }
    }
    return kept;
}

std::vector<trusted_key> product_trust_list()
{
    return without_test_keys(builtin_keys());
}

} // namespace vss::license::signed_file
