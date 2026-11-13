//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/revocations.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace vss;
using namespace vss::plugin;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;

constexpr std::chrono::year_month_day day{2026y / 11 / 14};

/// The SHA-512 of no bytes, which is the hash of the plugin in the exact
/// text test.
constexpr std::string_view empty_digest =
    "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
    "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e";

std::string repeated(std::string_view unit, std::size_t count)
{
    std::string out;
    for (std::size_t i = 0; i < count; ++i)
        out.append(unit);
    return out;
}

plugin_hash hash_of(unsigned seed)
{
    plugin_hash hash;
    hash.bytes = sha512_of(std::string_view(std::to_string(seed)));
    return hash;
}

public_key_bytes key_bytes(std::uint8_t fill)
{
    public_key_bytes bytes{};
    bytes.fill(fill);
    return bytes;
}

approved_key make_key(std::string id, std::uint8_t fill = 1)
{
    approved_key key;
    key.key_id = std::move(id);
    key.public_key = key_bytes(fill);
    key.name = "Acme Ltd";
    key.approved_on = day;
    return key;
}

/// A decision that is allowed: signed by `key_id`, or unsigned when it is
/// empty.
plugin_decision make_decision(std::string id, unsigned seed,
                              std::string key_id = {})
{
    plugin_decision decision;
    decision.plugin_id = std::move(id);
    decision.hash = hash_of(seed);
    decision.level = key_id.empty() ? trust_level::unsigned_plugin
                                    : trust_level::third_party_signed;
    decision.key_id = std::move(key_id);
    decision.enabled = true;
    decision.granted = {permission::ports_read};
    decision.decided_on = day;
    return decision;
}

/// The two helpers say the line of the call, not their own.
void require_added(const result<void>& done,
                   std::source_location where = std::source_location::current())
{
    if (!done.has_value())
        vss_test::raise("the call is accepted", where.file_name(),
                        static_cast<int>(where.line()));
}

void require_error(const result<void>& done, errc expected,
                   std::source_location where = std::source_location::current())
{
    if (!done.has_error() || done.error() != expected)
        vss_test::raise("the call is refused with the error expected",
                        where.file_name(), static_cast<int>(where.line()));
}

/// The id of the n-th plugin of a long run.
std::string numbered_id(std::size_t n)
{
    std::string id = "com.example.p";
    id += std::to_string(n);
    return id;
}

/// A store that uses every field of every record.
trust_store full_store()
{
    trust_store store;
    approved_key acme = make_key("acme-2026", 1);
    acme.name = "Acme Ltd";
    require_added(store.approve_key(acme));
    approved_key other = make_key("other.key_2", 2);
    other.name = "Zo\xC3\xAB's \"Plugins\"";
    other.approved_on = 2026y / 2 / 28;
    require_added(store.approve_key(other));

    plugin_decision signed_one =
        make_decision("com.example.plugin", 1, "acme-2026");
    signed_one.granted = {permission::ports_read, permission::traffic_tap,
                          permission::net_outbound};
    require_added(store.decide(signed_one));
    plugin_decision signed_two =
        make_decision("com.example.plugin", 2, "other.key_2");
    signed_two.enabled = false;
    signed_two.decided_on = 2026y / 11 / 15;
    require_added(store.decide(signed_two));
    plugin_decision unsigned_one = make_decision("com.example.plugin", 3);
    unsigned_one.run_without_sandbox = true;
    unsigned_one.granted.clear();
    require_added(store.decide(unsigned_one));
    plugin_decision official =
        make_decision("com.example.official", 4, "plugins-2026");
    official.level = trust_level::official;
    require_added(store.decide(official));
    plugin_decision stranger =
        make_decision("com.example.stranger", 5, "unknown-key");
    stranger.level = trust_level::signed_unknown_key;
    require_added(store.decide(stranger));

    REQUIRE(store.note_revocation_serial(7));
    REQUIRE(store.note_key_list_serial(4));
    store.note_date(std::chrono::sys_days{day});
    return store;
}

// -- text of stores ---------------------------------------------------------

std::string document(std::string_view keys, std::string_view plugins,
                     std::string_view ledger)
{
    std::string text = R"({ "trust_version": 1, "keys": [)";
    text += keys;
    text += R"(], "plugins": [)";
    text += plugins;
    text += R"(], "ledger": )";
    text += ledger;
    text += " }";
    return text;
}

constexpr std::string_view good_ledger =
    R"({ "revocation_serial": 7, "key_list_serial": 4,
         "latest_date": "2026-11-14" })";

std::string key_object(std::string_view id)
{
    std::string text = R"({ "key_id": ")";
    text += id;
    text += R"(", "public_key": ")";
    text += repeated("01", 32);
    text += R"(", "name": "Acme Ltd", "approved_on": "2026-11-14" })";
    return text;
}

std::string decision_object(std::string_view id, unsigned seed)
{
    std::string text = R"({ "plugin_id": ")";
    text += id;
    text += R"(", "plugin_hash": ")";
    text += to_hex(hash_of(seed).bytes);
    text += R"(", "key_id": "acme-2026", "level": "third_party_signed", )";
    text += R"("enabled": true, "granted": ["ports.read", "traffic.tap"], )";
    text += R"("run_without_sandbox": false, "decided_on": "2026-11-14" })";
    return text;
}

std::string replaced(std::string text, std::string_view from,
                     std::string_view to)
{
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

/// A document with one key and no decision.
std::string with_key(const std::string& key)
{
    return document(key, "", good_ledger);
}

/// A document with one decision and no key.
std::string with_decision(const std::string& decision)
{
    return document("", decision, good_ledger);
}

void require_refused(std::string_view text, std::string_view part)
{
    std::string explain = "old";
    const auto parsed = parse_trust_store(text, &explain);
    REQUIRE(parsed.has_error());
    REQUIRE(parsed.error() == errc::config_error);
    REQUIRE(!explain.empty());
    if (explain.find(part) == std::string::npos)
    {
        std::string message = "the detail \"";
        message += explain;
        message += "\" does not hold \"";
        message += part;
        message += '"';
        throw vss_test::failure(message);
    }
    // The same text is refused when no detail is asked for.
    REQUIRE(parse_trust_store(text).has_error());
}

std::string read_file(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, std::string_view text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::size_t count_entries(const fs::path& directory)
{
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory))
    {
        static_cast<void>(entry);
        ++count;
    }
    return count;
}

} // namespace

// -- writing and reading ----------------------------------------------------

TEST_CASE("plugin trust store: an empty store writes its four keys")
{
    const trust_store store;
    const std::string text = to_json(store);
    const std::string expected = R"json({
  "trust_version": 1,
  "keys": [],
  "plugins": [],
  "ledger": {
    "revocation_serial": 0,
    "key_list_serial": 0
  }
}
)json";
    REQUIRE_EQ(text, expected);

    std::string explain = "old";
    const auto parsed = parse_trust_store(text, &explain);
    REQUIRE(parsed.has_value());
    REQUIRE(explain.empty());
    REQUIRE(*parsed == store);

    // A file with nothing but the version is an empty store too.
    const auto least = parse_trust_store(R"({ "trust_version": 1 })");
    REQUIRE(least.has_value());
    REQUIRE(*least == store);
}

TEST_CASE("plugin trust store: a full store round trips")
{
    const trust_store store = full_store();
    REQUIRE_EQ(store.keys().size(), std::size_t{2});
    REQUIRE_EQ(store.decisions().size(), std::size_t{5});

    const std::string text = to_json(store);
    REQUIRE_EQ(to_json(store), text);
    REQUIRE(text.back() == '\n');

    std::string explain = "old";
    const auto parsed = parse_trust_store(text, &explain);
    REQUIRE(parsed.has_value());
    REQUIRE(explain.empty());
    REQUIRE(*parsed == store);
    REQUIRE_EQ(to_json(*parsed), text);

    // The order of the file is the order of the store, oldest first.
    REQUIRE_EQ(parsed->decisions()[0].hash.short_text(),
               hash_of(1).short_text());
    REQUIRE_EQ(parsed->decisions()[4].hash.short_text(),
               hash_of(5).short_text());
    REQUIRE(parsed->keys()[1] == store.keys()[1]);
    REQUIRE(parsed->ledger().latest_date == day);
}

TEST_CASE("plugin trust store: the text written is exactly this")
{
    trust_store store;
    approved_key key = make_key("acme-2026", 0x0a);
    key.name = "Acme \"Ltd\" \\ Co";
    require_added(store.approve_key(key));

    plugin_decision first = make_decision("com.example.plugin", 0, "acme-2026");
    first.hash = plugin_hash{sha512_of(std::string_view{})};
    first.granted = {permission::ports_read, permission::traffic_tap};
    require_added(store.decide(first));

    plugin_decision second = make_decision("com.example.other", 0);
    second.hash.bytes.fill(0xab);
    second.enabled = false;
    second.granted.clear();
    second.run_without_sandbox = true;
    second.decided_on = 2026y / 11 / 15;
    require_added(store.decide(second));

    REQUIRE(store.note_revocation_serial(7));
    REQUIRE(store.note_key_list_serial(4));
    store.note_date(std::chrono::sys_days{day});

    std::string expected = R"json({
  "trust_version": 1,
  "keys": [
    {
      "key_id": "acme-2026",
      "public_key": ")json";
    expected += repeated("0a", 32);
    expected += R"json(",
      "name": "Acme \"Ltd\" \\ Co",
      "approved_on": "2026-11-14"
    }
  ],
  "plugins": [
    {
      "plugin_id": "com.example.plugin",
      "plugin_hash": ")json";
    expected += empty_digest;
    expected += R"json(",
      "key_id": "acme-2026",
      "level": "third_party_signed",
      "enabled": true,
      "granted": [
        "ports.read",
        "traffic.tap"
      ],
      "run_without_sandbox": false,
      "decided_on": "2026-11-14"
    },
    {
      "plugin_id": "com.example.other",
      "plugin_hash": ")json";
    expected += repeated("ab", 64);
    expected += R"json(",
      "level": "unsigned",
      "enabled": false,
      "granted": [],
      "run_without_sandbox": true,
      "decided_on": "2026-11-15"
    }
  ],
  "ledger": {
    "revocation_serial": 7,
    "key_list_serial": 4,
    "latest_date": "2026-11-14"
  }
}
)json";
    REQUIRE_EQ(to_json(store), expected);

    const auto parsed = parse_trust_store(expected);
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == store);
}

TEST_CASE("plugin trust store: what a file may leave out")
{
    // No key_id for an unsigned plugin, and the ledger without a date.
    const std::string unsigned_decision =
        replaced(replaced(decision_object("com.example.plugin", 1),
                          R"("key_id": "acme-2026", )", ""),
                 "third_party_signed", "unsigned");
    const auto parsed = parse_trust_store(
        document("", unsigned_decision,
                 R"({ "revocation_serial": 0, "key_list_serial": 0 })"));
    REQUIRE(parsed.has_value());
    REQUIRE_EQ(parsed->decisions().size(), std::size_t{1});
    REQUIRE(parsed->decisions()[0].key_id.empty());
    REQUIRE(parsed->decisions()[0].level == trust_level::unsigned_plugin);
    REQUIRE(!parsed->ledger().latest_date.has_value());
    REQUIRE(to_json(*parsed).find("latest_date") == std::string::npos);
    REQUIRE(to_json(*parsed).find("\"key_id\"") == std::string::npos);

    // The ends of the serial range.
    const auto least = parse_trust_store(
        R"({ "trust_version": 1, "ledger": { "revocation_serial": 0,
             "key_list_serial": 9007199254740991 } })");
    REQUIRE(least.has_value());
    REQUIRE_EQ(least->ledger().revocation_serial, std::uint64_t{0});
    REQUIRE_EQ(least->ledger().key_list_serial, max_revocations_serial);

    // White space after the document is not text after it.
    REQUIRE(parse_trust_store("{ \"trust_version\": 1 }\n\n").has_value());
}

// -- approve_key ------------------------------------------------------------

TEST_CASE("plugin trust store: approve_key refuses what is not a key")
{
    const trust_store empty;
    trust_store store;

    for (const std::string_view id : {"", "a b", "a/b", "k\xC3\xA9y", "key:1"})
    {
        approved_key key = make_key("x");
        key.key_id = std::string(id);
        require_error(store.approve_key(key), errc::invalid_argument);
    }
    approved_key long_id = make_key(std::string(max_key_id_length + 1, 'k'));
    require_error(store.approve_key(long_id), errc::invalid_argument);

    approved_key no_name = make_key("acme-2026");
    no_name.name.clear();
    require_error(store.approve_key(no_name), errc::invalid_argument);
    approved_key long_name = make_key("acme-2026");
    long_name.name = std::string(max_name_length + 1, 'n');
    require_error(store.approve_key(long_name), errc::invalid_argument);
    approved_key control = make_key("acme-2026");
    control.name = "Acme\nLtd";
    require_error(store.approve_key(control), errc::invalid_argument);
    approved_key direction = make_key("acme-2026");
    direction.name = "Acme \xE2\x80\xAE Ltd";
    require_error(store.approve_key(direction), errc::invalid_argument);
    approved_key bad_utf8 = make_key("acme-2026");
    bad_utf8.name = "Acme \xC3";
    require_error(store.approve_key(bad_utf8), errc::invalid_argument);

    approved_key no_date = make_key("acme-2026");
    no_date.approved_on = std::chrono::year_month_day{};
    require_error(store.approve_key(no_date), errc::invalid_argument);
    approved_key no_such_day = make_key("acme-2026");
    no_such_day.approved_on = 2026y / 2 / 30;
    require_error(store.approve_key(no_such_day), errc::invalid_argument);
    // A year the file cannot write and read again.
    approved_key too_early = make_key("acme-2026");
    too_early.approved_on = 1999y / 12 / 31;
    require_error(store.approve_key(too_early), errc::invalid_argument);
    approved_key too_late = make_key("acme-2026");
    too_late.approved_on = 10000y / 1 / 1;
    require_error(store.approve_key(too_late), errc::invalid_argument);

    // None of the refusals left anything behind.
    REQUIRE(store == empty);

    // The longest id and name are fine.
    approved_key longest = make_key(std::string(max_key_id_length, 'k'));
    longest.name = std::string(max_name_length, 'n');
    require_added(store.approve_key(longest));
    REQUIRE(store.find_key(longest.key_id) != nullptr);
}

TEST_CASE("plugin trust store: approving a key again")
{
    trust_store store;
    require_added(store.approve_key(make_key("acme-2026", 1)));
    require_added(store.approve_key(make_key("other-key", 2)));
    REQUIRE_EQ(store.keys().size(), std::size_t{2});

    // The same id and key: the name and the date are replaced, in place.
    approved_key again = make_key("acme-2026", 1);
    again.name = "Acme Limited";
    again.approved_on = 2026y / 12 / 1;
    require_added(store.approve_key(again));
    REQUIRE_EQ(store.keys().size(), std::size_t{2});
    REQUIRE(store.keys()[0] == again);
    const approved_key* const found = store.find_key("acme-2026");
    REQUIRE(found != nullptr);
    REQUIRE_EQ(found->name, std::string("Acme Limited"));
    REQUIRE(found->approved_on == 2026y / 12 / 1);

    // The same id with another key never takes the first one's place.
    const trust_store before = store;
    approved_key changed = make_key("acme-2026", 9);
    changed.name = "Someone else";
    require_error(store.approve_key(changed), errc::already_exists);
    REQUIRE(store == before);

    REQUIRE(store.find_key("missing") == nullptr);
    REQUIRE(store.find_key("") == nullptr);
    REQUIRE(store.find_key("ACME-2026") == nullptr);
}

TEST_CASE("plugin trust store: only max_approved_keys keys are approved")
{
    trust_store store;
    for (std::size_t i = 0; i < max_approved_keys; ++i)
    {
        std::string id = "key-";
        id += std::to_string(i);
        require_added(store.approve_key(make_key(id, 1)));
    }
    REQUIRE_EQ(store.keys().size(), max_approved_keys);

    const trust_store before = store;
    require_error(store.approve_key(make_key("one-too-many", 1)),
                  errc::queue_full);
    REQUIRE(store == before);

    // A key that is there is not a new one.
    approved_key again = make_key("key-0", 1);
    again.name = "Renamed";
    require_added(store.approve_key(again));
    REQUIRE_EQ(store.keys().size(), max_approved_keys);
    REQUIRE_EQ(store.keys()[0].name, std::string("Renamed"));

    // Room is made by forgetting one.
    REQUIRE(store.forget_key("key-5"));
    require_added(store.approve_key(make_key("one-too-many", 1)));
    REQUIRE_EQ(store.keys().size(), max_approved_keys);
}

TEST_CASE("plugin trust store: forget_key takes the key and its decisions")
{
    trust_store store;
    require_added(store.approve_key(make_key("key-a", 1)));
    require_added(store.approve_key(make_key("key-b", 2)));
    require_added(store.decide(make_decision("com.example.one", 1, "key-a")));
    require_added(store.decide(make_decision("com.example.two", 2, "key-b")));
    require_added(store.decide(make_decision("com.example.three", 3)));
    require_added(store.decide(make_decision("com.example.one", 4, "key-a")));
    require_added(store.decide(make_decision("com.example.one", 5, "key-b")));

    REQUIRE(!store.forget_key("key-c"));
    REQUIRE(!store.forget_key(""));
    REQUIRE_EQ(store.decisions().size(), std::size_t{5});

    REQUIRE(store.forget_key("key-a"));
    REQUIRE_EQ(store.keys().size(), std::size_t{1});
    REQUIRE(store.find_key("key-a") == nullptr);
    REQUIRE(store.find_key("key-b") != nullptr);
    // What is left is what was not signed with the key, in its order.
    REQUIRE_EQ(store.decisions().size(), std::size_t{3});
    REQUIRE(store.decisions()[0] ==
            make_decision("com.example.two", 2, "key-b"));
    REQUIRE(store.decisions()[1] == make_decision("com.example.three", 3));
    REQUIRE(store.decisions()[2] ==
            make_decision("com.example.one", 5, "key-b"));
    REQUIRE(!store.forget_key("key-a"));
}

// -- decide, find and previous ----------------------------------------------

TEST_CASE("plugin trust store: decide refuses what is not a decision")
{
    const trust_store empty;
    trust_store store;

    for (const std::string_view id :
         {"", "x", "nodots", "Com.Example.Plugin", "com..example", "a b.c",
          "com.example.", "-com.example"})
    {
        plugin_decision decision = make_decision("com.example.plugin", 1);
        decision.plugin_id = std::string(id);
        require_error(store.decide(decision), errc::invalid_argument);
    }
    plugin_decision long_id = make_decision("com.example.plugin", 1);
    long_id.plugin_id = "com." + std::string(max_id_length, 'x');
    require_error(store.decide(long_id), errc::invalid_argument);

    for (const std::string_view key : {"a b", "k/1", "k\xC3\xA9y"})
    {
        plugin_decision decision =
            make_decision("com.example.plugin", 1, "acme-2026");
        decision.key_id = std::string(key);
        require_error(store.decide(decision), errc::invalid_argument);
    }
    plugin_decision long_key = make_decision(
        "com.example.plugin", 1, std::string(max_key_id_length + 1, 'k'));
    require_error(store.decide(long_key), errc::invalid_argument);

    // The level must fit the key id: unsigned exactly when there is none.
    for (const trust_level level :
         {trust_level::official, trust_level::third_party_signed,
          trust_level::signed_unknown_key})
    {
        plugin_decision decision = make_decision("com.example.plugin", 1);
        decision.level = level;
        require_error(store.decide(decision), errc::invalid_argument);
    }
    plugin_decision signed_unsigned =
        make_decision("com.example.plugin", 1, "acme-2026");
    signed_unsigned.level = trust_level::unsigned_plugin;
    require_error(store.decide(signed_unsigned), errc::invalid_argument);
    plugin_decision no_level = make_decision("com.example.plugin", 1);
    no_level.level = static_cast<trust_level>(99);
    require_error(store.decide(no_level), errc::invalid_argument);
    plugin_decision signed_no_level =
        make_decision("com.example.plugin", 1, "acme-2026");
    signed_no_level.level = static_cast<trust_level>(99);
    require_error(store.decide(signed_no_level), errc::invalid_argument);

    plugin_decision twice = make_decision("com.example.plugin", 1);
    twice.granted = {permission::ports_read, permission::traffic_tap,
                     permission::ports_read};
    require_error(store.decide(twice), errc::invalid_argument);
    plugin_decision no_permission = make_decision("com.example.plugin", 1);
    no_permission.granted = {static_cast<permission>(99)};
    require_error(store.decide(no_permission), errc::invalid_argument);

    plugin_decision no_date = make_decision("com.example.plugin", 1);
    no_date.decided_on = std::chrono::year_month_day{};
    require_error(store.decide(no_date), errc::invalid_argument);
    plugin_decision no_such_day = make_decision("com.example.plugin", 1);
    no_such_day.decided_on = 2026y / 4 / 31;
    require_error(store.decide(no_such_day), errc::invalid_argument);
    plugin_decision too_early = make_decision("com.example.plugin", 1);
    too_early.decided_on = 1999y / 1 / 1;
    require_error(store.decide(too_early), errc::invalid_argument);

    REQUIRE(store == empty);

    // The same decisions, made right, are taken.
    require_added(store.decide(make_decision("com.example.plugin", 1)));
    require_added(
        store.decide(make_decision("com.example.plugin", 1, "acme-2026")));
    plugin_decision all = make_decision("com.example.plugin", 2);
    all.granted = {permission::ports_read,      permission::traffic_tap,
                   permission::ports_write,     permission::topology_modify,
                   permission::modifier_inline, permission::faults,
                   permission::serial_devices,  permission::fs_read,
                   permission::fs_write,        permission::data_private,
                   permission::net_listen,      permission::net_outbound,
                   permission::process_spawn,   permission::bundle_section};
    require_added(store.decide(all));
    REQUIRE_EQ(store.decisions().size(), std::size_t{3});
}

TEST_CASE("plugin trust store: a new decision replaces the old and is newest")
{
    trust_store store;
    require_added(store.decide(make_decision("com.example.one", 1)));
    require_added(store.decide(make_decision("com.example.two", 2)));
    require_added(store.decide(make_decision("com.example.three", 3)));

    plugin_decision changed = make_decision("com.example.one", 1);
    changed.enabled = false;
    changed.granted.clear();
    changed.decided_on = 2026y / 12 / 24;
    require_added(store.decide(changed));

    REQUIRE_EQ(store.decisions().size(), std::size_t{3});
    REQUIRE(store.decisions()[0].plugin_id == "com.example.two");
    REQUIRE(store.decisions()[1].plugin_id == "com.example.three");
    REQUIRE(store.decisions()[2] == changed);
    const plugin_decision* const found =
        store.find("com.example.one", hash_of(1), "");
    REQUIRE(found != nullptr);
    REQUIRE(!found->enabled);
    REQUIRE(found->granted.empty());
}

TEST_CASE("plugin trust store: find is exact on id, hash and key id")
{
    trust_store store;
    require_added(
        store.decide(make_decision("com.example.plugin", 1, "acme-2026")));
    require_added(store.decide(make_decision("com.example.free", 2)));

    const plugin_decision* const hit =
        store.find("com.example.plugin", hash_of(1), "acme-2026");
    REQUIRE(hit != nullptr);
    REQUIRE(hit == &store.decisions()[0]);

    REQUIRE(store.find("com.example.plugin", hash_of(2), "acme-2026") ==
            nullptr);
    REQUIRE(store.find("com.example.plugin", hash_of(1), "other-key") ==
            nullptr);
    REQUIRE(store.find("com.example.plugin", hash_of(1), "") == nullptr);
    REQUIRE(store.find("com.example.plugin", hash_of(1), "ACME-2026") ==
            nullptr);
    REQUIRE(store.find("com.example.other", hash_of(1), "acme-2026") ==
            nullptr);
    REQUIRE(store.find("", hash_of(1), "acme-2026") == nullptr);

    // An unsigned decision is found with an empty key id and no other.
    REQUIRE(store.find("com.example.free", hash_of(2), "") ==
            &store.decisions()[1]);
    REQUIRE(store.find("com.example.free", hash_of(2), "acme-2026") == nullptr);
    REQUIRE(store.find("com.example.free", hash_of(1), "") == nullptr);
}

TEST_CASE("plugin trust store: previous is the newest other decision")
{
    trust_store store;
    REQUIRE(store.previous("com.example.plugin", hash_of(1), "") == nullptr);

    require_added(store.decide(make_decision("com.example.plugin", 1)));
    require_added(store.decide(make_decision("com.example.other", 9)));
    // One decision and nothing else for the id: nothing before it.
    REQUIRE(store.previous("com.example.plugin", hash_of(1), "") == nullptr);
    // Another version that was never decided: the one decision is it.
    const plugin_decision* const first =
        store.previous("com.example.plugin", hash_of(2), "");
    REQUIRE(first != nullptr);
    REQUIRE_EQ(first->hash.short_text(), hash_of(1).short_text());

    require_added(store.decide(make_decision("com.example.plugin", 2)));
    require_added(
        store.decide(make_decision("com.example.plugin", 3, "acme-2026")));

    // The newest other than the one asked about.
    const plugin_decision* const third =
        store.previous("com.example.plugin", hash_of(1), "");
    REQUIRE(third != nullptr);
    REQUIRE_EQ(third->hash.short_text(), hash_of(3).short_text());
    REQUIRE_EQ(third->key_id, std::string("acme-2026"));
    const plugin_decision* const second =
        store.previous("com.example.plugin", hash_of(3), "acme-2026");
    REQUIRE(second != nullptr);
    REQUIRE_EQ(second->hash.short_text(), hash_of(2).short_text());
    // The same hash with another signer is another decision.
    const plugin_decision* const same_files =
        store.previous("com.example.plugin", hash_of(3), "");
    REQUIRE(same_files != nullptr);
    REQUIRE(same_files == &store.decisions()[3]);
    // Only decisions for this id count.
    REQUIRE(store.previous("com.example.other", hash_of(9), "") == nullptr);
    REQUIRE(store.previous("com.example.none", hash_of(9), "") == nullptr);

    // Deciding again makes a decision the newest.
    require_added(store.decide(make_decision("com.example.plugin", 1)));
    const plugin_decision* const renewed =
        store.previous("com.example.plugin", hash_of(2), "");
    REQUIRE(renewed != nullptr);
    REQUIRE_EQ(renewed->hash.short_text(), hash_of(1).short_text());
}

TEST_CASE("plugin trust store: a plugin keeps its newest decisions only")
{
    trust_store store;
    require_added(store.decide(make_decision("com.example.other", 100)));
    for (unsigned seed = 0; seed < max_decisions_per_plugin; ++seed)
        require_added(store.decide(make_decision("com.example.plugin", seed)));
    REQUIRE_EQ(store.decisions().size(), max_decisions_per_plugin + 1);
    REQUIRE(store.find("com.example.plugin", hash_of(0), "") != nullptr);

    // A ninth decision drops the oldest of this id and no other.
    require_added(store.decide(make_decision("com.example.plugin", 50)));
    REQUIRE_EQ(store.decisions().size(), max_decisions_per_plugin + 1);
    REQUIRE(store.find("com.example.plugin", hash_of(0), "") == nullptr);
    REQUIRE(store.find("com.example.plugin", hash_of(1), "") != nullptr);
    REQUIRE(store.find("com.example.plugin", hash_of(50), "") != nullptr);
    REQUIRE(store.find("com.example.other", hash_of(100), "") != nullptr);
    REQUIRE(store.decisions().front().plugin_id == "com.example.other");
    REQUIRE(store.decisions().back().plugin_id == "com.example.plugin");
    REQUIRE(store.decisions().back().hash == hash_of(50));

    // Deciding again about a version that is there drops nothing.
    require_added(store.decide(make_decision("com.example.plugin", 3)));
    REQUIRE_EQ(store.decisions().size(), max_decisions_per_plugin + 1);
    REQUIRE(store.find("com.example.plugin", hash_of(1), "") != nullptr);
    REQUIRE(store.decisions().back().hash == hash_of(3));

    // Another signer with the same files is one more decision.
    require_added(
        store.decide(make_decision("com.example.plugin", 3, "acme-2026")));
    REQUIRE(store.find("com.example.plugin", hash_of(1), "") == nullptr);
    REQUIRE(store.find("com.example.plugin", hash_of(2), "") != nullptr);
    REQUIRE(store.find("com.example.plugin", hash_of(3), "acme-2026") !=
            nullptr);
}

TEST_CASE("plugin trust store: the whole store keeps its newest decisions")
{
    trust_store store;
    for (std::size_t i = 0; i < max_decisions; ++i)
        require_added(store.decide(
            make_decision(numbered_id(i), static_cast<unsigned>(i))));
    REQUIRE_EQ(store.decisions().size(), max_decisions);
    REQUIRE(store.decisions().front().plugin_id == numbered_id(0));

    require_added(
        store.decide(make_decision(numbered_id(max_decisions), 7777)));
    REQUIRE_EQ(store.decisions().size(), max_decisions);
    REQUIRE(store.decisions().front().plugin_id == numbered_id(1));
    REQUIRE(store.decisions().back().plugin_id == numbered_id(max_decisions));
    REQUIRE(store.find(numbered_id(0), hash_of(0), "") == nullptr);

    // Replacing a decision does not push another one out.
    require_added(store.decide(make_decision(numbered_id(5), 5)));
    REQUIRE_EQ(store.decisions().size(), max_decisions);
    REQUIRE(store.decisions().front().plugin_id == numbered_id(1));
    REQUIRE(store.decisions().back().plugin_id == numbered_id(5));

    // A store at the limit still writes and reads.
    const auto parsed = parse_trust_store(to_json(store));
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == store);
}

TEST_CASE("plugin trust store: forget removes one decision")
{
    trust_store store;
    require_added(
        store.decide(make_decision("com.example.plugin", 1, "acme-2026")));
    require_added(store.decide(make_decision("com.example.plugin", 1)));
    require_added(store.decide(make_decision("com.example.plugin", 2)));

    REQUIRE(!store.forget("com.example.plugin", hash_of(3), ""));
    REQUIRE(!store.forget("com.example.plugin", hash_of(2), "acme-2026"));
    REQUIRE(!store.forget("com.example.other", hash_of(2), ""));
    REQUIRE_EQ(store.decisions().size(), std::size_t{3});

    REQUIRE(store.forget("com.example.plugin", hash_of(1), ""));
    REQUIRE_EQ(store.decisions().size(), std::size_t{2});
    REQUIRE(store.find("com.example.plugin", hash_of(1), "") == nullptr);
    REQUIRE(store.find("com.example.plugin", hash_of(1), "acme-2026") !=
            nullptr);
    REQUIRE(!store.forget("com.example.plugin", hash_of(1), ""));
}

// -- the ledger -------------------------------------------------------------

TEST_CASE("plugin trust store: serials only go up")
{
    trust_store store;
    // Nothing seen is 0, and 0 is not higher than that.
    REQUIRE(!store.note_revocation_serial(0));
    REQUIRE(store.note_revocation_serial(5));
    REQUIRE_EQ(store.ledger().revocation_serial, std::uint64_t{5});
    const trust_store seen = store;
    REQUIRE(!store.note_revocation_serial(5));
    REQUIRE(!store.note_revocation_serial(4));
    REQUIRE(!store.note_revocation_serial(0));
    REQUIRE(store == seen);
    REQUIRE(store.note_revocation_serial(6));
    REQUIRE_EQ(store.ledger().revocation_serial, std::uint64_t{6});

    // The key list has a serial of its own.
    REQUIRE_EQ(store.ledger().key_list_serial, std::uint64_t{0});
    REQUIRE(!store.note_key_list_serial(0));
    REQUIRE(store.note_key_list_serial(2));
    REQUIRE(!store.note_key_list_serial(2));
    REQUIRE(!store.note_key_list_serial(1));
    REQUIRE(store.note_key_list_serial(3));
    REQUIRE_EQ(store.ledger().key_list_serial, std::uint64_t{3});
    REQUIRE_EQ(store.ledger().revocation_serial, std::uint64_t{6});

    // A serial that the file could not hold is not kept.
    const trust_store before = store;
    REQUIRE(!store.note_revocation_serial(max_revocations_serial + 1));
    REQUIRE(!store.note_key_list_serial(max_revocations_serial + 1));
    REQUIRE(!store.note_revocation_serial(
        std::numeric_limits<std::uint64_t>::max()));
    REQUIRE(store == before);
    REQUIRE(store.note_revocation_serial(max_revocations_serial));
    REQUIRE(store.note_key_list_serial(max_revocations_serial));
    const auto parsed = parse_trust_store(to_json(store));
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == store);
}

TEST_CASE("plugin trust store: the date only goes forward")
{
    trust_store store;
    REQUIRE(!store.ledger().latest_date.has_value());

    const std::chrono::sys_days first{2026y / 11 / 14};
    REQUIRE(store.note_date(first) == first);
    REQUIRE(store.ledger().latest_date == day);

    // The clock set back returns the date that was seen and keeps it.
    const std::chrono::sys_days earlier{2026y / 11 / 1};
    const trust_store kept = store;
    REQUIRE(store.note_date(earlier) == first);
    REQUIRE(store.note_date(first) == first);
    REQUIRE(store.note_date(std::chrono::sys_days{2000y / 1 / 1}) == first);
    REQUIRE(store == kept);

    const std::chrono::sys_days later{2026y / 12 / 2};
    REQUIRE(store.note_date(later) == later);
    REQUIRE(store.ledger().latest_date == 2026y / 12 / 2);
    REQUIRE(store.note_date(first) == later);
    REQUIRE(store.ledger().latest_date == 2026y / 12 / 2);
}

TEST_CASE("plugin trust store: a date the file cannot hold is not kept")
{
    // A clock that was never set reads 1970.  It is returned as it is, and
    // is not written to a file that would then be refused.
    trust_store store;
    const std::chrono::sys_days epoch{1970y / 1 / 1};
    REQUIRE(store.note_date(epoch) == epoch);
    REQUIRE(!store.ledger().latest_date.has_value());

    const std::chrono::sys_days far{10000y / 1 / 1};
    REQUIRE(store.note_date(far) == far);
    REQUIRE(!store.ledger().latest_date.has_value());

    const std::chrono::sys_days now{day};
    REQUIRE(store.note_date(now) == now);
    REQUIRE(store.note_date(far) == far);
    REQUIRE(store.ledger().latest_date == day);
    REQUIRE(parse_trust_store(to_json(store)).has_value());
}

// -- refused text -----------------------------------------------------------

TEST_CASE("plugin trust store: text that is not a store is refused")
{
    require_refused("", "not JSON");
    require_refused("{", "not JSON");
    require_refused("7", "must be an object");
    require_refused("[]", "must be an object");
    require_refused("null", "must be an object");
    require_refused("{}", "trust_version: must be 1");
    require_refused(R"({ "trust_version": 0 })", "trust_version: must be 1");
    require_refused(R"({ "trust_version": 2 })", "trust_version: must be 1");
    require_refused(R"({ "trust_version": "1" })", "must be 1");
    require_refused(R"({ "trust_version": 1.0 })", "must be 1");
    require_refused(R"({ "trust_version": null })", "must be 1");
    require_refused(R"({ "keys": [] })", "trust_version: must be 1");

    require_refused(R"({ "trust_version": 1, "extra": 1 })",
                    "unknown key \"extra\"");
    require_refused(R"({ "trust_version": 1, "Keys": [] })", "unknown key");
    require_refused(R"({ "trust_version": 1, "trust_version": 1 })",
                    "given twice");
    require_refused(R"({ "trust_version": 1, "keys": [], "keys": [] })",
                    "given twice");
    require_refused(R"({ "trust_version": 1 } x)", "after the JSON document");
    require_refused(R"({ "trust_version": 1 }{ "trust_version": 1 })",
                    "after the JSON document");
    require_refused(R"({ "trust_version": 1, "keys": [[[[[[[]]]]]]] })",
                    "not JSON");

    require_refused(R"({ "trust_version": 1, "keys": {} })",
                    "keys: must be a list");
    require_refused(R"({ "trust_version": 1, "keys": null })",
                    "keys: must be a list");
    require_refused(R"({ "trust_version": 1, "plugins": "none" })",
                    "plugins: must be a list");
    require_refused(R"({ "trust_version": 1, "ledger": [] })",
                    "ledger: must be an object");
    require_refused(R"({ "trust_version": 1, "keys": [1] })",
                    "keys[0]: must be an object");
    require_refused(R"({ "trust_version": 1, "plugins": [{}, 1] })",
                    "plugins[0].plugin_id: is required");
}

TEST_CASE("plugin trust store: a key that is not right is refused")
{
    // The base is accepted.
    REQUIRE(parse_trust_store(with_key(key_object("acme-2026"))).has_value());

    const std::string key = key_object("acme-2026");
    require_refused(with_key(replaced(key, "}", R"(, "extra": 1 })")),
                    "keys[0]: unknown key \"extra\"");
    require_refused(with_key(replaced(key, R"("key_id": "acme-2026", )", "")),
                    "keys[0].key_id: is required");
    require_refused(with_key(replaced(key, R"("name": "Acme Ltd", )", "")),
                    "keys[0].name: is required");
    require_refused(
        with_key(replaced(key, R"(, "approved_on": "2026-11-14")", "")),
        "keys[0].approved_on: is required");
    require_refused(with_key(replaced(key, "acme-2026", "a b")),
                    "keys[0].key_id: cannot be \"a b\"");
    require_refused(with_key(replaced(key, "acme-2026", "")),
                    "keys[0].key_id: cannot be");
    require_refused(with_key(replaced(key, R"("acme-2026")", "7")),
                    "keys[0].key_id: must be a string");

    const std::string hex = repeated("01", 32);
    require_refused(with_key(replaced(key, hex, repeated("0A", 32))),
                    "keys[0].public_key: must be 64 lower-case hex");
    require_refused(with_key(replaced(key, hex, repeated("01", 31))),
                    "keys[0].public_key: must be 64 lower-case hex");
    require_refused(with_key(replaced(key, hex, repeated("01", 33))),
                    "keys[0].public_key: must be 64 lower-case hex");
    require_refused(with_key(replaced(key, hex, repeated("0g", 32))),
                    "keys[0].public_key: must be 64 lower-case hex");
    require_refused(with_key(replaced(key, '"' + hex + '"', "[]")),
                    "keys[0].public_key: must be a string");

    require_refused(with_key(replaced(key, "Acme Ltd", "")),
                    "keys[0].name: must be 1 to 100 bytes");
    require_refused(with_key(replaced(key, "Acme Ltd", std::string(101, 'n'))),
                    "keys[0].name: must be 1 to 100 bytes");
    require_refused(with_key(replaced(key, "Acme Ltd", "a\\u0001b")),
                    "keys[0].name: must be 1 to 100 bytes");
    require_refused(with_key(replaced(key, "Acme Ltd", "a\\u202eb")),
                    "keys[0].name: must be 1 to 100 bytes");

    require_refused(with_key(replaced(key, "2026-11-14", "2026-02-30")),
                    "keys[0].approved_on: must be a date");
    require_refused(with_key(replaced(key, "2026-11-14", "14.11.2026")),
                    "keys[0].approved_on: must be a date");
    require_refused(with_key(replaced(key, "2026-11-14", "1999-11-14")),
                    "keys[0].approved_on: must be a date");
    require_refused(with_key(replaced(key, R"("2026-11-14")", "20261114")),
                    "keys[0].approved_on: must be a string");

    // The same id twice: with another key it is a conflict, and with the
    // same key it is still a repeat, which a file may not hold.
    const std::string other_key =
        replaced(key, repeated("01", 32), repeated("02", 32));
    require_refused(document(key + ", " + other_key, "", good_ledger),
                    "keys[1]: key_id \"acme-2026\" has another public key");
    require_refused(document(key + ", " + key, "", good_ledger),
                    "keys[1]: key_id \"acme-2026\" is given twice");
    // Two keys with different ids are fine, even with the same bytes.
    REQUIRE(parse_trust_store(document(key + ", " + key_object("second-key"),
                                       "", good_ledger))
                .has_value());
}

TEST_CASE("plugin trust store: too many keys are refused")
{
    std::string keys;
    for (std::size_t i = 0; i < max_approved_keys; ++i)
    {
        if (i != 0)
            keys += ", ";
        std::string id = "key-";
        id += std::to_string(i);
        keys += key_object(id);
    }
    const auto full = parse_trust_store(document(keys, "", good_ledger));
    REQUIRE(full.has_value());
    REQUIRE_EQ(full->keys().size(), max_approved_keys);

    keys += ", ";
    keys += key_object("one-too-many");
    require_refused(document(keys, "", good_ledger),
                    "keys: has more than 256 entries");
}

TEST_CASE("plugin trust store: a decision that is not right is refused")
{
    // The base is accepted.
    const std::string decision = decision_object("com.example.plugin", 1);
    REQUIRE(parse_trust_store(with_decision(decision)).has_value());

    require_refused(with_decision(replaced(decision, "}", R"(, "extra": 1 })")),
                    "plugins[0]: unknown key \"extra\"");
    for (const std::string_view missing :
         {R"("plugin_id": "com.example.plugin", )", R"("enabled": true, )",
          R"("run_without_sandbox": false, )",
          R"("level": "third_party_signed", )"})
    {
        require_refused(with_decision(replaced(decision, missing, "")),
                        "is required");
    }
    require_refused(
        with_decision(replaced(
            decision, R"("granted": ["ports.read", "traffic.tap"], )", "")),
        "plugins[0].granted: is required");
    require_refused(with_decision(replaced(
                        decision, R"(, "decided_on": "2026-11-14")", "")),
                    "plugins[0].decided_on: is required");

    require_refused(with_decision(replaced(decision, "com.example.plugin",
                                           "Com.Example.Plugin")),
                    "plugins[0].plugin_id: cannot be");
    require_refused(
        with_decision(replaced(decision, "com.example.plugin", "nodots")),
        "plugins[0].plugin_id: cannot be \"nodots\"");
    require_refused(
        with_decision(replaced(decision, R"("com.example.plugin")", "1")),
        "plugins[0].plugin_id: must be a string");

    const std::string hex = to_hex(hash_of(1).bytes);
    std::string upper = hex;
    upper[0] = 'A';
    upper[1] = 'F';
    require_refused(with_decision(replaced(decision, hex, upper)),
                    "plugins[0].plugin_hash: must be 128 lower-case hex");
    require_refused(with_decision(replaced(decision, hex, hex.substr(0, 127))),
                    "plugins[0].plugin_hash: must be 128 lower-case hex");
    require_refused(with_decision(replaced(decision, hex, hex + "00")),
                    "plugins[0].plugin_hash: must be 128 lower-case hex");
    require_refused(with_decision(replaced(decision, hex, repeated("0", 64))),
                    "plugins[0].plugin_hash: must be 128 lower-case hex");

    require_refused(with_decision(replaced(decision, "acme-2026", "a b")),
                    "plugins[0].key_id: cannot be \"a b\"");
    require_refused(with_decision(replaced(decision, "acme-2026", "")),
                    "plugins[0].key_id: cannot be");
    require_refused(with_decision(replaced(decision, R"("acme-2026")", "3")),
                    "plugins[0].key_id: must be a string");

    require_refused(
        with_decision(replaced(decision, "third_party_signed", "x")),
        "plugins[0].level: unknown value \"x\"");
    require_refused(
        with_decision(replaced(decision, "third_party_signed", "Official")),
        "plugins[0].level: unknown value");
    require_refused(with_decision(replaced(decision, "third_party_signed",
                                           "unsigned_plugin")),
                    "plugins[0].level: unknown value");
    require_refused(
        with_decision(replaced(decision, R"("third_party_signed")", "1")),
        "plugins[0].level: must be a string");
    // A level must fit the key id.
    require_refused(
        with_decision(replaced(decision, "third_party_signed", "unsigned")),
        "plugins[0].level: does not fit the key_id");
    const std::string no_key =
        replaced(decision, R"("key_id": "acme-2026", )", "");
    require_refused(with_decision(no_key),
                    "plugins[0].level: does not fit the key_id");
    require_refused(
        with_decision(replaced(no_key, "third_party_signed", "official")),
        "plugins[0].level: does not fit the key_id");
    require_refused(with_decision(replaced(no_key, "third_party_signed",
                                           "signed_unknown_key")),
                    "plugins[0].level: does not fit the key_id");
    REQUIRE(parse_trust_store(with_decision(replaced(
                                  no_key, "third_party_signed", "unsigned")))
                .has_value());

    require_refused(with_decision(replaced(decision, "true", "1")),
                    "plugins[0].enabled: must be true or false");
    require_refused(with_decision(replaced(decision, "false", R"("no")")),
                    "plugins[0].run_without_sandbox: must be true or false");

    require_refused(
        with_decision(replaced(decision, R"(["ports.read", "traffic.tap"])",
                               R"("ports.read")")),
        "plugins[0].granted: must be a list");
    require_refused(with_decision(replaced(decision, R"("traffic.tap")", "2")),
                    "plugins[0].granted: must hold only strings");
    require_refused(
        with_decision(replaced(decision, "traffic.tap", "traffic.tapp")),
        "plugins[0].granted: unknown permission \"traffic.tapp\"");
    require_refused(
        with_decision(replaced(decision, "traffic.tap", "Ports.Read")),
        "plugins[0].granted: unknown permission");
    require_refused(
        with_decision(replaced(decision, "traffic.tap", "ports.read")),
        "plugins[0].granted: holds \"ports.read\" twice");

    require_refused(
        with_decision(replaced(decision, "2026-11-14", "2026-13-14")),
        "plugins[0].decided_on: must be a date");
    require_refused(
        with_decision(replaced(decision, "2026-11-14", "1999-12-31")),
        "plugins[0].decided_on: must be a date");
    require_refused(
        with_decision(replaced(decision, R"("2026-11-14")", "null")),
        "plugins[0].decided_on: must be a string");

    // The place of the entry is in the line.
    require_refused(
        with_decision(decision + ", " +
                      replaced(decision_object("com.example.two", 2), "true",
                               R"("maybe")")),
        "plugins[1].enabled");
}

TEST_CASE("plugin trust store: a file that repeats a decision is refused")
{
    const std::string one = decision_object("com.example.plugin", 1);
    const std::string two = decision_object("com.example.plugin", 2);
    REQUIRE(parse_trust_store(with_decision(one + ", " + two)).has_value());

    // The same id, hash and key id twice, whatever else differs.
    require_refused(with_decision(one + ", " + one),
                    "plugins[1]: a decision for this plugin_id");
    require_refused(
        with_decision(one + ", " + two + ", " + replaced(one, "false", "true")),
        "plugins[2]: a decision for this plugin_id");
    // The same files from another signer, or none, is another decision.
    const std::string free =
        replaced(replaced(one, R"("key_id": "acme-2026", )", ""),
                 "third_party_signed", "unsigned");
    REQUIRE(parse_trust_store(with_decision(one + ", " + free)).has_value());
    require_refused(with_decision(free + ", " + free), "plugins[1]");

    // Eight for one id is the most, and a ninth is not dropped silently.
    std::string eight;
    for (unsigned seed = 0; seed < max_decisions_per_plugin; ++seed)
    {
        if (seed != 0)
            eight += ", ";
        eight += decision_object("com.example.plugin", seed);
    }
    const auto parsed = parse_trust_store(with_decision(eight));
    REQUIRE(parsed.has_value());
    REQUIRE_EQ(parsed->decisions().size(), max_decisions_per_plugin);
    std::string nine = eight;
    nine += ", ";
    nine += decision_object("com.example.plugin", 8);
    require_refused(with_decision(nine), "plugins[8]: more than 8 decisions");
    // Another id after the eight is fine.
    eight += ", ";
    eight += decision_object("com.example.other", 8);
    REQUIRE(parse_trust_store(with_decision(eight)).has_value());
}

TEST_CASE("plugin trust store: too many decisions are refused")
{
    std::string plugins;
    for (std::size_t i = 0; i < max_decisions; ++i)
    {
        if (i != 0)
            plugins += ", ";
        plugins += decision_object(numbered_id(i), static_cast<unsigned>(i));
    }
    const auto full = parse_trust_store(document("", plugins, good_ledger));
    REQUIRE(full.has_value());
    REQUIRE_EQ(full->decisions().size(), max_decisions);
    REQUIRE(full->decisions().front().plugin_id == numbered_id(0));
    REQUIRE(full->decisions().back().plugin_id ==
            numbered_id(max_decisions - 1));

    plugins += ", ";
    plugins += decision_object(numbered_id(max_decisions), 7777);
    require_refused(document("", plugins, good_ledger),
                    "plugins: has more than 1024 entries");
}

TEST_CASE("plugin trust store: a ledger that is not right is refused")
{
    const auto with_ledger = [](std::string_view ledger) {
        return document("", "", ledger);
    };
    REQUIRE(parse_trust_store(with_ledger(good_ledger)).has_value());

    require_refused(with_ledger("7"), "ledger: must be an object");
    require_refused(with_ledger(R"({ "revocation_serial": 1,
                                     "key_list_serial": 1, "x": 1 })"),
                    "ledger: unknown key \"x\"");
    require_refused(with_ledger(R"({ "key_list_serial": 1 })"),
                    "ledger.revocation_serial: is required");
    require_refused(with_ledger(R"({ "revocation_serial": 1 })"),
                    "ledger.key_list_serial: is required");
    for (const std::string_view bad :
         {"-1", "9007199254740992", "18446744073709551615", "1.5", "1e3",
          R"("7")", "null", "true"})
    {
        std::string ledger = R"({ "revocation_serial": )";
        ledger += bad;
        ledger += R"(, "key_list_serial": 1 })";
        require_refused(with_ledger(ledger),
                        "ledger.revocation_serial: must be a whole number "
                        "from 0 to 9007199254740991");
        std::string other = R"({ "revocation_serial": 1, "key_list_serial": )";
        other += bad;
        other += " }";
        require_refused(with_ledger(other),
                        "ledger.key_list_serial: must be a whole number");
    }
    require_refused(
        with_ledger(R"({ "revocation_serial": 1, "key_list_serial": 1,
                         "latest_date": "2026-02-30" })"),
        "ledger.latest_date: must be a date");
    require_refused(
        with_ledger(R"({ "revocation_serial": 1, "key_list_serial": 1,
                         "latest_date": "1999-01-01" })"),
        "ledger.latest_date: must be a date");
    require_refused(
        with_ledger(R"({ "revocation_serial": 1, "key_list_serial": 1,
                         "latest_date": 20261114 })"),
        "ledger.latest_date: must be a string");
    require_refused(
        with_ledger(R"({ "revocation_serial": 1, "key_list_serial": 1,
                         "latest_date": null })"),
        "ledger.latest_date: must be a string");
}

TEST_CASE("plugin trust store: only the first problem is told")
{
    // A key that is wrong comes before a decision that is wrong.
    const std::string key =
        replaced(key_object("acme-2026"), "acme-2026", "a b");
    const std::string decision =
        replaced(decision_object("com.example.plugin", 1), "true", "1");
    std::string explain;
    const auto parsed =
        parse_trust_store(document(key, decision, "7"), &explain);
    REQUIRE(parsed.has_error());
    REQUIRE(explain.find("keys[0].key_id") == 0);
    REQUIRE(explain.find("plugins") == std::string::npos);
    REQUIRE(explain.find("ledger") == std::string::npos);
}

TEST_CASE("plugin trust store: text over the limit is refused")
{
    // Spaces after the document would be allowed, but not that many.
    std::string text = R"({ "trust_version": 1 })";
    text.append(max_trust_store_size, ' ');
    require_refused(text, "trust store: is larger than 1048576 bytes");

    // A store of exactly the limit is not too large.
    text = R"({ "trust_version": 1 })";
    text.resize(max_trust_store_size, ' ');
    REQUIRE(parse_trust_store(text).has_value());
}

// -- files ------------------------------------------------------------------

TEST_CASE("plugin trust store: a file that is not there is an empty store")
{
    const vss_test_support::scratch_directory scratch;
    std::string explain = "old";
    const auto loaded =
        load_trust_store(scratch.path() / "trust.json", &explain);
    REQUIRE(loaded.has_value());
    REQUIRE(explain.empty());
    REQUIRE(*loaded == trust_store{});

    // A directory that is not there is no file either.
    const auto deeper = load_trust_store(scratch.path() / "no" / "trust.json");
    REQUIRE(deeper.has_value());
    REQUIRE(*deeper == trust_store{});
}

TEST_CASE("plugin trust store: a saved store loads again")
{
    const vss_test_support::scratch_directory scratch;
    const fs::path path = scratch.path() / "trust.json";
    const trust_store store = full_store();

    REQUIRE(save_trust_store(path, store).has_value());
    REQUIRE_EQ(read_file(path), to_json(store));
    // The text on disk is what was written, with no change of line ends.
    REQUIRE(read_file(path).find('\r') == std::string::npos);

    std::string explain = "old";
    const auto loaded = load_trust_store(path, &explain);
    REQUIRE(loaded.has_value());
    REQUIRE(explain.empty());
    REQUIRE(*loaded == store);

    // Saving again over the file replaces it and leaves nothing beside it.
    trust_store changed = store;
    require_added(changed.decide(make_decision("com.example.new", 42)));
    REQUIRE(save_trust_store(path, changed).has_value());
    REQUIRE(save_trust_store(path, changed).has_value());
    REQUIRE_EQ(count_entries(scratch.path()), std::size_t{1});
    REQUIRE(!fs::exists(fs::path(path.string() + ".tmp")));
    const auto again = load_trust_store(path);
    REQUIRE(again.has_value());
    REQUIRE(*again == changed);
    REQUIRE(!(*again == store));
}

TEST_CASE("plugin trust store: a store is saved over an old file")
{
    const vss_test_support::scratch_directory scratch;
    const fs::path path = scratch.path() / "trust.json";
    write_file(path, "old text that is not a store");
    // A leftover of an earlier run is overwritten, not read.
    write_file(fs::path(path.string() + ".tmp"), "left behind");

    REQUIRE(save_trust_store(path, trust_store{}).has_value());
    REQUIRE_EQ(read_file(path), to_json(trust_store{}));
    REQUIRE_EQ(count_entries(scratch.path()), std::size_t{1});
}

TEST_CASE("plugin trust store: saving where there is no directory fails")
{
    const vss_test_support::scratch_directory scratch;
    const fs::path directory = scratch.path() / "missing";
    const auto saved = save_trust_store(directory / "trust.json", full_store());
    REQUIRE(saved.has_error());
    REQUIRE(saved.error() == errc::io_error);
    // Nothing was made, not the directory and not a file in the scratch one.
    REQUIRE(!fs::exists(directory));
    REQUIRE_EQ(count_entries(scratch.path()), std::size_t{0});
}

TEST_CASE("plugin trust store: a save that cannot finish leaves the old file")
{
    const vss_test_support::scratch_directory scratch;
    // A directory has the name of the file, so the new file cannot take its
    // place.  The directory is as it was and the new file is removed.
    const fs::path path = scratch.path() / "trust.json";
    fs::create_directory(path);
    write_file(path / "inside", "kept");

    const auto saved = save_trust_store(path, full_store());
    REQUIRE(saved.has_error());
    REQUIRE(saved.error() == errc::io_error);
    REQUIRE(fs::is_directory(path));
    REQUIRE_EQ(read_file(path / "inside"), std::string("kept"));
    REQUIRE_EQ(count_entries(scratch.path()), std::size_t{1});
}

TEST_CASE("plugin trust store: a file that cannot be used is an error")
{
    const vss_test_support::scratch_directory scratch;

    // Text that is not a store: the reader's error, with its line.
    const fs::path bad = scratch.path() / "bad.json";
    write_file(bad, R"({ "trust_version": 2 })");
    std::string explain;
    const auto refused = load_trust_store(bad, &explain);
    REQUIRE(refused.has_error());
    REQUIRE(refused.error() == errc::config_error);
    REQUIRE(explain.find("trust_version: must be 1") != std::string::npos);

    const fs::path empty = scratch.path() / "empty.json";
    write_file(empty, "");
    const auto nothing = load_trust_store(empty, &explain);
    REQUIRE(nothing.has_error());
    REQUIRE(nothing.error() == errc::config_error);
    REQUIRE(!explain.empty());

    // A file over the limit is refused.
    const fs::path big = scratch.path() / "big.json";
    std::string text = R"({ "trust_version": 1 })";
    text.append(max_trust_store_size, ' ');
    write_file(big, text);
    const auto too_big = load_trust_store(big, &explain);
    REQUIRE(too_big.has_error());
    REQUIRE(too_big.error() == errc::config_error);
    REQUIRE(explain.find("is larger than") != std::string::npos);

    // Something that is there and is not a file cannot be read.
    explain.clear();
    const auto directory = load_trust_store(scratch.path(), &explain);
    REQUIRE(directory.has_error());
    REQUIRE(directory.error() == errc::io_error);
    REQUIRE(!explain.empty());
    REQUIRE(load_trust_store(scratch.path()).has_error());
}
