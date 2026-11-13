//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence store against real folders in a temporary directory. Every
// licence here is signed with the published RFC 8032 test key, which the
// program itself never trusts.
//

#include "backend/fake_studio_backend.hpp"
#include "backend/licence_store.hpp"

#include <vss/license/sign.hpp>
#include <vss/license/test_keys.hpp>

#include "test_framework.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using namespace vss::gui;
namespace lic = vss::license;

namespace
{

lic::time_point at(std::string_view text)
{
    return *lic::parse_time(text);
}

lic::licence annual(std::string id = "VSS-2026-000123")
{
    lic::licence value;
    value.licence_id = std::move(id);
    value.kind = lic::licence_kind::annual;
    value.edition = lic::edition::pro;
    value.licensee_name = "Example Lab Pty Ltd";
    value.issued_at = at("2026-11-13T02:00:00Z");
    value.starts_at = at("2026-11-13T00:00:00Z");
    value.expires_at = at("2027-11-13T00:00:00Z");
    value.grace_days = 28;
    value.seats = 3;
    value.features = {"plugins.official", "driver.signed", "support"};
    return value;
}

QByteArray signed_with_test_key(std::string_view purpose,
                                const std::string& payload)
{
    const std::span<const std::uint8_t> bytes{
        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()};
    const std::string text = lic::signed_file::write(lic::signing::sign(
        purpose, lic::test_keys::key_id, bytes, lic::test_keys::private_key()));
    return QByteArray::fromStdString(text);
}

QByteArray licence_file(const lic::licence& value)
{
    return signed_with_test_key(lic::licence_purpose,
                                *lic::write_payload(value));
}

QString write_file(const QString& folder, const QString& name,
                   const QByteArray& bytes)
{
    QDir().mkpath(folder);
    const QString path = QDir(folder).filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    return path;
}

/// Folders of a test and a clock the test moves.
struct fixture
{
    QTemporaryDir root;
    lic::time_point now = at("2027-06-01T00:00:00Z");

    QString user() const { return root.filePath("user/licences"); }
    QString machine() const { return root.filePath("machine/licences"); }
    QString inbox() const { return root.filePath("inbox"); }
    QString clock_file() const { return root.filePath("user/clock.json"); }

    licence_store_options options()
    {
        licence_store_options value;
        value.user_directory = user();
        value.machine_directory = machine();
        value.clock_file = clock_file();
        const auto keys = lic::test_keys::trusted();
        value.keys.assign(keys.begin(), keys.end());
        value.computer_name = QStringLiteral("LAB-RIG-07");
        value.clock = [this] { return now; };
        return value;
    }
};

} // namespace

TEST_CASE("licence store: no files is the Community edition")
{
    fixture f;
    licence_store store(f.options());
    store.refresh();
    REQUIRE(store.view().status.state == lic::state::none);
    REQUIRE(store.view().files.empty());
    REQUIRE_EQ(store.view().computer_name, QStringLiteral("LAB-RIG-07"));
    // Nothing is made on disk just by looking.
    REQUIRE(!QDir(f.user()).exists());
}

TEST_CASE("licence store: import copies a valid licence and leaves the "
          "original")
{
    fixture f;
    const QByteArray bytes = licence_file(annual());
    const QString source = write_file(f.inbox(), "from-mail.vsslic", bytes);

    licence_store store(f.options());
    const licence_import_result result = store.import_file(source);
    REQUIRE(result.outcome == licence_import_outcome::imported);
    REQUIRE(result.status.state == lic::state::valid);
    REQUIRE(QFile::exists(source));

    const QString copy = QDir(f.user()).filePath("VSS-2026-000123.vsslic");
    QFile installed(copy);
    REQUIRE(installed.open(QIODevice::ReadOnly));
    REQUIRE(installed.readAll() == bytes);

    REQUIRE(store.view().status.state == lic::state::valid);
    REQUIRE_EQ(store.view().files.size(), std::size_t{1});
    REQUIRE(store.view().files.front().in_use);
    REQUIRE(!store.view().files.front().machine_wide);
    REQUIRE(lic::check(store.view().status, "plugins.official").granted);

    // The same file again is not copied twice.
    REQUIRE(store.import_file(source).outcome ==
            licence_import_outcome::already_installed);
    REQUIRE_EQ(QDir(f.user()).entryList(QDir::Files).size(), 1);
}

TEST_CASE("licence store: a file that is not valid is never copied")
{
    fixture f;
    licence_store store(f.options());
    REQUIRE(store
                .import_file(write_file(f.inbox(), "good.vsslic",
                                        licence_file(annual())))
                .outcome == licence_import_outcome::imported);

    // Damaged: one character of the signed text changed.
    QByteArray damaged = licence_file(annual("VSS-2026-000200"));
    const auto position = damaged.indexOf("\"payload\":\"") + 20;
    damaged[position] = damaged[position] == 'A' ? 'B' : 'A';
    const auto bad =
        store.import_file(write_file(f.inbox(), "damaged.vsslic", damaged));
    REQUIRE(bad.outcome == licence_import_outcome::not_valid);
    REQUIRE(!bad.detail.isEmpty());

    const auto junk = store.import_file(
        write_file(f.inbox(), "junk.vsslic", "not a licence"));
    REQUIRE(junk.outcome == licence_import_outcome::not_valid);
    REQUIRE(junk.status.reason == lic::reason::malformed);

    // Signed for another purpose.
    const auto wrong_purpose = store.import_file(write_file(
        f.inbox(), "purpose.vsslic",
        signed_with_test_key("revocations", *lic::write_payload(annual()))));
    REQUIRE(wrong_purpose.outcome == licence_import_outcome::not_valid);

    // For other computers.
    lic::licence elsewhere = annual("VSS-2026-000300");
    elsewhere.machines = {"OFFICE-*"};
    const auto other = store.import_file(
        write_file(f.inbox(), "elsewhere.vsslic", licence_file(elsewhere)));
    REQUIRE(other.outcome == licence_import_outcome::not_valid);
    REQUIRE(other.status.reason == lic::reason::wrong_computer);

    // Too large to be a licence, and a file that is not there.
    REQUIRE(store
                .import_file(write_file(f.inbox(), "huge.vsslic",
                                        QByteArray(70 * 1024, 'x')))
                .outcome == licence_import_outcome::not_valid);
    REQUIRE(
        store.import_file(QDir(f.inbox()).filePath("missing.vsslic")).outcome ==
        licence_import_outcome::unreadable);

    // The valid licence is still the only file and still in use.
    REQUIRE_EQ(QDir(f.user()).entryList(QDir::Files).size(), 1);
    REQUIRE(store.view().status.state == lic::state::valid);
}

TEST_CASE("licence store: the program's own key list accepts no test key")
{
    fixture f;
    licence_store_options options = f.options();
    options.keys = lic::signed_file::product_trust_list();
    licence_store store(options);
    const auto result = store.import_file(
        write_file(f.inbox(), "test-key.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == licence_import_outcome::not_valid);
    REQUIRE(!QDir(f.user()).exists());
}

TEST_CASE("licence store: an ended licence is imported and turns nothing on")
{
    fixture f;
    f.now = at("2028-03-01T00:00:00Z");
    licence_store store(f.options());
    const auto result = store.import_file(
        write_file(f.inbox(), "old.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == licence_import_outcome::imported);
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

    licence_store store(f.options());
    REQUIRE(store.refresh());
    REQUIRE(!store.refresh());
    const licence_view& view = store.view();
    REQUIRE(view.status.value);
    REQUIRE_EQ(view.status.value->licence_id, std::string("VSS-2026-000900"));
    REQUIRE_EQ(view.files.size(), std::size_t{3});
    REQUIRE(view.files[0].in_use);
    REQUIRE(view.files[0].machine_wide);
    REQUIRE_EQ(view.files[0].file_name, QStringLiteral("site.vsslic"));

    // An administrator's file is never removed; the user's own is.
    REQUIRE(!store.remove(QStringLiteral("site.vsslic")));
    REQUIRE(QFile::exists(QDir(f.machine()).filePath("site.vsslic")));
    REQUIRE(!store.remove(QStringLiteral("../machine/licences/site.vsslic")));
    REQUIRE(store.remove(QStringLiteral("broken.vsslic")));
    REQUIRE(store.remove(QStringLiteral("VSS-2026-000123.vsslic")));
    REQUIRE_EQ(store.view().files.size(), std::size_t{1});
    REQUIRE(QFile::exists(QDir(f.user()).filePath("notes.txt")));
}

TEST_CASE("licence store: importing a second copy never replaces a file")
{
    fixture f;
    licence_store store(f.options());
    write_file(f.user(), "VSS-2026-000123.vsslic", "an older file");
    const auto result = store.import_file(
        write_file(f.inbox(), "new.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == licence_import_outcome::imported);
    QFile older(QDir(f.user()).filePath("VSS-2026-000123.vsslic"));
    REQUIRE(older.open(QIODevice::ReadOnly));
    REQUIRE(older.readAll() == QByteArray("an older file"));
    REQUIRE(QFile::exists(QDir(f.user()).filePath("VSS-2026-000123-2.vsslic")));
}

TEST_CASE("licence store: a licence id never becomes a path")
{
    // The library refuses an id with anything but letters, digits and
    // hyphens, so no such licence can be signed or read. The store also
    // builds the file name from those characters only.
    REQUIRE(!lic::write_payload(annual("../../evil id")));

    fixture f;
    licence_store store(f.options());
    const auto result = store.import_file(write_file(
        f.inbox(), "odd name with spaces.vsslic", licence_file(annual())));
    REQUIRE(result.outcome == licence_import_outcome::imported);
    REQUIRE_EQ(QDir(f.user()).entryList(QDir::Files),
               QStringList{QStringLiteral("VSS-2026-000123.vsslic")});
}

TEST_CASE("licence store: a signed revocation list withdraws a licence")
{
    fixture f;
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    licence_store store(f.options());
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

TEST_CASE("licence store: the latest time seen is kept between runs")
{
    fixture f;
    write_file(f.user(), "VSS-2026-000123.vsslic", licence_file(annual()));
    {
        licence_store store(f.options());
        store.refresh();
        REQUIRE(store.view().status.state == lic::state::valid);
    }
    REQUIRE(QFile::exists(f.clock_file()));

    // The clock is set back past the tolerance: a new run notices.
    f.now = at("2027-01-01T00:00:00Z");
    {
        licence_store store(f.options());
        store.refresh();
        REQUIRE(store.view().status.state == lic::state::clock_suspect);
    }

    // A licence with a reset token sets the stored time back, once.
    lic::licence reset = annual("VSS-2026-000124");
    reset.issued_at = at("2026-12-20T00:00:00Z");
    reset.clock_reset = "reset-1";
    {
        licence_store store(f.options());
        const auto result = store.import_file(
            write_file(f.inbox(), "reset.vsslic", licence_file(reset)));
        REQUIRE(result.outcome == licence_import_outcome::imported);
        REQUIRE(result.status.state == lic::state::valid);
        REQUIRE(store.view().status.state == lic::state::valid);
    }
    {
        licence_store store(f.options());
        store.refresh();
        REQUIRE(store.view().status.state == lic::state::valid);
    }
}

TEST_CASE("fake backend: a test sets any licence and sees what was asked")
{
    FakeStudioBackend backend;
    REQUIRE(backend.licence().status.state == lic::state::none);
    REQUIRE(!backend.entitlement(QStringLiteral("plugins.official")).granted);
    REQUIRE(backend.entitlement(QStringLiteral("plugins.official")).reason ==
            lic::reason::no_licence);
    REQUIRE(backend.import_licence(QStringLiteral("a.vsslic")).outcome ==
            licence_import_outcome::unsupported);

    int changed = 0;
    QObject::connect(&backend, &StudioBackend::licence_changed, &backend,
                     [&changed] { ++changed; });
    licence_view view;
    view.status = lic::evaluate(annual(), nullptr,
                                []
                                {
                                    lic::context where;
                                    where.now = at("2027-06-01T00:00:00Z");
                                    return where;
                                }());
    licence_file_view file;
    file.status = view.status;
    file.file_name = QStringLiteral("VSS-2026-000123.vsslic");
    file.in_use = true;
    view.files.push_back(file);

    licence_import_result imported;
    imported.outcome = licence_import_outcome::imported;
    imported.status = view.status;
    backend.set_import_result(imported, view);
    REQUIRE(backend.import_licence(QStringLiteral("b.vsslic")).outcome ==
            licence_import_outcome::imported);
    REQUIRE_EQ(changed, 1);
    REQUIRE(backend.entitlement(QStringLiteral("plugins.official")).granted);
    REQUIRE(
        !backend.entitlement(QStringLiteral("policy.official-only")).granted);
    REQUIRE_EQ(backend.imported_paths().size(), 2);

    REQUIRE(!backend.remove_licence(QStringLiteral("other.vsslic")));
    REQUIRE(backend.remove_licence(QStringLiteral("VSS-2026-000123.vsslic")));
    REQUIRE(backend.licence().status.state == lic::state::none);
    REQUIRE_EQ(changed, 2);
}
