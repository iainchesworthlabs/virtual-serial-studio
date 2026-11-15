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
#include "dialogs/templates_dialog.hpp"
#include "qt_wait.hpp"
#include "widgets/dialogs.hpp"

#include <vss/config/document.hpp>
#include <vss/studio/studio.hpp>

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QLabel>
#include <QSettings>
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

/// Answers the next file dialog by choosing `path` and pressing `confirm`.
void drive_file_dialog(Session& s, const QString& path, const char* confirm)
{
    Probe::instance().tolerate(true);
    s.pilot().expect(
        "the file dialog",
        [](QWidget* d)
        {
            return d->findChild<QLineEdit*>(QStringLiteral("fileNameEdit")) !=
                   nullptr;
        },
        [path, confirm](QWidget* d)
        {
            // Qt's own dialog, driven through its interface: on macOS the
            // typed name and the click on its button do not confirm it
            // reliably, and what is under test is the window's use of the
            // file, not the dialog.
            auto* chooser = qobject_cast<QFileDialog*>(d);
            REQUIRE(chooser != nullptr);
            chooser->selectFile(path);
            QPushButton* ok = button(d, QString::fromLatin1(confirm));
            // The dialog lists the folder in the background, and its button
            // stays disabled until it knows the file.
            REQUIRE(
                vss_test::wait_until([ok] { return ok->isEnabled(); }, 5000));
            static_cast<QDialog*>(chooser)->accept();
        });
}

/// Waits for the window to finish giving the engine the setup it was asked to
/// open, which it does on a worker thread with the menu locked.
void wait_until_applied(Session& s)
{
    wait_for("the window to finish opening the setup",
             [&s] { return !s.window().applying(); });
    s.settle();
}

/// True when a label of the dialog has this text in it.
bool has_label_text(QWidget* dialog, const QString& text)
{
    for (const auto* label : dialog->findChildren<QLabel*>())
    {
        if (label->text().contains(text))
            return true;
    }
    return false;
}

/// Chooses the example "Test bench, no hardware" in the next templates dialog.
void use_the_example(Session& s)
{
    s.pilot().expect_type<TemplatesDialog>(
        "the examples",
        [](QWidget* d)
        {
            click(accessible(d, QStringLiteral("Test bench, no hardware")));
            click(button(d, QStringLiteral("Use template")));
        });
}

/// The names of the ports that run, in order.
QStringList port_names(Session& s)
{
    QStringList names;
    for (const auto& port : s.backend().ports())
        names << port.name;
    names.sort();
    return names;
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
    { drive_file_dialog(s, path, confirm); };
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

TEST_CASE("engine: opening a setup replaces the one that runs")
{
    EngineRig rig;
    Session s("engine: opening a setup replaces the one that runs",
              *rig.backend, Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Fixture", "b");
    ad::wire(s, a, b);

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString first = directory.filePath(QStringLiteral("first.json"));
    const QString second = directory.filePath(QStringLiteral("second.json"));

    // A second setup written by hand: two other ports and a link.
    vss::config::document other;
    other.name = "second";
    for (const char* name : {"c", "d"})
    {
        vss::config::port_entry port;
        port.name = name;
        port.kind = "virtual";
        port.virtual_kind = "loopback";
        other.ports.push_back(port);
    }
    vss::config::link_entry link;
    link.mode = "duplex";
    link.sources = {"c"};
    link.targets = {"d"};
    other.links.push_back(link);
    REQUIRE(vss::config::save_file(second.toStdString(), other));

    s.step(QStringLiteral("save the running setup as first.json"));
    drive_file_dialog(s, first, "Save");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Save as"));
    s.settle();

    s.step(QStringLiteral("open first.json while it runs"));
    drive_file_dialog(s, first, "Open");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Open"));
    wait_until_applied(s);
    // The ports are the file's, not those and a second set of them, and the
    // ones that did not change are the same ports.
    REQUIRE_EQ(port_names(s), (QStringList{"a", "b"}));
    REQUIRE_EQ(s.backend().links().size(), 1);
    REQUIRE(s.backend().port(a).has_value());
    REQUIRE(s.backend().port(b).has_value());

    s.step(QStringLiteral("open second.json while first.json runs"));
    drive_file_dialog(s, second, "Open");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Open"));
    wait_until_applied(s);
    REQUIRE_EQ(port_names(s), (QStringList{"c", "d"}));
    REQUIRE_EQ(s.backend().links().size(), 1);
    ad::require_consistent(s);

    s.step(QStringLiteral("open first.json again"));
    drive_file_dialog(s, first, "Open");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Open"));
    wait_until_applied(s);
    Probe::instance().tolerate(false);
    s.pilot().check();
    REQUIRE_EQ(port_names(s), (QStringList{"a", "b"}));
    REQUIRE_EQ(s.backend().links().size(), 1);

    // Nothing was added to the file by opening the other one.
    const auto saved = vss::config::load_file(first.toStdString());
    REQUIRE(saved.has_value());
    REQUIRE_EQ(saved->ports.size(), std::size_t{2});
    s.finish();
}

TEST_CASE("open: a failed open that lost a port marks the setup changed")
{
    Session s("open: a failed open that lost a port marks the setup changed",
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Loopback", "b");
    ad::wire(s, a, b);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("lab.json"));

    s.step(QStringLiteral("save the setup, so the title has no unsaved mark"));
    drive_file_dialog(s, path, "Save");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Save as"));
    s.settle();
    REQUIRE(!s.window().windowTitle().contains(QLatin1Char('*')));

    s.step(QStringLiteral("open it again, but the engine fails and loses a"));
    s.fake().lose_port_on_failed_replace(QStringLiteral("a"));
    s.fake().fail_next(vss::make_error_code(vss::errc::already_exists));
    s.pilot().expect_type<ErrorDialog>(
        "the error",
        [](QWidget* dialog)
        {
            // Not everything came back, and the dialog says so as its own
            // sentence, not glued to the engine's text.
            REQUIRE(has_label_text(
                dialog,
                QStringLiteral("Not everything that was running could be put "
                               "back, so the setup is marked as changed.")));
            REQUIRE(!has_label_text(dialog, QStringLiteral("already exists")));
            static_cast<QDialog*>(dialog)->accept();
        });
    drive_file_dialog(s, path, "Open");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Open"));
    wait_until_applied(s);
    Probe::instance().tolerate(false);
    s.pilot().check();

    // What runs is no longer what the file says, and the title says so.
    REQUIRE_EQ(s.backend().ports().size(), 1);
    REQUIRE(s.window().windowTitle().contains(QLatin1Char('*')));
    s.finish();
}

TEST_CASE("open: a failed open that changed nothing leaves the title alone")
{
    Session s("open: a failed open that changed nothing leaves the title "
              "alone",
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Loopback", "b");
    ad::wire(s, a, b);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("lab.json"));
    drive_file_dialog(s, path, "Save");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Save as"));
    s.settle();
    REQUIRE(!s.window().windowTitle().contains(QLatin1Char('*')));

    s.fake().fail_next(vss::make_error_code(vss::errc::already_exists));
    s.pilot().expect_type<ErrorDialog>("the error", [](QWidget* d)
                                       { static_cast<QDialog*>(d)->accept(); });
    drive_file_dialog(s, path, "Open");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Open"));
    wait_until_applied(s);
    Probe::instance().tolerate(false);
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    REQUIRE(!s.window().windowTitle().contains(QLatin1Char('*')));
    s.finish();
}

TEST_CASE("open: a failed open that put everything back keeps the title clean")
{
    Session s("open: a failed open that put everything back keeps the title "
              "clean",
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Loopback", "b");
    const auto c = make(s, "Loopback", "c");
    ad::wire(s, a, b);
    ad::wire(s, b, c);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("lab.json"));
    drive_file_dialog(s, path, "Save");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Save as"));
    s.settle();
    REQUIRE(!s.window().windowTitle().contains(QLatin1Char('*')));

    // The open fails and the engine puts its first link back, which gives it
    // a new id and a later place.  The setup is the same one, so the title
    // keeps no unsaved mark and the dialog does not say anything was lost.
    s.fake().remake_link_on_failed_replace();
    s.fake().fail_next(vss::make_error_code(vss::errc::already_exists));
    s.pilot().expect_type<ErrorDialog>(
        "the error",
        [](QWidget* d)
        {
            REQUIRE(!has_label_text(d, QStringLiteral("could be put back")));
            static_cast<QDialog*>(d)->accept();
        });
    drive_file_dialog(s, path, "Open");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Open"));
    wait_until_applied(s);
    Probe::instance().tolerate(false);
    s.pilot().check();
    REQUIRE_EQ(s.backend().links().size(), 2);
    REQUIRE(!s.window().windowTitle().contains(QLatin1Char('*')));
    s.finish();
}

TEST_CASE("open: a template that fails with nothing touched says nothing was "
          "changed")
{
    Session s("open: a template that fails with nothing touched says nothing "
              "was changed",
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Loopback", "b");
    ad::wire(s, a, b);
    QSettings().setValue(QStringLiteral("ui/askOnDiscard"), false);

    s.fake().fail_next(vss::make_error_code(vss::errc::already_exists));
    use_the_example(s);
    s.pilot().expect_type<ErrorDialog>(
        "the error",
        [](QWidget* d)
        {
            REQUIRE(has_label_text(d, QStringLiteral("Nothing was changed")));
            static_cast<QDialog*>(d)->accept();
        });
    Probe::instance().tolerate(true);
    choose_menu(s, QStringLiteral("File"),
                QStringLiteral("Setups and templates"));
    wait_until_applied(s);
    Probe::instance().tolerate(false);
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    s.finish();
}

TEST_CASE("open: a template that fails after ports were made again does not "
          "say nothing was changed")
{
    Session s("open: a template that fails after ports were made again does "
              "not say nothing was changed",
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Loopback", "b");
    ad::wire(s, a, b);
    QSettings().setValue(QStringLiteral("ui/askOnDiscard"), false);

    // The engine puts its link back with a new id: the setup is the same one,
    // but it was not left alone.
    s.fake().remake_link_on_failed_replace();
    s.fake().fail_next(vss::make_error_code(vss::errc::already_exists));
    use_the_example(s);
    s.pilot().expect_type<ErrorDialog>(
        "the error",
        [](QWidget* d)
        {
            REQUIRE(has_label_text(d, QStringLiteral("Check the device")));
            REQUIRE(!has_label_text(d, QStringLiteral("Nothing was changed")));
            REQUIRE(!has_label_text(d, QStringLiteral("could be put back")));
            static_cast<QDialog*>(d)->accept();
        });
    Probe::instance().tolerate(true);
    choose_menu(s, QStringLiteral("File"),
                QStringLiteral("Setups and templates"));
    wait_until_applied(s);
    Probe::instance().tolerate(false);
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    s.finish();
}
