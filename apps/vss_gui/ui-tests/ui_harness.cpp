//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "ui_harness.hpp"

#include "qt_wait.hpp"
#include "simple/simple_page.hpp"
#include "theme/theme.hpp"
#include "topology/links_list.hpp"
#include "topology/toolbox.hpp"
#include "topology/topology_view.hpp"
#include "ui_platform.hpp"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenuBar>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QToolButton>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

namespace vss::gui::ui
{
namespace
{

using clock_type = std::chrono::steady_clock;

std::int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clock_type::now().time_since_epoch())
        .count();
}

std::int64_t thread_cpu_ns()
{
    return platform::thread_cpu_ns();
}

int env_int(const char* name, int fallback)
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok && value > 0 ? value : fallback;
}

QString plain(QString text)
{
    text.remove(QLatin1Char('&'));
    return text;
}

std::string sanitized(const std::string& name)
{
    std::string out;
    for (const char c : name)
    {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9');
        out.push_back(keep ? c : '_');
    }
    return out;
}

QString failure_directory()
{
    const QString configured = qEnvironmentVariable("VSS_UI_FAILURE_DIR");
    return configured.isEmpty()
               ? QDir::temp().filePath(QStringLiteral("vss-ui-failures"))
               : configured;
}

void send_mouse(QWidget* widget, QEvent::Type type, const QPoint& position,
                Qt::MouseButton button, Qt::MouseButtons buttons,
                Qt::KeyboardModifiers mods)
{
    QMouseEvent event(type, QPointF(position),
                      QPointF(widget->mapToGlobal(position)), button, buttons,
                      mods);
    QApplication::sendEvent(widget, &event);
}

} // namespace

capabilities full_capabilities()
{
    capabilities caps;
    caps.network_server = true;
    caps.benchmark = true;
    caps.faults = true;
    caps.fixture_echo = true;
    caps.line_format = true;
    caps.recreate_ports = true;
    caps.undo = true;
    caps.line_matrix = true;
    caps.shared_bus = true;
    caps.json_positions = true;
    caps.rename_port = true;
    caps.port_settings = true;
    return caps;
}

namespace
{

/// The release budgets by journey, from `perf_budgets.json`.
struct budget_table
{
    int default_ms = 150;
    std::map<std::string, int> journeys;
};

const budget_table& budgets()
{
    static const budget_table table = []
    {
        budget_table loaded;
        QString path = qEnvironmentVariable("VSS_UI_BUDGETS_FILE");
        if (path.isEmpty())
            path = QStringLiteral(VSS_UI_BUDGETS_PATH);
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
            return loaded;
        const QJsonObject root =
            QJsonDocument::fromJson(file.readAll()).object();
        loaded.default_ms =
            root.value(QStringLiteral("default_ms")).toInt(loaded.default_ms);
        const QJsonObject journeys =
            root.value(QStringLiteral("journeys")).toObject();
        for (auto it = journeys.begin(); it != journeys.end(); ++it)
            loaded.journeys[it.key().toStdString()] = it.value().toInt();
        return loaded;
    }();
    return table;
}

} // namespace

int stall_budget_ms(const std::string& journey)
{
    const int forced = env_int("VSS_UI_BUDGET_MS", 0);
    if (forced > 0)
        return forced;
    const budget_table& table = budgets();
    const auto found = table.journeys.find(journey);
    const int release_ms =
        found != table.journeys.end() ? found->second : table.default_ms;
    return release_ms * env_int("VSS_UI_BUDGET_SCALE", 1);
}

int stall_budget_ms()
{
    return Probe::instance().budget();
}

// ---- Probe ---------------------------------------------------------------

struct Probe::state
{
    std::atomic<bool> armed{false};
    std::atomic<std::int64_t> last_beat{0};
    std::atomic<std::int64_t> step_started{0};
    std::int64_t last_cpu = 0;
    mutable std::mutex lock;
    std::string journey;
    std::string current;
    std::vector<std::string> log;
    std::vector<stall> stalls;
    std::atomic<bool> tolerant{false};
    std::atomic<int> extra_ms{0};
    std::atomic<int> budget{150};
    std::int64_t worst_cpu = 0;
    std::string worst_cpu_step;
    std::int64_t worst = 0;
    std::string worst_step;
    int steps = 0;
    QTimer* timer = nullptr;
    // A plain thread and a flag, not std::jthread: the libc++ of the macOS 15
    // toolchain does not have it.
    std::atomic<bool> stopping{false};
    std::thread watchdog;

    ~state()
    {
        stopping = true;
        if (watchdog.joinable())
            watchdog.join();
    }
};

Probe::Probe() : state_(std::make_unique<state>()) {}

Probe& Probe::instance()
{
    static Probe probe;
    return probe;
}

void Probe::start(const std::string& journey)
{
    {
        const std::lock_guard<std::mutex> guard(state_->lock);
        state_->journey = journey;
        state_->current = "start";
        state_->log.clear();
        state_->stalls.clear();
        state_->worst = 0;
        state_->worst_step.clear();
        state_->worst_cpu = 0;
        state_->worst_cpu_step.clear();
        state_->budget = stall_budget_ms(journey);
        state_->steps = 0;
    }
    state_->last_beat = now_ns();
    state_->last_cpu = thread_cpu_ns();
    state_->step_started = state_->last_beat.load();
    state_->armed = true;
    if (state_->timer == nullptr)
    {
        state_->timer = new QTimer(QApplication::instance());
        state_->timer->setTimerType(Qt::PreciseTimer);
        state_->timer->setInterval(4);
        QObject::connect(state_->timer, &QTimer::timeout, state_->timer,
                         [this] { beat(); });
    }
    state_->timer->start();
    if (!state_->watchdog.joinable())
    {
        const int hang_ms = env_int("VSS_UI_HANG_MS", 30000);
        // The thread holds the state itself, not the Probe's pointer to it: at
        // exit the pointer is already null (libc++) while ~state joins the
        // thread, and reading through it crashed the macOS test programs.
        state* const shared = state_.get();
        shared->watchdog = std::thread(
            [shared, hang_ms]()
            {
                while (!shared->stopping)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (!shared->armed)
                        continue;
                    const std::int64_t gap =
                        std::max((now_ns() - shared->last_beat),
                                 (now_ns() - shared->step_started)) /
                        1'000'000;
                    if (gap < hang_ms)
                        continue;
                    std::string text;
                    {
                        const std::lock_guard<std::mutex> guard(shared->lock);
                        text = "[HANG] " + shared->journey +
                               ": the journey "
                               "has been in one step, or away from the event "
                               "loop, for " +
                               std::to_string(gap) + " ms in step \"" +
                               shared->current + "\"\n";
                        for (const auto& line : shared->log)
                            text += "        step: " + line + "\n";
                    }
                    std::fputs(text.c_str(), stdout);
                    std::fflush(stdout);
                    std::_Exit(3);
                }
            });
    }
}

void Probe::allow_bulk(int items)
{
    state_->extra_ms = items * stall_budget_ms() / 4;
}

void Probe::tolerate(bool on)
{
    state_->tolerant = on;
}

void Probe::stop()
{
    state_->armed = false;
    if (state_->timer != nullptr)
        state_->timer->stop();
}

bool Probe::armed() const
{
    return state_->armed;
}

void Probe::step(const std::string& text)
{
    beat();
    {
        const std::lock_guard<std::mutex> guard(state_->lock);
        state_->current = text;
        state_->log.push_back(text);
        ++state_->steps;
    }
    state_->extra_ms = 0;
    state_->step_started = now_ns();
    if (qEnvironmentVariableIsSet("VSS_UI_TRACE"))
    {
        std::printf("        step: %s\n", text.c_str());
        std::fflush(stdout);
    }
}

void Probe::beat()
{
    const std::int64_t now = now_ns();
    const std::int64_t cpu = thread_cpu_ns();
    if (state_->armed)
    {
        const std::int64_t gap = (now - state_->last_beat) / 1'000'000;
        const std::int64_t used = (cpu - state_->last_cpu) / 1'000'000;
        const std::lock_guard<std::mutex> guard(state_->lock);
        if (gap > state_->worst)
        {
            state_->worst = gap;
            state_->worst_step = state_->current;
        }
        if (used > state_->worst_cpu)
        {
            state_->worst_cpu = used;
            state_->worst_cpu_step = state_->current;
        }
        // Computing for longer than the budget is a stall. So is waiting for
        // more than ten times it, as on a lock a freeze sits in. Waiting for a
        // little longer is not: on a busy machine every wait grows, and what
        // the budget judges is the application's own work.
        const std::int64_t allowed = stall_budget_ms() + state_->extra_ms;
        if ((used > allowed || gap > 10 * allowed) && !state_->tolerant)
            state_->stalls.push_back({state_->current, gap, used});
    }
    state_->last_beat = now;
    state_->last_cpu = cpu;
}

std::string Probe::where() const
{
    const std::lock_guard<std::mutex> guard(state_->lock);
    std::string text = "journey \"" + state_->journey + "\", step \"" +
                       state_->current + "\"\n";
    for (const auto& line : state_->log)
        text += "        step: " + line + "\n";
    return text;
}

std::vector<Probe::stall> Probe::stalls() const
{
    const std::lock_guard<std::mutex> guard(state_->lock);
    return state_->stalls;
}

std::vector<std::string> Probe::log() const
{
    const std::lock_guard<std::mutex> guard(state_->lock);
    return state_->log;
}

std::int64_t Probe::worst_cpu_milliseconds() const
{
    const std::lock_guard<std::mutex> guard(state_->lock);
    return state_->worst_cpu;
}

std::string Probe::worst_cpu_step() const
{
    const std::lock_guard<std::mutex> guard(state_->lock);
    return state_->worst_cpu_step;
}

std::int64_t Probe::worst_milliseconds() const
{
    const std::lock_guard<std::mutex> guard(state_->lock);
    return state_->worst;
}

std::string Probe::worst_step() const
{
    const std::lock_guard<std::mutex> guard(state_->lock);
    return state_->worst_step;
}

int Probe::budget() const
{
    return state_->budget;
}

int Probe::steps() const
{
    const std::lock_guard<std::mutex> guard(state_->lock);
    return state_->steps;
}

namespace
{

[[noreturn]] void crash_exit(const char* what)
{
    std::string text =
        std::string("[CRASH] ") + what + " in " + Probe::instance().where();
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
    std::_Exit(4);
}

} // namespace

void install_crash_reporter()
{
    std::set_terminate(
        []
        {
            std::string what = "terminate called";
            if (const auto error = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(error);
                }
                catch (const std::exception& known)
                {
                    what += std::string(": ") + known.what();
                }
                catch (...)
                {
                    what += " with an unknown exception";
                }
            }
            crash_exit(what.c_str());
        });
    std::signal(SIGABRT, [](int) { crash_exit("abort() was called"); });
    platform::install_crash_hooks([](const char* what) { crash_exit(what); });
}

// ---- Pilot ---------------------------------------------------------------

struct Pilot::state
{
    struct expectation
    {
        std::string what;
        Match match;
        Step step;
        bool used = false;
    };
    struct handled_dialog
    {
        QPointer<QWidget> widget;
        QElapsedTimer age;
        bool closed = false;
    };

    QTimer* timer = nullptr;
    /// While a step runs, the poll timer is mid-slot and Qt does not fire it
    /// again, so a dialog the step opens (and waits for) would never be seen.
    /// This second timer polls for it until the step returns.
    QTimer* nested = nullptr;
    std::vector<expectation> expectations;
    std::vector<handled_dialog> handled;
    QStringList unexpected;
    QStringList visited;
    Step fallback;
    int depth = 0;
    std::vector<std::string> errors;
    int ran = 0;

    void poll()
    {
        for (auto& dialog : handled)
        {
            if (dialog.widget != nullptr && !dialog.closed &&
                dialog.widget->isVisible() && dialog.age.hasExpired(8000))
            {
                dialog.closed = true;
                errors.push_back(
                    "a dialog stayed open for 8 s after its step: " +
                    describe(dialog.widget).toStdString() + "; its buttons: " +
                    button_texts(dialog.widget)
                        .join(QStringLiteral(" | "))
                        .toStdString() +
                    "; what it shows: " +
                    accessible_names(dialog.widget)
                        .join(QStringLiteral(" | "))
                        .toStdString());
                dialog.widget->close();
            }
        }
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr)
            return;
        for (const auto& dialog : handled)
        {
            if (dialog.widget == modal)
                return;
        }
        handled.push_back({QPointer<QWidget>(modal), {}, false});
        handled.back().age.start();

        for (auto& pending : expectations)
        {
            if (pending.used || !pending.match(modal))
                continue;
            pending.used = true;
            ++ran;
            if (nested != nullptr)
                nested->start();
            try
            {
                pending.step(modal);
            }
            catch (const std::exception& error)
            {
                errors.push_back(pending.what + ": " + error.what());
                if (modal != nullptr)
                    modal->close();
            }
            if (nested != nullptr && depth == 0)
                nested->stop();
            // The time a dialog may stay open counts from the end of its step.
            for (auto& dialog : handled)
            {
                if (dialog.widget == modal)
                    dialog.age.restart();
            }
            return;
        }
        if (fallback && depth < 1)
        {
            visited << describe(modal);
            ++depth;
            if (nested != nullptr)
                nested->start();
            try
            {
                fallback(modal);
            }
            catch (const std::exception& error)
            {
                errors.push_back("the fallback on " +
                                 describe(modal).toStdString() + ": " +
                                 error.what());
            }
            --depth;
            if (nested != nullptr && depth == 0)
                nested->stop();
            if (modal != nullptr && modal->isVisible())
                modal->close();
            return;
        }
        if (fallback)
        {
            visited << describe(modal);
            modal->close();
            return;
        }
        unexpected << describe(modal);
        modal->close();
    }
};

Pilot::Pilot() : state_(std::make_unique<state>())
{
    state_->timer = new QTimer();
    state_->timer->setInterval(5);
    QObject::connect(state_->timer, &QTimer::timeout, state_->timer,
                     [this]
                     {
                         Probe::instance().beat();
                         state_->poll();
                     });
    state_->timer->start();
    state_->nested = new QTimer();
    state_->nested->setInterval(5);
    QObject::connect(state_->nested, &QTimer::timeout, state_->nested,
                     [this]
                     {
                         Probe::instance().beat();
                         state_->poll();
                     });
}

Pilot::~Pilot()
{
    delete state_->nested;
    delete state_->timer;
}

void Pilot::expect(std::string what, Match match, Step step)
{
    state_->expectations.push_back(
        {std::move(what), std::move(match), std::move(step), false});
}

void Pilot::expect_button(std::string what, const QString& button_text)
{
    expect(
        std::move(what), [button_text](QWidget* widget)
        { return has_button(widget, button_text); },
        [button_text](QWidget* dialog) { click(button(dialog, button_text)); });
}

int Pilot::ran() const
{
    return state_->ran;
}

bool Pilot::all_ran() const
{
    for (const auto& expectation : state_->expectations)
    {
        if (!expectation.used)
            return false;
    }
    return true;
}

void Pilot::fallback(Step step)
{
    state_->fallback = std::move(step);
}

QStringList Pilot::visited() const
{
    return state_->visited;
}

QStringList Pilot::unexpected() const
{
    return state_->unexpected;
}

void Pilot::clear()
{
    state_->expectations.clear();
}

void Pilot::check()
{
    std::string problems;
    for (const auto& error : state_->errors)
        problems += "\n  " + error;
    for (const auto& dialog : state_->unexpected)
        problems += "\n  an unexpected dialog opened: " + dialog.toStdString();
    for (const auto& expectation : state_->expectations)
    {
        if (!expectation.used)
            problems += "\n  the dialog never opened: " + expectation.what;
    }
    state_->errors.clear();
    state_->unexpected.clear();
    if (!problems.empty())
        throw vss_test::failure("dialogs went wrong:" + problems);
}

// ---- Session -------------------------------------------------------------

namespace
{

/// The look every journey starts from; a journey that changes it must not
/// leave it to the next one.
void reset_look()
{
    Theme::instance().set_mode(theme_mode::light);
    Theme::instance().set_density(density_mode::comfortable);
}

} // namespace

Session::Session(std::string journey, mode where, capabilities caps)
    : journey_(std::move(journey)),
      owned_(std::make_unique<FakeStudioBackend>()), backend_(owned_.get())
{
    owned_->set_capabilities(caps);
    QSettings settings;
    settings.clear();
    // Closing the window must not stop on the unsaved-changes prompt.
    settings.setValue(QStringLiteral("ui/askOnDiscard"), false);
    settings.setValue(QStringLiteral("ui/advanced"), where == mode::advanced);
    settings.sync();
    build_window();
}

Session::Session(std::string journey, StudioBackend& backend, mode where)
    : journey_(std::move(journey)), backend_(&backend)
{
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("ui/askOnDiscard"), false);
    settings.setValue(QStringLiteral("ui/advanced"), where == mode::advanced);
    settings.sync();
    build_window();
}

void Session::build_window()
{
    reset_look();
    window_ = std::make_unique<ShellWindow>(*backend_);
    window_->resize(1280, 800);
    window_->show();
    window_->activateWindow();
    static_cast<void>(QTest::qWaitForWindowActive(window_.get(), 2000));
    QApplication::processEvents();
    Probe::instance().start(journey_);
}

Session::~Session()
{
    Probe::instance().stop();
    const bool failed = std::uncaught_exceptions() > 0;
    if (failed)
        report_failure();
    if (qEnvironmentVariableIsSet("VSS_UI_TIMING") || failed)
    {
        const auto& probe = Probe::instance();
        std::printf("        [%s] %d steps, worst stall %lld ms (%s)\n",
                    journey_.c_str(), probe.steps(),
                    static_cast<long long>(probe.worst_milliseconds()),
                    probe.worst_step().c_str());
    }
    if (window_ != nullptr)
    {
        window_->close();
        QApplication::processEvents();
    }
}

void Session::report_failure()
{
    std::string text = "        journey steps:\n";
    for (const auto& line : Probe::instance().log())
        text += "          - " + line + "\n";
    const QString directory = failure_directory();
    QDir().mkpath(directory);
    const QString base =
        QDir(directory).filePath(QString::fromStdString(sanitized(journey_)));
    if (window_ != nullptr)
    {
        const QString path = base + QStringLiteral(".png");
        if (window_->grab().save(path))
            text += "        window: " + path.toStdString() + "\n";
    }
    if (QWidget* modal = QApplication::activeModalWidget())
    {
        const QString path = base + QStringLiteral("-dialog.png");
        if (modal->grab().save(path))
            text += "        dialog: " + path.toStdString() + "\n";
    }
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
}

FakeStudioBackend& Session::fake()
{
    if (owned_ == nullptr)
        throw vss_test::failure("this journey runs on the real engine");
    return *owned_;
}

void Session::step(const QString& text)
{
    Probe::instance().step(text.toStdString());
    QApplication::processEvents();
}

void Session::settle()
{
    for (int round = 0; round < 3; ++round)
    {
        Probe::instance().beat();
        QApplication::processEvents();
    }
    Probe::instance().beat();
}

void Session::finish()
{
    settle();
    pilot_.check();
    if (QApplication::activeModalWidget() != nullptr)
    {
        throw vss_test::failure(
            "a dialog is still open: " +
            describe(QApplication::activeModalWidget()).toStdString());
    }
    std::string text;
    for (const auto& stall : Probe::instance().stalls())
    {
        text += "\n  " + std::to_string(stall.milliseconds) + " ms (" +
                std::to_string(stall.cpu_milliseconds) +
                " ms of CPU) in step \"" + stall.step + "\"";
    }
    if (!text.empty())
    {
        throw vss_test::failure("the GUI thread was blocked for more than " +
                                std::to_string(stall_budget_ms()) +
                                " ms:" + text);
    }
}

simple::SimplePage* Session::simple_page()
{
    return window_->findChild<simple::SimplePage*>();
}

topology::TopologyView* Session::canvas()
{
    return window_->findChild<topology::TopologyView*>();
}

topology::Toolbox* Session::toolbox()
{
    return window_->findChild<topology::Toolbox*>();
}

topology::LinksList* Session::links_list()
{
    return window_->findChild<topology::LinksList*>();
}

QDockWidget* Session::dock(const char* object_name)
{
    auto* found =
        window_->findChild<QDockWidget*>(QString::fromLatin1(object_name));
    if (found == nullptr)
        throw vss_test::failure(std::string("no dock ") + object_name);
    return found;
}

QAction* Session::action(const char* id)
{
    auto* found = window_->findChild<QAction*>(QString::fromLatin1(id));
    if (found == nullptr)
        throw vss_test::failure(std::string("no action ") + id);
    return found;
}

QAbstractItemView* Session::ports_view()
{
    for (auto* view : dock("portsDock")->findChildren<QAbstractItemView*>())
    {
        if (view->isVisibleTo(window_.get()))
            return view;
    }
    throw vss_test::failure("the Ports dock shows no list");
}

int Session::ports_rows()
{
    for (auto* view : dock("portsDock")->findChildren<QAbstractItemView*>())
    {
        if (view->isVisibleTo(window_.get()) && view->model() != nullptr)
            return view->model()->rowCount();
    }
    return 0;
}

bool Session::is_advanced()
{
    return action("view.advanced")->isChecked();
}

void Session::set_mode(mode where)
{
    if (is_advanced() == (where == mode::advanced))
        return;
    // The View menu, the way a person reaches it.
    choose_menu(*this, QStringLiteral("View"), QStringLiteral("Advanced mode"));
    settle();
    if (is_advanced() != (where == mode::advanced))
        throw vss_test::failure("choosing Advanced mode did not switch mode");
}

void Session::restart_window()
{
    window_->close();
    QApplication::processEvents();
    window_.reset();
    window_ = std::make_unique<ShellWindow>(*backend_);
    window_->resize(1280, 800);
    window_->show();
    window_->activateWindow();
    QApplication::processEvents();
}

void expect_failure(const char* issue, const std::function<void()>& body)
{
    try
    {
        body();
    }
    catch (const vss_test::failure& failure)
    {
        std::string first = failure.what();
        if (const auto cut = first.find('\n'); cut != std::string::npos)
            first.resize(cut);
        std::printf("[XFAIL] %s: %s\n", issue, first.c_str());
        return;
    }
    throw vss_test::failure(std::string("the failure expected for ") + issue +
                            " no longer happens: remove expect_failure");
}

// ---- Input ---------------------------------------------------------------

QString describe(const QWidget* widget)
{
    if (widget == nullptr)
        return QStringLiteral("(none)");
    QString text = QString::fromLatin1(widget->metaObject()->className());
    if (!widget->objectName().isEmpty())
        text += QStringLiteral(" #") + widget->objectName();
    if (!widget->accessibleName().isEmpty())
        text += QStringLiteral(" \"") + widget->accessibleName() +
                QStringLiteral("\"");
    else if (const auto* push = qobject_cast<const QPushButton*>(widget))
        text += QStringLiteral(" \"") + push->text() + QStringLiteral("\"");
    else if (widget->isWindow() && !widget->windowTitle().isEmpty())
        text += QStringLiteral(" \"") + widget->windowTitle() +
                QStringLiteral("\"");
    return text;
}

void reveal(QWidget* widget)
{
    for (QWidget* parent = widget->parentWidget(); parent != nullptr;
         parent = parent->parentWidget())
    {
        if (auto* area = qobject_cast<QScrollArea*>(parent))
            area->ensureWidgetVisible(widget, 0, 12);
    }
    QApplication::processEvents();
}

void require_usable(QWidget* widget, const char* what)
{
    if (widget == nullptr)
        throw vss_test::failure(std::string("cannot find ") + what);
    if (!widget->isVisible())
        throw vss_test::failure(std::string(what) + " is not visible: " +
                                describe(widget).toStdString());
    if (!widget->isEnabled())
        throw vss_test::failure(std::string(what) + " is disabled: " +
                                describe(widget).toStdString());
}

namespace
{

/// Throws when something else lies over the point, which a person would hit
/// instead of the widget.
void require_reachable(QWidget* widget, const QPoint& position)
{
    QWidget* top = widget->window();
    const QPoint in_window = widget->mapTo(top, position);
    if (!top->rect().contains(in_window))
        throw vss_test::failure("the point lies outside the window: " +
                                describe(widget).toStdString());
    QWidget* hit = top->childAt(in_window);
    if (hit == nullptr || hit == widget || widget->isAncestorOf(hit) ||
        hit->isAncestorOf(widget))
        return;
    throw vss_test::failure("another widget lies over " +
                            describe(widget).toStdString() + ": " +
                            describe(hit).toStdString());
}

} // namespace

void click(QWidget* widget, Qt::KeyboardModifiers mods)
{
    require_usable(widget, "the widget to click");
    reveal(widget);
    click_at(widget, widget->rect().center(), mods);
}

void click_at(QWidget* widget, const QPoint& position,
              Qt::KeyboardModifiers mods)
{
    require_usable(widget, "the widget to click");
    if (qobject_cast<QPushButton*>(widget) != nullptr)
        require_reachable(widget, position);
    Probe::instance().beat();
    QTest::mouseClick(widget, Qt::LeftButton, mods, position);
    Probe::instance().beat();
    QApplication::processEvents();
    Probe::instance().beat();
}

void click_screen(const QPoint& global)
{
    QWidget* target = QApplication::widgetAt(global);
    if (target == nullptr)
        return;
    Probe::instance().beat();
    QTest::mouseClick(target, Qt::LeftButton, {},
                      target->mapFromGlobal(global));
    Probe::instance().beat();
    QApplication::processEvents();
}

void double_click_at(QWidget* widget, const QPoint& position)
{
    require_usable(widget, "the widget to double-click");
    Probe::instance().beat();
    QTest::mouseDClick(widget, Qt::LeftButton, {}, position);
    Probe::instance().beat();
    QApplication::processEvents();
    Probe::instance().beat();
}

void type_into(QLineEdit* field, const QString& text)
{
    click(field);
    QTest::keyClick(field, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(field, Qt::Key_Delete);
    Probe::instance().beat();
    for (const QChar letter : text)
    {
        QTest::keyClicks(field, QString(letter));
        Probe::instance().beat();
    }
    QApplication::processEvents();
    Probe::instance().beat();
}

void key(QWidget* widget, Qt::Key key_code, Qt::KeyboardModifiers mods)
{
    // Shortcuts only fire in the active window, and the offscreen platform
    // loses the active window when a menu or a dialog closes.
    if (QApplication::activeWindow() != widget->window() &&
        QApplication::activePopupWidget() == nullptr)
    {
        widget->window()->activateWindow();
        QApplication::processEvents();
        // Activating a window gives its focus back to the widget that had it.
        if (widget->focusPolicy() != Qt::NoFocus && !widget->hasFocus())
            widget->setFocus();
    }
    Probe::instance().beat();
    QTest::keyClick(widget, key_code, mods);
    Probe::instance().beat();
    QApplication::processEvents();
    Probe::instance().beat();
}

void choose(QComboBox* combo, const QString& text)
{
    if (combo->isEditable() && combo->lineEdit() != nullptr)
    {
        // An editable list takes the value typed, as a person types it.
        type_into(combo->lineEdit(), text);
        key(combo->lineEdit(), Qt::Key_Return);
        if (combo->currentText() != text)
            throw vss_test::failure("typing \"" + text.toStdString() +
                                    "\" left \"" +
                                    combo->currentText().toStdString() +
                                    "\": " + describe(combo).toStdString());
        return;
    }
    click(combo);
    const int row = combo->findText(text);
    if (row < 0)
        throw vss_test::failure("the list has no \"" + text.toStdString() +
                                "\": " + describe(combo).toStdString());
    QAbstractItemView* view = combo->view();
    if (!view->isVisible())
        throw vss_test::failure("the list did not open: " +
                                describe(combo).toStdString());
    const QModelIndex index = combo->model()->index(row, 0);
    view->scrollTo(index);
    Probe::instance().beat();
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {},
                      view->visualRect(index).center());
    QApplication::processEvents();
    combo->hidePopup();
    Probe::instance().beat();
    if (combo->currentText() != text)
        throw vss_test::failure("clicking \"" + text.toStdString() +
                                "\" left \"" +
                                combo->currentText().toStdString() +
                                "\": " + describe(combo).toStdString());
}

void click_row(QListWidget* list, int row)
{
    require_usable(list, "the list");
    QListWidgetItem* item = list->item(row);
    if (item == nullptr)
        throw vss_test::failure("the list has no row " + std::to_string(row));
    list->scrollToItem(item);
    Probe::instance().beat();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                      list->visualItemRect(item).center());
    Probe::instance().beat();
    QApplication::processEvents();
    Probe::instance().beat();
}

void drag(QWidget* widget, const QPoint& from, const QPoint& to)
{
    Probe::instance().beat();
    send_mouse(widget, QEvent::MouseButtonPress, from, Qt::LeftButton,
               Qt::LeftButton, {});
    constexpr int kSteps = 6;
    for (int step = 1; step <= kSteps; ++step)
    {
        const QPoint at = from + (to - from) * step / kSteps;
        send_mouse(widget, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton,
                   {});
        Probe::instance().beat();
    }
    send_mouse(widget, QEvent::MouseButtonRelease, to, Qt::LeftButton,
               Qt::NoButton, {});
    Probe::instance().beat();
    QApplication::processEvents();
    Probe::instance().beat();
}

QMenu* open_context_menu(QWidget* widget, const QPoint& position)
{
    Probe::instance().beat();
    QContextMenuEvent event(QContextMenuEvent::Mouse, position,
                            widget->mapToGlobal(position));
    QApplication::sendEvent(widget, &event);
    QApplication::processEvents();
    Probe::instance().beat();
    return qobject_cast<QMenu*>(QApplication::activePopupWidget());
}

QStringList menu_labels(const QMenu* menu)
{
    QStringList labels;
    if (menu == nullptr)
        return labels;
    for (const QAction* entry : menu->actions())
    {
        if (!entry->isSeparator())
            labels << plain(entry->text());
    }
    return labels;
}

bool menu_has(const QMenu* menu, const QString& text)
{
    if (menu == nullptr)
        return false;
    for (const QAction* entry : menu->actions())
    {
        if (!entry->isSeparator() && plain(entry->text()) == text)
            return entry->isEnabled();
    }
    return false;
}

void choose_menu(Session& session, const QString& menu, const QString& entry,
                 bool prefix)
{
    auto* bar = session.window().menuBar();
    QAction* title = nullptr;
    for (QAction* candidate : bar->actions())
    {
        if (plain(candidate->text()) == menu)
            title = candidate;
    }
    if (title == nullptr || title->menu() == nullptr)
        throw vss_test::failure("the menu bar has no " + menu.toStdString() +
                                " menu");
    Probe::instance().beat();
    QTest::mouseClick(bar, Qt::LeftButton, {},
                      bar->actionGeometry(title).center());
    QApplication::processEvents();
    QMenu* opened = title->menu();
    if (!opened->isVisible())
        throw vss_test::failure("clicking " + menu.toStdString() +
                                " did not open its menu");
    if (!prefix)
    {
        choose_entry(opened, entry);
        return;
    }
    for (const QAction* candidate : opened->actions())
    {
        if (!candidate->isSeparator() &&
            plain(candidate->text()).startsWith(entry))
        {
            choose_entry(opened, plain(candidate->text()));
            return;
        }
    }
    throw vss_test::failure(
        "the " + menu.toStdString() + " menu has no entry starting with \"" +
        entry.toStdString() + "\"; it has: " +
        menu_labels(opened).join(QStringLiteral(" | ")).toStdString());
}

QMenu* open_submenu(QMenu* menu, const QString& text)
{
    if (menu == nullptr)
        throw vss_test::failure("no menu is open to open \"" +
                                text.toStdString() + "\" in");
    for (QAction* entry : menu->actions())
    {
        if (entry->isSeparator() || plain(entry->text()) != text ||
            entry->menu() == nullptr)
            continue;
        Probe::instance().beat();
        QTest::mouseClick(menu, Qt::LeftButton, {},
                          menu->actionGeometry(entry).center());
        QApplication::processEvents();
        if (!entry->menu()->isVisible())
        {
            // The click only highlights on some styles: open it as the Right
            // arrow key does.
            menu->setActiveAction(entry);
            QTest::keyClick(menu, Qt::Key_Right);
            QApplication::processEvents();
        }
        Probe::instance().beat();
        if (!entry->menu()->isVisible())
            throw vss_test::failure("the submenu \"" + text.toStdString() +
                                    "\" did not open");
        return entry->menu();
    }
    throw vss_test::failure(
        "the menu has no submenu \"" + text.toStdString() + "\"; it has: " +
        menu_labels(menu).join(QStringLiteral(" | ")).toStdString());
}

void require_entry(const QMenu* menu, const QString& text)
{
    if (!menu_has(menu, text))
        throw vss_test::failure(
            "the menu has no enabled \"" + text.toStdString() + "\"; it has: " +
            menu_labels(menu).join(QStringLiteral(" | ")).toStdString());
}

QString status_text(Session& session)
{
    auto* bar = session.window().statusBar();
    return bar != nullptr ? bar->currentMessage() : QString();
}

void choose_entry(QMenu* menu, const QString& text)
{
    if (menu == nullptr)
        throw vss_test::failure("no menu is open to choose \"" +
                                text.toStdString() + "\" from");
    if (!menu->isVisible())
        throw vss_test::failure("the menu is not showing, so \"" +
                                text.toStdString() + "\" cannot be chosen");
    for (QAction* entry : menu->actions())
    {
        if (entry->isSeparator() || plain(entry->text()) != text)
            continue;
        if (!entry->isEnabled())
            throw vss_test::failure("the menu entry \"" + text.toStdString() +
                                    "\" is disabled");
        Probe::instance().beat();
        QTest::mouseClick(menu, Qt::LeftButton, {},
                          menu->actionGeometry(entry).center());
        Probe::instance().beat();
        QApplication::processEvents();
        Probe::instance().beat();
        return;
    }
    throw vss_test::failure(
        "the menu has no \"" + text.toStdString() + "\"; it has: " +
        menu_labels(menu).join(QStringLiteral(" | ")).toStdString());
}

void close_menu(QMenu* menu)
{
    if (menu != nullptr)
        menu->close();
    QApplication::processEvents();
}

// ---- Finding things ------------------------------------------------------

QCheckBox* checkbox(QWidget* root, const QString& text)
{
    for (auto* candidate : root->findChildren<QCheckBox*>())
    {
        if (candidate->isVisibleTo(root) && plain(candidate->text()) == text)
            return candidate;
    }
    throw vss_test::failure("no check box \"" + text.toStdString() +
                            "\" under " + describe(root).toStdString());
}

QPushButton* button(QWidget* root, const QString& text)
{
    for (auto* candidate : root->findChildren<QPushButton*>())
    {
        if (!candidate->isVisibleTo(root))
            continue;
        if (plain(candidate->text()) == text ||
            candidate->accessibleName() == text)
            return candidate;
    }
    throw vss_test::failure(
        "no button \"" + text.toStdString() + "\" under " +
        describe(root).toStdString() + "; the buttons are: " +
        button_texts(root).join(QStringLiteral(" | ")).toStdString());
}

QPushButton* button_starting(QWidget* root, const QStringList& prefixes)
{
    for (auto* candidate : root->findChildren<QPushButton*>())
    {
        if (!candidate->isVisibleTo(root))
            continue;
        for (const QString& prefix : prefixes)
        {
            if (plain(candidate->text()).startsWith(prefix))
                return candidate;
        }
    }
    return nullptr;
}

bool has_button(QWidget* root, const QString& text)
{
    for (auto* candidate : root->findChildren<QPushButton*>())
    {
        if (candidate->isVisibleTo(root) &&
            (plain(candidate->text()) == text ||
             candidate->accessibleName() == text))
            return true;
    }
    return false;
}

QStringList button_texts(QWidget* root)
{
    QStringList texts;
    for (auto* candidate : root->findChildren<QPushButton*>())
    {
        if (!candidate->isVisibleTo(root))
            continue;
        texts << (candidate->text().isEmpty() ? candidate->accessibleName()
                                              : plain(candidate->text()));
    }
    return texts;
}

QWidget* accessible(QWidget* root, const QString& name)
{
    for (auto* candidate : root->findChildren<QWidget*>())
    {
        if (candidate->isVisibleTo(root) && candidate->accessibleName() == name)
            return candidate;
    }
    throw vss_test::failure("nothing named \"" + name.toStdString() +
                            "\" under " + describe(root).toStdString());
}

QStringList accessible_names(QWidget* root)
{
    QStringList names;
    for (auto* candidate : root->findChildren<QWidget*>())
    {
        if (candidate->isVisibleTo(root) &&
            !candidate->accessibleName().isEmpty())
            names << candidate->accessibleName();
    }
    return names;
}

int exercise(QWidget* root, const QStringList& avoid, int milliseconds)
{
    QElapsedTimer clock;
    clock.start();
    const auto avoided = [&avoid](const QString& text)
    {
        for (const QString& word : avoid)
        {
            if (text.contains(word, Qt::CaseInsensitive))
                return true;
        }
        return false;
    };
    int used = 0;
    int skipped = 0;
    QPointer<QWidget> guard(root);
    // Snapshots: a click may rebuild the page it is on.
    QList<QPointer<QAbstractButton>> buttons;
    for (auto* candidate : root->findChildren<QAbstractButton*>())
        buttons.append(candidate);
    for (const auto& pointer : buttons)
    {
        if (guard == nullptr || clock.hasExpired(milliseconds))
            return used;
        QAbstractButton* candidate = pointer;
        if (candidate == nullptr || !candidate->isVisibleTo(root) ||
            !candidate->isEnabled() || candidate->parentWidget() == nullptr)
            continue;
        const QString text =
            plain(candidate->text()) + candidate->accessibleName();
        if (avoided(text) || qobject_cast<QToolButton*>(candidate) != nullptr)
            continue;
        const bool toggles = candidate->isCheckable() ||
                             qobject_cast<QCheckBox*>(candidate) != nullptr;
        // Trying controls is exploring, not asserting: one that cannot be
        // reached is skipped, not a failure of the journey.
        try
        {
            click(candidate);
            ++used;
            if (toggles && pointer != nullptr && candidate->isEnabled())
                click(candidate);
        }
        catch (const vss_test::failure&)
        {
            ++skipped;
        }
    }
    QList<QPointer<QComboBox>> combos;
    for (auto* candidate : root->findChildren<QComboBox*>())
        combos.append(candidate);
    for (const auto& pointer : combos)
    {
        QComboBox* combo = pointer;
        if (guard == nullptr || combo == nullptr || !combo->isVisibleTo(root) ||
            !combo->isEnabled() || combo->isEditable() ||
            clock.hasExpired(milliseconds))
            continue;
        if (avoided(combo->accessibleName()))
            continue;
        const int original = combo->currentIndex();
        for (int row = 0; row < combo->count() && row < 6; ++row)
        {
            if (row == original || guard == nullptr)
                continue;
            try
            {
                choose(combo, combo->itemText(row));
                ++used;
            }
            catch (const vss_test::failure&)
            {
                ++skipped;
            }
        }
        if (original >= 0 && original < combo->count() &&
            combo->currentIndex() != original)
            choose(combo, combo->itemText(original));
    }
    if (skipped > 0 && qEnvironmentVariableIsSet("VSS_UI_TRACE"))
        std::printf("        exercise: %d control(s) could not be reached\n",
                    skipped);
    return used;
}

// ---- Waiting -------------------------------------------------------------

void wait_for(const char* what, const std::function<bool()>& condition,
              int timeout_ms)
{
    if (!vss_test::wait_until(condition, timeout_ms))
        throw vss_test::failure(std::string("timed out waiting for ") + what);
}

void deliver_traffic(Session& session, vss::endpoint_id port,
                     const QByteArray& bytes, vss::data_direction direction)
{
    gui_event event;
    event.kind = vss::event_kind::data;
    event.endpoint = port;
    if (const auto view = session.backend().port(port))
        event.name = view->name;
    event.direction = direction;
    event.data = bytes;
    event.when = vss::clock_type::now();
    event_batch batch;
    batch.append(event);
    Probe::instance().beat();
    Q_EMIT session.backend().batch_ready(batch);
    QApplication::processEvents();
    Probe::instance().beat();
}

// ---- Topology helpers ----------------------------------------------------

vss::endpoint_id port_id(StudioBackend& backend, const QString& name)
{
    for (const auto& port : backend.ports())
    {
        if (port.name == name)
            return port.id;
    }
    throw vss_test::failure("no port named " + name.toStdString());
}

port_view port_of(StudioBackend& backend, vss::endpoint_id id)
{
    if (const auto found = backend.port(id))
        return *found;
    throw vss_test::failure("no port with id " + std::to_string(id));
}

std::vector<QString> port_names(StudioBackend& backend)
{
    std::vector<QString> names;
    for (const auto& port : backend.ports())
        names.push_back(port.name);
    return names;
}

QPoint node_point(topology::TopologyView* view, vss::endpoint_id id)
{
    return view->mapFromScene(view->node_rect(id).center());
}

QPoint link_point(topology::TopologyView* view, vss::link_id id)
{
    return view->mapFromScene(view->link_anchor(id));
}

} // namespace vss::gui::ui
