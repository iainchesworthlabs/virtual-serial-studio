//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The harness of the UI suite: a window that is driven only by input events,
// a pilot for dialogs that run their own event loop, a probe that fails a
// journey when the GUI thread stalls, and a step log with a window grab for
// the journey that failed. See docs/testing.md for the map of the journeys.
//

#pragma once

#include "test_framework.hpp"

#include "backend/capabilities.hpp"
#include "backend/fake_studio_backend.hpp"
#include "shell/shell_window.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vss::gui::topology
{
class LinksList;
class Toolbox;
class TopologyView;
} // namespace vss::gui::topology

namespace vss::gui::simple
{
class JobForm;
class SetupPage;
class SimplePage;
} // namespace vss::gui::simple

namespace vss::gui::ui
{

/// Every capability the real engine reports on a desktop, so the journeys see
/// the whole window and not the stripped-down one.
capabilities full_capabilities();

/// How long the GUI thread may compute inside one step of a journey, in
/// milliseconds (and ten times as long it may wait). The budget of each
/// journey is in `perf_budgets.json`, written for a release build; a build
/// that is slower by a known factor (`VSS_UI_BUDGET_SCALE`, set by CMake for
/// debug, sanitizer and coverage builds) multiplies it. `VSS_UI_BUDGET_MS`
/// replaces the whole table with one number, for a quick look.
int stall_budget_ms(const std::string& journey);
/// The budget of the journey now running.
int stall_budget_ms();

/// Measures how long the GUI thread is away from the event loop. The test
/// beats before and after every simulated input, and a timer beats while the
/// event loop runs, so a gap larger than the budget is a handler that blocked
/// the window. A second thread notices a thread that never comes back, prints
/// the step it hung in and ends the process, so a freeze fails with a name
/// instead of waiting for the CTest timeout.
class Probe
{
public:
    struct stall
    {
        std::string step;
        std::int64_t milliseconds = 0;
        /// CPU time the GUI thread used in that gap. Far below the gap means
        /// it waited (a lock, another thread, a busy machine); close to it
        /// means it computed.
        std::int64_t cpu_milliseconds = 0;
    };

    static Probe& instance();

    /// Starts watching a journey. Nothing is measured before this.
    void start(const std::string& journey);
    void stop();
    bool armed() const;
    /// While on, a long gap is not a stall. For steps that hand the thread to
    /// the operating system on purpose, such as the system's file dialog
    /// listing drives, which can wait on a network drive for seconds.
    void tolerate(bool on);
    /// For the step now running: it works on `items` things at once (fifty
    /// ports deleted together), so the budget grows by a quarter of itself for
    /// each. The next step starts from the plain budget again.
    void allow_bulk(int items);

    /// Names what the journey is doing now and counts as a beat.
    void step(const std::string& text);
    void beat();

    /// A line for the crash report: the journey and the step it was in.
    std::string where() const;

    /// The most CPU time the GUI thread used in one gap of the journey, and the
    /// step it was in. The numbers `perf_budgets.json` is made from.
    std::int64_t worst_cpu_milliseconds() const;
    std::string worst_cpu_step() const;

    std::vector<stall> stalls() const;
    std::vector<std::string> log() const;
    std::int64_t worst_milliseconds() const;
    std::string worst_step() const;
    int steps() const;
    /// The budget of the journey now running, in milliseconds.
    int budget() const;

private:
    Probe();
    struct state;
    std::unique_ptr<state> state_;
};

/// Makes a crash or an uncaught exception print the journey and its step
/// before the process ends, so a lost run still says where it was.
void install_crash_reporter();

/// Drives the dialogs of a journey. A dialog's exec() blocks the test body,
/// so the answer to it is armed first and runs from a timer once the dialog is
/// on screen. A dialog nobody expected is closed and fails the journey, which
/// is how a dialog that opens again (or opens when it should not) is caught.
class Pilot
{
public:
    using Match = std::function<bool(QWidget*)>;
    using Step = std::function<void(QWidget*)>;

    Pilot();
    ~Pilot();
    Pilot(const Pilot&) = delete;
    Pilot& operator=(const Pilot&) = delete;

    /// The next dialog that `match` accepts is handed to `step`. Expectations
    /// are used in the order they were armed.
    void expect(std::string what, Match match, Step step);

    template <class Dialog> void expect_type(std::string what, Step step)
    {
        expect(
            std::move(what), [](QWidget* widget)
            { return qobject_cast<Dialog*>(widget) != nullptr; },
            std::move(step));
    }

    /// A dialog with a button of this text, such as a confirmation.
    void expect_button(std::string what, const QString& button_text);

    /// How many expectations ran.
    int ran() const;
    /// True when every armed expectation has run.
    bool all_ran() const;
    /// Throws when a step failed, an expected dialog never opened, or a
    /// dialog nobody expected appeared.
    void check();
    /// Forgets the expectations that did not run (for a journey that stops
    /// early on purpose).
    void clear();
    /// The dialogs nobody expected, as "Class: title".
    QStringList unexpected() const;

    /// What to do with a dialog nobody expected, instead of failing: `step`
    /// runs on it and it is closed afterwards. For the journeys that walk
    /// every menu entry and have to look at whatever opens.
    void fallback(Step step);
    /// The dialogs the fallback handled, as "Class: title".
    QStringList visited() const;

private:
    struct state;
    std::unique_ptr<state> state_;
};

/// A window, a backend and everything a journey needs. It starts from a clean
/// QSettings and shows the main window the way a first start does. When the
/// journey fails it writes a PNG of the window to the failure directory and
/// prints the steps that were taken.
class Session
{
public:
    enum class mode
    {
        simple,
        advanced
    };

    /// With the in-memory backend. `caps` are the backend's capabilities.
    explicit Session(std::string journey, mode where = mode::simple,
                     capabilities caps = full_capabilities());
    /// With a backend the caller owns, which must outlive the session.
    Session(std::string journey, StudioBackend& backend,
            mode where = mode::simple);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// The backend as the window sees it.
    StudioBackend& backend() { return *backend_; }
    /// The in-memory backend; throws when the session runs on another one.
    FakeStudioBackend& fake();
    ShellWindow& window() { return *window_; }
    Pilot& pilot() { return pilot_; }
    const std::string& journey() const noexcept { return journey_; }

    /// Logs what the journey is about to do. Use the words of a person.
    void step(const QString& text);
    /// Lets queued work run until the event queue is quiet.
    void settle();
    /// Ends the journey: the dialogs ran, no dialog is left, and the GUI
    /// thread never stalled past the budget.
    void finish();

    // The pieces of the window, found the way a person finds them.
    simple::SimplePage* simple_page();
    topology::TopologyView* canvas();
    topology::Toolbox* toolbox();
    topology::LinksList* links_list();
    QDockWidget* dock(const char* object_name);
    QAction* action(const char* id);
    /// The list or table of the Ports dock that is showing.
    QAbstractItemView* ports_view();
    /// How many rows the Ports dock shows; 0 when it shows its empty state.
    int ports_rows();

    /// Switches mode through the Advanced mode switch of the window.
    void set_mode(mode where);
    bool is_advanced();

    /// Rebuilds the window on the same backend, as quitting and starting
    /// again does. Settings stay.
    void restart_window();

private:
    void build_window();
    void report_failure();

    std::string journey_;
    std::unique_ptr<FakeStudioBackend> owned_;
    StudioBackend* backend_ = nullptr;
    std::unique_ptr<ShellWindow> window_;
    Pilot pilot_;
};

/// Marks a step that is known to fail. The body runs; a failure inside it is
/// reported as an expected failure that names the issue, and a body that
/// passes fails the journey so the marker is removed with the fix.
void expect_failure(const char* issue, const std::function<void()>& body);

// ---- Input ---------------------------------------------------------------
//
// Every function sends real events to the widget under the point and beats
// the probe around them, so the time a handler takes is measured.

/// Scrolls the widget into view in every scroll area above it.
void reveal(QWidget* widget);
/// Throws unless the widget is visible and enabled, naming it.
void require_usable(QWidget* widget, const char* what);
/// A short description for a failure message.
QString describe(const QWidget* widget);

void click(QWidget* widget, Qt::KeyboardModifiers mods = {});
void click_at(QWidget* widget, const QPoint& position,
              Qt::KeyboardModifiers mods = {});
void double_click_at(QWidget* widget, const QPoint& position);
/// Clicks whatever is under a screen point now, as a hand does that clicks
/// again before the window has changed, or after it has. Nothing is checked
/// about the target.
void click_screen(const QPoint& global);
/// Focuses a field with a click, replaces its text and types the new one a
/// key at a time.
void type_into(QLineEdit* field, const QString& text);
void key(QWidget* widget, Qt::Key key, Qt::KeyboardModifiers mods = {});
/// Opens a combo box with a click and picks the entry with a click.
void choose(QComboBox* combo, const QString& text);
void click_row(QListWidget* list, int row);
/// Presses, moves in steps and releases, as a drag does.
void drag(QWidget* widget, const QPoint& from, const QPoint& to);

/// Sends the context-menu event a right click produces and returns the menu
/// that opened, or nullptr when none did.
QMenu* open_context_menu(QWidget* widget, const QPoint& position);
QStringList menu_labels(const QMenu* menu);
/// Clicks an entry of an open menu. Throws, listing the entries, when it is
/// missing or disabled.
void choose_entry(QMenu* menu, const QString& text);
/// True when the menu has an enabled entry with this text.
bool menu_has(const QMenu* menu, const QString& text);
/// Opens a menu of the menu bar with a click and chooses an entry. With
/// `prefix` the entry only has to start with `entry` (Undo, Undo Add port).
void choose_menu(Session& session, const QString& menu, const QString& entry,
                 bool prefix = false);
/// Opens a menu of the menu bar, then the submenus in `path` but the last,
/// and chooses the last entry: View, Panes, Topology JSON.
void choose_menu_path(Session& session, const QStringList& path);
/// Opens the submenu of an entry with a click and returns it, or throws when
/// it did not open.
QMenu* open_submenu(QMenu* menu, const QString& text);
/// Throws, listing the entries, unless the menu has an enabled entry.
void require_entry(const QMenu* menu, const QString& text);
/// The text the status bar shows now.
QString status_text(Session& session);
void close_menu(QMenu* menu);

/// Looks at a window the way a person trying every control would: clicks
/// each enabled button, flips each check box and switch there and back, and
/// picks each entry of each list, skipping controls whose text contains one of
/// `avoid` (those that remove, install or restart something). Dialogs the
/// clicks open are the pilot's business. Returns how many controls it used.
int exercise(QWidget* root, const QStringList& avoid, int milliseconds = 6000);

// ---- Finding things ------------------------------------------------------

/// A visible child by object name. Throws, naming the root, when missing.
template <class T = QWidget> T* named(QWidget* root, const char* object_name)
{
    for (auto* candidate : root->findChildren<T*>(
             QString::fromLatin1(object_name), Qt::FindChildrenRecursively))
    {
        if (candidate->isVisibleTo(root))
            return candidate;
    }
    throw vss_test::failure(std::string("no visible ") + object_name +
                            " under " + describe(root).toStdString());
}

/// A check box with this text, visible under `root`.
QCheckBox* checkbox(QWidget* root, const QString& text);
/// A push button with this text or accessible name, visible under `root`.
QPushButton* button(QWidget* root, const QString& text);
/// The first visible button whose text starts with one of the prefixes, or
/// nullptr.
QPushButton* button_starting(QWidget* root, const QStringList& prefixes);
/// True when such a button exists.
bool has_button(QWidget* root, const QString& text);
/// The texts of the visible push buttons, for failure messages.
QStringList button_texts(QWidget* root);
/// The first visible widget with this accessible name.
QWidget* accessible(QWidget* root, const QString& name);
QStringList accessible_names(QWidget* root);

// ---- Waiting -------------------------------------------------------------

/// Runs the event loop until `condition` holds. Throws, with `what`, when it
/// does not within `timeout_ms`.
void wait_for(const char* what, const std::function<bool()>& condition,
              int timeout_ms = 5000);

/// What the hub would deliver for bytes crossing a port: one data event, on
/// the GUI thread, as the engine bridge does. The in-memory backend never
/// raises events of its own.
void deliver_traffic(Session& session, vss::endpoint_id port,
                     const QByteArray& bytes,
                     vss::data_direction direction = vss::data_direction::tx);

// ---- Topology helpers ----------------------------------------------------

vss::endpoint_id port_id(StudioBackend& backend, const QString& name);
/// The port with this id; throws when there is none.
port_view port_of(StudioBackend& backend, vss::endpoint_id id);
std::vector<QString> port_names(StudioBackend& backend);
QPoint node_point(topology::TopologyView* view, vss::endpoint_id id);
QPoint link_point(topology::TopologyView* view, vss::link_id id);

} // namespace vss::gui::ui
