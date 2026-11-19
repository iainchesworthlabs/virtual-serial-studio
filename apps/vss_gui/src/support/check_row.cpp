//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/check_row.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/dialogs.hpp"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace vss::gui::support
{
namespace
{

struct tone_colours
{
    colour_role fill;
    colour_role glyph;
    StatusChip::kind chip;
};

tone_colours colours_for(row_tone tone)
{
    switch (tone)
    {
    case row_tone::ok:
        return {colour_role::okSoft, colour_role::ok,
                StatusChip::kind::healthy};
    case row_tone::warn:
        return {colour_role::amberSoft, colour_role::amber,
                StatusChip::kind::waiting};
    case row_tone::info:
        return {colour_role::accentSoft, colour_role::accentText,
                StatusChip::kind::fixture};
    case row_tone::neutral:
        break;
    }
    return {colour_role::fill, colour_role::ink2, StatusChip::kind::closed};
}

} // namespace

CheckRowWidget::CheckRowWidget(const spec& row, QWidget* parent)
    : QFrame(parent), row_(row)
{
    setFrameShape(QFrame::NoFrame);
    auto* layout = new QGridLayout(this);
    layout_ = layout;
    const Theme& theme = Theme::instance();
    const int tile = theme.metric(QStringLiteral("tile"));
    layout->setContentsMargins(theme.space(3), theme.space(2), theme.space(3),
                               theme.space(2));
    layout->setHorizontalSpacing(theme.space(2));
    layout->setVerticalSpacing(2);
    layout->setColumnMinimumWidth(0, tile);
    layout->setColumnStretch(1, 1);
    layout->setColumnStretch(2, 0);

    icon_holder_ = new QLabel(this);
    icon_holder_->setFixedSize(tile, tile);
    icon_holder_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* icon_layout = new QVBoxLayout(icon_holder_);
    icon_layout->setContentsMargins(0, 0, 0, 0);
    glyph_ = new QLabel(icon_holder_);
    glyph_->setAlignment(Qt::AlignCenter);
    icon_layout->addWidget(glyph_, 0, Qt::AlignCenter);
    layout->addWidget(icon_holder_, 0, 0, 3, 1, Qt::AlignTop);

    // Icon | title and sentence | status column.  The status column is its
    // own grid column so every chip shares one right edge and the text never
    // runs under it; the action sits under the sentence, left-aligned.
    title_ = new QLabel(row_.title, this);
    title_->setFont(Theme::instance().font(text_style::body));
    title_->setWordWrap(true);
    QFont bold = title_->font();
    bold.setWeight(QFont::DemiBold);
    title_->setFont(bold);
    layout->addWidget(title_, 0, 1, Qt::AlignVCenter);

    if (!row_.chip.isEmpty())
    {
        auto* chip =
            new StatusChip(row_.chip, colours_for(row_.tone).chip, this);
        chip->set_small(true);
        chip_ = chip;
        chip_->setAccessibleName(row_.chip);
        layout->addWidget(chip_, 0, 2, Qt::AlignRight | Qt::AlignVCenter);
    }

    auto* sentence = new QLabel(row_.sentence, this);
    sentence->setFont(Theme::instance().font(text_style::body));
    sentence->setProperty("muted", true);
    sentence->setWordWrap(true);
    sentence->setTextInteractionFlags(Qt::TextSelectableByMouse);
    polish(sentence);
    layout->addWidget(sentence, 1, 1);

    if (!row_.action_text.isEmpty())
    {
        action_ = make_dialog_button(
            row_.action_text, row_.primary_action ? "primary" : nullptr, this);
        action_->setAutoDefault(false);
        action_->setAccessibleName(row_.action_text);
        connect(action_, &QPushButton::clicked, this,
                &CheckRowWidget::action_triggered);
        layout->addWidget(action_, 2, 1, Qt::AlignLeft | Qt::AlignVCenter);
    }

    apply_style();
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { apply_style(); });
}

int CheckRowWidget::chip_width() const
{
    return chip_ != nullptr ? chip_->sizeHint().width() : 0;
}

void CheckRowWidget::set_status_column_width(int width)
{
    layout_->setColumnMinimumWidth(2, width);
}

QString CheckRowWidget::title_text() const
{
    return title_ != nullptr ? title_->text() : QString();
}

void CheckRowWidget::apply_style()
{
    if (row_.highlight)
        setProperty("note", QStringLiteral("warn"));
    else if (row_.framed)
        setProperty("card", true);
    setAttribute(Qt::WA_StyledBackground, true);
    polish(this);

    // A kind tile: a tinted rounded square with no edge (rule 8).
    const tone_colours colours = colours_for(row_.tone);
    icon_holder_->setStyleSheet(
        QStringLiteral("QLabel { background: %1; border: 0; "
                       "border-radius: %2px; }")
            .arg(Theme::instance().colour(colours.fill).name(QColor::HexRgb))
            .arg(Theme::instance().radius(QStringLiteral("tile"))));
    glyph_->setPixmap(
        IconProvider::instance()
            .icon(row_.icon.isEmpty() ? QStringLiteral("info") : row_.icon,
                  colours.glyph, 18)
            .pixmap(18, 18));
}

} // namespace vss::gui::support
