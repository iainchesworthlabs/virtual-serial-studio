//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reference licence reader of sdk/licence-reader, which a plugin may
// copy, against Studio's own library. The two share no code. Every test
// vector goes through both and both must give the answer the vector states;
// then licence files of many more shapes go through both, and the test fails
// where they disagree.
//

#include <vss/license/license.hpp>
#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/license/test_keys.hpp>
#include <vss/license/words.hpp>

#include "licence_reader.hpp"
#include "licence_vectors.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace lic = vss::license;
namespace sf = vss::license::signed_file;
namespace reader = vss_licence_reader;
namespace vectors = vss_test_support::licence_vectors;
using vectors::licence_vector;

/// What either reader said, in the words of the index.
struct verdict
{
    std::string reason;
    bool granted = false;
    std::string licence_id;
    std::string state;
    int days_left = 0;

    friend bool operator==(const verdict&, const verdict&) = default;
};

std::ostream& operator<<(std::ostream& out, const verdict& value)
{
    return out << value.reason << (value.granted ? " granted " : " refused ")
               << '[' << value.licence_id << "] " << value.state << ' '
               << value.days_left;
}

std::string_view reason_name(lic::reason value)
{
    switch (value)
    {
    case lic::reason::ok:
        return "ok";
    case lic::reason::no_licence:
        return "no_licence";
    case lic::reason::expired:
        return "expired";
    case lic::reason::not_yet_valid:
        return "not_yet_valid";
    case lic::reason::clock_suspect:
        return "clock_suspect";
    case lic::reason::malformed:
        return "malformed";
    case lic::reason::newer_version:
        return "newer_version";
    case lic::reason::bad_signature:
        return "bad_signature";
    case lic::reason::revoked:
        return "revoked";
    case lic::reason::wrong_computer:
        return "wrong_computer";
    case lic::reason::not_in_licence:
        return "not_in_licence";
    }
    return "unknown";
}

std::string_view state_name(reader::state value)
{
    switch (value)
    {
    case reader::state::valid:
        return "valid";
    case reader::state::expiring:
        return "expiring";
    case reader::state::grace:
        return "grace";
    case reader::state::expired:
        return "expired";
    case reader::state::not_yet_valid:
        return "not_yet_valid";
    case reader::state::clock_suspect:
        return "clock_suspect";
    case reader::state::invalid:
        return "invalid";
    }
    return "unknown";
}

lic::time_point studio_time(std::string_view text)
{
    return *lic::parse_time(text);
}

reader::seconds reader_time(std::string_view text)
{
    reader::seconds value = 0;
    REQUIRE(reader::parse_time(text, value));
    return value;
}

/// What a caller knows, in the form the vectors use: times as text, empty
/// for none.
struct setting
{
    std::string keys = "test";
    std::string now = "2027-01-01T00:00:00Z";
    std::string build_time;
    std::string latest_seen;
    std::vector<std::string> features{"plugins.official"};
    std::string released;
    std::string computer_name;
};

setting setting_of(const licence_vector& vector)
{
    setting where;
    where.keys = vector.keys;
    where.now = vector.now;
    where.build_time = vector.build_time;
    where.latest_seen = vector.latest_seen;
    where.features = vector.features;
    where.released = vector.released;
    where.computer_name = vector.computer_name;
    return where;
}

const vectors::key_list& key_list_named(const std::string& name)
{
    static const std::vector<vectors::key_list> lists = vectors::key_lists();
    const auto found = std::ranges::find(lists, name, &vectors::key_list::name);
    REQUIRE(found != lists.end());
    return *found;
}

sf::public_key test_public_key()
{
    return lic::signing::public_key_of(lic::test_keys::private_key());
}

/// The answer of Studio's own library.
verdict studio_verdict(std::string_view file_text, const setting& where)
{
    std::vector<sf::trusted_key> keys;
    for (const vectors::vector_key& key : key_list_named(where.keys).keys)
    {
        sf::trusted_key trusted;
        trusted.key_id = key.id;
        trusted.purpose = std::string(lic::licence_purpose);
        trusted.key = test_public_key();
        if (!key.not_before.empty())
        {
            trusted.not_before = studio_time(key.not_before);
        }
        if (!key.not_after.empty())
        {
            trusted.not_after = studio_time(key.not_after);
        }
        keys.push_back(trusted);
    }

    lic::context context;
    context.now = studio_time(where.now);
    if (!where.build_time.empty())
    {
        context.build_time = studio_time(where.build_time);
    }
    if (!where.latest_seen.empty())
    {
        context.latest_seen = studio_time(where.latest_seen);
    }
    context.computer_name = where.computer_name;
    context.keys = keys;
    const lic::status status = lic::evaluate(file_text, context);

    verdict answer;
    answer.reason = std::string(reason_name(status.reason));
    answer.state = std::string(lic::state_name(status.state));
    answer.days_left = status.days_left;
    if (status.value)
    {
        answer.licence_id = status.value->licence_id;
    }
    // The licence grants the answer when it names any one of the features.
    for (const std::string& feature : where.features)
    {
        const lic::entitlement entitlement =
            where.released.empty()
                ? lic::check(status, feature)
                : lic::check(status, feature, studio_time(where.released));
        if (entitlement.granted)
        {
            answer.granted = true;
            answer.reason = "ok";
            break;
        }
        answer.reason = std::string(reason_name(entitlement.reason));
    }
    return answer;
}

/// The answer of the reference reader.
verdict reader_verdict(std::string_view file_text, const setting& where)
{
    reader::question asked;
    for (const vectors::vector_key& key : key_list_named(where.keys).keys)
    {
        reader::trusted_key trusted;
        trusted.id = key.id;
        trusted.public_key = test_public_key();
        if (!key.not_before.empty())
        {
            trusted.not_before = reader_time(key.not_before);
        }
        if (!key.not_after.empty())
        {
            trusted.not_after = reader_time(key.not_after);
        }
        asked.keys.push_back(trusted);
    }
    asked.now = reader_time(where.now);
    if (!where.build_time.empty())
    {
        asked.build_time = reader_time(where.build_time);
    }
    if (!where.latest_seen.empty())
    {
        asked.latest_seen = reader_time(where.latest_seen);
    }
    asked.features = where.features;
    if (!where.released.empty())
    {
        asked.has_released = true;
        asked.released = reader_time(where.released);
    }
    const reader::answer result = reader::read_licence(file_text, asked);

    verdict answer;
    answer.reason = result.reason;
    answer.granted = result.granted;
    answer.licence_id = result.licence_id;
    answer.state = std::string(state_name(result.licence_state));
    answer.days_left = result.days_left;
    return answer;
}

/// Both readers' answers for one file, which must be the same. Says which
/// file it was when they are not.
verdict both(std::string_view what, std::string_view file_text,
             const setting& where)
{
    const verdict studio = studio_verdict(file_text, where);
    const verdict reference = reader_verdict(file_text, where);
    if (!(studio == reference))
    {
        std::cerr << "the readers disagree on " << what << ": Studio says "
                  << studio << ", the reference reader says " << reference
                  << '\n';
    }
    REQUIRE(studio == reference);
    return reference;
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

std::string signed_licence(std::string_view payload)
{
    const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    return sf::write(lic::signing::sign(lic::licence_purpose,
                                        lic::test_keys::key_id, bytes,
                                        lic::test_keys::private_key()));
}

/// A payload both readers accept, with room for one more member and for
/// other text in place of a member's value.
std::string payload_with(
    std::string_view extra, std::string_view grace = "28",
    std::string_view licensee = "{\"name\":\"Test vector, not a customer\"}")
{
    std::string text =
        "{\"licence_id\":\"VSS-TEST-000002\",\"kind\":\"annual\","
        "\"edition\":\"pro\",\"licensee\":";
    text += licensee;
    text += ",\"issued_at\":\"2026-11-01T00:00:00Z\","
            "\"starts_at\":\"2026-11-01T00:00:00Z\","
            "\"expires_at\":\"2027-11-01T00:00:00Z\",\"grace_days\":";
    text += grace;
    text += ",\"seats\":1,\"features\":[\"plugins.official\"],";
    text += extra;
    text += "\"critical\":[]}";
    return text;
}

} // namespace

TEST_CASE("licence vectors: the files are what the generator makes")
{
    // The files a plugin author downloads and the answers this repository
    // tests are the same thing only while this holds. To write the files
    // again: vss_licence_vectors sdk/licence-reader/vectors
    const std::filesystem::path folder = VSS_LICENCE_VECTORS_DIR;
    const std::vector<licence_vector> all = vectors::all();
    REQUIRE(all.size() >= 60);
    for (const licence_vector& vector : all)
    {
        const std::filesystem::path file = folder / (vector.name + ".vsslic");
        if (!std::filesystem::exists(file) ||
            read_file(file) != vector.file_text)
        {
            std::cerr << "not what the generator makes: " << file.string()
                      << '\n';
        }
        REQUIRE(std::filesystem::exists(file));
        REQUIRE(read_file(file) == vector.file_text);
    }
    REQUIRE(read_file(folder / "index.json") == vectors::index_text());

    // No file is left over from a vector that was removed, and no name is
    // used twice.
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
    {
        if (entry.path().extension() == ".vsslic")
        {
            ++files;
        }
    }
    REQUIRE_EQ(files, all.size());
}

TEST_CASE("licence vectors: both readers give the answer each vector states")
{
    for (const licence_vector& vector : vectors::all())
    {
        const setting where = setting_of(vector);
        const verdict studio = studio_verdict(vector.file_text, where);
        const verdict reference = reader_verdict(vector.file_text, where);
        for (const verdict* answer : {&studio, &reference})
        {
            const bool as_stated = answer->reason == vector.reason &&
                                   answer->granted == vector.granted &&
                                   answer->licence_id == vector.licence_id &&
                                   answer->state == vector.state;
            if (!as_stated)
            {
                std::cerr << vector.name << ": "
                          << (answer == &studio ? "Studio's library"
                                                : "the reference reader")
                          << " says " << *answer << ", the vector states "
                          << vector.reason << '\n';
            }
            REQUIRE(as_stated);
        }
        REQUIRE(studio == reference);
    }
}

TEST_CASE("licence vectors: only the test key signs them")
{
    // A vector that verifies under any key list verifies under the test
    // key, whose private half is published; the test key's id says so.
    REQUIRE(lic::test_keys::key_id.starts_with(sf::test_key_prefix));
    REQUIRE_EQ(vectors::test_public_key_hex(),
               std::string("d75a980182b10ab7d54bfed3c964073a"
                           "0ee172f3daa62325af021a68f707511a"));
    for (const vectors::key_list& list : vectors::key_lists())
    {
        for (const vectors::vector_key& key : list.keys)
        {
            REQUIRE(key.id.starts_with(sf::test_key_prefix));
        }
    }
    // And the keys a product trusts refuse every one of them.
    const std::vector<sf::trusted_key> product = sf::product_trust_list();
    for (const licence_vector& vector : vectors::all())
    {
        lic::context context;
        context.now = studio_time(vector.now);
        context.keys = product;
        REQUIRE(!lic::evaluate(vector.file_text, context).active());
    }
}

TEST_CASE("licence reader: agrees with Studio's library on payloads of many "
          "shapes")
{
    const std::vector<std::string> payloads = {
        payload_with(""),
        // Numbers, in the forms JSON allows.
        payload_with("", "0"),
        payload_with("", "366"),
        payload_with("", "367"),
        payload_with("", "-1"),
        payload_with("", "-0"),
        payload_with("", "0.0"),
        payload_with("", "0e0"),
        payload_with("", "1e"),
        payload_with("", "00"),
        payload_with("", "28.0"),
        payload_with("", "2.8e1"),
        payload_with("", "28e0"),
        payload_with("", "028"),
        payload_with("", "+28"),
        payload_with("", "\"28\""),
        payload_with("", "null"),
        payload_with("", "true"),
        payload_with("", "99999999999999999999999999"),
        payload_with("", "9223372036854775807"),
        payload_with("", "9223372036854775808"),
        payload_with("", "-9223372036854775808"),
        payload_with("", "-9223372036854775809"),
        payload_with("", "18446744073709551615"),
        payload_with("\"later\":-0.5e+3,"),
        payload_with("\"later\":1E400,"),
        payload_with("\"later\":1.,"),
        payload_with("\"later\":.5,"),
        payload_with("\"later\":0x10,"),
        payload_with("\"later\":-,"),
        payload_with("\"later\":NaN,"),
        // Text and escapes.
        payload_with("\"later\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\","),
        payload_with("\"later\":\"\\u00e9\\u4e2d\\ud83d\\ude00\","),
        payload_with("\"later\":\"\\ud83d\","),
        payload_with("\"later\":\"\\ude00\","),
        payload_with("\"later\":\"\\ud83d\\u0041\","),
        payload_with("\"later\":\"\\u12g4\","),
        payload_with("\"later\":\"\\x41\","),
        payload_with("\"later\":\"\\u0000\","),
        payload_with("\"later\":\"\xc3\xa9 \xe4\xb8\xad \xf0\x9f\x98\x80\","),
        payload_with("\"later\":\"\xc0\xaf\","),
        payload_with("\"later\":\"\xe0\x80\xaf\","),
        payload_with("\"later\":\"\xed\xa0\x80\","),
        payload_with("\"later\":\"\xf4\x90\x80\x80\","),
        payload_with("\"later\":\"\xf8\x88\x80\x80\x80\","),
        payload_with("\"later\":\"\xc3\","),
        payload_with("\"later\":\"\x80\","),
        payload_with("\"later\":\"tab\there\","),
        payload_with("\"later\":\"line\nbreak\","),
        payload_with("\"later\":'single',"),
        // Names.
        payload_with("\"\":1,"),
        payload_with("\"seats\":2,"),
        payload_with("\"se\\u0061ts\":2,"),
        payload_with("\"later\":{\"a\":1,\"b\":{\"a\":2}},"),
        payload_with("\"later\":{\"a\":1,\"\\u0061\":2},"),
        payload_with("later:1,"),
        // Lists, objects and what may follow them.
        payload_with("\"later\":[],\"more\":{},"),
        payload_with("\"later\":[1,],"),
        payload_with("\"later\":[,1],"),
        payload_with("\"later\":{\"a\":1,},"),
        payload_with("\"later\":[1 2],"),
        payload_with("\"later\":{\"a\" 1},"),
        payload_with("\"later\":[[[[[[[1]]]]]]],"),
        payload_with("\"later\":[[[[[[[[1]]]]]]]],"),
        payload_with("\"later\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":"
                     "{\"a\":1}}}}}}},"),
        payload_with("\"later\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":"
                     "{\"a\":{\"a\":1}}}}}}}},"),
        payload_with("\"later\": \t\r\n[ 1 , 2 ] ,"),
        payload_with("\"later\":1, // a comment\n"),
        payload_with("\"later\":1, /* a comment */"),
        payload_with("\"later\":True,"),
        payload_with("\"later\":nul,"),
        // The licensee.
        payload_with("", "28", "{\"name\":\"\"}"),
        payload_with("", "28", "{}"),
        payload_with("", "28", "\"a name\""),
        payload_with("", "28", "null"),
        payload_with("", "28", "{\"name\":\"A\",\"contact\":null}"),
        payload_with("", "28", "{\"name\":\"A\",\"contact\":\"\"}"),
        payload_with("", "28", "{\"name\":\"A\",\"contact\":7}"),
        payload_with("", "28", "{\"name\":\"A\",\"email\":\"a@b.co\"}"),
        payload_with("", "28", "{\"name\":\"A\",\"email\":\"a@b\"}"),
        payload_with("", "28", "{\"name\":\"A\",\"email\":\"a@@b.co\"}"),
        payload_with("", "28", "{\"name\":\"A\",\"email\":\"a@b.co.\"}"),
        payload_with("", "28", "{\"name\":\"A\",\"email\":\"a b@c.co\"}"),
        payload_with("", "28", "{\"name\":\"A\",\"email\":\"o'b+x@c-d.co\"}"),
        payload_with("", "28", "{\"name\":\"A\",\"email\":\"a@-c.co\"}"),
        payload_with("", "28", "{\"name\":\"A\",\"email\":\"a@c..co\"}"),
        payload_with("", "28", "{\"name\":\"A\\u007fB\"}"),
        payload_with("", "28", "{\"name\":\"" + std::string(200, 'n') + "\"}"),
        payload_with("", "28", "{\"name\":\"" + std::string(201, 'n') + "\"}"),
        // The optional members.
        payload_with("\"updates_until\":\"2027-11-01T00:00:00Z\","),
        payload_with("\"updates_until\":\"\","),
        payload_with("\"updates_until\":\"2027-13-01T00:00:00Z\","),
        payload_with("\"updates_until\":20271101,"),
        payload_with("\"replaces\":\"VSS-TEST-000001\","),
        payload_with("\"replaces\":\"vss-test-000001\","),
        payload_with("\"clock_reset\":\"reset-a\","),
        payload_with("\"clock_reset\":\"Reset A\","),
        payload_with("\"machines\":[],"),
        payload_with("\"machines\":[\"LAB-*\",\"a_b.c\"],"),
        payload_with("\"machines\":[\"*\"],"),
        payload_with("\"machines\":[\"two words\"],"),
        payload_with("\"machines\":\"LAB-1\","),
        payload_with("\"machines\":null,"),
        payload_with("\"machines\":[\"" + std::string(64, 'm') +
                     "*\",\"lab-7\"],"),
        payload_with("\"machines\":[\"" + std::string(65, 'm') + "*\"],"),
        // `critical`.
        payload_with("\"later\":1,")
            .replace(payload_with("\"later\":1,").find("\"critical\":[]"), 13,
                     "\"critical\":[\"later\"]"),
        payload_with("\"later\":1,")
            .replace(payload_with("\"later\":1,").find("\"critical\":[]"), 13,
                     "\"critical\":[\"\"]"),
        payload_with("\"later\":1,")
            .replace(payload_with("\"later\":1,").find("\"critical\":[]"), 13,
                     "\"critical\":\"seats\""),
        payload_with("\"later\":1,")
            .replace(payload_with("\"later\":1,").find("\"critical\":[]"), 13,
                     "\"critical\":[7]"),
        payload_with("\"later\":1,")
            .replace(payload_with("\"later\":1,").find(",\"critical\":[]"), 14,
                     ""),
        // Not an object at all, or more than one value.
        "",
        " ",
        "null",
        "[]",
        "\"text\"",
        "{}",
        payload_with("") + " ",
        payload_with("") + "\n\t ",
        payload_with("") + "{}",
        payload_with("") + ",",
        "\xef\xbb\xbf" + payload_with(""),
        " " + payload_with(""),
    };

    std::size_t accepted = 0;
    for (const std::string& payload : payloads)
    {
        setting where;
        // A name on the list of the payloads that list computers.
        where.computer_name = "lab-7";
        const verdict answer = both(payload, signed_licence(payload), where);
        if (answer.reason == "ok")
        {
            ++accepted;
        }
    }
    // Both kinds are in the list: some are read and some are refused.
    REQUIRE(accepted >= 20);
    REQUIRE(accepted + 40 <= payloads.size());
}

TEST_CASE("licence reader: agrees with Studio's library on feature names")
{
    const std::vector<std::string> entries = {
        "plugins.official",
        "plugins.*",
        "plugins.a-b.c9",
        "a",
        "a.*",
        ".*",
        "*",
        "plugins.",
        ".plugins",
        "plugins..x",
        "Plugins.x",
        "plugins.x.*.*",
        "plugins.x y",
        std::string(136, 'f'),
        std::string(137, 'f'),
        std::string(136, 'f') + ".*",
        std::string(137, 'f') + ".*",
        "",
    };
    const std::vector<std::string> asked = {
        "plugins.official",
        "plugins",
        "plugins.a-b.c9",
        "pluginsx",
        "a",
        "a.b",
        "a.b.c",
        std::string(136, 'f'),
        "plugins.x",
        "support",
    };
    for (const std::string& entry : entries)
    {
        const std::string payload = payload_with("").replace(
            payload_with("").find("plugins.official"), 16, entry);
        for (const std::string& feature : asked)
        {
            setting where;
            where.features = {feature};
            std::string what = entry;
            what += " asked for ";
            what += feature;
            both(what, signed_licence(payload), where);
            REQUIRE_EQ(reader::feature_matches(entry, feature),
                       lic::feature_matches(entry, feature));
        }
    }
}

TEST_CASE("licence reader: agrees with Studio's library on every small "
          "change to a good file")
{
    const std::string good = signed_licence(payload_with(""));
    const setting where;
    REQUIRE_EQ(both("the good file", good, where).reason, std::string("ok"));

    // Each byte in turn: one bit flipped, then a few other bytes in its
    // place, then the byte removed. Nearly all are refused; what matters is
    // that the two readers refuse the same ones for the same reason.
    std::size_t refused = 0;
    for (std::size_t at = 0; at < good.size(); ++at)
    {
        for (const char other : {static_cast<char>(good[at] ^ 0x01), '"', '\\',
                                 ' ', '0', 'A', '-', '\0', '\x80'})
        {
            std::string changed = good;
            changed[at] = other;
            const verdict answer =
                both("a changed byte at " + std::to_string(at), changed, where);
            if (answer.reason != "ok")
            {
                ++refused;
            }
        }
        std::string shorter = good;
        shorter.erase(at, 1);
        both("a byte removed at " + std::to_string(at), shorter, where);
    }
    REQUIRE(refused > good.size() * 8);
}

TEST_CASE("licence reader: agrees with Studio's library on envelopes")
{
    const std::string payload = payload_with("");
    const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    const sf::envelope envelope =
        lic::signing::sign(lic::licence_purpose, lic::test_keys::key_id, bytes,
                           lic::test_keys::private_key());
    const std::string body = sf::base64url_encode(envelope.payload);
    const std::string signature = sf::base64url_encode(envelope.sig);
    const auto file = [&](std::string_view format, std::string_view purpose,
                          std::string_view key_id, std::string_view extra)
    {
        std::string text = "{\"format\":";
        text += format;
        text += ",\"purpose\":";
        text += purpose;
        text += ",\"key_id\":";
        text += key_id;
        text +=
            ",\"payload\":\"" + body + "\",\"signature\":\"" + signature + "\"";
        text += extra;
        text += '}';
        return text;
    };
    const std::string licence = "\"licence\"";
    const std::string key = "\"test-rfc8032-1\"";
    const std::vector<std::string> files = {
        file("1", licence, key, ""),
        file("0", licence, key, ""),
        file("2", licence, key, ""),
        file("1000", licence, key, ""),
        file("1001", licence, key, ""),
        file("-1", licence, key, ""),
        file("1.0", licence, key, ""),
        file("1e0", licence, key, ""),
        file("\"1\"", licence, key, ""),
        file("null", licence, key, ""),
        file("2", "7", key, ""),
        file("2", licence, key, ",\"more\":1"),
        file("1", licence, key, ",\"more\":1"),
        file("1", "\"\"", key, ""),
        file("1", "\"Licence\"", key, ""),
        file("1", "\"licence \"", key, ""),
        file("1", "\"revocations\"", key, ""),
        file("1", "\"plugin\"", key, ""),
        file("1", "\"" + std::string(32, 'p') + "\"", key, ""),
        file("1", "\"" + std::string(33, 'p') + "\"", key, ""),
        file("1", "7", key, ""),
        file("1", licence, "\"\"", ""),
        file("1", licence, "\"Test-RFC8032-1\"", ""),
        file("1", licence, "\"test_rfc8032_1\"", ""),
        file("1", licence, "\"test-other-1\"", ""),
        file("1", licence, "\"" + std::string(64, 'k') + "\"", ""),
        file("1", licence, "\"" + std::string(65, 'k') + "\"", ""),
        file("1", licence, "null", ""),
        "[" + file("1", licence, key, "") + "]",
        file("1", licence, key, "") + "\r\n",
        "\r\n\t " + file("1", licence, key, ""),
    };
    for (const std::string& text : files)
    {
        both(text.substr(0, 70), text, setting{});
    }

    // The payload and the signature as text: what base64url allows.
    for (const std::string_view ending :
         {"", "=", "==", "A", "AA", "AAA", "AAAA", "B", "_", "-", "+", "/", " ",
          "\\n"})
    {
        const std::string start =
            "{\"format\":1,\"purpose\":\"licence\","
            "\"key_id\":\"test-rfc8032-1\",\"payload\":\"";
        std::string longer_payload = start;
        longer_payload += body;
        longer_payload += ending;
        longer_payload += "\",\"signature\":\"";
        longer_payload += signature;
        longer_payload += "\"}";
        std::string what = "a payload ending in ";
        what += ending;
        both(what, longer_payload, setting{});

        std::string other_signature = start;
        other_signature += body;
        other_signature += "\",\"signature\":\"";
        other_signature += signature.substr(0, signature.size() - 1);
        other_signature += ending;
        other_signature += "\"}";
        what = "a signature ending in ";
        what += ending;
        both(what, other_signature, setting{});
    }
}

TEST_CASE("licence reader: agrees with Studio's library at every time of a "
          "licence's life")
{
    const std::string file = signed_licence(payload_with(""));
    // From before it was signed to after its grace, in steps that do not
    // divide a day, with and without the lower bounds for the clock.
    const reader::seconds first = reader_time("2026-10-25T00:00:00Z");
    const reader::seconds last = reader_time("2027-12-05T00:00:00Z");
    std::size_t states = 0;
    std::string previous;
    for (reader::seconds now = first; now <= last; now += 23 * 3600 + 13)
    {
        const lic::time_point moment{std::chrono::seconds{now}};
        setting where;
        where.now = lic::format_time(moment);
        const verdict plain = both("the time " + where.now, file, where);
        if (plain.state != previous)
        {
            ++states;
        }
        previous = plain.state;

        where.latest_seen = "2027-03-01T00:00:00Z";
        both("the time " + where.now + " with a latest time seen", file, where);
        where.latest_seen.clear();
        where.build_time = "2027-06-01T12:00:00Z";
        both("the time " + where.now + " with a build time", file, where);
    }
    // clock_suspect, not_yet_valid (inside the 48 hours before it was
    // signed), valid, expiring, grace, expired.
    REQUIRE_EQ(states, std::size_t{6});
}

TEST_CASE("licence reader: reads the same times as Studio's library")
{
    // Every day from 2000 to 2104, which covers the leap years and the
    // year 2100 that is not one.
    using days = std::chrono::days;
    const std::chrono::sys_days start =
        std::chrono::year{2000} / std::chrono::January / 1;
    const std::chrono::sys_days end =
        std::chrono::year{2104} / std::chrono::December / 31;
    for (std::chrono::sys_days day = start; day <= end; day += days{1})
    {
        const lic::time_point moment =
            lic::time_point{day} + std::chrono::seconds{86399};
        const std::string text = lic::format_time(moment);
        reader::seconds value = 0;
        REQUIRE(reader::parse_time(text, value));
        REQUIRE_EQ(value, moment.time_since_epoch().count());
    }

    for (const std::string_view text :
         {"2026-11-13T02:10:00Z",  "1999-12-31T23:59:59Z",
          "2000-01-01T00:00:00Z",  "9999-12-31T23:59:59Z",
          "2026-02-29T00:00:00Z",  "2028-02-29T00:00:00Z",
          "2100-02-29T00:00:00Z",  "2400-02-29T00:00:00Z",
          "2026-00-10T00:00:00Z",  "2026-13-10T00:00:00Z",
          "2026-04-31T00:00:00Z",  "2026-04-00T00:00:00Z",
          "2026-11-13T24:00:00Z",  "2026-11-13T23:60:00Z",
          "2026-11-13T23:59:60Z",  "2026-11-13 02:10:00Z",
          "2026-11-13T02:10:00",   "2026-11-13T02:10:00z",
          "2026-11-13T02:10:00+0", "2026-11-13t02:10:00Z",
          "2026/11/13T02:10:00Z",  "2026-11-13T02.10.00Z",
          "20261113T021000Z",      " 2026-11-13T02:10:00Z",
          "2026-11-13T02:10:00Z ", "2O26-11-13T02:10:00Z",
          "-026-11-13T02:10:00Z",  ""})
    {
        const auto studio = lic::parse_time(text);
        reader::seconds value = 0;
        const bool read = reader::parse_time(text, value);
        REQUIRE_EQ(read, studio.has_value());
        if (studio)
        {
            REQUIRE_EQ(value, studio->time_since_epoch().count());
        }
    }
}

TEST_CASE("licence reader: keeps its stored clock as Studio's library does")
{
    const std::string with_token =
        signed_licence(payload_with("\"clock_reset\":\"reset-a\","));
    const std::string without = signed_licence(payload_with(""));

    reader::question asked;
    reader::trusted_key key;
    key.id = std::string(lic::test_keys::key_id);
    key.public_key = test_public_key();
    asked.keys.push_back(key);
    asked.now = reader_time("2027-01-01T00:00:00Z");
    const reader::answer token = reader::read_licence(with_token, asked);
    const reader::answer plain = reader::read_licence(without, asked);
    REQUIRE(token.verified);
    REQUIRE_EQ(token.clock_reset, std::string("reset-a"));
    REQUIRE(plain.clock_reset.empty());
    REQUIRE_EQ(token.key_id, std::string(lic::test_keys::key_id));
    REQUIRE_EQ(token.issued_at, reader_time("2026-11-01T00:00:00Z"));
    REQUIRE_EQ(token.starts_at, reader_time("2026-11-01T00:00:00Z"));
    REQUIRE_EQ(token.expires_at, reader_time("2027-11-01T00:00:00Z"));
    REQUIRE_EQ(token.grace_ends, reader_time("2027-11-29T00:00:00Z"));

    // The latest time only moves forward.
    reader::clock_state stored;
    REQUIRE_EQ(stored.latest_seen, reader::no_time);
    stored = reader::observe(stored, reader_time("2027-06-01T00:00:00Z"));
    stored = reader::observe(stored, reader_time("2027-05-01T00:00:00Z"));
    REQUIRE_EQ(stored.latest_seen, reader_time("2027-06-01T00:00:00Z"));

    // A licence without a token changes nothing; one with a token sets the
    // time back to its issued_at, once.
    stored = reader::apply_reset(stored, plain);
    REQUIRE_EQ(stored.latest_seen, reader_time("2027-06-01T00:00:00Z"));
    stored = reader::apply_reset(stored, token);
    REQUIRE_EQ(stored.latest_seen, token.issued_at);
    REQUIRE_EQ(stored.applied_resets.size(), std::size_t{1});
    stored = reader::observe(stored, reader_time("2027-06-01T00:00:00Z"));
    stored = reader::apply_reset(stored, token);
    REQUIRE_EQ(stored.latest_seen, reader_time("2027-06-01T00:00:00Z"));

    // A file that did not verify resets nothing, whatever it says.
    reader::answer forged = token;
    forged.verified = false;
    forged.clock_reset = "reset-b";
    stored = reader::apply_reset(stored, forged);
    REQUIRE_EQ(stored.applied_resets.size(), std::size_t{1});

    // The same steps in Studio's library end in the same state.
    lic::clock_state studio;
    studio = lic::observe(studio, studio_time("2027-06-01T00:00:00Z"));
    lic::licence imported;
    imported.clock_reset = "reset-a";
    imported.issued_at = studio_time("2026-11-01T00:00:00Z");
    studio = lic::apply_reset(studio, imported);
    REQUIRE_EQ(studio.latest_seen.time_since_epoch().count(), token.issued_at);
    REQUIRE(studio.applied_resets == stored.applied_resets);

    // Only the last 32 tokens are kept.
    for (int index = 0; index < 40; ++index)
    {
        reader::answer next = token;
        next.clock_reset = "reset-" + std::to_string(index);
        stored = reader::apply_reset(stored, next);
    }
    REQUIRE_EQ(stored.applied_resets.size(), std::size_t{32});
    REQUIRE_EQ(stored.applied_resets.back(), std::string("reset-39"));
}

TEST_CASE("licence reader: with no feature named it says whether the licence "
          "is in force")
{
    const std::string file = signed_licence(payload_with(""));
    reader::question asked;
    reader::trusted_key key;
    key.id = std::string(lic::test_keys::key_id);
    key.public_key = test_public_key();
    asked.keys.push_back(key);

    asked.now = reader_time("2027-01-01T00:00:00Z");
    reader::answer answer = reader::read_licence(file, asked);
    REQUIRE(answer.granted);
    REQUIRE_EQ(answer.reason, std::string("ok"));
    REQUIRE_EQ(answer.days_left, 304);

    asked.now = reader_time("2028-01-01T00:00:00Z");
    answer = reader::read_licence(file, asked);
    REQUIRE(!answer.granted);
    REQUIRE_EQ(answer.reason, std::string("expired"));

    // The public key is checked for its form too: one that is not written
    // the one canonical way verifies nothing.
    asked.now = reader_time("2027-01-01T00:00:00Z");
    asked.keys.front().public_key.fill(0xff);
    answer = reader::read_licence(file, asked);
    REQUIRE_EQ(answer.reason, std::string("bad_signature"));
    REQUIRE(answer.licence_id.empty());
}
