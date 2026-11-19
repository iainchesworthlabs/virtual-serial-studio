//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "licence_vectors.hpp"

#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/license/test_keys.hpp>

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace vss_test_support::licence_vectors
{

namespace
{

namespace lic = vss::license;
namespace sf = vss::license::signed_file;

constexpr std::string_view test_id = "VSS-TEST-000001";

// The times the vectors turn on. The licence they nearly all use was
// issued on 1 November 2026 for one year, with 28 days of grace.
constexpr std::string_view expires = "2027-11-01T00:00:00Z";
constexpr std::string_view grace_ends = "2027-11-29T00:00:00Z";
constexpr std::string_view mid_term = "2027-01-01T00:00:00Z";

/// The members of a payload, each as the JSON text that follows its name.
struct payload_fields
{
    std::string licence_id = "\"VSS-TEST-000001\"";
    std::string kind = "\"annual\"";
    std::string edition = "\"pro\"";
    std::string licensee =
        "{\"name\":\"Test vector, not a customer\","
        "\"email\":\"vectors@virtual-serial-studio.invalid\"}";
    std::string issued_at = "\"2026-11-01T00:00:00Z\"";
    std::string starts_at = "\"2026-11-01T00:00:00Z\"";
    std::string expires_at = "\"2027-11-01T00:00:00Z\"";
    std::string grace_days = "28";
    std::string seats = "1";
    std::string features =
        "[\"plugins.official\",\"driver.signed\",\"support\"]";
    std::string machines = "[]";
    /// Further members, written whole, each followed by a comma.
    std::string extra;
    std::string critical = "[]";
};

std::string payload_text(const payload_fields& fields)
{
    std::string text = "{\"licence_id\":" + fields.licence_id;
    text += ",\"kind\":" + fields.kind;
    text += ",\"edition\":" + fields.edition;
    text += ",\"licensee\":" + fields.licensee;
    text += ",\"issued_at\":" + fields.issued_at;
    text += ",\"starts_at\":" + fields.starts_at;
    text += ",\"expires_at\":" + fields.expires_at;
    text += ",\"grace_days\":" + fields.grace_days;
    text += ",\"seats\":" + fields.seats;
    text += ",\"features\":" + fields.features;
    text += ",\"machines\":" + fields.machines;
    text += "," + fields.extra;
    text += "\"critical\":" + fields.critical + "}";
    return text;
}

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return {text.begin(), text.end()};
}

/// The text of an envelope from its five members as written, so that a
/// vector can hold what the signing code would never write.
std::string envelope_text(std::string_view format, std::string_view purpose,
                          std::string_view key_id, std::string_view payload,
                          std::string_view signature)
{
    std::string text = "{\"format\":";
    text += format;
    text += ",\"purpose\":\"";
    text += purpose;
    text += "\",\"key_id\":\"";
    text += key_id;
    text += "\",\"payload\":\"";
    text += payload;
    text += "\",\"signature\":\"";
    text += signature;
    text += "\"}\n";
    return text;
}

/// A payload signed with the test key, in its parts.
struct signed_parts
{
    std::string payload;
    std::string signature;
    sf::signature raw_signature{};
};

signed_parts sign_parts(std::string_view purpose, std::string_view payload)
{
    const sf::envelope envelope =
        lic::signing::sign(purpose, lic::test_keys::key_id, bytes_of(payload),
                           lic::test_keys::private_key());
    signed_parts parts;
    parts.payload = sf::base64url_encode(envelope.payload);
    parts.signature = sf::base64url_encode(envelope.sig);
    parts.raw_signature = envelope.sig;
    return parts;
}

/// The licence file for a payload, signed with the test key.
std::string licence_file(std::string_view payload)
{
    const signed_parts parts = sign_parts("licence", payload);
    return envelope_text("1", "licence", lic::test_keys::key_id, parts.payload,
                         parts.signature);
}

std::string licence_file(const payload_fields& fields)
{
    return licence_file(payload_text(fields));
}

/// The licence file for a payload with `signature` in place of the true
/// one.
std::string file_with_signature(std::string_view payload,
                                const sf::signature& signature)
{
    return envelope_text("1", "licence", lic::test_keys::key_id,
                         sf::base64url_encode(bytes_of(payload)),
                         sf::base64url_encode(signature));
}

/// The true signature with the order of the group added to its second
/// half. It stands for the same number, written another way; RFC 8032
/// refuses it.
sf::signature with_order_added(sf::signature signature)
{
    constexpr std::array<std::uint8_t, 32> order{
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
        0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    unsigned carry = 0;
    for (std::size_t index = 0; index < order.size(); ++index)
    {
        const unsigned sum =
            static_cast<unsigned>(signature[32 + index]) + order[index] + carry;
        signature[32 + index] = static_cast<std::uint8_t>(sum & 0xffU);
        carry = sum >> 8U;
    }
    return signature;
}

/// A signature whose first half is `point`, a way of writing the neutral
/// point that RFC 8032 does not allow, and whose second half makes the
/// equation hold for a reader that accepts that writing. Made with the test
/// key's private half, which is published.
sf::signature forged_with_point(const std::array<std::uint8_t, 32>& point,
                                std::string_view payload)
{
    const lic::signing::seed seed = lic::test_keys::private_key();
    const sf::public_key public_key = lic::signing::public_key_of(seed);

    std::array<std::uint8_t, 64> expanded{};
    crypto_sha512(expanded.data(), seed.data(), seed.size());
    std::array<std::uint8_t, 32> scalar{};
    crypto_eddsa_trim_scalar(scalar.data(), expanded.data());

    const std::vector<std::uint8_t> message =
        sf::signed_message("licence", bytes_of(payload));
    std::vector<std::uint8_t> hashed(point.begin(), point.end());
    hashed.insert(hashed.end(), public_key.begin(), public_key.end());
    hashed.insert(hashed.end(), message.begin(), message.end());
    std::array<std::uint8_t, 64> wide{};
    crypto_sha512(wide.data(), hashed.data(), hashed.size());
    std::array<std::uint8_t, 32> challenge{};
    crypto_eddsa_reduce(challenge.data(), wide.data());

    const std::array<std::uint8_t, 32> zero{};
    std::array<std::uint8_t, 32> second{};
    crypto_eddsa_mul_add(second.data(), challenge.data(), scalar.data(),
                         zero.data());

    sf::signature signature{};
    std::copy(point.begin(), point.end(), signature.begin());
    std::copy(second.begin(), second.end(), signature.begin() + 32);
    return signature;
}

/// `text` followed by spaces up to `size` bytes. White space after the
/// object is still JSON.
std::string padded_to(std::string text, std::size_t size)
{
    text.append(size - text.size(), ' ');
    return text;
}

licence_vector make(std::string name, std::string about, std::string file,
                    std::string_view now, std::string reason, bool granted,
                    std::string state)
{
    licence_vector vector;
    vector.name = std::move(name);
    vector.about = std::move(about);
    vector.file_text = std::move(file);
    vector.now = std::string(now);
    vector.reason = std::move(reason);
    vector.granted = granted;
    vector.state = std::move(state);
    // Every answer but the refusals made before the payload is read names
    // the licence.
    const bool unread = vector.state == "invalid" && vector.reason != "revoked";
    vector.licence_id = unread ? std::string() : std::string(test_id);
    return vector;
}

/// A refusal made before the payload is read.
licence_vector refused(std::string name, std::string about, std::string file,
                       std::string reason)
{
    return make(std::move(name), std::move(about), std::move(file), mid_term,
                std::move(reason), false, "invalid");
}

std::string json_text(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string time_or_null(std::string_view text)
{
    return text.empty() ? std::string("null") : json_text(text);
}

void add_terms(std::vector<licence_vector>& list)
{
    const std::string good = licence_file(payload_fields{});

    list.push_back(make("good", "A licence in the middle of its term.", good,
                        mid_term, "ok", true, "valid"));
    list.push_back(make("expiring", "17 days before the end of the term.", good,
                        "2027-10-15T00:00:00Z", "ok", true, "expiring"));
    list.push_back(make("last-second",
                        "The time is expires_at exactly: still in its term.",
                        good, expires, "ok", true, "expiring"));
    list.push_back(make("grace", "Nine days into the 28 days of grace.", good,
                        "2027-11-10T00:00:00Z", "ok", true, "grace"));
    list.push_back(make("grace-last-second",
                        "The time is the end of the grace period exactly.",
                        good, grace_ends, "ok", true, "grace"));
    list.push_back(make("expired-first-second",
                        "One second after the grace period.", good,
                        "2027-11-29T00:00:01Z", "expired", false, "expired"));
    list.push_back(make("expired", "Well after the grace period.", good,
                        "2027-12-15T00:00:00Z", "expired", false, "expired"));

    payload_fields later;
    later.starts_at = "\"2027-01-01T00:00:00Z\"";
    list.push_back(make("not-yet-valid",
                        "Issued in November 2026 for a term that starts in "
                        "January 2027, read in December 2026.",
                        licence_file(later), "2026-12-01T00:00:00Z",
                        "not_yet_valid", false, "not_yet_valid"));

    payload_fields trial;
    trial.kind = "\"trial\"";
    trial.expires_at = "\"2026-11-15T00:00:00Z\"";
    trial.grace_days = "0";
    list.push_back(make("trial", "A 14 day trial with 10 days left.",
                        licence_file(trial), "2026-11-05T00:00:00Z", "ok", true,
                        "valid"));
    list.push_back(make("trial-expiring",
                        "A trial with two days left: a trial is expiring "
                        "in its last three days, not its last 30.",
                        licence_file(trial), "2026-11-13T00:00:00Z", "ok", true,
                        "expiring"));
    list.push_back(make("trial-ended",
                        "A trial has no grace: it has ended one second "
                        "after expires_at.",
                        licence_file(trial), "2026-11-15T00:00:01Z", "expired",
                        false, "expired"));
}

void add_features(std::vector<licence_vector>& list)
{
    const std::string good = licence_file(payload_fields{});

    licence_vector missing =
        make("feature-missing",
             "The licence is in force and does not name the feature.", good,
             mid_term, "not_in_licence", false, "valid");
    missing.features = {"plugins.com.example.other"};
    list.push_back(missing);

    payload_fields one_plugin;
    one_plugin.features = "[\"plugins.com.example.decoder\",\"support\"]";
    licence_vector by_id =
        make("feature-by-plugin-id",
             "The licence names one plugin and not plugins.official; the "
             "plugin asks for both names.",
             licence_file(one_plugin), mid_term, "ok", true, "valid");
    by_id.features = {"plugins.official", "plugins.com.example.decoder"};
    list.push_back(by_id);

    payload_fields every_plugin;
    every_plugin.features = "[\"plugins.*\"]";
    licence_vector wildcard =
        make("feature-wildcard", "plugins.* covers plugins.official.",
             licence_file(every_plugin), mid_term, "ok", true, "valid");
    list.push_back(wildcard);

    licence_vector not_covered = make(
        "feature-wildcard-not-covered",
        "plugins.* does not cover a name outside plugins.",
        licence_file(every_plugin), mid_term, "not_in_licence", false, "valid");
    not_covered.features = {"driver.signed"};
    list.push_back(not_covered);

    payload_fields window;
    window.extra = "\"updates_until\":\"2027-11-01T00:00:00Z\",";
    licence_vector inside = make(
        "version-window-inside",
        "The licence has ended. It sets updates_until, and what is "
        "asked about was released before that date.",
        licence_file(window), "2028-06-01T00:00:00Z", "ok", true, "expired");
    inside.released = "2027-06-01T00:00:00Z";
    list.push_back(inside);

    licence_vector outside =
        make("version-window-outside",
             "The same licence; what is asked about was released after "
             "updates_until.",
             licence_file(window), "2028-06-01T00:00:00Z", "expired", false,
             "expired");
    outside.released = "2028-01-01T00:00:00Z";
    list.push_back(outside);

    licence_vector unnamed = make(
        "version-window-feature-missing",
        "The window keeps only what the licence names.", licence_file(window),
        "2028-06-01T00:00:00Z", "expired", false, "expired");
    unnamed.released = "2027-06-01T00:00:00Z";
    unnamed.features = {"plugins.com.example.other"};
    list.push_back(unnamed);

    licence_vector no_window =
        make("version-window-not-set",
             "A licence without updates_until keeps nothing after it ends.",
             good, "2028-06-01T00:00:00Z", "expired", false, "expired");
    no_window.released = "2027-06-01T00:00:00Z";
    list.push_back(no_window);
}

void add_clock(std::vector<licence_vector>& list)
{
    const std::string good = licence_file(payload_fields{});

    licence_vector seen =
        make("clock-suspect-latest-seen",
             "The clock reads three days before the latest time the "
             "caller has seen.",
             good, mid_term, "clock_suspect", false, "clock_suspect");
    seen.latest_seen = "2027-01-04T00:00:00Z";
    list.push_back(seen);

    licence_vector tolerated =
        make("clock-tolerated",
             "The clock reads 48 hours before the latest time seen, which "
             "is allowed.",
             good, mid_term, "ok", true, "valid");
    tolerated.latest_seen = "2027-01-03T00:00:00Z";
    list.push_back(tolerated);

    licence_vector just_over =
        make("clock-suspect-one-second",
             "48 hours and one second before the latest time seen.", good,
             mid_term, "clock_suspect", false, "clock_suspect");
    just_over.latest_seen = "2027-01-03T00:00:01Z";
    list.push_back(just_over);

    licence_vector built =
        make("clock-suspect-build-time",
             "The clock reads two months before the caller was built.", good,
             mid_term, "clock_suspect", false, "clock_suspect");
    built.build_time = "2027-03-01T00:00:00Z";
    list.push_back(built);

    list.push_back(make("clock-suspect-issued-at",
                        "The clock reads a month before the licence was "
                        "signed.",
                        good, "2026-10-01T00:00:00Z", "clock_suspect", false,
                        "clock_suspect"));
}

void add_keys(std::vector<licence_vector>& list)
{
    const std::string good = licence_file(payload_fields{});

    licence_vector unknown =
        refused("unknown-key", "The caller's key list has no key of this id.",
                good, "newer_version");
    unknown.keys = "other-id";
    list.push_back(unknown);

    licence_vector none = refused("no-keys", "The caller trusts no key at all.",
                                  good, "newer_version");
    none.keys = "empty";
    list.push_back(none);

    licence_vector early =
        make("key-not-yet-good",
             "The key is good from 2027; the licence was signed in 2026.", good,
             mid_term, "revoked", false, "invalid");
    early.keys = "test-from-2027";
    list.push_back(early);

    licence_vector late =
        make("key-no-longer-good",
             "The key was good until the start of 2026; the licence was "
             "signed after that.",
             good, mid_term, "revoked", false, "invalid");
    late.keys = "test-until-2026";
    list.push_back(late);
}

void add_signatures(std::vector<licence_vector>& list)
{
    const std::string payload = payload_text(payload_fields{});
    const signed_parts parts = sign_parts("licence", payload);

    // One character of the payload text differs: the seat count.
    payload_fields other;
    other.seats = "9";
    list.push_back(refused(
        "bad-signature-payload-changed",
        "The payload was changed after it was signed.",
        envelope_text("1", "licence", lic::test_keys::key_id,
                      sf::base64url_encode(bytes_of(payload_text(other))),
                      parts.signature),
        "bad_signature"));

    sf::signature flipped = parts.raw_signature;
    flipped[0] = static_cast<std::uint8_t>(flipped[0] ^ 0x01U);
    list.push_back(
        refused("bad-signature-bit", "One bit of the signature is flipped.",
                file_with_signature(payload, flipped), "bad_signature"));

    list.push_back(refused(
        "bad-signature-all-zero", "The signature is 64 zero bytes.",
        file_with_signature(payload, sf::signature{}), "bad_signature"));

    list.push_back(refused(
        "malleable-order-added",
        "The true signature with the group order added to its second "
        "half: the same number written another way.",
        file_with_signature(payload, with_order_added(parts.raw_signature)),
        "bad_signature"));

    std::array<std::uint8_t, 32> above_prime{};
    above_prime.fill(0xff);
    above_prime[0] = 0xee;
    above_prime[31] = 0x7f;
    list.push_back(refused(
        "malleable-point-above-prime",
        "The first half of the signature writes the neutral point with a "
        "y that is not below the prime; the second half fits it.",
        file_with_signature(payload, forged_with_point(above_prime, payload)),
        "bad_signature"));

    std::array<std::uint8_t, 32> signed_zero{};
    signed_zero[0] = 0x01;
    signed_zero[31] = 0x80;
    list.push_back(refused(
        "malleable-point-sign-bit",
        "The first half of the signature writes the neutral point with "
        "the sign bit set although x is zero; the second half fits it.",
        file_with_signature(payload, forged_with_point(signed_zero, payload)),
        "bad_signature"));

    const signed_parts as_revocations = sign_parts("revocations", payload);
    list.push_back(
        refused("wrong-purpose",
                "Signed, correctly, as a revocation list: not a licence.",
                envelope_text("1", "revocations", lic::test_keys::key_id,
                              as_revocations.payload, as_revocations.signature),
                "malformed"));
    list.push_back(
        refused("purpose-swapped",
                "Signed as a revocation list, then the purpose changed to "
                "licence: the purpose is part of what is signed.",
                envelope_text("1", "licence", lic::test_keys::key_id,
                              as_revocations.payload, as_revocations.signature),
                "bad_signature"));
}

void add_envelopes(std::vector<licence_vector>& list)
{
    const std::string payload = payload_text(payload_fields{});
    const signed_parts parts = sign_parts("licence", payload);
    const std::string_view key_id = lic::test_keys::key_id;
    const std::string good = licence_file(payload);

    list.push_back(refused(
        "unknown-format", "The envelope says format 2.",
        envelope_text("2", "licence", key_id, parts.payload, parts.signature),
        "newer_version"));
    list.push_back(refused(
        "format-not-whole", "The format is written 1.0, not 1.",
        envelope_text("1.0", "licence", key_id, parts.payload, parts.signature),
        "malformed"));
    list.push_back(refused("format-is-text",
                           "The format is the text \"1\", not the number.",
                           envelope_text("\"1\"", "licence", key_id,
                                         parts.payload, parts.signature),
                           "malformed"));

    list.push_back(make("size-64-kib",
                        "A good licence followed by spaces up to 65536 "
                        "bytes, the most a file may have.",
                        padded_to(good, sf::max_file_size), mid_term, "ok",
                        true, "valid"));
    list.push_back(refused(
        "over-64-kib", "The same file with one more space: 65537 bytes.",
        padded_to(good, sf::max_file_size + 1), "malformed"));

    list.push_back(refused("bad-base64-padding",
                           "The payload ends with = padding.",
                           envelope_text("1", "licence", key_id,
                                         parts.payload + "=", parts.signature),
                           "malformed"));
    std::string other_alphabet = parts.payload;
    other_alphabet[0] = '+';
    list.push_back(refused(
        "bad-base64-alphabet",
        "The payload has a + from the other base64 alphabet.",
        envelope_text("1", "licence", key_id, other_alphabet, parts.signature),
        "malformed"));
    std::string spare_bits = parts.signature;
    spare_bits.back() = spare_bits.back() == 'B' ? 'C' : 'B';
    list.push_back(refused(
        "bad-base64-spare-bits",
        "The last character of the signature has spare bits that are not "
        "zero.",
        envelope_text("1", "licence", key_id, parts.payload, spare_bits),
        "malformed"));
    list.push_back(refused(
        "signature-too-short", "The signature is 63 bytes.",
        envelope_text("1", "licence", key_id, parts.payload,
                      sf::base64url_encode(std::span<const std::uint8_t>(
                          parts.raw_signature.data(), 63))),
        "malformed"));

    std::string twice = good;
    twice.insert(twice.find("\"key_id\""), "\"key_id\":\"test-other-1\",");
    list.push_back(refused("duplicate-member-envelope",
                           "The envelope has key_id twice.", twice,
                           "malformed"));
    std::string six = good;
    six.insert(six.find("\"key_id\""), "\"note\":\"extra\",");
    list.push_back(refused("extra-member-envelope",
                           "The envelope has a sixth member.", six,
                           "malformed"));
    list.push_back(refused(
        "missing-member-envelope", "The envelope has no key_id.",
        "{\"format\":1,\"purpose\":\"licence\",\"payload\":\"" + parts.payload +
            "\",\"signature\":\"" + parts.signature + "\"}\n",
        "malformed"));
    list.push_back(refused("trailing-text", "Text follows the object.",
                           good + "x\n", "malformed"));
    list.push_back(refused("not-json", "Not JSON at all.",
                           "this is not a licence\n", "malformed"));
    list.push_back(refused("empty", "An empty file.", "", "malformed"));

    std::string escaped = good;
    escaped.replace(escaped.find("\"purpose\""), 9, "\"pur\\u0070ose\"");
    list.push_back(make("escaped-member-name",
                        "The name purpose is written with an escape, which "
                        "JSON allows.",
                        escaped, mid_term, "ok", true, "valid"));
}

void add_payloads(std::vector<licence_vector>& list)
{
    payload_fields unknown_critical;
    unknown_critical.extra = "\"site_lock\":true,";
    unknown_critical.critical = "[\"site_lock\"]";
    list.push_back(
        refused("unknown-critical",
                "The payload has a member this version does not know and names "
                "it in critical.",
                licence_file(unknown_critical), "newer_version"));

    payload_fields known_critical;
    known_critical.critical = "[\"expires_at\",\"features\"]";
    list.push_back(make(
        "known-critical", "critical names only members every reader knows.",
        licence_file(known_critical), mid_term, "ok", true, "valid"));

    payload_fields unknown_member;
    unknown_member.extra = "\"colour\":{\"name\":\"teal\",\"parts\":[1,2.5]},";
    list.push_back(make("unknown-member",
                        "A member this version does not know, not named in "
                        "critical, is ignored.",
                        licence_file(unknown_member), mid_term, "ok", true,
                        "valid"));

    payload_fields null_optional;
    null_optional.extra = "\"updates_until\":null,\"replaces\":null,";
    list.push_back(make("optional-null",
                        "An optional member may be null, which is the same "
                        "as leaving it out.",
                        licence_file(null_optional), mid_term, "ok", true,
                        "valid"));

    payload_fields twice;
    twice.extra = "\"seats\":2,";
    list.push_back(refused("duplicate-member-payload",
                           "The signed payload has seats twice.",
                           licence_file(twice), "malformed"));

    payload_fields twice_inside;
    twice_inside.extra = "\"later\":{\"a\":1,\"a\":2},";
    list.push_back(refused(
        "duplicate-member-nested",
        "A member this version ignores holds an object with a name twice.",
        licence_file(twice_inside), "malformed"));

    payload_fields deep;
    deep.extra = "\"later\":[[[[[[[1]]]]]]],";
    list.push_back(make("nesting-8",
                        "A member nested eight deep, counting the payload "
                        "object: the most that is read.",
                        licence_file(deep), mid_term, "ok", true, "valid"));
    payload_fields too_deep;
    too_deep.extra = "\"later\":[[[[[[[[1]]]]]]]],";
    list.push_back(refused("nesting-9", "A member nested nine deep.",
                           licence_file(too_deep), "malformed"));

    payload_fields bad_text;
    bad_text.licensee = "{\"name\":\"Test \xff vector\"}";
    list.push_back(refused("not-utf-8",
                           "The licensee's name holds a byte that is not "
                           "UTF-8.",
                           licence_file(bad_text), "malformed"));

    payload_fields control;
    control.licensee = "{\"name\":\"Test\\u0007vector\"}";
    list.push_back(refused("control-character",
                           "The licensee's name holds a control character.",
                           licence_file(control), "malformed"));

    payload_fields bad_date;
    bad_date.expires_at = "\"2027-02-30T00:00:00Z\"";
    list.push_back(refused("bad-date", "expires_at is 30 February.",
                           licence_file(bad_date), "malformed"));

    payload_fields leap_day;
    leap_day.expires_at = "\"2028-02-29T00:00:00Z\"";
    list.push_back(make("leap-day", "expires_at is 29 February 2028.",
                        licence_file(leap_day), mid_term, "ok", true, "valid"));

    payload_fields no_zone;
    no_zone.expires_at = "\"2027-11-01T00:00:00+00:00\"";
    list.push_back(refused("date-not-z",
                           "expires_at is not in the form that ends in Z.",
                           licence_file(no_zone), "malformed"));

    payload_fields backwards;
    backwards.expires_at = "\"2026-10-01T00:00:00Z\"";
    list.push_back(refused("ends-before-it-starts",
                           "expires_at is before starts_at.",
                           licence_file(backwards), "malformed"));

    payload_fields fraction;
    fraction.grace_days = "28.0";
    list.push_back(refused("grace-not-whole", "grace_days is written 28.0.",
                           licence_file(fraction), "malformed"));

    payload_fields bad_feature;
    bad_feature.features = "[\"Plugins.Official\"]";
    list.push_back(refused("feature-upper-case",
                           "A feature name with upper-case letters.",
                           licence_file(bad_feature), "malformed"));

    payload_fields bad_email;
    bad_email.licensee =
        "{\"name\":\"Test vector, not a customer\",\"email\":\"not an "
        "address\"}";
    list.push_back(refused("bad-email",
                           "The licensee's email has not the form of an "
                           "address.",
                           licence_file(bad_email), "malformed"));

    list.push_back(refused("payload-not-object",
                           "The signed payload is a JSON list.",
                           licence_file("[1,2,3]"), "malformed"));

    payload_fields listed;
    listed.machines = "[\"LAB-*\",\"bench-3\"]";
    licence_vector machines =
        make("machines-listed",
             "The licence lists computers. The reference reader does not "
             "check the list; Studio's library does, and is told a name "
             "that is on it.",
             licence_file(listed), mid_term, "ok", true, "valid");
    machines.computer_name = "lab-7";
    list.push_back(machines);

    payload_fields reset;
    reset.extra = "\"clock_reset\":\"reset-2027-a\",";
    list.push_back(make("clock-reset-token",
                        "The licence carries a clock_reset token. Reading "
                        "it changes no answer; the caller applies the "
                        "token to its own stored time.",
                        licence_file(reset), mid_term, "ok", true, "valid"));
}

} // namespace

std::string test_public_key_hex()
{
    const sf::public_key key =
        lic::signing::public_key_of(lic::test_keys::private_key());
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    for (const std::uint8_t byte : key)
    {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0fU]);
    }
    return text;
}

std::vector<key_list> key_lists()
{
    const std::string id(lic::test_keys::key_id);
    return {
        {"test", {{id, "", ""}}},
        {"test-from-2027", {{id, "2027-01-01T00:00:00Z", ""}}},
        {"test-until-2026", {{id, "", "2026-01-01T00:00:00Z"}}},
        {"other-id", {{"test-other-1", "", ""}}},
        {"empty", {}},
    };
}

std::vector<licence_vector> all()
{
    std::vector<licence_vector> list;
    add_terms(list);
    add_features(list);
    add_clock(list);
    add_keys(list);
    add_signatures(list);
    add_envelopes(list);
    add_payloads(list);
    return list;
}

std::string index_text()
{
    std::string text = "{\n";
    text += "  \"about\": \"Test vectors for Virtual Serial Studio licence "
            "files. See README.md beside this file. CC0-1.0.\",\n";
    text += "  \"key_note\": \"Every file is signed with the private key of "
            "RFC 8032, section 7.1, test 1, which is published. A program "
            "that ships must never trust this key.\",\n";
    text +=
        "  \"test_public_key\": " + json_text(test_public_key_hex()) + ",\n";
    text += "  \"key_lists\": {\n";
    const std::vector<key_list> lists = key_lists();
    for (std::size_t index = 0; index < lists.size(); ++index)
    {
        text += "    " + json_text(lists[index].name) + ": [";
        for (std::size_t at = 0; at < lists[index].keys.size(); ++at)
        {
            const vector_key& key = lists[index].keys[at];
            text += at == 0 ? "" : ", ";
            text += "{\"id\": " + json_text(key.id) +
                    ", \"not_before\": " + time_or_null(key.not_before) +
                    ", \"not_after\": " + time_or_null(key.not_after) + "}";
        }
        text += index + 1 == lists.size() ? "]\n" : "],\n";
    }
    text += "  },\n";
    text += "  \"vectors\": [\n";
    const std::vector<licence_vector> vectors = all();
    for (std::size_t index = 0; index < vectors.size(); ++index)
    {
        const licence_vector& vector = vectors[index];
        text += "    {\n";
        text += "      \"file\": " + json_text(vector.name + ".vsslic") + ",\n";
        text += "      \"about\": " + json_text(vector.about) + ",\n";
        text += "      \"keys\": " + json_text(vector.keys) + ",\n";
        text += "      \"now\": " + json_text(vector.now) + ",\n";
        text +=
            "      \"build_time\": " + time_or_null(vector.build_time) + ",\n";
        text += "      \"latest_seen\": " + time_or_null(vector.latest_seen) +
                ",\n";
        text += "      \"features\": [";
        for (std::size_t at = 0; at < vector.features.size(); ++at)
        {
            text += at == 0 ? "" : ", ";
            text += json_text(vector.features[at]);
        }
        text += "],\n";
        text += "      \"released\": " + time_or_null(vector.released) + ",\n";
        text +=
            "      \"computer_name\": " + time_or_null(vector.computer_name) +
            ",\n";
        text += "      \"expect\": {\"reason\": " + json_text(vector.reason) +
                ", \"granted\": " + (vector.granted ? "true" : "false") +
                ", \"licence_id\": " + time_or_null(vector.licence_id) +
                ", \"state\": " + json_text(vector.state) + "}\n";
        text += index + 1 == vectors.size() ? "    }\n" : "    },\n";
    }
    text += "  ]\n}\n";
    return text;
}

} // namespace vss_test_support::licence_vectors
