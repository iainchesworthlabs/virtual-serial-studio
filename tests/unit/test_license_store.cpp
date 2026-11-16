//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence store against real folders in a temporary directory. Every
// licence here is signed with the published RFC 8032 test key, which the
// program itself never trusts.
//

#include <vss/license/store.hpp>

#include "licence_test_files.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace lic = vss::license;
using namespace vss_test_support::licence_files;

namespace
{

namespace fs = std::filesystem;

bool has_text(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("licence store: no files is the Community edition")
{
    fixture f;
    lic::store store(f.options());
    store.refresh();
    REQUIRE(store.view().status.state == lic::state::none);
    REQUIRE(store.view().files.empty());
    REQUIRE_EQ(store.view().computer_name, std::string("LAB-RIG-07"));
    // Nothing is made on disk just by looking.
    REQUIRE(!fs::exists(f.user()));
    REQUIRE(!fs::exists(f.machine()));
}

TEST_CASE("licence store: import copies a valid licence and leaves the "
          "original")
{
    fixture f;
    const std::string bytes = licence_file(annual());
    const fs::path source = write_file(f.inbox(), "from-mail.vsslic", bytes);

    lic::store store(f.options());
    const lic::import_result result = store.import_file(source);
    REQUIRE(result.outcome == lic::import_outcome::imported);
    REQUIRE(result.status.state == lic::state::valid);
    REQUIRE(fs::exists(source));

    const fs::path copy = f.user() / "VSS-2026-000123.vsslic";
    REQUIRE(read_file(copy) == bytes);
    // No half-written file is left beside it.
    REQUIRE_EQ(entries_in(f.user()), std::size_t{1});

    REQUIRE(store.view().status.state == lic::state::valid);
    REQUIRE_EQ(store.view().files.size(), std::size_t{1});
    REQUIRE(store.view().files.front().in_use);
    REQUIRE(!store.view().files.front().machine_wide);
    REQUIRE_EQ(store.view().files.front().file_name,
               std::string("VSS-2026-000123.vsslic"));
    REQUIRE(lic::check(store.view().status, "plugins.official").granted);

    // The same file again is not copied twice, and neither is the copy.
    REQUIRE(store.import_file(source).outcome ==
            lic::import_outcome::already_installed);
    REQUIRE(store.import_file(copy).outcome ==
            lic::import_outcome::already_installed);
    REQUIRE_EQ(entries_in(f.user()), std::size_t{1});
}

TEST_CASE("licence store: a licence in the machine folder is installed "
          "already")
{
    fixture f;
    const std::string bytes = licence_file(annual());
    write_file(f.machine(), "site.vsslic", bytes);
    lic::store store(f.options());
    const auto result =
        store.import_file(write_file(f.inbox(), "again.vsslic", bytes));
    REQUIRE(result.outcome == lic::import_outcome::already_installed);
    REQUIRE(!fs::exists(f.user()));
}

TEST_CASE("licence store: a file that is not valid is never copied")
{
    fixture f;
    lic::store store(f.options());
    REQUIRE(store
                .import_file(write_file(f.inbox(), "good.vsslic",
                                        licence_file(annual())))
                .outcome == lic::import_outcome::imported);

    // Damaged: one character of the signed text changed.
    std::string damaged = licence_file(annual("VSS-2026-000200"));
    const auto position = damaged.find("\"payload\":\"") + 20;
    damaged[position] = damaged[position] == 'A' ? 'B' : 'A';
    const auto bad =
        store.import_file(write_file(f.inbox(), "damaged.vsslic", damaged));
    REQUIRE(bad.outcome == lic::import_outcome::not_valid);
    REQUIRE(!bad.detail.empty());

    const auto junk = store.import_file(
        write_file(f.inbox(), "junk.vsslic", "not a licence"));
    REQUIRE(junk.outcome == lic::import_outcome::not_valid);
    REQUIRE(junk.status.reason == lic::reason::malformed);
    REQUIRE(!junk.detail.empty());

    // Signed for another purpose.
    const auto wrong_purpose = store.import_file(write_file(
        f.inbox(), "purpose.vsslic",
        signed_with_test_key("revocations", *lic::write_payload(annual()))));
    REQUIRE(wrong_purpose.outcome == lic::import_outcome::not_valid);
    REQUIRE(!wrong_purpose.detail.empty());

    // For other computers.
    lic::licence elsewhere = annual("VSS-2026-000300");
    elsewhere.machines = {"OFFICE-*"};
    const auto other = store.import_file(
        write_file(f.inbox(), "elsewhere.vsslic", licence_file(elsewhere)));
    REQUIRE(other.outcome == lic::import_outcome::not_valid);
    REQUIRE(other.status.reason == lic::reason::wrong_computer);
    // Its signature verified, so there is nothing for the details to say.
    REQUIRE(other.detail.empty());

    // Too large to be a licence, and a file that is not there.
    const auto huge = store.import_file(
        write_file(f.inbox(), "huge.vsslic", too_large_file()));
    REQUIRE(huge.outcome == lic::import_outcome::not_valid);
    REQUIRE(huge.status.reason == lic::reason::malformed);
    REQUIRE(!huge.detail.empty());
    REQUIRE(store.import_file(f.inbox() / "missing.vsslic").outcome ==
            lic::import_outcome::unreadable);
    // A folder is no licence either.
    REQUIRE(store.import_file(f.inbox()).outcome ==
            lic::import_outcome::unreadable);

    // The valid licence is still the only file and still in use.
    REQUIRE_EQ(entries_in(f.user()), std::size_t{1});
    REQUIRE(store.view().status.state == lic::state::valid);
}

TEST_CASE("licence store: a withdrawn licence is not imported")
{
    fixture f;
    lic::revocation_list list;
    list.sequence = 1;
    list.issued_at = at("2027-02-01T00:00:00Z");
    list.licence_ids = {"VSS-2026-000123"};
    write_file(f.machine(), "revocations.vssrev",
               signed_with_test_key(lic::revocations_purpose,
                                    *lic::write_revocations(list)));
    lic::store store(f.options());
    const auto result = store.import_file(
        write_file(f.inbox(), "withdrawn.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == lic::import_outcome::not_valid);
    REQUIRE(result.status.reason == lic::reason::revoked);
    REQUIRE(!fs::exists(f.user()));
}

TEST_CASE("licence store: the program's own key list accepts no test key")
{
    fixture f;
    lic::store_options options = f.options();
    options.keys = lic::signed_file::product_trust_list();
    lic::store store(options);
    const auto result = store.import_file(
        write_file(f.inbox(), "test-key.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == lic::import_outcome::not_valid);
    REQUIRE(!fs::exists(f.user()));
}

TEST_CASE("licence store: an ended licence is imported and turns nothing on")
{
    fixture f;
    f.now = at("2028-03-01T00:00:00Z");
    lic::store store(f.options());
    const auto result = store.import_file(
        write_file(f.inbox(), "old.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == lic::import_outcome::imported);
    REQUIRE(result.status.state == lic::state::expired);
    REQUIRE(!lic::check(store.view().status, "plugins.official").granted);
}

TEST_CASE("licence store: the best licence is used and the others are "
          "listed")
{
    fixture f;
    lic::licence site = annual("VSS-2026-000900");
    site.kind = lic::licence_kind::enterprise;
    site.edition = lic::edition::enterprise;
    site.seats = 0;
    write_file(f.machine(), "site.vsslic", licence_file(site));
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    write_file(f.user(), "broken.vsslic", "{}");
    write_file(f.user(), "notes.txt", "not looked at");

    lic::store store(f.options());
    REQUIRE(store.refresh());
    REQUIRE(!store.refresh());
    const lic::store_view& view = store.view();
    REQUIRE(view.status.value);
    REQUIRE_EQ(view.status.value->licence_id, std::string("VSS-2026-000900"));
    REQUIRE_EQ(view.files.size(), std::size_t{3});
    REQUIRE(view.files[0].in_use);
    REQUIRE(view.files[0].machine_wide);
    REQUIRE_EQ(view.files[0].file_name, std::string("site.vsslic"));
    REQUIRE(!view.files[1].in_use);
    REQUIRE_EQ(view.files[1].file_name, std::string("VSS-2026-000123.vsslic"));
    REQUIRE_EQ(view.files[2].file_name, std::string("broken.vsslic"));
    REQUIRE(view.files[2].status.state == lic::state::invalid);

    // An administrator's file is never removed; the user's own is.
    REQUIRE(!store.remove("site.vsslic"));
    REQUIRE(fs::exists(f.machine() / "site.vsslic"));
    REQUIRE(!store.remove("../../machine/licences/site.vsslic"));
    REQUIRE(!store.remove("missing.vsslic"));
    REQUIRE(!store.remove("notes.txt"));
    REQUIRE(store.remove("broken.vsslic"));
    REQUIRE(store.remove("VSS-2026-000123.vsslic"));
    REQUIRE_EQ(store.view().files.size(), std::size_t{1});
    REQUIRE(fs::exists(f.user() / "notes.txt"));
    REQUIRE(fs::exists(f.machine() / "site.vsslic"));
}

TEST_CASE("licence store: a new file in a folder changes the view")
{
    fixture f;
    lic::store store(f.options());
    REQUIRE(!store.refresh());
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    REQUIRE(store.refresh());
    REQUIRE(!store.refresh());
    // A different licence in the same file is a change too.
    write_file(f.user(), "VSS-2026-000123.vsslic",
               licence_file(annual("VSS-2026-000124")));
    REQUIRE(store.refresh());
    REQUIRE(store.remove("VSS-2026-000123.vsslic"));
    REQUIRE(store.view().status.state == lic::state::none);
}

TEST_CASE("licence store: only files with the suffix are read, in name order")
{
    fixture f;
    write_file(f.user(), "b.vsslic", "{}");
    write_file(f.user(), "A.VSSLIC", "{}");
    write_file(f.user(), "c.vsslic.txt", "{}");
    write_file(f.user(), "vsslic", "{}");
    write_file(f.user(), "d.vssrev", "{}");
    // A folder with the suffix is not a file.
    fs::create_directories(f.user() / "folder.vsslic");

    lic::store store(f.options());
    store.refresh();
    REQUIRE_EQ(store.view().files.size(), std::size_t{2});
    REQUIRE_EQ(store.view().files[0].file_name, std::string("A.VSSLIC"));
    REQUIRE_EQ(store.view().files[1].file_name, std::string("b.vsslic"));
}

TEST_CASE("licence store: at most 64 files are read from a folder")
{
    fixture f;
    for (int number = 0; number < 70; ++number)
    {
        const std::string digits = std::to_string(number);
        write_file(f.user(),
                   "f" + std::string(2 - digits.size(), '0') + digits +
                       ".vsslic",
                   "{}");
    }
    lic::store store(f.options());
    store.refresh();
    REQUIRE_EQ(store.view().files.size(), lic::max_files_per_folder);
    REQUIRE_EQ(store.view().files.front().file_name, std::string("f00.vsslic"));
    REQUIRE_EQ(store.view().files.back().file_name, std::string("f63.vsslic"));
}

TEST_CASE("licence store: a file too large to be a licence is listed and not "
          "read")
{
    fixture f;
    write_file(f.user(), "huge.vsslic", too_large_file());
    lic::store store(f.options());
    store.refresh();
    REQUIRE_EQ(store.view().files.size(), std::size_t{1});
    REQUIRE(store.view().files.front().status.state == lic::state::invalid);
    REQUIRE(store.view().files.front().status.reason == lic::reason::malformed);
}

TEST_CASE("licence store: importing a second copy never replaces a file")
{
    fixture f;
    lic::store store(f.options());
    write_file(f.user(), "VSS-2026-000123.vsslic", "an older file");
    const auto result = store.import_file(
        write_file(f.inbox(), "new.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == lic::import_outcome::imported);
    REQUIRE(read_file(f.user() / "VSS-2026-000123.vsslic") == "an older file");
    REQUIRE(fs::exists(f.user() / "VSS-2026-000123-2.vsslic"));

    // The next one takes the next number.
    write_file(f.user(), "VSS-2026-000124.vsslic", "another older file");
    const auto again = store.import_file(write_file(
        f.inbox(), "newer.vsslic", licence_file(annual("VSS-2026-000124"))));
    REQUIRE(again.outcome == lic::import_outcome::imported);
    REQUIRE(fs::exists(f.user() / "VSS-2026-000124-2.vsslic"));
}

TEST_CASE("licence store: a link at the new name is not written through")
{
    fixture f;
    fs::create_directories(f.user());
    std::error_code failed;
    fs::create_symlink(f.root.path() / "nowhere",
                       f.user() / "VSS-2026-000123.vsslic", failed);
    if (failed)
        VSS_SKIP("this system does not let the test make a link");

    lic::store store(f.options());
    const auto result = store.import_file(
        write_file(f.inbox(), "new.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == lic::import_outcome::imported);
    REQUIRE(!fs::exists(f.root.path() / "nowhere"));
    REQUIRE(fs::exists(f.user() / "VSS-2026-000123-2.vsslic"));
}

TEST_CASE("licence store: an import that has no free name or no folder is "
          "unreadable")
{
    fixture f;
    const auto source =
        write_file(f.inbox(), "new.vsslic", licence_file(annual()));
    {
        // Every name the store would try is taken.
        write_file(f.user(), "VSS-2026-000123.vsslic", "1");
        for (int number = 2; number <= 64; ++number)
            write_file(f.user(),
                       "VSS-2026-000123-" + std::to_string(number) + ".vsslic",
                       std::to_string(number));
        lic::store store(f.options());
        REQUIRE(store.import_file(source).outcome ==
                lic::import_outcome::unreadable);
        REQUIRE_EQ(entries_in(f.user()), std::size_t{64});
    }
    {
        // No folder to copy to.
        lic::store_options options = f.options();
        options.user_directory.clear();
        lic::store store(options);
        REQUIRE(store.import_file(source).outcome ==
                lic::import_outcome::unreadable);
        REQUIRE(!store.remove("VSS-2026-000123.vsslic"));
    }
    {
        // A file where the folder should be.
        fixture g;
        write_file(g.root.path() / "user", "licences", "a file");
        lic::store store(g.options());
        const auto result = store.import_file(source);
        REQUIRE(result.outcome == lic::import_outcome::unreadable);
    }
}

TEST_CASE("licence store: a licence id never becomes a path")
{
    // The library refuses an id with anything but letters, digits and
    // hyphens, so no such licence can be signed or read. The store also
    // builds the file name from those characters only.
    REQUIRE(!lic::write_payload(annual("../../evil id")));

    fixture f;
    lic::store store(f.options());
    const auto result = store.import_file(write_file(
        f.inbox(), "odd name with spaces.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == lic::import_outcome::imported);
    REQUIRE_EQ(entries_in(f.user()), std::size_t{1});
    REQUIRE(fs::exists(f.user() / "VSS-2026-000123.vsslic"));
}

TEST_CASE("licence store: a signed revocation list withdraws a licence")
{
    fixture f;
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    lic::store store(f.options());
    store.refresh();
    REQUIRE(store.view().status.state == lic::state::valid);

    lic::revocation_list list;
    list.sequence = 3;
    list.issued_at = at("2027-02-01T00:00:00Z");
    list.licence_ids = {"VSS-2026-000123"};
    write_file(f.machine(), "revocations.vssrev",
               signed_with_test_key(lic::revocations_purpose,
                                    *lic::write_revocations(list)));
    // A list nobody signed is ignored.
    write_file(f.user(), "forged.vssrev", "{\"sequence\":99}");

    REQUIRE(store.refresh());
    REQUIRE(store.view().status.state == lic::state::invalid);
    REQUIRE(store.view().status.reason == lic::reason::revoked);
}

TEST_CASE("licence store: the revocation list with the higher number wins")
{
    fixture f;
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));

    lic::revocation_list newer_list;
    newer_list.sequence = 5;
    newer_list.issued_at = at("2027-02-01T00:00:00Z");
    newer_list.licence_ids = {"VSS-2026-000123"};
    write_file(f.machine(), "newer.vssrev",
               signed_with_test_key(lic::revocations_purpose,
                                    *lic::write_revocations(newer_list)));
    lic::revocation_list older_list;
    older_list.sequence = 2;
    older_list.issued_at = at("2027-01-01T00:00:00Z");
    write_file(f.user(), "older.vssrev",
               signed_with_test_key(lic::revocations_purpose,
                                    *lic::write_revocations(older_list)));
    // A list too large to be read is ignored.
    write_file(f.user(), "huge.vssrev", too_large_file());

    lic::store store(f.options());
    store.refresh();
    REQUIRE(store.view().status.reason == lic::reason::revoked);
}

TEST_CASE("licence store: the latest time seen is kept between runs")
{
    fixture f;
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    {
        lic::store store(f.options());
        store.refresh();
        REQUIRE(store.view().status.state == lic::state::valid);
    }
    REQUIRE(fs::exists(f.clock_file()));
    REQUIRE_EQ(read_file(f.clock_file()),
               std::string("{\"latest_seen\":\"2027-06-01T00:00:00Z\","
                           "\"applied_resets\":[]}"));

    // The clock is set back past the tolerance: a new run notices.
    f.now = at("2027-01-01T00:00:00Z");
    {
        lic::store store(f.options());
        store.refresh();
        REQUIRE(store.view().status.state == lic::state::clock_suspect);
    }
    // And the stored time did not move back.
    REQUIRE(has_text(read_file(f.clock_file()), "2027-06-01T00:00:00Z"));

    // A licence with a reset token sets the stored time back, once.
    lic::licence reset = annual("VSS-2026-000124");
    reset.issued_at = at("2026-12-20T00:00:00Z");
    reset.clock_reset = "reset-1";
    {
        lic::store store(f.options());
        const auto result = store.import_file(
            write_file(f.inbox(), "reset.vsslic", licence_file(reset)));
        REQUIRE(result.outcome == lic::import_outcome::imported);
        REQUIRE(result.status.state == lic::state::valid);
        REQUIRE(store.view().status.state == lic::state::valid);
    }
    {
        lic::store store(f.options());
        store.refresh();
        REQUIRE(store.view().status.state == lic::state::valid);
    }
    REQUIRE(has_text(read_file(f.clock_file()), "\"reset-1\""));

    // The same token in another licence does nothing the second time.
    lic::licence again = annual("VSS-2026-000125");
    again.issued_at = at("2026-12-25T00:00:00Z");
    again.clock_reset = "reset-1";
    f.now = at("2027-06-02T00:00:00Z");
    {
        lic::store store(f.options());
        const auto result = store.import_file(
            write_file(f.inbox(), "again.vsslic", licence_file(again)));
        REQUIRE(result.outcome == lic::import_outcome::imported);
    }
    const std::string text = read_file(f.clock_file());
    REQUIRE(text.find("reset-1") == text.rfind("reset-1"));
    REQUIRE(has_text(text, "2027-06-02T00:00:00Z"));
}

TEST_CASE("licence store: a clock file that cannot be read is the same as "
          "none")
{
    fixture f;
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    const std::string good = "{\"latest_seen\":\"2027-06-01T00:00:00Z\","
                             "\"applied_resets\":[\"reset-1\"]}";

    const std::string unreadable[] = {
        "not json",
        "[]",
        "{\"latest_seen\":\"2027-06-01T00:00:00Z\",\"latest_seen\":\"x\"}",
        "{\"latest_seen\":5,\"applied_resets\":\"x\"}",
        "{\"latest_seen\":\"yesterday\",\"applied_resets\":[1,null]}",
        std::string(too_large_file().size(), ' ') + good};
    for (const std::string& text : unreadable)
    {
        write_file(f.root.path() / "user", "clock.json", text);
        // The clock reads 2027-01-01. Had the text above been taken, a later
        // time would make it suspect.
        f.now = at("2027-01-01T00:00:00Z");
        lic::store store(f.options());
        store.refresh();
        REQUIRE(store.view().status.state == lic::state::valid);
    }

    // The good file, for comparison, makes the same clock suspect.
    write_file(f.root.path() / "user", "clock.json", good);
    f.now = at("2027-01-01T00:00:00Z");
    lic::store store(f.options());
    store.refresh();
    REQUIRE(store.view().status.state == lic::state::clock_suspect);
}

TEST_CASE("licence store: with no clock file the time is kept in memory")
{
    fixture f;
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    lic::store_options options = f.options();
    options.clock_file.clear();
    lic::store store(options);
    store.refresh();
    REQUIRE(store.view().status.state == lic::state::valid);

    f.now = at("2027-01-01T00:00:00Z");
    store.refresh();
    REQUIRE(store.view().status.state == lic::state::clock_suspect);
    REQUIRE(!fs::exists(f.clock_file()));
}

TEST_CASE("licence store: a clock file in a folder that is not there makes "
          "the folder")
{
    fixture f;
    lic::store_options options = f.options();
    options.clock_file = f.root.path() / "a" / "b" / "clock.json";
    lic::store store(options);
    store.refresh();
    REQUIRE(fs::exists(options.clock_file));
}

TEST_CASE("licence store: without a clock of its own it uses the system "
          "clock")
{
    fixture f;
    lic::store_options options = f.options();
    options.clock = {};
    {
        lic::store store(options);
        store.refresh();
    }
    const std::string text = read_file(f.clock_file());
    REQUIRE(has_text(text, "latest_seen"));
    // The time written is a time that has passed.
    const auto start = text.find("\"latest_seen\":\"") + 15;
    const auto seen = lic::parse_time(text.substr(start, 20));
    REQUIRE(seen);
    REQUIRE(*seen >= at("2026-01-01T00:00:00Z"));
    REQUIRE(*seen <= std::chrono::floor<std::chrono::seconds>(
                         std::chrono::system_clock::now()));
}

TEST_CASE("licence store: the build time is a time that has passed")
{
    fixture f;
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    lic::store_options options = f.options();
    options.build_time = at("2027-05-01T00:00:00Z");
    f.now = at("2027-01-01T00:00:00Z");
    lic::store store(options);
    store.refresh();
    REQUIRE(store.view().status.state == lic::state::clock_suspect);
}

TEST_CASE("licence store: a licence for named computers is judged with the "
          "computer's name")
{
    fixture f;
    lic::licence named = annual();
    named.machines = {"lab-*"};
    write_file(f.user(), "named.vsslic", licence_file(named));
    lic::store store(f.options());
    store.refresh();
    REQUIRE(store.view().status.state == lic::state::valid);

    lic::store_options options = f.options();
    options.computer_name = "OFFICE-1";
    lic::store other(options);
    other.refresh();
    REQUIRE(other.view().status.reason == lic::reason::wrong_computer);
}
