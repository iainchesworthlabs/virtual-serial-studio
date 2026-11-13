//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/accessibility.hpp"

#include "advanced/add_fixture_dialog.hpp"
#include "advanced/benchmark_dialog.hpp"
#include "advanced/export_command_dialog.hpp"
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
#include "flows/rename_dialog.hpp"
#include "flows/watch_dialog.hpp"
#include "shell/action_registry.hpp"
#include "support/about_dialog.hpp"
#include "support/event_log.hpp"
#include "support/help_contents_dialog.hpp"
#include "support/licence_dialog.hpp"
#include "support/settings.hpp"
#include "support/settings_dialog.hpp"
#include "support/setup_check_dialog.hpp"
#include "support/setup_facts.hpp"
#include "support/shortcuts_dialog.hpp"
#include "support/support_bundle_dialog.hpp"
#include "support/support_dialog.hpp"
#include "support/troubleshoot_dialog.hpp"
#include "theme/theme.hpp"
#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>
#include <vss/license/license.hpp>

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QAccessibleInterface>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QWidget>

#include <cstdio>
#include <memory>

namespace vss::gui
{
namespace
{

/// A tool button that shows only its icon does not draw its text, so the text
/// cannot be clipped.
bool shows_icon_only(const QAbstractButton* button)
{
    const auto* tool = qobject_cast<const QToolButton*>(button);
    return tool != nullptr && tool->toolButtonStyle() == Qt::ToolButtonIconOnly;
}

QString widget_name(QWidget* widget)
{
    if (!widget->accessibleName().isEmpty())
        return widget->accessibleName();
    if (QAccessibleInterface* interface =
            QAccessible::queryAccessibleInterface(widget))
    {
        const QString name = interface->text(QAccessible::Name);
        if (!name.isEmpty())
            return name;
    }
    if (auto* button = qobject_cast<QAbstractButton*>(widget))
        return button->text();
    // A buddy label is the explicit name for a field.
    for (QLabel* label : widget->findChildren<QLabel*>())
    {
        if (label->buddy() == widget && !label->text().isEmpty())
            return label->text();
    }
    return {};
}

bool focusable(QWidget* widget)
{
    if (!widget->isEnabled() || widget->focusPolicy() == Qt::NoFocus)
        return false;
    return (widget->focusPolicy() &
            (Qt::TabFocus | Qt::StrongFocus | Qt::WheelFocus)) != 0;
}

bool composite_container(QWidget* widget)
{
    return qobject_cast<QAbstractScrollArea*>(widget) != nullptr ||
           qobject_cast<QTabWidget*>(widget) != nullptr ||
           qobject_cast<QTabBar*>(widget) != nullptr;
}

bool inside_scroll_area(QWidget* widget)
{
    for (QWidget* parent = widget->parentWidget(); parent != nullptr;
         parent = parent->parentWidget())
    {
        if (qobject_cast<QScrollArea*>(parent) != nullptr)
            return true;
    }
    return false;
}

QString screenshot_directory()
{
    QString configured = qEnvironmentVariable("VSS_QA_SHOTS");
    if (!configured.isEmpty() && configured != QLatin1String("1"))
        return configured;
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("../gui-shots"));
}

class Checker
{
public:
    explicit Checker(QStringList& failures) : failures_(failures) {}

    void run(QWidget& root, const QString& label)
    {
        root.show();
        // A dialog shown and closed before the check can leave no active
        // window, and then nothing can take focus.
        root.activateWindow();
        QApplication::processEvents();
        Theme::instance().set_mode(theme_mode::light);
        QApplication::processEvents();
        check_tree(root, label);
        const bool capture = !qEnvironmentVariable("VSS_QA_SHOTS").isEmpty();
        if (capture)
            grab(root, label + QStringLiteral("-light"));
        Theme::instance().set_mode(theme_mode::dark);
        QApplication::processEvents();
        check_tree(root, label + QStringLiteral(" (dark)"));
        if (capture)
            grab(root, label + QStringLiteral("-dark"));
        Theme::instance().set_mode(theme_mode::light);
        QApplication::processEvents();
    }

    void compact(QWidget& root, const QString& label)
    {
        if (qEnvironmentVariable("VSS_QA_SHOTS").isEmpty())
            return;
        const QSize original = root.size();
        root.resize(960, 640);
        QApplication::processEvents();
        Theme::instance().set_mode(theme_mode::light);
        QApplication::processEvents();
        grab(root, label + QStringLiteral("-compact-light"));
        Theme::instance().set_mode(theme_mode::dark);
        QApplication::processEvents();
        grab(root, label + QStringLiteral("-compact-dark"));
        Theme::instance().set_mode(theme_mode::light);
        root.resize(original);
        QApplication::processEvents();
    }

private:
    void fail(const QString& text)
    {
        failures_ << text;
        std::fprintf(stderr, "selftest: FAIL: %s\n", text.toUtf8().constData());
    }

    void grab(QWidget& widget, const QString& name)
    {
        const QString directory = screenshot_directory();
        if (!QDir().mkpath(directory))
        {
            fail(QStringLiteral("cannot create the screenshot directory %1")
                     .arg(directory));
            return;
        }
        const QPixmap image = widget.grab();
        if (!image.save(
                QDir(directory).filePath(name + QStringLiteral(".png"))))
        {
            fail(QStringLiteral("cannot write the screenshot %1").arg(name));
        }
    }

    void check_tree(QWidget& root, const QString& label)
    {
        QSet<QWidget*> seen;
        const bool can_measure_fonts = !QFontDatabase::families().isEmpty();
        int checked = 0;
        for (QWidget* widget : root.findChildren<QWidget*>())
        {
            if (!widget->isVisible() || !widget->isEnabled())
                continue;
            if (qobject_cast<QScrollBar*>(widget) != nullptr)
                continue;
            if (qobject_cast<QComboBox*>(widget->parentWidget()) != nullptr ||
                qobject_cast<QAbstractSpinBox*>(widget->parentWidget()) !=
                    nullptr)
                continue;
            if (composite_container(widget))
                continue;
            if (!focusable(widget))
                continue;

            const QString name = widget_name(widget);
            if (name.isEmpty())
            {
                fail(QStringLiteral("%1: focusable %2 has no accessible name")
                         .arg(label, widget->metaObject()->className()));
            }

            if (auto* button = qobject_cast<QAbstractButton*>(widget))
            {
                if (button->width() < 24 || button->height() < 24)
                {
                    fail(QStringLiteral(
                             "%1: button \"%2\" is %3x%4, below 24x24")
                             .arg(label, name)
                             .arg(button->width())
                             .arg(button->height()));
                }
            }

            if (inside_scroll_area(widget))
            {
                ++checked;
                continue;
            }

            if (auto* label_widget = qobject_cast<QLabel*>(widget);
                can_measure_fonts && label_widget != nullptr &&
                !label_widget->wordWrap() &&
                label_widget->textFormat() == Qt::PlainText &&
                !label_widget->text().isEmpty() &&
                label_widget->sizeHint().width() >
                    label_widget->contentsRect().width() + 2)
            {
                fail(QStringLiteral("%1: label \"%2\" is clipped (%3 > %4)")
                         .arg(label, label_widget->text())
                         .arg(label_widget->sizeHint().width())
                         .arg(label_widget->contentsRect().width()));
            }
            if (auto* button = qobject_cast<QAbstractButton*>(widget);
                can_measure_fonts && button != nullptr &&
                !button->text().isEmpty() &&
                !shows_icon_only(button) &&
                button->fontMetrics().horizontalAdvance(button->text()) >
                    button->contentsRect().width() - 4)
            {
                fail(QStringLiteral("%1: button text \"%2\" is clipped")
                         .arg(label, button->text()));
            }

            widget->setFocus(Qt::TabFocusReason);
            QApplication::processEvents();
            if (QApplication::focusWidget() != widget)
            {
                fail(QStringLiteral("%1: %2 cannot take keyboard focus")
                         .arg(label, widget->metaObject()->className()));
            }
            seen.insert(widget);
            ++checked;
        }
        if (checked == 0)
            fail(QStringLiteral("%1: no focusable controls were found")
                     .arg(label));
    }

    QStringList& failures_;
};

void configure_fake(FakeStudioBackend& backend)
{
    setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.com0com_found = true;
    facts.com0com_pairs = 2;
    backend.set_facts(facts);
    backend.set_free_pairs(2);

    device_view device;
    device.path = QStringLiteral("COM3");
    device.description = QStringLiteral("Serial device");
    backend.set_devices({device});
}

/// A licence that ends in ten days, so the Licence page shows its note, its
/// features and the list of other licences.
licence_view sample_licence(const char* now = "2026-11-21T00:00:00Z")
{
    namespace lic = vss::license;
    lic::licence value;
    value.licence_id = "VSS-SAMPLE-000001";
    value.licensee_name = "Example Lab";
    value.starts_at = *lic::parse_time("2026-01-01T00:00:00Z");
    value.issued_at = value.starts_at;
    value.expires_at = *lic::parse_time("2026-12-01T00:00:00Z");
    value.seats = 3;
    value.features = {"plugins.official", "support"};
    value.machines = {"LAB-*"};
    lic::context where;
    where.now = *lic::parse_time(now);
    where.computer_name = "LAB-RIG-07";

    licence_view view;
    view.status = lic::evaluate(value, nullptr, where);
    view.computer_name = QStringLiteral("LAB-RIG-07");
    licence_file_view in_use;
    in_use.status = view.status;
    in_use.file_name = QStringLiteral("VSS-SAMPLE-000001.vsslic");
    in_use.in_use = true;
    view.files.push_back(in_use);
    licence_file_view other = in_use;
    other.file_name = QStringLiteral("VSS-SAMPLE-000002.vsslic");
    other.in_use = false;
    other.machine_wide = true;
    view.files.push_back(other);
    return view;
}

} // namespace

QStringList run_accessibility_checks(ActionRegistry& registry,
                                     QWidget* main_window)
{
    VSS_PROFILE_ZONE("gui.shell.accessibility.check");
    QStringList failures;
    // Every failure from every check is appended to one list for the caller.
    Checker checker(failures);

    // The real main window is checked when one was given; the dialogs below
    // are always built against a fake backend.
    if (main_window != nullptr)
    {
        checker.run(*main_window, QStringLiteral("main-window"));
        checker.compact(*main_window, QStringLiteral("main-window"));
    }

    // No real driver or hub is touched. The fake gets two virtual ports to
    // link and a mock port to act as a fixture.
    FakeStudioBackend backend;
    configure_fake(backend);
    const auto alpha = backend.create_virtual_port(
        QStringLiteral("alpha"), vss::virtual_port_kind::pty, true);
    const auto beta = backend.create_virtual_port(
        QStringLiteral("beta"), vss::virtual_port_kind::pty, true);
    const auto fixture = backend.create_mock_port(QStringLiteral("fixture"), {},
                                                  QStringLiteral("scripted"));
    // Without these ports most dialogs cannot be built, so report it and stop.
    if (!alpha || !beta || !fixture)
    {
        failures << QStringLiteral(
            "the accessibility fixture could not be built");
        return failures;
    }

    {
        // One dialog per block so each is destroyed before the next is built.
        // Each is only shown, never run modally, so this cannot block.
        AddPortDialog dialog(backend, true);
        checker.run(dialog, QStringLiteral("add-port"));
    }
    {
        PortReadyDialog dialog(backend, *alpha, QStringLiteral("auto"), true);
        checker.run(dialog, QStringLiteral("port-ready"));
    }
    {
        PairDialog dialog(backend, true);
        checker.run(dialog, QStringLiteral("pair"));
    }
    {
        AttachDialog dialog(backend, true);
        checker.run(dialog, QStringLiteral("attach"));
    }
    {
        WatchDialog dialog(backend, *alpha);
        checker.run(dialog, QStringLiteral("watch"));
    }
    {
        RenameDialog dialog(backend, *alpha);
        checker.run(dialog, QStringLiteral("rename"));
    }
    {
        ConnectPortsDialog dialog(backend, *alpha, true);
        checker.run(dialog, QStringLiteral("connect-ports"));
    }
    {
        // Walk the wizard page by page, stopping if a step is refused.
        ShareWizard dialog(backend, true);
        checker.run(dialog, QStringLiteral("share-step1"));
        if (dialog.next())
        {
            checker.run(dialog, QStringLiteral("share-step2"));
            if (dialog.share())
                checker.run(dialog, QStringLiteral("share-step3"));
        }
    }
    {
        NetworkEndpointDialog dialog(backend, true, *alpha);
        checker.run(dialog, QStringLiteral("network-endpoint"));
    }
    {
        TemplatesDialog dialog(backend);
        checker.run(dialog, QStringLiteral("templates"));
    }
    {
        AddFixtureDialog dialog(backend, *alpha);
        checker.run(dialog, QStringLiteral("add-fixture"));
    }
    {
        WiringDialog dialog(backend, *fixture);
        checker.run(dialog, QStringLiteral("wiring"));
    }
    {
        BenchmarkDialog dialog(backend);
        checker.run(dialog, QStringLiteral("benchmark"));
    }
    {
        ExportCommandDialog dialog(backend, QStringLiteral("setup.json"));
        checker.run(dialog, QStringLiteral("export-command"));
    }
    {
        // Dialogs from the support area follow: setup, troubleshooting, the
        // event log, settings, about, shortcuts and help.
        support::SetupCheckDialog dialog(backend);
        checker.run(dialog, QStringLiteral("setup-check"));
    }
    {
        support::TroubleshootDialog dialog(backend, *alpha);
        checker.run(dialog, QStringLiteral("troubleshoot"));
    }
    {
        // The support bundle, with no licence and with one that names
        // computers, so that every row of the checklist is walked.
        support::EventLog log;
        support::SupportBundleDialog dialog(backend, log);
        checker.run(dialog, QStringLiteral("support-bundle"));
    }
    {
        FakeStudioBackend licensed;
        configure_fake(licensed);
        licensed.set_licence(sample_licence());
        support::EventLog log;
        support::SupportBundleDialog dialog(licensed, log);
        checker.run(dialog, QStringLiteral("support-bundle-licence"));
    }
    {
        // The Support page in its three variants.
        support::SupportDialog dialog(backend);
        checker.run(dialog, QStringLiteral("support-community"));
    }
    {
        FakeStudioBackend licensed;
        licensed.set_licence(sample_licence());
        support::SupportDialog dialog(licensed);
        checker.run(dialog, QStringLiteral("support-included"));
    }
    {
        FakeStudioBackend ended;
        ended.set_licence(sample_licence("2027-01-05T00:00:00Z"));
        support::SupportDialog dialog(ended);
        checker.run(dialog, QStringLiteral("support-ended"));
    }
    {
        support::SettingsDialog dialog(backend, support::load_ui_settings());
        checker.run(dialog, QStringLiteral("settings"));
    }
    {
        support::AboutDialog dialog(backend);
        checker.run(dialog, QStringLiteral("about"));
    }
    {
        support::ShortcutsDialog dialog(shortcut_entries(registry));
        checker.run(dialog, QStringLiteral("shortcuts"));
    }
    {
        support::HelpContentsDialog dialog;
        checker.run(dialog, QStringLiteral("help-contents"));
    }
    {
        // The Licence page, with a licence so that every part shows.
        FakeStudioBackend licensed;
        capabilities caps;
        caps.licences = true;
        licensed.set_capabilities(caps);
        licensed.set_licence(sample_licence());
        support::LicenceDialog dialog(licensed);
        checker.run(dialog, QStringLiteral("licence"));
    }
    {
        support::RenewDialog dialog(QStringLiteral("VSS-SAMPLE-000001"));
        checker.run(dialog, QStringLiteral("licence-renew"));
    }
    {
        // The generic confirm and error dialogs are checked with sample text.
        ConfirmDialog dialog(
            QStringLiteral("Remove port"), QStringLiteral("Remove alpha?"),
            QStringLiteral("Remove"), QStringLiteral("Cancel"), true);
        checker.run(dialog, QStringLiteral("confirm"));
    }
    {
        ErrorDialog dialog(QStringLiteral("Could not open COM3"),
                           QStringLiteral("The device is in use."),
                           QStringLiteral("access denied"));
        checker.run(dialog, QStringLiteral("error"));
    }

    return failures;
}

} // namespace vss::gui
