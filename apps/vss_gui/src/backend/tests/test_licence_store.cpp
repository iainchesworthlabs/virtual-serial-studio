//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence store of the GUI is an adaptor over vss::license::store (whose
// own tests are in tests/unit/test_license_store.cpp): it turns the library's
// answers into the values a screen is given. Every licence here is signed
// with the published RFC 8032 test key, which the program itself never trusts.
//

#include "backend/fake_studio_backend.hpp"
#include "backend/licence_store.hpp"

#include "licence_test_files.hpp"
#include "test_framework.hpp"

#include <QFile>
#include <QString>

#include <filesystem>
#include <string>

using namespace vss::gui;
using namespace vss_test_support::licence_files;

namespace
{

QString text(const std::filesystem::path& path)
{
    return QString::fromStdString(path.generic_string());
}

lic::store_view sample_view()
{
    lic::store_view view;
    view.computer_name = "LAB-RIG-07";
    lic::found_licence site;
    site.status.state = lic::state::valid;
    site.status.reason = lic::reason::ok;
    site.status.days_left = 40;
    site.file_name = "site.vsslic";
    site.machine_wide = true;
    site.in_use = true;
    lic::found_licence own;
    own.status.state = lic::state::invalid;
    own.status.reason = lic::reason::malformed;
    // An e with an acute accent, in UTF-8.
    own.file_name = "Lizenz-\xc3\xa9.vsslic";
    view.files = {site, own};
    view.status = site.status;
    return view;
}

} // namespace

TEST_CASE("licence store: the library's view becomes the screens' view")
{
    const licence_view view = to_licence_view(sample_view());
    REQUIRE(view.status.state == lic::state::valid);
    REQUIRE_EQ(view.status.days_left, 40);
    REQUIRE_EQ(view.computer_name, QStringLiteral("LAB-RIG-07"));
    REQUIRE_EQ(view.files.size(), std::size_t{2});
    REQUIRE_EQ(view.files[0].file_name, QStringLiteral("site.vsslic"));
    REQUIRE(view.files[0].machine_wide);
    REQUIRE(view.files[0].in_use);
    REQUIRE(view.files[0].status.state == lic::state::valid);
    // File names are UTF-8 in the library and text on a screen.
    REQUIRE_EQ(view.files[1].file_name,
               QString::fromUtf8("Lizenz-\xc3\xa9.vsslic"));
    REQUIRE(!view.files[1].machine_wide);
    REQUIRE(!view.files[1].in_use);
    REQUIRE(view.files[1].status.reason == lic::reason::malformed);

    // Nothing found is the Community edition.
    const licence_view none = to_licence_view(lic::store_view{});
    REQUIRE(none.status.state == lic::state::none);
    REQUIRE(none.files.empty());
}

TEST_CASE("licence store: every import outcome has its own")
{
    const struct
    {
        lic::import_outcome from;
        licence_import_outcome to;
    } outcomes[] = {
        {lic::import_outcome::imported, licence_import_outcome::imported},
        {lic::import_outcome::already_installed,
         licence_import_outcome::already_installed},
        {lic::import_outcome::not_valid, licence_import_outcome::not_valid},
        {lic::import_outcome::unreadable, licence_import_outcome::unreadable}};
    for (const auto& outcome : outcomes)
    {
        lic::import_result result;
        result.outcome = outcome.from;
        result.status.state = lic::state::invalid;
        result.status.reason = lic::reason::bad_signature;
        result.detail = "details";
        const licence_import_result converted =
            to_licence_import_result(result);
        REQUIRE(converted.outcome == outcome.to);
        REQUIRE(converted.status.reason == lic::reason::bad_signature);
        REQUIRE_EQ(converted.detail, QStringLiteral("details"));
    }
}

TEST_CASE("licence store: a path from the dialog keeps its characters")
{
    const std::filesystem::path path =
        to_path(QString::fromUtf8("/tmp/Lizenz-\xc3\xa9/a b.vsslic"));
    const std::u8string back = path.generic_u8string();
    REQUIRE_EQ(std::string(back.begin(), back.end()),
               std::string("/tmp/Lizenz-\xc3\xa9/a b.vsslic"));
    REQUIRE(to_path(QString()).empty());
}

TEST_CASE("licence store: import, refresh and remove keep the view current")
{
    fixture f;
    licence_store store(f.options());
    REQUIRE(store.view().status.state == lic::state::none);
    REQUIRE_EQ(store.view().computer_name, QStringLiteral("LAB-RIG-07"));
    REQUIRE(!store.refresh());

    const QString source =
        text(write_file(f.inbox(), "from-mail.vsslic", licence_file(annual())));
    const licence_import_result imported = store.import_file(source);
    REQUIRE(imported.outcome == licence_import_outcome::imported);
    REQUIRE(imported.status.state == lic::state::valid);
    REQUIRE(store.view().status.state == lic::state::valid);
    REQUIRE_EQ(store.view().files.size(), std::size_t{1});
    REQUIRE_EQ(store.view().files.front().file_name,
               QStringLiteral("VSS-2026-000123.vsslic"));
    REQUIRE(store.view().files.front().in_use);
    REQUIRE(QFile::exists(source));

    REQUIRE(store.import_file(source).outcome ==
            licence_import_outcome::already_installed);
    REQUIRE(store.import_file(text(f.inbox() / "missing.vsslic")).outcome ==
            licence_import_outcome::unreadable);
    const licence_import_result junk = store.import_file(
        text(write_file(f.inbox(), "junk.vsslic", "not a licence")));
    REQUIRE(junk.outcome == licence_import_outcome::not_valid);
    REQUIRE(junk.status.reason == lic::reason::malformed);
    REQUIRE(!junk.detail.isEmpty());

    // A file put in the folder by hand is noticed by a refresh.
    write_file(f.user(), "second.vsslic",
               licence_file(annual("VSS-2026-000124")));
    REQUIRE(store.refresh());
    REQUIRE_EQ(store.view().files.size(), std::size_t{2});
    REQUIRE(!store.refresh());

    REQUIRE(!store.remove(QStringLiteral("missing.vsslic")));
    REQUIRE(store.remove(QStringLiteral("second.vsslic")));
    REQUIRE_EQ(store.view().files.size(), std::size_t{1});
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
