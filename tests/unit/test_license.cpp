//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Licence payloads, the state a licence is in and what it grants
// (docs/licensing.md).
//

#include <vss/license/license.hpp>
#include <vss/license/sign.hpp>
#include <vss/license/test_keys.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace vss::license;
using std::chrono::hours;
using std::chrono::minutes;
using std::chrono::seconds;
using std::chrono::sys_days;
namespace sf = vss::license::signed_file;

namespace
{

time_point at(int year, unsigned month, unsigned day, int hour = 0)
{
    return time_point{
               sys_days{std::chrono::year{year} / std::chrono::month{month} /
                        std::chrono::day{day}}} +
           hours{hour};
}

std::span<const std::uint8_t> bytes_of(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

/// An annual Pro licence for 13 November 2026 to 13 November 2027.
licence annual()
{
    licence value;
    value.licence_id = "VSS-2026-000123";
    value.kind = licence_kind::annual;
    value.edition = edition::pro;
    value.licensee_name = "Example Lab Pty Ltd";
    value.licensee_contact = "Test team";
    value.issued_at = at(2026, 11, 13, 2);
    value.starts_at = at(2026, 11, 13);
    value.expires_at = at(2027, 11, 13);
    value.grace_days = 28;
    value.seats = 3;
    value.features = {"plugins.official", "driver.signed", "support"};
    return value;
}

std::string signed_text(const licence& value,
                        std::string_view purpose = licence_purpose)
{
    const auto payload = write_payload(value);
    REQUIRE(payload);
    return sf::write(signing::sign(purpose, test_keys::key_id,
                                   bytes_of(*payload),
                                   test_keys::private_key()));
}

std::string signed_payload(std::string_view payload)
{
    return sf::write(signing::sign(licence_purpose, test_keys::key_id,
                                   bytes_of(payload),
                                   test_keys::private_key()));
}

context on(time_point now)
{
    context where;
    where.now = now;
    where.computer_name = "LAB-RIG-07";
    where.keys = test_keys::trusted();
    return where;
}

/// The payload of `annual()` with one piece of text replaced.
std::string payload_with(std::string_view from, std::string_view to)
{
    auto text = write_payload(annual());
    REQUIRE(text);
    const auto at_position = text->find(from);
    REQUIRE(at_position != std::string::npos);
    text->replace(at_position, from.size(), to);
    return *text;
}

void expect_payload_error(const std::string& payload, errc expected)
{
    const auto parsed = parse_payload(bytes_of(payload));
    REQUIRE(!parsed);
    REQUIRE(parsed.error() == expected);
}

} // namespace

TEST_CASE("licence: times are read and written in one form")
{
    const auto parsed = parse_time("2026-11-13T02:10:05Z");
    REQUIRE(parsed);
    REQUIRE(*parsed == at(2026, 11, 13, 2) + minutes{10} + seconds{5});
    REQUIRE_EQ(format_time(*parsed), std::string("2026-11-13T02:10:05Z"));
    REQUIRE(parse_time("2028-02-29T23:59:59Z"));

    for (const char* bad :
         {"", "2026-11-13", "2026-11-13T02:10:05", "2026-11-13T02:10:05+00",
          "2026-11-13 02:10:05Z", "2026-13-01T00:00:00Z",
          "2027-02-29T00:00:00Z", "2026-11-13T24:00:00Z",
          "2026-11-13T00:60:00Z", "2026-11-13T00:00:60Z",
          "1999-12-31T00:00:00Z", "2026-11-13T02:10:05z",
          "2026-1 -13T02:10:05Z", "2026-11-13T02:10:05Z "})
    {
        REQUIRE(!parse_time(bad));
    }
}

TEST_CASE("licence: payload round trip")
{
    licence value = annual();
    value.machines = {"LAB-RIG-*", "bench-3"};
    value.updates_until = value.expires_at;
    value.replaces = "VSS-2025-000007";
    value.clock_reset = "reset-2026-11-13-a";
    const auto text = write_payload(value);
    REQUIRE(text);
    const auto parsed = parse_payload(bytes_of(*text));
    REQUIRE(parsed);
    REQUIRE(*parsed == value);

    licence plain = annual();
    plain.licensee_contact.clear();
    const auto plain_text = write_payload(plain);
    REQUIRE(plain_text);
    REQUIRE(plain_text->find("contact") == std::string::npos);
    REQUIRE(plain_text->find("updates_until") == std::string::npos);
    REQUIRE(*parse_payload(bytes_of(*plain_text)) == plain);
}

TEST_CASE("licence: the documented payload is read")
{
    const std::string_view payload = R"({
      "licence_id": "VSS-2026-000123",
      "kind": "annual",
      "edition": "pro",
      "licensee": { "name": "Example Lab Pty Ltd", "contact": "Test team" },
      "issued_at": "2026-11-13T02:10:00Z",
      "starts_at": "2026-11-13T00:00:00Z",
      "expires_at": "2027-11-13T23:59:59Z",
      "grace_days": 28,
      "seats": 3,
      "features": ["plugins.official", "driver.signed", "support"],
      "machines": [],
      "updates_until": null,
      "replaces": null,
      "critical": []
    })";
    const auto parsed = parse_payload(bytes_of(payload));
    REQUIRE(parsed);
    REQUIRE_EQ(parsed->licence_id, std::string("VSS-2026-000123"));
    REQUIRE(parsed->kind == licence_kind::annual);
    REQUIRE(parsed->edition == edition::pro);
    REQUIRE_EQ(parsed->licensee_name, std::string("Example Lab Pty Ltd"));
    REQUIRE_EQ(parsed->seats, 3);
    REQUIRE_EQ(parsed->grace_days, 28);
    REQUIRE_EQ(parsed->features.size(), std::size_t{3});
    REQUIRE(parsed->machines.empty());
    REQUIRE(!parsed->updates_until);
    REQUIRE(parsed->replaces.empty());
}

TEST_CASE("licence: a payload this version cannot honour is refused")
{
    REQUIRE(
        parse_payload(bytes_of(payload_with("\"seats\":3", "\"seats\":3"))));

    expect_payload_error("", errc::malformed);
    expect_payload_error("[]", errc::malformed);
    expect_payload_error("{}", errc::malformed);
    // The same key twice, wherever it is.
    expect_payload_error(
        payload_with("\"seats\":3", "\"seats\":3,\"seats\":900"),
        errc::malformed);
    expect_payload_error(
        payload_with("\"name\":", "\"name\":\"Someone Else\",\"name\":"),
        errc::malformed);

    expect_payload_error(payload_with("VSS-2026-000123", "vss 1"),
                         errc::malformed);
    expect_payload_error(payload_with("\"annual\"", "\"perpetual\""),
                         errc::malformed);
    expect_payload_error(payload_with("\"pro\"", "\"community\""),
                         errc::malformed);
    expect_payload_error(payload_with("Example Lab Pty Ltd", ""),
                         errc::malformed);
    expect_payload_error(payload_with("Example Lab Pty Ltd", "Line\\nbreak"),
                         errc::malformed);
    expect_payload_error(payload_with("\"seats\":3", "\"seats\":-1"),
                         errc::malformed);
    expect_payload_error(payload_with("\"seats\":3", "\"seats\":3.5"),
                         errc::malformed);
    expect_payload_error(payload_with("\"seats\":3", "\"seats\":\"3\""),
                         errc::malformed);
    expect_payload_error(
        payload_with("\"grace_days\":28", "\"grace_days\":4000"),
        errc::malformed);
    expect_payload_error(payload_with("\"driver.signed\"", "\"Driver Signed\""),
                         errc::malformed);
    expect_payload_error(payload_with("\"driver.signed\"", "7"),
                         errc::malformed);
    expect_payload_error(
        payload_with("\"machines\":[]", "\"machines\":[\"*\"]"),
        errc::malformed);
    expect_payload_error(
        payload_with("\"machines\":[]", "\"machines\":[\"rig 7\"]"),
        errc::malformed);
    expect_payload_error(payload_with("\"machines\":[]", "\"machines\":{}"),
                         errc::malformed);
    // Ends before it starts.
    expect_payload_error(
        payload_with("2027-11-13T00:00:00Z", "2026-11-12T00:00:00Z"),
        errc::malformed);
    expect_payload_error(payload_with("2027-11-13T00:00:00Z", "next year"),
                         errc::malformed);
    expect_payload_error(
        payload_with("\"licensee\":{", "\"licensee\":[{").append("]"),
        errc::malformed);
}

TEST_CASE("licence: unknown fields are ignored unless they are critical")
{
    const auto ignored = parse_payload(bytes_of(
        payload_with("\"seats\":3", "\"seats\":3,\"colour\":\"teal\"")));
    REQUIRE(ignored);
    REQUIRE(*ignored == annual());

    // A critical field this version knows is fine.
    REQUIRE(parse_payload(bytes_of(
        payload_with("\"critical\":[]", "\"critical\":[\"machines\"]"))));

    expect_payload_error(payload_with("\"critical\":[]",
                                      "\"critical\":[\"max_ports\"],"
                                      "\"max_ports\":4"),
                         errc::newer_version);
    expect_payload_error(payload_with("\"critical\":[]", "\"critical\":7"),
                         errc::malformed);
}

TEST_CASE("licence: states across the term")
{
    const std::string text = signed_text(annual());
    struct row
    {
        time_point now;
        state expected_state;
        reason expected_reason;
        int days_left;
        bool active;
    };
    const row rows[] = {
        // Before the start. Issued at 02:00, so the day before is inside
        // the clock tolerance.
        {at(2026, 11, 12, 12), state::not_yet_valid, reason::not_yet_valid, 0,
         false},
        {at(2026, 11, 13), state::valid, reason::ok, 365, true},
        {at(2027, 6, 1), state::valid, reason::ok, 165, true},
        // 31 days left is valid, 30 is expiring.
        {at(2027, 10, 13), state::valid, reason::ok, 31, true},
        {at(2027, 10, 14), state::expiring, reason::ok, 30, true},
        {at(2027, 11, 12, 12), state::expiring, reason::ok, 0, true},
        // The last second of the term.
        {at(2027, 11, 13), state::expiring, reason::ok, 0, true},
        {at(2027, 11, 13) + seconds{1}, state::grace, reason::ok, 27, true},
        {at(2027, 12, 1), state::grace, reason::ok, 10, true},
        // The last second of the grace period.
        {at(2027, 12, 11), state::grace, reason::ok, 0, true},
        {at(2027, 12, 11) + seconds{1}, state::expired, reason::expired, 0,
         false},
        {at(2030, 1, 1), state::expired, reason::expired, 0, false},
    };
    for (const row& expected : rows)
    {
        const status result = evaluate(text, on(expected.now));
        REQUIRE(result.state == expected.expected_state);
        REQUIRE(result.reason == expected.expected_reason);
        REQUIRE_EQ(result.days_left, expected.days_left);
        REQUIRE_EQ(result.active(), expected.active);
        REQUIRE(result.value);
        REQUIRE(*result.value == annual());
        REQUIRE_EQ(result.key_id, std::string(test_keys::key_id));
        REQUIRE(result.grace_ends == at(2027, 12, 11));
    }
}

TEST_CASE("licence: a trial has no grace period and a short warning")
{
    licence trial = annual();
    trial.kind = licence_kind::trial;
    trial.expires_at = at(2026, 11, 27);
    trial.grace_days = 0;
    trial.seats = 1;
    const std::string text = signed_text(trial);

    REQUIRE(evaluate(text, on(at(2026, 11, 20))).state == state::valid);
    REQUIRE(evaluate(text, on(at(2026, 11, 23))).state == state::valid);
    const status last_days = evaluate(text, on(at(2026, 11, 24)));
    REQUIRE(last_days.state == state::expiring);
    REQUIRE_EQ(last_days.days_left, 3);
    const status over = evaluate(text, on(at(2026, 11, 27) + seconds{1}));
    REQUIRE(over.state == state::expired);
    REQUIRE(!over.active());
}

TEST_CASE("licence: files that are not valid licences")
{
    const context where = on(at(2027, 1, 1));
    const auto expect = [&where](std::string_view text, reason expected)
    {
        const status result = evaluate(text, where);
        REQUIRE(result.state == state::invalid);
        REQUIRE(result.reason == expected);
        REQUIRE(!result.active());
        REQUIRE(!result.value);
        REQUIRE(result.key_id.empty());
    };

    expect("", reason::malformed);
    expect("hello", reason::malformed);
    expect(std::string(sf::max_file_size + 1, 'x'), reason::malformed);

    std::string text = signed_text(annual());
    const auto format = text.find("\"format\":1");
    REQUIRE(format != std::string::npos);
    std::string newer = text;
    newer.replace(format, 10, "\"format\":2");
    expect(newer, reason::newer_version);

    // One character of the payload changed.
    std::string tampered = text;
    const auto payload = tampered.find("\"payload\":\"") + 11;
    tampered[payload + 20] = tampered[payload + 20] == 'A' ? 'B' : 'A';
    expect(tampered, reason::bad_signature);

    // Signed, but not as a licence.
    expect(signed_text(annual(), revocations_purpose), reason::malformed);

    // Signed, and the payload is not one this version reads.
    expect(signed_payload("{\"licence_id\":\"VSS-1\"}"), reason::malformed);
    expect(signed_payload(
               payload_with("\"critical\":[]", "\"critical\":[\"future\"]")),
           reason::newer_version);

    // A key this build does not know.
    context no_keys = where;
    no_keys.keys = {};
    const status unknown = evaluate(text, no_keys);
    REQUIRE(unknown.state == state::invalid);
    REQUIRE(unknown.reason == reason::newer_version);
}

TEST_CASE("licence: a product build refuses a licence signed with the test key")
{
    const auto keys = sf::product_trust_list();
    context where = on(at(2027, 1, 1));
    where.keys = keys;
    const status result = evaluate(signed_text(annual()), where);
    REQUIRE(result.state == state::invalid);
    REQUIRE(!result.active());
    REQUIRE(!result.value);

    // The same licence, signed with the test key under the id of each real
    // key, is refused for its signature.
    const auto payload = write_payload(annual());
    REQUIRE(payload);
    for (const std::string_view id : {"lic-2026-a", "lic-2026-b"})
    {
        const std::string forged = sf::write(signing::sign(
            licence_purpose, id, bytes_of(*payload), test_keys::private_key()));
        const status refused = evaluate(forged, where);
        REQUIRE(refused.state == state::invalid);
        REQUIRE(refused.reason == reason::bad_signature);
        REQUIRE(!refused.value);
        REQUIRE(!check(refused, "plugins.official").granted);
    }
}

TEST_CASE("licence: revocation by licence id and by key")
{
    const std::string text = signed_text(annual());
    context where = on(at(2027, 1, 1));
    REQUIRE(evaluate(text, where).state == state::valid);

    revocation_list list;
    list.sequence = 4;
    list.licence_ids = {"VSS-2026-000001", "VSS-2026-000123"};
    where.revocations = &list;
    status result = evaluate(text, where);
    REQUIRE(result.state == state::invalid);
    REQUIRE(result.reason == reason::revoked);
    // The page can still say whose licence was withdrawn.
    REQUIRE(result.value);
    REQUIRE(!check(result, "plugins.official").granted);

    list.licence_ids = {"VSS-2026-000001"};
    REQUIRE(evaluate(text, where).state == state::valid);

    // The key leaked after this licence was issued: it keeps working.
    list.keys = {{std::string(test_keys::key_id), at(2026, 12, 1)}};
    REQUIRE(evaluate(text, where).state == state::valid);
    // The key leaked before it was issued.
    list.keys = {{std::string(test_keys::key_id), at(2026, 11, 1)}};
    REQUIRE(evaluate(text, where).reason == reason::revoked);
    // The key is withdrawn outright.
    list.keys = {{std::string(test_keys::key_id), std::nullopt}};
    REQUIRE(evaluate(text, where).reason == reason::revoked);
    // Another key's revocation does not touch it.
    list.keys = {{"lic-2020-a", std::nullopt}};
    REQUIRE(evaluate(text, where).state == state::valid);
}

TEST_CASE("licence: a key's window and revocation in the trust list")
{
    const std::string text = signed_text(annual());
    std::vector<sf::trusted_key> keys(test_keys::trusted().begin(),
                                      test_keys::trusted().end());
    context where = on(at(2027, 1, 1));
    where.keys = keys;

    keys[0].not_after = at(2026, 11, 1);
    status result = evaluate(text, where);
    REQUIRE(result.state == state::invalid);
    REQUIRE(result.reason == reason::revoked);
    REQUIRE(result.value);

    keys[0].not_after = time_point::max();
    keys[0].revoked_from = at(2026, 11, 13);
    REQUIRE(evaluate(text, where).reason == reason::revoked);
    keys[0].revoked_from = at(2026, 11, 14);
    REQUIRE(evaluate(text, where).state == state::valid);
    keys[0].revoked_from = time_point::min();
    result = evaluate(text, where);
    REQUIRE(result.reason == reason::revoked);
    REQUIRE(!result.value);
}

TEST_CASE("licence: revocation lists")
{
    revocation_list list;
    list.sequence = 7;
    list.issued_at = at(2027, 2, 1);
    list.licence_ids = {"VSS-2026-000123"};
    list.keys = {{"lic-2026-a", at(2027, 1, 15)}, {"lic-2025-a", std::nullopt}};

    const auto payload = write_revocations(list);
    REQUIRE(payload);
    const auto parsed = parse_revocations(bytes_of(*payload));
    REQUIRE(parsed);
    REQUIRE(*parsed == list);
    // Ids only: nothing in the list can hold a name.
    REQUIRE(!parse_revocations(bytes_of(
        R"({"sequence":1,"issued_at":"2027-02-01T00:00:00Z","licences":["Example Lab"]})")));
    REQUIRE(!parse_revocations(bytes_of("{}")));
    REQUIRE(!parse_revocations(
        bytes_of(R"({"sequence":-1,"issued_at":"2027-02-01T00:00:00Z"})")));
    const auto future = parse_revocations(bytes_of(
        R"({"sequence":1,"issued_at":"2027-02-01T00:00:00Z","critical":["x"]})"));
    REQUIRE(!future);
    REQUIRE(future.error() == errc::newer_version);

    const auto sign_list = [&](std::string_view purpose)
    {
        return sf::write(signing::sign(purpose, test_keys::key_id,
                                       bytes_of(*payload),
                                       test_keys::private_key()));
    };
    const auto read =
        read_revocations(sign_list(revocations_purpose), test_keys::trusted());
    REQUIRE(read);
    REQUIRE(*read == list);

    // A licence signature is not a revocation signature.
    const auto wrong =
        read_revocations(sign_list(licence_purpose), test_keys::trusted());
    REQUIRE(!wrong);
    REQUIRE(wrong.error() == errc::wrong_purpose);
    REQUIRE(!read_revocations("junk", test_keys::trusted()));
    REQUIRE(!read_revocations(sign_list(revocations_purpose), {}));

    std::vector<sf::trusted_key> keys(test_keys::trusted().begin(),
                                      test_keys::trusted().end());
    keys[1].not_after = at(2027, 1, 1);
    const auto late = read_revocations(sign_list(revocations_purpose), keys);
    REQUIRE(!late);
    REQUIRE(late.error() == errc::key_not_valid);

    // The program never goes back to an older list.
    revocation_list older = list;
    older.sequence = 6;
    older.licence_ids.clear();
    REQUIRE_EQ(newer(list, older).sequence, 7U);
    REQUIRE_EQ(newer(older, list).sequence, 7U);
    REQUIRE_EQ(newer(list, list).sequence, 7U);
}

TEST_CASE("licence: the list built into this release withdraws nothing yet")
{
    const revocation_list built_in = builtin_revocations();
    REQUIRE_EQ(built_in.sequence, 0U);
    REQUIRE(built_in.licence_ids.empty());
    REQUIRE(built_in.keys.empty());

    // Any signed list is newer than it, so a file a customer is sent takes
    // its place.
    revocation_list sent;
    sent.sequence = 1;
    sent.licence_ids = {"VSS-2026-000123"};
    REQUIRE_EQ(newer(built_in, sent).sequence, 1U);
    REQUIRE_EQ(newer(sent, built_in).sequence, 1U);
}

TEST_CASE("licence: computer names")
{
    REQUIRE(machine_matches("LAB-RIG-07", "LAB-RIG-07"));
    REQUIRE(machine_matches("lab-rig-07", "LAB-RIG-07"));
    REQUIRE(machine_matches("LAB-RIG-*", "lab-rig-07"));
    REQUIRE(machine_matches("LAB-RIG-*", "LAB-RIG-"));
    REQUIRE(!machine_matches("LAB-RIG-07", "LAB-RIG-070"));
    REQUIRE(!machine_matches("LAB-RIG-07", "LAB-RIG-0"));
    REQUIRE(!machine_matches("LAB-RIG-*", "LAB-BENCH-1"));
    REQUIRE(!machine_matches("LAB-RIG-07", ""));

    licence value = annual();
    value.edition = edition::enterprise;
    value.kind = licence_kind::enterprise;
    value.seats = 0;
    value.machines = {"BENCH-1", "LAB-RIG-*"};
    const std::string text = signed_text(value);

    context where = on(at(2027, 1, 1));
    REQUIRE(evaluate(text, where).state == state::valid);
    where.computer_name = "bench-1";
    REQUIRE(evaluate(text, where).state == state::valid);
    where.computer_name = "OFFICE-PC";
    const status result = evaluate(text, where);
    REQUIRE(result.state == state::invalid);
    REQUIRE(result.reason == reason::wrong_computer);
    // The page lists the names the licence is for.
    REQUIRE(result.value);
    REQUIRE_EQ(result.value->machines.size(), std::size_t{2});
    where.computer_name.clear();
    REQUIRE(evaluate(text, where).reason == reason::wrong_computer);
}

TEST_CASE("licence: the clock check")
{
    const std::string text = signed_text(annual());

    // The program has seen 1 March. A clock inside the tolerance is fine.
    context where = on(at(2027, 2, 27));
    where.latest_seen = at(2027, 3, 1);
    REQUIRE(evaluate(text, where).state == state::valid);
    where.now = at(2027, 2, 27) - seconds{1};
    status result = evaluate(text, where);
    REQUIRE(result.state == state::clock_suspect);
    REQUIRE(result.reason == reason::clock_suspect);
    REQUIRE(!result.active());
    REQUIRE(!check(result, "plugins.official").granted);
    REQUIRE(check(result, "plugins.official").reason == reason::clock_suspect);

    // Putting the clock right clears it.
    where.now = at(2027, 3, 2);
    REQUIRE(evaluate(text, where).state == state::valid);

    // A clock set back past the end of the term to keep an expired licence
    // alive: the stored time says otherwise.
    where.now = at(2027, 6, 1);
    where.latest_seen = at(2028, 2, 1);
    REQUIRE(evaluate(text, where).state == state::clock_suspect);

    // With nothing stored, the build date and the signing time still bound
    // the clock.
    context fresh = on(at(2026, 6, 1));
    REQUIRE(evaluate(text, fresh).state == state::clock_suspect);
    fresh.now = at(2027, 1, 1);
    fresh.build_time = at(2027, 3, 1);
    REQUIRE(evaluate(text, fresh).state == state::clock_suspect);
    fresh.build_time = at(2027, 1, 2);
    REQUIRE(evaluate(text, fresh).state == state::valid);
}

TEST_CASE("licence: the stored time moves forward, and a reset works once")
{
    clock_state stored;
    stored = observe(stored, at(2027, 1, 1));
    REQUIRE(stored.latest_seen == at(2027, 1, 1));
    stored = observe(stored, at(2026, 1, 1));
    REQUIRE(stored.latest_seen == at(2027, 1, 1));

    // The clock once read 2031. An ordinary licence does not move it back.
    stored = observe(stored, at(2031, 1, 1));
    REQUIRE(apply_reset(stored, annual()) == stored);

    licence reset = annual();
    reset.issued_at = at(2027, 1, 5);
    reset.clock_reset = "reset-000123-a";
    const clock_state after = apply_reset(stored, reset);
    REQUIRE(after.latest_seen == at(2027, 1, 5));
    REQUIRE_EQ(after.applied_resets.size(), std::size_t{1});

    // The clock check passes again with the stored time put right.
    context where = on(at(2027, 1, 6));
    where.latest_seen = stored.latest_seen;
    REQUIRE(evaluate(signed_text(reset), where).state == state::clock_suspect);
    where.latest_seen = after.latest_seen;
    REQUIRE(evaluate(signed_text(reset), where).state == state::valid);

    // Importing the same file again later does nothing.
    const clock_state later = observe(after, at(2027, 8, 1));
    REQUIRE(apply_reset(later, reset) == later);

    // The list of used tokens stays bounded.
    clock_state many;
    for (int i = 0; i < 40; ++i)
    {
        licence next = reset;
        next.clock_reset = "reset-" + std::to_string(i);
        many = apply_reset(many, next);
    }
    REQUIRE_EQ(many.applied_resets.size(), std::size_t{32});
    REQUIRE_EQ(many.applied_resets.back(), std::string("reset-39"));
}

TEST_CASE("licence: features")
{
    REQUIRE(feature_matches("plugins.official", "plugins.official"));
    REQUIRE(!feature_matches("plugins.official", "plugins.officials"));
    REQUIRE(!feature_matches("plugins.official", "plugins"));
    REQUIRE(feature_matches("plugins.*", "plugins.replay"));
    REQUIRE(feature_matches("plugins.*", "plugins.a.b"));
    REQUIRE(!feature_matches("plugins.*", "plugins"));
    REQUIRE(!feature_matches("plugins.*", "plugins."));
    REQUIRE(!feature_matches("plugins.*", "pluginsx.replay"));

    const std::string text = signed_text(annual());
    const status valid = evaluate(text, on(at(2027, 1, 1)));
    entitlement answer = check(valid, "plugins.official");
    REQUIRE(answer.granted);
    REQUIRE(answer.reason == reason::ok);
    REQUIRE_EQ(answer.licence_id, std::string("VSS-2026-000123"));
    REQUIRE_EQ(answer.days_left, 316);

    answer = check(valid, "policy.official-only");
    REQUIRE(!answer.granted);
    REQUIRE(answer.reason == reason::not_in_licence);

    // In the grace period the features still work.
    const status grace = evaluate(text, on(at(2027, 12, 1)));
    REQUIRE(check(grace, "support").granted);
    REQUIRE_EQ(check(grace, "support").days_left, 10);

    const status expired = evaluate(text, on(at(2028, 1, 1)));
    answer = check(expired, "plugins.official");
    REQUIRE(!answer.granted);
    REQUIRE(answer.reason == reason::expired);
    REQUIRE_EQ(answer.licence_id, std::string("VSS-2026-000123"));

    const status early = evaluate(text, on(at(2026, 11, 12, 12)));
    REQUIRE(check(early, "support").reason == reason::not_yet_valid);

    answer = check(no_licence(), "plugins.official");
    REQUIRE(!answer.granted);
    REQUIRE(answer.reason == reason::no_licence);
    REQUIRE(answer.licence_id.empty());

    answer = check(evaluate("junk", on(at(2027, 1, 1))), "support");
    REQUIRE(!answer.granted);
    REQUIRE(answer.reason == reason::malformed);
}

TEST_CASE("licence: the best of several files")
{
    REQUIRE(best({}).state == state::none);
    REQUIRE(best({}).reason == reason::no_licence);

    const context where = on(at(2027, 12, 1));
    // In its grace period on 1 December 2027.
    const status old_pro = evaluate(signed_text(annual()), where);
    REQUIRE(old_pro.state == state::grace);

    licence renewal = annual();
    renewal.licence_id = "VSS-2027-000050";
    renewal.issued_at = at(2027, 11, 1);
    renewal.starts_at = at(2027, 11, 13);
    renewal.expires_at = at(2028, 11, 13);
    renewal.replaces = "VSS-2026-000123";
    const status new_pro = evaluate(signed_text(renewal), where);
    REQUIRE(new_pro.state == state::valid);

    licence site = renewal;
    site.licence_id = "VSS-2027-000051";
    site.edition = edition::enterprise;
    site.kind = licence_kind::enterprise;
    site.expires_at = at(2028, 6, 1);
    const status enterprise = evaluate(signed_text(site), where);

    const status broken = evaluate("junk", where);
    const status ended = evaluate(signed_text(annual()), on(at(2029, 1, 1)));
    REQUIRE(ended.state == state::expired);

    const auto id = [](const status& s) { return s.value->licence_id; };
    std::vector<status> files{broken, old_pro, new_pro};
    REQUIRE_EQ(id(best(files)), std::string("VSS-2027-000050"));
    // Enterprise beats Pro, even with the earlier end.
    files.push_back(enterprise);
    REQUIRE_EQ(id(best(files)), std::string("VSS-2027-000051"));
    // Grace beats expired and broken.
    files = {ended, broken, old_pro};
    REQUIRE(best(files).state == state::grace);
    // An expired licence is shown in preference to a broken file.
    files = {broken, ended};
    REQUIRE(best(files).state == state::expired);
    files = {broken};
    REQUIRE(best(files).state == state::invalid);

    // Two valid Pro licences: the later end wins.
    licence longer = renewal;
    longer.licence_id = "VSS-2027-000052";
    longer.expires_at = at(2029, 1, 1);
    files = {new_pro, evaluate(signed_text(longer), where)};
    REQUIRE_EQ(id(best(files)), std::string("VSS-2027-000052"));
}

TEST_CASE("licence: the format on disk has not changed")
{
    // A licence a customer holds must keep reading. This file was written by
    // this version of the format, with the throwaway test key; Ed25519 is
    // deterministic, so writing it again gives the same bytes.
    const auto path = std::filesystem::path(VSS_TEST_DATA_DIR) / "licence" /
                      "annual-test-key.vsslic";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::string golden{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};
    REQUIRE_EQ(golden, signed_text(annual()));

    const status result = evaluate(golden, on(at(2027, 1, 1)));
    REQUIRE(result.state == state::valid);
    REQUIRE(result.value);
    REQUIRE(*result.value == annual());
}
