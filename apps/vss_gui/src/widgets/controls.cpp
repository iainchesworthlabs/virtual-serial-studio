//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "widgets/controls.hpp"

#include "theme/focus_ring_style.hpp"
#include "theme/icon_provider.hpp"

#include <QButtonGroup>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QTransform>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace vss::gui
{
namespace
{

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

QString colour_name(colour_role role)
{
    return Theme::instance().colour(role).name(QColor::HexRgb);
}

} // namespace

SegmentedControl::SegmentedControl(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(32);
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    connect(group_, &QButtonGroup::idToggled, this,
            [this](int id, bool checked)
            {
                if (!checked || current_id_ == id)
                    return;
                current_id_ = id;
                Q_EMIT current_changed(id);
            });
    apply_style();
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { apply_style(); });
}

void SegmentedControl::apply_style()
{
    // A track in the fill colour with the selected segment raised on it as a
    // panel-coloured thumb with a hairline.  The selected segment is never an
    // accent fill: the accent is kept for primary actions and selection.
    const int inset = compact_ ? 2 : 3;
    const int height = compact_ ? 28 : 32;
    const int padding = compact_ ? 10 : 12;
    layout_->setContentsMargins(inset, inset, inset, inset);
    layout_->setSpacing(2);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(
        QStringLiteral(
            "vss--gui--SegmentedControl { background: %1; border: 0; "
            "border-radius: %7px; }"
            "QRadioButton { border: 1px solid transparent; border-radius: "
            "%8px; background: transparent; color: %2; padding: 0 %5px; "
            "min-height: %6px; spacing: 0; }"
            "QRadioButton::indicator { width: 0px; height: 0px; "
            "border: 0; margin: 0; background: transparent; image: none; }"
            "QRadioButton:checked { background: %3; color: %4; "
            "border-color: %9; }"
            "QRadioButton:hover:!checked { color: %4; }")
            .arg(colour_name(colour_role::fillHover),
                 colour_name(colour_role::ink2),
                 colour_name(colour_role::panel), colour_name(colour_role::ink))
            .arg(padding)
            // The 1 px border is drawn inside the button, so the content box
            // is two pixels smaller than the button.
            .arg(height - 2 * inset - 2)
            .arg(Theme::instance().radius(QStringLiteral("control")) + 1)
            .arg(Theme::instance().radius(QStringLiteral("control")) - 1)
            .arg(colour_name(colour_role::line2)));
}

void SegmentedControl::set_compact(bool compact)
{
    if (compact_ == compact)
        return;
    compact_ = compact;
    setFixedHeight(compact ? 28 : 32);
    const auto buttons = group_->buttons();
    for (QAbstractButton* button : buttons)
    {
        button->setFont(Theme::instance().font(compact ? text_style::caption
                                                       : text_style::body));
    }
    apply_style();
}

int SegmentedControl::add_item(const QString& text, int id)
{
    if (id < 0)
        id = next_id_;
    next_id_ = std::max(next_id_, id + 1);

    auto* button = new QRadioButton(text, this);
    button->setAccessibleName(text);
    button->setFont(Theme::instance().font(compact_ ? text_style::caption
                                                    : text_style::body));
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    group_->addButton(button, id);
    layout_->addWidget(button);
    if (current_id_ < 0)
    {
        current_id_ = id;
        button->setChecked(true);
    }
    return id;
}

void SegmentedControl::set_current(int id)
{
    if (id == current_id_ && group_->checkedId() == id)
        return;
    if (QAbstractButton* button = group_->button(id))
    {
        button->setChecked(true);
        if (current_id_ != id)
        {
            current_id_ = id;
            Q_EMIT current_changed(id);
        }
    }
}

int SegmentedControl::current() const
{
    return group_->checkedId();
}

int SegmentedControl::count() const
{
    return static_cast<int>(group_->buttons().size());
}

void SegmentedControl::keyPressEvent(QKeyEvent* event)
{
    const auto buttons = group_->buttons();
    if (buttons.isEmpty())
    {
        QWidget::keyPressEvent(event);
        return;
    }
    int index = 0;
    for (int i = 0; i < buttons.size(); ++i)
    {
        if (buttons.at(i)->isChecked())
        {
            index = i;
            break;
        }
    }
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)
    {
        const int direction = event->key() == Qt::Key_Right ? 1 : -1;
        index = static_cast<int>((index + direction + buttons.size()) %
                                 buttons.size());
        buttons.at(index)->setFocus(Qt::OtherFocusReason);
        set_current(group_->id(buttons.at(index)));
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

PickCard::PickCard(QWidget* parent) : QFrame(parent)
{
    setProperty("pick", true);
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(12);

    radio_ = new QRadioButton(this);
    radio_->setText({});
    layout->addWidget(radio_, 0, Qt::AlignTop);

    auto* column = new QVBoxLayout();
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    title_ = new QLabel(this);
    title_->setFont(semibold(Theme::instance().font(text_style::bodyLarge)));
    title_->setWordWrap(true);
    description_ = new QLabel(this);
    description_->setFont(Theme::instance().font(text_style::body));
    description_->setWordWrap(true);
    meta_ = new QLabel(this);
    meta_->setFont(Theme::instance().font(text_style::caption));
    meta_->setProperty("muted", true);
    meta_->setWordWrap(true);
    meta_->hide();
    column->addWidget(title_);
    column->addWidget(description_);
    column->addWidget(meta_);
    column->addStretch(1);
    layout->addLayout(column, 1);

    trailing_ = new QLabel(this);
    trailing_->setFont(Theme::instance().font(text_style::data));
    trailing_->setProperty("muted", true);
    trailing_->setAttribute(Qt::WA_TransparentForMouseEvents);
    trailing_->setAlignment(Qt::AlignRight | Qt::AlignTop);
    trailing_->hide();
    layout->addWidget(trailing_, 0, Qt::AlignTop);

    connect(radio_, &QRadioButton::toggled, this,
            [this](bool checked)
            {
                refresh();
                if (checked)
                    Q_EMIT selected();
            });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { refresh(); });
    repaint_on_theme_change(this);
}

PickCard::PickCard(const QString& title, const QString& description,
                   QWidget* parent)
    : PickCard(parent)
{
    set_title(title);
    set_description(description);
}

void PickCard::set_title(const QString& title)
{
    title_->setText(title);
    radio_->setAccessibleName(title);
    const QString accessible =
        description_->text().isEmpty()
            ? title
            : tr("%1. %2").arg(title, description_->text());
    setAccessibleName(accessible);
}

void PickCard::set_description(const QString& description)
{
    description_->setText(description);
    set_title(title_->text());
}

void PickCard::set_meta(const QString& meta)
{
    meta_->setText(meta);
    meta_->setVisible(!meta.isEmpty());
}

void PickCard::set_trailing(const QString& trailing)
{
    trailing_->setText(trailing);
    trailing_->setVisible(!trailing.isEmpty());
    updateGeometry();
}

void PickCard::set_selected(bool selected)
{
    radio_->setChecked(selected);
}

bool PickCard::is_selected() const
{
    return radio_->isChecked();
}

void PickCard::refresh()
{
    setProperty("selected", radio_->isChecked());
    polish(this);
    update();
}

void PickCard::mousePressEvent(QMouseEvent* event)
{
    // The release only reaches this card when the press was accepted.  The
    // labels ignore the mouse, so without this a click anywhere but on the
    // small radio circle did nothing.
    if (event->button() == Qt::LeftButton)
    {
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void PickCard::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && !radio_->isChecked())
        radio_->setChecked(true);
    QFrame::mouseReleaseEvent(event);
}

Switch::Switch(const QString& label, QWidget* parent) : QCheckBox(parent)
{
    setFont(Theme::instance().font(text_style::body));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFocusPolicy(Qt::StrongFocus);
    connect(this, &QCheckBox::toggled, this, [this] { refresh_text(); });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { update(); });
    set_label(label);
    refresh_text();
}

void Switch::set_label(const QString& label)
{
    label_ = label;
    setAccessibleName(label_);
    refresh_text();
    updateGeometry();
}

void Switch::refresh_text()
{
    QString text;
    if (!label_.isEmpty())
    {
        text = isChecked() ? QObject::tr("%1 on").arg(label_)
                           : QObject::tr("%1 off").arg(label_);
    }
    QCheckBox::setText(text);
    update();
}

QSize Switch::sizeHint() const
{
    const QFontMetrics metrics(font());
    const int text_width =
        text().isEmpty() ? 0 : metrics.horizontalAdvance(text());
    // The focus ring goes around the whole control, so the track and the
    // label keep the room it needs (its width and gap, the smallest spacing
    // step) from every edge.
    const int room = Theme::instance().space(0);
    return QSize(2 * room + 34 + 8 + text_width,
                 2 * room + std::max(18, metrics.height()));
}

QSize Switch::minimumSizeHint() const
{
    return sizeHint();
}

void Switch::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const Theme& theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int room = theme.space(0);
    const int track_y = (height() - 18) / 2;
    const QRectF track(room + 0.5, track_y + 0.5, 34, 18);
    const QColor on = theme.colour(isEnabled() ? colour_role::accent
                                               : colour_role::disabledText);
    const QColor off = theme.colour(colour_role::trackOff);
    painter.setPen(Qt::NoPen);
    painter.setBrush(isChecked() ? on : off);
    painter.drawRoundedRect(track, 9, 9);

    const qreal knob_x = room + (isChecked() ? 18.0 : 2.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.colour(colour_role::onAccent));
    painter.drawEllipse(QRectF(knob_x, track_y + 2, 14, 14));

    // The ring goes around the whole control, track and label, as it does
    // for a check box.
    if (hasFocus() && FocusRingStyle::keyboard_focus())
    {
        FocusRingStyle::paint_ring(
            painter, QRectF(rect()),
            Theme::instance().radius(QStringLiteral("control")));
    }

    painter.setPen(theme.colour(isEnabled() ? colour_role::ink
                                            : colour_role::disabledText));
    painter.setFont(font());
    const int text_x = room + 34 + 8;
    painter.drawText(QRect(text_x, 0, width() - text_x - room, height()),
                     Qt::AlignLeft | Qt::AlignVCenter, text());
}

bool Switch::hitButton(const QPoint& position) const
{
    return rect().contains(position);
}

Stepper::Stepper(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumHeight(28);
    repaint_on_theme_change(this);
}

void Stepper::set_steps(const QStringList& steps)
{
    steps_ = steps;
    current_ = std::clamp(current_, 0,
                          std::max(0, static_cast<int>(steps_.size()) - 1));
    update_accessible_name();
    updateGeometry();
    update();
}

void Stepper::set_current(int index)
{
    if (steps_.isEmpty())
        return;
    current_ = std::clamp(index, 0, static_cast<int>(steps_.size()) - 1);
    update_accessible_name();
    update();
}

void Stepper::update_accessible_name()
{
    if (steps_.isEmpty())
    {
        setAccessibleName({});
        return;
    }
    const int index =
        std::clamp(current_, 0, static_cast<int>(steps_.size()) - 1);
    setAccessibleName(QObject::tr("Step %1 of %2, %3")
                          .arg(index + 1)
                          .arg(steps_.size())
                          .arg(steps_.at(index)));
}

QSize Stepper::sizeHint() const
{
    const QFontMetrics metrics(Theme::instance().font(text_style::body));
    int width = 0;
    for (int index = 0; index < steps_.size(); ++index)
    {
        width += 20 + 6 + metrics.horizontalAdvance(steps_.at(index));
        if (index + 1 < steps_.size())
            width += 24;
    }
    return QSize(width, 28);
}

QSize Stepper::minimumSizeHint() const
{
    return sizeHint();
}

void Stepper::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const Theme& theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QFont font = Theme::instance().font(text_style::body);
    const QFontMetrics metrics(font);
    const int centre_y = height() / 2;
    int x = 0;

    for (int index = 0; index < steps_.size(); ++index)
    {
        const bool done = index < current_;
        const bool active = index == current_;
        const QRectF circle(x, centre_y - 10, 20, 20);
        if (done || active)
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.colour(colour_role::accent));
            painter.drawEllipse(circle);
        }
        else
        {
            painter.setPen(QPen(theme.colour(colour_role::control), 1.0));
            painter.setBrush(theme.colour(colour_role::panel));
            painter.drawEllipse(circle.adjusted(0.5, 0.5, -0.5, -0.5));
        }

        if (done)
        {
            QPen check(theme.colour(colour_role::onAccent), 2.0);
            check.setCapStyle(Qt::RoundCap);
            painter.setPen(check);
            painter.drawLine(QPointF(x + 5, centre_y),
                             QPointF(x + 9, centre_y + 4));
            painter.drawLine(QPointF(x + 9, centre_y + 4),
                             QPointF(x + 15, centre_y - 4));
        }
        else
        {
            painter.setPen(done || active ? theme.colour(colour_role::onAccent)
                                          : theme.colour(colour_role::ink2));
            painter.setFont(font);
            painter.drawText(circle, Qt::AlignCenter,
                             QString::number(index + 1));
        }

        painter.setPen(done || active ? theme.colour(colour_role::ink)
                                      : theme.colour(colour_role::ink2));
        painter.setFont(font);
        painter.drawText(QRect(x + 26, 0,
                               metrics.horizontalAdvance(steps_.at(index)) + 2,
                               height()),
                         Qt::AlignLeft | Qt::AlignVCenter, steps_.at(index));
        x += 20 + 6 + metrics.horizontalAdvance(steps_.at(index));
        if (index + 1 < steps_.size())
        {
            painter.setPen(QPen(theme.colour(colour_role::line), 1.0));
            painter.drawLine(QPointF(x + 2, centre_y),
                             QPointF(x + 22, centre_y));
            x += 24;
        }
    }
}

Disclosure::Disclosure(const QString& title, QWidget* parent) : QWidget(parent)
{
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(4);

    header_ = new QToolButton(this);
    header_->setCheckable(true);
    header_->setAutoRaise(true);
    header_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header_->setIconSize(QSize(16, 16));
    header_->setFont(Theme::instance().font(text_style::body));
    header_->setAccessibleName(title);
    header_->setText(title);
    layout_->addWidget(header_);

    content_ = new QWidget(this);
    auto* content_layout = new QVBoxLayout(content_);
    content_layout->setContentsMargins(22, 0, 0, 0);
    content_layout->setSpacing(4);
    content_->setMaximumHeight(0);
    content_->setVisible(false);
    layout_->addWidget(content_);

    animation_ = new QPropertyAnimation(content_, "maximumHeight", this);
    animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation_, &QPropertyAnimation::finished, this,
            [this]
            {
                if (content_->maximumHeight() == 0)
                    content_->setVisible(false);
                else
                    content_->setMaximumHeight(QWIDGETSIZE_MAX);
            });
    connect(header_, &QToolButton::clicked, this, [this] { toggle(); });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { apply_state(header_->isChecked(), false); });

    apply_state(false, false);
}

void Disclosure::set_title(const QString& title)
{
    header_->setText(title);
    header_->setAccessibleName(title);
}

void Disclosure::set_content(QWidget* content)
{
    auto* content_layout = qobject_cast<QVBoxLayout*>(content_->layout());
    while (QLayoutItem* item = content_layout->takeAt(0))
    {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    if (content != nullptr)
    {
        content_layout->addWidget(content);
        content->show();
    }
    apply_state(header_->isChecked(), false);
}

void Disclosure::set_expanded(bool expanded)
{
    if (header_->isChecked() == expanded)
        return;
    const QSignalBlocker blocker(header_);
    header_->setChecked(expanded);
    apply_state(expanded, true);
}

bool Disclosure::is_expanded() const
{
    return header_->isChecked();
}

void Disclosure::toggle()
{
    apply_state(header_->isChecked(), true);
    Q_EMIT expanded_changed(header_->isChecked());
}

void Disclosure::apply_state(bool expanded, bool animate)
{
    const int duration = Theme::instance().motion_ms(motion_kind::disclosure);
    animation_->stop();
    if (expanded)
    {
        content_->setVisible(true);
        content_->setMaximumHeight(QWIDGETSIZE_MAX);
        const int target = content_->sizeHint().height();
        if (animate && duration > 0)
        {
            content_->setMaximumHeight(0);
            animation_->setDuration(duration);
            animation_->setStartValue(0);
            animation_->setEndValue(target);
            animation_->start();
        }
        else
        {
            content_->setMaximumHeight(QWIDGETSIZE_MAX);
        }
    }
    else if (animate && duration > 0)
    {
        animation_->setDuration(duration);
        animation_->setStartValue(content_->maximumHeight() == QWIDGETSIZE_MAX
                                      ? content_->height()
                                      : content_->maximumHeight());
        animation_->setEndValue(0);
        animation_->start();
    }
    else
    {
        content_->setMaximumHeight(0);
        content_->setVisible(false);
    }

    // The icon set has the pair: a right chevron (expand) while collapsed and
    // a down chevron once open.
    header_->setIcon(IconProvider::instance().icon(
        expanded ? QStringLiteral("chevron-down") : QStringLiteral("expand"),
        expanded ? colour_role::accentStrong : colour_role::ink2, 16));
    header_->setAccessibleName(expanded
                                   ? tr("%1, expanded").arg(header_->text())
                                   : tr("%1, collapsed").arg(header_->text()));
}

FormRow::FormRow(const QString& label, QWidget* field, const QString& hint,
                 QWidget* parent)
    : QWidget(parent), field_(field)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    label_ = new QLabel(label, this);
    label_->setFont(semibold(Theme::instance().font(text_style::caption)));
    label_->setBuddy(field);
    layout->addWidget(label_);
    layout->addWidget(field);

    hint_ = new QLabel(hint, this);
    hint_->setFont(Theme::instance().font(text_style::caption));
    hint_->setProperty("muted", true);
    hint_->setWordWrap(true);
    hint_->setVisible(!hint.isEmpty());
    layout->addWidget(hint_);

    error_ = new QLabel(this);
    error_->setFont(Theme::instance().font(text_style::caption));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: %1")
                              .arg(Theme::instance()
                                       .colour(colour_role::bad)
                                       .name(QColor::HexRgb)));
    error_->setVisible(false);
    layout->addWidget(error_);

    field_->setAccessibleName(label);
    field_->setFont(Theme::instance().font(text_style::body));
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this]
            {
                label_->setFont(
                    semibold(Theme::instance().font(text_style::caption)));
                hint_->setFont(Theme::instance().font(text_style::caption));
                error_->setFont(Theme::instance().font(text_style::caption));
                field_->setFont(Theme::instance().font(text_style::body));
                error_->setStyleSheet(QStringLiteral("color: %1")
                                          .arg(Theme::instance()
                                                   .colour(colour_role::bad)
                                                   .name(QColor::HexRgb)));
            });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { update(); });
}

void FormRow::set_label(const QString& label)
{
    label_->setText(label);
    field_->setAccessibleName(label);
    label_->setBuddy(field_);
}

void FormRow::set_hint(const QString& hint)
{
    hint_->setText(hint);
    hint_->setVisible(!hint.isEmpty());
}

void FormRow::set_error(const QString& error)
{
    error_->setText(error);
    error_->setVisible(!error.isEmpty());
    field_->setProperty("invalid", !error.isEmpty());
    polish(field_);
    field_->setAccessibleDescription(error);
    if (!error.isEmpty())
        announce_name_change(field_);
}

void FormRow::clear_error()
{
    set_error({});
}

Toast::Toast(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("Toast"));
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_StyledBackground);
    setFocusPolicy(Qt::NoFocus);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 10, 10, 10);
    layout->setSpacing(12);

    label_ = new QLabel(this);
    label_->setFont(Theme::instance().font(text_style::body));
    label_->setWordWrap(true);
    layout->addWidget(label_, 1);

    action_ = new QPushButton(this);
    action_->setFont(Theme::instance().font(text_style::body));
    action_->setCursor(Qt::PointingHandCursor);
    action_->hide();
    layout->addWidget(action_, 0, Qt::AlignVCenter);

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(6000);
    connect(timer_, &QTimer::timeout, this, &Toast::dismiss);
    connect(action_, &QPushButton::clicked, this,
            [this]
            {
                if (callback_)
                    callback_();
                dismiss();
            });
}

void Toast::set_message(const QString& text, const QString& action_text,
                        std::function<void()> action)
{
    label_->setText(text);
    setAccessibleName(text);
    callback_ = std::move(action);
    action_->setVisible(!action_text.isEmpty());
    action_->setText(action_text);
    if (!action_text.isEmpty())
    {
        action_->setAccessibleName(action_text);
        action_->setStyleSheet(
            QStringLiteral("QPushButton { background: transparent; border: 0; "
                           "color: %1; padding: 0 4px; }"
                           "QPushButton:hover { color: %2; }")
                .arg(Theme::instance()
                         .colour(colour_role::toastLink)
                         .name(QColor::HexRgb),
                     Theme::instance()
                         .colour(colour_role::toastText)
                         .name(QColor::HexRgb)));
    }
    adjustSize();
    restart_timer();
}

void Toast::dismiss()
{
    hide();
    deleteLater();
}

void Toast::enterEvent(QEnterEvent* event)
{
    timer_->stop();
    QFrame::enterEvent(event);
}

void Toast::leaveEvent(QEvent* event)
{
    restart_timer();
    QFrame::leaveEvent(event);
}

void Toast::restart_timer()
{
    timer_->start();
}

ToastManager& ToastManager::instance()
{
    static ToastManager manager;
    return manager;
}

ToastManager::ToastManager(QObject* parent) : QObject(parent) {}

void ToastManager::show(QWidget* anchor, const QString& text,
                        const QString& action_text,
                        std::function<void()> action)
{
    if (anchor == nullptr)
        return;
    QWidget* window = anchor->window();
    if (window == nullptr)
        return;

    auto* toast = new Toast(window);
    toasts_.append(toast);
    connect(toast, &QObject::destroyed, this,
            [this, toast]
            {
                for (int index = 0; index < toasts_.size(); ++index)
                {
                    if (toasts_.at(index).data()
                        == static_cast<QWidget*>(toast))
                    {
                        toasts_.removeAt(index);
                        break;
                    }
                }
            });
    toast->set_message(text, action_text, std::move(action));
    toast->show();
    toast->raise();
    position_toasts(window);
}

void ToastManager::position_toasts(QWidget* window)
{
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [](const QPointer<QWidget>& toast)
                                 { return toast.isNull(); }),
                  toasts_.end());

    int bottom = window->height() - 24;
    for (int index = static_cast<int>(toasts_.size()) - 1; index >= 0; --index)
    {
        auto* toast = qobject_cast<Toast*>(toasts_.at(index).data());
        if (toast == nullptr || !toast->isVisible())
            continue;
        toast->adjustSize();
        const int x = (window->width() - toast->width()) / 2;
        const int y = bottom - toast->height();
        toast->move(std::max(0, x), std::max(0, y));
        bottom = y - 8;
    }
}

} // namespace vss::gui
