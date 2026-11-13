//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "widgets/surfaces.hpp"

#include "theme/focus_ring_style.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

void repolish(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

PaneHeader::PaneHeader(const QString& title, QWidget* parent) : QFrame(parent)
{
    setProperty("paneHeader", true);
    setObjectName(QStringLiteral("paneHeader"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    const Theme& theme = Theme::instance();
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(theme.space(3) - 2, 0, theme.space(1), 0);
    layout_->setSpacing(theme.space(1));

    title_ = new QLabel(title, this);
    title_->setObjectName(QStringLiteral("paneTitle"));
    title_->setAccessibleName(title);
    layout_->addWidget(title_);

    count_ = new QLabel(this);
    count_->setObjectName(QStringLiteral("paneCount"));
    count_->setAlignment(Qt::AlignCenter);
    count_->setFixedHeight(20);
    count_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    count_->hide();
    layout_->addWidget(count_);
    layout_->addStretch(1);

    setAccessibleName(title);
    apply_theme();
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { apply_theme(); });
}

void PaneHeader::apply_theme()
{
    const Theme& theme = Theme::instance();
    setMinimumHeight(theme.metric(QStringLiteral("paneHeader")));
    title_->setFont(theme.font(text_style::heading));
    count_->setFont(theme.font(text_style::caption));
    count_->setStyleSheet(
        QStringLiteral("QLabel#paneCount { background: %1; color: %2; "
                       "border-radius: 10px; padding: 0 7px; "
                       "}")
            .arg(theme.colour(colour_role::fill).name(),
                 theme.colour(colour_role::ink3).name()));
}

void PaneHeader::set_title(const QString& title)
{
    title_->setText(title);
    title_->setAccessibleName(title);
    setAccessibleName(title);
}

QString PaneHeader::title() const
{
    return title_->text();
}

void PaneHeader::set_count(int count)
{
    if (count < 0)
    {
        count_->hide();
        return;
    }
    count_->setText(QString::number(count));
    count_->setAccessibleName(tr("%n item(s)", nullptr, count));
    count_->show();
}

void PaneHeader::add_action(QWidget* widget)
{
    layout_->addWidget(widget);
}

void PaneHeader::add_leading(QWidget* widget)
{
    layout_->insertWidget(leading_index_, widget);
    ++leading_index_;
}

void PaneHeader::set_fill(QWidget* widget)
{
    if (fill_ != nullptr)
    {
        layout_->removeWidget(fill_);
        fill_ = nullptr;
    }
    // The spacer after the title keeps no stretch while a fill widget takes
    // all the spare width.
    const int spacer = leading_index_;
    if (widget == nullptr)
    {
        layout_->setStretch(spacer, 1);
        return;
    }
    layout_->insertWidget(leading_index_, widget, 1);
    layout_->setStretch(leading_index_ + 1, 0);
    fill_ = widget;
}

int PaneHeader::lead_width() const
{
    const int count = count_->isHidden() ? 0 : count_->sizeHint().width();
    return layout_->contentsMargins().left() +
           layout_->contentsMargins().right() + title_->sizeHint().width() +
           count + 2 * layout_->spacing();
}

Pane::Pane(const QString& title, QWidget* parent) : QFrame(parent)
{
    setProperty("pane", true);
    layout_ = new QVBoxLayout(this);
    // One pixel for the border, so the body's corners stay inside it.
    layout_->setContentsMargins(1, 1, 1, 1);
    layout_->setSpacing(0);
    header_ = new PaneHeader(title, this);
    layout_->addWidget(header_);
    setAccessibleName(title);
}

void Pane::set_body(QWidget* body)
{
    if (body_ != nullptr)
    {
        layout_->removeWidget(body_);
        body_->deleteLater();
    }
    body_ = body;
    if (body_ != nullptr)
    {
        body_->setParent(this);
        layout_->addWidget(body_, 1);
    }
}

KindTile::KindTile(const QString& icon, tint value, QWidget* parent)
    : QFrame(parent), icon_(icon), tint_(value)
{
    const int size = Theme::instance().metric(QStringLiteral("tile"));
    setFixedSize(size, size);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    glyph_ = new QLabel(this);
    glyph_->setAlignment(Qt::AlignCenter);
    glyph_->setStyleSheet(QStringLiteral("background: transparent;"));
    layout->addWidget(glyph_);
    refresh();
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { refresh(); });
}

void KindTile::set_icon(const QString& icon)
{
    icon_ = icon;
    refresh();
}

void KindTile::set_tint(tint value)
{
    tint_ = value;
    refresh();
}

void KindTile::refresh()
{
    QString tint_name;
    colour_role ink = colour_role::accentText;
    switch (tint_)
    {
    case tint::accent:
        tint_name = QStringLiteral("accent");
        ink = colour_role::accentText;
        break;
    case tint::violet:
        tint_name = QStringLiteral("violet");
        ink = colour_role::violet;
        break;
    case tint::ok:
        tint_name = QStringLiteral("ok");
        ink = colour_role::ok;
        break;
    case tint::neutral:
        tint_name = QStringLiteral("neutral");
        ink = colour_role::ink2;
        break;
    }
    setProperty("tile", tint_name);
    repolish(this);
    const QIcon icon = IconProvider::instance().icon(icon_, ink, 16);
    glyph_->setPixmap(icon.pixmap(QSize(16, 16), devicePixelRatioF()));
}

ActionTile::ActionTile(const QString& icon, KindTile::tint tint,
                       const QString& title, const QString& description,
                       QWidget* parent)
    : QAbstractButton(parent)
{
    const Theme& theme = Theme::instance();
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setText(title);
    setAccessibleName(title);
    setAccessibleDescription(description);

    auto* layout = new QVBoxLayout(this);
    const int pad = theme.space(3) - 2;
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.space(1));
    tile_ = new KindTile(icon, tint, this);
    layout->addWidget(tile_, 0, Qt::AlignLeft);
    title_ = new QLabel(title, this);
    QFont strong = theme.font(text_style::body);
    strong.setWeight(QFont::DemiBold);
    title_->setFont(strong);
    title_->setWordWrap(true);
    description_ = new QLabel(description, this);
    description_->setProperty("secondary", true);
    description_->setFont(theme.font(text_style::caption));
    description_->setWordWrap(true);
    for (QLabel* label : {title_, description_})
    {
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setStyleSheet(QStringLiteral("background: transparent;"));
        layout->addWidget(label);
    }
    layout->addStretch(1);
    connect(this, &QAbstractButton::toggled, this, [this] { update(); });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { update(); });
}

void ActionTile::set_description(const QString& description)
{
    description_->setText(description);
    setAccessibleDescription(description);
    updateGeometry();
}

QString ActionTile::description() const
{
    return description_->text();
}

QSize ActionTile::sizeHint() const
{
    const int width = 250;
    return QSize(width, heightForWidth(width));
}

QSize ActionTile::minimumSizeHint() const
{
    return QSize(160, heightForWidth(160));
}

int ActionTile::heightForWidth(int width) const
{
    return layout() != nullptr ? layout()->totalHeightForWidth(width)
                               : QAbstractButton::heightForWidth(width);
}

void ActionTile::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const Theme& theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal radius = theme.radius(QStringLiteral("card"));
    const bool checked = isCheckable() && isChecked();
    QColor fill = theme.colour(colour_role::panel);
    if (checked)
        fill = theme.colour(colour_role::accentSoft);
    else if (isDown())
        fill = theme.colour(colour_role::pressed);
    else if (hovered_ && isEnabled())
        fill = theme.colour(colour_role::hover);
    QColor edge = theme.colour(colour_role::line2);
    qreal edge_width = 1.0;
    if (checked)
    {
        edge = theme.colour(colour_role::accent);
        edge_width = 2.0;
    }
    else if (hovered_ && isEnabled())
    {
        edge = theme.colour(colour_role::control);
    }
    const qreal half = edge_width / 2.0;
    const QRectF frame = QRectF(rect()).adjusted(half, half, -half, -half);
    painter.setPen(QPen(edge, edge_width));
    painter.setBrush(fill);
    painter.drawRoundedRect(frame, radius, radius);

    if (hasFocus() && FocusRingStyle::keyboard_focus())
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(theme.colour(colour_role::focus), 2.0));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), radius,
                                radius);
    }
}

void ActionTile::enterEvent(QEnterEvent* event)
{
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
}

void ActionTile::leaveEvent(QEvent* event)
{
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

GroupLabel::GroupLabel(const QString& text, QWidget* parent)
    : QLabel(text, parent)
{
    setProperty("groupLabel", true);
    setFont(Theme::instance().font(text_style::group));
    setAccessibleName(text);
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { setFont(Theme::instance().font(text_style::group)); });
}

Group::Group(QWidget* parent) : QFrame(parent)
{
    setProperty("group", true);
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(1, 1, 1, 1);
    layout_->setSpacing(0);
}

QFrame* make_group_divider(QWidget* parent)
{
    auto* divider = new QFrame(parent);
    divider->setProperty("groupDivider", true);
    divider->setFixedHeight(1);
    return divider;
}

void Group::add_row(QWidget* row)
{
    if (count_ > 0)
        layout_->addWidget(make_group_divider(this));
    row->setParent(this);
    layout_->addWidget(row);
    ++count_;
}

QWidget* Group::add_field(const QString& label, QWidget* value)
{
    const Theme& theme = Theme::instance();
    auto* row = new QWidget(this);
    row->setMinimumHeight(36);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(theme.space(2) - 2, theme.space(0),
                               theme.space(1), theme.space(0));
    layout->setSpacing(theme.space(1));
    auto* name = new QLabel(label, row);
    name->setProperty("muted", true);
    name->setMinimumWidth(64);
    name->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(name);
    value->setParent(row);
    layout->addWidget(value, 1);
    if (auto* text = qobject_cast<QLabel*>(value))
        text->setBuddy(nullptr);
    add_row(row);
    return row;
}

DialogFrame::DialogFrame(QWidget* parent) : QWidget(parent)
{
    const Theme& theme = Theme::instance();
    const int padding = theme.metric(QStringLiteral("layout.dialogPadding"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* head = new QWidget(this);
    auto* head_layout = new QVBoxLayout(head);
    head_layout->setContentsMargins(padding, padding, padding, theme.space(3));
    head_layout->setSpacing(theme.space(0));
    title_ = new QLabel(head);
    title_->setFont(theme.font(text_style::title));
    title_->setWordWrap(true);
    subtitle_ = new QLabel(head);
    subtitle_->setProperty("secondary", true);
    subtitle_->setWordWrap(true);
    subtitle_->hide();
    head_layout->addWidget(title_);
    head_layout->addWidget(subtitle_);
    outer->addWidget(head);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll_);
    content->setObjectName(QStringLiteral("dialogBody"));
    content->setStyleSheet(
        QStringLiteral("QWidget#dialogBody { background: transparent; }"));
    scroll_->viewport()->setAutoFillBackground(false);
    body_ = new QVBoxLayout(content);
    body_->setContentsMargins(padding, 0, padding, theme.space(1));
    body_->setSpacing(theme.metric(QStringLiteral("layout.groupGap")));
    scroll_->setWidget(content);
    outer->addWidget(scroll_, 1);

    auto* foot = new QFrame(this);
    foot->setObjectName(QStringLiteral("dialogFooter"));
    footer_ = new QHBoxLayout(foot);
    footer_->setContentsMargins(padding, theme.space(2), padding, padding - 4);
    footer_->setSpacing(theme.space(1));
    footer_->addStretch(1);
    outer->addWidget(foot);

    connect(&Theme::instance(), &Theme::theme_changed, this, [this]
            { title_->setFont(Theme::instance().font(text_style::title)); });
}

void DialogFrame::set_title(const QString& title)
{
    title_->setText(title);
    title_->setAccessibleName(title);
}

void DialogFrame::set_subtitle(const QString& subtitle)
{
    subtitle_->setText(subtitle);
    subtitle_->setVisible(!subtitle.isEmpty());
}

void DialogFrame::add_footer_button(QPushButton* button)
{
    footer_->addWidget(button);
}

void DialogFrame::add_footer_leading(QWidget* widget)
{
    footer_->insertWidget(leading_, widget);
    ++leading_;
}

} // namespace vss::gui
