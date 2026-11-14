//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every key in this file is a THROWAWAY TEST KEY: a private key made of one
// repeated byte, written down here for anyone to read.  None of them signs
// anything outside these tests.
//

#include <vss/core/error.hpp>
#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/revocations.hpp>
#include <vss/plugin/signature.hpp>
#include <vss/plugin/verify.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace vss;
using namespace vss::plugin;
using namespace std::chrono_literals;
namespace signing = vss::license::signing;
namespace signed_file = vss::license::signed_file;

namespace
{

constexpr std::chrono::sys_days today{2026y / 11 / 14};

/// THROWAWAY TEST KEY: the private key whose 32 bytes are all `fill`.
signing::seed test_seed(std::uint8_t fill)
{
    signing::seed seed{};
    seed.fill(fill);
    return seed;
}

/// The first official key list as text, and the same list once the owner
/// has signed it, from the top of the repository.
constexpr std::string_view key_list_payload_file =
    "libs/vss_plugin/keys/official-keys.payload.json";
constexpr std::string_view signed_key_list_file =
    "libs/vss_plugin/keys/official-keys.json";

/// The bytes of a file of the repository, or nothing when it is not there.
/// The test data directory is two levels below the top.
std::string read_repository_file(std::string_view name)
{
    const std::filesystem::path path =
        std::filesystem::path(VSS_TEST_DATA_DIR) / ".." / ".." / name;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

key_bytes public_of(std::uint8_t fill)
{
    return signing::public_key_of(test_seed(fill));
}

std::string hex_of(const key_bytes& key)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : key)
    {
        out += digits[byte >> 4];
        out += digits[byte & 0x0f];
    }
    return out;
}

/// The text of an envelope for `payload`, signed with the test key `fill`.
std::string signed_text(std::string_view purpose, std::string_view key_id,
                        std::string_view payload, std::uint8_t fill)
{
    std::vector<std::uint8_t> bytes;
    for (const char c : payload)
        bytes.push_back(static_cast<std::uint8_t>(c));
    return signed_file::write(
        signing::sign(purpose, key_id, bytes, test_seed(fill)));
}

// The test keys, by the byte each is made of.
constexpr std::uint8_t root_fill = 0x01;
constexpr std::uint8_t plugins_fill = 0x02;
constexpr std::uint8_t revocations_fill = 0x03;
constexpr std::uint8_t author_fill = 0x04;
constexpr std::uint8_t stranger_fill = 0x05;

std::string key_entry(std::string_view key_id, std::uint8_t fill,
                      std::string_view purpose, std::string_view from,
                      std::string_view to)
{
    std::string text = R"({ "key_id": ")";
    text += key_id;
    text += R"(", "public_key": ")";
    text += hex_of(public_of(fill));
    text += R"(", "purpose": ")";
    text += purpose;
    text += R"(", "name": "Virtual Serial Studio", "valid_from": ")";
    text += from;
    text += R"(", "valid_to": ")";
    text += to;
    text += R"(" })";
    return text;
}

/// A key list with one key for plugins and one for revocation lists, both
/// counting from November 2026 to November 2028.
std::string key_list_payload(std::string_view revoked = "")
{
    std::string text = R"({ "key_list_version": 1, "serial": 4,
        "issued": "2026-11-13", "keys": [ )";
    text += key_entry("plugins-2026", plugins_fill, "plugin", "2026-11-01",
                      "2028-11-01");
    text += ", ";
    text += key_entry("revocations-2026", revocations_fill,
                      "plugin-revocations", "2026-11-01", "2028-11-01");
    text += R"( ], "revoked": [)";
    text += revoked;
    text += "] }";
    return text;
}

key_list official_keys(std::string_view revoked = "")
{
    const auto parsed = parse_key_list(key_list_payload(revoked));
    REQUIRE(parsed.has_value());
    return parsed.value();
}

std::vector<root_key> roots()
{
    return {root_key{"root-2026", public_of(root_fill)}};
}

void require_key_list_refused(std::string_view payload, std::string_view part)
{
    std::string detail;
    const auto parsed = parse_key_list(payload, &detail);
    REQUIRE(parsed.has_error());
    REQUIRE(parsed.error() == errc::config_error);
    REQUIRE(detail.find(part) != std::string::npos);
}

/// A plugin as the signature check sees it: its manifest bytes, its
/// manifest, and the pieces of the check's input.
struct plugin_under_test
{
    std::string manifest_bytes = "the exact bytes of plugin.json";
    manifest declared;
    key_list official = official_keys();
    std::vector<approved_signer> administrator_keys;
    std::vector<approved_signer> user_keys;
    std::optional<std::string> signature;

    plugin_under_test()
    {
        declared.id = "com.example.plugin";
        declared.version = plugin_version{1, 2, 0, {}};
    }

    /// What a correct `plugin.sig` of this plugin signs.
    std::string claim() const
    {
        std::string text = R"({ "plugin_id": ")";
        text += declared.id;
        text += R"(", "version": ")";
        text += to_string(declared.version);
        text += R"(", "manifest_sha512": ")";
        text += to_hex(sha512_of(manifest_bytes));
        text += R"(" })";
        return text;
    }

    void sign_with(std::string_view key_id, std::uint8_t fill)
    {
        signature = signed_text(plugin_purpose, key_id, claim(), fill);
    }

    signature_finding check(std::string* detail = nullptr,
                            std::chrono::sys_days day = today) const
    {
        signature_input input;
        input.manifest_bytes = manifest_bytes;
        input.declared = &declared;
        if (signature)
            input.signature_text = *signature;
        input.official = &official;
        input.administrator_keys = administrator_keys;
        input.user_keys = user_keys;
        input.today = day;
        return check_signature(input, detail);
    }
};

void require_invalid(const plugin_under_test& plugin, std::string_view part,
                     std::chrono::sys_days day = today)
{
    std::string detail;
    const signature_finding found = plugin.check(&detail, day);
    REQUIRE(found.state == signature_state::invalid);
    REQUIRE(found.key_id.empty());
    REQUIRE(found.origin == key_origin::unknown);
    REQUIRE(detail.find(part) != std::string::npos);
}

} // namespace

// -- the key list -----------------------------------------------------------

TEST_CASE("plugin signature: the owner's two root keys are built in")
{
    // The public halves the owner gave on 14 November 2026.  They are
    // written here a second time on purpose: a change to a root key has to
    // be made in two places that agree.
    const std::span<const root_key> built_in = builtin_root_keys();
    REQUIRE_EQ(built_in.size(), std::size_t{2});
    REQUIRE(built_in[0].key_id == "root-2026");
    REQUIRE_EQ(
        hex_of(built_in[0].public_key),
        "3a94dda0542ec5631e0ce810151baa831dd7f128ff9c393a68fef153026b0ca1");
    REQUIRE(built_in[1].key_id == "root-next");
    REQUIRE_EQ(
        hex_of(built_in[1].public_key),
        "648b4ce8ef09b6ce5d959d741e81e1063e73af05d3d156c5a0072b78c7503e1c");
    for (const root_key& key : built_in)
        REQUIRE(is_key_id(key.key_id));
    // No test key is a root.
    REQUIRE(built_in[0].public_key != public_of(root_fill));
    REQUIRE(built_in[1].public_key != public_of(root_fill));
}

TEST_CASE("plugin signature: the first key list names the owner's two "
          "signing keys")
{
    // The text that the owner signs, as it is in the repository.
    const std::string payload = read_repository_file(key_list_payload_file);
    std::string detail;
    const auto parsed = parse_key_list(payload, &detail);
    REQUIRE(detail.empty());
    REQUIRE(parsed.has_value());
    const key_list& list = parsed.value();
    REQUIRE_EQ(list.serial, std::uint64_t{1});
    REQUIRE_EQ(list.keys.size(), std::size_t{2});
    REQUIRE(list.revoked.empty());

    const official_key* const plugins =
        list.find("plugins-2026", plugin_purpose);
    REQUIRE(plugins != nullptr);
    REQUIRE_EQ(
        hex_of(plugins->public_key),
        "0b94598447ecf7fa42609642f82542367efab0ec27ebde097e8e1a397963cb21");
    const official_key* const revocations =
        list.find("revocations-2026", revocations_purpose);
    REQUIRE(revocations != nullptr);
    REQUIRE_EQ(
        hex_of(revocations->public_key),
        "ecdb7521bfd8c088d31d904fd6f5b89da3ed9c593415f885307795db592868b1");

    // Both count for two years from the day the list is dated.
    for (const official_key* key : {plugins, revocations})
    {
        REQUIRE(key->valid_from == list.issued);
        REQUIRE(key->valid_from == 2026y / 11 / 14);
        REQUIRE(key->valid_to == 2028y / 11 / 14);
    }
    // A signing key is not a root key, and the two differ.
    for (const root_key& root : builtin_root_keys())
    {
        REQUIRE(root.public_key != plugins->public_key);
        REQUIRE(root.public_key != revocations->public_key);
    }
    REQUIRE(plugins->public_key != revocations->public_key);
}

TEST_CASE("plugin signature: the signed key list is accepted by the built-in "
          "root")
{
    // The owner signs the list with the root private key, which is not
    // here.  Until the signed file is in the repository there is nothing to
    // check.
    const std::string file = read_repository_file(signed_key_list_file);
    if (file.empty())
        VSS_SKIP("the owner has not signed the key list yet");

    std::string detail;
    const auto signed_list = read_key_list(file, builtin_root_keys(), &detail);
    REQUIRE(detail.empty());
    REQUIRE(signed_list.has_value());
    // What was signed is the text in the repository.
    const auto payload =
        parse_key_list(read_repository_file(key_list_payload_file));
    REQUIRE(payload.has_value());
    REQUIRE(signed_list.value() == payload.value());
}

TEST_CASE("plugin signature: a key list reads and answers")
{
    std::string detail = "old";
    const auto parsed = parse_key_list(key_list_payload(), &detail);
    REQUIRE(parsed.has_value());
    REQUIRE(detail.empty());
    const key_list& list = parsed.value();
    REQUIRE_EQ(list.serial, std::uint64_t{4});
    REQUIRE(list.issued == 2026y / 11 / 13);
    REQUIRE_EQ(list.keys.size(), std::size_t{2});
    REQUIRE(list.revoked.empty());

    const official_key* const plugins =
        list.find("plugins-2026", plugin_purpose);
    REQUIRE(plugins != nullptr);
    REQUIRE(plugins->public_key == public_of(plugins_fill));
    REQUIRE_EQ(plugins->name, "Virtual Serial Studio");
    // A key is found for its own purpose only.
    REQUIRE(list.find("plugins-2026", revocations_purpose) == nullptr);
    REQUIRE(list.find("revocations-2026", revocations_purpose) != nullptr);
    REQUIRE(list.find("other", plugin_purpose) == nullptr);

    // It counts on its first and its last day, and on no day outside them.
    REQUIRE(counts_on(*plugins, std::chrono::sys_days{2026y / 11 / 1}));
    REQUIRE(counts_on(*plugins, std::chrono::sys_days{2028y / 11 / 1}));
    REQUIRE(!counts_on(*plugins, std::chrono::sys_days{2026y / 10 / 31}));
    REQUIRE(!counts_on(*plugins, std::chrono::sys_days{2028y / 11 / 2}));

    const key_list with_revoked = official_keys(R"("plugins-2025")");
    REQUIRE(with_revoked.is_revoked("plugins-2025"));
    REQUIRE(!with_revoked.is_revoked("plugins-2026"));

    const auto least = parse_key_list(
        R"({ "key_list_version": 1, "serial": 1, "issued": "2026-11-13" })");
    REQUIRE(least.has_value());
    REQUIRE(least.value().keys.empty());
}

TEST_CASE("plugin signature: a key list that is not understood is refused")
{
    const auto with_keys = [](std::string_view entries) {
        std::string text = R"({ "key_list_version": 1, "serial": 1,
            "issued": "2026-11-13", "keys": [ )";
        text += entries;
        text += " ] }";
        return text;
    };
    const std::string good = key_entry("k", plugins_fill, "plugin",
                                       "2026-11-01", "2028-11-01");
    REQUIRE(parse_key_list(with_keys(good)).has_value());

    require_key_list_refused("", "not JSON");
    require_key_list_refused("[]", "must be an object");
    require_key_list_refused(R"({ "serial": 1 })", "key_list_version");
    require_key_list_refused(
        R"({ "key_list_version": 2, "serial": 1, "issued": "2026-11-13" })",
        "key_list_version: must be 1");
    require_key_list_refused(
        R"({ "key_list_version": 1, "serial": 0, "issued": "2026-11-13" })",
        "serial");
    require_key_list_refused(R"({ "key_list_version": 1, "serial": 1 })",
                             "issued");
    require_key_list_refused(
        R"({ "key_list_version": 1, "serial": 1, "issued": "13.11.2026" })",
        "issued: must be a date");
    require_key_list_refused(
        R"({ "key_list_version": 1, "serial": 1, "issued": "2026-11-13",
             "extra": 1 })",
        "unknown key");
    require_key_list_refused(
        R"({ "key_list_version": 1, "serial": 1, "serial": 2,
             "issued": "2026-11-13" })",
        "given twice");
    require_key_list_refused(with_keys(R"("k")"), "keys: must be an object");
    require_key_list_refused(with_keys(good + ", " + good), "twice");
    // The same id under the other purpose is still the same id.
    require_key_list_refused(
        with_keys(good + ", " +
                  key_entry("k", revocations_fill, "plugin-revocations",
                            "2026-11-01", "2028-11-01")),
        "twice");
    // Only the two purposes of plugins: a licence key has no place here.
    require_key_list_refused(
        with_keys(key_entry("k", plugins_fill, "licence", "2026-11-01",
                            "2028-11-01")),
        "keys.purpose: cannot be");
    require_key_list_refused(
        with_keys(key_entry("k k", plugins_fill, "plugin", "2026-11-01",
                            "2028-11-01")),
        "keys.key_id: cannot be");
    require_key_list_refused(
        with_keys(key_entry("k", plugins_fill, "plugin", "2028-11-01",
                            "2026-11-01")),
        "keys.valid_to: is before valid_from");
    require_key_list_refused(
        with_keys(key_entry("k", plugins_fill, "plugin", "2026-11-01",
                            "soon")),
        "keys.valid_to: must be a date");
    require_key_list_refused(
        with_keys(R"({ "key_id": "k", "public_key": "abcd",
                       "purpose": "plugin", "name": "n",
                       "valid_from": "2026-11-01",
                       "valid_to": "2028-11-01" })"),
        "keys.public_key: must be 64");
    require_key_list_refused(
        with_keys(R"({ "key_id": "k", "purpose": "plugin" })"),
        "must be a string");
    require_key_list_refused(
        R"({ "key_list_version": 1, "serial": 1, "issued": "2026-11-13",
             "revoked": ["a b"] })",
        "revoked: holds something that is not a key id");
    require_key_list_refused(
        R"({ "key_list_version": 1, "serial": 1, "issued": "2026-11-13",
             "revoked": "k" })",
        "revoked: must be a list");
}

TEST_CASE("plugin signature: a key list counts only when a root signed it")
{
    const std::string payload = key_list_payload();
    const std::vector<root_key> known = roots();

    std::string detail = "old";
    const auto good = read_key_list(
        signed_text(key_list_purpose, "root-2026", payload, root_fill), known,
        &detail);
    REQUIRE(good.has_value());
    REQUIRE(detail.empty());
    REQUIRE(good.value() == official_keys());

    const auto refused = [&](const std::string& text, std::string_view part) {
        std::string why;
        const auto read = read_key_list(text, known, &why);
        REQUIRE(read.has_error());
        REQUIRE(read.error() == plugin_errc::signature_invalid);
        REQUIRE(why.find(part) != std::string::npos);
    };
    refused("not an envelope", "not a signed file");
    refused(payload, "not a signed file");
    // Signed for another purpose, by the right key.
    refused(signed_text(plugin_purpose, "root-2026", payload, root_fill),
            "not signed as a key list");
    // Signed by a key that is not a root.
    refused(signed_text(key_list_purpose, "root-2027", payload, root_fill),
            "not signed with a root key");
    // Signed under the root's name with another key.
    refused(signed_text(key_list_purpose, "root-2026", payload,
                        stranger_fill),
            "does not hold");
    // The roots built into this program are not the test root: a list
    // signed with the test key under a built-in root's id does not pass.
    {
        std::string why;
        const auto read = read_key_list(
            signed_text(key_list_purpose, "root-2026", payload, root_fill),
            builtin_root_keys(), &why);
        REQUIRE(read.has_error());
        REQUIRE(read.error() == plugin_errc::signature_invalid);
    }
    // A payload changed after it was signed.
    {
        signed_file::envelope changed = signed_file::parse(
            signed_text(key_list_purpose, "root-2026", payload, root_fill))
                                            .value();
        changed.payload.back() = static_cast<std::uint8_t>(' ');
        refused(signed_file::write(changed), "does not hold");
    }
    // A root signs something that is not a key list.
    {
        std::string why;
        const auto read = read_key_list(
            signed_text(key_list_purpose, "root-2026", "{}", root_fill),
            known, &why);
        REQUIRE(read.has_error());
        REQUIRE(read.error() == errc::config_error);
    }
}

// -- the revocation list ----------------------------------------------------

TEST_CASE("plugin signature: a revocation list counts only when an official "
          "key for it signed it")
{
    const key_list official = official_keys();
    const std::string payload =
        R"({ "revocations_version": 1, "serial": 7,
             "issued_at": "2026-11-13", "key_ids": ["plugins-2025"] })";

    std::string detail = "old";
    const auto good = read_revocations(
        signed_text(revocations_purpose, "revocations-2026", payload,
                    revocations_fill),
        official, &detail);
    REQUIRE(good.has_value());
    REQUIRE(detail.empty());
    REQUIRE_EQ(good.value().serial, std::uint64_t{7});
    REQUIRE(good.value().revokes_key("plugins-2025"));

    const auto refused = [&](const std::string& text, const key_list& keys,
                             std::string_view part) {
        std::string why;
        const auto read = read_revocations(text, keys, &why);
        REQUIRE(read.has_error());
        REQUIRE(read.error() == plugin_errc::signature_invalid);
        REQUIRE(why.find(part) != std::string::npos);
    };
    refused("nothing", official, "not a signed file");
    // The key for plugins does not sign revocation lists, and a plugin
    // signature is not a revocation list.
    refused(signed_text(revocations_purpose, "plugins-2026", payload,
                        plugins_fill),
            official, "not signed with an official key");
    refused(signed_text(plugin_purpose, "revocations-2026", payload,
                        revocations_fill),
            official, "not signed as a revocation list");
    refused(signed_text(revocations_purpose, "revocations-2026", payload,
                        stranger_fill),
            official, "does not hold");
    refused(signed_text(revocations_purpose, "someone", payload,
                        revocations_fill),
            official, "not signed with an official key");
    // A withdrawn key signs nothing more.
    refused(signed_text(revocations_purpose, "revocations-2026", payload,
                        revocations_fill),
            official_keys(R"("revocations-2026")"), "was withdrawn");
    // A list dated before the key counted.
    refused(signed_text(revocations_purpose, "revocations-2026",
                        R"({ "revocations_version": 1, "serial": 7,
                             "issued_at": "2026-10-31" })",
                        revocations_fill),
            official, "did not count on the day");
    // The right key signs something that is not a revocation list.
    {
        std::string why;
        const auto read = read_revocations(
            signed_text(revocations_purpose, "revocations-2026", "{}",
                        revocations_fill),
            official, &why);
        REQUIRE(read.has_error());
        REQUIRE(read.error() == errc::config_error);
    }
}

// -- the signing script -----------------------------------------------------

TEST_CASE("plugin signature: the signing script and this code make the same "
          "file")
{
    // tools/plugin_sign.py signs with OpenSSL and writes the envelope
    // itself.  This is what it wrote for the plugin below with the test key
    // 0x02 under the id plugins-2026; tools/test_plugin_sign.py holds the
    // same text.  It must be what this code writes for the same input, and
    // it must pass the check.
    const std::string from_script =
        R"({"format":1,"purpose":"plugin","key_id":"plugins-2026",)"
        R"("payload":"eyJwbHVnaW5faWQiOiJjb20uZXhhbXBsZS5wbHVnaW4iLCJ2ZXJza)"
        R"(W9uIjoiMS4yLjAiLCJtYW5pZmVzdF9zaGE1MTIiOiJjN2RjMzFhNWU4MDUxNGUxNW)"
        R"(QyMjljNTk5MTUxNGIxYjFmNjNiYzIwODk2NmQ4YTQxMTAwYTUwZWUyYzkxMWU0MGI)"
        R"(zYjYyYzBlZmFmYTU1MDUwNTVlNmU2MjNkY2QzNzU0NDM3NzdkN2FjNzEzOGFiNzYz)"
        R"(MTNiNTczZWExNGNjOSJ9","signature":"nYUUXb6INV8HqenR8Cga7kIe2_5gf7)"
        R"(71OxWCG3okcgVFMDbAyx0FhGbFiXEcMF9J6ecFAfwCN6dNzgMCw234AQ"})"
        "\n";

    plugin_under_test plugin;
    std::string compact = R"({"plugin_id":"com.example.plugin",)";
    compact += R"("version":"1.2.0","manifest_sha512":")";
    compact += to_hex(sha512_of(plugin.manifest_bytes));
    compact += R"("})";
    REQUIRE_EQ(signed_text(plugin_purpose, "plugins-2026", compact,
                           plugins_fill),
               from_script);

    plugin.signature = from_script;
    const signature_finding found = plugin.check();
    REQUIRE(found.state == signature_state::valid);
    REQUIRE(found.origin == key_origin::official);
    REQUIRE_EQ(found.key_id, "plugins-2026");
}

// -- the signature of a plugin ----------------------------------------------

TEST_CASE("plugin signature: a plugin without plugin.sig is not signed")
{
    const plugin_under_test plugin;
    std::string detail = "old";
    const signature_finding found = plugin.check(&detail);
    REQUIRE(found.state == signature_state::absent);
    REQUIRE(found.key_id.empty());
    REQUIRE(detail.empty());
}

TEST_CASE("plugin signature: an official key makes an official signature")
{
    plugin_under_test plugin;
    plugin.sign_with("plugins-2026", plugins_fill);
    std::string detail = "old";
    const signature_finding found = plugin.check(&detail);
    REQUIRE(found.state == signature_state::valid);
    REQUIRE(found.origin == key_origin::official);
    REQUIRE_EQ(found.key_id, "plugins-2026");
    REQUIRE(detail.empty());

    // And the run decision takes it as official.
    plugin.declared.not_after = 2027y / 11 / 1;
    plugin.declared.files = {file_entry{"bin/plugin", 1, {}}};
    plugin.declared.needs.api = api_range{api_version{1, 0}, 1, std::nullopt};
    plugin.sign_with("plugins-2026", plugins_fill);
    verify_input input;
    input.declared = &plugin.declared;
    input.signature = plugin.check();
    input.today = today;
    input.host_api = api_version{1, 0};
    input.sandbox = sandbox_level::full;
    const auto decided = verify(input);
    REQUIRE(decided.has_value());
    REQUIRE(decided.value().level == trust_level::official);
}

TEST_CASE("plugin signature: an approved key makes a third-party signature")
{
    plugin_under_test plugin;
    plugin.sign_with("acme-2026", author_fill);

    plugin.administrator_keys = {
        approved_signer{"acme-2026", public_of(author_fill)}};
    signature_finding found = plugin.check();
    REQUIRE(found.state == signature_state::valid);
    REQUIRE(found.origin == key_origin::administrator);
    REQUIRE_EQ(found.key_id, "acme-2026");

    plugin.administrator_keys.clear();
    plugin.user_keys = {approved_signer{"acme-2026", public_of(author_fill)}};
    found = plugin.check();
    REQUIRE(found.state == signature_state::valid);
    REQUIRE(found.origin == key_origin::user);

    // Both know the id: the administrator's key is the one that counts.
    plugin.administrator_keys = {
        approved_signer{"acme-2026", public_of(author_fill)}};
    found = plugin.check();
    REQUIRE(found.origin == key_origin::administrator);
}

TEST_CASE("plugin signature: a key nobody knows cannot be checked")
{
    plugin_under_test plugin;
    plugin.sign_with("stranger", stranger_fill);
    std::string detail = "old";
    const signature_finding found = plugin.check(&detail);
    REQUIRE(found.state == signature_state::unknown_key);
    REQUIRE_EQ(found.key_id, "stranger");
    REQUIRE(found.origin == key_origin::unknown);
    REQUIRE(detail.empty());

    // The same with no official key list at all.
    signature_input input;
    input.manifest_bytes = plugin.manifest_bytes;
    input.declared = &plugin.declared;
    input.signature_text = *plugin.signature;
    input.today = today;
    REQUIRE(check_signature(input).state == signature_state::unknown_key);
    plugin.sign_with("plugins-2026", plugins_fill);
    input.signature_text = *plugin.signature;
    REQUIRE(check_signature(input).state == signature_state::unknown_key);
}

TEST_CASE("plugin signature: the first place that knows the key id decides")
{
    // A user approved a key under the id of an official key.  A plugin
    // signed with the user's key does not pass: the official key has that
    // id, and the signature fails with it.
    plugin_under_test plugin;
    plugin.user_keys = {
        approved_signer{"plugins-2026", public_of(author_fill)}};
    plugin.sign_with("plugins-2026", author_fill);
    require_invalid(plugin, "does not hold");

    // The same between the administrator's keys and the user's.
    plugin_under_test other;
    other.administrator_keys = {
        approved_signer{"acme", public_of(author_fill)}};
    other.user_keys = {approved_signer{"acme", public_of(stranger_fill)}};
    other.sign_with("acme", stranger_fill);
    require_invalid(other, "does not hold");
}

TEST_CASE("plugin signature: what was signed must be this plugin")
{
    {
        plugin_under_test plugin;
        plugin.sign_with("plugins-2026", plugins_fill);
        plugin.manifest_bytes += ' ';
        require_invalid(plugin, "plugin.json is not the one that was signed");
    }
    {
        plugin_under_test plugin;
        plugin.sign_with("plugins-2026", plugins_fill);
        plugin.declared.id = "com.example.other";
        require_invalid(plugin, "another plugin");
    }
    {
        plugin_under_test plugin;
        plugin.sign_with("plugins-2026", plugins_fill);
        plugin.declared.version = plugin_version{1, 2, 1, {}};
        require_invalid(plugin, "another version");
    }
    {
        // A valid envelope of another kind: a revocation list's signature.
        plugin_under_test plugin;
        plugin.signature = signed_text(revocations_purpose, "plugins-2026",
                                       plugin.claim(), plugins_fill);
        require_invalid(plugin, "not signed as a plugin");
    }
    {
        plugin_under_test plugin;
        plugin.signature = "not an envelope";
        require_invalid(plugin, "not a signed file");
    }
    {
        // Signed, by the right key, but what is signed is something else.
        plugin_under_test plugin;
        plugin.signature =
            signed_text(plugin_purpose, "plugins-2026", "{}", plugins_fill);
        require_invalid(plugin, "not as it should be");
        plugin.signature = signed_text(
            plugin_purpose, "plugins-2026",
            R"({ "plugin_id": "com.example.plugin", "version": "1.2.0",
                 "manifest_sha512": "x", "extra": true })",
            plugins_fill);
        require_invalid(plugin, "not as it should be");
    }
    {
        // A payload changed after it was signed.
        plugin_under_test plugin;
        plugin.sign_with("plugins-2026", plugins_fill);
        signed_file::envelope changed =
            signed_file::parse(*plugin.signature).value();
        plugin.manifest_bytes = "other bytes";
        const std::string claim = plugin.claim();
        changed.payload.assign(claim.begin(), claim.end());
        plugin.signature = signed_file::write(changed);
        require_invalid(plugin, "does not hold");
    }
    {
        plugin_under_test plugin;
        plugin.sign_with("plugins-2026", plugins_fill);
        signature_input input;
        input.manifest_bytes = plugin.manifest_bytes;
        input.signature_text = *plugin.signature;
        input.official = &plugin.official;
        input.today = today;
        std::string detail;
        REQUIRE(check_signature(input, &detail).state ==
                signature_state::invalid);
        REQUIRE(!detail.empty());
    }
}

TEST_CASE("plugin signature: an official key signs only for plugins, only in "
          "its dates and only until it is withdrawn")
{
    {
        // The key for revocation lists signs a plugin.
        plugin_under_test plugin;
        plugin.sign_with("revocations-2026", revocations_fill);
        require_invalid(plugin, "not a key for plugins");
    }
    {
        plugin_under_test plugin;
        plugin.sign_with("plugins-2026", plugins_fill);
        // Its first and last day count; the days outside do not.
        REQUIRE(plugin.check(nullptr, std::chrono::sys_days{2026y / 11 / 1})
                    .state == signature_state::valid);
        REQUIRE(plugin.check(nullptr, std::chrono::sys_days{2028y / 11 / 1})
                    .state == signature_state::valid);
        require_invalid(plugin, "does not count today",
                        std::chrono::sys_days{2026y / 10 / 31});
        require_invalid(plugin, "does not count today",
                        std::chrono::sys_days{2028y / 11 / 2});
    }
    {
        plugin_under_test plugin;
        plugin.official = official_keys(R"("plugins-2026")");
        plugin.sign_with("plugins-2026", plugins_fill);
        require_invalid(plugin, "was withdrawn");
    }
    {
        // A withdrawn id stays withdrawn when a user approves a key under
        // it.
        plugin_under_test plugin;
        plugin.official = official_keys(R"("acme")");
        plugin.user_keys = {approved_signer{"acme", public_of(author_fill)}};
        plugin.sign_with("acme", author_fill);
        require_invalid(plugin, "was withdrawn");
    }
    {
        // Signed under the official key's name with another key.
        plugin_under_test plugin;
        plugin.sign_with("plugins-2026", stranger_fill);
        require_invalid(plugin, "does not hold");
    }
}
