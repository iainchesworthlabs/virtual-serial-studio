//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli licence` run in the test process, with licence folders the test
// made. The real folders of this computer are never read.
//

#include <licence_command.hpp>

#include <vss/license/store.hpp>

#include "licence_test_files.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lic = vss::license;
using namespace vss_test_support::licence_files;

namespace
{

namespace fs = std::filesystem;

/// How a command ended.
struct run_result
{
    int status = 0;
    std::string out;
    std::string err;
};

run_result licence_cli(fixture& f, std::vector<std::string> words)
{
    words.insert(words.begin(), "licence");
    std::ostringstream out;
    std::ostringstream err;
    run_result result;
    result.status = vss_cli::command_licence(words, f.options(), out, err);
    result.out = out.str();
    result.err = err.str();
    return result;
}

/// The lines of `text`, without their line endings.
std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

bool has_line(const std::string& text, std::string_view line)
{
    for (const std::string& each : lines_of(text))
    {
        if (each == line)
            return true;
    }
    return false;
}

bool has_text(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("vss_cli licence: with no licence it says community")
{
    fixture f;
    const run_result result = licence_cli(f, {});
    REQUIRE_EQ(result.status, 0);
    REQUIRE(result.err.empty());
    REQUIRE_EQ(lines_of(result.out),
               (std::vector<std::string>{"Edition: community", "State: none",
                                         "Reason: there is no licence",
                                         "Files: none"}));
    // Looking makes no licence folder.
    REQUIRE(!fs::exists(f.user()));
    REQUIRE(!fs::exists(f.machine()));
}

TEST_CASE("vss_cli licence: a Pro licence is described line by line")
{
    fixture f;
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    const run_result result = licence_cli(f, {});
    REQUIRE_EQ(result.status, 0);
    REQUIRE(result.err.empty());
    REQUIRE_EQ(
        lines_of(result.out),
        (std::vector<std::string>{
            "Edition: pro", "State: valid", "Licence: VSS-2026-000123",
            "Kind: annual", "Licensed to: Example Lab Pty Ltd",
            "Term: 2026-11-13 to 2027-11-13", "Days left: 165", "Seats: 3",
            "Files:", "  VSS-2026-000123.vsslic (user, valid, in use)"}));
}

TEST_CASE("vss_cli licence: a site licence of the Enterprise edition")
{
    fixture f;
    lic::licence site = annual("VSS-2026-000900");
    site.kind = lic::licence_kind::enterprise;
    site.edition = lic::edition::enterprise;
    site.seats = 0;
    write_file(f.machine(), "site.vsslic", licence_file(site));
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    write_file(f.user(), "broken.vsslic", "{}");

    const run_result result = licence_cli(f, {});
    REQUIRE_EQ(result.status, 0);
    REQUIRE(has_line(result.out, "Edition: enterprise"));
    REQUIRE(has_line(result.out, "Licence: VSS-2026-000900"));
    REQUIRE(has_line(result.out, "Seats: site licence"));
    REQUIRE(has_line(result.out, "Files:"));
    // The file in use comes first, then the others in name order.
    const std::vector<std::string> lines = lines_of(result.out);
    REQUIRE(lines.size() >= 3);
    REQUIRE_EQ(lines[lines.size() - 3],
               std::string("  site.vsslic (machine, valid, in use)"));
    REQUIRE_EQ(lines[lines.size() - 2],
               std::string("  VSS-2026-000123.vsslic (user, valid)"));
    REQUIRE_EQ(lines[lines.size() - 1],
               std::string("  broken.vsslic (user, invalid)"));
}

TEST_CASE("vss_cli licence: a trial is a Pro trial")
{
    fixture f;
    lic::licence trial = annual("VSS-2026-000500");
    trial.kind = lic::licence_kind::trial;
    trial.expires_at = at("2027-06-10T00:00:00Z");
    write_file(f.user(), "trial.vsslic", licence_file(trial));

    const run_result result = licence_cli(f, {});
    // A trial is the Pro edition; the kind of licence says it is a trial.
    REQUIRE(has_line(result.out, "Edition: pro"));
    REQUIRE(has_line(result.out, "Kind: trial"));
    REQUIRE(has_line(result.out, "State: valid"));
    REQUIRE(has_line(result.out, "Days left: 9"));
}

TEST_CASE("vss_cli licence: a licence that has ended is the Community "
          "edition")
{
    fixture f;
    f.now = at("2028-03-01T00:00:00Z");
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));

    const run_result result = licence_cli(f, {});
    REQUIRE_EQ(result.status, 0);
    REQUIRE(has_line(result.out, "Edition: community"));
    REQUIRE(has_line(result.out, "State: expired"));
    REQUIRE(has_line(result.out, "Reason: the licence has ended"));
    // The licence is still named, so a person can see whose it was.
    REQUIRE(has_line(result.out, "Licence: VSS-2026-000123"));
    REQUIRE(has_line(result.out, "Term: 2026-11-13 to 2027-11-13"));
    REQUIRE(!has_text(result.out, "Days left"));
}

TEST_CASE("vss_cli licence: a licence for other computers says so")
{
    fixture f;
    lic::licence elsewhere = annual("VSS-2026-000300");
    elsewhere.machines = {"OFFICE-*"};
    write_file(f.user(), "elsewhere.vsslic", licence_file(elsewhere));

    const run_result result = licence_cli(f, {});
    REQUIRE_EQ(result.status, 0);
    REQUIRE(has_line(result.out, "Edition: community"));
    REQUIRE(has_line(result.out, "State: invalid"));
    REQUIRE(has_line(result.out,
                     "Reason: the licence does not name this computer"));
}

TEST_CASE("vss_cli licence: it never prints a key or the contents of a file")
{
    fixture f;
    const std::string bytes = licence_file(annual());
    write_file(f.user(), "VSS-2026-000123.vsslic", bytes);
    write_file(f.user(), "junk.vsslic", "secret words in a file");

    const run_result result = licence_cli(f, {});
    REQUIRE_EQ(result.status, 0);
    REQUIRE(!has_text(result.out, "secret words"));
    REQUIRE(!has_text(result.out, "payload"));
    REQUIRE(!has_text(result.out, "signature"));
    REQUIRE(!has_text(result.out, "test-rfc8032-1"));
    REQUIRE(!has_text(result.out, "Features"));
}

TEST_CASE("vss_cli licence: a file name cannot change the terminal")
{
    fixture f;
    fs::create_directories(f.user());
    const std::string name = std::string("odd\x1b[31m.vsslic");
    std::ofstream file(f.user() / name, std::ios::binary);
    if (!file.is_open())
        VSS_SKIP("this system does not allow a control character in a name");
    file << "{}";
    file.close();

    const run_result result = licence_cli(f, {});
    REQUIRE_EQ(result.status, 0);
    REQUIRE(!has_text(result.out, "\x1b"));
    REQUIRE(has_text(result.out, "odd?[31m.vsslic"));
}

TEST_CASE("vss_cli licence import: copies a valid licence")
{
    fixture f;
    const std::string bytes = licence_file(annual());
    const fs::path source = write_file(f.inbox(), "from-mail.vsslic", bytes);

    const run_result result = licence_cli(f, {"import", source.string()});
    REQUIRE_EQ(result.status, 0);
    REQUIRE(result.err.empty());
    REQUIRE_EQ(lines_of(result.out).size(), std::size_t{1});
    REQUIRE(has_text(result.out, "Imported licence VSS-2026-000123"));
    REQUIRE(has_text(result.out, "valid"));
    REQUIRE(read_file(f.user() / "VSS-2026-000123.vsslic") == bytes);
    REQUIRE(fs::exists(source));

    // What `licence` shows afterwards.
    const run_result after = licence_cli(f, {});
    REQUIRE(has_line(after.out, "Edition: pro"));
}

TEST_CASE("vss_cli licence import: the same licence twice is not copied "
          "twice")
{
    fixture f;
    const fs::path source =
        write_file(f.inbox(), "a.vsslic", licence_file(annual()));
    REQUIRE_EQ(licence_cli(f, {"import", source.string()}).status, 0);

    const run_result again = licence_cli(f, {"import", source.string()});
    REQUIRE_EQ(again.status, 0);
    REQUIRE(has_text(again.out, "installed already"));
    REQUIRE_EQ(entries_in(f.user()), std::size_t{1});
}

TEST_CASE("vss_cli licence import: a file that is not a licence is refused")
{
    fixture f;
    const fs::path junk = write_file(f.inbox(), "junk.vsslic", "not a licence");
    const run_result result = licence_cli(f, {"import", junk.string()});
    REQUIRE_EQ(result.status, 1);
    REQUIRE(result.out.empty());
    REQUIRE_EQ(result.err,
               std::string("Not imported: the file is damaged or is not a "
                           "licence.\n"));
    REQUIRE(!fs::exists(f.user()));

    // A licence for other computers.
    lic::licence elsewhere = annual("VSS-2026-000300");
    elsewhere.machines = {"OFFICE-*"};
    const fs::path other =
        write_file(f.inbox(), "elsewhere.vsslic", licence_file(elsewhere));
    const run_result named = licence_cli(f, {"import", other.string()});
    REQUIRE_EQ(named.status, 1);
    REQUIRE(has_text(named.err, "does not name this computer"));

    // Too large to be a licence.
    const fs::path huge =
        write_file(f.inbox(), "huge.vsslic", too_large_file());
    REQUIRE_EQ(licence_cli(f, {"import", huge.string()}).status, 1);
    REQUIRE(!fs::exists(f.user()));
}

TEST_CASE("vss_cli licence import: a licence signed with the test key is "
          "refused by the release keys")
{
    // The program's own key list, as `vss_cli licence` uses it outside a
    // test. The published test key is not in it.
    fixture f;
    lic::store_options release = f.options();
    release.keys = lic::signed_file::product_trust_list();
    const fs::path source =
        write_file(f.inbox(), "test-key.vsslic", licence_file(annual()));

    std::ostringstream out;
    std::ostringstream err;
    const int status = vss_cli::command_licence(
        {"licence", "import", source.string()}, release, out, err);
    REQUIRE_EQ(status, 1);
    REQUIRE(out.str().empty());
    REQUIRE(has_text(err.str(), "Not imported:"));
    REQUIRE(!fs::exists(f.user()));
    // The original is where it was, and nothing of it was printed.
    REQUIRE(fs::exists(source));
    REQUIRE(!has_text(err.str(), "payload"));
    REQUIRE(!has_text(err.str(), "signature\""));
}

TEST_CASE("vss_cli licence import: a withdrawn licence is refused")
{
    fixture f;
    lic::revocation_list list;
    list.sequence = 3;
    list.issued_at = at("2027-02-01T00:00:00Z");
    list.licence_ids = {"VSS-2026-000123"};
    write_file(f.machine(), "revocations.vssrev",
               signed_with_test_key(lic::revocations_purpose,
                                    *lic::write_revocations(list)));
    const fs::path source =
        write_file(f.inbox(), "withdrawn.vsslic", licence_file(annual()));

    const run_result result = licence_cli(f, {"import", source.string()});
    REQUIRE_EQ(result.status, 1);
    REQUIRE(result.out.empty());
    REQUIRE(has_text(result.err, "withdrawn"));
    REQUIRE(!fs::exists(f.user()));

    // The same licence is copied when nothing withdraws it.
    fixture clean;
    const fs::path again =
        write_file(clean.inbox(), "good.vsslic", licence_file(annual()));
    REQUIRE_EQ(licence_cli(clean, {"import", again.string()}).status, 0);
    REQUIRE(fs::exists(clean.user() / "VSS-2026-000123.vsslic"));
}

TEST_CASE("vss_cli licence import: a file that is not there is refused")
{
    fixture f;
    const run_result result =
        licence_cli(f, {"import", (f.inbox() / "missing.vsslic").string()});
    REQUIRE_EQ(result.status, 1);
    REQUIRE(result.out.empty());
    REQUIRE(has_text(result.err, "cannot be read"));
    REQUIRE(!fs::exists(f.user()));
}

TEST_CASE("vss_cli licence import: no folder to copy to is refused")
{
    fixture f;
    const fs::path source =
        write_file(f.inbox(), "a.vsslic", licence_file(annual()));
    lic::store_options options = f.options();
    options.user_directory.clear();
    std::ostringstream out;
    std::ostringstream err;
    const int status = vss_cli::command_licence(
        {"licence", "import", source.string()}, options, out, err);
    REQUIRE_EQ(status, 1);
    REQUIRE(has_text(err.str(), "cannot be written"));
}

TEST_CASE("vss_cli licence: a command line it does not understand is a usage "
          "error")
{
    fixture f;
    const std::vector<std::vector<std::string>> wrong = {
        {"import"}, {"import", "a.vsslic", "b.vsslic"}, {"list"}, {"--json"}};
    for (const std::vector<std::string>& words : wrong)
    {
        const run_result result = licence_cli(f, words);
        REQUIRE_EQ(result.status, 2);
        REQUIRE(result.out.empty());
        REQUIRE(has_text(result.err, "vss_cli licence import <file>"));
    }
    REQUIRE(!fs::exists(f.user()));
}

TEST_CASE("vss_cli licence: the help names both commands")
{
    const std::string usage = vss_cli::licence_usage();
    REQUIRE(has_text(usage, "vss_cli licence\n"));
    REQUIRE(has_text(usage, "vss_cli licence import <file>"));
}

TEST_CASE("vss_cli licence: with no options of its own it reads this "
          "computer's folders")
{
    // Only a command line it refuses, so that nothing here depends on what
    // is installed on the computer that runs the test.
    REQUIRE_EQ(vss_cli::command_licence({"licence", "import"}), 2);
}
