//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/permission_dialog.hpp"

#include "support/elevated_step.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace vss::gui::support
{
namespace
{

namespace perm = vss::permissions;

QLabel* paragraph(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::body));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

struct wording
{
    QString title;
    QString subtitle;
    QString body;
    QString command_label;
};

/// The wording for a serial device that the account cannot open.
void describe_serial_access(wording& out, const perm::fact& fact,
                            const QString& subject)
{
    out.title = QCoreApplication::translate("PermissionDialog",
                                            "Serial device access");
    if (fact.why == perm::reason::not_in_group)
    {
        out.subtitle = QCoreApplication::translate(
                           "PermissionDialog",
                           "Your account is not in the group that owns %1.")
                           .arg(subject);
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "On Linux, the group that owns a serial device decides who may "
            "open it. Add your account to that group, then sign out and "
            "back in. Studio never changes a group by itself.");
    }
    else if (fact.why == perm::reason::relogin_needed)
    {
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog",
            "Your account is in the group, but this login "
            "does not know yet.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "Sign out and back in. To use the group in one terminal right "
            "now, run the command below there and start Studio from that "
            "terminal.");
    }
    else if (fact.why == perm::reason::mode_denies)
    {
        out.subtitle =
            QCoreApplication::translate(
                "PermissionDialog",
                "The group that owns %1 may not read and write it.")
                .arg(subject);
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "Look at the permissions of the device with the command below. "
            "Changing them is a system setting, so Studio leaves it to "
            "you; "
            "a udev rule makes a change last across restarts.");
    }
    else
    {
        out.subtitle =
            QCoreApplication::translate(
                "PermissionDialog",
                "The file permissions allow %1, but something outside "
                "them refused it.")
                .arg(subject);
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "Linux security policy (AppArmor, SELinux) or a sandbox "
            "(Flatpak, Snap) can refuse a device that the file permissions "
            "allow. The text below is the fix for the one that applies to "
            "this system.");
    }
    out.command_label =
        QCoreApplication::translate("PermissionDialog", "Command");
}

/// The wording for a sandbox or security policy that confines Studio.
void describe_confinement(wording& out, const perm::fact& fact,
                          const QString& subject)
{
    out.title =
        QCoreApplication::translate("PermissionDialog", "Confinement");
    if (fact.why == perm::reason::flatpak)
    {
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog", "Studio runs inside a Flatpak sandbox.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "A Flatpak sees only the devices and folders it was given. If "
            "a "
            "serial device is missing, give the application access to "
            "devices with the command below.");
    }
    else if (fact.why == perm::reason::snap)
    {
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog", "Studio runs inside a Snap sandbox.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "A Snap reaches a serial device only through an interface that "
            "is connected. The command below lists what is connected; the "
            "serial-port interface is the one a serial device needs.");
    }
    else if (fact.why == perm::reason::apparmor_profile)
    {
        out.subtitle = QCoreApplication::translate(
                           "PermissionDialog",
                           "An AppArmor profile confines Studio (%1).")
                           .arg(subject);
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "The profile may refuse serial devices, pseudo terminals or "
            "Studio's link folder. If a program is refused, add the lines "
            "below to the local override of the profile and reload it. "
            "Studio never edits a profile.");
    }
    else if (fact.why == perm::reason::macos_sandbox)
    {
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog", "Studio runs in the macOS App Sandbox.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "A sandboxed app reaches a serial device and the network only "
            "with entitlements. The lines below are the ones a sandboxed "
            "build of Studio needs in its entitlements file.");
    }
    else
    {
        out.subtitle = QCoreApplication::translate("PermissionDialog",
                                                   "SELinux is enforcing.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "This is normal and nothing is wrong unless a device is "
            "refused. If one is, the commands below find the denial and "
            "build a policy module for it. Read what audit2allow proposes "
            "before you install it.");
    }
    out.command_label =
        QCoreApplication::translate("PermissionDialog", "Commands");
}

/// The wording for a Linux firewall that may stop other computers connecting.
void describe_linux_firewall(wording& out, const perm::fact& fact)
{
    out.title = QCoreApplication::translate("PermissionDialog", "Firewall");
    if (fact.why == perm::reason::ufw_active ||
        fact.why == perm::reason::firewalld_active)
    {
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog",
            "A firewall is on and may stop other computers "
            "reaching this port.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "Studio cannot read the firewall rules without administrator "
            "rights. If other computers cannot connect, open the port with "
            "the command below.");
    }
    else
    {
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog", "Studio cannot read the firewall rules.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "Reading them needs administrator rights, and neither ufw nor "
            "firewalld is on. If other computers cannot connect, check the "
            "rules with an administrator account, for example with "
            "sudo nft list ruleset.");
    }
    out.command_label =
        QCoreApplication::translate("PermissionDialog", "Command");
}

/// The wording for the macOS application firewall.
void describe_macos_firewall(wording& out, const perm::fact& fact)
{
    out.title =
        QCoreApplication::translate("PermissionDialog", "macOS firewall");
    if (fact.why == perm::reason::alf_prompt)
    {
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog",
            "The first connection from another computer makes macOS ask.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "With the firewall on, macOS asks whether the program may "
            "accept incoming connections, once for each program. Choose "
            "Allow. If you chose Don't Allow, open System Settings, "
            "Network, "
            "Firewall, Options, and set the program to Allow incoming "
            "connections, or run the commands below in a terminal. Studio "
            "never changes the firewall.");
        out.command_label =
            QCoreApplication::translate("PermissionDialog", "Commands");
    }
    else if (fact.why == perm::reason::block_all_inbound)
    {
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog",
            "The firewall blocks all incoming connections.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "That setting overrides every program. Open System Settings, "
            "Network, Firewall, Options, and turn off Block all incoming "
            "connections if you want other computers to connect.");
    }
    else
    {
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog", "Studio could not read the firewall.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "Look at System Settings, Network, Firewall. If it is on and "
            "other computers cannot connect, check the Options.");
    }
}

wording wording_for(const perm::fact& fact)
{
    const QString subject = QString::fromStdString(fact.subject);
    wording out;

    switch (fact.what)
    {
    case perm::topic::serial_access:
        describe_serial_access(out, fact, subject);
        break;
    case perm::topic::confinement:
        describe_confinement(out, fact, subject);
        break;
    case perm::topic::linux_firewall:
        describe_linux_firewall(out, fact);
        break;
    case perm::topic::macos_firewall:
        describe_macos_firewall(out, fact);
        break;
    case perm::topic::macos_local_network:
        out.title = QCoreApplication::translate("PermissionDialog",
                                                "Local Network access");
        out.subtitle = QCoreApplication::translate(
            "PermissionDialog",
            "macOS asks once whether Studio may use your local network.");
        out.body = QCoreApplication::translate(
            "PermissionDialog",
            "From macOS 15, an app needs your permission to find and connect "
            "to "
            "devices on the local network. Studio cannot check the answer. If "
            "you chose Don't Allow and other computers cannot connect, open "
            "System Settings, Privacy & Security, Local Network, and turn "
            "Virtual Serial Studio on.");
        break;
    default:
        out.title =
            QCoreApplication::translate("PermissionDialog", "Permissions");
        break;
    }
    return out;
}

} // namespace

PermissionDialog::PermissionDialog(const perm::fact& fact, bool step_possible,
                                   QWidget* parent)
    : QDialog(parent), fact_(fact)
{
    can_run_ = step_possible && fact.group_step.has_value();
    build();
}

void PermissionDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.permission_dialog.build");
    VSS_PROFILE_MESSAGE("gui.dialog.permission.opened");
    const wording words = wording_for(fact_);
    setWindowTitle(words.title);
    setAccessibleName(words.title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 0);
    layout->setSpacing(12);
    add_dialog_heading(layout, words.title, words.subtitle, this);
    if (!words.body.isEmpty())
        layout->addWidget(paragraph(words.body, this));

    if (!fact_.remedy.empty())
    {
        auto* command = new CodeBlock(this);
        command->set_text(QString::fromStdString(fact_.remedy),
                          words.command_label);
        layout->addWidget(command);
    }

    result_ = new NoteBanner(NoteBanner::kind::info, {}, this);
    result_->hide();
    layout->addWidget(result_);
    layout->addStretch(1);

    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    if (fact_.group_step.has_value())
    {
        const QString label =
            QCoreApplication::translate("PermissionDialog",
                                        "Add me to the %1 group")
                .arg(QString::fromStdString(fact_.group_step->group));
        step_ = make_dialog_button(label, "primary", footer);
        step_->setAutoDefault(false);
        step_->setAccessibleName(label);
        if (can_run_)
        {
            step_->setToolTip(QCoreApplication::translate(
                "PermissionDialog",
                "Your desktop will ask for your password before anything "
                "changes."));
            connect(step_, &QPushButton::clicked, this, [this] { run_step(); });
        }
        else
        {
            step_->setEnabled(false);
            step_->setToolTip(QCoreApplication::translate(
                "PermissionDialog", "Not available in this build"));
        }
        footer_layout->addWidget(step_);
    }
    auto* close = make_dialog_button(
        QCoreApplication::translate("PermissionDialog", "Close"), nullptr,
        footer);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close);
    layout->addWidget(footer);

    close->setFocus(Qt::OtherFocusReason);
}

void PermissionDialog::run_step()
{
    VSS_PROFILE_ZONE("gui.support.permission_dialog.run_step");
    // Never starts the system prompt unless the build offers the step.
    if (!can_run_ || step_ == nullptr || !fact_.group_step.has_value())
        return;
    step_->setEnabled(false);
    const step_report report = add_to_serial_group(*fact_.group_step);
    result_->set_kind(report.done() ? NoteBanner::kind::ok
                                    : NoteBanner::kind::warn);
    result_->set_text(report.message);
    result_->show();
    if (report.done())
    {
        done_ = true;
        return;
    }
    step_->setEnabled(true);
}

} // namespace vss::gui::support
