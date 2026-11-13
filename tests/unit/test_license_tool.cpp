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

#include <string>

using namespace vss_test_support::license_tool;
namespace tool = vss::license::tool;
namespace sf = vss::license::signed_file;

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
