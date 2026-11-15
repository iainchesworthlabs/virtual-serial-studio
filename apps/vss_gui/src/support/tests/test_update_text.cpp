//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The words of the update screens, which have no widgets: the status sentence
// of every state, the kinds of install, the size, the time of the last check
// and the texts of the banners and the chip.
//

#include "test_framework.hpp"

#include "support/update_text.hpp"

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QTime>
#include <QTimeZone>

#include <optional>

using namespace vss::gui;
namespace upd = vss::update;

namespace
{

/// English as written in Australia for the length of a test, so the words
/// and the decimal separator are the ones the spec shows.
class english_locale
{
public:
    english_locale() : saved_(QLocale())
    {
        QLocale::setDefault(QLocale(QLocale::English, QLocale::Australia));
    }
    ~english_locale() { QLocale::setDefault(saved_); }
    english_locale(const english_locale&) = delete;
    english_locale& operator=(const english_locale&) = delete;

private:
    QLocale saved_;
};

updates::studio_offer offer_of(const char* version, bool with_file)
{
    updates::studio_offer offer;
    offer.version_text = QString::fromLatin1(version);
    if (with_file)
    {
        updates::offered_file file;
        file.name = QStringLiteral("studio.exe");
        file.size = 1000;
        offer.file = file;
    }
    return offer;
}

updates::snapshot state_of(updates::check_state state)
{
    updates::snapshot snapshot;
    snapshot.state = state;
    return snapshot;
}

QString status(const updates::snapshot& snapshot)
{
    return support::update_status_text(snapshot, QStringLiteral("0.2.0"));
}

} // namespace

TEST_CASE("update text: a size is written in MB, one decimal below 10 MB")
{
    const english_locale locale;
    constexpr std::uint64_t kMegabyte = std::uint64_t{1} << 20;
    REQUIRE_EQ(support::update_size_text(3355443), QStringLiteral("3.2 MB"));
    // 9.9 MB keeps its decimal.
    REQUIRE_EQ(support::update_size_text(10380902), QStringLiteral("9.9 MB"));
    // 9.96 MB would read "10.0 MB", so it is written whole.
    REQUIRE_EQ(support::update_size_text(10444000), QStringLiteral("10 MB"));
    REQUIRE_EQ(support::update_size_text(10 * kMegabyte),
               QStringLiteral("10 MB"));
    REQUIRE_EQ(support::update_size_text(48 * kMegabyte),
               QStringLiteral("48 MB"));
    REQUIRE_EQ(support::update_size_text(0), QStringLiteral("0.0 MB"));
}

TEST_CASE("update text: the three kinds of install")
{
    using upd::file_kind;
    for (const file_kind kind : {file_kind::windows_installer,
                                 file_kind::linux_deb, file_kind::linux_rpm})
        REQUIRE_EQ(support::install_kind_text(kind),
                   QStringLiteral("installed for everyone on this computer"));
    for (const file_kind kind :
         {file_kind::windows_zip, file_kind::macos_archive,
          file_kind::linux_archive})
        REQUIRE_EQ(support::install_kind_text(kind),
                   QStringLiteral("unpacked from an archive"));
    REQUIRE_EQ(support::install_kind_text(file_kind::macos_dmg),
               QStringLiteral("copied from the disk image"));
}

TEST_CASE("update text: the status sentence of every state")
{
    using updates::check_failure;
    using updates::check_state;

    REQUIRE_EQ(status(state_of(check_state::idle)),
               QStringLiteral("Not checked yet."));
    REQUIRE_EQ(status(state_of(check_state::checking)),
               QStringLiteral("Checking..."));
    // `current` names the running version.
    REQUIRE_EQ(status(state_of(check_state::current)),
               QStringLiteral("0.2.0 is the newest version."));

    // `available` names the offer's version, with or without a file.
    updates::snapshot available = state_of(check_state::available);
    available.offer = offer_of("0.3.0-rc.1", true);
    REQUIRE_EQ(status(available), QStringLiteral("0.3.0-rc.1 is available."));
    available.offer = offer_of("0.3.0", false);
    REQUIRE_EQ(status(available),
               QStringLiteral("0.3.0 is available. Download it from "
                              "virtual-serial-studio.com."));

    updates::snapshot failed = state_of(check_state::failed);
    failed.failure = check_failure::unreachable;
    REQUIRE_EQ(status(failed),
               QStringLiteral("Studio could not reach the update server. "
                              "Check the connection and try again."));
    failed.failure = check_failure::untrusted;
    REQUIRE_EQ(status(failed),
               QStringLiteral("The update information could not be trusted, "
                              "so nothing was changed."));

    REQUIRE_EQ(
        status(state_of(check_state::disabled_by_policy)),
        QStringLiteral("Update checks are turned off by your administrator."));
    REQUIRE_EQ(status(state_of(check_state::no_keys)),
               QStringLiteral("This build cannot check for updates."));
}

TEST_CASE("update text: a developer build names the newest release")
{
    using updates::check_state;
    // After a check that found a release.
    updates::snapshot checked = state_of(check_state::current);
    checked.developer_build = true;
    checked.offer = offer_of("0.3.0", false);
    REQUIRE_EQ(status(checked),
               QStringLiteral(
                   "This is a developer build. The newest release is 0.3.0."));

    // A feed that named no release reads as `current` does.
    checked.offer.reset();
    REQUIRE_EQ(status(checked), QStringLiteral("0.2.0 is the newest version."));

    // Before any check, and while one runs, as for any build.
    updates::snapshot idle = state_of(check_state::idle);
    idle.developer_build = true;
    REQUIRE_EQ(status(idle), QStringLiteral("Not checked yet."));
    updates::snapshot checking = state_of(check_state::checking);
    checking.developer_build = true;
    REQUIRE_EQ(status(checking), QStringLiteral("Checking..."));
}

TEST_CASE("update text: This version with and without the kind of install")
{
    using upd::file_kind;
    REQUIRE_EQ(support::update_this_version_text(QStringLiteral("0.2.0"),
                                                 std::nullopt),
               QStringLiteral("0.2.0"));
    REQUIRE_EQ(
        support::update_this_version_text(QStringLiteral("0.2.0"),
                                          file_kind::windows_installer),
        QStringLiteral("0.2.0, installed for everyone on this computer"));
    REQUIRE_EQ(support::update_this_version_text(QStringLiteral("0.2.0-rc.1"),
                                                 file_kind::linux_archive),
               QStringLiteral("0.2.0-rc.1, unpacked from an archive"));
    REQUIRE_EQ(support::update_this_version_text(QStringLiteral("0.2.0"),
                                                 file_kind::macos_dmg),
               QStringLiteral("0.2.0, copied from the disk image"));
}

TEST_CASE("update text: Last checked is a local time in the user's locale, or "
          "Never")
{
    const english_locale locale;
    REQUIRE_EQ(support::update_last_checked_text(QDateTime()),
               QStringLiteral("Never"));

    const QDateTime when(QDate(2026, 7, 14), QTime(12, 30), QTimeZone::UTC);
    const QString text = support::update_last_checked_text(when);
    // The time is converted to local time and written the locale's way.
    REQUIRE_EQ(text,
               QLocale().toString(when.toLocalTime(), QLocale::ShortFormat));
    // The short format of English (Australia) writes the year with two digits.
    REQUIRE(text.contains(QStringLiteral("26")));
    REQUIRE(!text.contains(QLatin1Char('T')));
    REQUIRE(!text.contains(QLatin1Char('Z')));
}

TEST_CASE("update text: the banners, the toasts and the chip")
{
    REQUIRE_EQ(support::update_question_text(),
               QStringLiteral(
                   "Check for updates automatically? Once a day Studio would "
                   "ask virtual-serial-studio.com for the newest version. "
                   "Nothing about you or this computer is sent."));
    REQUIRE_EQ(support::update_notice_text(QStringLiteral("0.3.0")),
               QStringLiteral("Virtual Serial Studio 0.3.0 is available."));
    REQUIRE_EQ(support::update_answer_toast(true),
               QStringLiteral("Studio will check for updates once a day."));
    REQUIRE_EQ(support::update_answer_toast(false),
               QStringLiteral("Studio will not check for updates. You can "
                              "change this in Settings."));
    REQUIRE_EQ(support::update_chip_text(), QStringLiteral("Update available"));
    REQUIRE_EQ(support::update_chip_name(QStringLiteral("0.3.0-rc.1")),
               QStringLiteral("Update available: version 0.3.0-rc.1. Open the "
                              "update."));
}
