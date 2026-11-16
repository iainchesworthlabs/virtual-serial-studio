//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Updates tab of About and the Updates group of Settings, driven by the
// real update service with the fake transport of its own tests. No test
// makes a request.
//

#include "test_framework.hpp"

#include "updates/tests/fixture.hpp"

#include "backend/fake_studio_backend.hpp"
#include "support/about_dialog.hpp"
#include "support/build_info.hpp"
#include "support/settings.hpp"
#include "support/settings_dialog.hpp"
#include "support/update_text.hpp"

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>

using namespace vss::gui;
using namespace vss::gui::updates;
using namespace vss::gui::updates::testing;

namespace
{

QByteArray feed_for(const std::string& version, std::uint64_t serial = 5)
{
    return feed_body(serial, {make_release(version)});
}

template <class Widget> Widget* named(QWidget& root, const char* name)
{
    auto* widget = root.findChild<Widget*>(QString::fromLatin1(name));
    REQUIRE(widget != nullptr);
    return widget;
}

QStringList tab_titles(const QTabWidget& tabs)
{
    QStringList titles;
    for (int index = 0; index < tabs.count(); ++index)
        titles << tabs.tabText(index);
    return titles;
}

/// The version About shows as "this version".
QString build_version()
{
    return support::current_build_info().version;
}

/// An About dialog on the Updates tab, shown.
struct about_on_updates
{
    FakeStudioBackend backend;
    support::AboutDialog dialog;

    explicit about_on_updates(UpdateService& service)
        : dialog(backend, &service)
    {
        dialog.show_updates_tab();
        dialog.show();
        QApplication::processEvents();
    }

    QString text(const char* name)
    {
        return named<QLabel>(dialog, name)->text();
    }
    QPushButton* check() { return named<QPushButton>(dialog, "updatesCheck"); }
    QPushButton* update()
    {
        return named<QPushButton>(dialog, "updatesUpdate");
    }
    QCheckBox* automatic()
    {
        return named<QCheckBox>(dialog, "updatesAutomatic");
    }
    bool shown(QWidget* widget) { return widget->isVisibleTo(&dialog); }
};

/// What an update handler of assistive technology was told, politely.
QStringList polite_announcements;

void collect_announcements(QAccessibleEvent* event)
{
    if (event->type() != QAccessible::Announcement)
        return;
    const auto* announcement =
        static_cast<QAccessibleAnnouncementEvent*>(event);
    if (announcement->politeness() ==
        QAccessible::AnnouncementPoliteness::Polite)
        polite_announcements << announcement->message();
}

/// Collects the polite announcements for its lifetime.
class announcement_listener
{
public:
    announcement_listener()
        : previous_(QAccessible::installUpdateHandler(&collect_announcements))
    {
        polite_announcements.clear();
    }
    ~announcement_listener() { QAccessible::installUpdateHandler(previous_); }
    announcement_listener(const announcement_listener&) = delete;
    announcement_listener& operator=(const announcement_listener&) = delete;

private:
    QAccessible::UpdateHandler previous_;
};

} // namespace

// -- About, the tabs ---------------------------------------------------------

TEST_CASE("update screens: About without the service has no Updates tab")
{
    FakeStudioBackend backend;
    support::AboutDialog dialog(backend);
    // The five tabs and their order are as they were.
    REQUIRE_EQ(
        tab_titles(*dialog.tabs()),
        QStringList({QStringLiteral("About"),
                     QStringLiteral("Build information"),
                     QStringLiteral("Environment"), QStringLiteral("Licences"),
                     QStringLiteral("Fonts")}));
    REQUIRE(dialog.findChild<QPushButton*>(QStringLiteral("updatesCheck")) ==
            nullptr);
    // Asking for the tab is harmless.
    dialog.show_updates_tab();
    REQUIRE_EQ(dialog.tabs()->currentIndex(), 0);

    // A null service is the same as none.
    support::AboutDialog null_service(backend, nullptr, nullptr);
    REQUIRE_EQ(null_service.tabs()->count(), 5);
}

TEST_CASE("update screens: About with the service has Updates second")
{
    harness h;
    h.create();
    FakeStudioBackend backend;
    support::AboutDialog dialog(backend, h.service.get());
    REQUIRE_EQ(
        tab_titles(*dialog.tabs()),
        QStringList({QStringLiteral("About"), QStringLiteral("Updates"),
                     QStringLiteral("Build information"),
                     QStringLiteral("Environment"), QStringLiteral("Licences"),
                     QStringLiteral("Fonts")}));
    // It opens on About, and the call selects Updates.
    REQUIRE_EQ(dialog.tabs()->currentIndex(), 0);
    dialog.show_updates_tab();
    REQUIRE_EQ(dialog.tabs()->currentIndex(), 1);
    // Opening the tab makes no request.
    REQUIRE_EQ(h.transport.feed_requests, 0);
    REQUIRE_EQ(h.probe->calls, 0);
}

// -- About, each state -------------------------------------------------------

TEST_CASE("update screens: About before any check")
{
    harness h;
    h.create();
    about_on_updates about(*h.service);
    REQUIRE_EQ(about.text("updatesStatus"), QStringLiteral("Not checked yet."));
    // The kind of install is not known yet, so the version stands alone.
    REQUIRE_EQ(about.text("updatesVersion"), build_version());
    REQUIRE_EQ(about.text("updatesChecked"), QStringLiteral("Never"));
    REQUIRE(about.check()->isEnabled());
    REQUIRE(about.automatic()->isEnabled());
    REQUIRE(!about.automatic()->isChecked());
    REQUIRE(!about.shown(about.update()));
    REQUIRE_EQ(h.transport.feed_requests, 0);
}

TEST_CASE("update screens: Check now makes one request and follows it")
{
    harness h;
    h.create();
    about_on_updates about(*h.service);

    about.check()->click();
    REQUIRE_EQ(h.transport.feed_requests, 1);
    REQUIRE_EQ(about.text("updatesStatus"), QStringLiteral("Checking..."));
    // The button is off while the check runs, so a second press does
    // nothing.
    REQUIRE(!about.check()->isEnabled());
    about.check()->click();
    REQUIRE_EQ(h.transport.feed_requests, 1);

    h.transport.reply_feed(transport_error::none, feed_for("0.2.0"));
    REQUIRE_EQ(
        about.text("updatesStatus"),
        QStringLiteral("%1 is the newest version.").arg(build_version()));
    REQUIRE(about.check()->isEnabled());
    REQUIRE(!about.shown(about.update()));
    // The kind of install is known now.
    REQUIRE_EQ(
        about.text("updatesVersion"),
        support::update_this_version_text(
            build_version(), upd_kind(upd::file_kind::windows_installer)));
    REQUIRE(about.text("updatesVersion")
                .endsWith(QStringLiteral("installed for everyone on this "
                                         "computer")));
    // The time of the check, in the locale, not "Never".
    REQUIRE_EQ(about.text("updatesChecked"),
               support::update_last_checked_text(start_time()));
    REQUIRE(about.text("updatesChecked") != QStringLiteral("Never"));
}

TEST_CASE("update screens: About names the kinds of install")
{
    struct row
    {
        upd::file_kind kind = upd::file_kind::windows_installer;
        const char* text = "";
    };
    const row rows[] = {
        {upd::file_kind::windows_installer,
         "installed for everyone on this computer"},
        {upd::file_kind::linux_deb, "installed for everyone on this computer"},
        {upd::file_kind::windows_zip, "unpacked from an archive"},
        {upd::file_kind::linux_archive, "unpacked from an archive"},
        {upd::file_kind::macos_dmg, "copied from the disk image"}};
    for (const row& item : rows)
    {
        harness h;
        h.probe->answer = item.kind;
        h.create();
        about_on_updates about(*h.service);
        h.check_with(feed_for("0.2.0"));
        REQUIRE_EQ(about.text("updatesVersion"),
                   QStringLiteral("%1, %2").arg(
                       build_version(), QString::fromLatin1(item.text)));
    }
}

TEST_CASE("update screens: About when a version is available")
{
    harness h;
    h.create();
    about_on_updates about(*h.service);
    h.check_with(feed_for("0.3.0"));

    REQUIRE_EQ(about.text("updatesStatus"),
               QStringLiteral("0.3.0 is available."));
    REQUIRE(about.shown(about.update()));
    REQUIRE_EQ(about.update()->text(), QStringLiteral("Update..."));
    REQUIRE(about.update()->isEnabled());
    // It is the primary button of the tab.
    REQUIRE_EQ(about.update()->property("role").toString(),
               QStringLiteral("primary"));
    REQUIRE(about.check()->isEnabled());

    // "Update..." says so, and nothing else happens.
    int requests = 0;
    QObject::connect(&about.dialog, &support::AboutDialog::update_requested,
                     &about.dialog, [&requests] { ++requests; });
    about.update()->click();
    REQUIRE_EQ(requests, 1);
    REQUIRE_EQ(h.transport.feed_requests, 1);
    REQUIRE_EQ(h.transport.download_requests, 0);
}

TEST_CASE("update screens: About when a version has no file for this install")
{
    harness h;
    h.probe->answer = upd::file_kind::linux_deb;
    h.create();
    about_on_updates about(*h.service);
    h.check_with(feed_for("0.3.0"));

    REQUIRE_EQ(about.text("updatesStatus"),
               QStringLiteral("0.3.0 is available. Download it from "
                              "virtual-serial-studio.com."));
    // The update dialog then offers the downloads page, so the button stays.
    REQUIRE(about.shown(about.update()));
}

TEST_CASE("update screens: About after a check that failed")
{
    {
        harness h;
        h.create();
        about_on_updates about(*h.service);
        h.check_failing(transport_error::network);
        REQUIRE_EQ(about.text("updatesStatus"),
                   QStringLiteral("Studio could not reach the update server. "
                                  "Check the connection and try again."));
        REQUIRE(about.check()->isEnabled());
        REQUIRE(!about.shown(about.update()));
        // The time of the last accepted feed is not changed by a failure.
        REQUIRE_EQ(about.text("updatesChecked"), QStringLiteral("Never"));
    }
    {
        harness h;
        h.create();
        about_on_updates about(*h.service);
        h.check_with(badly_signed_body(5, {make_release("0.3.0")}));
        REQUIRE_EQ(about.text("updatesStatus"),
                   QStringLiteral("The update information could not be "
                                  "trusted, so nothing was changed."));
        REQUIRE(about.check()->isEnabled());
    }
}

TEST_CASE("update screens: About under a policy and in a build with no key")
{
    {
        harness h;
        h.settings.disabled_by_policy = true;
        h.create();
        about_on_updates about(*h.service);
        REQUIRE_EQ(about.text("updatesStatus"),
                   QStringLiteral(
                       "Update checks are turned off by your administrator."));
        REQUIRE(!about.check()->isEnabled());
        REQUIRE(!about.automatic()->isEnabled());
        REQUIRE(!about.shown(about.update()));
    }
    {
        harness h;
        h.settings.keys.clear();
        h.create();
        about_on_updates about(*h.service);
        REQUIRE_EQ(about.text("updatesStatus"),
                   QStringLiteral("This build cannot check for updates."));
        REQUIRE(!about.check()->isEnabled());
        REQUIRE(!about.automatic()->isEnabled());
        REQUIRE(!about.shown(about.update()));
        // A press of the disabled button makes no request.
        about.check()->click();
        REQUIRE_EQ(h.transport.feed_requests, 0);
    }
}

TEST_CASE("update screens: About in a developer build")
{
    harness h;
    h.settings.running_version = QStringLiteral("0.2.0-dev");
    h.create();
    about_on_updates about(*h.service);
    h.check_with(feed_for("0.3.0"));
    REQUIRE_EQ(about.text("updatesStatus"),
               QStringLiteral(
                   "This is a developer build. The newest release is 0.3.0."));
    // No install is offered.
    REQUIRE(!about.shown(about.update()));
    REQUIRE(about.check()->isEnabled());
}

// -- About, the check box, the focus and the announcements -------------------

TEST_CASE("update screens: the About check box is the service's setting")
{
    harness h;
    h.create();
    about_on_updates about(*h.service);
    REQUIRE(!about.automatic()->isChecked());

    // The box changes the setting.
    about.automatic()->click();
    REQUIRE(h.service->automatic());
    about.automatic()->click();
    REQUIRE(!h.service->automatic());

    // And the box follows the setting when something else changes it.
    h.service->set_automatic(true);
    REQUIRE(about.automatic()->isChecked());
    h.service->set_automatic(false);
    REQUIRE(!about.automatic()->isChecked());
    // The question of the first start is another way to change it.
    h.service->answer_question(true);
    REQUIRE(about.automatic()->isChecked());
}

TEST_CASE("update screens: Check now has the focus again when its check ends")
{
    harness h;
    h.create();
    about_on_updates about(*h.service);
    QPushButton* check = about.check();

    // It had the focus when it was pressed.
    check->setFocus(Qt::TabFocusReason);
    REQUIRE(about.dialog.focusWidget() == check);
    check->click();
    REQUIRE(!check->isEnabled());
    REQUIRE(about.dialog.focusWidget() != check);
    h.transport.reply_feed(transport_error::none, feed_for("0.2.0"));
    REQUIRE(check->isEnabled());
    REQUIRE(about.dialog.focusWidget() == check);

    // It did not have it: the focus is left where it is.
    about.automatic()->setFocus(Qt::TabFocusReason);
    check->click();
    h.transport.reply_feed(transport_error::none, feed_for("0.2.0", 6));
    REQUIRE(about.dialog.focusWidget() == about.automatic());
}

TEST_CASE("update screens: the status sentence is a polite live region")
{
    harness h;
    h.create();
    about_on_updates about(*h.service);
    QLabel* status = named<QLabel>(about.dialog, "updatesStatus");
    REQUIRE_EQ(status->accessibleName(), QStringLiteral("Not checked yet."));

    const announcement_listener listener;
    h.service->check_now();
    REQUIRE_EQ(status->accessibleName(), QStringLiteral("Checking..."));
    h.transport.reply_feed(transport_error::none, feed_for("0.3.0"));
    REQUIRE_EQ(status->accessibleName(), QStringLiteral("0.3.0 is available."));
    REQUIRE_EQ(polite_announcements,
               QStringList({QStringLiteral("Checking..."),
                            QStringLiteral("0.3.0 is available.")}));

    // A change of something else says nothing.
    polite_announcements.clear();
    h.service->set_automatic(true);
    REQUIRE(polite_announcements.isEmpty());
}

TEST_CASE("update screens: every control of the Updates tab has a name")
{
    harness h;
    h.create();
    about_on_updates about(*h.service);
    h.check_with(feed_for("0.3.0"));
    for (QPushButton* button : {about.check(), about.update()})
    {
        REQUIRE(!button->text().isEmpty());
        REQUIRE(button->focusPolicy() != Qt::NoFocus);
        REQUIRE(button->minimumSizeHint().height() >= 24);
    }
    REQUIRE_EQ(about.check()->text(), QStringLiteral("Check now"));
    REQUIRE_EQ(about.automatic()->text(),
               QStringLiteral("Check for updates automatically"));
    REQUIRE(about.automatic()->focusPolicy() != Qt::NoFocus);
    for (const char* name :
         {"updatesVersion", "updatesStatus", "updatesChecked"})
    {
        QLabel* label = named<QLabel>(about.dialog, name);
        REQUIRE_EQ(label->accessibleName(), label->text());
    }

    // The muted line under the card.
    bool found = false;
    for (QLabel* label : about.dialog.findChildren<QLabel*>())
        found = found ||
                label->text() ==
                    QStringLiteral(
                        "Once a day Studio asks virtual-serial-studio.com for "
                        "the newest version. Nothing about you or this "
                        "computer is sent.");
    REQUIRE(found);
}

// -- Settings ----------------------------------------------------------------

namespace
{

QCheckBox* settings_box(support::SettingsDialog& dialog, const char* name)
{
    return named<QCheckBox>(dialog, name);
}

} // namespace

TEST_CASE("update screens: Settings without the service has no Updates group")
{
    FakeStudioBackend backend;
    support::SettingsDialog dialog(backend, support::load_ui_settings());
    REQUIRE(dialog.findChild<QCheckBox*>(QStringLiteral("updateAutomatic")) ==
            nullptr);
    REQUIRE(dialog.findChild<QCheckBox*>(QStringLiteral("updatePrerelease")) ==
            nullptr);
    support::SettingsDialog null_service(backend, support::load_ui_settings(),
                                         nullptr, nullptr);
    REQUIRE(null_service.findChild<QCheckBox*>(
                QStringLiteral("updateAutomatic")) == nullptr);
}

TEST_CASE("update screens: Settings ends the General page with the group")
{
    harness h;
    h.create();
    FakeStudioBackend backend;
    support::SettingsDialog dialog(backend, support::load_ui_settings(),
                                   h.service.get());
    QCheckBox* automatic = settings_box(dialog, "updateAutomatic");
    QCheckBox* prerelease = settings_box(dialog, "updatePrerelease");
    REQUIRE_EQ(automatic->text(), QStringLiteral("Check for updates "
                                                 "automatically"));
    REQUIRE_EQ(prerelease->text(), QStringLiteral("Include pre-release "
                                                  "versions"));

    // They are on the General page, below its last group.
    QCheckBox* discard = settings_box(dialog, "askOnDiscard");
    QLayout* page = discard->parentWidget()->layout();
    REQUIRE(page != nullptr);
    REQUIRE(automatic->parentWidget() == discard->parentWidget());
    REQUIRE(page->indexOf(discard) < page->indexOf(automatic));
    REQUIRE(page->indexOf(automatic) < page->indexOf(prerelease));

    // The help lines of the spec.
    QStringList lines;
    for (QLabel* label : discard->parentWidget()->findChildren<QLabel*>())
        lines << label->text();
    REQUIRE(lines.contains(QStringLiteral("Updates")));
    REQUIRE(lines.contains(QStringLiteral(
        "Once a day Studio asks virtual-serial-studio.com for the newest "
        "version. Nothing about you or this computer is sent.")));
    REQUIRE(lines.contains(QStringLiteral(
        "Offers release candidates as well as finished releases.")));
}

TEST_CASE("update screens: the Settings check boxes apply at once and follow "
          "the service")
{
    harness h;
    h.create();
    FakeStudioBackend backend;
    support::SettingsDialog dialog(backend, support::load_ui_settings(),
                                   h.service.get());
    QCheckBox* automatic = settings_box(dialog, "updateAutomatic");
    QCheckBox* prerelease = settings_box(dialog, "updatePrerelease");
    REQUIRE(!automatic->isChecked());
    REQUIRE(!prerelease->isChecked());
    REQUIRE(automatic->isEnabled());
    REQUIRE(prerelease->isEnabled());

    automatic->click();
    REQUIRE(h.service->automatic());
    prerelease->click();
    REQUIRE(h.service->include_prerelease());
    automatic->click();
    prerelease->click();
    REQUIRE(!h.service->automatic());
    REQUIRE(!h.service->include_prerelease());

    // Another place changes a setting while the dialog is open.
    h.service->set_automatic(true);
    h.service->set_include_prerelease(true);
    REQUIRE(automatic->isChecked());
    REQUIRE(prerelease->isChecked());
    h.service->set_include_prerelease(false);
    REQUIRE(!prerelease->isChecked());

    // No line says the boxes are off, since they are on.
    REQUIRE(dialog.findChild<QLabel*>(QStringLiteral("updateNote")) != nullptr);
    REQUIRE(!named<QLabel>(dialog, "updateNote")->isVisibleTo(&dialog));
}

TEST_CASE("update screens: the Settings check boxes under a policy and with no "
          "key")
{
    {
        harness h;
        h.settings.disabled_by_policy = true;
        h.create();
        FakeStudioBackend backend;
        support::SettingsDialog dialog(backend, support::load_ui_settings(),
                                       h.service.get());
        REQUIRE(!settings_box(dialog, "updateAutomatic")->isEnabled());
        REQUIRE(!settings_box(dialog, "updatePrerelease")->isEnabled());
        QLabel* note = named<QLabel>(dialog, "updateNote");
        REQUIRE(note->isVisibleTo(&dialog));
        REQUIRE_EQ(note->text(), QStringLiteral("Set by your administrator."));
    }
    {
        harness h;
        h.settings.keys.clear();
        h.create();
        FakeStudioBackend backend;
        support::SettingsDialog dialog(backend, support::load_ui_settings(),
                                       h.service.get());
        REQUIRE(!settings_box(dialog, "updateAutomatic")->isEnabled());
        REQUIRE(!settings_box(dialog, "updatePrerelease")->isEnabled());
        QLabel* note = named<QLabel>(dialog, "updateNote");
        REQUIRE(note->isVisibleTo(&dialog));
        REQUIRE_EQ(note->text(),
                   QStringLiteral("This build cannot check for updates."));
    }
}

TEST_CASE("update screens: Reset to defaults leaves the update settings alone")
{
    harness h;
    h.create();
    h.service->answer_question(true);
    h.service->set_include_prerelease(true);
    REQUIRE(h.service->automatic());
    REQUIRE(!h.service->question_due());

    FakeStudioBackend backend;
    support::SettingsDialog dialog(backend, support::load_ui_settings(),
                                   h.service.get());
    dialog.reset_to_defaults();
    REQUIRE(h.service->automatic());
    REQUIRE(h.service->include_prerelease());
    REQUIRE(!h.service->question_due());
    REQUIRE(settings_box(dialog, "updateAutomatic")->isChecked());
    REQUIRE(settings_box(dialog, "updatePrerelease")->isChecked());
}
