//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/wiring_dialog.hpp"

#include "advanced/chrome.hpp"
#include "advanced/line_matrix_panel.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

QString endpoint_state_word(vss::endpoint_state state)
{
    switch (state)
    {
    case vss::endpoint_state::open:
        return QCoreApplication::translate("WiringDialog", "Open");
    case vss::endpoint_state::failed:
        return QCoreApplication::translate("WiringDialog", "Failed");
    case vss::endpoint_state::created:
    case vss::endpoint_state::opening:
    case vss::endpoint_state::closed:
        return QCoreApplication::translate("WiringDialog", "Closed");
    }
    return QCoreApplication::translate("WiringDialog", "Closed");
}

StatusChip::kind endpoint_state_kind(vss::endpoint_state state)
{
    switch (state)
    {
    case vss::endpoint_state::open:
        return StatusChip::kind::healthy;
    case vss::endpoint_state::failed:
        return StatusChip::kind::failed;
    default:
        return StatusChip::kind::closed;
    }
}

} // namespace

WiringDialog::WiringDialog(StudioBackend& backend, vss::endpoint_id endpoint,
                           QWidget* parent)
    : QDialog(parent), backend_(backend), endpoint_(endpoint)
{
    // The port may have gone since the dialog was requested, so fall back to
    // a number for the title and handle a missing port below.
    const auto port = backend_.port(endpoint);
    const QString name =
        port ? port->name
             : tr("Port %1").arg(static_cast<qulonglong>(endpoint));
    const QString title = tr("Inspector: %1").arg(name);
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.table")),
        760);

    // Find the two-way link this port is on and the name of the port at its
    // other end.  The first matching link wins.
    QString other_name;
    bool linked = false;
    if (port)
    {
        for (const auto& link : backend_.links())
        {
            if (link.shape != link_shape::two_way)
                continue;
            const bool in_sources = link.sources.contains(endpoint);
            const bool in_targets = link.targets.contains(endpoint);
            if (!in_sources && !in_targets)
                continue;
            const auto other_ids = in_sources ? link.targets : link.sources;
            if (!other_ids.isEmpty())
            {
                if (const auto other = backend_.port(other_ids.first()))
                    other_name = other->name;
            }
            linked = true;
            break;
        }
    }
    other_name_ = std::move(other_name);
    linked_ = linked;

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 0);
    layout->setSpacing(16);

    // No port: show only a heading and a Close button, then stop building.
    if (!port)
    {
        layout->addWidget(make_header(title, tr("That port is no longer open."),
                                      this, true, {}));
        layout->addStretch(1);
        QHBoxLayout* footer_layout = nullptr;
        auto* footer = make_footer(this, footer_layout);
        auto* close = make_button(tr("Close"), "primary", footer);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        footer_layout->addWidget(close);
        layout->addWidget(footer);
        close->setDefault(true);
        close->setFocus(Qt::OtherFocusReason);
        return;
    }

    // Header chips: a fixture marker for a mock port, then the port's state.
    QVector<QWidget*> chips;
    if (port->kind == vss::endpoint_kind::mock)
    {
        fixture_chip_ = new StatusChip(tr("Fixture (mock)"),
                                       StatusChip::kind::fixture, this);
        chips << fixture_chip_;
    }
    state_chip_ = new StatusChip(endpoint_state_word(port->state),
                                 endpoint_state_kind(port->state), this);
    chips << state_chip_;

    layout->addWidget(make_header(title, {}, this, true, chips));

    // The subtitle says which link the wiring edits apply to.  The rich text
    // escapes the port name; the accessible name carries the plain text.
    subtitle_ = new QLabel(this);
    subtitle_->setFont(Theme::instance().font(text_style::body));
    subtitle_->setWordWrap(true);
    subtitle_->setTextFormat(Qt::RichText);
    set_ink2(subtitle_);
    if (linked_)
    {
        subtitle_->setText(
            tr("Linked two-way to <b>%1</b>. Wiring changes apply to that "
               "link only.")
                .arg(other_name_.toHtmlEscaped()));
        subtitle_->setAccessibleName(
            tr("Linked two-way to %1. Wiring changes apply to that link only.")
                .arg(other_name_));
    }
    else
    {
        subtitle_->setText(tr("Not linked to another port yet."));
        subtitle_->setAccessibleName(tr("Not linked to another port yet."));
    }
    layout->addWidget(subtitle_);

    // The panel does the real work; its messages appear in the notice below.
    panel_ = new LineMatrixPanel(backend_, endpoint_, this);
    connect(panel_, &LineMatrixPanel::message, this,
            &WiringDialog::show_notice);
    layout->addWidget(panel_, 1);

    // Hidden until show_notice() has something to say.
    notice_ = new QLabel(this);
    notice_->setFont(Theme::instance().font(text_style::caption));
    notice_->setProperty("muted", true);
    notice_->setWordWrap(true);
    notice_->hide();
    layout->addWidget(notice_);

    // Footer: a reminder on the left and Close on the right.
    QHBoxLayout* footer_layout = nullptr;
    auto* footer = make_footer(this, footer_layout);
    auto* note =
        new QLabel(tr("Faults are only available on fixture ports."), footer);
    note->setFont(Theme::instance().font(text_style::caption));
    note->setProperty("muted", true);
    footer_layout->insertWidget(0, note);
    auto* close = make_button(tr("Close"), "primary", footer);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close);
    layout->addWidget(footer);

    // Close is the default button.  Focus it now, and again once the event
    // loop runs so the focus still holds after the dialog is shown.
    close->setDefault(true);
    close->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, close,
                       [close] { close->setFocus(Qt::OtherFocusReason); });
}

QString WiringDialog::subtitle_text() const
{
    return subtitle_ == nullptr ? QString() : subtitle_->text();
}

QString WiringDialog::notice_text() const
{
    return notice_ == nullptr ? QString() : notice_->text();
}

void WiringDialog::show_notice(const QString& text)
{
    if (notice_ == nullptr)
        return;
    notice_->setText(text);
    notice_->setAccessibleName(text);
    notice_->setVisible(!text.isEmpty());
}

} // namespace vss::gui
