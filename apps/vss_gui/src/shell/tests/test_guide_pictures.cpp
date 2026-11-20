//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pictures of the user guide (docs/user-guide/). Set VSS_GUIDE_PICTURES to
// a folder outside the repository and run this test, and it draws each window
// and dialog the guide shows, in the light and the dark theme, to PNG files
// named `<picture>-light.png` and `<picture>-dark.png`. The ports are samples
// (COM3, COM7, /dev/pts/1), so no name of this machine appears. Without the
// variable the test does nothing.
//
// The pictures a guide page uses are copied from that folder to docs/images/
// after a person has looked at each one.
//

#include "test_framework.hpp"

#include "advanced/add_fixture_dialog.hpp"
#include "advanced/export_command_dialog.hpp"
#include "advanced/line_matrix_panel.hpp"
#include "advanced/wiring_dialog.hpp"
#include "backend/fake_studio_backend.hpp"
#include "dialogs/connect_ports.hpp"
#include "dialogs/network_endpoint.hpp"
#include "dialogs/share_wizard.hpp"
#include "dialogs/templates_dialog.hpp"
#include "flows/add_port_dialog.hpp"
#include "flows/attach_dialog.hpp"
#include "flows/pair_dialog.hpp"
#include "flows/port_ready_dialog.hpp"
#include "flows/watch_dialog.hpp"
#include "shell/shell_window.hpp"
#include "support/settings.hpp"
#include "support/settings_dialog.hpp"
#include "support/setup_check_dialog.hpp"
#include "support/troubleshoot_dialog.hpp"
#include "theme/theme.hpp"
#include "updates/tests/fixture.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QTableWidget>
#include <QThread>

#include <functional>
#include <utility>
#include <vector>

namespace
{

using vss::gui::FakeStudioBackend;
using vss::gui::platform_kind;
using vss::gui::theme_mode;

void settle()
{
    for (int pass = 0; pass < 6; ++pass)
    {
        QApplication::processEvents();
        QThread::msleep(30);
    }
    QApplication::processEvents();
}

/// Runs `body` once in each theme, with the name part of the file.
void in_each_theme(const std::function<void(const QString&)>& body)
{
    for (const theme_mode mode : {theme_mode::light, theme_mode::dark})
    {
        vss::gui::Theme::instance().set_mode(mode);
        settle();
        body(mode == theme_mode::light ? QStringLiteral("light")
                                       : QStringLiteral("dark"));
    }
    vss::gui::Theme::instance().set_mode(theme_mode::light);
    settle();
}

/// Draws `widget` at `size` (its own size when `size` is empty) to
/// `<folder>/<name>-<theme>.png`.
void shoot(const QString& folder, QWidget& widget, const QSize& size,
           const QString& name, const QString& theme)
{
    if (size.isValid())
        widget.resize(size);
    widget.show();
    settle();
    // No field shows a caret, a selection or a focus ring: a picture shows
    // the window as it is, not where the cursor happens to be.
    if (QWidget* focused = widget.focusWidget())
        focused->clearFocus();
    for (QLineEdit* edit : widget.findChildren<QLineEdit*>())
        edit->deselect();
    settle();
    const QString path = QDir(folder).filePath(name + QLatin1Char('-') + theme +
                                               QStringLiteral(".png"));
    REQUIRE(widget.grab().save(path));
    widget.hide();
}

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

/// A Windows PC with a real device on COM3 and `pairs` free com0com pairs.
/// The ports for programs are COM7, COM8 and so on, and the ends that Studio
/// holds are COM17, COM18 and so on.
void windows_pc(FakeStudioBackend& backend, int pairs)
{
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.com0com_found = true;
    facts.com0com_pairs = pairs;
    facts.serial_devices = 1;
    facts.elevation_known = true;
    facts.elevated = false;
    backend.set_facts(facts);
    std::vector<vss::gui::device_view> devices = {
        serial_device(QStringLiteral("COM3"))};
    for (int pair = 0; pair < pairs; ++pair)
    {
        const unsigned first = 7 + static_cast<unsigned>(pair);
        devices.push_back(pair_end(QStringLiteral("COM%1").arg(first), pair,
                                   QStringLiteral("A")));
        devices.push_back(pair_end(QStringLiteral("COM%1").arg(first + 10),
                                   pair, QStringLiteral("B")));
    }
    backend.set_devices(std::move(devices));
    backend.set_free_pairs(pairs);
}

/// A PC with no driver and no com0com: the launcher says setup needs
/// attention.
void windows_pc_without_driver(FakeStudioBackend& backend)
{
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.serial_devices = 1;
    facts.elevation_known = true;
    facts.elevated = false;
    backend.set_facts(facts);
    backend.set_devices({serial_device(QStringLiteral("COM3"))});
}

/// The Studio driver is installed, so the setup check is ready.
void windows_pc_with_driver(FakeStudioBackend& backend)
{
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.driver_found = true;
    facts.driver_version = QStringLiteral("2");
    facts.serial_devices = 1;
    facts.elevation_known = true;
    facts.elevated = false;
    backend.set_facts(facts);
    backend.set_devices({serial_device(QStringLiteral("COM3"))});
}

/// Names the programs of the wizard's second page.
void name_programs(vss::gui::ShareWizard& wizard, const QStringList& names)
{
    auto tables = wizard.findChildren<QTableWidget*>();
    for (QTableWidget* table : tables)
    {
        for (int row = 0; row < table->rowCount() && row < names.size(); ++row)
        {
            if (auto* edit =
                    qobject_cast<QLineEdit*>(table->cellWidget(row, 0)))
                edit->setText(names.at(row));
        }
    }
}

/// The Advanced window with the launcher showing, as View, Show launcher
/// leaves it.
void launcher_pictures(const QString& folder, const QString& name,
                       const QSize& size, vss::gui::density_mode density)
{
    in_each_theme(
        [&](const QString& theme)
        {
            QSettings().clear();
            vss::gui::Theme::instance().set_density(density);
            FakeStudioBackend backend;
            windows_pc_without_driver(backend);
            {
                vss::gui::ShellWindow window(backend);
                window.resize(size);
                window.show();
                settle();
                auto* advanced =
                    window.findChild<QAction*>(QStringLiteral("view.advanced"));
                REQUIRE(advanced != nullptr);
                advanced->trigger();
                settle();
                auto* launcher = window.findChild<QAction*>(
                    QStringLiteral("view.showLauncher"));
                REQUIRE(launcher != nullptr);
                launcher->trigger();
                settle();
                shoot(folder, window, size, name, theme);
            }
            vss::gui::Theme::instance().set_density(
                vss::gui::density_mode::comfortable);
            QSettings().clear();
        });
}

} // namespace

TEST_CASE("guide pictures: the windows and dialogs of the user guide")
{
    const QString folder =
        QString::fromLocal8Bit(qgetenv("VSS_GUIDE_PICTURES"));
    if (folder.isEmpty())
        return;
    REQUIRE(QDir().mkpath(folder));
    QSettings().clear();

    // Getting started: the window with the launcher, at the sizes of the
    // guide.
    launcher_pictures(folder, QStringLiteral("launcher"), QSize(1280, 800),
                      vss::gui::density_mode::comfortable);
    launcher_pictures(folder, QStringLiteral("launcher-compact"),
                      QSize(960, 640), vss::gui::density_mode::compact);

    // Getting started: the dialog that opens when a port is made, on Linux.
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            const auto port = backend.create_virtual_port(
                QStringLiteral("port-1"), vss::virtual_port_kind::pty, true);
            REQUIRE(port.has_value());
            vss::gui::PortReadyDialog dialog(backend, *port,
                                             QStringLiteral("auto"), false,
                                             platform_kind::linux);
            shoot(folder, dialog, QSize(760, 720),
                  QStringLiteral("port-ready-linux"), theme);
        });

    // Adding ports.
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            windows_pc(backend, 2);
            vss::gui::AddPortDialog dialog(
                backend, true, vss::gui::AddPortDialog::mode::virtual_port,
                platform_kind::windows);
            shoot(folder, dialog, QSize(760, 840), QStringLiteral("add-port"),
                  theme);
        });
    in_each_theme(
        [&](const QString& theme)
        {
            // No free pair: Create pair is off and the warning says why.
            FakeStudioBackend backend;
            windows_pc(backend, 0);
            vss::gui::PairDialog dialog(backend, false, platform_kind::windows);
            shoot(folder, dialog, QSize(760, 700),
                  QStringLiteral("linked-pair"), theme);
        });
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            windows_pc(backend, 2);
            vss::gui::AttachDialog dialog(backend, false,
                                          platform_kind::windows);
            shoot(folder, dialog, QSize(760, 960),
                  QStringLiteral("attach-device"), theme);
        });
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            windows_pc(backend, 2);
            vss::gui::capabilities caps;
            caps.network_server = true;
            backend.set_capabilities(caps);
            vss::gui::NetworkEndpointDialog dialog(backend);
            shoot(folder, dialog, QSize(760, 900),
                  QStringLiteral("network-endpoint"), theme);
        });
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            windows_pc(backend, 2);
            const auto terminal = backend.create_virtual_port(
                QStringLiteral("terminal"), vss::virtual_port_kind::pty, true);
            REQUIRE(terminal.has_value());
            vss::gui::AddFixtureDialog dialog(backend, *terminal);
            shoot(folder, dialog, QSize(760, 620),
                  QStringLiteral("mock-fixture"), theme);
        });
    in_each_theme(
        [&](const QString& theme)
        {
            // The line matrix and the six Inject buttons, which need a build
            // that can inject faults.
            FakeStudioBackend backend;
            vss::gui::capabilities caps;
            caps.faults = true;
            backend.set_capabilities(caps);
            const auto modem = backend.create_mock_port(
                QStringLiteral("modem"), {}, QStringLiteral("scripted"));
            const auto terminal = backend.create_virtual_port(
                QStringLiteral("terminal"), vss::virtual_port_kind::pty, true);
            REQUIRE(modem.has_value());
            REQUIRE(terminal.has_value());
            REQUIRE(backend.connect_ports(*modem, *terminal, {}).has_value());
            vss::gui::WiringDialog dialog(backend, *modem);
            REQUIRE(dialog.panel() != nullptr);
            shoot(folder, dialog, QSize(1100, 760),
                  QStringLiteral("fixture-faults"), theme);
        });

    // Links between ports: the dialog with all six ways to connect.
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            vss::gui::capabilities caps = backend.caps();
            caps.shared_bus = true;
            backend.set_capabilities(caps);
            for (const char* name : {"alpha", "beta", "gamma"})
            {
                static_cast<void>(backend.create_virtual_port(
                    QString::fromLatin1(name), vss::virtual_port_kind::pty,
                    true));
            }
            vss::gui::ConnectPortsDialog dialog(backend);
            shoot(folder, dialog, QSize(920, 700),
                  QStringLiteral("connect-ports"), theme);
        });

    // Sharing a device, on Windows. The first two steps are for a PC with no
    // free driver pair, so every program is Studio only and the warning
    // offers Create another pair. The last step is for a PC with two free
    // pairs, so two programs can open their ports.
    for (const int pairs : {0, 2})
    {
        in_each_theme(
            [&](const QString& theme)
            {
                FakeStudioBackend backend;
                windows_pc(backend, pairs);
                vss::gui::capabilities caps;
                caps.network_server = true;
                backend.set_capabilities(caps);
                vss::gui::ShareWizard wizard(backend);
                wizard.resize(760, 700);
                wizard.show();
                settle();
                if (pairs == 0)
                {
                    shoot(folder, wizard, QSize(760, 700),
                          QStringLiteral("share-device"), theme);
                }
                REQUIRE(wizard.next());
                name_programs(wizard, {QStringLiteral("terminal"),
                                       QStringLiteral("test-tool"),
                                       QStringLiteral("logger")});
                settle();
                if (pairs == 0)
                {
                    shoot(folder, wizard, QSize(760, 760),
                          QStringLiteral("share-programs"), theme);
                    return;
                }
                REQUIRE(wizard.share());
                settle();
                shoot(folder, wizard, QSize(760, 640),
                      QStringLiteral("share-ready"), theme);
            });
    }

    // Monitor and capture.
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            windows_pc(backend, 2);
            const auto port = backend.create_driver_port(
                QStringLiteral("terminal"), false, 7);
            REQUIRE(port.has_value());
            vss::gui::WatchDialog dialog(backend, *port,
                                         platform_kind::windows);
            shoot(folder, dialog, QSize(760, 600),
                  QStringLiteral("watch-traffic"), theme);
        });

    // Setups.
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            vss::gui::TemplatesDialog dialog(backend);
            shoot(folder, dialog, QSize(920, 700), QStringLiteral("templates"),
                  theme);
        });
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            vss::gui::ExportCommandDialog dialog(backend,
                                                 QStringLiteral("setup.json"));
            shoot(folder, dialog, QSize(640, 320),
                  QStringLiteral("export-command"), theme);
        });

    // Settings and help.
    in_each_theme(
        [&](const QString& theme)
        {
            QSettings().clear();
            vss::gui::updates::testing::harness updates;
            updates.create();
            FakeStudioBackend backend;
            vss::gui::support::SettingsDialog dialog(
                backend, vss::gui::support::load_ui_settings(),
                updates.service.get());
            shoot(folder, dialog, QSize(860, 760), QStringLiteral("settings"),
                  theme);
        });
    in_each_theme(
        [&](const QString& theme)
        {
            FakeStudioBackend backend;
            windows_pc_with_driver(backend);
            vss::gui::support::SetupCheckDialog dialog(backend);
            shoot(folder, dialog, QSize(760, 720),
                  QStringLiteral("setup-check"), theme);
        });
    in_each_theme(
        [&](const QString& theme)
        {
            // The port is open and has no link, so the later checks wait.
            FakeStudioBackend backend;
            windows_pc_with_driver(backend);
            const auto port = backend.create_driver_port(
                QStringLiteral("terminal"), false, 7);
            REQUIRE(port.has_value());
            vss::gui::support::TroubleshootDialog dialog(backend, *port);
            shoot(folder, dialog, QSize(640, 760),
                  QStringLiteral("troubleshoot"), theme);
        });
    QSettings().clear();
}
