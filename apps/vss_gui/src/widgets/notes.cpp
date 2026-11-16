//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "widgets/notes.hpp"

#include "theme/focus_ring_style.hpp"
#include "theme/icon_provider.hpp"

#include <QClipboard>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QSizePolicy>
#include <QToolTip>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

QString icon_for(NoteBanner::kind kind)
{
    switch (kind)
    {
    case NoteBanner::kind::info:
        return QStringLiteral("info");
    case NoteBanner::kind::ok:
        return QStringLiteral("done");
    case NoteBanner::kind::warn:
        return QStringLiteral("warning");
    case NoteBanner::kind::danger:
        return QStringLiteral("fault");
    }
    return QStringLiteral("info");
}

colour_role colour_for(NoteBanner::kind kind)
{
    switch (kind)
    {
    case NoteBanner::kind::info:
        return colour_role::accentStrong;
    case NoteBanner::kind::ok:
        return colour_role::ok;
    case NoteBanner::kind::warn:
        return colour_role::amber;
    case NoteBanner::kind::danger:
        return colour_role::bad;
    }
    return colour_role::ink;
}

QString property_for(NoteBanner::kind kind)
{
    switch (kind)
    {
    case NoteBanner::kind::info:
        return QStringLiteral("info");
    case NoteBanner::kind::ok:
        return QStringLiteral("ok");
    case NoteBanner::kind::warn:
        return QStringLiteral("warn");
    case NoteBanner::kind::danger:
        return QStringLiteral("danger");
    }
    return QStringLiteral("info");
}

} // namespace

CopyButton::CopyButton(QWidget* parent) : QAbstractButton(parent)
{
    setFixedSize(24, 24);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Copy"));
    connect(this, &QAbstractButton::clicked, this, &CopyButton::do_copy);
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { update(); });
}

QSize CopyButton::sizeHint() const
{
    return QSize(24, 24);
}

QSize CopyButton::minimumSizeHint() const
{
    return QSize(24, 24);
}

void CopyButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    const Theme& theme = Theme::instance();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QColor background = Qt::transparent;
    if (isDown())
        background = theme.colour(colour_role::accentSoft);
    else if (underMouse())
        background = theme.colour(colour_role::sunken);
    if (background.alpha() > 0)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }

    if (hasFocus() && FocusRingStyle::keyboard_focus())
    {
        // The hover fill is inset 1 px with a radius of 4, so its outer
        // radius is 5.
        FocusRingStyle::paint_ring(painter, QRectF(rect()), 5);
    }

    const QPixmap glyph =
        IconProvider::instance()
            .icon(QStringLiteral("copy"), colour_role::ink, 16)
            .pixmap(16, 16);
    painter.drawPixmap((width() - 16) / 2, (height() - 16) / 2, glyph);
}

void CopyButton::enterEvent(QEnterEvent* event)
{
    update();
    QAbstractButton::enterEvent(event);
}

void CopyButton::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        click();
        event->accept();
        return;
    }
    QAbstractButton::keyPressEvent(event);
}

void CopyButton::leaveEvent(QEvent* event)
{
    update();
    QAbstractButton::leaveEvent(event);
}

void CopyButton::set_copy_text(const QString& text, const QString& thing)
{
    text_ = text;
    thing_ = thing;
    const QString label =
        thing.isEmpty() ? tr("Copy") : tr("Copy %1").arg(thing);
    setAccessibleName(label);
    setToolTip(label);
}

void CopyButton::do_copy()
{
    QGuiApplication::clipboard()->setText(text_);
    setToolTip(tr("Copied"));
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, height())), tr("Copied"),
                       this);
    Q_EMIT copied();
}

CodeBlock::CodeBlock(QWidget* parent) : QFrame(parent)
{
    setProperty("code", true);
    setFrameShape(QFrame::NoFrame);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 6, 6, 6);
    layout->setSpacing(8);

    label_ = new QLabel(this);
    label_->setFont(Theme::instance().font(text_style::data));
    label_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                    Qt::TextSelectableByKeyboard);
    label_->setWordWrap(true);
    label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(label_, 1);

    copy_ = new CopyButton(this);
    layout->addWidget(copy_, 0, Qt::AlignTop);
    connect(copy_, &CopyButton::copied, this, &CodeBlock::copied);
    repaint_on_theme_change(this);
}

void CodeBlock::set_text(const QString& text, const QString& access_name)
{
    access_name_ = access_name;
    setAccessibleName(access_name);
    label_->setText(text);
    label_->setAccessibleName(access_name);
    copy_->set_copy_text(text, access_name);
}

void CodeBlock::set_text(const QString& text)
{
    set_text(text, access_name_);
}

QString CodeBlock::text() const
{
    return label_->text();
}

NoteBanner::NoteBanner(QWidget* parent) : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(8);

    icon_ = new QLabel(this);
    icon_->setFixedSize(16, 16);
    icon_->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(icon_, 0, Qt::AlignTop);

    text_ = new QLabel(this);
    text_->setWordWrap(true);
    text_->setFont(Theme::instance().font(text_style::body));
    text_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(text_, 1);

    apply_kind();
    repaint_on_theme_change(this);
}

NoteBanner::NoteBanner(kind value, const QString& text, QWidget* parent)
    : NoteBanner(parent)
{
    set_kind(value);
    set_text(text);
}

void NoteBanner::set_kind(kind value)
{
    kind_ = value;
    apply_kind();
}

void NoteBanner::set_text(const QString& text)
{
    accessible_text_.clear();
    text_->setText(text);
    refresh_accessible_name();
}

void NoteBanner::set_text(const QString& text, const QString& accessible_text)
{
    accessible_text_ = accessible_text;
    text_->setText(text);
    refresh_accessible_name();
}

void NoteBanner::set_text_format(Qt::TextFormat format)
{
    text_->setTextFormat(format);
}

QString NoteBanner::text() const
{
    return text_->text();
}

void NoteBanner::set_action(const QString& text, std::function<void()> callback)
{
    if (action_ == nullptr)
    {
        action_ = new QPushButton(this);
        set_role(action_, "quiet");
        action_->setProperty("size", QStringLiteral("small"));
        polish(action_);
        action_->setCursor(Qt::PointingHandCursor);
        auto* layout = qobject_cast<QHBoxLayout*>(this->layout());
        layout->addWidget(action_, 0, Qt::AlignVCenter);
        connect(action_, &QPushButton::clicked, this,
                &NoteBanner::action_triggered);
    }
    action_->setText(text);
    action_->setAccessibleName(text);
    action_->show();
    if (callback)
    {
        connect(action_, &QPushButton::clicked, this,
                [callback = std::move(callback)] { callback(); });
    }
}

void NoteBanner::clear_action()
{
    if (action_ != nullptr)
        action_->hide();
}

void NoteBanner::refresh_accessible_name()
{
    const QString plain =
        accessible_text_.isEmpty() ? text_->text() : accessible_text_;
    if (kind_ == kind::warn)
        setAccessibleName(tr("Warning: %1").arg(plain));
    else if (kind_ == kind::danger)
        setAccessibleName(tr("Error: %1").arg(plain));
    else
        setAccessibleName(plain);
    announce_name_change(this);
}

void NoteBanner::apply_kind()
{
    setProperty("note", property_for(kind_));
    polish(this);
    icon_->setPixmap(IconProvider::instance()
                         .icon(icon_for(kind_), colour_for(kind_), 16)
                         .pixmap(16, 16));
    refresh_accessible_name();
}

ResultSentence::ResultSentence(QWidget* parent)
    : NoteBanner(kind::info, {}, parent)
{
}

void ResultSentence::set_result_text(const QString& text)
{
    set_text(text);
    announce_name_change(this);
}

void ResultSentence::set_result_text(const QString& text,
                                     const QString& accessible_text)
{
    set_text(text, accessible_text);
    announce_name_change(this);
}

} // namespace vss::gui
