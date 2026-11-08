//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/firewall_dialog.hpp"

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

QString protocol_name(perm::transport_protocol protocol)
{
    return protocol == perm::transport_protocol::tcp ? QStringLiteral("TCP")
                                                     : QStringLiteral("UDP");
}

QString networks_text(long profiles)
{
    QStringList names;
    if ((profiles & perm::profile_domain) != 0)
        names << QCoreApplication::translate("FirewallDialog", "domain");
    if ((profiles & perm::profile_private) != 0)
        names << QCoreApplication::translate("FirewallDialog", "private");
    if ((profiles & perm::profile_public) != 0)
        names << QCoreApplication::translate("FirewallDialog", "public");
    return names.join(QStringLiteral(", "));
}

QLabel* paragraph(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::body));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

} // namespace

FirewallDialog::FirewallDialog(const perm::fact& fact, bool step_possible,
                               QWidget* parent)
    : QDialog(parent), fact_(fact)
{
    can_run_ = step_possible && fact.step.has_value();
    build(can_run_);
}

void FirewallDialog::build(bool step_possible)
{
    VSS_PROFILE_ZONE("gui.support.firewall.build");
    VSS_PROFILE_MESSAGE("gui.dialog.firewall.opened");
    // Window chrome: accessible name, panel surface and the small dialog
    // width as the minimum.
    setWindowTitle(
        QCoreApplication::translate("FirewallDialog", "Windows Firewall"));
    setAccessibleName(
        QCoreApplication::translate("FirewallDialog", "Windows Firewall"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 0);
    layout->setSpacing(12);

    // Three situations share this dialog. With no allow rule Studio can offer
    // to add one; a block rule or the block-all setting needs the user to
    // change Windows themselves.
    const bool no_rule = fact_.why == perm::reason::no_allow_rule;
    add_dialog_heading(
        layout,
        QCoreApplication::translate("FirewallDialog", "Windows Firewall"),
        no_rule ? QCoreApplication::translate(
                      "FirewallDialog",
                      "Other computers cannot reach this port until a rule "
                      "allows it.")
                : QCoreApplication::translate(
                      "FirewallDialog",
                      "Windows Firewall stops other computers reaching this "
                      "port."),
        this);

    // Explain the cause in words that match the reason.
    if (no_rule)
    {
        layout->addWidget(paragraph(
            QCoreApplication::translate(
                "FirewallDialog",
                "Studio is waiting for other computers to connect. Windows "
                "Firewall blocks connections that no rule allows. Studio never "
                "changes the firewall by itself."),
            this));
    }
    else if (fact_.why == perm::reason::blocked_by_rule)
    {
        // Studio never removes a block rule, so the text only says where to
        // find it.
        layout->addWidget(paragraph(
            QCoreApplication::translate(
                "FirewallDialog",
                "A rule in Windows Firewall blocks this program on this port. "
                "A block rule wins over an allow rule, and Studio does not "
                "remove it. Open Windows Defender Firewall with Advanced "
                "Security, find the block rule under Inbound Rules, and "
                "disable or delete it if you want other computers to connect."),
            this));
    }
    else
    {
        // The remaining reason: the block-all-incoming setting, which beats
        // every rule.
        layout->addWidget(paragraph(
            QCoreApplication::translate(
                "FirewallDialog",
                "Windows Firewall is set to block all incoming connections, "
                "which overrides every rule. Open Windows Security, choose "
                "Firewall and network protection, pick the network you are on, "
                "and turn off Block all incoming connections."),
            this));
    }

    // When there is a rule Studio could add, spell out exactly what it would
    // allow, so the user can judge it before the permission prompt.
    if (fact_.step.has_value())
    {
        const perm::firewall_rule_spec& step = *fact_.step;
        layout->addWidget(
            paragraph(QCoreApplication::translate(
                          "FirewallDialog",
                          "The rule Studio would add lets only this in:"),
                      this));
        auto* details = paragraph(
            QCoreApplication::translate(
                "FirewallDialog",
                "Program: %1\nProtocol and port: %2 %3\nWho: %4\nNetworks: %5")
                .arg(QString::fromStdString(step.program),
                     protocol_name(step.protocol))
                .arg(step.port)
                .arg(step.scope == perm::rule_scope::local_subnet
                         ? QCoreApplication::translate(
                               "FirewallDialog",
                               "computers on your local network")
                         : QCoreApplication::translate("FirewallDialog",
                                                       "any computer"),
                     networks_text(step.profiles)),
            this);
        layout->addWidget(details);
    }

    // Warn about the Windows permission prompt only when this build can
    // really run the elevated step.
    if (fact_.step.has_value() && step_possible)
    {
        layout->addWidget(paragraph(
            QCoreApplication::translate(
                "FirewallDialog",
                "Changing the firewall needs administrator rights, so "
                "Windows will show a permission prompt. It names the "
                "program and its publisher: choose Yes if they match. "
                "Studio itself keeps running without administrator rights."),
            this));
    }

    // Show the command form of the fix. The lead-in differs: an alternative
    // when the button works, the only route when it does not.
    if (!fact_.remedy.empty())
    {
        layout->addWidget(paragraph(
            step_possible
                ? QCoreApplication::translate(
                      "FirewallDialog",
                      "The same rule as a command, if you prefer to run it "
                      "yourself in a terminal opened as administrator:")
                : QCoreApplication::translate(
                      "FirewallDialog",
                      "To add the rule yourself, run this in a terminal opened "
                      "as administrator:"),
            this));
        auto* command = new CodeBlock(this);
        command->set_text(
            QString::fromStdString(fact_.remedy),
            QCoreApplication::translate("FirewallDialog", "Command"));
        layout->addWidget(command);
    }

    // The result banner stays hidden until allow() reports an outcome.
    result_ = new NoteBanner(NoteBanner::kind::info, {}, this);
    result_->hide();
    layout->addWidget(result_);
    layout->addStretch(1);

    // Footer. The Allow button exists only when no rule is the problem; the
    // other reasons can only be fixed in Windows itself.
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    if (no_rule)
    {
        allow_ = make_dialog_button(
            QCoreApplication::translate("FirewallDialog",
                                        "Allow in Windows Firewall"),
            "primary", footer);
        // Not an auto-default, so Enter never starts the elevated step; Close
        // is the default button.
        allow_->setAutoDefault(false);
        allow_->setAccessibleName(QCoreApplication::translate(
            "FirewallDialog", "Allow in Windows Firewall"));
        if (step_possible)
        {
            allow_->setToolTip(QCoreApplication::translate(
                "FirewallDialog",
                "Windows will ask for administrator permission before the "
                "rule is added."));
            connect(allow_, &QPushButton::clicked, this, [this] { allow(); });
        }
        else
        {
            // Keep the button visible but disabled, with the reason in the
            // tooltip, rather than hiding it.
            allow_->setEnabled(false);
            allow_->setToolTip(QCoreApplication::translate(
                "FirewallDialog", "Not available in this build"));
        }
        footer_layout->addWidget(allow_);
    }
    auto* close = make_dialog_button(
        QCoreApplication::translate("FirewallDialog", "Close"), nullptr,
        footer);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close);
    layout->addWidget(footer);

    close->setFocus(Qt::OtherFocusReason);
}

void FirewallDialog::allow()
{
    VSS_PROFILE_ZONE("gui.support.firewall.allow");
    // Never starts the operating system prompt unless the build offers the
    // step.
    if (!can_run_ || allow_ == nullptr || !fact_.step)
        return;
    allow_->setEnabled(false);
    const step_report report = allow_through_firewall(*fact_.step);
    result_->set_kind(report.done() ? NoteBanner::kind::ok
                                    : NoteBanner::kind::warn);
    result_->set_text(report.message);
    result_->show();
    if (report.done())
    {
        added_ = true;
        Q_EMIT rule_added_signal();
        return;
    }
    // Declined or failed: the user may try again.
    allow_->setEnabled(true);
}

} // namespace vss::gui::support
