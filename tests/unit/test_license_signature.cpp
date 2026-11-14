//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The signature check behind licence files: the Ed25519 code against the
// vectors of RFC 8032 and Project Wycheproof, and the signed envelope.
//

#include <vss/core/bytes.hpp>
#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/license/test_keys.hpp>

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <monocypher-ed25519.h>

#include "test_framework.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace vss::license;
namespace sf = vss::license::signed_file;

namespace
{

std::vector<std::uint8_t> hex(std::string_view text)
{
    const auto bytes = vss::from_hex(text);
    REQUIRE(bytes);
    return {bytes->begin(), bytes->end()};
}

template <std::size_t N>
std::array<std::uint8_t, N> hex_array(std::string_view text)
{
    const auto bytes = hex(text);
    REQUIRE_EQ(bytes.size(), N);
    std::array<std::uint8_t, N> out{};
    std::ranges::copy(bytes, out.begin());
    return out;
}

std::span<const std::uint8_t> bytes_of(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

struct rfc_vector
{
    std::string_view seed;
    std::string_view public_key;
    std::string_view message;
    std::string_view signature;
};

// RFC 8032, section 7.1, tests 1 to 3.
constexpr std::array<rfc_vector, 3> rfc_vectors{{
    {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
     "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
     "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
     "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
}};

sf::envelope signed_envelope(std::string_view purpose, std::string_view payload)
{
    return signing::sign(purpose, test_keys::key_id, bytes_of(payload),
                         test_keys::private_key());
}

} // namespace

TEST_CASE("licence signature: RFC 8032 vectors")
{
    for (const rfc_vector& vector : rfc_vectors)
    {
        auto seed = hex_array<32>(vector.seed);
        const auto public_key = hex_array<32>(vector.public_key);
        const auto message = hex(vector.message);
        const auto expected = hex_array<64>(vector.signature);

        REQUIRE(signing::public_key_of(seed) == public_key);

        std::array<std::uint8_t, 64> secret{};
        std::array<std::uint8_t, 32> derived{};
        crypto_ed25519_key_pair(secret.data(), derived.data(), seed.data());
        std::array<std::uint8_t, 64> made{};
        crypto_ed25519_sign(made.data(), secret.data(), message.data(),
                            message.size());
        REQUIRE(made == expected);
        REQUIRE_EQ(crypto_ed25519_check(expected.data(), public_key.data(),
                                        message.data(), message.size()),
                   0);

        auto wrong = expected;
        wrong[10] ^= 0x01U;
        REQUIRE_NE(crypto_ed25519_check(wrong.data(), public_key.data(),
                                        message.data(), message.size()),
                   0);
    }
}

TEST_CASE("licence signature: the test key is the key of RFC 8032 test 1")
{
    REQUIRE(test_keys::private_key() == hex_array<32>(rfc_vectors[0].seed));
    REQUIRE(test_keys::trusted()[0].key ==
            hex_array<32>(rfc_vectors[0].public_key));
    REQUIRE(signing::public_key_of(test_keys::private_key()) ==
            test_keys::trusted()[0].key);
}

TEST_CASE("licence signature: Wycheproof Ed25519 vectors")
{
    const auto text = read_file(std::filesystem::path(VSS_TEST_DATA_DIR) /
                                "wycheproof" / "ed25519_test.json");
    const boost::json::value document = boost::json::parse(text);
    int valid = 0;
    int invalid = 0;
    std::string wrong;
    for (const auto& group : document.at("testGroups").as_array())
    {
        const auto public_key =
            hex_array<32>(group.at("publicKey").at("pk").as_string().c_str());
        for (const auto& test : group.at("tests").as_array())
        {
            const std::string_view expected =
                test.at("result").as_string().c_str();
            const auto message = hex(test.at("msg").as_string().c_str());
            const auto sig = hex(test.at("sig").as_string().c_str());
            // The envelope refuses a signature that is not 64 bytes before
            // any curve arithmetic, so such a vector is rejected by size.
            bool accepted = false;
            if (sig.size() == 64)
            {
                sf::signature signature{};
                std::ranges::copy(sig, signature.begin());
                accepted = sf::verify_ed25519(signature, public_key, message);
            }
            if (expected == "valid")
            {
                ++valid;
            }
            else if (expected == "invalid")
            {
                ++invalid;
            }
            // An "acceptable" vector may go either way.
            if ((expected == "valid" && !accepted) ||
                (expected == "invalid" && accepted))
            {
                wrong += std::to_string(test.at("tcId").as_int64()) + " ";
            }
        }
    }
    // The test case ids of every vector that went the wrong way.
    REQUIRE_EQ(wrong, std::string());
    // The file has to have been read: both kinds are there in number.
    REQUIRE(valid > 50);
    REQUIRE(invalid > 50);
}

TEST_CASE("licence signature: encodings RFC 8032 does not allow are refused")
{
    // Wycheproof test 151: the first half of the signature is the point
    // with y = 1 written with the sign bit of x set. Monocypher alone
    // accepts it; the RFC does not.
    const auto key = hex_array<32>(rfc_vectors[0].public_key);
    const auto message = hex("313233343030");
    const auto sig = hex_array<64>(
        "0100000000000000000000000000000000000000000000000000000000000080"
        "c803ee1f2342aa96ff698a393d1ab5e66f3eda101d6d120b394c3fd32c117d0a");
    REQUIRE_EQ(crypto_ed25519_check(sig.data(), key.data(), message.data(),
                                    message.size()),
               0);
    REQUIRE(!sf::verify_ed25519(sig, key, message));

    // A y coordinate at or above the field prime, in the signature or in
    // the key, never reaches the arithmetic.
    const auto good_sig = hex_array<64>(rfc_vectors[0].signature);
    REQUIRE(sf::verify_ed25519(good_sig, key, {}));
    for (const std::string_view y :
         {"edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
          "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
          "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
          "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})
    {
        auto bad_sig = good_sig;
        std::ranges::copy(hex_array<32>(y), bad_sig.begin());
        REQUIRE(!sf::verify_ed25519(bad_sig, key, {}));
        REQUIRE(!sf::verify_ed25519(good_sig, hex_array<32>(y), {}));
    }
}

TEST_CASE("signed file: base64url round trip and strictness")
{
    for (std::size_t size = 0; size < 70; ++size)
    {
        std::vector<std::uint8_t> data(size);
        for (std::size_t i = 0; i < size; ++i)
        {
            data[i] = static_cast<std::uint8_t>(i * 37U + size);
        }
        const std::string text = sf::base64url_encode(data);
        REQUIRE(text.find('=') == std::string::npos);
        const auto back = sf::base64url_decode(text);
        REQUIRE(back);
        REQUIRE(*back == data);
    }

    REQUIRE_EQ(sf::base64url_encode(bytes_of("\xfb\xff")), std::string("-_8"));
    REQUIRE(sf::base64url_decode(""));

    // Padding, white space, the other alphabet, an impossible length and
    // spare bits that are not zero.
    for (const char* bad :
         {"QQ==", "QQ ", "Q\nQ", "+/8", "Q", "QUJDR", "QR", "QUF"})
    {
        const auto decoded = sf::base64url_decode(bad);
        REQUIRE(!decoded);
        REQUIRE(decoded.error() == errc::malformed);
    }
}

TEST_CASE("signed file: write and parse round trip")
{
    const sf::envelope file = signed_envelope("licence", "{\"a\":1}");
    const std::string text = sf::write(file);
    const auto parsed = sf::parse(text);
    REQUIRE(parsed);
    REQUIRE_EQ(parsed->format, sf::current_format);
    REQUIRE_EQ(parsed->purpose, std::string("licence"));
    REQUIRE_EQ(parsed->key_id, std::string(test_keys::key_id));
    REQUIRE(parsed->payload == file.payload);
    REQUIRE(parsed->sig == file.sig);
}

TEST_CASE("signed file: parse refuses what it should")
{
    const std::string good = sf::write(signed_envelope("licence", "{}"));
    REQUIRE(sf::parse(good));

    const auto expect = [](std::string_view text, errc expected)
    {
        const auto parsed = sf::parse(text);
        REQUIRE(!parsed);
        REQUIRE(parsed.error() == expected);
    };
    const auto replaced = [&good](std::string_view from, std::string_view to)
    {
        std::string text = good;
        const auto at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text;
    };

    expect(std::string(sf::max_file_size + 1, ' '), errc::too_large);
    expect("", errc::malformed);
    expect("not json", errc::malformed);
    expect("[]", errc::malformed);
    expect("{}", errc::malformed);
    expect(good + "x", errc::malformed);

    expect(replaced("\"format\":1", "\"format\":2"), errc::newer_version);
    expect(replaced("\"format\":1", "\"format\":0"), errc::malformed);
    expect(replaced("\"format\":1", "\"format\":1.0"), errc::malformed);
    expect(replaced("\"format\":1", "\"format\":\"1\""), errc::malformed);
    // A sixth field, and the same key twice.
    expect(replaced("\"format\":1", "\"format\":1,\"extra\":true"),
           errc::malformed);
    expect(replaced("\"format\":1", "\"format\":1,\"format\":1"),
           errc::malformed);
    expect(replaced("\"purpose\":\"licence\"", "\"purpose\":\"Licence\""),
           errc::malformed);
    expect(replaced("\"purpose\":\"licence\"", "\"purpose\":7"),
           errc::malformed);
    expect(replaced("\"key_id\":\"test-", "\"key_id\":\"test "),
           errc::malformed);
    // A signature one character short, and one that is not base64url.
    expect(replaced("\"signature\":\"", "\"signature\":\"="), errc::malformed);
    {
        std::string text = good;
        const auto at = text.find("\"signature\":\"") + 13;
        text.erase(at, 1);
        expect(text, errc::malformed);
    }
}

TEST_CASE("signed file: verify accepts a good file and names the key")
{
    const sf::envelope file = signed_envelope("licence", "payload");
    const auto key = sf::verify(file, "licence", test_keys::trusted());
    REQUIRE(key);
    REQUIRE_EQ((*key)->key_id, std::string(test_keys::key_id));
    REQUIRE_EQ((*key)->purpose, std::string("licence"));
}

TEST_CASE("signed file: verify refuses a changed file")
{
    const sf::envelope file = signed_envelope("licence", "payload");
    const auto expect = [](const sf::envelope& changed, errc expected)
    {
        const auto key = sf::verify(changed, "licence", test_keys::trusted());
        REQUIRE(!key);
        REQUIRE(key.error() == expected);
    };

    sf::envelope changed = file;
    changed.payload[0] ^= 0x01U;
    expect(changed, errc::bad_signature);

    changed = file;
    changed.payload.push_back(0);
    expect(changed, errc::bad_signature);

    changed = file;
    changed.sig[63] ^= 0x80U;
    expect(changed, errc::bad_signature);

    changed = file;
    changed.key_id = "lic-2026-a";
    expect(changed, errc::unknown_key);

    changed = file;
    changed.format = 2;
    expect(changed, errc::newer_version);

    const auto wrong = sf::verify(file, "revocations", test_keys::trusted());
    REQUIRE(!wrong);
    REQUIRE(wrong.error() == errc::wrong_purpose);

    const auto none = sf::verify(file, "licence", {});
    REQUIRE(!none);
    REQUIRE(none.error() == errc::unknown_key);
}

TEST_CASE("signed file: a signature for one purpose never passes as another")
{
    // The same key is trusted for both purposes here, which is the mistake
    // the purpose inside the signed message guards against.
    sf::envelope file = signed_envelope("revocations", "payload");
    REQUIRE(sf::verify(file, "revocations", test_keys::trusted()));

    file.purpose = "licence";
    const auto key = sf::verify(file, "licence", test_keys::trusted());
    REQUIRE(!key);
    REQUIRE(key.error() == errc::bad_signature);

    // The message is tag, 0, purpose, 0, payload, so a purpose cannot run
    // into the payload either.
    const auto a = sf::signed_message("ab", bytes_of("c"));
    const auto b = sf::signed_message("a", bytes_of("bc"));
    REQUIRE(a != b);
    const std::string_view tag = "VSS-SIGNED-FILE-1";
    REQUIRE(std::equal(tag.begin(), tag.end(), a.begin()));
    REQUIRE_EQ(a[tag.size()], 0);
    REQUIRE_EQ(a.size(), tag.size() + 1 + 2 + 1 + 1);
}

TEST_CASE("signed file: a key is trusted for its own purpose only")
{
    std::vector<sf::trusted_key> keys{test_keys::trusted()[0]};
    REQUIRE_EQ(keys[0].purpose, std::string("licence"));
    const sf::envelope file = signed_envelope("revocations", "payload");
    const auto key = sf::verify(file, "revocations", keys);
    REQUIRE(!key);
    REQUIRE(key.error() == errc::unknown_key);
}

TEST_CASE("signed file: key windows and revocation")
{
    using namespace std::chrono;
    const sf::time_point start{sys_days{2026y / January / 1}};
    const sf::time_point end{sys_days{2027y / January / 1}};

    sf::trusted_key key = test_keys::trusted()[0];
    key.not_before = start;
    key.not_after = end;
    REQUIRE(sf::check_key_window(key, start));
    REQUIRE(sf::check_key_window(key, end));
    REQUIRE(!sf::check_key_window(key, start - seconds{1}));
    REQUIRE(!sf::check_key_window(key, end + seconds{1}));

    // Leaked on 1 June: files signed before then keep working.
    const sf::time_point leak{sys_days{2026y / June / 1}};
    key.revoked_from = leak;
    REQUIRE(sf::check_key_window(key, leak - seconds{1}));
    REQUIRE(!sf::check_key_window(key, leak));
    const sf::envelope file = signed_envelope("licence", "payload");
    std::vector<sf::trusted_key> keys{key};
    REQUIRE(sf::verify(file, "licence", keys));

    // Revoked outright: nothing it signed is accepted.
    keys[0].revoked_from = sf::time_point::min();
    const auto refused = sf::verify(file, "licence", keys);
    REQUIRE(!refused);
    REQUIRE(refused.error() == errc::key_not_valid);
}

TEST_CASE("signed file: a product trusts no test key")
{
    // Nothing is built in until the owner adds the real public keys.
    REQUIRE(sf::builtin_keys().empty());
    REQUIRE(sf::product_trust_list().empty());

    std::vector<sf::trusted_key> keys(test_keys::trusted().begin(),
                                      test_keys::trusted().end());
    sf::trusted_key real = keys[0];
    real.key_id = "lic-2026-a";
    keys.push_back(real);
    const auto kept = sf::without_test_keys(keys);
    REQUIRE_EQ(kept.size(), std::size_t{1});
    REQUIRE_EQ(kept[0].key_id, std::string("lic-2026-a"));
}

TEST_CASE("licence errors: every code has its own message")
{
    std::vector<std::string> seen;
    for (const errc code :
         {errc::success, errc::too_large, errc::malformed, errc::newer_version,
          errc::wrong_purpose, errc::unknown_key, errc::key_not_valid,
          errc::bad_signature})
    {
        const boost::system::error_code ec = code;
        REQUIRE_EQ(std::string(ec.category().name()),
                   std::string("vss.license"));
        const std::string message = ec.message();
        REQUIRE(!message.empty());
        REQUIRE(std::ranges::find(seen, message) == seen.end());
        seen.push_back(message);
    }
}
