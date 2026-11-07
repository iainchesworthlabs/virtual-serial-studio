//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/setup_check_dialog.hpp"

#include "support/check_row.hpp"
#include "support/driver_setup_dialog.hpp"
#include "support/elevated_step.hpp"
#include "support/firewall_dialog.hpp"
#include "support/permission_dialog.hpp"
#include "support/permission_rows.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"
#include "widgets/surfaces.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace vss::gui::support
{
namespace
{

row_tone tone_for(setup_state state)
{
    switch (state)
    {
    case setup_state::ok:
        return row_tone::ok;
    case setup_state::info:
        return row_tone::info;
    case setup_state::attention:
        return row_tone::warn;
    case setup_state::not_checked:
        break;
    }
    return row_tone::neutral;
}

void show_link_failure(QWidget* parent)
{
    ErrorDialog dialog(
        QCoreApplication::translate("SetupCheck",
                                    "Could not open the project page"),
        QCoreApplication::translate(
            "SetupCheck",
            "Your browser did not open. Copy the link and open it yourself."),
        {}, parent);
    dialog.exec();
}

/// The instructions dialog behind "Install com0com" and "Why is a driver
/// needed?".  It never installs or downloads anything itself.
void show_driver_instructions(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate(
        "SetupCheck", "Install the com0com driver"));
    dialog.setAccessibleName(QCoreApplication::translate(
        "SetupCheck", "Install the com0com driver"));
    dialog.setProperty("surface", QStringLiteral("panel"));
    polish(&dialog);
    dialog.setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 0);
    layout->setSpacing(12);
    add_dialog_heading(
        layout,
        QCoreApplication::translate("SetupCheck", "Install the com0com driver"),
        QCoreApplication::translate(
            "SetupCheck",
            "Virtual Serial Studio does not install drivers for you."),
        &dialog);

    auto* why = new QLabel(
        QCoreApplication::translate(
            "SetupCheck",
            "Other programs cannot see a virtual port until a driver creates a "
            "COM port. On Windows that driver is com0com."),
        &dialog);
    why->setFont(Theme::instance().font(text_style::body));
    why->setWordWrap(true);
    layout->addWidget(why);

    auto* steps = new QLabel(
        QCoreApplication::translate(
            "SetupCheck",
            "1. Open the com0com project page.\n"
            "2. Download the latest setup program.\n"
            "3. Run it as administrator and keep the default options.\n"
            "4. Come back to Setup check and choose Re-check."),
        &dialog);
    steps->setFont(Theme::instance().font(text_style::body));
    steps->setWordWrap(true);
    steps->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(steps);

    auto* link = new CodeBlock(&dialog);
    link->set_text(
        com0com_project_url(),
        QCoreApplication::translate("SetupCheck", "com0com project page"));
    layout->addWidget(link);
    layout->addStretch(1);

    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(&dialog, footer_layout);
    auto* open = make_dialog_button(
        QCoreApplication::translate("SetupCheck", "Open project page"),
        "primary", footer);
    open->setAutoDefault(false);
    QObject::connect(open, &QPushButton::clicked, &dialog,
                     [&dialog]
                     {
                         if (!open_com0com_project_page())
                             show_link_failure(&dialog);
                     });
    footer_layout->addWidget(open);
    auto* close = make_dialog_button(
        QCoreApplication::translate("SetupCheck", "Close"), nullptr, footer);
    close->setDefault(true);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    footer_layout->addWidget(close);
    layout->addWidget(footer);

    close->setFocus(Qt::OtherFocusReason);
    dialog.exec();
}

void show_dialout_help(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate(
        "SetupCheck", "Add me to the dialout group"));
    dialog.setAccessibleName(QCoreApplication::translate(
        "SetupCheck", "Add me to the dialout group"));
    dialog.setProperty("surface", QStringLiteral("panel"));
    polish(&dialog);
    dialog.setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 0);
    layout->setSpacing(12);
    add_dialog_heading(
        layout,
        QCoreApplication::translate("SetupCheck",
                                    "Add me to the dialout group"),
        QCoreApplication::translate(
            "SetupCheck",
            "Run this command in a terminal, then sign out and back in."),
        &dialog);

    auto* command = new CodeBlock(&dialog);
    command->set_text(dialout_add_command(),
                      QCoreApplication::translate("SetupCheck", "Command"));
    layout->addWidget(command);
    layout->addStretch(1);

    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(&dialog, footer_layout);
    auto* close = make_dialog_button(
        QCoreApplication::translate("SetupCheck", "Close"), "primary", footer);
    close->setDefault(true);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    footer_layout->addWidget(close);
    layout->addWidget(footer);

    close->setFocus(Qt::OtherFocusReason);
    dialog.exec();
}

} // namespace

SetupCheckDialog::SetupCheckDialog(StudioBackend& backend, QWidget* parent)
    : QDialog(parent), backend_(backend)
{
    build();
    report_ = query_setup_report(backend_);
    rebuild();
}

void SetupCheckDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.setup_check.build");
    VSS_PROFILE_MESSAGE("gui.dialog.setup_check.opened");
    setWindowTitle(QCoreApplication::translate("SetupCheck", "Setup check"));
    setAccessibleName(QCoreApplication::translate("SetupCheck", "Setup check"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    const int width = Theme::instance().metric(
        QStringLiteral("layout.dialogWidths.standard"));
    resize(width, 720);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(24, 20, 24, 14);
    header_layout->setSpacing(12);
    auto* heading = new QVBoxLayout();
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(4);
    add_dialog_heading(
        heading, QCoreApplication::translate("SetupCheck", "Setup check"),
        QCoreApplication::translate(
            "SetupCheck",
            "What this PC needs so other programs can use your virtual ports."),
        header);
    header_layout->addLayout(heading, 1);
    header_chip_ =
        new StatusChip(QCoreApplication::translate("SetupCheck", "All set"),
                       StatusChip::kind::healthy, header);
    header_layout->addWidget(header_chip_, 0, Qt::AlignTop);
    outer->addWidget(header);

    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(24, 4, 24, 16);
    body_layout->setSpacing(10);

    driver_note_ = new NoteBanner(NoteBanner::kind::warn, {}, body);
    driver_note_->hide();
    body_layout->addWidget(driver_note_);

    // One grouped section with dividers, not a stack of outlined cards
    // (`10-design-language.md`, rule 10).
    auto* group = new QFrame(body);
    group->setProperty("group", true);
    group->setAttribute(Qt::WA_StyledBackground, true);
    polish(group);
    rows_host_ = group;
    rows_layout_ = new QVBoxLayout(rows_host_);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(0);
    body_layout->addWidget(rows_host_);

    platform_note_ = new QLabel(body);
    platform_note_->setFont(Theme::instance().font(text_style::caption));
    platform_note_->setProperty("muted", true);
    platform_note_->setWordWrap(true);
    polish(platform_note_);
    body_layout->addWidget(platform_note_);
    body_layout->addStretch(1);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    auto* why = make_dialog_button(
        QCoreApplication::translate("SetupCheck", "Why is a driver needed?"),
        "quiet", footer);
    why->setAutoDefault(false);
    connect(why, &QPushButton::clicked, this,
            [this] { show_driver_instructions(this); });
    why->setVisible(host_platform() == QLatin1String("windows"));
    footer_layout->addWidget(why);
    auto* recheck = make_dialog_button(
        QCoreApplication::translate("SetupCheck", "Re-check"), nullptr, footer);
    recheck->setAutoDefault(false);
    connect(recheck, &QPushButton::clicked, this, &SetupCheckDialog::recheck);
    footer_layout->addWidget(recheck);
    auto* close = make_dialog_button(
        QCoreApplication::translate("SetupCheck", "Close"), "primary", footer);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close);
    outer->addWidget(footer);

    close->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, close,
                       [close] { close->setFocus(Qt::OtherFocusReason); });
}

void SetupCheckDialog::recheck()
{
    VSS_PROFILE_ZONE("gui.support.setup_check.recheck");
    report_ = query_setup_report(backend_);
    rebuild();
}

CheckRowWidget* SetupCheckDialog::row_widget(int index) const
{
    if (index < 0 || index >= rows_.size())
        return nullptr;
    return rows_.at(index);
}

void SetupCheckDialog::rebuild()
{
    VSS_PROFILE_ZONE("gui.support.setup_check.rebuild");
    while (QLayoutItem* item = rows_layout_->takeAt(0))
    {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    rows_.clear();

    for (const setup_row& row : report_.rows)
    {
        CheckRowWidget::spec spec;
        spec.tone = tone_for(row.state);
        spec.icon = row.icon;
        spec.title = row.title;
        spec.chip = row.chip;
        spec.sentence = row.sentence;
        spec.action_text = row.action_text;
        spec.primary_action = row.action == setup_action::install_driver ||
                              row.action == setup_action::setup_driver;
        spec.highlight = row.highlight;
        spec.framed = false;
        if (!rows_.isEmpty())
            rows_layout_->addWidget(make_group_divider(rows_host_));
        auto* widget = new CheckRowWidget(spec, rows_host_);
        widget->setAccessibleName(row.chip.isEmpty()
                                      ? row.title
                                      : tr("%1, %2").arg(row.title, row.chip));
        const setup_action action = row.action;
        connect(widget, &CheckRowWidget::action_triggered, this,
                [this, action] { handle_action(action); });
        rows_layout_->addWidget(widget);
        rows_.push_back(widget);
    }

    switch (report_.state)
    {
    case setup_report::readiness::ready:
        header_chip_->set_kind(StatusChip::kind::healthy);
        break;
    case setup_report::readiness::needs_attention:
        header_chip_->set_kind(StatusChip::kind::waiting);
        break;
    case setup_report::readiness::not_checked:
        header_chip_->set_kind(StatusChip::kind::closed);
        break;
    }
    header_chip_->set_text(report_.header_chip);

    driver_note_->setVisible(!report_.driver_note.isEmpty());
    if (!report_.driver_note.isEmpty())
        driver_note_->set_text(report_.driver_note);
    platform_note_->setText(report_.platform_note);
}

void SetupCheckDialog::show_driver_setup()
{
    DriverSetupDialog dialog(backend_, backend_.check_driver().problem, this);
    dialog.exec();
    // Whatever happened, ask again: the row changes when the set-up worked.
    recheck();
}

void SetupCheckDialog::show_firewall_help()
{
    // Ask again: the user may have changed the firewall since the last check.
    const auto facts = vss::permissions::probe(permission_context(backend_));
    if (const auto windows =
            find_fact(facts, vss::permissions::topic::windows_firewall))
    {
        FirewallDialog dialog(*windows, firewall_step_available(), this);
        dialog.exec();
        if (dialog.rule_added())
            recheck();
        return;
    }
    if (find_fact(facts, vss::permissions::topic::linux_firewall))
    {
        show_permission_help(vss::permissions::topic::linux_firewall);
        return;
    }
    show_permission_help(vss::permissions::topic::macos_firewall);
}

void SetupCheckDialog::show_permission_help(vss::permissions::topic what)
{
    // Ask again, for the same reason.
    const auto fact =
        find_fact(vss::permissions::probe(permission_context(backend_)), what);
    if (!fact)
        return;
    PermissionDialog dialog(*fact, group_step_available(), this);
    dialog.exec();
    if (dialog.step_done())
        recheck();
}

void SetupCheckDialog::handle_action(setup_action action)
{
    switch (action)
    {
    case setup_action::none:
        break;
    case setup_action::install_driver:
        show_driver_instructions(this);
        break;
    case setup_action::setup_driver:
        show_driver_setup();
        break;
    case setup_action::restart_elevated:
        Q_EMIT restart_as_administrator_requested();
        break;
    case setup_action::refresh_devices:
        recheck();
        break;
    case setup_action::continue_test:
        Q_EMIT continue_in_test_mode_requested();
        accept();
        break;
    case setup_action::dialout_help:
        show_dialout_help(this);
        break;
    case setup_action::firewall_help:
        show_firewall_help();
        break;
    case setup_action::serial_access_help:
        show_permission_help(vss::permissions::topic::serial_access);
        break;
    case setup_action::confinement_help:
        show_permission_help(vss::permissions::topic::confinement);
        break;
    case setup_action::network_privacy_help:
        show_permission_help(vss::permissions::topic::macos_local_network);
        break;
    }
}

} // namespace vss::gui::support
