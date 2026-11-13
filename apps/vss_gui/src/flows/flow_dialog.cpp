//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/flow_dialog.hpp"

#include "theme/theme.hpp"
#include "widgets/common.hpp"

#include <vss/core/profile.hpp>

#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{

FlowDialog::FlowDialog(const QString& title, const QString& subtitle,
                       QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    setSizeGripEnabled(true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 20, 20, 0);
    outer->setSpacing(16);

    title_ = new QLabel(title, this);
    title_->setObjectName(QStringLiteral("flowTitle"));
    title_->setFont(Theme::instance().font(text_style::title));
    title_->setWordWrap(true);
    title_->setAccessibleName(title);
    outer->addWidget(title_);

    subtitle_ = new QLabel(subtitle, this);
    subtitle_->setObjectName(QStringLiteral("flowSubtitle"));
    subtitle_->setFont(Theme::instance().font(text_style::body));
    subtitle_->setWordWrap(true);
    subtitle_->setProperty("muted", true);
    subtitle_->setAccessibleName(subtitle);
    outer->addWidget(subtitle_);

    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("flowScroll"));
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setFocusPolicy(Qt::NoFocus);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll_->verticalScrollBar()->setAccessibleName(tr("Scroll"));
    scroll_->horizontalScrollBar()->setAccessibleName(tr("Scroll"));
    scroll_->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: 0; }"));
    scroll_->viewport()->setAutoFillBackground(false);

    body_ = new QWidget(scroll_);
    body_->setObjectName(QStringLiteral("flowBody"));
    body_->setAutoFillBackground(false);
    body_layout_ = new QVBoxLayout(body_);
    body_layout_->setContentsMargins(0, 0, 4, 0);
    body_layout_->setSpacing(16);
    scroll_->setWidget(body_);
    outer->addWidget(scroll_, 1);

    auto* footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("dialogFooter"));
    footer->setAttribute(Qt::WA_StyledBackground);
    footer_layout_ = new QHBoxLayout(footer);
    footer_layout_->setContentsMargins(20, 10, 20, 10);
    footer_layout_->setSpacing(8);
    footer_layout_->addStretch(1);
    outer->addWidget(footer);

    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this, footer]
            {
                apply_theme();
                footer->setStyleSheet(
                    QStringLiteral("QFrame#dialogFooter { background: %1; "
                                   "border-top: 1px solid %2; }")
                        .arg(Theme::instance()
                                 .colour(colour_role::sunken)
                                 .name(QColor::HexRgb),
                             Theme::instance()
                                 .colour(colour_role::line)
                                 .name(QColor::HexRgb)));
            });
    apply_theme();
    footer->setStyleSheet(
        QStringLiteral(
            "QFrame#dialogFooter { background: %1; border-top: 1px solid %2; }")
            .arg(Theme::instance()
                     .colour(colour_role::sunken)
                     .name(QColor::HexRgb),
                 Theme::instance()
                     .colour(colour_role::line)
                     .name(QColor::HexRgb)));

    // Enter runs the primary action from any field.  The shortcut belongs to
    // this window, so it does not fire through an open ErrorDialog.
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter})
    {
        auto* shortcut = new QShortcut(QKeySequence(key), this);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this,
                [this]
                {
                    if (primary_ != nullptr && primary_->isEnabled())
                        primary_->click();
                });
    }
}

QPushButton* FlowDialog::add_footer_button(const QString& text,
                                           const QString& role)
{
    auto* button = new QPushButton(text, this);
    button->setObjectName(role == QLatin1String("primary")
                              ? QStringLiteral("primaryButton")
                              : QStringLiteral("footerButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setAccessibleName(text);
    if (!role.isEmpty())
    {
        button->setProperty("role", role);
        polish(button);
    }
    footer_layout_->addWidget(button);
    if (role == QLatin1String("primary"))
    {
        primary_ = button;
        button->setDefault(true);
        button->setAutoDefault(true);
    }
    else
    {
        button->setAutoDefault(false);
    }
    return button;
}

void FlowDialog::set_primary_text(const QString& text)
{
    if (primary_ == nullptr)
        return;
    primary_->setText(text);
    primary_->setAccessibleName(text);
}

QString FlowDialog::setting_up_text()
{
    return tr("Setting up. Windows installs each new port, which can take up "
              "to 20 seconds.");
}

void FlowDialog::done(int result)
{
    // The worker reports to this dialog, so it stays until the answer is in.
    if (work_.busy())
        return;
    QDialog::done(result);
}

bool FlowDialog::run_in_background(std::function<void()> work,
                                   std::function<void()> finished,
                                   const QString& primary_text,
                                   const QString& status_text)
{
    if (work_.busy())
        return false;

    if (busy_row_ == nullptr)
    {
        // Outside the scrolling body, so it stays in view and is not greyed
        // out with the fields.
        busy_row_ = new QWidget(this);
        busy_row_->setObjectName(QStringLiteral("flowBusy"));
        auto* row = new QVBoxLayout(busy_row_);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        auto* bar = new QProgressBar(busy_row_);
        bar->setRange(0, 0);
        bar->setTextVisible(false);
        bar->setAccessibleName(tr("Progress"));
        row->addWidget(bar);
        busy_label_ = new QLabel(busy_row_);
        busy_label_->setObjectName(QStringLiteral("flowBusyText"));
        busy_label_->setFont(Theme::instance().font(text_style::body));
        busy_label_->setWordWrap(true);
        row->addWidget(busy_label_);
        busy_row_->setVisible(false);
        auto* outer = static_cast<QVBoxLayout*>(layout());
        outer->insertWidget(outer->indexOf(scroll_) + 1, busy_row_);
    }

    footer_state_.clear();
    for (int index = 0; index < footer_layout_->count(); ++index)
    {
        auto* button =
            qobject_cast<QPushButton*>(footer_layout_->itemAt(index)->widget());
        if (button == nullptr)
            continue;
        footer_state_.push_back({button, button->isEnabled()});
        button->setEnabled(false);
    }
    body_->setEnabled(false);
    if (primary_ != nullptr)
    {
        idle_primary_text_ = primary_->text();
        set_primary_text(primary_text);
    }
    busy_label_->setText(status_text);
    busy_label_->setAccessibleName(status_text);
    busy_row_->setVisible(true);
    announce_name_change(busy_label_);

    return work_.start(std::move(work),
                       [this, finished = std::move(finished)]
                       {
                           end_busy();
                           finished();
                       });
}

void FlowDialog::end_busy()
{
    for (const auto& [button, enabled] : footer_state_)
        button->setEnabled(enabled);
    footer_state_.clear();
    body_->setEnabled(true);
    if (primary_ != nullptr)
        set_primary_text(idle_primary_text_);
    busy_row_->setVisible(false);
}

void FlowDialog::set_primary_enabled(bool enabled)
{
    if (primary_ != nullptr)
        primary_->setEnabled(enabled);
}

void FlowDialog::set_subtitle_text(const QString& text)
{
    subtitle_->setText(text);
    subtitle_->setAccessibleName(text);
}

void FlowDialog::set_title_text(const QString& text)
{
    title_->setText(text);
    title_->setAccessibleName(text);
    setWindowTitle(text);
    setAccessibleName(text);
}

void FlowDialog::focus_first(QWidget* widget)
{
    if (widget == nullptr)
        return;
    QTimer::singleShot(0, widget,
                       [widget] { widget->setFocus(Qt::OtherFocusReason); });
}

void FlowDialog::finish_layout(int preferred_height)
{
    VSS_PROFILE_ZONE("gui.flows.dialog.finish_layout");
    // Keep short bodies at the top instead of stretching the last frame to
    // fill the viewport.  Tall bodies simply scroll.
    body_layout_->addStretch(1);
    const int width = Theme::instance()
                          .size(QStringLiteral("layout.dialogWidths.standard"))
                          .width();
    const QScreen* screen = parentWidget() != nullptr
                                ? parentWidget()->screen()
                                : QGuiApplication::primaryScreen();
    int height = preferred_height;
    if (screen != nullptr)
    {
        height = std::min(
            height, std::max(480, screen->availableGeometry().height() - 80));
    }
    setMinimumWidth(480);
    resize(width, height);
    if (primary_ != nullptr)
        primary_->setDefault(true);
}

void FlowDialog::apply_theme()
{
    VSS_PROFILE_ZONE("gui.flows.dialog.apply_theme");
    title_->setFont(Theme::instance().font(text_style::title));
    subtitle_->setFont(Theme::instance().font(text_style::body));
    const QColor panel = Theme::instance().colour(colour_role::panel);
    QPalette viewport = scroll_->viewport()->palette();
    viewport.setColor(QPalette::Window, panel);
    viewport.setColor(QPalette::Base, panel);
    scroll_->viewport()->setPalette(viewport);
    scroll_->viewport()->setAutoFillBackground(true);
    body_->setStyleSheet(QStringLiteral("QWidget#flowBody { background: %1; }")
                             .arg(panel.name(QColor::HexRgb)));
}

} // namespace vss::gui
