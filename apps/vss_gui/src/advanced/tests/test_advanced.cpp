//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "advanced/add_fixture_dialog.hpp"
#include "advanced/benchmark_dialog.hpp"
#include "advanced/event_log.hpp"
#include "advanced/export_command_dialog.hpp"
#include "advanced/json_editor.hpp"
#include "advanced/json_validation.hpp"
#include "advanced/line_matrix_panel.hpp"
#include "advanced/link_settings_dialog.hpp"
#include "advanced/owner_bar.hpp"
#include "advanced/wiring_dialog.hpp"
#include "backend/fake_studio_backend.hpp"
#include "shell/shell_window.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/half_duplex_fields.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <thread>

namespace
{

void clear_settings()
{
    QSettings settings;
    settings.clear();
}

vss::endpoint_id make_port(vss::gui::FakeStudioBackend& backend,
                           const QString& name)
{
    const auto created = backend.create_virtual_port(
        name, vss::virtual_port_kind::loopback, true);
    REQUIRE(created.has_value());
    return *created;
}

vss::endpoint_id make_fixture(vss::gui::FakeStudioBackend& backend,
                              const QString& name)
{
    const auto created = backend.create_mock_port(name, vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    REQUIRE(created.has_value());
    return *created;
}

/// Pumps the event loop until `done()` or about five seconds have passed.
template <class Predicate>
bool wait_for(Predicate done)
{
    for (int attempt = 0; attempt < 1000 && !done(); ++attempt)
    {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QApplication::processEvents();
    return done();
}

QString valid_document()
{
    return QStringLiteral(R"({
  "version": 1,
  "name": "lab",
  "ports": [
    { "name": "device", "kind": "physical", "uri": "serial://COM3" },
    { "name": "terminal", "kind": "virtual", "virtual_kind": "auto" }
  ],
  "links": [
    { "mode": "duplex", "sources": ["device"], "targets": ["terminal"] }
  ]
})");
}

void check_accessible_controls(QWidget* root)
{
    for (QPushButton* button : root->findChildren<QPushButton*>())
    {
        if (!button->isVisible())
            continue;
        REQUIRE(!button->text().isEmpty() ||
                !button->accessibleName().isEmpty());
        REQUIRE(button->height() >= 24 || button->sizeHint().height() >= 24);
    }
    for (QToolButton* button : root->findChildren<QToolButton*>())
    {
        if (!button->isVisible())
            continue;
        REQUIRE(!button->text().isEmpty() ||
                !button->accessibleName().isEmpty());
    }
    for (QCheckBox* box : root->findChildren<QCheckBox*>())
    {
        if (!box->isVisible())
            continue;
        REQUIRE(!box->accessibleName().isEmpty());
        REQUIRE(box->height() >= 24 || box->sizeHint().height() >= 24);
    }
    for (QRadioButton* button : root->findChildren<QRadioButton*>())
    {
        if (!button->isVisible())
            continue;
        REQUIRE(!button->accessibleName().isEmpty());
    }
    for (QComboBox* combo : root->findChildren<QComboBox*>())
    {
        if (!combo->isVisible())
            continue;
        REQUIRE(!combo->accessibleName().isEmpty());
    }
    for (QLineEdit* field : root->findChildren<QLineEdit*>())
    {
        if (!field->isVisible())
            continue;
        REQUIRE(!field->accessibleName().isEmpty());
    }
    for (QPlainTextEdit* editor : root->findChildren<QPlainTextEdit*>())
    {
        if (!editor->isVisible())
            continue;
        REQUIRE(!editor->accessibleName().isEmpty());
    }
    for (QTableWidget* table : root->findChildren<QTableWidget*>())
    {
        if (!table->isVisible())
            continue;
        REQUIRE(!table->accessibleName().isEmpty());
    }
    for (QProgressBar* progress : root->findChildren<QProgressBar*>())
    {
        if (!progress->isVisible())
            continue;
        REQUIRE(!progress->accessibleName().isEmpty());
    }
}

} // namespace

TEST_CASE("owner bar: join owner actions reach the engine")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    const vss::endpoint_id recipe =
        make_port(backend, QStringLiteral("recipe-tool"));
    const vss::endpoint_id qa = make_port(backend, QStringLiteral("qa-runner"));
    const vss::endpoint_id device =
        make_port(backend, QStringLiteral("device"));

    vss::link_options options;
    options.policy = vss::merge_policy::owner;
    const auto joined = backend.join({recipe, qa}, device, options);
    REQUIRE(joined.has_value());

    OwnerBar bar(backend);
    bar.set_link(backend.link(*joined));
    REQUIRE(bar.has_link());
    REQUIRE_EQ(bar.summary(),
               QStringLiteral("recipe-tool and qa-runner into device"));
    REQUIRE_EQ(bar.owner_name(), QStringLiteral("Nobody yet"));
    REQUIRE_EQ(bar.give_button()->text(),
               QStringLiteral("Give to recipe-tool"));
    REQUIRE(!bar.policy_combo()->isEnabled());
    REQUIRE_EQ(bar.policy_combo()->toolTip(),
               QStringLiteral("Not available in this build"));

    bar.give_button()->click();
    QApplication::processEvents();
    REQUIRE(vss_test::require_value(backend.link(*joined)).owner == recipe);
    REQUIRE_EQ(bar.owner_name(), QStringLiteral("recipe-tool"));
    REQUIRE_EQ(bar.give_button()->text(), QStringLiteral("Give to qa-runner"));
    REQUIRE(bar.release_button()->isEnabled());

    bar.release_button()->click();
    QApplication::processEvents();
    REQUIRE(vss_test::require_value(backend.link(*joined)).owner == 0);

    vss::link_options mixed;
    mixed.policy = vss::merge_policy::interleave;
    const auto split = backend.split(device, {recipe}, mixed);
    REQUIRE(split.has_value());
    REQUIRE(!OwnerBar::supports_owner(
        vss_test::require_value(backend.link(*split))));
}

TEST_CASE("owner bar: a split with the owner policy lists its targets")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    const vss::endpoint_id device =
        make_port(backend, QStringLiteral("device"));
    const vss::endpoint_id recipe =
        make_port(backend, QStringLiteral("recipe-tool"));
    const vss::endpoint_id qa = make_port(backend, QStringLiteral("qa-runner"));

    vss::link_options options;
    options.policy = vss::merge_policy::owner;
    const auto split = backend.split(device, {recipe, qa}, options);
    REQUIRE(split.has_value());
    REQUIRE(OwnerBar::supports_owner(
        vss_test::require_value(backend.link(*split))));
    REQUIRE(OwnerBar::meets_at(vss_test::require_value(backend.link(*split)),
                               device));
    REQUIRE(!OwnerBar::meets_at(vss_test::require_value(backend.link(*split)),
                                recipe));

    OwnerBar bar(backend);
    bar.set_link(backend.link(*split));
    REQUIRE(bar.has_link());
    REQUIRE_EQ(bar.summary(),
               QStringLiteral("device to recipe-tool and qa-runner"));
    REQUIRE_EQ(bar.owner_name(), QStringLiteral("Nobody yet"));
    bool tagged_split = false;
    for (const auto* tag : bar.findChildren<QLabel*>())
    {
        REQUIRE(tag->text() != QStringLiteral("Join"));
        tagged_split = tagged_split ||
                       (tag->text() == QStringLiteral("Split") &&
                        tag->accessibleName() == QStringLiteral("Split link"));
    }
    REQUIRE(tagged_split);

    // The candidates are the targets; the source is never offered.
    REQUIRE_EQ(bar.give_button()->text(),
               QStringLiteral("Give to recipe-tool"));
    bar.give_button()->click();
    QApplication::processEvents();
    REQUIRE_EQ(vss_test::require_value(backend.link(*split)).owner, recipe);
    REQUIRE_EQ(bar.owner_name(), QStringLiteral("recipe-tool"));
    REQUIRE_EQ(bar.give_button()->text(), QStringLiteral("Give to qa-runner"));
    REQUIRE(bar.release_button()->isEnabled());

    bar.give_button()->click();
    QApplication::processEvents();
    REQUIRE_EQ(vss_test::require_value(backend.link(*split)).owner, qa);
    REQUIRE_EQ(bar.give_button()->text(),
               QStringLiteral("Give to recipe-tool"));

    bar.release_button()->click();
    QApplication::processEvents();
    REQUIRE_EQ(vss_test::require_value(backend.link(*split)).owner,
               vss::endpoint_id{0});
    REQUIRE_EQ(bar.owner_name(), QStringLiteral("Nobody yet"));
    REQUIRE(!bar.release_button()->isEnabled());

    // The engine's rule holds whatever the bar offers: the source cannot own a
    // split.
    REQUIRE(!backend.set_owner(*split, device).has_value());
    REQUIRE_EQ(vss_test::require_value(backend.link(*split)).owner,
               vss::endpoint_id{0});

    // A split with one target has nothing to choose between, and one without
    // the owner policy has no owner.
    const vss::endpoint_id lone = make_port(backend, QStringLiteral("lone"));
    const vss::endpoint_id other = make_port(backend, QStringLiteral("other"));
    const auto single = backend.split(other, {lone}, options);
    REQUIRE(single.has_value());
    REQUIRE(!OwnerBar::supports_owner(
        vss_test::require_value(backend.link(*single))));
    bar.set_link(backend.link(*single));
    REQUIRE(!bar.has_link());
}

TEST_CASE("shell: the owner bar shows for a split with the owner policy")
{
    using namespace vss::gui;
    clear_settings();
    FakeStudioBackend backend;
    const vss::endpoint_id device =
        make_port(backend, QStringLiteral("device"));
    const vss::endpoint_id recipe =
        make_port(backend, QStringLiteral("recipe-tool"));
    const vss::endpoint_id qa = make_port(backend, QStringLiteral("qa-runner"));

    ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();
    QAction* advanced =
        window.findChild<QAction*>(QStringLiteral("view.advanced"));
    REQUIRE(advanced != nullptr);
    advanced->trigger();
    QApplication::processEvents();
    auto* owner = window.findChild<OwnerBar*>();
    REQUIRE(owner != nullptr);
    REQUIRE(!owner->isVisible());

    vss::link_options options;
    options.policy = vss::merge_policy::owner;
    const auto split = backend.split(device, {recipe, qa}, options);
    REQUIRE(split.has_value());
    QApplication::processEvents();
    REQUIRE(owner->has_link());
    REQUIRE(owner->link_id() == *split);
    REQUIRE_EQ(owner->summary(),
               QStringLiteral("device to recipe-tool and qa-runner"));

    // Topology, Owner goes to the bar's buttons instead of saying there is
    // nothing to control.
    QAction* owner_action =
        window.findChild<QAction*>(QStringLiteral("topology.owner"));
    REQUIRE(owner_action != nullptr);
    owner_action->trigger();
    QApplication::processEvents();
    REQUIRE(owner->has_link());

    // Back to Simple mode, as the saved setting is shared with other tests.
    advanced->trigger();
    QApplication::processEvents();
    REQUIRE(!advanced->isChecked());
}

TEST_CASE("owner bar: capability off disables the actions with one tooltip")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    const vss::endpoint_id first =
        make_port(backend, QStringLiteral("recipe-tool"));
    const vss::endpoint_id second =
        make_port(backend, QStringLiteral("qa-runner"));
    const vss::endpoint_id target =
        make_port(backend, QStringLiteral("device"));
    vss::link_options options;
    options.policy = vss::merge_policy::owner;
    const auto joined = backend.join({first, second}, target, options);
    REQUIRE(joined.has_value());

    capabilities caps;
    caps.owner = false;
    backend.set_capabilities(caps);
    OwnerBar bar(backend);
    bar.set_link(backend.link(*joined));
    REQUIRE(!bar.release_button()->isEnabled());
    REQUIRE(!bar.give_button()->isEnabled());
    REQUIRE_EQ(bar.release_button()->toolTip(),
               QStringLiteral("Not available in this build"));
    REQUIRE_EQ(bar.give_button()->toolTip(),
               QStringLiteral("Not available in this build"));
}

TEST_CASE("add fixture: creates a real mock port and its two-way link")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    const vss::endpoint_id terminal =
        make_port(backend, QStringLiteral("terminal"));

    AddFixtureDialog dialog(backend, terminal);
    REQUIRE_EQ(dialog.fixture_name(), QStringLiteral("modem"));
    REQUIRE_EQ(dialog.baud_rate(), 115200);
    REQUIRE(dialog.target_port() == terminal);
    REQUIRE(dialog.result_text().contains(QStringLiteral("terminal")));

    REQUIRE(dialog.add_fixture());
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
    const auto ports = backend.ports();
    const auto mock =
        std::find_if(ports.begin(), ports.end(), [](const port_view& view)
                     { return view.kind == vss::endpoint_kind::mock; });
    REQUIRE(mock != ports.end());
    REQUIRE(mock->serial.baud_rate == 115200u);
    const auto links = backend.links();
    REQUIRE_EQ(links.size(), std::size_t{1});
    REQUIRE(links.front().shape == link_shape::two_way);
}

TEST_CASE("add fixture: capability off keeps the button disabled")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.fixtures = false;
    backend.set_capabilities(caps);
    static_cast<void>(make_port(backend, QStringLiteral("terminal")));

    AddFixtureDialog dialog(backend, 0);
    REQUIRE(!dialog.add_button()->isEnabled());
    REQUIRE_EQ(dialog.add_button()->toolTip(),
               QStringLiteral("Not available in this build"));
    REQUIRE(!dialog.add_fixture());
    REQUIRE_EQ(backend.ports().size(), std::size_t{1});
}

TEST_CASE("line matrix: read-only standard preset and gated faults")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    const vss::endpoint_id modem =
        make_fixture(backend, QStringLiteral("modem"));
    const vss::endpoint_id terminal =
        make_port(backend, QStringLiteral("terminal"));
    const auto linked = backend.connect_ports(modem, terminal, {});
    REQUIRE(linked.has_value());

    LineMatrixPanel panel(backend, modem);
    QCheckBox* dtr_dsr = panel.box_named(QStringLiteral("DTR to DSR"));
    REQUIRE(dtr_dsr != nullptr);
    REQUIRE(dtr_dsr->isChecked());
    REQUIRE(!dtr_dsr->isEnabled());
    REQUIRE_EQ(dtr_dsr->toolTip(),
               QStringLiteral("Not available in this build"));
    REQUIRE(panel.box_named(QStringLiteral("DTR to DCD"))->isChecked());
    REQUIRE(panel.box_named(QStringLiteral("RTS to CTS"))->isChecked());
    REQUIRE(!panel.box_named(QStringLiteral("RTS to DSR"))->isChecked());
    REQUIRE(!panel.preset_control()->isEnabled());
    REQUIRE_EQ(panel.preset_control()->toolTip(),
               QStringLiteral("Not available in this build"));
    REQUIRE(!panel.emulate_switch()->isEnabled());

    REQUIRE_EQ(panel.inject_count(), 6);
    for (int index = 0; index < panel.inject_count(); ++index)
    {
        QPushButton* inject = panel.inject_button(index);
        REQUIRE(inject != nullptr);
        REQUIRE(!inject->isEnabled());
        REQUIRE_EQ(inject->toolTip(),
                   QStringLiteral("Not available in this build"));
    }
    REQUIRE_EQ(panel.history_table()->rowCount(), 0);
    REQUIRE(panel.caption_text().contains(QStringLiteral("terminal")));
    REQUIRE_EQ(
        panel.wiring_text(),
        QStringLiteral("With this wiring, raising DTR on modem raises DSR "
                       "and DCD on terminal. Raising RTS raises CTS."));
}

TEST_CASE("line matrix: faults are injected into a running fixture")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.faults = true;
    backend.set_capabilities(caps);
    const vss::endpoint_id modem =
        make_fixture(backend, QStringLiteral("modem"));

    LineMatrixPanel panel(backend, modem);
    for (int index = 0; index < panel.inject_count(); ++index)
    {
        REQUIRE(panel.inject_button(index)->isEnabled());
        REQUIRE_EQ(panel.inject_button(index)->toolTip(), QString());
    }

    panel.inject_button(3)->click();
    REQUIRE_EQ(backend.injected().size(), std::size_t{1});
    REQUIRE(backend.injected().front() == vss::fault_kind::break_condition);
    REQUIRE_EQ(panel.history_table()->rowCount(), 1);
    REQUIRE_EQ(panel.history_table()->item(0, 1)->text(),
               QStringLiteral("Break"));
    REQUIRE_EQ(panel.history_table()->item(0, 2)->text(),
               QStringLiteral("Error event reported on modem"));

    // The engine stops the fixture on the fault, so the tiles go off with the
    // reason and the history stays.
    for (int index = 0; index < panel.inject_count(); ++index)
    {
        REQUIRE(!panel.inject_button(index)->isEnabled());
        REQUIRE_EQ(panel.inject_button(index)->toolTip(),
                   QStringLiteral("The fixture is not running."));
    }
    REQUIRE_EQ(panel.history_table()->rowCount(), 1);
}

TEST_CASE("line matrix: faults are refused for a port that is not a fixture")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.faults = true;
    backend.set_capabilities(caps);
    const vss::endpoint_id terminal =
        make_port(backend, QStringLiteral("terminal"));

    LineMatrixPanel panel(backend, terminal);
    for (int index = 0; index < panel.inject_count(); ++index)
    {
        REQUIRE(!panel.inject_button(index)->isEnabled());
        REQUIRE_EQ(panel.inject_button(index)->toolTip(),
                   QStringLiteral("Faults are only available on fixture ports."));
    }
    REQUIRE(
        backend.inject_fault(terminal, vss::fault_kind::parity).has_error());
    REQUIRE(backend.injected().empty());
}

namespace
{

vss::endpoint_id make_driver_port(vss::gui::FakeStudioBackend& backend,
                                  const QString& name)
{
    const auto created = backend.create_driver_port(name, true, 7);
    REQUIRE(created.has_value());
    return *created;
}

} // namespace

TEST_CASE("line matrix: a driver port with a program takes every fault")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.faults = true;
    caps.driver_faults = true;
    backend.set_capabilities(caps);
    const vss::endpoint_id meter =
        make_driver_port(backend, QStringLiteral("meter"));
    backend.set_peer_state(meter, peer_state::open);

    LineMatrixPanel panel(backend, meter);
    REQUIRE_EQ(panel.inject_count(), 6);
    for (int index = 0; index < 6; ++index)
    {
        REQUIRE(panel.inject_button(index)->isEnabled());
        REQUIRE_EQ(panel.inject_button(index)->toolTip(), QString());
        REQUIRE_EQ(panel.inject_button(index)->accessibleDescription(),
                   QString());
    }

    panel.inject_button(3)->click();
    REQUIRE_EQ(backend.injected().size(), std::size_t{1});
    REQUIRE(backend.injected().front() == vss::fault_kind::break_condition);
    REQUIRE_EQ(panel.history_table()->rowCount(), 1);
    REQUIRE_EQ(panel.history_table()->item(0, 1)->text(),
               QStringLiteral("Break"));
    REQUIRE_EQ(panel.history_table()->item(0, 2)->text(),
               QStringLiteral("Line error raised at the program on meter"));

    // The fault goes to the program, not to the port: it keeps running and
    // the tiles stay on.
    const auto after = backend.port(meter);
    REQUIRE(after.has_value());
    REQUIRE(after->state == vss::endpoint_state::open);
    REQUIRE(after->open);
    for (int index = 0; index < 6; ++index)
        REQUIRE(panel.inject_button(index)->isEnabled());

    // A hang-up drops the carrier and the ready line at the program; the port
    // and the program stay.
    REQUIRE(
        backend.set_line(meter, vss::control_line::dcd, true).has_value());
    REQUIRE(
        backend.set_line(meter, vss::control_line::dsr, true).has_value());
    panel.inject_button(4)->click();
    REQUIRE_EQ(backend.injected().size(), std::size_t{2});
    REQUIRE(backend.injected().back() == vss::fault_kind::hangup);
    REQUIRE_EQ(panel.history_table()->item(1, 1)->text(),
               QStringLiteral("Hang-up"));
    REQUIRE_EQ(panel.history_table()->item(1, 2)->text(),
               QStringLiteral("DSR and DCD dropped on meter"));
    const auto hung_up = backend.port(meter);
    REQUIRE(hung_up.has_value());
    REQUIRE(!hung_up->lines.dcd());
    REQUIRE(!hung_up->lines.dsr());
    REQUIRE(hung_up->state == vss::endpoint_state::open);
    REQUIRE(hung_up->peer == peer_state::open);
    for (int index = 0; index < 6; ++index)
        REQUIRE(panel.inject_button(index)->isEnabled());

    // A disconnect takes the port from the program: the port keeps running
    // with nobody on it, so the tiles go off with that reason.
    panel.inject_button(5)->click();
    REQUIRE_EQ(backend.injected().size(), std::size_t{3});
    REQUIRE(backend.injected().back() == vss::fault_kind::disconnect);
    REQUIRE_EQ(panel.history_table()->item(2, 1)->text(),
               QStringLiteral("Disconnect"));
    REQUIRE_EQ(panel.history_table()->item(2, 2)->text(),
               QStringLiteral("Port taken back from the program on meter"));
    const auto unplugged = backend.port(meter);
    REQUIRE(unplugged.has_value());
    REQUIRE(unplugged->state == vss::endpoint_state::open);
    REQUIRE(unplugged->peer == peer_state::not_open);
    panel.refresh();
    for (int index = 0; index < 6; ++index)
    {
        REQUIRE(!panel.inject_button(index)->isEnabled());
        REQUIRE_EQ(panel.inject_button(index)->toolTip(),
                   QStringLiteral("No program has the port open."));
    }
}

TEST_CASE("line matrix: a driver port with no program refuses every fault")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.faults = true;
    caps.driver_faults = true;
    backend.set_capabilities(caps);
    const vss::endpoint_id meter =
        make_driver_port(backend, QStringLiteral("meter"));

    // The peer is unknown until the backend says otherwise, and not open when
    // it says so: neither has a program on the port.
    for (const peer_state peer : {peer_state::unknown, peer_state::not_open})
    {
        backend.set_peer_state(meter, peer);
        LineMatrixPanel panel(backend, meter);
        for (int index = 0; index < 6; ++index)
        {
            REQUIRE(!panel.inject_button(index)->isEnabled());
            REQUIRE_EQ(panel.inject_button(index)->toolTip(),
                       QStringLiteral("No program has the port open."));
            REQUIRE_EQ(panel.inject_button(index)->accessibleDescription(),
                       QStringLiteral("No program has the port open."));
        }
    }
    REQUIRE(backend.injected().empty());
}

TEST_CASE("line matrix: a driver port without driver faults is refused")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.faults = true;
    backend.set_capabilities(caps);
    const vss::endpoint_id meter =
        make_driver_port(backend, QStringLiteral("meter"));
    backend.set_peer_state(meter, peer_state::open);

    LineMatrixPanel panel(backend, meter);
    for (int index = 0; index < panel.inject_count(); ++index)
    {
        REQUIRE(!panel.inject_button(index)->isEnabled());
        REQUIRE_EQ(
            panel.inject_button(index)->toolTip(),
            QStringLiteral("Faults are only available on fixture ports."));
    }
    REQUIRE(backend.inject_fault(meter, vss::fault_kind::parity).has_error());
    REQUIRE(backend.injected().empty());

    // Without the faults capability at all the tiles say so, as before.
    caps.faults = false;
    backend.set_capabilities(caps);
    panel.refresh();
    for (int index = 0; index < panel.inject_count(); ++index)
    {
        REQUIRE_EQ(panel.inject_button(index)->toolTip(),
                   QStringLiteral("Not available in this build"));
    }
}

TEST_CASE("line matrix: capability on applies a wiring change")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.line_matrix = true;
    backend.set_capabilities(caps);
    const vss::endpoint_id modem =
        make_fixture(backend, QStringLiteral("modem"));
    const vss::endpoint_id terminal =
        make_port(backend, QStringLiteral("terminal"));
    const auto linked = backend.connect_ports(modem, terminal, {});
    REQUIRE(linked.has_value());

    LineMatrixPanel panel(backend, modem);
    QCheckBox* dtr_cts = panel.box_named(QStringLiteral("DTR to CTS"));
    REQUIRE(dtr_cts != nullptr);
    REQUIRE(dtr_cts->isEnabled());
    dtr_cts->setChecked(true);
    QApplication::processEvents();
    const auto stored = backend.link_wiring(*linked);
    REQUIRE(stored.has_value());
    REQUIRE(stored->dtr_to_cts);
}

TEST_CASE("wiring dialog: fixture header and subtitle")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    const vss::endpoint_id modem =
        make_fixture(backend, QStringLiteral("modem"));
    const vss::endpoint_id terminal =
        make_port(backend, QStringLiteral("terminal"));
    REQUIRE(backend.connect_ports(modem, terminal, {}).has_value());

    WiringDialog dialog(backend, modem);
    REQUIRE(dialog.panel() != nullptr);
    REQUIRE(dialog.fixture_chip() != nullptr);
    REQUIRE(dialog.state_chip() != nullptr);
    REQUIRE_EQ(dialog.state_chip()->text(), QStringLiteral("Open"));
    REQUIRE(dialog.subtitle_text().contains(QStringLiteral("terminal")));
}

TEST_CASE("json: syntax, mode, duplicate name and loop errors")
{
    using namespace vss::gui;
    const json_validation good = validate_json_document(valid_document());
    REQUIRE(good.valid());
    REQUIRE_EQ(good.version, 1);
    REQUIRE_EQ(good.ports, 2);
    REQUIRE_EQ(good.links, 1);
    REQUIRE_EQ(good.checks.size(), 3);
    REQUIRE(good.formatted.contains(QStringLiteral("\"version\": 1")));
    REQUIRE_EQ(good.name, QStringLiteral("lab"));

    const json_validation broken = validate_json_document(
        QStringLiteral("{\n  \"version\": 1,\n  \"ports\": [\n"));
    REQUIRE(!broken.valid());
    REQUIRE(broken.problem.has_value());
    REQUIRE(broken.problem->has_line());

    QString unknown_mode = valid_document();
    unknown_mode.replace(QStringLiteral("\"duplex\""),
                         QStringLiteral("\"splt\""));
    const json_validation mode = validate_json_document(unknown_mode);
    REQUIRE(!mode.valid());
    REQUIRE(mode.problem.has_value());
    REQUIRE(
        mode.problem->summary.contains(QStringLiteral("unknown link mode")));
    REQUIRE(mode.problem->detail.contains(
        QStringLiteral("Did you mean \"split\"?")));
    REQUIRE_EQ(mode.problem->fix_from, QStringLiteral("splt"));
    REQUIRE_EQ(mode.problem->fix_to, QStringLiteral("split"));
    REQUIRE(mode.problem->line > 0);

    QString duplicate = valid_document();
    duplicate.replace(
        QStringLiteral("{ \"name\": \"terminal\", \"kind\": \"virtual\", "
                       "\"virtual_kind\": \"auto\" }"),
        QStringLiteral("{ \"name\": \"device\", \"kind\": \"virtual\", "
                       "\"virtual_kind\": \"auto\" }"));
    const json_validation names = validate_json_document(duplicate);
    REQUIRE(!names.valid());
    REQUIRE(names.problem.has_value());
    REQUIRE(names.problem->summary.contains(QStringLiteral("used twice")));
    REQUIRE(names.problem->line > 0);

    const QString loop = QStringLiteral(
        "{\"version\":1,\"name\":\"lab\",\"ports\":["
        "{\"name\":\"a\",\"kind\":\"virtual\"},"
        "{\"name\":\"b\",\"kind\":\"virtual\"}],\"links\":["
        "{\"mode\":\"duplex\",\"sources\":[\"a\"],\"targets\":[\"b\"]},"
        "{\"mode\":\"redirect\",\"sources\":[\"b\"],\"targets\":[\"a\"]}]}");
    const json_validation looped = validate_json_document(loop);
    REQUIRE(!looped.valid());
    REQUIRE(looped.problem.has_value());
    REQUIRE(looped.problem->summary.contains(QStringLiteral("loop")));
    REQUIRE(looped.problem->line > 0);
}

TEST_CASE("json: every schema error names the line of its field")
{
    using namespace vss::gui;

    // The second port has the bad kind, so the line must be the second port's
    // and not the first match of the word "kind".
    const QString bad_kind = QStringLiteral(
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"lab\",\n"
        "  \"ports\": [\n"
        "    { \"name\": \"a\", \"kind\": \"virtual\" },\n"
        "    { \"name\": \"b\",\n"
        "      \"kind\": \"serial\" }\n"
        "  ]\n"
        "}\n");
    const json_validation kind = validate_json_document(bad_kind);
    REQUIRE(!kind.valid());
    REQUIRE(kind.problem.has_value());
    REQUIRE_EQ(kind.problem->line, 7);

    const QString bad_baud = QStringLiteral(
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"lab\",\n"
        "  \"ports\": [\n"
        "    { \"name\": \"a\", \"kind\": \"virtual\" },\n"
        "    { \"name\": \"b\", \"kind\": \"virtual\",\n"
        "      \"serial\": {\n"
        "        \"parity\": \"none\",\n"
        "        \"baud_rate\": -5\n"
        "      } }\n"
        "  ]\n"
        "}\n");
    const json_validation baud = validate_json_document(bad_baud);
    REQUIRE(!baud.valid());
    REQUIRE(baud.problem.has_value());
    REQUIRE_EQ(baud.problem->line, 9);
    REQUIRE(baud.problem->summary.startsWith(QStringLiteral("Line 9:")));

    const QString bad_type = QStringLiteral(
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"lab\",\n"
        "  \"ports\": [\n"
        "    { \"name\": \"a\", \"kind\": \"virtual\",\n"
        "      \"serial\": { \"parity\": 7 } }\n"
        "  ]\n"
        "}\n");
    const json_validation type = validate_json_document(bad_type);
    REQUIRE(!type.valid());
    REQUIRE_EQ(vss_test::require_value(type.problem).line, 6);

    const QString not_a_list = QStringLiteral(
        "{\n"
        "  \"version\": 1,\n"
        "  \"ports\": 5\n"
        "}\n");
    const json_validation list = validate_json_document(not_a_list);
    REQUIRE(!list.valid());
    REQUIRE_EQ(vss_test::require_value(list.problem).line, 3);

    const QString missing_port = QStringLiteral(
        "{\n"
        "  \"version\": 1,\n"
        "  \"ports\": [ { \"name\": \"a\", \"kind\": \"virtual\" } ],\n"
        "  \"links\": [\n"
        "    { \"mode\": \"duplex\",\n"
        "      \"sources\": [\"a\"],\n"
        "      \"targets\": [\"ghost\"] }\n"
        "  ]\n"
        "}\n");
    const json_validation missing = validate_json_document(missing_port);
    REQUIRE(!missing.valid());
    REQUIRE_EQ(vss_test::require_value(missing.problem).line, 7);

    const QString empty_name = QStringLiteral(
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"\",\n"
        "  \"ports\": []\n"
        "}\n");
    const json_validation unnamed = validate_json_document(empty_name);
    REQUIRE(!unnamed.valid());
    REQUIRE_EQ(vss_test::require_value(unnamed.problem).line, 3);
}

TEST_CASE("json editor: apply only when valid and dirty")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    TopologyJsonEditor editor(backend);
    editor.show();
    QApplication::processEvents();

    REQUIRE(!editor.is_dirty());
    REQUIRE(!editor.can_apply());
    REQUIRE(!editor.apply_button()->isEnabled());
    REQUIRE_EQ(editor.status_chip()->text(),
               QStringLiteral("Valid, version 1"));

    editor.set_text(valid_document());
    editor.revalidate();
    REQUIRE(editor.is_dirty());
    REQUIRE(editor.can_apply());
    REQUIRE(editor.apply_button()->isEnabled());
    REQUIRE(editor.apply());
    REQUIRE(editor.busy());
    REQUIRE(!editor.apply_button()->isEnabled());
    REQUIRE(!editor.apply());
    REQUIRE(vss_test::wait_until([&editor] { return !editor.busy(); }));
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
    REQUIRE(!editor.is_dirty());

    editor.set_text(QStringLiteral("{\n \"version\": 1,\n"));
    editor.revalidate();
    REQUIRE(!editor.can_apply());
    REQUIRE(!editor.apply_button()->isEnabled());
    REQUIRE(!editor.apply());
    REQUIRE_EQ(backend.ports().size(), std::size_t{2});
    REQUIRE(editor.error_summary().contains(QStringLiteral("Line")));

    editor.set_text(valid_document());
    editor.revalidate();
    QString unknown_mode = valid_document();
    unknown_mode.replace(QStringLiteral("\"duplex\""),
                         QStringLiteral("\"splt\""));
    editor.set_text(unknown_mode);
    editor.revalidate();
    REQUIRE(!editor.go_to_line_button()->isHidden());
    REQUIRE(!editor.fix_button()->isHidden());
    REQUIRE_EQ(editor.fix_button()->text(), QStringLiteral("Use \"split\""));
    editor.fix_button()->click();
    QApplication::processEvents();
    REQUIRE(editor.validation().valid());
}

TEST_CASE("event log: state changes and local events, not traffic")
{
    using namespace vss::gui;
    EventLogPanel log;
    event_batch batch;
    gui_event error;
    error.kind = vss::event_kind::error;
    error.name = QStringLiteral("modem");
    error.error = QStringLiteral("Parity error");
    batch.push_back(error);
    gui_event started;
    started.kind = vss::event_kind::started;
    started.name = QStringLiteral("terminal");
    batch.push_back(started);
    gui_event data;
    data.kind = vss::event_kind::data;
    data.name = QStringLiteral("terminal");
    data.data = vss::gui::event_payload(QByteArrayLiteral("hello"));
    batch.push_back(data);

    log.append(batch);
    REQUIRE_EQ(log.row_count(), 2);
    log.add_local(QStringLiteral("Applied the topology JSON."));
    REQUIRE_EQ(log.row_count(), 3);
    log.clear();
    REQUIRE_EQ(log.row_count(), 0);
}

TEST_CASE("benchmark: capability and link refusal are visible")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    BenchmarkDialog dialog(backend);
    dialog.show();
    QApplication::processEvents();
    REQUIRE(!dialog.can_run());
    REQUIRE_EQ(dialog.block_reason(),
               QStringLiteral("Add two ports to benchmark a connection."));

    const vss::endpoint_id first = make_port(backend, QStringLiteral("first"));
    const vss::endpoint_id second =
        make_port(backend, QStringLiteral("second"));
    QApplication::processEvents();
    REQUIRE(!dialog.ports_linked());
    REQUIRE_EQ(dialog.block_reason(),
               QStringLiteral("Not available in this build"));
    REQUIRE(!dialog.run_button()->isEnabled());
    REQUIRE_EQ(dialog.run_button()->toolTip(),
               QStringLiteral("Not available in this build"));

    REQUIRE(backend.connect_ports(first, second, {}).has_value());
    QApplication::processEvents();
    REQUIRE(dialog.ports_linked());
    REQUIRE(dialog.block_reason().contains(QStringLiteral("linked in Studio")));
    REQUIRE(dialog.run_button()->toolTip().contains(
        QStringLiteral("Disconnect them first")));
    REQUIRE(!dialog.can_run());

    capabilities caps;
    caps.benchmark = true;
    backend.set_capabilities(caps);
    REQUIRE(!dialog.can_run());
}

TEST_CASE("benchmark: a run reports what arrived")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.benchmark = true;
    backend.set_capabilities(caps);
    make_port(backend, QStringLiteral("first"));
    make_port(backend, QStringLiteral("second"));

    BenchmarkDialog dialog(backend);
    dialog.show();
    QApplication::processEvents();
    REQUIRE(dialog.can_run());
    REQUIRE(dialog.run_button()->isEnabled());
    REQUIRE_EQ(dialog.run_button()->text(), QStringLiteral("Run"));

    dialog.run_button()->click();
    REQUIRE(wait_for([&] { return !dialog.running(); }));

    // The dialog's defaults: 10 MB in 4 KB chunks, between the two ports.
    REQUIRE(backend.last_benchmark().has_value());
    REQUIRE_EQ(backend.last_benchmark()->bytes, std::uint64_t{10} << 20);
    REQUIRE_EQ(backend.last_benchmark()->chunk, std::size_t{4096});
    REQUIRE_EQ(dialog.result_chip()->text(), QStringLiteral("Passed"));
    REQUIRE_EQ(dialog.metric_text(0), QStringLiteral("10.0 MB"));
    REQUIRE_EQ(dialog.metric_text(1), QStringLiteral("10.0 MB"));
    REQUIRE_EQ(dialog.metric_text(2), QStringLiteral("0 B"));
    REQUIRE(dialog.finished_text().startsWith(QStringLiteral("Finished at ")));
    REQUIRE_EQ(dialog.progress_bar()->value(), 100);
    REQUIRE(dialog.copy_result_button()->isEnabled());
    REQUIRE(dialog.summary().startsWith(
        QStringLiteral("Passed: sent 10.0 MB, received 10.0 MB, lost 0 B")));
}

TEST_CASE("benchmark: a short count fails and says by how much")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.benchmark = true;
    backend.set_capabilities(caps);
    backend.set_benchmark_outcome(1024ULL * 1024, std::chrono::milliseconds(0));
    make_port(backend, QStringLiteral("first"));
    make_port(backend, QStringLiteral("second"));

    BenchmarkDialog dialog(backend);
    dialog.show();
    dialog.run_button()->click();
    REQUIRE(wait_for([&] { return !dialog.running(); }));

    REQUIRE_EQ(dialog.result_chip()->text(), QStringLiteral("Failed"));
    REQUIRE_EQ(dialog.metric_text(1), QStringLiteral("9.0 MB"));
    REQUIRE_EQ(dialog.metric_text(2), QStringLiteral("1.0 MB"));
    REQUIRE(dialog.summary().startsWith(QStringLiteral("Failed: sent 10.0 MB")));
}

TEST_CASE("benchmark: Cancel stops a run that is going")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.benchmark = true;
    backend.set_capabilities(caps);
    backend.set_benchmark_outcome(0, std::chrono::milliseconds(3000));
    make_port(backend, QStringLiteral("first"));
    make_port(backend, QStringLiteral("second"));

    BenchmarkDialog dialog(backend);
    dialog.show();
    dialog.run_button()->click();
    REQUIRE(dialog.running());
    REQUIRE_EQ(dialog.run_button()->text(), QStringLiteral("Cancel"));
    REQUIRE(dialog.run_button()->isEnabled());
    REQUIRE(!dialog.from_combo()->isEnabled());

    dialog.run_button()->click();
    REQUIRE(wait_for([&] { return !dialog.running(); }));
    REQUIRE_EQ(dialog.result_chip()->text(), QStringLiteral("Cancelled"));
    REQUIRE_EQ(dialog.run_button()->text(), QStringLiteral("Run"));
    REQUIRE(!dialog.copy_result_button()->isEnabled());
    REQUIRE(dialog.summary().isEmpty());
}

TEST_CASE("benchmark: a dialog closed in the middle of a run goes away cleanly")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    capabilities caps;
    caps.benchmark = true;
    backend.set_capabilities(caps);
    backend.set_benchmark_outcome(0, std::chrono::milliseconds(3000));
    make_port(backend, QStringLiteral("first"));
    make_port(backend, QStringLiteral("second"));

    {
        BenchmarkDialog dialog(backend);
        dialog.show();
        dialog.run_button()->click();
        REQUIRE(dialog.running());
        dialog.close();
    }
    QApplication::processEvents();
}

TEST_CASE("export command: uses the real file name")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    ExportCommandDialog dialog(backend, QStringLiteral("C:/tmp/lab.json"));
    REQUIRE_EQ(dialog.command(),
               QStringLiteral("vss_cli run lab.json --seconds=60"));
    dialog.set_document_path(QStringLiteral("C:/tmp/bench-1.json"));
    REQUIRE_EQ(dialog.command(),
               QStringLiteral("vss_cli run bench-1.json --seconds=60"));
    REQUIRE(dialog.panel() != nullptr);
}

TEST_CASE("advanced controls have accessible names and 24 px targets")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    const vss::endpoint_id modem =
        make_fixture(backend, QStringLiteral("modem"));
    const vss::endpoint_id terminal =
        make_port(backend, QStringLiteral("terminal"));
    const auto linked = backend.connect_ports(modem, terminal, {});
    REQUIRE(linked.has_value());
    vss::link_options options;
    options.policy = vss::merge_policy::owner;
    const vss::endpoint_id second =
        make_port(backend, QStringLiteral("second"));
    const auto joined = backend.join({terminal, second}, modem, options);
    REQUIRE(joined.has_value());

    WiringDialog wiring(backend, modem);
    wiring.show();
    QApplication::processEvents();
    check_accessible_controls(&wiring);

    AddFixtureDialog fixture(backend, terminal);
    fixture.show();
    QApplication::processEvents();
    check_accessible_controls(&fixture);

    BenchmarkDialog benchmark(backend);
    benchmark.show();
    QApplication::processEvents();
    check_accessible_controls(&benchmark);

    ExportCommandDialog export_command(backend, QString());
    export_command.show();
    QApplication::processEvents();
    check_accessible_controls(&export_command);

    TopologyJsonEditor editor(backend);
    editor.show();
    QApplication::processEvents();
    check_accessible_controls(&editor);

    OwnerBar owner(backend);
    owner.set_link(backend.link(*joined));
    owner.show();
    QApplication::processEvents();
    check_accessible_controls(&owner);

    EventLogPanel log;
    log.show();
    QApplication::processEvents();
    check_accessible_controls(&log);
}

TEST_CASE("advanced: render review screenshots in both themes")
{
    using namespace vss::gui;
    FakeStudioBackend backend;
    const vss::endpoint_id modem =
        make_fixture(backend, QStringLiteral("modem"));
    const vss::endpoint_id terminal =
        make_port(backend, QStringLiteral("terminal"));
    REQUIRE(backend.connect_ports(modem, terminal, {}).has_value());
    const vss::endpoint_id second =
        make_port(backend, QStringLiteral("second"));
    vss::link_options options;
    options.policy = vss::merge_policy::owner;
    const auto joined = backend.join({terminal, second}, modem, options);
    REQUIRE(joined.has_value());

    const QString directory = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("../gui-shots"));
    REQUIRE(QDir().mkpath(directory));
    const auto shoot =
        [&directory](QWidget& widget, const QString& name, const QSize& size)
    {
        widget.resize(size);
        widget.show();
        Theme::instance().set_mode(theme_mode::light);
        QApplication::processEvents();
        REQUIRE(widget.grab().save(
            QDir(directory).filePath(name + QStringLiteral("-light.png"))));
        Theme::instance().set_mode(theme_mode::dark);
        QApplication::processEvents();
        REQUIRE(widget.grab().save(
            QDir(directory).filePath(name + QStringLiteral("-dark.png"))));
        Theme::instance().set_mode(theme_mode::light);
        // hide(), not close(): a QMainWindow would run the discard prompt.
        widget.hide();
    };

    WiringDialog wiring(backend, modem);
    shoot(wiring, QStringLiteral("wiring"), QSize(1100, 760));

    AddFixtureDialog fixture(backend, terminal);
    shoot(fixture, QStringLiteral("add-fixture"), QSize(760, 620));

    BenchmarkDialog benchmark(backend);
    shoot(benchmark, QStringLiteral("benchmark"), QSize(760, 720));

    // A separate backend with one duplex link so the review shot shows the
    // valid state, not the deliberate loop above.
    FakeStudioBackend json_backend;
    const vss::endpoint_id json_modem =
        make_fixture(json_backend, QStringLiteral("modem"));
    const vss::endpoint_id json_terminal =
        make_port(json_backend, QStringLiteral("terminal"));
    REQUIRE(
        json_backend.connect_ports(json_modem, json_terminal, {}).has_value());
    TopologyJsonEditor editor(json_backend);
    shoot(editor, QStringLiteral("topology-json"), QSize(1100, 620));

    ExportCommandDialog export_command(backend,
                                       QStringLiteral("C:/tmp/lab.json"));
    shoot(export_command, QStringLiteral("export-command"), QSize(640, 320));

    OwnerBar owner(backend);
    owner.set_link(backend.link(*joined));
    shoot(owner, QStringLiteral("owner-bar"), QSize(900, 130));

    EventLogPanel log;
    shoot(log, QStringLiteral("event-log"), QSize(900, 400));

    ShellWindow shell(backend);
    shell.resize(1280, 800);
    shell.show();
    QApplication::processEvents();
    QAction* advanced =
        shell.findChild<QAction*>(QStringLiteral("view.advanced"));
    REQUIRE(advanced != nullptr);
    advanced->trigger();
    QApplication::processEvents();
    REQUIRE(advanced->isChecked());
    QAction* mock = shell.findChild<QAction*>(QStringLiteral("ports.mock"));
    REQUIRE(mock != nullptr);
    REQUIRE(mock->isVisible());
    auto* owner_bar = shell.findChild<OwnerBar*>();
    REQUIRE(owner_bar != nullptr);
    REQUIRE(owner_bar->has_link());
    shoot(shell, QStringLiteral("shell-advanced"), QSize(1280, 800));
}

TEST_CASE("shell: advanced docks follow the mode and the owner bar appears")
{
    using namespace vss::gui;
    clear_settings();
    FakeStudioBackend backend;
    const vss::endpoint_id recipe =
        make_port(backend, QStringLiteral("recipe-tool"));
    const vss::endpoint_id qa = make_port(backend, QStringLiteral("qa-runner"));
    const vss::endpoint_id device =
        make_port(backend, QStringLiteral("device"));

    ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    QApplication::processEvents();

    QAction* advanced =
        window.findChild<QAction*>(QStringLiteral("view.advanced"));
    QAction* log_action =
        window.findChild<QAction*>(QStringLiteral("view.log"));
    QAction* json_action =
        window.findChild<QAction*>(QStringLiteral("view.json"));
    REQUIRE(advanced != nullptr);
    REQUIRE(log_action != nullptr);
    REQUIRE(json_action != nullptr);
    auto* log_dock = window.findChild<QDockWidget*>(QStringLiteral("logDock"));
    auto* json_dock =
        window.findChild<QDockWidget*>(QStringLiteral("jsonDock"));
    REQUIRE(log_dock != nullptr);
    REQUIRE(json_dock != nullptr);
    REQUIRE(!log_action->isVisible());
    REQUIRE(log_dock->isHidden());

    advanced->trigger();
    QApplication::processEvents();
    REQUIRE(log_action->isVisible());
    REQUIRE(json_action->isVisible());
    REQUIRE(!log_dock->isHidden());
    REQUIRE(!json_dock->isHidden());

    auto* owner = window.findChild<OwnerBar*>();
    REQUIRE(owner != nullptr);
    REQUIRE(!owner->isVisible());

    vss::link_options options;
    options.policy = vss::merge_policy::owner;
    const auto joined = backend.join({recipe, qa}, device, options);
    REQUIRE(joined.has_value());
    QApplication::processEvents();
    REQUIRE(owner->has_link());
    REQUIRE_EQ(owner->summary(),
               QStringLiteral("recipe-tool and qa-runner into device"));

    advanced->trigger();
    QApplication::processEvents();
    REQUIRE(!log_action->isVisible());
    REQUIRE(log_dock->isHidden());
    REQUIRE(json_dock->isHidden());
}

namespace
{

QString bus_document(const QString& links)
{
    return QStringLiteral(
               "{\"version\":1,\"name\":\"lab\",\"ports\":["
               "{\"name\":\"a\",\"kind\":\"virtual\"},"
               "{\"name\":\"b\",\"kind\":\"virtual\"},"
               "{\"name\":\"c\",\"kind\":\"virtual\"},"
               "{\"name\":\"tap\",\"kind\":\"virtual\"}],\"links\":[") +
           links + QStringLiteral("]}");
}

} // namespace

TEST_CASE("json: a bus with a tap is valid and round trips")
{
    using namespace vss::gui;
    const json_validation good =
        validate_json_document(bus_document(QStringLiteral(
            "{\"mode\":\"bus\",\"members\":[\"a\",\"b\",\"c\"]},"
            "{\"mode\":\"join\",\"sources\":[\"a\",\"b\",\"c\"],"
            "\"targets\":[\"tap\"],\"emulate_modem_lines\":false}")));
    REQUIRE(good.valid());
    REQUIRE_EQ(good.links, 2);
    REQUIRE(good.formatted.contains(QStringLiteral("\"mode\": \"bus\"")));
    REQUIRE(good.formatted.contains(QStringLiteral("\"members\"")));
}

TEST_CASE("json: a bus is checked for size, names, membership and loops")
{
    using namespace vss::gui;
    const auto problem_of = [](const QString& links)
    {
        const json_validation checked =
            validate_json_document(bus_document(links));
        REQUIRE(!checked.valid());
        REQUIRE(checked.problem.has_value());
        return *checked.problem;
    };

    REQUIRE(problem_of(QStringLiteral("{\"mode\":\"bus\",\"members\":[\"a\"]}"))
                .summary.contains(QStringLiteral("between 2 and 64 ports")));
    REQUIRE(problem_of(QStringLiteral("{\"mode\":\"bus\"}"))
                .summary.contains(QStringLiteral("between 2 and 64 ports")));
    REQUIRE(problem_of(QStringLiteral(
                           "{\"mode\":\"bus\",\"members\":[\"a\",\"zzz\"]}"))
                .summary.contains(QStringLiteral("No port is called \"zzz\"")));
    REQUIRE(problem_of(QStringLiteral("{\"mode\":\"bus\",\"members\":[\"a\","
                                      "\"b\"]},"
                                      "{\"mode\":\"bus\",\"members\":[\"b\","
                                      "\"c\"]}"))
                .summary.contains(QStringLiteral("one bus only")));
    // A route out of the bus and back into it is a loop.
    REQUIRE(problem_of(
                QStringLiteral("{\"mode\":\"redirect\",\"sources\":[\"a\"],"
                               "\"targets\":[\"tap\"]},"
                               "{\"mode\":\"redirect\",\"sources\":[\"tap\"],"
                               "\"targets\":[\"b\"]},"
                               "{\"mode\":\"bus\",\"members\":[\"a\",\"b\"]}"))
                .summary.contains(QStringLiteral("loop")));
}

TEST_CASE("json: a bus is refused plainly where the build has no bus")
{
    using namespace vss::gui;
    const json_validation refused = validate_json_document(
        bus_document(
            QStringLiteral("{\"mode\":\"bus\",\"members\":[\"a\",\"b\"]}")),
        false);
    REQUIRE(!refused.valid());
    REQUIRE(refused.problem.has_value());
    REQUIRE(refused.problem->summary.contains(
        QStringLiteral("Bus links are not available in this build")));

    // A file without a bus is unaffected.
    REQUIRE(validate_json_document(valid_document(), false).valid());
}

TEST_CASE("json: a link's delay and jitter are accepted and placed when wrong")
{
    using namespace vss::gui;
    const auto document = [](const QString& extra)
    {
        return QStringLiteral(
                   "{\n\"version\":1,\n\"name\":\"lab\",\n\"ports\":[\n"
                   "{\"name\":\"a\",\"kind\":\"virtual\"},\n"
                   "{\"name\":\"b\",\"kind\":\"virtual\"}],\n\"links\":[\n"
                   "{\"mode\":\"duplex\",\"sources\":[\"a\"],"
                   "\"targets\":[\"b\"],\n") +
               extra + QStringLiteral("}]}");
    };

    const json_validation fine = validate_json_document(
        document(QStringLiteral("\"delay_ms\":190,\"jitter_ms\":10")));
    REQUIRE(fine.valid());
    REQUIRE(fine.formatted.contains(QStringLiteral("\"delay_ms\": 190")));

    const json_validation too_long = validate_json_document(
        document(QStringLiteral("\"delay_ms\":70000")));
    REQUIRE(!too_long.valid());
    REQUIRE(too_long.problem.has_value());
    REQUIRE_EQ(too_long.problem->line, 9);

    const json_validation too_jittery = validate_json_document(document(
        QStringLiteral("\"delay_ms\":10,\n\"jitter_ms\":20")));
    REQUIRE(!too_jittery.valid());
    REQUIRE(too_jittery.problem.has_value());
    REQUIRE_EQ(too_jittery.problem->line, 10);
}

namespace
{

/// A document whose one link is followed by `extra`, written on lines of its
/// own so the tests can name the line of each member.
QString half_duplex_document(const QString& mode, const QString& extra)
{
    const QString ends = mode == QStringLiteral("bus")
                             ? QStringLiteral("\"members\":[\"a\",\"b\"]")
                             : QStringLiteral("\"sources\":[\"a\"],"
                                              "\"targets\":[\"b\"]");
    return QStringLiteral("{\n\"version\":1,\n\"name\":\"lab\",\n\"ports\":[\n"
                          "{\"name\":\"a\",\"kind\":\"virtual\"},\n"
                          "{\"name\":\"b\",\"kind\":\"virtual\"}],\n"
                          "\"links\":[\n"
                          "{\"mode\":\"") +
           mode + QStringLiteral("\",\n") + ends +
           (extra.isEmpty() ? QString() : QStringLiteral(",\n") + extra) +
           QStringLiteral("}]}");
}

} // namespace

TEST_CASE("json: a half-duplex bus is accepted and round trips")
{
    using namespace vss::gui;
    const json_validation good = validate_json_document(half_duplex_document(
        QStringLiteral("bus"),
        QStringLiteral("\"delay_ms\":5,\n\"half_duplex\":{\"bit_rate\":19200,"
                       "\"turnaround_ms\":2,\"collisions\":\"garble\","
                       "\"rts_gating\":true}")));
    REQUIRE(good.valid());
    REQUIRE(good.formatted.contains(QStringLiteral("\"bit_rate\": 19200")));
    REQUIRE(
        good.formatted.contains(QStringLiteral("\"collisions\": \"garble\"")));

    // Every member is optional; the object being there turns the option on.
    REQUIRE(validate_json_document(
                half_duplex_document(QStringLiteral("bus"),
                                     QStringLiteral("\"half_duplex\":{}")))
                .valid());
}

TEST_CASE("json: a half-duplex bus is refused plainly where the build has none")
{
    using namespace vss::gui;
    const QString text = half_duplex_document(
        QStringLiteral("bus"), QStringLiteral("\"half_duplex\":{}"));
    const json_validation refused = validate_json_document(text, true, false);
    REQUIRE(!refused.valid());
    const json_problem problem = vss_test::require_value(refused.problem);
    REQUIRE(problem.summary.contains(
        QStringLiteral("Half-duplex buses are not available in this build")));
    REQUIRE_EQ(problem.detail,
               QStringLiteral("This build cannot run a bus as a half-duplex "
                              "line. Remove \"half_duplex\" from the link."));
    REQUIRE_EQ(problem.line, 10);
    REQUIRE(problem.summary.startsWith(QStringLiteral("Line 10:")));

    // The same file is fine with the capability, and a file without the
    // member validates as before whatever the capability.
    REQUIRE(validate_json_document(text, true, true).valid());
    const QString plain = half_duplex_document(QStringLiteral("bus"), {});
    REQUIRE(validate_json_document(plain, true, false).valid());
    REQUIRE(validate_json_document(plain, true, true).valid());
    REQUIRE(validate_json_document(valid_document(), true, false).valid());

    // A build without buses still says that first.
    const json_validation no_bus = validate_json_document(text, false, false);
    REQUIRE(
        vss_test::require_value(no_bus.problem)
            .summary.contains(QStringLiteral("Bus links are not available")));
}

TEST_CASE("json: a half-duplex line is checked and each error is placed")
{
    using namespace vss::gui;
    const auto problem_of = [](const QString& mode, const QString& extra)
    {
        const json_validation checked =
            validate_json_document(half_duplex_document(mode, extra));
        REQUIRE(!checked.valid());
        return vss_test::require_value(checked.problem);
    };

    // Only a bus can be a half-duplex line.
    const json_problem duplex = problem_of(
        QStringLiteral("duplex"), QStringLiteral("\"half_duplex\":{}"));
    REQUIRE(duplex.summary.contains(
        QStringLiteral("Only a bus can be a half-duplex line")));
    REQUIRE_EQ(duplex.detail,
               QStringLiteral("Remove \"half_duplex\", or make the link a "
                              "bus."));
    REQUIRE_EQ(duplex.line, 10);

    const json_problem not_object = problem_of(
        QStringLiteral("bus"), QStringLiteral("\"half_duplex\":true"));
    REQUIRE(not_object.summary.contains(
        QStringLiteral("\"half_duplex\" must be an object")));
    REQUIRE_EQ(not_object.line, 10);

    // The line of the member, not of the object that holds it.
    const auto inside = [&problem_of](const QString& members)
    {
        return problem_of(QStringLiteral("bus"),
                          QStringLiteral("\"half_duplex\":{\n") + members +
                              QStringLiteral("\n}"));
    };
    const json_problem slow = inside(QStringLiteral("\"bit_rate\":10"));
    REQUIRE(slow.summary.contains(
        QStringLiteral("The line rate must be between 50 and 4000000 bit/s")));
    REQUIRE_EQ(slow.line, 11);
    REQUIRE_EQ(inside(QStringLiteral("\"bit_rate\":4000001")).line, 11);
    REQUIRE_EQ(inside(QStringLiteral("\"bit_rate\":9600.5")).line, 11);
    REQUIRE_EQ(inside(QStringLiteral("\"bit_rate\":\"fast\"")).line, 11);

    const json_problem turn =
        inside(QStringLiteral("\"bit_rate\":9600,\n\"turnaround_ms\":60001"));
    REQUIRE(turn.summary.contains(
        QStringLiteral("The turn-around must be between 0 and 60000 ms")));
    REQUIRE_EQ(turn.line, 12);
    REQUIRE_EQ(inside(QStringLiteral("\"turnaround_ms\":-1")).line, 11);

    const json_problem action =
        inside(QStringLiteral("\"collisions\":\"stop\""));
    REQUIRE(action.summary.contains(
        QStringLiteral("\"collisions\" must be report or garble")));
    REQUIRE(action.detail.contains(QStringLiteral("Use report or garble")));
    REQUIRE_EQ(action.line, 11);
    REQUIRE_EQ(inside(QStringLiteral("\"collisions\":3")).line, 11);

    const json_problem gating =
        inside(QStringLiteral("\"rts_gating\":\"yes\""));
    REQUIRE(gating.summary.contains(
        QStringLiteral("\"rts_gating\" must be true or false")));
    REQUIRE_EQ(gating.line, 11);
}

TEST_CASE("event log: a bus fault shows its sentence")
{
    using namespace vss::gui;
    EventLogPanel log;
    gui_event fault;
    fault.kind = vss::event_kind::bus_fault;
    fault.name = QStringLiteral("p1");
    fault.message = QStringLiteral("sent 8 bytes while p0 was sending");
    log.append({fault});
    REQUIRE_EQ(log.row_count(), 1);

    auto* table = log.findChild<QTableWidget*>();
    REQUIRE(table != nullptr);
    REQUIRE_EQ(table->item(0, 1)->text(), QStringLiteral("p1"));
    REQUIRE_EQ(table->item(0, 2)->text(), QStringLiteral("Bus"));
    REQUIRE_EQ(table->item(0, 3)->text(), fault.message);
}

TEST_CASE("link settings: a bus shows the half-duplex line and returns it")
{
    using namespace vss::gui;
    link_view bus;
    bus.shape = link_shape::bus;
    bus.mode = vss::link_mode::bus;
    bus.sources = {1, 2, 3};
    bus.options.delay_ms = 9;
    bus.options.half_duplex.enabled = true;
    bus.options.half_duplex.bit_rate = 2400;
    bus.options.half_duplex.turnaround_ms = 6;
    bus.options.half_duplex.on_collision = vss::collision_action::garble;
    bus.options.half_duplex.rts_gating = true;

    LinkSettingsDialog dialog(bus, QStringLiteral("a, b and c"), true);
    dialog.show();
    QApplication::processEvents();
    REQUIRE(dialog.half_duplex_shown());
    HalfDuplexFields* fields = dialog.half_duplex_fields();
    REQUIRE(fields != nullptr);
    REQUIRE(fields->isVisibleTo(&dialog));
    REQUIRE(fields->enable_box()->isChecked());
    REQUIRE_EQ(fields->bit_rate_spin()->value(), 2400);
    REQUIRE(fields->rows_enabled());
    REQUIRE(dialog.options().half_duplex == bus.options.half_duplex);
    REQUIRE_EQ(dialog.options().delay_ms, 9u);

    fields->bit_rate_spin()->setValue(115200);
    fields->collisions_combo()->setCurrentIndex(0);
    REQUIRE(dialog.apply_button()->isEnabled());
    REQUIRE_EQ(dialog.options().half_duplex.bit_rate, 115200u);
    REQUIRE(dialog.options().half_duplex.on_collision ==
            vss::collision_action::report);

    // Unchecking the choice turns the line off whatever the rows hold.
    fields->enable_box()->setChecked(false);
    REQUIRE(!dialog.options().half_duplex.enabled);
    REQUIRE(!fields->rows_enabled());
}

TEST_CASE("link settings: a bus without the capability, and other links")
{
    using namespace vss::gui;
    link_view bus;
    bus.shape = link_shape::bus;
    bus.mode = vss::link_mode::bus;
    bus.sources = {1, 2};
    LinkSettingsDialog limited(bus, QStringLiteral("a and b"), false);
    limited.show();
    QApplication::processEvents();
    REQUIRE(limited.half_duplex_shown());
    REQUIRE(!limited.half_duplex_fields()->enable_box()->isEnabled());
    REQUIRE_EQ(limited.half_duplex_fields()->enable_box()->toolTip(),
               QStringLiteral("Not available in this build"));
    REQUIRE(!limited.options().half_duplex.enabled);

    link_view pair;
    pair.shape = link_shape::two_way;
    pair.sources = {1};
    pair.targets = {2};
    LinkSettingsDialog plain(pair, QStringLiteral("a and b"), true);
    plain.show();
    QApplication::processEvents();
    REQUIRE(!plain.half_duplex_shown());
    REQUIRE(!plain.half_duplex_fields()->isVisibleTo(&plain));
    REQUIRE(plain.options().half_duplex == vss::half_duplex_options{});
}
