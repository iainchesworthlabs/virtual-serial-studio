//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parts of the update screens that live in the main window: the banner,
// the status bar chip and the Help menu action. The service is the real one,
// with the fakes of its own tests, so no test makes a request.
//

#include "test_framework.hpp"

#include "updates/tests/fixture.hpp"

#include "backend/fake_studio_backend.hpp"
#include "shell/action_registry.hpp"
#include "shell/licence_notice.hpp"
#include "shell/shell_window.hpp"
#include "shell/status_bar.hpp"
#include "shell/update_notice.hpp"
#include "support/about_dialog.hpp"
#include "support/update_dialog.hpp"
#include "widgets/controls.hpp"

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

using namespace vss::gui;
using namespace vss::gui::updates;
using namespace vss::gui::updates::testing;

namespace
{

QByteArray feed_for(const std::string& version, std::uint64_t serial = 5)
{
    return feed_body(serial, {make_release(version)});
}

/// A check on the timer that finds `version`: the answer to the question is
/// yes, the window was shown a minute ago, and the feed names the version.
void find_by_timer(harness& h, const std::string& version)
{
    h.service->answer_question(true);
    h.service->start();
    h.set_time(60);
    REQUIRE(h.transport.feed_pending());
    h.transport.reply_feed(transport_error::none, feed_for(version));
}

/// True when the banner would not show if its window did.
bool hidden(const QWidget& widget)
{
    return !widget.isVisibleTo(widget.parentWidget());
}

QStringList toast_texts(QWidget& parent)
{
    QStringList texts;
    for (Toast* toast : parent.findChildren<Toast*>())
    {
        for (QLabel* label : toast->findChildren<QLabel*>())
            texts << label->text();
    }
    return texts;
}

QMenu* help_menu(ShellWindow& window)
{
    for (QAction* top : window.menuBar()->actions())
    {
        if (top->menu() != nullptr &&
            top->text().remove(QLatin1Char('&')) == QStringLiteral("Help"))
            return top->menu();
    }
    return nullptr;
}

/// The ids of a menu's actions, in order.
QStringList ids_of(const QMenu& menu)
{
    QStringList ids;
    for (QAction* action : menu.actions())
        ids << action->objectName();
    return ids;
}

void press_key(QWidget* widget, int key, const QString& text)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QApplication::sendEvent(widget, &release);
}

void press_mouse(QWidget* widget)
{
    const QPointF at(widget->width() / 2.0, widget->height() / 2.0);
    QMouseEvent press(QEvent::MouseButtonPress, at, widget->mapToGlobal(at),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, widget->mapToGlobal(at),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

/// What an About dialog looked like when it opened while `trigger` ran.
struct about_seen
{
    bool found = false;
    int tab = -1;
    QString tab_title;
    QString status;
};

/// Rejects the About dialog that opens while `trigger` runs, after reading
/// which tab is on show.
template <class Trigger> about_seen about_opened_by(Trigger&& trigger)
{
    about_seen seen;
    QTimer::singleShot(
        0, QApplication::instance(),
        [&seen]
        {
            for (QWidget* widget : QApplication::topLevelWidgets())
            {
                auto* dialog = qobject_cast<support::AboutDialog*>(widget);
                if (dialog == nullptr)
                    continue;
                seen.found = true;
                seen.tab = dialog->tabs()->currentIndex();
                seen.tab_title = dialog->tabs()->tabText(seen.tab);
                if (auto* status = dialog->findChild<QLabel*>(
                        QStringLiteral("updatesStatus")))
                    seen.status = status->text();
                dialog->reject();
            }
        });
    trigger();
    return seen;
}

/// Counts the name changes of one widget, which is how a banner is announced.
QObject* watched_object = nullptr;
int name_change_count = 0;

void count_name_changes(QAccessibleEvent* event)
{
    if (event->type() == QAccessible::NameChanged &&
        event->object() == watched_object)
        ++name_change_count;
}

class name_listener
{
public:
    explicit name_listener(QObject* object)
        : previous_(QAccessible::installUpdateHandler(&count_name_changes))
    {
        watched_object = object;
        name_change_count = 0;
    }
    ~name_listener()
    {
        watched_object = nullptr;
        QAccessible::installUpdateHandler(previous_);
    }
    name_listener(const name_listener&) = delete;
    name_listener& operator=(const name_listener&) = delete;

private:
    QAccessible::UpdateHandler previous_;
};

} // namespace

// -- the banner: the question ------------------------------------------------

TEST_CASE("update notice: the question shows first, and is no dialog")
{
    harness h;
    h.create();
    QWidget host;
    UpdateNotice notice(*h.service, &host);

    REQUIRE(notice.shown() == UpdateNotice::content::question);
    REQUIRE(!hidden(notice));
    REQUIRE(qobject_cast<QDialog*>(&notice) == nullptr);
    REQUIRE(!notice.isWindow());
    REQUIRE(notice.get_kind() == NoteBanner::kind::info);
    REQUIRE_EQ(notice.objectName(), QStringLiteral("updateNotice"));
    REQUIRE_EQ(notice.second_button()->objectName(),
               QStringLiteral("updateNoticeSecond"));
    REQUIRE_EQ(notice.text(),
               QStringLiteral(
                   "Check for updates automatically? Once a day Studio would "
                   "ask virtual-serial-studio.com for the newest version. "
                   "Nothing about you or this computer is sent."));
    REQUIRE_EQ(notice.action_button()->text(), QStringLiteral("Turn on"));
    REQUIRE_EQ(notice.second_button()->text(), QStringLiteral("No thanks"));
    REQUIRE_EQ(notice.second_button()->accessibleName(),
               QStringLiteral("No thanks"));
    REQUIRE_EQ(notice.accessibleName(), notice.text());
    // It takes no focus.
    REQUIRE(notice.focusPolicy() == Qt::NoFocus);
    // Asking makes no request.
    REQUIRE_EQ(h.transport.feed_requests, 0);
}

TEST_CASE("update notice: each answer is kept and the banner goes")
{
    for (const bool turn_on : {true, false})
    {
        harness h;
        h.create();
        QWidget host;
        UpdateNotice notice(*h.service, &host);
        int answers = 0;
        bool answered_on = !turn_on;
        QObject::connect(&notice, &UpdateNotice::answered, &notice,
                         [&](bool on)
                         {
                             ++answers;
                             answered_on = on;
                         });

        (turn_on ? notice.action_button() : notice.second_button())->click();
        REQUIRE_EQ(answers, 1);
        REQUIRE_EQ(answered_on, turn_on);
        REQUIRE(notice.shown() == UpdateNotice::content::none);
        REQUIRE(hidden(notice));
        REQUIRE_EQ(h.service->automatic(), turn_on);
        REQUIRE(!h.service->question_due());
        REQUIRE(QSettings().value(QString::fromLatin1(keys::kAsked)).toBool());

        // It does not come back, not after a restart either.
        h.recreate();
        QWidget next_host;
        UpdateNotice next(*h.service, &next_host);
        REQUIRE(next.shown() == UpdateNotice::content::none);
        REQUIRE_EQ(h.service->automatic(), turn_on);
    }
}

TEST_CASE("update notice: no question under a policy, with no key or in a "
          "developer build")
{
    {
        harness h;
        h.settings.disabled_by_policy = true;
        h.create();
        QWidget host;
        UpdateNotice notice(*h.service, &host);
        REQUIRE(notice.shown() == UpdateNotice::content::none);
        REQUIRE(hidden(notice));
    }
    {
        harness h;
        h.settings.keys.clear();
        h.create();
        QWidget host;
        UpdateNotice notice(*h.service, &host);
        REQUIRE(hidden(notice));
    }
    {
        harness h;
        h.settings.running_version = QStringLiteral("0.2.0-dev");
        h.create();
        QWidget host;
        UpdateNotice notice(*h.service, &host);
        REQUIRE(hidden(notice));
        // Not after a check either: the developer build is told, not asked.
        h.check_with(feed_for("0.3.0"));
        REQUIRE(hidden(notice));
    }
}

TEST_CASE("update notice: its text is set, and so announced, only when it "
          "changes")
{
    harness h;
    h.create();
    QWidget host;
    UpdateNotice notice(*h.service, &host);
    const name_listener listener(&notice);

    // Anything else that changes in the service reads the banner again, and
    // says nothing.
    notice.refresh();
    h.service->set_include_prerelease(true);
    h.service->set_automatic(true);
    REQUIRE(notice.shown() == UpdateNotice::content::question);
    REQUIRE_EQ(name_change_count, 0);

    // A new text is announced once.
    h.service->answer_question(true);
    REQUIRE(hidden(notice));
    h.service->start();
    h.set_time(60);
    h.transport.reply_feed(transport_error::none, feed_for("0.3.0"));
    // Setting a banner's text announces the change, which is more than one
    // event of the widget toolkit: what counts is that it happens once.
    REQUIRE(name_change_count > 0);
    const int announced = name_change_count;
    notice.refresh();
    h.service->set_include_prerelease(false);
    REQUIRE_EQ(name_change_count, announced);
}

// -- the banner: the update notice -------------------------------------------

TEST_CASE("update notice: a timer check that finds a version shows it")
{
    harness h;
    h.create();
    QWidget host;
    UpdateNotice notice(*h.service, &host);
    h.service->answer_question(true);
    REQUIRE(hidden(notice));

    h.service->start();
    h.set_time(60);
    h.transport.reply_feed(transport_error::none, feed_for("0.3.0"));
    REQUIRE(notice.shown() == UpdateNotice::content::update);
    REQUIRE(!hidden(notice));
    REQUIRE(notice.get_kind() == NoteBanner::kind::info);
    REQUIRE_EQ(notice.text(),
               QStringLiteral("Virtual Serial Studio 0.3.0 is available."));
    REQUIRE_EQ(notice.action_button()->text(), QStringLiteral("Details"));
    REQUIRE_EQ(notice.second_button()->text(), QStringLiteral("Later"));
    REQUIRE_EQ(notice.accessibleName(), notice.text());

    // "Details" says so, and keeps the banner.
    int details = 0;
    QObject::connect(&notice, &UpdateNotice::details_requested, &notice,
                     [&details] { ++details; });
    notice.action_button()->click();
    REQUIRE_EQ(details, 1);
    REQUIRE(!hidden(notice));

    // "Later" hides it, and the chip stays.
    notice.second_button()->click();
    REQUIRE(hidden(notice));
    REQUIRE(notice.shown() == UpdateNotice::content::none);
    REQUIRE(h.service->chip_due());
    REQUIRE(!h.service->notice_due());
}

TEST_CASE("update notice: a check by hand adds no banner")
{
    harness h;
    h.create();
    h.service->answer_question(false);
    QWidget host;
    UpdateNotice notice(*h.service, &host);
    h.check_with(feed_for("0.3.0"));
    REQUIRE(h.service->chip_due());
    REQUIRE(hidden(notice));
}

TEST_CASE("update notice: the question is asked before an update is told of")
{
    // The answer to the question is not given, and the automatic check is on
    // by another way: the question stays until it is answered.
    harness h;
    h.create();
    h.service->set_automatic(true);
    QWidget host;
    UpdateNotice notice(*h.service, &host);
    h.service->start();
    h.set_time(60);
    h.transport.reply_feed(transport_error::none, feed_for("0.3.0"));
    REQUIRE(h.service->notice_due());
    REQUIRE(notice.shown() == UpdateNotice::content::question);
    notice.second_button()->click();
    REQUIRE(notice.shown() == UpdateNotice::content::update);
}

// -- the chip ----------------------------------------------------------------

TEST_CASE("update chip: hidden at first, shown with its name when an update "
          "is found, and hidden when the offer goes")
{
    harness h;
    h.create();
    FakeStudioBackend backend;
    StatusBar bar(backend);
    bar.resize(1000, 32);
    bar.show();
    bar.set_update_service(h.service.get());
    QApplication::processEvents();

    ChipButton* chip = bar.update_chip();
    REQUIRE(chip != nullptr);
    REQUIRE(!chip->isVisibleTo(&bar));

    h.check_with(feed_for("0.3.0"));
    QApplication::processEvents();
    REQUIRE(chip->isVisibleTo(&bar));
    REQUIRE_EQ(chip->chip()->text(), QStringLiteral("Update available"));
    REQUIRE(chip->chip()->get_kind() == StatusChip::kind::info);
    REQUIRE_EQ(
        chip->accessibleName(),
        QStringLiteral("Update available: version 0.3.0. Open the update."));
    REQUIRE(chip->focusPolicy() != Qt::NoFocus);
    REQUIRE(chip->height() >= 24);
    // Left of the edition chip.
    REQUIRE(chip->geometry().right() < bar.edition_chip()->geometry().left());

    // A newer version changes the name.
    h.check_with(feed_for("0.4.0", 6));
    REQUIRE_EQ(chip->accessibleName(),
               QStringLiteral("Update available: version 0.4.0. Open the "
                              "update."));

    // The offer goes when a check finds nothing newer.
    h.check_with(feed_for("0.2.0", 7));
    REQUIRE(!chip->isVisibleTo(&bar));
}

TEST_CASE("update chip: pressed with the mouse, Space and Enter it asks for "
          "the update")
{
    harness h;
    h.create();
    FakeStudioBackend backend;
    StatusBar bar(backend);
    bar.resize(1000, 32);
    bar.show();
    bar.set_update_service(h.service.get());
    h.check_with(feed_for("0.3.0"));
    QApplication::processEvents();

    int requests = 0;
    QObject::connect(&bar, &StatusBar::update_requested, &bar,
                     [&requests] { ++requests; });
    ChipButton* chip = bar.update_chip();
    REQUIRE(chip->isVisibleTo(&bar));
    press_mouse(chip);
    REQUIRE_EQ(requests, 1);
    press_key(chip, Qt::Key_Space, QStringLiteral(" "));
    REQUIRE_EQ(requests, 2);
    press_key(chip, Qt::Key_Return, QStringLiteral("\r"));
    REQUIRE_EQ(requests, 3);
    chip->click();
    REQUIRE_EQ(requests, 4);
}

TEST_CASE("update chip: stays through a download, goes when the version is "
          "skipped on a timer check")
{
    {
        harness h;
        h.create();
        FakeStudioBackend backend;
        StatusBar bar(backend);
        bar.set_update_service(h.service.get());
        bar.show();
        h.check_with(feed_for("0.3.0"));
        h.service->start_download();
        REQUIRE_EQ(h.state().download, download_state::downloading);
        REQUIRE(bar.update_chip()->isVisibleTo(&bar));
    }
    {
        harness h;
        h.create();
        FakeStudioBackend backend;
        StatusBar bar(backend);
        bar.set_update_service(h.service.get());
        bar.show();
        find_by_timer(h, "0.3.0");
        REQUIRE(bar.update_chip()->isVisibleTo(&bar));
        h.service->skip_offered_version();
        REQUIRE(!bar.update_chip()->isVisibleTo(&bar));
    }
}

TEST_CASE("update chip: a bar with no service never shows it")
{
    FakeStudioBackend backend;
    StatusBar bar(backend);
    bar.show();
    QApplication::processEvents();
    REQUIRE(bar.update_chip() != nullptr);
    REQUIRE(!bar.update_chip()->isVisibleTo(&bar));
    // The edition chip is as it was.
    REQUIRE(bar.edition_chip()->isVisibleTo(&bar));
    REQUIRE(bar.edition_chip()->accessibleName().startsWith(
        QStringLiteral("Edition: ")));
}

// -- the window --------------------------------------------------------------

TEST_CASE("update window: with no service there is no update screen")
{
    QSettings().clear();
    FakeStudioBackend backend;
    ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    REQUIRE(window.update_notice() == nullptr);
    auto* action = window.findChild<QAction*>(QStringLiteral("help.updates"));
    REQUIRE(action != nullptr);
    REQUIRE(!action->isEnabled());
    auto* status = window.findChild<StatusBar*>();
    REQUIRE(status != nullptr);
    REQUIRE(!status->update_chip()->isVisibleTo(status));

    // Without a service the action is registered, and off.
    REQUIRE(action->statusTip().isEmpty());
    QSettings().clear();
}

TEST_CASE("update window: Check for updates is in Help, after Support and "
          "before About")
{
    QSettings().clear();
    harness h;
    h.create();
    FakeStudioBackend backend;
    ShellWindow window(backend);
    window.set_update_service(h.service.get());
    window.show();
    QApplication::processEvents();

    QMenu* help = help_menu(window);
    REQUIRE(help != nullptr);
    const QStringList ids = ids_of(*help);
    const qsizetype support = ids.indexOf(QStringLiteral("help.support"));
    REQUIRE(support >= 0);
    REQUIRE_EQ(ids.at(support + 1), QStringLiteral("help.updates"));
    REQUIRE_EQ(ids.at(support + 2), QStringLiteral("help.about"));

    auto* action = window.findChild<QAction*>(QStringLiteral("help.updates"));
    REQUIRE(action != nullptr);
    REQUIRE_EQ(action->text(), QStringLiteral("Check for updates..."));
    REQUIRE(action->shortcut().isEmpty());
    REQUIRE(action->isEnabled());

    // The registry has it in the same place, and Search actions finds it.
    auto* registry = window.findChild<ActionRegistry*>();
    REQUIRE(registry != nullptr);
    QStringList registered;
    for (QAction* item : registry->actions())
        registered << item->objectName();
    const qsizetype in_registry =
        registered.indexOf(QStringLiteral("help.support"));
    REQUIRE_EQ(registered.at(in_registry + 1), QStringLiteral("help.updates"));
    REQUIRE_EQ(registered.at(in_registry + 2), QStringLiteral("help.about"));
    for (const char* query : {"Check for updates",
                              "update version new release download", "release"})
    {
        bool found = false;
        for (QAction* match : registry->search(QString::fromLatin1(query)))
            found = found ||
                    match->data().toString() == QStringLiteral("help.updates");
        REQUIRE(found);
    }
    REQUIRE(registry->shortcuts_unique());
    QSettings().clear();
}

TEST_CASE("update window: the action is off under a policy and with no key, "
          "and says why")
{
    QSettings().clear();
    {
        harness h;
        h.settings.disabled_by_policy = true;
        h.create();
        FakeStudioBackend backend;
        ShellWindow window(backend);
        window.set_update_service(h.service.get());
        auto* action =
            window.findChild<QAction*>(QStringLiteral("help.updates"));
        REQUIRE(action != nullptr);
        REQUIRE(!action->isEnabled());
        REQUIRE_EQ(action->toolTip(),
                   QStringLiteral(
                       "Update checks are turned off by your administrator."));
        REQUIRE_EQ(action->statusTip(), action->toolTip());
        // No banner either.
        REQUIRE(hidden(*window.update_notice()));
    }
    {
        harness h;
        h.settings.keys.clear();
        h.create();
        FakeStudioBackend backend;
        ShellWindow window(backend);
        window.set_update_service(h.service.get());
        auto* action =
            window.findChild<QAction*>(QStringLiteral("help.updates"));
        REQUIRE(!action->isEnabled());
        REQUIRE_EQ(action->toolTip(),
                   QStringLiteral("This build cannot check for updates."));
        REQUIRE(hidden(*window.update_notice()));
    }
    QSettings().clear();
}

TEST_CASE("update window: the action opens About on the Updates tab and "
          "makes one request")
{
    QSettings().clear();
    harness h;
    h.create();
    FakeStudioBackend backend;
    ShellWindow window(backend);
    window.set_update_service(h.service.get());
    window.show();
    QApplication::processEvents();

    auto* action = window.findChild<QAction*>(QStringLiteral("help.updates"));
    REQUIRE(action != nullptr);
    const about_seen seen = about_opened_by([action] { action->trigger(); });
    REQUIRE(seen.found);
    REQUIRE_EQ(seen.tab, 1);
    REQUIRE_EQ(seen.tab_title, QStringLiteral("Updates"));
    REQUIRE_EQ(seen.status, QStringLiteral("Checking..."));
    REQUIRE_EQ(h.transport.feed_requests, 1);
    REQUIRE_EQ(h.state().state, check_state::checking);

    // About from the menu does not check, and opens on its first tab.
    auto* about = window.findChild<QAction*>(QStringLiteral("help.about"));
    h.transport.reply_feed(transport_error::none, feed_for("0.2.0"));
    const about_seen plain = about_opened_by([about] { about->trigger(); });
    REQUIRE(plain.found);
    REQUIRE_EQ(plain.tab, 0);
    REQUIRE_EQ(h.transport.feed_requests, 1);
    QSettings().clear();
}

TEST_CASE("update window: the banner sits below the licence notice, takes no "
          "focus and its answers give a toast")
{
    QSettings().clear();
    harness h;
    h.create();
    FakeStudioBackend backend;
    ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();
    QWidget* focused = window.focusWidget();
    window.set_update_service(h.service.get());
    QApplication::processEvents();

    UpdateNotice* notice = window.update_notice();
    REQUIRE(notice != nullptr);
    REQUIRE(notice->shown() == UpdateNotice::content::question);
    REQUIRE(notice->isVisible());
    REQUIRE(window.focusWidget() == focused);
    QLayout* column = window.centralWidget()->layout();
    REQUIRE_EQ(column->indexOf(notice),
               column->indexOf(window.licence_notice()) + 1);

    // "Turn on" shows its toast.
    notice->action_button()->click();
    REQUIRE(h.service->automatic());
    REQUIRE(toast_texts(window).contains(
        QStringLiteral("Studio will check for updates once a day.")));
    QSettings().clear();
}

TEST_CASE("update window: No thanks gives its toast")
{
    QSettings().clear();
    harness h;
    h.create();
    FakeStudioBackend backend;
    ShellWindow window(backend);
    window.set_update_service(h.service.get());
    window.show();
    QApplication::processEvents();

    window.update_notice()->second_button()->click();
    REQUIRE(!h.service->automatic());
    REQUIRE(toast_texts(window).contains(
        QStringLiteral("Studio will not check for updates. You can change "
                       "this in Settings.")));
    REQUIRE(hidden(*window.update_notice()));
    QSettings().clear();
}

TEST_CASE("update window: the chip, Details and Update... in About ask for "
          "the update dialog")
{
    QSettings().clear();
    harness h;
    h.create();
    FakeStudioBackend backend;
    ShellWindow window(backend);
    window.set_update_service(h.service.get());
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    int requests = 0;
    bool about_was_open = false;
    QObject::connect(&window, &ShellWindow::update_dialog_requested, &window,
                     [&]
                     {
                         ++requests;
                         for (QWidget* widget : QApplication::topLevelWidgets())
                         {
                             if (qobject_cast<support::AboutDialog*>(widget) !=
                                     nullptr &&
                                 widget->isVisible())
                                 about_was_open = true;
                         }
                     });

    // Nothing is asked for before an update is found.
    auto* status = window.findChild<StatusBar*>();
    REQUIRE(!status->update_chip()->isVisibleTo(status));
    REQUIRE_EQ(requests, 0);

    find_by_timer(h, "0.3.0");
    QApplication::processEvents();
    REQUIRE(status->update_chip()->isVisibleTo(status));
    REQUIRE(window.update_notice()->isVisible());
    REQUIRE_EQ(window.update_notice()->text(),
               QStringLiteral("Virtual Serial Studio 0.3.0 is available."));
    // The window did not move the focus to show them.
    status->update_chip()->click();
    REQUIRE_EQ(requests, 1);
    window.update_notice()->action_button()->click();
    REQUIRE_EQ(requests, 2);
    REQUIRE(!about_was_open);

    // "Update..." in About closes About, then asks.
    auto* action = window.findChild<QAction*>(QStringLiteral("help.updates"));
    QTimer::singleShot(
        0, QApplication::instance(),
        [&h]
        {
            // The check of the action ends with the version still on offer.
            h.transport.reply_feed(transport_error::none, feed_for("0.3.0", 6));
            for (QWidget* widget : QApplication::topLevelWidgets())
            {
                if (auto* dialog = qobject_cast<support::AboutDialog*>(widget))
                {
                    auto* button = dialog->findChild<QPushButton*>(
                        QStringLiteral("updatesUpdate"));
                    if (button != nullptr && button->isVisibleTo(dialog))
                        button->click();
                    else
                        dialog->reject();
                }
            }
        });
    action->trigger();
    REQUIRE_EQ(requests, 3);
    REQUIRE(!about_was_open);
    QSettings().clear();
}

TEST_CASE("update shell: the window opens the update dialog for an update "
          "on offer, and for nothing else")
{
    QSettings().clear();
    harness h;
    h.create();
    FakeStudioBackend backend;
    ShellWindow window(backend);
    window.set_update_service(h.service.get());
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    // What the program connects.
    QObject::connect(&window, &ShellWindow::update_dialog_requested, &window,
                     &ShellWindow::show_update_dialog);
    int dialogs = 0;
    QString title;
    const auto close_dialog = [&dialogs, &title]
    {
        for (QWidget* widget : QApplication::topLevelWidgets())
        {
            if (auto* dialog = qobject_cast<support::UpdateDialog*>(widget))
            {
                ++dialogs;
                title = dialog->windowTitle();
                static_cast<QDialog*>(dialog)->reject();
            }
        }
    };

    // Nothing is on offer: no dialog opens, and the call returns.
    QTimer::singleShot(0, QApplication::instance(), close_dialog);
    window.show_update_dialog();
    QApplication::processEvents();
    REQUIRE_EQ(dialogs, 0);

    find_by_timer(h, "0.3.0");
    QApplication::processEvents();
    QTimer::singleShot(0, QApplication::instance(), close_dialog);
    window.findChild<StatusBar*>()->update_chip()->click();
    REQUIRE_EQ(dialogs, 1);
    REQUIRE_EQ(title, QStringLiteral("Update Virtual Serial Studio"));
    // Closing the dialog changed nothing: the update is still on offer.
    REQUIRE(h.service->chip_due());
    QSettings().clear();
}
