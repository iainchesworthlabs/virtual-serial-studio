//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Advanced mode, by hand on the canvas: the toolbox, the Add sheet, wiring,
// moving, the context menus, the Send and Monitor docks, undo and the
// workspace files.
//

#include "advanced_steps.hpp"

#include "advanced/port_settings_dialog.hpp"
#include "advanced/wiring_dialog.hpp"
#include "dialogs/connect_ports.hpp"
#include "dialogs/network_endpoint.hpp"
#include "dialogs/share_wizard.hpp"
#include "flows/attach_dialog.hpp"
#include "flows/port_ready_dialog.hpp"
#include "flows/rename_dialog.hpp"
#include "qt_wait.hpp"
#include "simple/setup_kinds.hpp"
#include "simple/simple_page.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QSpinBox>
#include <QTableView>
#include <QTemporaryDir>

namespace
{

using namespace vss::gui;
using namespace vss::gui::ui;
namespace ad = vss::gui::ui::adv;

vss::endpoint_id make(Session& s, const char* tile, const char* name)
{
    return ad::add_named(s, QString::fromLatin1(tile),
                         QString::fromLatin1(name));
}

QPoint empty_spot(Session& s)
{
    auto* view = s.canvas();
    return QPoint(view->viewport()->width() - 40,
                  view->viewport()->height() - 40);
}

} // namespace

TEST_CASE("advanced: the toolbox offers every tile")
{
    Session s("advanced: the toolbox offers every tile",
              Session::mode::advanced);
    s.step(QStringLiteral("read the toolbox"));
    REQUIRE_EQ(s.toolbox()->tile_labels().join(QStringLiteral(" | ")),
               QStringLiteral("Port | Fixture | Loopback | Device | Network | "
                              "Pair | Split | Join | Bus | Switch | Redirect | "
                              "Monitor | Share | Bench"));
    REQUIRE(s.canvas()->isVisible());
    REQUIRE(s.canvas()->is_editing());
    s.finish();
}

TEST_CASE("advanced: add a port, a fixture and a loopback")
{
    Session s("advanced: add a port, a fixture and a loopback",
              Session::mode::advanced);
    s.step(QStringLiteral("add a port"));
    const auto port = make(s, "Port", "console");
    s.step(QStringLiteral("add a fixture"));
    const auto fixture = make(s, "Fixture", "modem");
    s.step(QStringLiteral("add a loopback"));
    const auto loopback = make(s, "Loopback", "echo");
    REQUIRE_EQ(port_of(s.backend(), port).kind,
               vss::endpoint_kind::virtual_port);
    REQUIRE_EQ(port_of(s.backend(), fixture).kind, vss::endpoint_kind::mock);
    REQUIRE(port_of(s.backend(), loopback).studio_only);
    ad::require_consistent(s);
    s.step(QStringLiteral("each is a node on the canvas that can be selected"));
    for (const auto id : {port, fixture, loopback})
    {
        ad::click_node(s, id);
        REQUIRE_EQ(s.canvas()->selected_port(), id);
    }
    s.finish();
}

TEST_CASE("advanced: add every bundle from the toolbox")
{
    Session s("advanced: add every bundle from the toolbox",
              Session::mode::advanced);
    // What each tile builds with its default sheet: the ports it makes, the
    // links, and the shape of the first link.
    struct bundle
    {
        const char* label;
        std::size_t ports;
        std::size_t links;
        link_shape shape;
    };
    const bundle bundles[] = {
        {"Pair", 2, 1, link_shape::two_way},
        {"Split", 3, 1, link_shape::split},
        {"Join", 3, 1, link_shape::join},
        {"Bus", 3, 1, link_shape::bus},
        {"Switch", 3, 1, link_shape::join},
        {"Redirect", 2, 1, link_shape::one_way},
        {"Monitor", 2, 1, link_shape::monitor},
        {"Bench", 4, 2, link_shape::two_way},
    };
    for (const bundle& item : bundles)
    {
        s.step(QStringLiteral("add a ") + QString::fromLatin1(item.label));
        ad::expect_add_sheet(s);
        ad::add_tile(s, QString::fromLatin1(item.label));
        s.pilot().check();
        REQUIRE_EQ(s.backend().ports().size(), item.ports);
        REQUIRE_EQ(s.backend().links().size(), item.links);
        REQUIRE(s.backend().links().front().shape == item.shape);
        ad::require_consistent(s);
        s.step(QStringLiteral("clear the canvas"));
        ad::clear_canvas(s);
        if (!s.backend().ports().empty())
            throw vss_test::failure(
                "the canvas was not cleared: " +
                std::to_string(s.backend().ports().size()) + " ports left, " +
                std::to_string(s.canvas()->selected_ports().size()) +
                " selected; the status says: " + status_text(s).toStdString());
    }
    s.finish();
}

TEST_CASE("advanced: add by dropping a tile and from the canvas menu")
{
    Session s("advanced: add by dropping a tile and from the canvas menu",
              Session::mode::advanced);
    s.step(QStringLiteral("drag the Port tile onto the canvas"));
    ad::expect_add_sheet(s);
    ad::drop_tile(s, canvas_item::port, QPoint(300, 200));
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 1);
    s.step(QStringLiteral("right-click empty canvas, choose Add fixture"));
    ad::expect_add_sheet(s);
    QMenu* menu = open_context_menu(s.canvas()->viewport(), empty_spot(s));
    REQUIRE(menu != nullptr);
    REQUIRE(menu_has(menu, QStringLiteral("Add port")));
    REQUIRE(menu_has(menu, QStringLiteral("Add loopback")));
    REQUIRE(menu_has(menu, QStringLiteral("Add bundle")));
    choose_entry(menu, QStringLiteral("Add fixture"));
    s.settle();
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    s.step(QStringLiteral("choose Add bundle > Pair"));
    ad::expect_add_sheet(s);
    menu = open_context_menu(s.canvas()->viewport(), empty_spot(s));
    REQUIRE(menu != nullptr);
    QMenu* bundles = open_submenu(menu, QStringLiteral("Add bundle"));
    choose_entry(bundles, QStringLiteral("Pair"));
    s.settle();
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 4);
    ad::require_consistent(s);
    s.finish();
}

TEST_CASE("advanced: link and unlink ports by dragging a wire")
{
    Session s("advanced: link and unlink ports by dragging a wire",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Port", "b");
    const auto c = make(s, "Port", "c");
    s.step(QStringLiteral("drag a wire from a to b: two-way"));
    ad::wire(s, a, b, wire_kind::two_way);
    REQUIRE_EQ(s.backend().links().size(), 1);
    REQUIRE_EQ(s.backend().links().front().shape, link_shape::two_way);
    s.step(QStringLiteral("drag a wire from b to c: one way"));
    ad::wire(s, b, c, wire_kind::one_way);
    REQUIRE_EQ(s.backend().links().size(), 2);
    s.step(QStringLiteral("drag a wire from c back to a: it would loop"));
    REQUIRE(ad::wire_refused(s, c, a));
    REQUIRE_EQ(s.backend().links().size(), 2);
    s.step(QStringLiteral("select the second link and press Delete"));
    const vss::link_id second = s.backend().links().back().id;
    const auto segments = s.canvas()->link_segments(second);
    REQUIRE(!segments.isEmpty());
    const QPoint on_line =
        s.canvas()->mapFromScene(segments.front().pointAt(0.5));
    REQUIRE(s.canvas()->itemAt(on_line) != nullptr);
    click_at(s.canvas()->viewport(), on_line);
    REQUIRE_EQ(s.canvas()->selected_link(), second);
    ad::delete_selection(s);
    REQUIRE_EQ(s.backend().links().size(), 1);
    ad::require_consistent(s);
    s.step(QStringLiteral("a monitor link from a to c"));
    ad::wire(s, a, c, wire_kind::monitor);
    REQUIRE_EQ(s.backend().links().size(), 2);
    s.finish();
}

TEST_CASE("advanced: link selected ports from the selection menu")
{
    Session s("advanced: link selected ports from the selection menu",
              Session::mode::advanced);
    const auto hub = make(s, "Port", "hub");
    const auto x = make(s, "Port", "x");
    const auto y = make(s, "Port", "y");
    s.step(QStringLiteral("select three ports and split from the hub"));
    ad::select_ports(s, {x, y, hub});
    QMenu* menu =
        open_context_menu(s.canvas()->viewport(), node_point(s.canvas(), hub));
    REQUIRE(menu != nullptr);
    QMenu* sub = open_submenu(menu, QStringLiteral("Link selected"));
    choose_entry(sub, QStringLiteral("Split from hub"));
    s.settle();
    if (s.backend().links().size() != 1)
        throw vss_test::failure("Split made no link; the status says: " +
                                status_text(s).toStdString());
    REQUIRE_EQ(s.backend().links().front().shape, link_shape::split);
    ad::require_consistent(s);
    s.finish();
}

TEST_CASE("advanced: move nodes by dragging and with the keyboard")
{
    Session s("advanced: move nodes by dragging and with the keyboard",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Port", "b");
    ad::wire(s, a, b);
    auto* view = s.canvas();
    s.step(QStringLiteral("drag node a down and to the right"));
    const QPointF before = view->node_rect(a).topLeft();
    const QPoint from = node_point(view, a);
    drag(view->viewport(), from, from + QPoint(60, 80));
    s.settle();
    const QPointF after = view->node_rect(a).topLeft();
    REQUIRE(after != before);
    REQUIRE(view->is_arranged());
    s.step(QStringLiteral("focus node b and nudge it with the arrow keys"));
    ad::click_node(s, b);
    const QPointF b_before = view->node_rect(b).topLeft();
    key(view, Qt::Key_Right);
    key(view, Qt::Key_Down, Qt::ShiftModifier);
    s.settle();
    REQUIRE(view->node_rect(b).topLeft() != b_before);
    s.step(QStringLiteral("Fit with Ctrl+0"));
    key(view, Qt::Key_0, Qt::ControlModifier);
    REQUIRE(view->is_auto_fit());
    s.step(QStringLiteral("zoom in and out with the keyboard"));
    const qreal zoom = view->zoom();
    key(view, Qt::Key_Plus, Qt::ControlModifier);
    REQUIRE(view->zoom() > zoom);
    key(view, Qt::Key_Minus, Qt::ControlModifier);
    s.step(QStringLiteral("Tidy layout from the canvas menu"));
    QMenu* menu = open_context_menu(view->viewport(), empty_spot(s));
    REQUIRE(menu != nullptr);
    choose_entry(menu, QStringLiteral("Tidy layout"));
    s.settle();
    REQUIRE(!view->is_arranged());
    s.step(QStringLiteral("Fit from the canvas menu"));
    menu = open_context_menu(view->viewport(), empty_spot(s));
    REQUIRE(menu != nullptr);
    choose_entry(menu, QStringLiteral("Fit"));
    s.finish();
}

TEST_CASE("advanced: the right-click menu of a node and of a link")
{
    Session s("advanced: the right-click menu of a node and of a link",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Fixture", "b");
    ad::wire(s, a, b, wire_kind::one_way);
    auto* view = s.canvas();
    s.step(QStringLiteral("right-click node a"));
    QMenu* menu = open_context_menu(view->viewport(), node_point(view, a));
    REQUIRE(menu != nullptr);
    const QStringList labels = menu_labels(menu);
    for (const char* wanted : {"Settings...", "Duplicate", "Delete"})
        REQUIRE(labels.contains(QString::fromLatin1(wanted)));
    close_menu(menu);
    s.step(QStringLiteral("right-click the link"));
    menu = open_context_menu(view->viewport(),
                             link_point(view, s.backend().links().front().id));
    REQUIRE(menu != nullptr);
    require_entry(menu, QStringLiteral("Reverse direction"));
    require_entry(menu, QStringLiteral("Delete"));
    s.step(QStringLiteral("choose Reverse direction"));
    choose_entry(menu, QStringLiteral("Reverse direction"));
    s.settle();
    REQUIRE_EQ(s.backend().links().size(), 1);
    s.step(QStringLiteral("right-click node a, Duplicate"));
    menu = open_context_menu(view->viewport(), node_point(view, a));
    REQUIRE(menu != nullptr);
    choose_entry(menu, QStringLiteral("Duplicate"));
    s.settle();
    REQUIRE_EQ(s.backend().ports().size(), 3);
    s.step(QStringLiteral("right-click node a, Delete"));
    menu = open_context_menu(view->viewport(), node_point(view, a));
    REQUIRE(menu != nullptr);
    s.pilot().expect(
        "confirm Delete",
        [](QWidget* d) { return has_button(d, QStringLiteral("Delete")); },
        [](QWidget* d) { click(button(d, QStringLiteral("Delete"))); });
    choose_entry(menu, QStringLiteral("Delete"));
    s.settle();
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    ad::require_consistent(s);
    s.finish();
}

TEST_CASE("advanced: send once and repeat from the Send dock")
{
    Session s("advanced: send once and repeat from the Send dock",
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    const auto b = make(s, "Fixture", "b");
    ad::wire(s, a, b);
    QWidget* dock = s.dock("sendDock");
    s.step(QStringLiteral("select a, type hello and click Send"));
    ad::click_node(s, a);
    auto* input = qobject_cast<QLineEdit*>(
        accessible(dock, QStringLiteral("Bytes to send")));
    REQUIRE(input != nullptr);
    type_into(input, QStringLiteral("hello"));
    click(button(dock, QStringLiteral("Send")));
    s.settle();
    REQUIRE_EQ(s.fake().sent().size(), 1);
    REQUIRE(s.fake().sent().front().startsWith("hello"));
    REQUIRE_EQ(s.fake().sent_to().front(), a);

    s.step(QStringLiteral("tick Repeat: it keeps sending from a"));
    click(checkbox(dock, QStringLiteral("Repeat")));
    auto* interval = qobject_cast<QSpinBox*>(
        accessible(dock, QStringLiteral("Repeat interval")));
    REQUIRE(interval != nullptr);
    type_into(interval->findChild<QLineEdit*>(), QStringLiteral("50"));
    key(interval, Qt::Key_Tab);
    wait_for("the repeat to send", [&] { return s.fake().sent().size() >= 4; });

    s.step(QStringLiteral("select b while the repeat runs"));
    ad::click_node(s, b);
    const std::size_t before = s.fake().sent().size();
    wait_for("more repeats",
             [&] { return s.fake().sent().size() >= before + 3; });
    for (std::size_t index = before; index < s.fake().sent_to().size(); ++index)
        REQUIRE_EQ(s.fake().sent_to()[index], a);

    s.step(QStringLiteral("Stop repeat"));
    click(button(dock, QStringLiteral("Stop repeat")));
    s.settle();
    const std::size_t stopped = s.fake().sent().size();
    vss_test::spin(250);
    REQUIRE_EQ(s.fake().sent().size(), stopped);
    s.finish();
}

TEST_CASE("advanced: the Monitor shows traffic and can pause and clear")
{
    Session s("advanced: the Monitor shows traffic and can pause and clear",
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    QWidget* send = s.dock("sendDock");
    QWidget* monitor = s.dock("monitorDock");
    ad::click_node(s, a);
    auto* input = qobject_cast<QLineEdit*>(
        accessible(send, QStringLiteral("Bytes to send")));
    type_into(input, QStringLiteral("ping"));
    click(button(send, QStringLiteral("Send")));
    s.settle();
    deliver_traffic(s, a, QByteArray("ping"));
    auto* table = monitor->findChild<QTableView*>();
    REQUIRE(table != nullptr);
    wait_for("the traffic row",
             [&] { return table->model()->rowCount() >= 1; });
    s.step(QStringLiteral("Pause, send again: no new row"));
    click(button(monitor, QStringLiteral("Pause")));
    const int rows = table->model()->rowCount();
    click(button(send, QStringLiteral("Send")));
    s.settle();
    deliver_traffic(s, a, QByteArray("ping"));
    REQUIRE_EQ(table->model()->rowCount(), rows);
    click(button(monitor, QStringLiteral("Resume")));
    s.step(QStringLiteral("Clear empties the table"));
    click(button(monitor, QStringLiteral("Clear")));
    s.settle();
    REQUIRE_EQ(table->model()->rowCount(), 0);
    s.finish();
}

TEST_CASE("advanced: undo and redo from the Edit menu")
{
    Session s("advanced: undo and redo from the Edit menu",
              Session::mode::advanced);
    make(s, "Port", "a");
    const auto b = make(s, "Port", "b");
    s.step(QStringLiteral("Undo takes b away"));
    choose_menu(s, QStringLiteral("Edit"), QStringLiteral("Undo"), true);
    s.settle();
    if (s.backend().ports().size() != 1)
        throw vss_test::failure(
            "Undo left " + std::to_string(s.backend().ports().size()) +
            " ports; the status says: " + status_text(s).toStdString());
    s.step(QStringLiteral("Redo brings it back"));
    choose_menu(s, QStringLiteral("Edit"), QStringLiteral("Redo"), true);
    s.settle();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    REQUIRE(b != 0);
    ad::require_consistent(s);
    s.step(QStringLiteral("undo with the keyboard shortcut"));
    s.window().activateWindow();
    QApplication::processEvents();
    if (QApplication::activeWindow() != &s.window())
        throw vss_test::failure(
            "the window is not the active window: " +
            describe(QApplication::activeWindow()).toStdString());
    key(s.canvas(), Qt::Key_Z, Qt::ControlModifier);
    s.settle();
    REQUIRE_EQ(s.backend().ports().size(), 1);
    s.finish();
}

TEST_CASE("advanced: save a workspace, start over and reopen it")
{
    Session s("advanced: save a workspace, start over and reopen it",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Fixture", "b");
    ad::wire(s, a, b);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("lab.json"));
    const auto file_dialog = [&](const char* confirm)
    {
        // Qt's file dialog lists the drives on the GUI thread, and a network
        // drive can make that wait; it is not the application's own work.
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
    s.step(QStringLiteral("File > Save as"));
    file_dialog("Save");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Save as"));
    s.settle();
    s.pilot().check();
    REQUIRE(QFileInfo::exists(path));
    s.step(QStringLiteral("File > New empties the workspace"));
    choose_menu(s, QStringLiteral("File"), QStringLiteral("New"));
    s.settle();
    REQUIRE(s.backend().ports().empty());
    ad::require_consistent(s);
    s.step(QStringLiteral("File > Open brings the ports and the link back"));
    file_dialog("Open");
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Open"));
    // The ports are made on a worker thread, so wait for them to appear.
    REQUIRE(vss_test::wait_until(
        [&s] { return s.backend().ports().size() == 2; }, 10000));
    s.settle();
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    REQUIRE_EQ(s.backend().links().size(), 1);
    ad::require_consistent(s);
    Probe::instance().tolerate(false);
    s.step(QStringLiteral("File > Save writes to the same file"));
    choose_menu(s, QStringLiteral("File"), QStringLiteral("Save"));
    s.settle();
    s.finish();
}

TEST_CASE("advanced: attach a real device from the Device tile")
{
    Session s("advanced: attach a real device from the Device tile",
              Session::mode::advanced);
    s.fake().set_devices({[]
                          {
                              device_view device;
                              device.path = QStringLiteral("COM7");
                              device.description = QStringLiteral("USB Serial");
                              return device;
                          }()});
    s.pilot().expect_type<AttachDialog>(
        "the Attach dialog",
        [](QWidget* d)
        {
            REQUIRE(has_button(d, QStringLiteral("Attach device")));
            click(button(d, QStringLiteral("Refresh")));
            auto* row = named<QWidget>(d, "attachDeviceRow");
            click(row);
            click(button(d, QStringLiteral("Attach device")));
        });
    s.pilot().expect_type<PortReadyDialog>("the port is ready", [](QWidget* d)
                                           { key(d, Qt::Key_Escape); });
    s.step(QStringLiteral("click the Device tile, choose COM7, Attach device"));
    ad::add_tile(s, QStringLiteral("Device"));
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    REQUIRE_EQ(s.backend().links().size(), 1);
    REQUIRE_EQ(s.backend().ports().front().kind, vss::endpoint_kind::physical);
    ad::require_consistent(s);
    s.finish();
}

TEST_CASE("advanced: put a device on the network from the Network tile")
{
    Session s("advanced: put a device on the network from the Network tile",
              Session::mode::advanced);
    s.fake().set_devices({[]
                          {
                              device_view device;
                              device.path = QStringLiteral("COM7");
                              return device;
                          }()});
    s.pilot().expect_type<NetworkEndpointDialog>(
        "the network dialog",
        [](QWidget* d) { click(button(d, QStringLiteral("Start bridge"))); });
    s.step(QStringLiteral("click the Network tile, Start bridge"));
    ad::add_tile(s, QStringLiteral("Network"));
    s.pilot().check();
    REQUIRE_EQ(s.backend().links().size(), 1);
    ad::require_consistent(s);
    s.finish();
}

TEST_CASE("advanced: share a device with the wizard")
{
    Session s("advanced: share a device with the wizard",
              Session::mode::advanced);
    s.fake().set_devices({[]
                          {
                              device_view device;
                              device.path = QStringLiteral("COM7");
                              return device;
                          }()});
    s.pilot().expect_type<ShareWizard>(
        "the share wizard",
        [](QWidget* d)
        {
            click(button(d, QStringLiteral("Refresh")));
            for (int page = 0; page < 6; ++page)
            {
                if (has_button(d, QStringLiteral("Next")))
                    click(button(d, QStringLiteral("Next")));
                else
                    break;
            }
            // The last page's primary button creates the share; the page that
            // follows says it is ready and has Done.
            click(button(d, QStringLiteral("Share device")));
            wait_for("the Ready page",
                     [d] { return has_button(d, QStringLiteral("Done")); });
            click(button(d, QStringLiteral("Done")));
            return;
        });
    s.step(QStringLiteral("click the Share tile and click through the wizard"));
    ad::add_tile(s, QStringLiteral("Share"));
    s.pilot().check();
    REQUIRE(s.backend().ports().size() >= 3);
    REQUIRE_EQ(s.backend().links().size(), 1);
    ad::require_consistent(s);
    s.finish();
}

TEST_CASE("advanced: connect ports with the Connect dialog")
{
    Session s("advanced: connect ports with the Connect dialog",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    make(s, "Port", "b");
    ad::click_node(s, a);
    s.pilot().expect_type<ConnectPortsDialog>(
        "the connect dialog",
        [](QWidget* d) { click(button(d, QStringLiteral("Connect"))); });
    s.step(QStringLiteral("Ctrl+L, then Connect"));
    key(s.canvas(), Qt::Key_L, Qt::ControlModifier);
    s.settle();
    s.pilot().check();
    REQUIRE_EQ(s.backend().links().size(), 1);
    ad::require_consistent(s);
    s.finish();
}

TEST_CASE("advanced: rename, change the settings of, and wire a port")
{
    Session s("advanced: rename, change the settings of, and wire a port",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Fixture", "b");
    ad::wire(s, a, b);
    auto* view = s.canvas();
    s.step(QStringLiteral("F2 renames the selected port"));
    ad::click_node(s, a);
    s.pilot().expect_type<RenameDialog>(
        "the rename dialog",
        [](QWidget* d)
        {
            auto* field = d->findChild<QLineEdit*>();
            REQUIRE(field != nullptr);
            type_into(field, QStringLiteral("console"));
            click(button(d, QStringLiteral("Rename")));
        });
    key(view, Qt::Key_F2);
    s.settle();
    s.pilot().check();
    REQUIRE_EQ(port_of(s.backend(), a).name, QStringLiteral("console"));
    s.step(QStringLiteral("Settings... from the menu changes the baud rate"));
    const auto baud_before = port_of(s.backend(), a).serial.baud_rate;
    s.pilot().expect_type<PortSettingsDialog>(
        "the port settings",
        [&s, a](QWidget* d)
        {
            QComboBox* baud = nullptr;
            for (auto* combo : d->findChildren<QComboBox*>())
            {
                if (combo->accessibleName() == QStringLiteral("Baud rate"))
                    baud = combo;
            }
            REQUIRE(baud != nullptr);
            // A value other than the one the port has, or Apply stays off.
            const bool at_9600 =
                port_of(s.backend(), a).serial.baud_rate == 9600;
            choose(baud,
                   at_9600 ? QStringLiteral("19200") : QStringLiteral("9600"));
            // Enter in the edited field can already apply it, which turns
            // Apply off while the dialog closes.
            if (d->isVisible() &&
                button(d, QStringLiteral("Apply"))->isEnabled())
                click(button(d, QStringLiteral("Apply")));
        });
    QMenu* menu = open_context_menu(view->viewport(), node_point(view, a));
    REQUIRE(menu != nullptr);
    choose_entry(menu, QStringLiteral("Settings..."));
    s.settle();
    s.pilot().check();
    REQUIRE_NE(port_of(s.backend(), a).serial.baud_rate, baud_before);
    s.step(QStringLiteral("the link wiring dialog, preset Loopback"));
    s.pilot().expect_type<WiringDialog>(
        "the wiring dialog",
        [](QWidget* d) { click(button(d, QStringLiteral("Close"))); });
    s.action("topology.wiring")->trigger();
    s.settle();
    s.pilot().check();
    s.finish();
}

TEST_CASE("advanced: every Simple job can be built by hand and Simple knows it")
{
    using vss::gui::simple::job;
    Session s(
        "advanced: every Simple job can be built by hand and Simple knows it",
        Session::mode::advanced);
    s.fake().set_devices({[]
                          {
                              device_view device;
                              device.path = QStringLiteral("COM7");
                              return device;
                          }()});
    const auto kind_now = [&]
    {
        const auto setups = vss::gui::simple::list_setups(s.backend().ports(),
                                                          s.backend().links());
        if (setups.size() != 1)
        {
            std::string text = "expected one setup, there are " +
                               std::to_string(setups.size()) + "; ports:";
            for (const auto& port : s.backend().ports())
                text += " " + port.name.toStdString();
            text += "; links:";
            for (const auto& link : s.backend().links())
                text += " [shape " +
                        std::to_string(static_cast<int>(link.shape)) + ", " +
                        std::to_string(link.sources.size()) + " in, " +
                        std::to_string(link.targets.size()) + " out]";
            throw vss_test::failure(text);
        }
        return setups.front().kind;
    };
    const auto bundle = [&](const char* tile)
    {
        ad::expect_add_sheet(s);
        ad::add_tile(s, QString::fromLatin1(tile));
        s.pilot().check();
    };
    struct entry
    {
        const char* name;
        job expected;
        std::function<void()> build;
    };
    const std::vector<entry> table = {
        {"a port for a program", job::port,
         [&] { make(s, "Port", "program"); }},
        {"a pair of programs", job::pair, [&] { bundle("Pair"); }},
        {"several on one line", job::bus, [&] { bundle("Bus"); }},
        {"several into one", job::combine, [&] { bundle("Join"); }},
    };
    for (const auto& item : table)
    {
        s.step(QString::fromLatin1(item.name));
        item.build();
        ad::require_consistent(s);
        const job found = kind_now();
        if (found != item.expected)
            throw vss_test::failure(
                std::string(item.name) + " is seen in Simple mode as \"" +
                vss::gui::simple::job_title(found).toStdString() +
                "\", not \"" +
                vss::gui::simple::job_title(item.expected).toStdString() +
                "\"");
        s.step(QStringLiteral("see it in Simple mode, then clear it"));
        s.set_mode(Session::mode::simple);
        REQUIRE_EQ(s.simple_page()->list()->count(), 1);
        s.set_mode(Session::mode::advanced);
        ad::clear_canvas(s);
    }
    s.finish();
}

TEST_CASE("advanced: undo and redo a delete and a link")
{
    Session s("advanced: undo and redo a delete and a link",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Port", "b");
    ad::wire(s, a, b);
    s.step(QStringLiteral("select both ports and Delete"));
    ad::select_ports(s, {a, b});
    ad::delete_selection(s);
    REQUIRE(s.backend().ports().empty());
    s.step(QStringLiteral("Undo restores both ports and the link"));
    choose_menu(s, QStringLiteral("Edit"), QStringLiteral("Undo"), true);
    s.settle();
    if (s.backend().ports().size() != 2)
        throw vss_test::failure(
            "Undo left " + std::to_string(s.backend().ports().size()) +
            " ports; the status says: " + status_text(s).toStdString());
    REQUIRE_EQ(s.backend().links().size(), 1);
    ad::require_consistent(s);
    s.step(QStringLiteral("Redo deletes them again"));
    choose_menu(s, QStringLiteral("Edit"), QStringLiteral("Redo"), true);
    s.settle();
    REQUIRE(s.backend().ports().empty());
    s.step(
        QStringLiteral("Undo twice more brings back the link, then the ports"));
    choose_menu(s, QStringLiteral("Edit"), QStringLiteral("Undo"), true);
    s.settle();
    REQUIRE_EQ(s.backend().ports().size(), 2);
    s.finish();
}
