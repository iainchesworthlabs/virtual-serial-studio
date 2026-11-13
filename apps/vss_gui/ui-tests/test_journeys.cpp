//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Journey tests: each one is a task from docs/gui-design/docs/screens/08
// (Simple mode) or screens/01 (Advanced mode), done
// the way a person does it, with key presses and mouse clicks sent to the real
// main window and its dialogs through QTest. The backend is the in-memory
// fake, so the results are the same on every platform.
//
// The dialogs run their own event loop (exec()), which blocks the test body.
// A step is therefore armed first and runs from a timer once the dialog it is
// waiting for is on screen.
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "flows/add_port_dialog.hpp"
#include "flows/port_ready_dialog.hpp"
#include "flows/watch_dialog.hpp"
#include "shell/command_palette.hpp"
#include "shell/launcher.hpp"
#include "shell/shell_window.hpp"
#include "simple/job_form.hpp"
#include "simple/job_grid.hpp"
#include "simple/setup_page.hpp"
#include "simple/simple_page.hpp"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QString>
#include <QSysInfo>
#include <QTest>
#include <QTimer>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace
{

using vss::gui::FakeStudioBackend;
using vss::gui::ShellWindow;

void clear_settings()
{
    QSettings settings;
    settings.clear();
}

/// A step waiting for a dialog. It runs once `ready` accepts the active modal
/// widget. If that never happens the step closes whatever is open, so a missing
/// dialog fails the test instead of hanging it.
class Waiting final
{
public:
    using Ready = std::function<bool(QWidget*)>;
    using Step = std::function<void(QWidget*)>;

    static constexpr int kPollMilliseconds = 10;
    static constexpr int kGiveUpMilliseconds = 8000;

    Waiting(Ready ready, Step step)
        : ready_(std::move(ready)), step_(std::move(step)),
          timer_(new QTimer(QApplication::instance()))
    {
        timer_->setInterval(kPollMilliseconds);
        QObject::connect(timer_, &QTimer::timeout, timer_, [this] { poll(); });
        timer_->start();
    }

    Waiting(const Waiting&) = delete;
    Waiting& operator=(const Waiting&) = delete;

    ~Waiting() { delete timer_; }

    /// True once the step has run. A failed requirement inside the step is
    /// thrown here, on the test's own stack, because an exception must not
    /// leave a timer slot.
    bool ran() const
    {
        if (!error_.empty())
            throw vss_test::failure(error_);
        return ran_;
    }

private:
    void poll()
    {
        elapsed_ += kPollMilliseconds;
        QWidget* modal = QApplication::activeModalWidget();
        if (modal != nullptr && ready_(modal))
        {
            timer_->stop();
            ran_ = true;
            try
            {
                step_(modal);
            }
            catch (const std::exception& error)
            {
                error_ = error.what();
                modal->close();
            }
            return;
        }
        if (elapsed_ >= kGiveUpMilliseconds)
        {
            timer_->stop();
            if (modal != nullptr)
                modal->close();
        }
    }

    Ready ready_;
    Step step_;
    QTimer* timer_;
    int elapsed_ = 0;
    bool ran_ = false;
    std::string error_;
};

template <class Dialog> bool is_a(QWidget* widget)
{
    return qobject_cast<Dialog*>(widget) != nullptr;
}

QPushButton* button(QWidget* parent, const QString& text)
{
    for (auto* candidate : parent->findChildren<QPushButton*>())
    {
        if (candidate->text() == text)
            return candidate;
    }
    return nullptr;
}

QAction* named_action(ShellWindow& window, const char* name)
{
    return window.findChild<QAction*>(QString::fromLatin1(name));
}

vss::gui::LauncherCard* launcher_card(ShellWindow& window, const QString& title)
{
    for (auto* card : window.findChildren<vss::gui::LauncherCard*>())
    {
        if (card->accessibleName() == title)
            return card;
    }
    return nullptr;
}

/// A shown window as a first start looks: Simple mode with the jobs up, or
/// Advanced mode with the launcher.
struct Session
{
    explicit Session(bool advanced = false)
    {
        clear_settings();
        // Closing the window must not stop on the unsaved-changes prompt.
        QSettings().setValue(QStringLiteral("ui/askOnDiscard"), false);
        QSettings().setValue(QStringLiteral("ui/advanced"), advanced);
        window = std::make_unique<ShellWindow>(backend);
        window->resize(1280, 800);
        window->show();
        QApplication::processEvents();
    }

    ~Session()
    {
        window->close();
        QApplication::processEvents();
    }

    vss::gui::simple::SimplePage* simple() const
    {
        return window->findChild<vss::gui::simple::SimplePage*>();
    }

    FakeStudioBackend backend;
    std::unique_ptr<ShellWindow> window;
};

vss::gui::simple::JobTile* job_tile(ShellWindow& window, const QString& title)
{
    for (auto* tile : window.findChildren<vss::gui::simple::JobTile*>())
    {
        if (tile->accessibleName() == title && tile->isVisible())
            return tile;
    }
    return nullptr;
}

} // namespace

TEST_CASE("journey: give my program a serial port, with the keyboard")
{
    Session session;
    auto& window = *session.window;

    auto* tile =
        job_tile(window, QStringLiteral("Give my program a serial port"));
    REQUIRE(tile != nullptr);
    tile->setFocus();
    QTest::keyClick(tile, Qt::Key_Return);
    QApplication::processEvents();

    auto* form = session.simple()->form();
    REQUIRE(form != nullptr);
    auto* label = form->findChild<QLineEdit*>(QStringLiteral("jobFormLabel"));
    REQUIRE(label != nullptr);
    label->setFocus();
    QTest::keyClicks(label, QStringLiteral("PuTTY"));
    QTest::mouseClick(form->create_button(), Qt::LeftButton);
    {
        const QPointer<vss::gui::simple::JobForm> building(form);
        REQUIRE(vss_test::wait_until(
            [&building] { return building.isNull() || !building->busy(); }));
    }
    QApplication::processEvents();

    REQUIRE_EQ(session.backend.ports().size(), 1);
    REQUIRE_EQ(session.backend.ports().front().name, QStringLiteral("PuTTY"));
    auto* page = session.simple()->setup_page();
    REQUIRE(page != nullptr);
    REQUIRE(page->isVisible());
    REQUIRE(page->steps_text().contains(QStringLiteral("In PuTTY, open")));
    REQUIRE_EQ(session.simple()->list()->count(), 1);
}

TEST_CASE("journey: connect two programs")
{
    Session session;
    auto& window = *session.window;

    auto* tile =
        job_tile(window, QStringLiteral("Connect two programs to each other"));
    REQUIRE(tile != nullptr);
    QTest::mouseClick(tile, Qt::LeftButton);
    QApplication::processEvents();

    auto* form = session.simple()->form();
    REQUIRE(form != nullptr);
    REQUIRE(form->create_button()->isEnabled());
    QTest::mouseClick(form->create_button(), Qt::LeftButton);
    {
        const QPointer<vss::gui::simple::JobForm> building(form);
        REQUIRE(vss_test::wait_until(
            [&building] { return building.isNull() || !building->busy(); }));
    }
    QApplication::processEvents();

    REQUIRE_EQ(session.backend.ports().size(), 2);
    REQUIRE_EQ(session.backend.links().size(), 1);
    auto* status = window.findChild<QStatusBar*>();
    REQUIRE(status != nullptr);
    REQUIRE(status->currentMessage().startsWith(QStringLiteral("Done.")));
}

TEST_CASE("journey: see the data on a port")
{
    Session session;
    auto& window = *session.window;
    const auto port =
        session.backend.create_auto_port(QStringLiteral("program"), true);
    REQUIRE(port.has_value());
    QApplication::processEvents();

    auto* tile = job_tile(window, QStringLiteral("See the data on a port"));
    REQUIRE(tile != nullptr);
    QTest::mouseClick(tile, Qt::LeftButton);
    QApplication::processEvents();

    auto* page = session.simple()->setup_page();
    REQUIRE(page != nullptr);
    REQUIRE(page->prefs().show_data);
    auto* data = page->findChild<QWidget*>(QStringLiteral("setupData"));
    REQUIRE(data != nullptr);
    REQUIRE(data->isVisible());
}

TEST_CASE("journey: watch traffic on a port, in Advanced mode")
{
    Session session(true);
    auto& window = *session.window;

    const auto port = session.backend.create_virtual_port(
        QStringLiteral("device"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    QApplication::processEvents();

    auto* watch_action = named_action(window, "topology.watch");
    REQUIRE(watch_action != nullptr);

    Waiting start(
        [](QWidget* widget) { return is_a<vss::gui::WatchDialog>(widget); },
        [](QWidget* dialog)
        {
            auto* primary = button(dialog, QStringLiteral("Start watching"));
            REQUIRE(primary != nullptr);
            REQUIRE(primary->isEnabled());
            QTest::mouseClick(primary, Qt::LeftButton);
        });

    watch_action->trigger();
    QApplication::processEvents();

    REQUIRE(start.ran());
    REQUIRE_EQ(session.backend.ports().size(), 2);
    REQUIRE_EQ(session.backend.links().size(), 1);
    auto* monitor =
        window.findChild<QDockWidget*>(QStringLiteral("monitorDock"));
    REQUIRE(monitor != nullptr);
    REQUIRE(monitor->isVisible());
}

TEST_CASE("journey: run an action from the command palette")
{
    Session session;
    auto& window = *session.window;

    const auto port = session.backend.create_virtual_port(
        QStringLiteral("device"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    QApplication::processEvents();

    auto* advanced = named_action(window, "view.advanced");
    REQUIRE(advanced != nullptr);
    REQUIRE(!advanced->isChecked());

    Waiting choose([](QWidget* widget)
                   { return is_a<vss::gui::CommandPalette>(widget); },
                   [](QWidget* dialog)
                   {
                       auto* search = dialog->findChild<QLineEdit*>();
                       REQUIRE(search != nullptr);
                       QTest::keyClicks(search, QStringLiteral("advanced"));
                       QTest::keyClick(search, Qt::Key_Return);
                   });

    QTest::keyClick(&window, Qt::Key_K, Qt::ControlModifier);
    QApplication::processEvents();

    REQUIRE(choose.ran());
    REQUIRE(advanced->isChecked());
}

TEST_CASE("journey: remove a setup with the Delete key")
{
    Session session;

    const auto port =
        session.backend.create_auto_port(QStringLiteral("program"), true);
    REQUIRE(port.has_value());
    QApplication::processEvents();

    auto* list = session.simple()->list();
    REQUIRE_EQ(list->count(), 1);
    list->setFocus();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                      list->visualItemRect(list->item(0)).center());
    QApplication::processEvents();
    REQUIRE(session.simple()->setup_page() != nullptr);

    Waiting confirm(
        [](QWidget* widget)
        { return button(widget, QStringLiteral("Remove setup")) != nullptr; },
        [](QWidget* dialog)
        {
            QTest::mouseClick(button(dialog, QStringLiteral("Remove setup")),
                              Qt::LeftButton);
        });

    QTest::keyClick(list, Qt::Key_Delete);
    QApplication::processEvents();
    QApplication::processEvents();

    REQUIRE(confirm.ran());
    REQUIRE(session.backend.ports().empty());
    REQUIRE(job_tile(*session.window,
                     QStringLiteral("Give my program a serial port")) !=
            nullptr);
}

TEST_CASE("journey: remove a port with the Delete key, in Advanced mode")
{
    Session session(true);
    auto& window = *session.window;

    const auto port = session.backend.create_virtual_port(
        QStringLiteral("device"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    QApplication::processEvents();

    auto* ports_dock =
        window.findChild<QDockWidget*>(QStringLiteral("portsDock"));
    REQUIRE(ports_dock != nullptr);
    auto* list = ports_dock->findChild<QListView*>();
    REQUIRE(list != nullptr);
    list->setFocus();
    list->setCurrentIndex(list->model()->index(0, 0));
    QApplication::processEvents();

    Waiting confirm(
        [](QWidget* widget)
        { return button(widget, QStringLiteral("Remove port")) != nullptr; },
        [](QWidget* dialog)
        {
            QTest::mouseClick(button(dialog, QStringLiteral("Remove port")),
                              Qt::LeftButton);
        });

    QTest::keyClick(list, Qt::Key_Delete);
    QApplication::processEvents();

    REQUIRE(confirm.ran());
    REQUIRE(session.backend.ports().empty());
    // Advanced mode builds on the canvas: the empty setup stays on it.
    REQUIRE(ports_dock->isVisible());
    auto* card = launcher_card(window, QStringLiteral("Give my program a "
                                                      "serial port"));
    REQUIRE(card == nullptr || !card->isVisible());
}
