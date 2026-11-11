//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "dialogs/connect_ports.hpp"
#include "dialogs/network_endpoint.hpp"
#include "dialogs/share_wizard.hpp"
#include "dialogs/templates_dialog.hpp"
#include "theme/theme.hpp"
#include "widgets/notes.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QToolButton>

namespace
{

vss::gui::device_view serial_device(const QString& path)
{
    vss::gui::device_view device;
    device.path = path;
    device.description = QStringLiteral("Serial device");
    return device;
}

vss::gui::device_view pair_end(const QString& path, int pair,
                               const QString& side)
{
    vss::gui::device_view device;
    device.path = path;
    device.com0com = true;
    device.virtual_device = true;
    device.com0com_pair = pair;
    device.com0com_side = side;
    return device;
}

void configure(vss::gui::FakeStudioBackend& backend)
{
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.com0com_found = true;
    facts.com0com_pairs = 2;
    backend.set_facts(facts);
    backend.set_devices(
        {serial_device(QStringLiteral("COM3")),
         pair_end(QStringLiteral("COM7"), 0, QStringLiteral("A")),
         pair_end(QStringLiteral("COM8"), 0, QStringLiteral("B")),
         pair_end(QStringLiteral("COM9"), 1, QStringLiteral("A")),
         pair_end(QStringLiteral("COM10"), 1, QStringLiteral("B"))});
    backend.set_free_pairs(2);
}

QString button_name(QWidget* widget)
{
    if (!widget->accessibleName().isEmpty())
        return widget->accessibleName();
    if (auto* button = qobject_cast<QAbstractButton*>(widget))
        return button->text();
    if (auto* label = qobject_cast<QLabel*>(widget))
        return label->text();
    return {};
}

bool is_focusable(QWidget* widget)
{
    return widget->focusPolicy() != Qt::NoFocus &&
           (widget->focusPolicy() &
            (Qt::TabFocus | Qt::StrongFocus | Qt::WheelFocus));
}

void check_accessible(QDialog& dialog)
{
    int checked = 0;
    for (QWidget* widget : dialog.findChildren<QWidget*>())
    {
        if (!widget->isVisibleTo(&dialog) || !widget->isEnabled())
            continue;
        if (!is_focusable(widget))
            continue;
        if (qobject_cast<QScrollBar*>(widget) != nullptr)
            continue;
        if (qobject_cast<QComboBox*>(widget->parentWidget()) != nullptr)
            continue; // QComboBox's internal line edit takes the combo's name.
        REQUIRE(!button_name(widget).isEmpty());
        ++checked;
    }
    REQUIRE(checked > 0);
}

void check_button_targets(QDialog& dialog)
{
    for (QAbstractButton* button : dialog.findChildren<QAbstractButton*>())
    {
        if (!button->isVisibleTo(&dialog) || button->inherits("QRadioButton") ||
            button->inherits("QCheckBox"))
        {
            continue;
        }
        const QSize hint = button->sizeHint();
        const QSize minimum = button->minimumSizeHint();
        REQUIRE(hint.width() >= 24 || minimum.width() >= 24 ||
                button->minimumWidth() >= 24);
        REQUIRE(hint.height() >= 24 || minimum.height() >= 24 ||
                button->minimumHeight() >= 24);
    }
}

QString shots_directory()
{
    const QString directory = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(QStringLiteral("../gui-shots"));
    REQUIRE(QDir().mkpath(directory));
    return directory;
}

void grab_both_themes(QDialog& dialog, const QString& name)
{
    QApplication::processEvents();
    REQUIRE(dialog.grab().save(
        QDir(shots_directory()).filePath(name + QStringLiteral("-light.png"))));
    vss::gui::Theme::instance().set_mode(vss::gui::theme_mode::dark);
    QApplication::processEvents();
    REQUIRE(dialog.grab().save(
        QDir(shots_directory()).filePath(name + QStringLiteral("-dark.png"))));
    vss::gui::Theme::instance().set_mode(vss::gui::theme_mode::light);
    QApplication::processEvents();
}

} // namespace

TEST_CASE("dialogs: every focusable control has a name and buttons are 24px")
{
    {
        vss::gui::FakeStudioBackend backend;
        for (const char* name : {"alpha", "beta", "gamma"})
        {
            static_cast<void>(backend.create_virtual_port(
                QString::fromLatin1(name), vss::virtual_port_kind::pty, true));
        }
        vss::gui::ConnectPortsDialog dialog(backend);
        dialog.show();
        QApplication::processEvents();
        check_accessible(dialog);
        check_button_targets(dialog);
        dialog.close();
    }
    {
        // The bus page with the half-duplex line on, so its rows are enabled.
        vss::gui::FakeStudioBackend backend;
        vss::gui::capabilities caps = backend.caps();
        caps.shared_bus = true;
        caps.half_duplex_bus = true;
        backend.set_capabilities(caps);
        for (const char* name : {"alpha", "beta", "gamma"})
        {
            static_cast<void>(backend.create_virtual_port(
                QString::fromLatin1(name), vss::virtual_port_kind::pty, true));
        }
        vss::gui::ConnectPortsDialog dialog(backend, 0, true);
        dialog.show();
        REQUIRE(dialog.set_mode(vss::gui::ConnectPortsDialog::mode::bus));
        dialog.findChild<QCheckBox*>(QStringLiteral("busHalfDuplex"))
            ->setChecked(true);
        QApplication::processEvents();
        check_accessible(dialog);
        check_button_targets(dialog);
        for (const char* name : {"busHalfDuplex", "busBitRate", "busTurnaround",
                                 "busCollisions", "busRtsGating"})
        {
            const auto* widget =
                dialog.findChild<QWidget*>(QString::fromLatin1(name));
            REQUIRE(widget != nullptr);
            REQUIRE(widget->isEnabled());
            REQUIRE(widget->sizeHint().height() >= 24);
        }
        dialog.close();
    }
    {
        vss::gui::FakeStudioBackend backend;
        configure(backend);
        vss::gui::ShareWizard wizard(backend);
        wizard.show();
        QApplication::processEvents();
        check_accessible(wizard);
        check_button_targets(wizard);
        REQUIRE(wizard.next());
        QApplication::processEvents();
        check_accessible(wizard);
        check_button_targets(wizard);
        REQUIRE(wizard.share());
        QApplication::processEvents();
        check_accessible(wizard);
        check_button_targets(wizard);
        wizard.close();
    }
    {
        vss::gui::FakeStudioBackend backend;
        configure(backend);
        vss::gui::NetworkEndpointDialog dialog(backend);
        dialog.show();
        QApplication::processEvents();
        check_accessible(dialog);
        check_button_targets(dialog);
        dialog.close();
    }
    {
        vss::gui::FakeStudioBackend backend;
        vss::gui::TemplatesDialog dialog(backend);
        dialog.show();
        QApplication::processEvents();
        check_accessible(dialog);
        check_button_targets(dialog);
        dialog.close();
    }
}

TEST_CASE("dialogs: Escape cancels and Enter runs the primary action")
{
    {
        vss::gui::FakeStudioBackend backend;
        static_cast<void>(backend.create_virtual_port(
            QStringLiteral("alpha"), vss::virtual_port_kind::pty, true));
        static_cast<void>(backend.create_virtual_port(
            QStringLiteral("beta"), vss::virtual_port_kind::pty, true));
        vss::gui::ConnectPortsDialog dialog(backend);
        dialog.show();
        QApplication::processEvents();
        // Focus moves once the window system activates the window, which
        // happens after show() returns and can be slow on a busy machine.
        REQUIRE(vss_test::wait_until(
            [] { return QApplication::focusWidget() != nullptr; }));
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&dialog, &escape);
        REQUIRE_EQ(static_cast<QDialog&>(dialog).result(), QDialog::Rejected);
    }
    {
        vss::gui::FakeStudioBackend backend;
        for (const char* name : {"alpha", "beta", "gamma"})
        {
            static_cast<void>(backend.create_virtual_port(
                QString::fromLatin1(name), vss::virtual_port_kind::pty, true));
        }
        vss::gui::ConnectPortsDialog dialog(backend);
        dialog.show();
        QApplication::processEvents();
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&dialog, &enter);
        QApplication::processEvents();
        REQUIRE_EQ(static_cast<QDialog&>(dialog).result(), QDialog::Accepted);
        REQUIRE_EQ(backend.links().size(), std::size_t{1});
    }
    {
        vss::gui::FakeStudioBackend backend;
        configure(backend);
        vss::gui::ShareWizard wizard(backend);
        wizard.show();
        QApplication::processEvents();
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&wizard, &enter);
        QApplication::processEvents();
        REQUIRE_EQ(wizard.current_step(), 1);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&wizard, &escape);
        REQUIRE_EQ(static_cast<QDialog&>(wizard).result(), QDialog::Rejected);
    }
}

TEST_CASE("dialogs: review screenshots in both themes")
{
    {
        vss::gui::FakeStudioBackend backend;
        for (const char* name : {"alpha", "beta", "gamma"})
        {
            static_cast<void>(backend.create_virtual_port(
                QString::fromLatin1(name), vss::virtual_port_kind::pty, true));
        }
        vss::gui::ConnectPortsDialog dialog(backend);
        dialog.resize(920, 700);
        dialog.show();
        dialog.set_mode(vss::gui::ConnectPortsDialog::mode::split);
        grab_both_themes(dialog, QStringLiteral("connect"));
        dialog.close();
    }
    {
        vss::gui::FakeStudioBackend backend;
        configure(backend);
        vss::gui::capabilities caps;
        caps.network_server = true;
        backend.set_capabilities(caps);
        vss::gui::ShareWizard wizard(backend);
        wizard.resize(760, 700);
        wizard.show();
        QApplication::processEvents();
        grab_both_themes(wizard, QStringLiteral("share-step1"));
        static_cast<void>(wizard.next());
        QApplication::processEvents();
        grab_both_themes(wizard, QStringLiteral("share-step2"));
        static_cast<void>(wizard.share());
        QApplication::processEvents();
        grab_both_themes(wizard, QStringLiteral("share-step3"));
        wizard.close();
    }
    {
        vss::gui::FakeStudioBackend backend;
        configure(backend);
        vss::gui::capabilities caps;
        caps.network_server = true;
        backend.set_capabilities(caps);
        vss::gui::NetworkEndpointDialog dialog(backend);
        dialog.resize(760, 760);
        dialog.show();
        grab_both_themes(dialog, QStringLiteral("network"));
        dialog.close();
    }
    {
        vss::gui::FakeStudioBackend backend;
        vss::gui::TemplatesDialog dialog(backend);
        dialog.resize(920, 700);
        dialog.show();
        grab_both_themes(dialog, QStringLiteral("templates"));
        dialog.close();
    }
}
