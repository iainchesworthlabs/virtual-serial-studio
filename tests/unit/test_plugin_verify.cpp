//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/revocations.hpp>
#include <vss/plugin/verify.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vss;
using namespace vss::plugin;
using namespace std::chrono_literals;

namespace
{

constexpr std::chrono::sys_days today{2026y / 11 / 13};

file_entry file(std::string path, std::string_view content)
{
    file_entry entry;
    entry.path = std::move(path);
    entry.size = content.size();
    entry.sha512 = sha512_of(content);
    return entry;
}

/// A manifest of version 1.2.0 that lists its files and asks for one
/// permission it needs and one it would like.
manifest good_manifest()
{
    manifest value;
    value.id = "com.example.plugin";
    value.name = "Example";
    value.version = plugin_version{1, 2, 0, {}};
    value.needs.api = api_range{api_version{1, 0}, 1, std::nullopt};
    value.run.executable = "bin/plugin";
    value.files = {file("bin/plugin", "program")};
    permission_request needed;
    needed.name = permission::ports_read;
    permission_request wanted;
    wanted.name = permission::net_outbound;
    wanted.required = false;
    value.permissions = {needed, wanted};
    return value;
}

signature_finding signed_by(std::string key_id, key_origin origin)
{
    signature_finding finding;
    finding.state = signature_state::valid;
    finding.key_id = std::move(key_id);
    finding.origin = origin;
    return finding;
}

/// A signature that names a key nobody on this computer knows.
signature_finding signed_by_unknown(std::string key_id)
{
    signature_finding finding;
    finding.state = signature_state::unknown_key;
    finding.key_id = std::move(key_id);
    return finding;
}

/// Everything a check needs, with nothing in the way: an unsigned plugin on
/// a computer that can keep it partly apart, and no policy file.
struct scene
{
    manifest declared = good_manifest();
    policy rules;
    revocation_list revoked;
    std::vector<std::string> capabilities{"engine_rules"};
    verify_input input;

    scene()
    {
        input.hash = hash_plugin(declared.files);
        input.today = today;
        input.host_api = api_version{1, 0};
        input.sandbox = sandbox_level::partial;
    }

    result<verify_result> run(std::string* detail = nullptr)
    {
        input.declared = &declared;
        input.capabilities = capabilities;
        return verify(input, detail);
    }

    void use_policy() { input.rules = &rules; }
    void use_revocations() { input.revocations = &revoked; }

    void make_official()
    {
        declared.not_after = 2027y / 11 / 1;
        input.signature = signed_by("plugins-2026", key_origin::official);
    }
};

void require_refused(scene& where, plugin_errc expected,
                     std::string_view part = {})
{
    std::string detail;
    const auto checked = where.run(&detail);
    REQUIRE(checked.has_error());
    REQUIRE(checked.error() == expected);
    REQUIRE(!detail.empty());
    REQUIRE(detail.find(part) != std::string::npos);
}

/// One entry of a policy's `allow_keys` with a public key that is valid hex.
std::string good_key_json(std::string_view key_id)
{
    std::string text = R"({ "key_id": ")";
    text += key_id;
    text += R"(", "public_key": ")";
    text += "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    text += R"(" })";
    return text;
}

void require_policy_refused(std::string_view text, std::string_view part)
{
    std::string detail;
    const auto parsed = parse_policy(text, &detail);
    REQUIRE(parsed.has_error());
    REQUIRE(parsed.error() == errc::config_error);
    REQUIRE(detail.find(part) != std::string::npos);
}

void require_revocations_refused(std::string_view text, std::string_view part)
{
    std::string detail;
    const auto parsed = parse_revocations(text, &detail);
    REQUIRE(parsed.has_error());
    REQUIRE(parsed.error() == errc::config_error);
    REQUIRE(detail.find(part) != std::string::npos);
}

/// The hash of a plugin that the list tests withdraw, as hex.
std::string a_hash()
{
    return to_hex(sha512_of(std::string_view("a plugin")));
}

} // namespace

// -- versions ---------------------------------------------------------------

TEST_CASE("plugin verify: versions are ordered as Semantic Versioning says")
{
    // The chain of the Semantic Versioning text, each earlier than the next.
    const std::vector<plugin_version> chain{
        {0, 9, 9, {}},          {1, 0, 0, "alpha"},  {1, 0, 0, "alpha.1"},
        {1, 0, 0, "alpha.beta"}, {1, 0, 0, "beta"},   {1, 0, 0, "beta.2"},
        {1, 0, 0, "beta.11"},    {1, 0, 0, "rc.1"},   {1, 0, 0, {}},
        {1, 0, 1, {}},           {1, 1, 0, "0"},      {1, 1, 0, {}},
        {2, 0, 0, {}}};
    for (std::size_t i = 0; i < chain.size(); ++i)
    {
        for (std::size_t j = 0; j < chain.size(); ++j)
        {
            REQUIRE(compare(chain[i], chain[j]) == (i <=> j));
        }
    }
    // Leading zeros do not make a number larger.
    REQUIRE(compare({1, 0, 0, "007"}, {1, 0, 0, "7"}) == 0);
    REQUIRE(compare({1, 0, 0, "0"}, {1, 0, 0, "00"}) == 0);
    REQUIRE(compare({1, 0, 0, "10"}, {1, 0, 0, "9"}) > 0);
    // A number comes before a word, whatever its size.
    REQUIRE(compare({1, 0, 0, "999"}, {1, 0, 0, "a"}) < 0);
}

// -- the policy -------------------------------------------------------------

TEST_CASE("plugin verify: a policy with only its version has the defaults")
{
    std::string detail = "old";
    const auto parsed = parse_policy(R"({ "policy_version": 1 })", &detail);
    REQUIRE(parsed.has_value());
    REQUIRE(detail.empty());
    REQUIRE(*parsed == policy{});
    REQUIRE(parsed->allow == allow_level::any);
    REQUIRE(parsed->min_sandbox == sandbox_level::partial);
    REQUIRE(!parsed->allow_no_sandbox);
    REQUIRE(parsed->user_plugins);
    REQUIRE(!parsed->on_timeout.has_value());
}

TEST_CASE("plugin verify: a policy reads every key")
{
    const auto parsed = parse_policy(R"({
        "policy_version": 1,
        "allow": "signed",
        "allow_keys": [
            { "key_id": "acme-2026", "name": "Acme Ltd",
              "public_key":
                "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a" },
            { "key_id": "9f2c41d07a6be835",
              "public_key":
                "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c" } ],
        "deny_plugins": ["com.example.bad"],
        "min_sandbox": "full",
        "allow_no_sandbox": true,
        "user_plugins": false,
        "on_timeout": "fail_closed",
        "permissions_denied": ["net.outbound", "process.spawn"]
    })");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->allow == allow_level::signed_only);
    REQUIRE_EQ(parsed->allow_keys.size(), std::size_t{2});
    REQUIRE_EQ(parsed->allow_keys[0].key_id, "acme-2026");
    REQUIRE_EQ(parsed->allow_keys[0].name, "Acme Ltd");
    REQUIRE_EQ(parsed->allow_keys[0].public_key[0], std::uint8_t{0xd7});
    REQUIRE_EQ(parsed->allow_keys[0].public_key[31], std::uint8_t{0x1a});
    REQUIRE_EQ(parsed->allow_keys[1].key_id, "9f2c41d07a6be835");
    REQUIRE(parsed->allow_keys[1].name.empty());
    REQUIRE_EQ(parsed->allow_keys[1].public_key[0], std::uint8_t{0x3d});
    REQUIRE_EQ(parsed->deny_plugins.size(), std::size_t{1});
    REQUIRE(parsed->min_sandbox == sandbox_level::full);
    REQUIRE(parsed->allow_no_sandbox);
    REQUIRE(!parsed->user_plugins);
    REQUIRE(parsed->on_timeout == timeout_action::fail_closed);
    REQUIRE_EQ(parsed->permissions_denied.size(), std::size_t{2});
    REQUIRE(parsed->permissions_denied[1] == permission::process_spawn);

    const auto open = parse_policy(
        R"({ "policy_version": 1, "allow": "official_only",
             "min_sandbox": "none", "on_timeout": null })");
    REQUIRE(open.has_value());
    REQUIRE(open->allow == allow_level::official_only);
    REQUIRE(open->min_sandbox == sandbox_level::none);
    REQUIRE(!open->on_timeout.has_value());
}

TEST_CASE("plugin verify: a policy that is not understood is refused")
{
    require_policy_refused("", "not JSON");
    require_policy_refused("[]", "must be an object");
    require_policy_refused("{}", "policy_version: is required");
    require_policy_refused(R"({ "policy_version": 2 })", "must be 1");
    require_policy_refused(R"({ "policy_version": "1" })", "must be 1");
    require_policy_refused(R"({ "policy_version": 1.0 })", "must be 1");
    require_policy_refused(R"({ "policy_version": 1, "alow": "any" })",
                           "unknown key \"alow\"");
    require_policy_refused(
        R"({ "policy_version": 1, "allow": "any", "allow": "signed" })",
        "given twice");
    require_policy_refused(R"({ "policy_version": 1 } x)", "after the JSON");
    require_policy_refused(R"({ "policy_version": 1, "allow": "all" })",
                           "allow: unknown value");
    require_policy_refused(R"({ "policy_version": 1, "allow": 1 })",
                           "allow: must be a string");
    require_policy_refused(
        R"({ "policy_version": 1, "allow_keys": "acme" })",
        "allow_keys: must be a list");
    const auto with_key = [](std::string_view entry) {
        std::string text = R"({ "policy_version": 1, "allow_keys": [ )";
        text += entry;
        text += " ] }";
        return text;
    };
    const std::string good_key = good_key_json("acme");
    REQUIRE(parse_policy(with_key(good_key)).has_value());
    // A key is an object with its public key, never a bare id: a signature
    // names its key by id only, so the key itself must be here.
    require_policy_refused(with_key(R"("acme")"),
                           "allow_keys: must hold objects");
    require_policy_refused(with_key(R"({ "key_id": "acme" })"),
                           "allow_keys.public_key: must be 64");
    require_policy_refused(with_key(R"({ "public_key": "00" })"),
                           "allow_keys.key_id: must be a key id");
    require_policy_refused(
        with_key(R"({ "key_id": "a b", "public_key": "00" })"),
        "allow_keys.key_id: must be a key id");
    require_policy_refused(
        with_key(R"({ "key_id": "acme", "public_key": "d75a" })"),
        "allow_keys.public_key: must be 64");
    {
        // Upper case is another spelling of the same key and is refused.
        std::string upper = good_key;
        const std::size_t at = upper.find("d75a");
        REQUIRE(at != std::string::npos);
        upper[at] = 'D';
        require_policy_refused(with_key(upper),
                               "allow_keys.public_key: must be 64");
    }
    require_policy_refused(
        with_key(good_key.substr(0, good_key.size() - 1) +
                 R"(, "name": 7 })"),
        "allow_keys.name: must be a short plain text");
    require_policy_refused(
        with_key(good_key.substr(0, good_key.size() - 1) +
                 R"(, "owner": "x" })"),
        "allow_keys: unknown key");
    require_policy_refused(with_key(good_key + ", " + good_key), "twice");
    require_policy_refused(
        R"({ "policy_version": 1, "deny_plugins": ["Not An Id"] })",
        "deny_plugins: cannot hold");
    require_policy_refused(
        R"({ "policy_version": 1, "min_sandbox": "strong" })",
        "min_sandbox: unknown value");
    require_policy_refused(
        R"({ "policy_version": 1, "allow_no_sandbox": "yes" })",
        "allow_no_sandbox: must be true or false");
    require_policy_refused(R"({ "policy_version": 1, "user_plugins": 0 })",
                           "user_plugins: must be true or false");
    require_policy_refused(
        R"({ "policy_version": 1, "on_timeout": "fail_safe" })",
        "on_timeout: unknown value");
    require_policy_refused(
        R"({ "policy_version": 1, "permissions_denied": ["net.everything"] })",
        "permissions_denied: cannot hold");
    require_policy_refused(
        R"({ "policy_version": 1, "allow_keys": [["deep"]] })",
        "allow_keys: must hold objects");

    std::string many = R"({ "policy_version": 1, "allow_keys": [)";
    for (std::size_t i = 0; i <= max_policy_keys; ++i)
    {
        if (i != 0)
            many += ',';
        std::string id = "k";
        id += std::to_string(i);
        many += good_key_json(id);
    }
    many += "] }";
    require_policy_refused(many, "more than 256 entries");

    std::string large = R"({ "policy_version": 1 })";
    large.append(max_policy_size, ' ');
    require_policy_refused(large, "is larger than");
}

TEST_CASE("plugin verify: the fallback policy allows only official plugins")
{
    const policy fallback = official_only_policy();
    REQUIRE(fallback.allow == allow_level::official_only);
    REQUIRE(fallback.min_sandbox == sandbox_level::full);
    REQUIRE(!fallback.allow_no_sandbox);
    REQUIRE(!fallback.user_plugins);
}

TEST_CASE("plugin verify: a key id is a short plain token")
{
    REQUIRE(is_key_id("plugins-2026"));
    REQUIRE(is_key_id("9f2c41d07a6be835"));
    REQUIRE(is_key_id("a-b-1"));
    // Only what a signed envelope can name: no upper case, dot or
    // underscore.
    REQUIRE(!is_key_id("Plugins-2026"));
    REQUIRE(!is_key_id("a.b"));
    REQUIRE(!is_key_id("a_b"));
    REQUIRE(is_key_id(std::string(max_key_id_length, 'k')));
    REQUIRE(!is_key_id(""));
    REQUIRE(!is_key_id(std::string(max_key_id_length + 1, 'k')));
    REQUIRE(!is_key_id("a b"));
    REQUIRE(!is_key_id("a/b"));
    REQUIRE(!is_key_id("k\xC3\xA9y"));
}

// -- the revocation list ----------------------------------------------------

TEST_CASE("plugin verify: a revocation list reads and answers")
{
    const std::string text =
        R"({ "revocations_version": 1, "serial": 7,
             "issued_at": "2026-11-13",
             "key_ids": ["plugins-2025"],
             "plugin_hashes": [")" +
        a_hash() + R"("],
             "plugins": [ { "id": "com.example.plugin",
                            "max_version": "1.2.0" } ] })";
    std::string detail = "old";
    const auto parsed = parse_revocations(text, &detail);
    REQUIRE(parsed.has_value());
    REQUIRE(detail.empty());
    REQUIRE_EQ(parsed->serial, std::uint64_t{7});
    REQUIRE(parsed->issued_at == 2026y / 11 / 13);

    REQUIRE(parsed->revokes_key("plugins-2025"));
    REQUIRE(!parsed->revokes_key("plugins-2026"));
    REQUIRE(!parsed->revokes_key(""));

    REQUIRE(parsed->revokes_hash(
        plugin_hash{sha512_of(std::string_view("a plugin"))}));
    REQUIRE(!parsed->revokes_hash(
        plugin_hash{sha512_of(std::string_view("another"))}));

    REQUIRE(parsed->revokes_plugin("com.example.plugin", {1, 2, 0, {}}));
    REQUIRE(parsed->revokes_plugin("com.example.plugin", {1, 2, 0, "rc.1"}));
    REQUIRE(parsed->revokes_plugin("com.example.plugin", {0, 1, 0, {}}));
    REQUIRE(!parsed->revokes_plugin("com.example.plugin", {1, 2, 1, {}}));
    REQUIRE(!parsed->revokes_plugin("com.example.other", {1, 0, 0, {}}));

    const auto least = parse_revocations(
        R"({ "revocations_version": 1, "serial": 1,
             "issued_at": "2026-01-01" })");
    REQUIRE(least.has_value());
    REQUIRE(least->key_ids.empty());
    REQUIRE(least->plugin_hashes.empty());
    REQUIRE(least->plugins.empty());
}

TEST_CASE("plugin verify: a revocation list that is not understood is refused")
{
    const auto with = [](std::string_view member) {
        std::string text =
            R"({ "revocations_version": 1, "serial": 1,
                 "issued_at": "2026-01-01", )";
        text += member;
        text += " }";
        return text;
    };
    require_revocations_refused("", "not JSON");
    require_revocations_refused("7", "must be an object");
    require_revocations_refused(R"({ "serial": 1 })",
                                "revocations_version: must be 1");
    require_revocations_refused(
        R"({ "revocations_version": 2, "serial": 1 })", "must be 1");
    require_revocations_refused(R"({ "revocations_version": 1 })",
                                "serial: must be a whole number");
    require_revocations_refused(
        R"({ "revocations_version": 1, "serial": 0 })", "serial");
    require_revocations_refused(
        R"({ "revocations_version": 1, "serial": -4 })", "serial");
    require_revocations_refused(
        R"({ "revocations_version": 1, "serial": 9007199254740992 })",
        "serial");
    require_revocations_refused(
        R"({ "revocations_version": 1, "serial": 1.5 })", "serial");
    require_revocations_refused(
        R"({ "revocations_version": 1, "serial": 1 })",
        "issued_at: is required");
    require_revocations_refused(
        R"({ "revocations_version": 1, "serial": 1,
             "issued_at": "13/11/2026" })",
        "issued_at: must be a date");
    require_revocations_refused(with(R"("extra": 1)"), "unknown key");
    require_revocations_refused(with(R"("serial": 2)"), "given twice");
    require_revocations_refused(with(R"("key_ids": "k")"),
                                "key_ids: must be a list");
    require_revocations_refused(with(R"("key_ids": ["a b"])"),
                                "not a key id");
    require_revocations_refused(with(R"("key_ids": [1])"), "not a key id");
    require_revocations_refused(with(R"("plugin_hashes": ["abc"])"),
                                "128 lower-case hex");
    std::string upper = a_hash();
    upper[0] = 'A';
    require_revocations_refused(
        with("\"plugin_hashes\": [\"" + upper + "\"]"), "128 lower-case hex");
    require_revocations_refused(with(R"("plugins": [ "id" ])"),
                                "plugins: must be an object");
    require_revocations_refused(
        with(R"("plugins": [ { "id": "com.example.plugin" } ])"),
        "plugins.max_version: is required");
    require_revocations_refused(
        with(R"("plugins": [ { "id": "X", "max_version": "1.0.0" } ])"),
        "plugins.id: cannot be");
    require_revocations_refused(
        with(R"("plugins": [ { "id": "com.example.plugin",
                               "max_version": "1.0" } ])"),
        "plugins.max_version: cannot be");
    require_revocations_refused(
        with(R"("plugins": [ { "id": "com.example.plugin",
                               "max_version": "1.0.0", "why": "" } ])"),
        "unknown key");

    std::string many = "\"key_ids\": [";
    for (std::size_t i = 0; i <= max_revoked_entries; ++i)
    {
        many += i == 0 ? "\"k\"" : ",\"k\"";
    }
    many += ']';
    require_revocations_refused(with(many), "more than 4096 entries");

    std::string large = with(R"("key_ids": [])");
    large.append(max_revocations_size, ' ');
    require_revocations_refused(large, "is larger than");
}

// -- the decision -----------------------------------------------------------

TEST_CASE("plugin verify: an unsigned plugin may run with the defaults")
{
    scene where;
    std::string detail = "old";
    const auto checked = where.run(&detail);
    REQUIRE(checked.has_value());
    REQUIRE(detail.empty());
    REQUIRE(checked->level == trust_level::unsigned_plugin);
    REQUIRE(checked->key_id.empty());
    REQUIRE(checked->denied_permissions.empty());
    REQUIRE(!checked->forced_on_timeout.has_value());
    REQUIRE(!checked->below_min_sandbox);
    // The same input gives the same answer.
    REQUIRE(*where.run() == *checked);
}

TEST_CASE("plugin verify: no manifest is an error of the caller")
{
    verify_input input;
    std::string detail;
    const auto checked = verify(input, &detail);
    REQUIRE(checked.has_error());
    REQUIRE(checked.error() == errc::invalid_argument);
    REQUIRE(!detail.empty());
}

TEST_CASE("plugin verify: the level follows where the key is known from")
{
    scene where;
    where.make_official();
    auto checked = where.run();
    REQUIRE(checked.has_value());
    REQUIRE(checked->level == trust_level::official);
    REQUIRE_EQ(checked->key_id, "plugins-2026");

    where.input.signature = signed_by("acme", key_origin::administrator);
    checked = where.run();
    REQUIRE(checked.has_value());
    REQUIRE(checked->level == trust_level::third_party_signed);

    where.input.signature = signed_by("acme", key_origin::user);
    checked = where.run();
    REQUIRE(checked.has_value());
    REQUIRE(checked->level == trust_level::third_party_signed);

    where.input.signature = signed_by_unknown("acme");
    checked = where.run();
    REQUIRE(checked.has_value());
    REQUIRE(checked->level == trust_level::signed_unknown_key);
    REQUIRE_EQ(checked->key_id, "acme");

    REQUIRE(to_string(trust_level::official) == "official");
    REQUIRE(to_string(trust_level::third_party_signed) ==
            "third_party_signed");
    REQUIRE(to_string(trust_level::signed_unknown_key) ==
            "signed_unknown_key");
    REQUIRE(to_string(trust_level::unsigned_plugin) == "unsigned");
    REQUIRE(to_string(allow_level::signed_only) == "signed");
    REQUIRE(to_string(sandbox_level::partial) == "partial");
}

TEST_CASE("plugin verify: a signature that does not hold is never 'unsigned'")
{
    scene where;
    where.input.signature.state = signature_state::invalid;
    require_refused(where, plugin_errc::signature_invalid);

    // An official key named by a signature that does not hold counts for
    // nothing.
    where.input.signature.key_id = "plugins-2026";
    where.input.signature.origin = key_origin::official;
    require_refused(where, plugin_errc::signature_invalid);

    scene no_key;
    no_key.input.signature = signed_by("", key_origin::official);
    require_refused(no_key, plugin_errc::signature_invalid, "names no key");
    no_key.input.signature = signed_by_unknown("");
    require_refused(no_key, plugin_errc::signature_invalid, "names no key");

    // "It holds" and "the key is not known" cannot both be true.
    scene no_origin;
    no_origin.input.signature = signed_by("acme", key_origin::unknown);
    require_refused(no_origin, plugin_errc::signature_invalid,
                    "a key that is not known");

    scene no_files;
    no_files.declared.files.clear();
    no_files.input.signature = signed_by("acme", key_origin::user);
    require_refused(no_files, plugin_errc::signature_invalid,
                    "list its files");
}

TEST_CASE("plugin verify: an official plugin must say when it ends")
{
    scene where;
    where.make_official();
    where.declared.not_after.reset();
    require_refused(where, plugin_errc::manifest_invalid, "not_after");

    // Anyone else may leave it out.
    where.input.signature = signed_by("acme", key_origin::user);
    REQUIRE(where.run().has_value());
}

TEST_CASE("plugin verify: a reserved id needs an official signature")
{
    scene where;
    where.declared.id = "com.virtualserialstudio.protocol";
    require_refused(where, plugin_errc::reserved_id);

    where.input.signature = signed_by("acme", key_origin::user);
    require_refused(where, plugin_errc::reserved_id);
    where.input.signature = signed_by("acme", key_origin::administrator);
    require_refused(where, plugin_errc::reserved_id);
    where.input.signature = signed_by_unknown("acme");
    require_refused(where, plugin_errc::reserved_id);

    where.make_official();
    REQUIRE(where.run().has_value());
}

TEST_CASE("plugin verify: a withdrawn key, copy or version does not run")
{
    {
        scene where;
        where.use_revocations();
        REQUIRE(where.run().has_value());
    }
    {
        // A key, wherever it is known from, the official list included.
        scene where;
        where.use_revocations();
        where.revoked.key_ids = {"plugins-2026"};
        where.make_official();
        require_refused(where, plugin_errc::revoked, "signing key");
        where.input.signature = signed_by("plugins-2026", key_origin::user);
        require_refused(where, plugin_errc::revoked, "signing key");
        where.input.signature = signed_by_unknown("plugins-2026");
        require_refused(where, plugin_errc::revoked, "signing key");
        where.input.signature = signed_by("other", key_origin::user);
        REQUIRE(where.run().has_value());
    }
    {
        scene where;
        where.use_revocations();
        where.revoked.plugin_hashes = {where.input.hash};
        require_refused(where, plugin_errc::revoked, "this copy");
        where.input.hash = hash_plugin({});
        REQUIRE(where.run().has_value());
    }
    {
        scene where;
        where.use_revocations();
        where.revoked.plugins = {
            revoked_plugin{"com.example.plugin", {1, 2, 0, {}}}};
        require_refused(where, plugin_errc::revoked, "this version");
        where.declared.version = plugin_version{1, 1, 9, {}};
        require_refused(where, plugin_errc::revoked, "this version");
        where.declared.version = plugin_version{1, 2, 1, {}};
        REQUIRE(where.run().has_value());
        // An official signature does not save a withdrawn version.
        where.declared.version = plugin_version{1, 2, 0, {}};
        where.make_official();
        require_refused(where, plugin_errc::revoked, "this version");
    }
}

TEST_CASE("plugin verify: a plugin runs on its last day and not after")
{
    scene where;
    where.declared.not_after = 2026y / 11 / 13;
    REQUIRE(where.run().has_value());

    where.input.today = today + std::chrono::days{1};
    require_refused(where, plugin_errc::expired);

    // The user may run a plugin that is not official anyway.
    where.input.run_expired = true;
    REQUIRE(where.run().has_value());
    where.input.signature = signed_by("acme", key_origin::user);
    REQUIRE(where.run().has_value());

    // An official plugin never.
    where.input.signature = signed_by("plugins-2026", key_origin::official);
    require_refused(where, plugin_errc::expired);
}

TEST_CASE("plugin verify: the policy names plugins that never run")
{
    scene where;
    where.use_policy();
    where.rules.deny_plugins = {"com.example.other", "com.example.plugin"};
    require_refused(where, plugin_errc::policy, "deny_plugins");
    where.make_official();
    require_refused(where, plugin_errc::policy, "deny_plugins");
    where.rules.deny_plugins = {"com.example.other"};
    REQUIRE(where.run().has_value());
}

TEST_CASE("plugin verify: official_only lets only official plugins run")
{
    scene where;
    where.use_policy();
    where.rules.allow = allow_level::official_only;
    require_refused(where, plugin_errc::policy, "only official");
    where.input.signature = signed_by("acme", key_origin::user);
    require_refused(where, plugin_errc::policy, "only official");
    where.input.signature = signed_by("acme", key_origin::administrator);
    require_refused(where, plugin_errc::policy, "only official");
    where.make_official();
    REQUIRE(where.run().has_value());

    // The policy used for a policy file that cannot be trusted.
    scene fallback;
    fallback.rules = official_only_policy();
    fallback.use_policy();
    fallback.input.sandbox = sandbox_level::full;
    require_refused(fallback, plugin_errc::policy);
    fallback.input.sandbox = sandbox_level::none;
    fallback.make_official();
    const auto checked = fallback.run();
    REQUIRE(checked.has_value());
    REQUIRE(checked->below_min_sandbox);
}

TEST_CASE("plugin verify: 'signed' needs a signature with an approved key")
{
    scene where;
    where.use_policy();
    where.rules.allow = allow_level::signed_only;
    require_refused(where, plugin_errc::signature_missing);
    where.input.signature = signed_by_unknown("acme");
    require_refused(where, plugin_errc::key_unknown);
    where.input.signature = signed_by("acme", key_origin::user);
    REQUIRE(where.run().has_value());
    where.input.signature = signed_by("acme", key_origin::administrator);
    REQUIRE(where.run().has_value());
    where.make_official();
    REQUIRE(where.run().has_value());
}

TEST_CASE("plugin verify: a denied permission stops only a plugin that needs "
          "it")
{
    scene where;
    where.use_policy();
    where.rules.permissions_denied = {permission::net_outbound,
                                      permission::process_spawn};
    auto checked = where.run();
    REQUIRE(checked.has_value());
    REQUIRE_EQ(checked->denied_permissions.size(), std::size_t{1});
    REQUIRE(checked->denied_permissions[0] == permission::net_outbound);

    where.rules.permissions_denied = {permission::ports_read};
    require_refused(where, plugin_errc::policy, "ports.read");
    // Official plugins too: the administrator has the last word.
    where.make_official();
    require_refused(where, plugin_errc::policy, "ports.read");
}

TEST_CASE("plugin verify: the policy's timeout choice is passed on")
{
    scene where;
    where.use_policy();
    where.rules.on_timeout = timeout_action::fail_closed;
    const auto checked = where.run();
    REQUIRE(checked.has_value());
    REQUIRE(checked->forced_on_timeout == timeout_action::fail_closed);
}

TEST_CASE("plugin verify: the host must speak the plugin's API and have what "
          "it needs")
{
    scene where;
    where.input.host_api = api_version{2, 0};
    require_refused(where, plugin_errc::api_incompatible);
    where.input.host_api = api_version{1, 7};
    REQUIRE(where.run().has_value());

    where.declared.needs.capabilities = {"engine_rules", "route_hook"};
    require_refused(where, plugin_errc::capability_missing, "route_hook");
    where.capabilities.push_back("route_hook");
    REQUIRE(where.run().has_value());
    where.capabilities.clear();
    require_refused(where, plugin_errc::capability_missing, "engine_rules");
}

TEST_CASE("plugin verify: a plugin that cannot be kept apart does not run")
{
    scene where;
    where.input.sandbox = sandbox_level::none;
    require_refused(where, plugin_errc::sandbox_insufficient, "partial");
    where.input.signature = signed_by("acme", key_origin::user);
    require_refused(where, plugin_errc::sandbox_insufficient);

    // An official plugin runs, and the result says at what level.
    where.make_official();
    auto checked = where.run();
    REQUIRE(checked.has_value());
    REQUIRE(checked->below_min_sandbox);

    // A stronger minimum from the administrator.
    scene strict;
    strict.use_policy();
    strict.rules.min_sandbox = sandbox_level::full;
    require_refused(strict, plugin_errc::sandbox_insufficient, "full");
    strict.input.sandbox = sandbox_level::full;
    checked = strict.run();
    REQUIRE(checked.has_value());
    REQUIRE(!checked->below_min_sandbox);

    // A minimum of none lets everything through.
    scene open;
    open.use_policy();
    open.rules.min_sandbox = sandbox_level::none;
    open.input.sandbox = sandbox_level::none;
    checked = open.run();
    REQUIRE(checked.has_value());
    REQUIRE(!checked->below_min_sandbox);
}

TEST_CASE("plugin verify: the override counts only where it is allowed")
{
    // Studio for everyone, no policy file: not allowed.
    scene everyone;
    everyone.input.sandbox = sandbox_level::none;
    everyone.input.sandbox_override = true;
    everyone.input.install = install_kind::for_everyone;
    require_refused(everyone, plugin_errc::sandbox_insufficient);
    REQUIRE(!may_override_sandbox(nullptr, install_kind::for_everyone));

    // Studio for everyone, and the administrator allows it.
    everyone.use_policy();
    everyone.rules.allow_no_sandbox = true;
    auto checked = everyone.run();
    REQUIRE(checked.has_value());
    REQUIRE(checked->below_min_sandbox);
    REQUIRE(may_override_sandbox(&everyone.rules, install_kind::for_everyone));
    // Allowed is not the same as chosen.
    everyone.input.sandbox_override = false;
    require_refused(everyone, plugin_errc::sandbox_insufficient);

    // Studio for one user, no policy file: the user's own choice.
    scene one_user;
    one_user.input.sandbox = sandbox_level::none;
    one_user.input.install = install_kind::for_one_user;
    require_refused(one_user, plugin_errc::sandbox_insufficient);
    one_user.input.sandbox_override = true;
    checked = one_user.run();
    REQUIRE(checked.has_value());
    REQUIRE(checked->below_min_sandbox);
    REQUIRE(may_override_sandbox(nullptr, install_kind::for_one_user));

    // Studio for one user, and a policy file that does not allow it.
    one_user.use_policy();
    require_refused(one_user, plugin_errc::sandbox_insufficient);
    REQUIRE(!may_override_sandbox(&one_user.rules, install_kind::for_one_user));
}

TEST_CASE("plugin verify: the first check that fails is the answer")
{
    // Everything is wrong with this one.
    scene where;
    where.use_policy();
    where.use_revocations();
    where.declared.id = "com.virtualserialstudio.fake";
    where.declared.not_after = 2026y / 1 / 1;
    where.declared.needs.capabilities = {"missing"};
    where.input.host_api = api_version{9, 0};
    where.input.sandbox = sandbox_level::none;
    where.rules.deny_plugins = {where.declared.id};
    where.revoked.plugin_hashes = {where.input.hash};
    where.input.signature.state = signature_state::invalid;

    require_refused(where, plugin_errc::signature_invalid);
    where.input.signature = {};
    require_refused(where, plugin_errc::reserved_id);
    where.declared.id = "com.example.plugin";
    where.rules.deny_plugins = {where.declared.id};
    require_refused(where, plugin_errc::revoked);
    where.revoked.plugin_hashes.clear();
    require_refused(where, plugin_errc::expired);
    where.declared.not_after.reset();
    require_refused(where, plugin_errc::policy);
    where.rules.deny_plugins.clear();
    require_refused(where, plugin_errc::api_incompatible);
    where.input.host_api = api_version{1, 0};
    require_refused(where, plugin_errc::capability_missing);
    where.declared.needs.capabilities.clear();
    require_refused(where, plugin_errc::sandbox_insufficient);
    where.input.sandbox = sandbox_level::partial;
    REQUIRE(where.run().has_value());
}
