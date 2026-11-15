//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The commands of vss_license, run in the test process. Every key here is
// the published RFC 8032 test key or one made for the test and thrown away.
//

#include "license_tool_fixture.hpp"

#include <vss/license/sign.hpp>
#include <vss/license/test_keys.hpp>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vss_test_support::license_tool;
namespace tool = vss::license::tool;
namespace sf = vss::license::signed_file;
namespace json = boost::json;

namespace
{

/// The golden licence's payload as an object a test can change.
json::object golden_request()
{
    return json::parse(golden_payload()).as_object();
}

/// What a request has to look like to be refused: the exit status, a message
/// that holds `expected`, and no file written. Signs with the test key.
void require_refused(const json::object& request, const std::string& expected)
{
    const scratch_directory scratch;
    const std::string file =
        write_text(scratch, "request.json", json::serialize(request));
    const std::string out = path_in(scratch, "out.vsslic");
    const outcome result =
        run_tool({"issue", "--request", file, "--out", out, "--test"});
    REQUIRE_EQ(result.status, tool::exit_failed);
    REQUIRE(result.err.find(expected) != std::string::npos);
    REQUIRE(result.out.empty());
    REQUIRE(!exists(out));
}

/// Whether a dry run of `issue` accepts the request.
bool dry_run_accepts(const json::object& request)
{
    const scratch_directory scratch;
    const std::string file =
        write_text(scratch, "request.json", json::serialize(request));
    return run_tool({"issue", "--request", file, "--dry-run"}).status ==
           tool::exit_ok;
}

json::object trial_request()
{
    json::object request = golden_request();
    request["kind"] = "trial";
    request["grace_days"] = 0;
    request["seats"] = 1;
    return request;
}

} // namespace

TEST_CASE("licence tool: no command or an unknown one is a usage error")
{
    scratch_directory scratch;
    const outcome none = run_tool({});
    REQUIRE_EQ(none.status, tool::exit_usage);
    REQUIRE(none.out.find("vss_license inspect") != std::string::npos);

    REQUIRE_EQ(run_tool({"--help"}).status, tool::exit_ok);
    REQUIRE_EQ(run_tool({"activate"}).status, tool::exit_usage);
    REQUIRE_EQ(run_tool({"inspect"}).status, tool::exit_usage);
    REQUIRE_EQ(run_tool({"inspect", golden_path(), "--online"}).status,
               tool::exit_usage);
    REQUIRE_EQ(run_tool({"verify", golden_path(), "--at"}).status,
               tool::exit_usage);
    REQUIRE_EQ(
        run_tool({"verify", golden_path(), "--at", "x", "--at", "y"}).status,
        tool::exit_usage);
}

TEST_CASE("licence tool: inspect prints a licence and says who signed it")
{
    const outcome trusted = run_tool({"inspect", golden_path(), "--test"});
    REQUIRE_EQ(trusted.status, tool::exit_ok);
    REQUIRE(trusted.out.find("Signature: verifies") != std::string::npos);
    REQUIRE(trusted.out.find("Key: test-rfc8032-1") != std::string::npos);
    REQUIRE(trusted.out.find("Licence: VSS-2026-000123") != std::string::npos);
    REQUIRE(trusted.out.find("Licensee: Example Lab Pty Ltd") !=
            std::string::npos);
    REQUIRE(trusted.out.find("Expires: 2027-11-13T00:00:00Z") !=
            std::string::npos);

    // A product build does not know the test key. Inspect still prints the
    // file and says plainly that it is not trusted.
    const outcome product = run_tool({"inspect", golden_path()});
    REQUIRE_EQ(product.status, tool::exit_ok);
    REQUIRE(product.out.find("NOT VERIFIED") != std::string::npos);
    REQUIRE(product.out.find("Licence: VSS-2026-000123") != std::string::npos);
}

TEST_CASE("licence tool: inspect refuses what is not a signed file")
{
    scratch_directory scratch;
    const std::string junk = write_text(scratch, "junk.vsslic", "not json");
    REQUIRE_EQ(run_tool({"inspect", junk}).status, tool::exit_failed);
    REQUIRE_EQ(run_tool({"inspect", path_in(scratch, "missing")}).status,
               tool::exit_failed);
}

TEST_CASE("licence tool: verify gives the state Studio would")
{
    const outcome valid =
        run_tool({"verify", golden_path(), "--test", "--at", "2027-01-01"});
    REQUIRE_EQ(valid.status, tool::exit_ok);
    REQUIRE(valid.out.find("State: valid") != std::string::npos);
    REQUIRE(valid.out.find("Days left: 316") != std::string::npos);

    const outcome grace = run_tool(
        {"verify", golden_path(), "--test", "--at", "2027-11-20T00:00:00Z"});
    REQUIRE_EQ(grace.status, tool::exit_ok);
    REQUIRE(grace.out.find("State: grace") != std::string::npos);

    const outcome expired =
        run_tool({"verify", golden_path(), "--test", "--at", "2028-03-01"});
    REQUIRE_EQ(expired.status, tool::exit_failed);
    REQUIRE(expired.out.find("State: expired") != std::string::npos);

    // Without --at the tool judges at the time it is given.
    const outcome now = run_tool({"verify", golden_path(), "--test"});
    REQUIRE_EQ(now.status, tool::exit_ok);
    REQUIRE(now.out.find("Judged at: 2027-06-01T00:00:00Z") !=
            std::string::npos);

    // A product build trusts no test key.
    const outcome product =
        run_tool({"verify", golden_path(), "--at", "2027-01-01"});
    REQUIRE_EQ(product.status, tool::exit_failed);
    REQUIRE(product.out.find("State: invalid") != std::string::npos);

    REQUIRE_EQ(
        run_tool({"verify", golden_path(), "--test", "--at", "March"}).status,
        tool::exit_usage);
}

TEST_CASE("licence tool: issue with the test key writes the golden file")
{
    scratch_directory scratch;
    const std::string request =
        write_text(scratch, "request.json", golden_payload());
    const std::string out = path_in(scratch, "out.vsslic");

    const outcome issued =
        run_tool({"issue", "--request", request, "--out", out, "--test"});
    REQUIRE_EQ(issued.status, tool::exit_ok);
    REQUIRE(issued.out.find("Signed with the test key") != std::string::npos);
    // Ed25519 signatures are deterministic, so this is the golden file.
    REQUIRE_EQ(read_text(out), read_text(golden_path()));

    // An existing file is never overwritten.
    const outcome again =
        run_tool({"issue", "--request", request, "--out", out, "--test"});
    REQUIRE_EQ(again.status, tool::exit_failed);
    REQUIRE(again.err.find("not overwritten") != std::string::npos);
}

TEST_CASE("licence tool: a dry run checks the request and uses no key")
{
    scratch_directory scratch;
    const std::string request =
        write_text(scratch, "request.json", golden_payload());
    const outcome dry = run_tool({"issue", "--request", request, "--dry-run"});
    REQUIRE_EQ(dry.status, tool::exit_ok);
    REQUIRE(dry.out.find("Licence: VSS-2026-000123") != std::string::npos);
    REQUIRE(dry.out.find("nothing was signed") != std::string::npos);

    const std::string bad =
        write_text(scratch, "bad.json", "{\"licence_id\":\"VSS-2026-1\"}");
    REQUIRE_EQ(run_tool({"issue", "--request", bad, "--dry-run"}).status,
               tool::exit_failed);
    REQUIRE_EQ(run_tool({"issue", "--dry-run"}).status, tool::exit_usage);
    REQUIRE_EQ(
        run_tool({"issue", "--request", path_in(scratch, "none"), "--dry-run"})
            .status,
        tool::exit_failed);
}

TEST_CASE("licence tool: a licence id in the ledger is refused")
{
    scratch_directory scratch;
    const std::string request =
        write_text(scratch, "request.json", golden_payload());
    const std::string used = write_text(
        scratch, "used.jsonl",
        "{\"event\":\"issued\",\"licence_id\":\"VSS-2026-000001\"}\n"
        "\n"
        "{\"event\":\"issued\",\"licence_id\":\"VSS-2026-000123\"}\r\n");
    const outcome refused = run_tool(
        {"issue", "--request", request, "--dry-run", "--ledger", used});
    REQUIRE_EQ(refused.status, tool::exit_failed);
    REQUIRE(refused.err.find("never used twice") != std::string::npos);

    const std::string other = write_text(
        scratch, "other.jsonl",
        "{\"event\":\"issued\",\"licence_id\":\"VSS-2026-000001\"}\n");
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--dry-run", "--ledger",
                         other})
                   .status,
               tool::exit_ok);

    const std::string broken =
        write_text(scratch, "broken.jsonl", "VSS-2026-000001\n");
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--dry-run", "--ledger",
                         broken})
                   .status,
               tool::exit_failed);
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--dry-run", "--ledger",
                         path_in(scratch, "none")})
                   .status,
               tool::exit_failed);
}

TEST_CASE("licence tool: the key options are checked before any key is read")
{
    scratch_directory scratch;
    const std::string request =
        write_text(scratch, "request.json", golden_payload());
    const std::string out = path_in(scratch, "out.vsslic");
    const std::string key = path_in(scratch, "key");

    // No --out.
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--test"}).status,
               tool::exit_usage);
    // No key at all, and two at once.
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--out", out,
                         "--key-id", "lic-2026-a"})
                   .status,
               tool::exit_usage);
    REQUIRE_EQ(
        run_tool({"issue", "--request", request, "--out", out, "--key-id",
                  "lic-2026-a", "--key-file", key, "--key-env", "VSS_KEY"})
            .status,
        tool::exit_usage);
    // No key id, a malformed one, and a test key id without --test.
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--out", out,
                         "--key-file", key})
                   .status,
               tool::exit_usage);
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--out", out,
                         "--key-id", "Lic 2026", "--key-file", key})
                   .status,
               tool::exit_usage);
    const outcome test_id =
        run_tool({"issue", "--request", request, "--out", out, "--key-id",
                  "test-rfc8032-1", "--key-file", key});
    REQUIRE_EQ(test_id.status, tool::exit_usage);
    REQUIRE(test_id.err.find("Use --test") != std::string::npos);
    // --test takes no other key option.
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--out", out, "--test",
                         "--key-id", "lic-2026-a"})
                   .status,
               tool::exit_usage);
    // A key file that is missing, and one that is not a key.
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--out", out,
                         "--key-id", "lic-2026-a", "--key-file", key})
                   .status,
               tool::exit_failed);
    const std::string not_a_key = write_text(scratch, "words", "hello\n");
    const outcome malformed =
        run_tool({"issue", "--request", request, "--out", out, "--key-id",
                  "lic-2026-a", "--key-file", not_a_key});
    REQUIRE_EQ(malformed.status, tool::exit_failed);
    REQUIRE(malformed.err.find("hello") == std::string::npos);
    REQUIRE(!exists(out));
}

TEST_CASE("licence tool: a key this build does not trust issues nothing")
{
    scratch_directory scratch;
    const std::string request =
        write_text(scratch, "request.json", golden_payload());
    const std::string out = path_in(scratch, "out.vsslic");

    // The test key under a product key id, from the environment. The build
    // has no such key in its trust list, so the tool's own check fails and
    // no file is written.
    std::string seed = vss::license::signing::format_seed(
        vss::license::test_keys::private_key());
    const outcome refused =
        run_tool({"issue", "--request", request, "--out", out, "--key-id",
                  "lic-2026-a", "--key-env", "VSS_TEST_ONLY_KEY"},
                 [&](const std::string& name) -> std::optional<std::string>
                 {
                     if (name == "VSS_TEST_ONLY_KEY")
                         return seed + "\n";
                     return std::nullopt;
                 });
    REQUIRE_EQ(refused.status, tool::exit_failed);
    REQUIRE(refused.err.find("trust list") != std::string::npos);
    REQUIRE(refused.out.find(seed) == std::string::npos);
    REQUIRE(refused.err.find(seed) == std::string::npos);
    REQUIRE(!exists(out));

    // A variable that is not set.
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--out", out,
                         "--key-id", "lic-2026-a", "--key-env", "VSS_UNSET"})
                   .status,
               tool::exit_failed);
}

TEST_CASE("licence tool: keygen writes a private key file and prints the "
          "public half only")
{
    scratch_directory scratch;
    const std::string key = path_in(scratch, "throwaway.key");

    const outcome made =
        run_tool({"keygen", "--key-id", "lic-unit-a", "--out-private", key});
    REQUIRE_EQ(made.status, tool::exit_ok);

    // The file holds a key in the form issue reads, and nothing printed
    // holds it.
    const std::string text = read_text(key);
    const auto seed = vss::license::signing::parse_seed(text);
    REQUIRE(seed);
    const std::string secret = text.substr(0, text.find('\n'));
    REQUIRE_EQ(secret.size(), std::size_t{43});
    REQUIRE(made.out.find(secret) == std::string::npos);
    REQUIRE(made.err.find(secret) == std::string::npos);

    // What is printed is the public half of that key.
    const auto public_half = vss::license::signing::public_key_of(*seed);
    REQUIRE(made.out.find("Public key: " + sf::base64url_encode(public_half)) !=
            std::string::npos);
    REQUIRE(made.out.find("Key id: lic-unit-a") != std::string::npos);

    // An existing file is never replaced, and two keys are never the same.
    REQUIRE_EQ(
        run_tool({"keygen", "--key-id", "lic-unit-a", "--out-private", key})
            .status,
        tool::exit_failed);
    REQUIRE_EQ(read_text(key), text);
    const std::string second = path_in(scratch, "second.key");
    REQUIRE_EQ(
        run_tool({"keygen", "--key-id", "lic-unit-b", "--out-private", second})
            .status,
        tool::exit_ok);
    REQUIRE(read_text(second) != text);

    // The new key signs, and the build refuses the result, because its
    // public half is not in the trust list.
    const std::string request =
        write_text(scratch, "request.json", golden_payload());
    const std::string out = path_in(scratch, "out.vsslic");
    REQUIRE_EQ(run_tool({"issue", "--request", request, "--out", out,
                         "--key-id", "lic-unit-a", "--key-file", key})
                   .status,
               tool::exit_failed);
    REQUIRE(!exists(out));

    REQUIRE_EQ(run_tool({"keygen", "--key-id", "test-mine", "--out-private",
                         path_in(scratch, "t.key")})
                   .status,
               tool::exit_usage);
    REQUIRE_EQ(run_tool({"keygen", "--key-id", "UPPER", "--out-private",
                         path_in(scratch, "u.key")})
                   .status,
               tool::exit_usage);
    REQUIRE_EQ(run_tool({"keygen", "--key-id", "lic-unit-c"}).status,
               tool::exit_usage);
}

TEST_CASE("licence tool: a signed revocation list withdraws a licence")
{
    scratch_directory scratch;
    vss::license::revocation_list list;
    list.sequence = 7;
    list.issued_at = *vss::license::parse_time("2027-02-01T00:00:00Z");
    list.licence_ids = {"VSS-2026-000123"};
    const auto payload = vss::license::write_revocations(list);
    REQUIRE(payload);
    const std::string request =
        write_text(scratch, "revocations.json", *payload);
    const std::string out = path_in(scratch, "revocations.vssrev");

    const outcome dry =
        run_tool({"revocations", "--request", request, "--dry-run"});
    REQUIRE_EQ(dry.status, tool::exit_ok);
    REQUIRE(dry.out.find("Sequence: 7") != std::string::npos);

    REQUIRE_EQ(
        run_tool({"revocations", "--request", request, "--out", out, "--test"})
            .status,
        tool::exit_ok);

    const outcome shown = run_tool({"inspect", out, "--test"});
    REQUIRE_EQ(shown.status, tool::exit_ok);
    REQUIRE(shown.out.find("Purpose: revocations") != std::string::npos);
    REQUIRE(shown.out.find("  VSS-2026-000123") != std::string::npos);

    const outcome withdrawn =
        run_tool({"verify", golden_path(), "--test", "--at", "2027-03-01",
                  "--revocations", out});
    REQUIRE_EQ(withdrawn.status, tool::exit_failed);
    REQUIRE(withdrawn.out.find("withdrawn") != std::string::npos);

    // A licence is not a revocation list, and a list is not a licence.
    REQUIRE_EQ(run_tool({"verify", golden_path(), "--test", "--revocations",
                         golden_path()})
                   .status,
               tool::exit_failed);
    REQUIRE_EQ(run_tool({"verify", out, "--test"}).status, tool::exit_failed);
    REQUIRE_EQ(run_tool({"verify", golden_path(), "--test", "--revocations",
                         path_in(scratch, "none")})
                   .status,
               tool::exit_failed);

    const std::string licence_request =
        write_text(scratch, "licence.json", golden_payload());
    REQUIRE_EQ(
        run_tool({"revocations", "--request", licence_request, "--dry-run"})
            .status,
        tool::exit_failed);
    REQUIRE_EQ(run_tool({"revocations", "--request", request, "--dry-run",
                         "--ledger", request})
                   .status,
               tool::exit_usage);
}

TEST_CASE("licence tool: a machine-bound licence needs --machine")
{
    scratch_directory scratch;
    auto value = vss::license::parse_payload(bytes_of(golden_payload()));
    REQUIRE(value);
    value->machines = {"LAB-RIG-*"};
    const auto payload = vss::license::write_payload(*value);
    REQUIRE(payload);
    const std::string request = write_text(scratch, "request.json", *payload);
    const std::string out = path_in(scratch, "bound.vsslic");
    const outcome issued =
        run_tool({"issue", "--request", request, "--out", out, "--test"});
    REQUIRE_EQ(issued.status, tool::exit_ok);
    REQUIRE(issued.out.find("Computers: LAB-RIG-*") != std::string::npos);

    REQUIRE_EQ(run_tool({"verify", out, "--test", "--at", "2027-01-01"}).status,
               tool::exit_failed);
    REQUIRE_EQ(run_tool({"verify", out, "--test", "--at", "2027-01-01",
                         "--machine", "lab-rig-07"})
                   .status,
               tool::exit_ok);
}

TEST_CASE("licence tool: a request field the tool does not know is refused by "
          "name")
{
    // The licence that was asked for is the one that is signed, so a typo is
    // an error and not a licence with the field left out.
    json::object singular = golden_request();
    singular["machine"] = json::array{"LAB-RIG-*"};
    require_refused(singular,
                    "the field \"machine\" is not one this tool knows");

    json::object misspelt = golden_request();
    misspelt["update_until"] = "2027-11-13T23:59:59Z";
    require_refused(misspelt, "update_until");

    json::object critical = golden_request();
    critical["critial"] = json::array{"updates_until"};
    require_refused(critical, "critial");

    json::object licensee = golden_request();
    licensee["licensee"].as_object()["contct"] = "Test team";
    require_refused(licensee, "\"licensee.contct\"");

    // A name that only looks right is shown so that it does not: a trailing
    // space is inside the quotes, and a control character, a zero-width
    // character or a look-alike letter is a question mark, one for each byte.
    json::object space = golden_request();
    space["features "] = json::array{"support"};
    require_refused(space, "the field \"features \" is not one");

    json::object control = golden_request();
    control["bad\nname"] = 1;
    require_refused(control, "\"bad?name\"");

    json::object zero_width = golden_request();
    zero_width["machines\xe2\x80\x8b"] = json::array();
    require_refused(zero_width, "\"machines???\"");

    // Cyrillic small letter ie, which looks like a Latin e.
    json::object look_alike = golden_request();
    look_alike["f\xd0\xb5"
               "atures"] = json::array();
    require_refused(look_alike, "\"f??atures\"");

    // The untouched request is still accepted.
    REQUIRE(dry_run_accepts(golden_request()));
}

TEST_CASE("licence tool: a request holding every payload field is accepted")
{
    // The tool's list of fields must not fall behind the library's: a request
    // the library writes with every optional field set has to pass.
    auto value = vss::license::parse_payload(bytes_of(golden_payload()));
    REQUIRE(value);
    value->kind = vss::license::licence_kind::enterprise;
    value->edition = vss::license::edition::enterprise;
    value->machines = {"LAB-RIG-*"};
    value->updates_until = *vss::license::parse_time("2027-11-13T23:59:59Z");
    value->replaces = "VSS-2026-000100";
    value->clock_reset = "reset-2027-a";
    const auto payload = vss::license::write_payload(*value);
    REQUIRE(payload);
    REQUIRE(dry_run_accepts(json::parse(*payload).as_object()));
}

TEST_CASE("licence tool: a request with no features, or with critical fields, "
          "is refused")
{
    // docs/licensing.md, "Payload": features is not marked optional.
    json::object none = golden_request();
    none.erase("features");
    require_refused(none, "features is missing");

    // An empty list says that the licence turns nothing on, on purpose.
    json::object empty = golden_request();
    empty["features"] = json::array();
    REQUIRE(dry_run_accepts(empty));

    // The tool writes `critical: []`, so a request that names fields would be
    // signed without them.
    json::object critical = golden_request();
    critical["critical"] = json::array{"updates_until"};
    require_refused(critical, "critical is not empty");
}

TEST_CASE("licence tool: the licence id has the form the design gives")
{
    // docs/licensing.md, "Payload": VSS-<year>-<six digits>.
    for (const char* id :
         {"VSS-2026-12345", "VSS-26-000123", "VSS-2026-0001234",
          "VSS-2026-00012A", "VSS-ABCD-000123", "VSS-2026-000123-1"})
    {
        json::object request = golden_request();
        request["licence_id"] = id;
        require_refused(request, "licence_id");
    }
    json::object renews = golden_request();
    renews["replaces"] = "VSS-2026-1";
    require_refused(renews, "replaces");

    json::object good = golden_request();
    good["replaces"] = "VSS-2026-000100";
    REQUIRE(dry_run_accepts(good));
}

TEST_CASE("licence tool: a trial has no grace period, one seat and no "
          "machines")
{
    // docs/licensing.md, "Payload" (grace_days) and "Trials".
    REQUIRE(dry_run_accepts(trial_request()));

    json::object grace = trial_request();
    grace["grace_days"] = 28;
    require_refused(grace, "grace_days must be 0");

    json::object seats = trial_request();
    seats["seats"] = 3;
    require_refused(seats, "seats must be 1");

    json::object machines = trial_request();
    machines["machines"] = json::array{"LAB-RIG-*"};
    require_refused(machines, "machines must be empty");

    // The same values are allowed on a licence that is not a trial.
    json::object annual = golden_request();
    annual["grace_days"] = 28;
    annual["seats"] = 3;
    annual["machines"] = json::array{"LAB-RIG-*"};
    REQUIRE(dry_run_accepts(annual));
}

TEST_CASE("licence tool: issued_at cannot be well ahead of the clock")
{
    // The tool's clock is 1 June 2027. docs/licensing.md, "Payload": issued_at
    // is when the owner signed the licence; the clock check allows 48 hours.
    json::object ahead = golden_request();
    ahead["issued_at"] = "2027-06-04T00:00:00Z";
    require_refused(ahead, "issued_at");

    json::object a_year = golden_request();
    a_year["issued_at"] = "2028-06-01T00:00:00Z";
    require_refused(a_year, "issued_at");

    // A licence signed after its term has ended.
    json::object after = golden_request();
    after["issued_at"] = "2027-06-01T00:00:01Z";
    after["expires_at"] = "2027-06-01T00:00:00Z";
    after["starts_at"] = "2027-05-01T00:00:00Z";
    require_refused(after, "issued_at is after expires_at");

    json::object near = golden_request();
    near["issued_at"] = "2027-06-02T12:00:00Z";
    REQUIRE(dry_run_accepts(near));
}

TEST_CASE("licence tool: a revocation request is read as strictly")
{
    vss::license::revocation_list list;
    list.sequence = 7;
    list.issued_at = *vss::license::parse_time("2027-02-01T00:00:00Z");
    list.licence_ids = {"VSS-2026-000123"};
    list.keys = {{"lic-2026-a", std::nullopt}};
    const auto payload = vss::license::write_revocations(list);
    REQUIRE(payload);

    const auto refused =
        [](const json::object& request, const std::string& expected)
    {
        const scratch_directory scratch;
        const std::string file =
            write_text(scratch, "request.json", json::serialize(request));
        const std::string out = path_in(scratch, "out.vssrev");
        const outcome result = run_tool(
            {"revocations", "--request", file, "--out", out, "--test"});
        REQUIRE_EQ(result.status, tool::exit_failed);
        REQUIRE(result.err.find(expected) != std::string::npos);
        REQUIRE(!exists(out));
    };

    json::object good = json::parse(*payload).as_object();
    {
        const scratch_directory scratch;
        REQUIRE_EQ(
            run_tool({"revocations", "--request",
                      write_text(scratch, "r.json", json::serialize(good)),
                      "--dry-run"})
                .status,
            tool::exit_ok);
    }

    json::object typo = good;
    typo["licenses"] = json::array{"VSS-2026-000999"};
    refused(typo, "licenses");

    json::object key_typo = good;
    key_typo["keys"].as_array()[0].as_object()["frm"] = "2027-01-01T00:00:00Z";
    refused(key_typo, "keys.frm");

    json::object critical = good;
    critical["critical"] = json::array{"keys"};
    refused(critical, "critical is not empty");

    // docs/licensing.md, "Revocation": signed lists start at 1.
    json::object zero = good;
    zero["sequence"] = 0;
    refused(zero, "sequence must be 1 or more");
}

TEST_CASE("licence tool: verify consults the list built into the build")
{
    vss::license::revocation_list builtin;
    builtin.sequence = 3;
    builtin.licence_ids = {"VSS-2026-000123"};

    // No --revocations: the built-in list still withdraws the licence, as it
    // does in Studio of the same build, and the output says which list.
    const outcome withdrawn = run_tool(
        {"verify", golden_path(), "--test", "--at", "2027-03-01"}, {}, builtin);
    REQUIRE_EQ(withdrawn.status, tool::exit_failed);
    REQUIRE(withdrawn.out.find("State: invalid") != std::string::npos);
    REQUIRE(withdrawn.out.find("withdrawn") != std::string::npos);
    REQUIRE(withdrawn.out.find("Revocations: the list built into this build "
                               "(sequence 3)") != std::string::npos);

    // With an empty list the same licence is valid.
    const outcome clean =
        run_tool({"verify", golden_path(), "--test", "--at", "2027-03-01"});
    REQUIRE_EQ(clean.status, tool::exit_ok);
    REQUIRE(clean.out.find("Revocations: the list built into this build "
                           "(sequence 0)") != std::string::npos);
}

TEST_CASE("licence tool: a given revocation list competes with the built-in "
          "one by sequence number")
{
    scratch_directory scratch;
    const auto signed_list = [&](std::uint64_t sequence,
                                 std::vector<std::string> ids,
                                 std::string_view name)
    {
        vss::license::revocation_list list;
        list.sequence = sequence;
        list.issued_at = *vss::license::parse_time("2027-02-01T00:00:00Z");
        list.licence_ids = std::move(ids);
        const auto payload = vss::license::write_revocations(list);
        REQUIRE(payload);
        const std::string request = write_text(scratch, "q.json", *payload);
        const std::string out = path_in(scratch, name);
        REQUIRE_EQ(run_tool({"revocations", "--request", request, "--out", out,
                             "--test"})
                       .status,
                   tool::exit_ok);
        return out;
    };

    vss::license::revocation_list builtin;
    builtin.sequence = 5;
    builtin.licence_ids = {"VSS-2026-000123"};

    // A newer file that no longer lists the licence replaces the built-in
    // list, as a signed list does in Studio.
    const std::string newer = signed_list(6, {"VSS-2026-000999"}, "six.vssrev");
    const outcome replaced =
        run_tool({"verify", golden_path(), "--test", "--at", "2027-03-01",
                  "--revocations", newer},
                 {}, builtin);
    REQUIRE_EQ(replaced.status, tool::exit_ok);
    REQUIRE(replaced.out.find("sequence 6") != std::string::npos);
    REQUIRE(replaced.out.find("sequence 5") != std::string::npos);
    REQUIRE(replaced.out.find("the file is used") != std::string::npos);

    // An older file does not take the built-in list's place.
    const std::string older = signed_list(4, {}, "four.vssrev");
    const outcome kept = run_tool({"verify", golden_path(), "--test", "--at",
                                   "2027-03-01", "--revocations", older},
                                  {}, builtin);
    REQUIRE_EQ(kept.status, tool::exit_failed);
    REQUIRE(kept.out.find("withdrawn") != std::string::npos);
    REQUIRE(kept.out.find("the built-in list is used") != std::string::npos);

    // The same number is the normal case after a release, when the built-in
    // list is set to that of the signed list of the day. The built-in list is
    // kept, as Studio keeps it, and the output does not call it newer.
    const std::string same = signed_list(5, {}, "five.vssrev");
    const outcome tied = run_tool({"verify", golden_path(), "--test", "--at",
                                   "2027-03-01", "--revocations", same},
                                  {}, builtin);
    REQUIRE_EQ(tied.status, tool::exit_failed);
    REQUIRE(tied.out.find("withdrawn") != std::string::npos);
    REQUIRE(tied.out.find("the built-in list is used") != std::string::npos);
    REQUIRE(tied.out.find("newer") == std::string::npos);
}

TEST_CASE("licence tool: a key file is read whole, with its white space")
{
    scratch_directory scratch;
    const std::string request =
        write_text(scratch, "request.json", golden_payload());
    const std::string out = path_in(scratch, "out.vsslic");
    const std::string seed = vss::license::signing::format_seed(
        vss::license::test_keys::private_key());

    // The key is read, parsed and used to sign; the build then refuses the
    // result because lic-2026-a is not the test key's id. Reaching that
    // message means every step before it worked.
    const std::string padded =
        write_text(scratch, "padded.key", "\r\n  " + seed + "  \r\n\r\n");
    const outcome signed_with_it =
        run_tool({"issue", "--request", request, "--out", out, "--key-id",
                  "lic-2026-a", "--key-file", padded});
    REQUIRE_EQ(signed_with_it.status, tool::exit_failed);
    REQUIRE(signed_with_it.err.find("trust list") != std::string::npos);
    REQUIRE(signed_with_it.err.find(seed) == std::string::npos);

    // A file longer than a signed file may be is not read as a key.
    const std::string huge = write_text(
        scratch, "huge.key", seed + std::string(sf::max_file_size, ' '));
    const outcome too_big =
        run_tool({"issue", "--request", request, "--out", out, "--key-id",
                  "lic-2026-a", "--key-file", huge});
    REQUIRE_EQ(too_big.status, tool::exit_failed);
    REQUIRE(too_big.err.find("Cannot read the private key") !=
            std::string::npos);

    // A folder is not a file.
    const outcome folder =
        run_tool({"issue", "--request", request, "--out", out, "--key-id",
                  "lic-2026-a", "--key-file", scratch.path().string()});
    REQUIRE_EQ(folder.status, tool::exit_failed);
    REQUIRE(folder.err.find("Cannot read the private key") !=
            std::string::npos);
    REQUIRE(!exists(out));
}
