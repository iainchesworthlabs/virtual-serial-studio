//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "dialogs/connect_ports.hpp"
#include "widgets/half_duplex_fields.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>

#include <vector>

namespace
{

struct fixture_ports
{
    vss::endpoint_id alpha = 0;
    vss::endpoint_id beta = 0;
    vss::endpoint_id gamma = 0;
    vss::endpoint_id delta = 0;
    vss::endpoint_id epsilon = 0;
};

fixture_ports make_ports(vss::gui::FakeStudioBackend& backend)
{
    fixture_ports ports;
    const auto make =
        [&backend](const QString& name, vss::virtual_port_kind kind)
    {
        const auto created = backend.create_virtual_port(name, kind, true);
        REQUIRE(created.has_value());
        return *created;
    };
    ports.alpha = make(QStringLiteral("alpha"), vss::virtual_port_kind::pty);
    ports.beta = make(QStringLiteral("beta"), vss::virtual_port_kind::pty);
    ports.gamma =
        make(QStringLiteral("gamma"), vss::virtual_port_kind::loopback);
    ports.delta = make(QStringLiteral("delta"), vss::virtual_port_kind::pty);
    ports.epsilon =
        make(QStringLiteral("epsilon"), vss::virtual_port_kind::loopback);
    backend.refresh();
    return ports;
}

void show(QDialog& dialog)
{
    dialog.show();
    QApplication::processEvents();
}

vss::gui::link_view only_link(const vss::gui::FakeStudioBackend& backend)
{
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
    return backend.links().front();
}

} // namespace

TEST_CASE("connect: every mode makes the right link")
{
    {
        vss::gui::FakeStudioBackend backend;
        const fixture_ports ports = make_ports(backend);
        vss::gui::ConnectPortsDialog dialog(backend);
        show(dialog);
        REQUIRE_EQ(dialog.current_mode(),
                   vss::gui::ConnectPortsDialog::mode::two_way);
        dialog.select_from(ports.alpha);
        dialog.select_to(ports.beta);
        REQUIRE(dialog.primary_enabled());
        REQUIRE(dialog.submit());
        REQUIRE_EQ(backend.links().size(), std::size_t{1});
        const vss::gui::link_view link = only_link(backend);
        REQUIRE_EQ(link.shape, vss::gui::link_shape::two_way);
        REQUIRE_EQ(link.sources.size(), 1);
        REQUIRE_EQ(link.sources.front(), ports.alpha);
        REQUIRE_EQ(link.targets.front(), ports.beta);
        REQUIRE(dialog.created_link().has_value());
    }
    {
        vss::gui::FakeStudioBackend backend;
        const fixture_ports ports = make_ports(backend);
        vss::gui::ConnectPortsDialog dialog(backend);
        show(dialog);
        REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::redirect));
        dialog.select_from(ports.alpha);
        dialog.select_to(ports.beta);
        REQUIRE(dialog.submit());
        REQUIRE_EQ(only_link(backend).shape, vss::gui::link_shape::one_way);
    }
    {
        vss::gui::FakeStudioBackend backend;
        const fixture_ports ports = make_ports(backend);
        vss::gui::ConnectPortsDialog dialog(backend);
        show(dialog);
        REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::monitor));
        dialog.select_from(ports.alpha);
        dialog.select_to(ports.beta);
        REQUIRE(dialog.submit());
        const vss::gui::link_view link = only_link(backend);
        REQUIRE_EQ(link.shape, vss::gui::link_shape::monitor);
        REQUIRE(!link.options.emulate_modem_lines);
    }
    {
        vss::gui::FakeStudioBackend backend;
        const fixture_ports ports = make_ports(backend);
        vss::gui::ConnectPortsDialog dialog(backend);
        show(dialog);
        REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::split));
        dialog.select_split_source(ports.alpha);
        dialog.set_target_ids({ports.beta, ports.delta});
        REQUIRE(dialog.submit());
        const vss::gui::link_view link = only_link(backend);
        REQUIRE_EQ(link.shape, vss::gui::link_shape::split);
        REQUIRE_EQ(link.sources.front(), ports.alpha);
        REQUIRE_EQ(link.targets.size(), 2);
    }
    {
        vss::gui::FakeStudioBackend backend;
        const fixture_ports ports = make_ports(backend);
        vss::gui::ConnectPortsDialog dialog(backend);
        show(dialog);
        REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::join));
        dialog.select_join_target(ports.delta);
        dialog.set_source_ids({ports.alpha, ports.beta});
        REQUIRE(dialog.submit());
        const vss::gui::link_view link = only_link(backend);
        REQUIRE_EQ(link.shape, vss::gui::link_shape::join);
        REQUIRE_EQ(link.targets.front(), ports.delta);
        REQUIRE_EQ(link.sources.size(), 2);
    }
}

TEST_CASE("connect: a real device links in every mode")
{
    // "Real device" attaches a port of kind physical. The connect dialog does
    // not filter by kind, so it takes part in each mode like any other port.
    using mode = vss::gui::ConnectPortsDialog::mode;
    const auto with_device =
        [](vss::gui::FakeStudioBackend& backend, vss::endpoint_id& device,
           vss::endpoint_id& first, vss::endpoint_id& second)
    {
        const auto attached =
            backend.attach_serial(QStringLiteral("device"),
                                  QStringLiteral("serial://COM3"), {}, true);
        REQUIRE(attached.has_value());
        device = *attached;
        REQUIRE(backend.port(device)->kind == vss::endpoint_kind::physical);
        first = *backend.create_virtual_port(QStringLiteral("first"),
                                             vss::virtual_port_kind::pty, true);
        second = *backend.create_virtual_port(
            QStringLiteral("second"), vss::virtual_port_kind::loopback, true);
        backend.refresh();
    };

    for (const mode chosen : {mode::two_way, mode::redirect, mode::monitor})
    {
        vss::gui::FakeStudioBackend backend;
        vss::endpoint_id device = 0;
        vss::endpoint_id first = 0;
        vss::endpoint_id second = 0;
        with_device(backend, device, first, second);
        vss::gui::ConnectPortsDialog dialog(backend);
        show(dialog);
        REQUIRE(dialog.set_mode(chosen));
        dialog.select_from(device);
        dialog.select_to(first);
        REQUIRE(dialog.primary_enabled());
        REQUIRE(dialog.submit());
        const vss::gui::link_view link = only_link(backend);
        REQUIRE_EQ(link.sources.front(), device);
        REQUIRE_EQ(link.targets.front(), first);
    }
    {
        vss::gui::FakeStudioBackend backend;
        vss::endpoint_id device = 0;
        vss::endpoint_id first = 0;
        vss::endpoint_id second = 0;
        with_device(backend, device, first, second);
        vss::gui::ConnectPortsDialog dialog(backend);
        show(dialog);
        REQUIRE(dialog.set_mode(mode::split));
        dialog.select_split_source(device);
        dialog.set_target_ids({first, second});
        REQUIRE(dialog.submit());
        REQUIRE_EQ(only_link(backend).shape, vss::gui::link_shape::split);
        REQUIRE_EQ(only_link(backend).sources.front(), device);
    }
    {
        vss::gui::FakeStudioBackend backend;
        vss::endpoint_id device = 0;
        vss::endpoint_id first = 0;
        vss::endpoint_id second = 0;
        with_device(backend, device, first, second);
        vss::gui::ConnectPortsDialog dialog(backend);
        show(dialog);
        REQUIRE(dialog.set_mode(mode::join));
        dialog.select_join_target(device);
        dialog.set_source_ids({first, second});
        REQUIRE(dialog.submit());
        REQUIRE_EQ(only_link(backend).shape, vss::gui::link_shape::join);
        REQUIRE_EQ(only_link(backend).targets.front(), device);
    }
    {
        // A real device and a loopback are not both Studio only, so the pair
        // is allowed, unlike two loopbacks.
        vss::gui::FakeStudioBackend backend;
        vss::endpoint_id device = 0;
        vss::endpoint_id first = 0;
        vss::endpoint_id second = 0;
        with_device(backend, device, first, second);
        vss::gui::ConnectPortsDialog dialog(backend);
        show(dialog);
        dialog.select_from(device);
        dialog.select_to(second);
        REQUIRE(dialog.primary_enabled());
    }
}

TEST_CASE("connect: result sentences and the live count line")
{
    vss::gui::FakeStudioBackend backend;
    const fixture_ports ports = make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend);
    show(dialog);

    dialog.select_from(ports.alpha);
    dialog.select_to(ports.beta);
    REQUIRE_EQ(dialog.accessible_result_text(),
               QStringLiteral("Everything alpha sends arrives at beta, and the "
                              "other way round."));

    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::redirect));
    REQUIRE_EQ(dialog.accessible_result_text(),
               QStringLiteral("Everything alpha sends arrives at beta. beta "
                              "cannot reply."));

    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::monitor));
    REQUIRE_EQ(dialog.accessible_result_text(),
               QStringLiteral("beta receives a read-only copy of what alpha "
                              "sends. It never drives modem lines."));

    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::split));
    dialog.select_split_source(ports.alpha);
    dialog.set_target_ids({ports.beta, ports.delta});
    REQUIRE_EQ(dialog.count_text(),
               QStringLiteral("2 ports chosen. That is enough for a split."));
    REQUIRE_EQ(dialog.accessible_result_text(),
               QStringLiteral("Everything alpha receives is copied to beta and "
                              "delta. Whatever either of them writes goes back "
                              "to alpha."));
    REQUIRE(dialog.primary_enabled());

    dialog.set_target_ids({ports.beta});
    REQUIRE_EQ(dialog.count_text(), QStringLiteral("Choose at least 2 ports."));
    REQUIRE(!dialog.primary_enabled());
    dialog.set_target_ids({});
    REQUIRE(!dialog.primary_enabled());
    dialog.set_target_ids({ports.beta, ports.delta});
    REQUIRE(dialog.primary_enabled());

    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::join));
    dialog.select_join_target(ports.delta);
    dialog.set_source_ids({ports.alpha, ports.beta});
    REQUIRE_EQ(dialog.count_text(),
               QStringLiteral("2 ports chosen. That is enough for a join."));
    REQUIRE_EQ(dialog.accessible_result_text(),
               QStringLiteral("Everything alpha and beta write goes to delta. "
                              "Their bytes are mixed in the order they arrive. "
                              "delta does not send anything back to them."));
}

TEST_CASE("connect: self-links and two loopback ports are blocked")
{
    vss::gui::FakeStudioBackend backend;
    const fixture_ports ports = make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend);
    show(dialog);

    dialog.select_from(ports.alpha);
    dialog.select_to(ports.alpha);
    REQUIRE(!dialog.primary_enabled());
    REQUIRE_EQ(dialog.block_reason(),
               QStringLiteral("A port cannot link to itself. Choose a "
                              "different port."));
    REQUIRE(!dialog.submit());
    REQUIRE(backend.links().empty());

    dialog.select_from(ports.gamma);
    dialog.select_to(ports.epsilon);
    REQUIRE(!dialog.two_way_allowed());
    REQUIRE(!dialog.primary_enabled());
    REQUIRE_EQ(
        dialog.block_reason(),
        QStringLiteral("Studio-only ports echo what they receive, so a two-way "
                       "link between them would repeat data. Use one-way, or "
                       "link a real or paired port."));
    REQUIRE(!dialog.set_mode(vss::gui::ConnectPortsDialog::mode::two_way));

    dialog.select_to(ports.delta);
    REQUIRE(dialog.two_way_allowed());
    REQUIRE(dialog.primary_enabled());
    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::redirect));
    REQUIRE(dialog.submit());
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
}

TEST_CASE("connect: a refused loop keeps the dialog open")
{
    vss::gui::FakeStudioBackend backend;
    const fixture_ports ports = make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend);
    show(dialog);
    dialog.select_from(ports.alpha);
    dialog.select_to(ports.beta);

    backend.fail_next(vss::make_error_code(vss::errc::cycle_detected));
    REQUIRE(!dialog.submit());
    REQUIRE(dialog.error_visible());
    REQUIRE_EQ(dialog.error_title(),
               QStringLiteral("That link would create a loop"));
    REQUIRE(
        dialog.error_body().contains(QStringLiteral("Nothing was changed")));
    REQUIRE(dialog.last_error().contains(QStringLiteral("cycle")));
    REQUIRE(!dialog.created_link().has_value());
    REQUIRE(backend.links().empty());

    dialog.select_to(ports.delta);
    REQUIRE(!dialog.error_visible());
    REQUIRE(dialog.submit());
    REQUIRE_EQ(backend.links().size(), std::size_t{1});
}

TEST_CASE("connect: the Advanced owner option changes the merge policy")
{
    vss::gui::FakeStudioBackend backend;
    const fixture_ports ports = make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend);
    show(dialog);
    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::join));
    dialog.select_join_target(ports.delta);
    dialog.set_source_ids({ports.alpha, ports.beta});

    REQUIRE(dialog.owner_check() != nullptr);
    REQUIRE(!dialog.owner_check()->isEnabled());
    int advanced_requests = 0;
    QObject::connect(&dialog, &vss::gui::ConnectPortsDialog::advanced_requested,
                     [&advanced_requests] { ++advanced_requests; });
    QPushButton* advanced = nullptr;
    for (QPushButton* button : dialog.findChildren<QPushButton*>())
    {
        if (button->text() == QStringLiteral("Turn on Advanced mode"))
            advanced = button;
    }
    REQUIRE(advanced != nullptr);
    advanced->click();
    QApplication::processEvents();
    REQUIRE_EQ(advanced_requests, 1);
    REQUIRE(dialog.advanced());
    REQUIRE(dialog.owner_check()->isEnabled());
    dialog.owner_check()->setChecked(true);
    REQUIRE(dialog.submit());
    REQUIRE_EQ(only_link(backend).options.policy, vss::merge_policy::owner);
}

TEST_CASE("connect: without Advanced the join keeps interleaving")
{
    vss::gui::FakeStudioBackend backend;
    const fixture_ports ports = make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend);
    show(dialog);
    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::join));
    dialog.select_join_target(ports.delta);
    dialog.set_source_ids({ports.alpha, ports.beta});
    REQUIRE(dialog.submit());
    REQUIRE_EQ(only_link(backend).options.policy,
               vss::merge_policy::interleave);
}

namespace
{

/// A backend whose engine can make a shared bus, as the real one does.
void allow_bus(vss::gui::FakeStudioBackend& backend)
{
    vss::gui::capabilities caps = backend.caps();
    caps.shared_bus = true;
    backend.set_capabilities(caps);
}

} // namespace

TEST_CASE("connect: a shared bus puts the chosen ports on one link")
{
    vss::gui::FakeStudioBackend backend;
    allow_bus(backend);
    const fixture_ports ports = make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend);
    show(dialog);

    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::bus));
    REQUIRE_EQ(dialog.current_mode(), vss::gui::ConnectPortsDialog::mode::bus);

    dialog.set_member_ids({ports.alpha, ports.beta, ports.delta});
    REQUIRE_EQ(dialog.count_text(),
               QStringLiteral("3 ports chosen. That is enough for a bus."));
    REQUIRE_EQ(dialog.accessible_result_text(),
               QStringLiteral("Each of alpha, beta and delta hears what the "
                              "others write, and none hears itself. Their "
                              "bytes are mixed in the order they arrive."));
    REQUIRE(dialog.primary_enabled());

    dialog.set_member_ids({ports.alpha});
    REQUIRE_EQ(dialog.count_text(), QStringLiteral("Choose at least 2 ports."));
    REQUIRE(!dialog.primary_enabled());
    REQUIRE_EQ(dialog.accessible_result_text(),
               QStringLiteral("Choose the ports to put on the bus."));

    dialog.set_member_ids({ports.alpha, ports.beta, ports.delta});
    REQUIRE(dialog.submit());
    const vss::gui::link_view link = only_link(backend);
    REQUIRE_EQ(link.shape, vss::gui::link_shape::bus);
    REQUIRE_EQ(link.mode, vss::link_mode::bus);
    REQUIRE_EQ(link.sources.size(), 3);
    REQUIRE(link.targets.isEmpty());
    REQUIRE(!link.options.emulate_modem_lines);
    REQUIRE_EQ(dialog.result_summary(),
               QStringLiteral("Put alpha, beta and delta on a shared bus."));
}

TEST_CASE("connect: the bus is disabled where the build has none")
{
    vss::gui::FakeStudioBackend backend;
    REQUIRE(!backend.caps().shared_bus);
    make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend);
    show(dialog);

    // The other five modes still work; the star stays the way to share.
    REQUIRE(!dialog.set_mode(vss::gui::ConnectPortsDialog::mode::bus));
    REQUIRE_EQ(dialog.current_mode(),
               vss::gui::ConnectPortsDialog::mode::two_way);

    QRadioButton* radio = nullptr;
    for (QRadioButton* candidate : dialog.findChildren<QRadioButton*>())
    {
        if (candidate->accessibleName() == QStringLiteral("Shared bus"))
            radio = candidate;
    }
    REQUIRE(radio != nullptr);
    REQUIRE(!radio->isEnabled());
    REQUIRE_EQ(radio->parentWidget()->toolTip(),
               QStringLiteral("Not available in this build"));
}

TEST_CASE("connect: a bus refused by the engine says why and stays open")
{
    vss::gui::FakeStudioBackend backend;
    allow_bus(backend);
    const fixture_ports ports = make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend);
    show(dialog);
    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::bus));
    dialog.set_member_ids({ports.alpha, ports.beta});

    backend.fail_next(vss::make_error_code(vss::errc::cycle_detected));
    REQUIRE(!dialog.submit());
    REQUIRE(dialog.error_visible());
    REQUIRE_EQ(dialog.error_title(),
               QStringLiteral("That link would create a loop"));
    REQUIRE(dialog.error_body().contains(QStringLiteral("out of the bus")));
    REQUIRE(backend.links().empty());

    backend.fail_next(vss::make_error_code(vss::errc::already_exists));
    REQUIRE(!dialog.submit());
    REQUIRE(dialog.error_body().contains(
        QStringLiteral("A port can be on one bus only")));

    REQUIRE(dialog.submit());
    REQUIRE_EQ(only_link(backend).shape, vss::gui::link_shape::bus);
}

namespace
{

/// A backend that makes a bus and, when asked, a half-duplex bus.
void allow_half_duplex(vss::gui::FakeStudioBackend& backend, bool allowed)
{
    vss::gui::capabilities caps = backend.caps();
    caps.shared_bus = true;
    caps.half_duplex_bus = allowed;
    backend.set_capabilities(caps);
}

} // namespace

TEST_CASE("connect: the bus page has the half-duplex controls at Advanced")
{
    vss::gui::FakeStudioBackend backend;
    allow_half_duplex(backend, true);
    make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend, 0, true);
    show(dialog);
    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::bus));

    const auto find = [&dialog](const char* name) -> QWidget*
    { return dialog.findChild<QWidget*>(QString::fromLatin1(name)); };
    auto* enable = qobject_cast<QCheckBox*>(find("busHalfDuplex"));
    auto* rate = qobject_cast<QSpinBox*>(find("busBitRate"));
    auto* turnaround = qobject_cast<QSpinBox*>(find("busTurnaround"));
    auto* collisions = qobject_cast<QComboBox*>(find("busCollisions"));
    auto* gating = qobject_cast<QCheckBox*>(find("busRtsGating"));
    REQUIRE(enable != nullptr);
    REQUIRE(rate != nullptr);
    REQUIRE(turnaround != nullptr);
    REQUIRE(collisions != nullptr);
    REQUIRE(gating != nullptr);
    REQUIRE(enable->isVisibleTo(&dialog));
    REQUIRE_EQ(enable->text(), QStringLiteral("Half-duplex line"));
    REQUIRE_EQ(rate->minimum(), static_cast<int>(vss::min_bus_bit_rate));
    REQUIRE_EQ(rate->maximum(), static_cast<int>(vss::max_bus_bit_rate));
    REQUIRE_EQ(rate->value(), 9600);
    REQUIRE_EQ(rate->suffix(), QStringLiteral(" bit/s"));
    REQUIRE_EQ(turnaround->minimum(), 0);
    REQUIRE_EQ(turnaround->maximum(), static_cast<int>(vss::max_link_delay_ms));
    REQUIRE_EQ(turnaround->suffix(), QStringLiteral(" ms"));
    REQUIRE_EQ(collisions->count(), 2);
    REQUIRE_EQ(collisions->itemText(0), QStringLiteral("Report it"));
    REQUIRE_EQ(collisions->itemText(1),
               QStringLiteral("Report it and garble the bytes"));
    REQUIRE_EQ(gating->text(), QStringLiteral("Send only while RTS is high"));
    for (QWidget* widget :
         {static_cast<QWidget*>(enable), static_cast<QWidget*>(rate),
          static_cast<QWidget*>(turnaround), static_cast<QWidget*>(collisions),
          static_cast<QWidget*>(gating)})
    {
        REQUIRE(!widget->accessibleName().isEmpty());
        REQUIRE(widget->focusPolicy() != Qt::NoFocus);
    }

    // The rows are disabled, not hidden, until the choice is on.
    REQUIRE(enable->isEnabled());
    REQUIRE(!enable->isChecked());
    for (QWidget* widget :
         {static_cast<QWidget*>(rate), static_cast<QWidget*>(turnaround),
          static_cast<QWidget*>(collisions), static_cast<QWidget*>(gating)})
    {
        REQUIRE(widget->isVisibleTo(&dialog));
        REQUIRE(!widget->isEnabled());
    }
    enable->setChecked(true);
    for (QWidget* widget :
         {static_cast<QWidget*>(rate), static_cast<QWidget*>(turnaround),
          static_cast<QWidget*>(collisions), static_cast<QWidget*>(gating)})
        REQUIRE(widget->isEnabled());
    enable->setChecked(false);
    REQUIRE(!rate->isEnabled());
}

TEST_CASE("connect: the half-duplex controls are an Advanced choice")
{
    vss::gui::FakeStudioBackend backend;
    allow_half_duplex(backend, true);
    make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend);
    show(dialog);
    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::bus));
    REQUIRE(dialog.half_duplex_fields() != nullptr);
    REQUIRE(!dialog.half_duplex_fields()->isVisibleTo(&dialog));
    dialog.set_advanced(true);
    REQUIRE(dialog.half_duplex_fields()->isVisibleTo(&dialog));
}

TEST_CASE("connect: the half-duplex choice is off where the build has none")
{
    vss::gui::FakeStudioBackend backend;
    allow_half_duplex(backend, false);
    make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend, 0, true);
    show(dialog);
    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::bus));

    auto* enable =
        dialog.findChild<QCheckBox*>(QStringLiteral("busHalfDuplex"));
    REQUIRE(enable != nullptr);
    REQUIRE(!enable->isEnabled());
    REQUIRE(!enable->isChecked());
    REQUIRE_EQ(enable->toolTip(),
               QStringLiteral("Not available in this build"));
    REQUIRE(!dialog.half_duplex_fields()->rows_enabled());
    REQUIRE(!dialog.findChild<QSpinBox*>(QStringLiteral("busBitRate"))
                 ->isEnabled());
}

TEST_CASE("connect: a half-duplex bus is made with the chosen line")
{
    vss::gui::FakeStudioBackend backend;
    allow_half_duplex(backend, true);
    const fixture_ports ports = make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend, 0, true);
    show(dialog);
    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::bus));
    dialog.set_member_ids({ports.alpha, ports.beta, ports.delta});

    dialog.findChild<QCheckBox*>(QStringLiteral("busHalfDuplex"))
        ->setChecked(true);
    dialog.findChild<QSpinBox*>(QStringLiteral("busBitRate"))->setValue(19200);
    dialog.findChild<QSpinBox*>(QStringLiteral("busTurnaround"))->setValue(4);
    dialog.findChild<QComboBox*>(QStringLiteral("busCollisions"))
        ->setCurrentIndex(1);
    dialog.findChild<QCheckBox*>(QStringLiteral("busRtsGating"))
        ->setChecked(true);

    REQUIRE(dialog.submit());
    const vss::gui::link_view link = only_link(backend);
    REQUIRE_EQ(link.shape, vss::gui::link_shape::bus);
    REQUIRE(link.options.half_duplex.enabled);
    REQUIRE_EQ(link.options.half_duplex.bit_rate, 19200u);
    REQUIRE_EQ(link.options.half_duplex.turnaround_ms, 4u);
    REQUIRE(link.options.half_duplex.on_collision ==
            vss::collision_action::garble);
    REQUIRE(link.options.half_duplex.rts_gating);
}

TEST_CASE("connect: a bus without the half-duplex choice has default options")
{
    vss::gui::FakeStudioBackend backend;
    allow_half_duplex(backend, true);
    const fixture_ports ports = make_ports(backend);
    vss::gui::ConnectPortsDialog dialog(backend, 0, true);
    show(dialog);
    REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::bus));
    dialog.set_member_ids({ports.alpha, ports.beta});

    // Values typed into the rows count for nothing while the choice is off.
    dialog.findChild<QSpinBox*>(QStringLiteral("busBitRate"))->setValue(19200);
    REQUIRE(dialog.submit());
    const vss::gui::link_view link = only_link(backend);
    REQUIRE_EQ(link.shape, vss::gui::link_shape::bus);
    REQUIRE(link.options.half_duplex == vss::half_duplex_options{});
}
