//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The same kind of journeys on the real engine: the window sits on
// `EngineStudioBackend` over a real `vss::studio`, so the hub threads, the
// event bridge and the GUI thread all take part. Only in-process port kinds
// are used (loopback and mock fixtures, and the loopback `auto` ports fall
// back to), so the group needs no driver, no device and no privileges.
// This is where a freeze after linking two ports shows up: the probe fails
// the journey and names the step.
//

#include "advanced_steps.hpp"
#include "simple_steps.hpp"

#include "backend/engine_studio_backend.hpp"
#include "qt_wait.hpp"

#include <vss/studio/studio.hpp>

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QSpinBox>
#include <QTableView>
#include <QTemporaryDir>

#include <memory>

namespace
{

using namespace vss::gui;
using namespace vss::gui::ui;
namespace ad = vss::gui::ui::adv;
namespace st = vss::gui::ui::steps;
using vss::gui::simple::job;

/// A real studio and the backend over it. It is declared before the session
/// in each journey so it outlives the window, as in the program.
struct EngineRig
{
    EngineRig()
        : studio(std::make_unique<vss::studio>(vss::studio_options{})),
          backend(std::make_unique<EngineStudioBackend>(*studio))
    {
    }

    std::unique_ptr<vss::studio> studio;
    std::unique_ptr<EngineStudioBackend> backend;
};

vss::endpoint_id make(Session& s, const char* tile, const char* name)
{
    return ad::add_named(s, QString::fromLatin1(tile),
                         QString::fromLatin1(name));
}

/// The traffic table of the Monitor dock.
QTableView* traffic(Session& s)
{
    auto* table = s.dock("monitorDock")->findChild<QTableView*>();
    if (table == nullptr)
        throw vss_test::failure("the Monitor has no traffic table");
    return table;
}

void send_from_dock(Session& s, const QString& text)
{
    QWidget* dock = s.dock("sendDock");
    auto* input = qobject_cast<QLineEdit*>(
        accessible(dock, QStringLiteral("Bytes to send")));
    type_into(input, text);
    click(button(dock, QStringLiteral("Send")));
}

} // namespace

TEST_CASE("engine: link a loopback to a fixture and keep working")
{
    EngineRig rig;
    Session s("engine: link a loopback to a fixture and keep working",
              *rig.backend, Session::mode::advanced);
    s.step(QStringLiteral("add a loopback"));
    const auto loop = make(s, "Loopback", "loop");
    s.step(QStringLiteral("add a fixture"));
    const auto fixture = make(s, "Fixture", "fx");
    s.step(QStringLiteral("link the loopback to the fixture"));
    ad::wire(s, loop, fixture);
    REQUIRE_EQ(s.backend().links().size(), 1);
    s.step(QStringLiteral("the window keeps answering for a second"));
    for (int tick = 0; tick < 20; ++tick)
    {
        vss_test::spin(50);
        Probe::instance().beat();
    }
    ad::require_consistent(s);
    s.step(QStringLiteral("select each node in turn"));
    ad::click_node(s, loop);
    ad::click_node(s, fixture);
    s.step(QStringLiteral("send through the link"));
    ad::click_node(s, loop);
    send_from_dock(s, QStringLiteral("hello"));
    wait_for("the traffic to show in the Monitor",
             [&] { return traffic(s)->model()->rowCount() >= 1; });
    s.step(QStringLiteral("remove the link and the ports"));
    ad::clear_canvas(s);
    REQUIRE(s.backend().ports().empty());
    s.finish();
}

TEST_CASE("engine: bytes cross a link and are counted")
{
    EngineRig rig;
    Session s("engine: bytes cross a link and are counted", *rig.backend,
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Loopback", "b");
    ad::wire(s, a, b);
    s.step(QStringLiteral("send from a"));
    ad::click_node(s, a);
    send_from_dock(s, QStringLiteral("0123456789"));
    wait_for("both ends to show traffic",
             [&]
             {
                 const auto first = s.backend().port(a);
                 const auto second = s.backend().port(b);
                 return first && second && first->stats.bytes_tx >= 10 &&
                        second->stats.bytes_rx >= 10;
             });
    wait_for("rows in the Monitor",
             [&] { return traffic(s)->model()->rowCount() >= 2; });
    s.finish();
}

TEST_CASE("engine: a repeat keeps the window responsive")
{
    EngineRig rig;
    Session s("engine: a repeat keeps the window responsive", *rig.backend,
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Fixture", "b");
    ad::wire(s, a, b);
    QWidget* dock = s.dock("sendDock");
    ad::click_node(s, a);
    auto* input = qobject_cast<QLineEdit*>(
        accessible(dock, QStringLiteral("Bytes to send")));
    type_into(input, QStringLiteral("tick"));
    // The fastest repeat the dock allows is 50 ms. A debug build cannot take
    // that rate from the real engine (each refresh costs it about a second of
    // the GUI thread), and the events then queue up faster than they run.
    // VSS_UI_BUDGET_SCALE is 1 for a release build.
    const bool fast_build =
        qEnvironmentVariableIntValue("VSS_UI_BUDGET_SCALE") <= 1;
    const QString interval_ms =
        fast_build ? QStringLiteral("50") : QStringLiteral("250");
    s.step(QStringLiteral("repeat every ") + interval_ms +
           QStringLiteral(" ms for a second and a half"));
    click(checkbox(dock, QStringLiteral("Repeat")));
    auto* interval = qobject_cast<QSpinBox*>(
        accessible(dock, QStringLiteral("Repeat interval")));
    type_into(interval->findChild<QLineEdit*>(), interval_ms);
    key(interval, Qt::Key_Tab);
    QElapsedTimer running;
    running.start();
    while (!running.hasExpired(1500))
    {
        vss_test::spin(50);
        Probe::instance().beat();
    }
    s.step(QStringLiteral("stop the repeat and remove everything"));
    click(button(dock, QStringLiteral("Stop repeat")));
    ad::clear_canvas(s);
    s.finish();
}

TEST_CASE("engine: Simple mode builds and removes a pair")
{
    EngineRig rig;
    if (rig.backend->caps().com_numbers)
        VSS_SKIP("the Studio driver is installed: Simple mode would make real "
                 "COM ports on this machine");
    Session s("engine: Simple mode builds and removes a pair", *rig.backend);
    s.step(QStringLiteral("connect two programs"));
    st::open_job(s, job::pair);
    st::create(s);
    st::require_counts(s, {2, 1});
    s.step(QStringLiteral("look at it in Advanced mode and select a node"));
    st::verify_in_advanced(s);
    s.step(QStringLiteral("remove the setup"));
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("engine: save, tear down and reopen a workspace")
{
    EngineRig rig;
    Session s("engine: save, tear down and reopen a workspace", *rig.backend,
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Fixture", "b");
    ad::wire(s, a, b);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("engine.json"));
    const auto file_dialog = [&](const char* confirm)
    {
        Probe::instance().tolerate(true);
        s.pilot().expect(
            "the file dialog",
            [](QWidget* d)
            {
                return d->findChild<QLineEdit*>(
                           QStringLiteral("fileNameEdit")) != nullptr;
            },
            [path, confirm](QWidget* d)
            {
                // Qt's own dialog, driven through its interface: on macOS
                // the typed name and the click on its button do not confirm
                // it reliably, and what is under test is the window's use of
                // the file, not the dialog.
                auto* chooser = qobject_cast<QFileDialog*>(d);
                REQUIRE(chooser != nullptr);
                chooser->selectFile(path);
                QPushButton* ok = button(d, QString::fromLatin1(confirm));
                // The dialog lists the folder in the background, and its
                // button stays disabled until it knows the file.
                REQUIRE(vss_test::wait_until([ok] { return ok->isEnabled(); },
                                             5000));
                static_cast<QDialog*>(chooser)->accept();
            });
    };
    file_dialog("Save");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Save as"));
    s.settle();
    choose_menu(s, QStringLiteral("File"), QStringLiteral("New"));
    s.settle();
    REQUIRE(s.backend().ports().empty());
    file_dialog("Open");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Open"));
    s.settle();
    Probe::instance().tolerate(false);
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    REQUIRE_EQ(s.backend().links().size(), 1);
    ad::require_consistent(s);
    s.finish();
}
