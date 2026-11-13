//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The awkward cases: cancelled dialogs, empty and duplicate names, many
// items, links between every pair of port types, rapid clicking, resizing,
// large text, both themes and a backend that fails.
//

#include "advanced_steps.hpp"
#include "simple_steps.hpp"

#include "advanced/port_settings_dialog.hpp"
#include "dialogs/connect_ports.hpp"
#include "dialogs/network_endpoint.hpp"
#include "dialogs/share_wizard.hpp"
#include "flows/attach_dialog.hpp"
#include "flows/rename_dialog.hpp"
#include "qt_input.hpp"
#include "qt_wait.hpp"
#include "support/about_dialog.hpp"
#include "support/build_info.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"

#include <QElapsedTimer>
#include <QLabel>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTabWidget>

namespace
{

using namespace vss::gui;
using namespace vss::gui::ui;
namespace ad = vss::gui::ui::adv;
namespace st = vss::gui::ui::steps;
using vss::gui::simple::job;

vss::endpoint_id make(Session& s, const char* tile, const char* name)
{
    return ad::add_named(s, QString::fromLatin1(tile),
                         QString::fromLatin1(name));
}

void cancel_with_escape(QWidget* dialog)
{
    key(dialog, Qt::Key_Escape);
}

} // namespace

TEST_CASE("edge: cancelling every dialog changes nothing")
{
    Session s("edge: cancelling every dialog changes nothing",
              Session::mode::advanced);
    s.fake().set_devices({[]
                          {
                              device_view device;
                              device.path = QStringLiteral("COM7");
                              return device;
                          }()});
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Port", "b");
    const std::size_t ports = s.backend().ports().size();
    const std::size_t links = s.backend().links().size();
    const auto unchanged = [&]
    {
        s.settle();
        s.pilot().check();
        REQUIRE_EQ(s.backend().ports().size(), ports);
        REQUIRE_EQ(s.backend().links().size(), links);
    };

    s.step(QStringLiteral("cancel the Add sheet with its Cancel button"));
    s.pilot().expect_type<topology::AddSheet>(
        "the Add sheet",
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    ad::add_tile(s, QStringLiteral("Port"));
    unchanged();

    s.step(QStringLiteral("cancel the Add sheet with Escape"));
    s.pilot().expect_type<topology::AddSheet>("the Add sheet",
                                              cancel_with_escape);
    ad::add_tile(s, QStringLiteral("Pair"));
    unchanged();

    s.step(QStringLiteral("cancel Attach, Network and Share"));
    s.pilot().expect_type<AttachDialog>(
        "attach",
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    ad::add_tile(s, QStringLiteral("Device"));
    s.pilot().expect_type<NetworkEndpointDialog>(
        "network",
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    ad::add_tile(s, QStringLiteral("Network"));
    s.pilot().expect_type<ShareWizard>(
        "share",
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    ad::add_tile(s, QStringLiteral("Share"));
    unchanged();

    s.step(QStringLiteral("cancel Connect, Rename and Settings"));
    ad::click_node(s, a);
    s.pilot().expect_type<ConnectPortsDialog>(
        "connect",
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    key(s.canvas(), Qt::Key_L, Qt::ControlModifier);
    s.settle();
    s.pilot().expect_type<RenameDialog>(
        "rename",
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    key(s.canvas(), Qt::Key_F2);
    s.settle();
    s.pilot().expect_type<PortSettingsDialog>(
        "settings",
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    QMenu* menu =
        open_context_menu(s.canvas()->viewport(), node_point(s.canvas(), b));
    REQUIRE(menu != nullptr);
    choose_entry(menu, QStringLiteral("Settings..."));
    unchanged();

    s.step(QStringLiteral("cancel the Delete confirmation"));
    ad::select_ports(s, {a, b});
    s.pilot().expect(
        "confirm Delete", [](QWidget* d)
        { return button_starting(d, {QStringLiteral("Cancel")}) != nullptr; },
        [](QWidget* d) { click(button(d, QStringLiteral("Cancel"))); });
    key(s.canvas(), Qt::Key_Delete);
    unchanged();
    s.finish();
}

TEST_CASE("edge: the Add sheet refuses empty and duplicate names")
{
    Session s("edge: the Add sheet refuses empty and duplicate names",
              Session::mode::advanced);
    make(s, "Port", "taken");
    s.step(QStringLiteral("an empty name, then a duplicate, then a free name"));
    s.pilot().expect_type<topology::AddSheet>(
        "the Add sheet",
        [](QWidget* d)
        {
            auto* sheet = qobject_cast<topology::AddSheet*>(d);
            QLineEdit* name = ad::sheet_name(sheet);
            type_into(name, QString());
            REQUIRE(!sheet->add_button()->isEnabled());
            type_into(name, QStringLiteral("taken"));
            REQUIRE(!sheet->add_button()->isEnabled());
            type_into(name, QStringLiteral("free"));
            REQUIRE(sheet->add_button()->isEnabled());
            click(sheet->add_button());
        });
    const int sheets_before = s.pilot().ran();
    ad::add_tile(s, QStringLiteral("Port"));
    s.settle();
    // One sheet, one Add: it must not open again.
    s.pilot().check();
    REQUIRE_EQ(s.pilot().ran(), sheets_before + 1);
    REQUIRE_EQ(s.backend().ports().size(), 2);
    s.step(QStringLiteral("a pair with the same name twice"));
    s.pilot().expect_type<topology::AddSheet>(
        "the Add sheet",
        [](QWidget* d)
        {
            auto* sheet = qobject_cast<topology::AddSheet*>(d);
            type_into(ad::sheet_name(sheet, 0), QStringLiteral("same"));
            type_into(ad::sheet_name(sheet, 1), QStringLiteral("same"));
            REQUIRE(!sheet->add_button()->isEnabled());
            type_into(ad::sheet_name(sheet, 1), QStringLiteral("other"));
            click(sheet->add_button());
        });
    ad::add_tile(s, QStringLiteral("Pair"));
    s.pilot().check();
    REQUIRE_EQ(s.backend().ports().size(), 4);
    ad::require_consistent(s);
    s.finish();
}

TEST_CASE("edge: a failing backend is reported and the next try works")
{
    Session s("edge: a failing backend is reported and the next try works");
    s.step(QStringLiteral("make the next call fail, then choose Create"));
    auto* form = st::open_job(s, job::pair);
    s.fake().fail_next(vss::make_error_code(vss::errc::invalid_argument));
    click(form->create_button());
    wait_for("the failed build", [&] { return !form->busy(); });
    s.settle();
    st::require_counts(s, {0, 0});
    REQUIRE(s.simple_page()->form() != nullptr);
    REQUIRE(form->create_button()->isEnabled());
    s.step(QStringLiteral("choose Create again"));
    st::create(s);
    st::require_counts(s, {2, 1});
    st::remove_shown_setup(s);
    s.finish();
}

TEST_CASE("edge: adding many items stays quick")
{
    Session s("edge: adding many items stays quick", Session::mode::advanced);
    constexpr int kPorts = 30;
    QElapsedTimer total;
    total.start();
    for (int index = 0; index < kPorts; ++index)
    {
        s.step(QStringLiteral("add port %1").arg(index + 1));
        ad::expect_add_sheet(s);
        ad::add_tile(s, QStringLiteral("Port"));
        s.pilot().check();
    }
    REQUIRE_EQ(s.backend().ports().size(), static_cast<std::size_t>(kPorts));
    ad::require_consistent(s);
    s.step(QStringLiteral("add ten pairs"));
    for (int index = 0; index < 10; ++index)
    {
        ad::expect_add_sheet(s);
        ad::add_tile(s, QStringLiteral("Pair"));
        s.pilot().check();
    }
    REQUIRE_EQ(s.backend().ports().size(),
               static_cast<std::size_t>(kPorts) + 20);
    REQUIRE_EQ(s.backend().links().size(), 10);
    ad::require_consistent(s);
    s.step(QStringLiteral("Fit, Tidy layout and select everything"));
    Probe::instance().allow_bulk(kPorts + 20);
    key(s.canvas(), Qt::Key_0, Qt::ControlModifier);
    s.canvas()->tidy_layout();
    s.settle();
    key(s.canvas(), Qt::Key_A, Qt::ControlModifier);
    REQUIRE_EQ(s.canvas()->selected_ports().size(), kPorts + 20);
    s.step(QStringLiteral("delete all of it with Delete"));
    Probe::instance().allow_bulk(kPorts + 20);
    ad::delete_selection(s);
    REQUIRE(s.backend().ports().empty());
    ad::require_consistent(s);
    s.step(QStringLiteral("undo brings every port and link back"));
    Probe::instance().allow_bulk(kPorts + 20);
    // The Edit menu names what Undo would undo; with fifty ports it must not
    // list all fifty.
    REQUIRE(s.action("edit.undo")->text().size() < 60);
    choose_menu(s, QStringLiteral("Edit"), QStringLiteral("Undo"), true);
    s.settle();
    vss_test::spin(500);
    if (s.backend().ports().size() != static_cast<std::size_t>(kPorts) + 20)
        throw vss_test::failure(
            "Undo left " + std::to_string(s.backend().ports().size()) +
            " ports, not 50; the status says: " + status_text(s).toStdString());
    REQUIRE_EQ(s.backend().links().size(), 10);
    s.finish();
}

TEST_CASE("edge: links between every pair of port types")
{
    Session s("edge: links between every pair of port types",
              Session::mode::advanced);
    // One of each kind, made the way the toolbox makes them, plus a real
    // device and a network listener, which come from their own dialogs.
    const auto virtual_port = make(s, "Port", "program");
    const auto fixture = make(s, "Fixture", "fixture");
    const auto loopback = make(s, "Loopback", "loop");
    const auto device = s.fake().attach_serial(
        QStringLiteral("device"), QStringLiteral("serial://COM7?baud=9600"),
        vss::serial_settings{}, true);
    REQUIRE(device.has_value());
    const auto network = s.fake().attach_serial(
        QStringLiteral("net"), QStringLiteral("tcp://10.0.0.5:4001"),
        vss::serial_settings{}, true);
    REQUIRE(network.has_value());
    s.settle();
    ad::require_consistent(s);
    const std::vector<vss::endpoint_id> all = {virtual_port, fixture, loopback,
                                               *device, *network};
    int tried = 0;
    for (std::size_t first = 0; first < all.size(); ++first)
    {
        for (std::size_t second = 0; second < all.size(); ++second)
        {
            if (first == second)
                continue;
            for (const wire_kind kind :
                 {wire_kind::two_way, wire_kind::one_way, wire_kind::monitor})
            {
                if (kind == wire_kind::two_way && second < first)
                    continue;
                s.step(QStringLiteral("link %1 to %2, kind %3")
                           .arg(s.backend().port(all[first])->name,
                                s.backend().port(all[second])->name)
                           .arg(static_cast<int>(kind)));
                const std::size_t before = s.backend().links().size();
                ad::wire(s, all[first], all[second], kind);
                ++tried;
                // The engine may refuse (an endpoint that cannot take it);
                // what must hold is that the canvas and the backend agree.
                REQUIRE(s.backend().links().size() <= before + 1);
                ad::require_consistent(s);
                if (s.backend().links().size() == before + 1)
                {
                    choose_menu(s, QStringLiteral("Edit"),
                                QStringLiteral("Undo"), true);
                    s.settle();
                    REQUIRE_EQ(s.backend().links().size(), before);
                }
            }
        }
    }
    REQUIRE(tried >= 40);
    s.finish();
}

TEST_CASE("edge: rapid clicking does not break the window")
{
    Session s("edge: rapid clicking does not break the window");
    s.step(QStringLiteral("click one job tile many times"));
    auto* tile = s.simple_page()->jobs()->tile(job::pair);
    const QPoint spot = tile->mapToGlobal(tile->rect().center());
    click(tile);
    for (int index = 0; index < 7; ++index)
        click_screen(spot);
    wait_for("the form", [&] { return s.simple_page()->form() != nullptr; });
    s.step(QStringLiteral("click Create twice in a row"));
    auto* form = s.simple_page()->form();
    QPointer<simple::JobForm> guard(form);
    click(form->create_button());
    // While the ports are being made Create is disabled, and Enter or a
    // second submit starts nothing.
    if (guard != nullptr && guard->busy())
    {
        REQUIRE(!form->create_button()->isEnabled());
        REQUIRE(!guard->submit());
    }
    wait_for("the build", [&] { return guard.isNull() || !guard->busy(); });
    s.settle();
    st::require_counts(s, {2, 1});
    s.step(QStringLiteral("switch mode ten times"));
    for (int index = 0; index < 5; ++index)
    {
        s.set_mode(Session::mode::advanced);
        s.set_mode(Session::mode::simple);
    }
    st::require_counts(s, {2, 1});
    s.step(QStringLiteral("remove it and click New repeatedly"));
    st::remove_shown_setup(s);
    for (int index = 0; index < 5; ++index)
        click(button(s.simple_page(), QStringLiteral("Set up something new")));
    s.finish();
}

TEST_CASE("edge: the window works at small and large sizes")
{
    Session s("edge: the window works at small and large sizes",
              Session::mode::advanced);
    const auto a = make(s, "Port", "a");
    const auto b = make(s, "Port", "b");
    ad::wire(s, a, b);
    const QSize sizes[] = {{1280, 800},  {900, 600}, {700, 500},
                           {1920, 1080}, {640, 900}, {1280, 800}};
    for (const QSize& size : sizes)
    {
        s.step(QStringLiteral("resize to %1x%2")
                   .arg(size.width())
                   .arg(size.height()));
        s.window().resize(size);
        s.settle();
        REQUIRE(s.canvas()->isVisible());
        ad::require_consistent(s);
        key(s.canvas(), Qt::Key_0, Qt::ControlModifier);
        REQUIRE(s.canvas()->is_auto_fit());
        // The nodes stay inside the canvas after Fit.
        const QRect viewport = s.canvas()->viewport()->rect();
        for (const auto id : {a, b})
            REQUIRE(viewport.contains(node_point(s.canvas(), id)));
    }
    s.step(QStringLiteral("in Simple mode at a small size a job is reachable"));
    s.set_mode(Session::mode::simple);
    s.window().resize(700, 500);
    s.settle();
    click(button(s.simple_page(), QStringLiteral("Set up something new")));
    click(s.simple_page()->jobs()->tile(job::bus));
    wait_for("the form", [&] { return s.simple_page()->form() != nullptr; });
    REQUIRE(s.simple_page()->form()->create_button()->isVisible());
    s.finish();
}

TEST_CASE("edge: text at 200 percent in the journeys")
{
    Session s("edge: text at 200 percent in the journeys");
    // ui_edge_200 runs the group with QT_SCALE_FACTOR=2, which already makes
    // everything twice as large, on an offscreen screen of 800 by 600 that no
    // real 200 percent display resembles. There the journeys run as they are.
    const bool scaled_by_qt = qEnvironmentVariableIsSet("QT_SCALE_FACTOR");
    const auto larger = [scaled_by_qt](QWidget* widget)
    {
        if (!scaled_by_qt)
            vss_test::scale_text(widget, 2.0);
    };
    s.step(QStringLiteral("scale all text to 200 percent"));
    larger(&s.window());
    s.settle();
    s.step(QStringLiteral("make a pair in Simple mode"));
    st::open_job(s, job::pair);
    st::create(s);
    st::require_counts(s, {2, 1});
    st::remove_shown_setup(s);
    s.set_mode(Session::mode::advanced);
    larger(&s.window());
    s.settle();
    s.step(
        QStringLiteral("the Add sheet fits its screen and shows its buttons"));
    s.pilot().expect_type<topology::AddSheet>(
        "the Add sheet",
        [larger, scaled_by_qt](QWidget* d)
        {
            larger(d);
            QApplication::processEvents();
            if (!scaled_by_qt)
                REQUIRE(d->height() <=
                        d->screen()->availableGeometry().height());
            auto* sheet = qobject_cast<topology::AddSheet*>(d);
            REQUIRE(sheet->add_button()->isVisible());
            const QRect rect(sheet->add_button()->mapTo(d, QPoint(0, 0)),
                             sheet->add_button()->size());
            REQUIRE(d->rect().contains(rect));
            click(sheet->add_button());
        });
    ad::add_tile(s, QStringLiteral("Pair"));
    s.pilot().check();
    ad::require_consistent(s);
    s.finish();
}

TEST_CASE("edge: both themes through the same journey")
{
    Session s("edge: both themes through the same journey");
    for (const char* theme : {"view.theme.dark", "view.theme.light"})
    {
        s.step(QStringLiteral("choose theme ") + QString::fromLatin1(theme));
        // Every widget of the window is restyled at once.
        Probe::instance().allow_bulk(10);
        s.action(theme)->trigger();
        s.settle();
        REQUIRE_EQ(Theme::instance().mode(),
                   std::string(theme) == "view.theme.dark" ? theme_mode::dark
                                                           : theme_mode::light);
        st::open_job(s, job::port);
        st::create(s);
        st::require_counts(s, {1, 0});
        st::remove_shown_setup(s);
        s.set_mode(Session::mode::advanced);
        make(s, "Port", "x");
        ad::clear_canvas(s);
        s.set_mode(Session::mode::simple);
    }
    s.finish();
}

TEST_CASE("edge: removing everything while a repeat is running")
{
    Session s("edge: removing everything while a repeat is running",
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    QWidget* dock = s.dock("sendDock");
    ad::click_node(s, a);
    type_into(qobject_cast<QLineEdit*>(
                  accessible(dock, QStringLiteral("Bytes to send"))),
              QStringLiteral("x"));
    click(checkbox(dock, QStringLiteral("Repeat")));
    wait_for("the first repeat", [&] { return !s.fake().sent().empty(); });
    s.step(QStringLiteral("delete the port the repeat sends from"));
    ad::click_node(s, a);
    ad::delete_selection(s, false);
    REQUIRE(s.backend().ports().empty());
    vss_test::spin(300);
    s.finish();
}

TEST_CASE("edge: a status message does not draw over the counters")
{
    Session s("edge: a status message does not draw over the counters",
              Session::mode::advanced);
    const auto a = make(s, "Loopback", "a");
    s.step(QStringLiteral(
        "send a few bytes, which says Queued in the status bar"));
    ad::click_node(s, a);
    QWidget* dock = s.dock("sendDock");
    type_into(qobject_cast<QLineEdit*>(
                  accessible(dock, QStringLiteral("Bytes to send"))),
              QStringLiteral("hi"));
    click(button(dock, QStringLiteral("Send")));
    vss_test::spin(700);
    s.settle();
    auto* bar = s.window().statusBar();
    REQUIRE(!bar->currentMessage().isEmpty());
    QStringList drawn;
    for (auto* label : bar->findChildren<QLabel*>())
    {
        const QString text = label->text();
        if (label->isVisibleTo(bar) &&
            (text.startsWith(QStringLiteral("Ports")) ||
             text.startsWith(QStringLiteral("Links")) ||
             text.startsWith(QStringLiteral("RX")) ||
             text.startsWith(QStringLiteral("Hub"))))
            drawn << text;
    }
    if (!drawn.isEmpty())
        throw vss_test::failure(
            "the status bar shows \"" + bar->currentMessage().toStdString() +
            "\" and still draws " +
            drawn.join(QStringLiteral(" | ")).toStdString() + " under it");
    s.finish();
}

TEST_CASE("edge: About shows the application icon, the version and the source")
{
    Session s("edge: About shows the application icon, the version and the "
              "source");
    s.step(QStringLiteral("the program has an application icon"));
    REQUIRE(!IconProvider::instance().app_icon().isNull());
    REQUIRE(!IconProvider::instance().app_icon().pixmap(32, 32).isNull());

    s.step(QStringLiteral("choose Help, About"));
    s.pilot().expect_type<support::AboutDialog>(
        "the About dialog",
        [](QWidget* dialog)
        {
            bool icon_drawn = false;
            bool version_shown = false;
            bool source_linked = false;
            const QString version = support::current_version_string();
            for (auto* label : dialog->findChildren<QLabel*>())
            {
                if (!label->pixmap(Qt::ReturnByValue).isNull())
                    icon_drawn = true;
                if (label->text().contains(QStringLiteral("Version ") +
                                           version) &&
                    label->text().contains(QStringLiteral("version 3 or later")))
                    version_shown = true;
                if (label->text().contains(
                        support::AboutDialog::source_url()) &&
                    label->textInteractionFlags().testFlag(
                        Qt::LinksAccessibleByKeyboard))
                    source_linked = true;
            }
            REQUIRE(icon_drawn);
            REQUIRE(version_shown);
            REQUIRE(source_linked);
            click(button(dialog, QStringLiteral("Close")));
        });
    choose_menu(s, QStringLiteral("Help"), QStringLiteral("About"));
    s.settle();
    s.pilot().check();
    s.finish();
}
