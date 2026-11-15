//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/plugin_widgets.hpp"

#include "support/plugin_text.hpp"
#include "theme/icon_provider.hpp"
#include "theme/scaled_font.hpp"
#include "widgets/common.hpp"
#include "widgets/notes.hpp"

#include <QAccessible>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace vss::gui::support
{

QLabel* make_plain_label(const QString& text, QWidget* parent, text_style style,
                         bool plugin_text)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setText(plugin_text ? wrappable_text(text) : text);
    label->setFont(Theme::instance().font(style));
    label->setWordWrap(true);
    return label;
}

QLabel* make_muted_label(const QString& text, QWidget* parent, text_style style,
                         bool plugin_text)
{
    QLabel* label = make_plain_label(text, parent, style, plugin_text);
    label->setProperty("muted", true);
    polish(label);
    return label;
}

QLabel* make_fixed_label(const QString& text, QWidget* parent, bool large)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setText(text);
    const QFont font = Theme::instance().font(text_style::data);
    label->setFont(large ? scaled_font(font, 1.5) : font);
    label->setWordWrap(false);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

// -- ElidedLabel --------------------------------------------------------------

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent)
{
    setTextFormat(Qt::PlainText);
    setWordWrap(false);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ElidedLabel::set_full_text(const QString& text)
{
    full_ = plain_text(text);
    setAccessibleName(full_);
    setToolTip(full_);
    elide();
}

QSize ElidedLabel::minimumSizeHint() const
{
    return QSize(fontMetrics().horizontalAdvance(QChar(0x2026)),
                 fontMetrics().height());
}

QSize ElidedLabel::sizeHint() const
{
    return QSize(fontMetrics().horizontalAdvance(full_),
                 fontMetrics().height());
}

void ElidedLabel::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    elide();
}

void ElidedLabel::changeEvent(QEvent* event)
{
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange)
    {
        updateGeometry();
        elide();
    }
}

void ElidedLabel::elide()
{
    const QString shown =
        fontMetrics().elidedText(full_, Qt::ElideRight, qMax(1, width()));
    if (shown != text())
        QLabel::setText(shown);
}

// -- FlowLayout ---------------------------------------------------------------

FlowLayout::FlowLayout(QWidget* parent, int horizontal_spacing,
                       int vertical_spacing)
    : QLayout(parent), horizontal_(horizontal_spacing),
      vertical_(vertical_spacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem* item)
{
    items_.append(item);
}

int FlowLayout::count() const
{
    return static_cast<int>(items_.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const
{
    return index >= 0 && index < count() ? items_.at(index) : nullptr;
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    return index >= 0 && index < count() ? items_.takeAt(index) : nullptr;
}

int FlowLayout::heightForWidth(int width) const
{
    return lay_out(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    lay_out(rect, false);
}

QSize FlowLayout::sizeHint() const
{
    return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem* item : items_)
        size = size.expandedTo(item->minimumSize());
    const QMargins margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(),
                        margins.top() + margins.bottom());
}

int FlowLayout::lay_out(const QRect& rect, bool test_only) const
{
    const QMargins margins = contentsMargins();
    const QRect inside = rect.adjusted(margins.left(), margins.top(),
                                       -margins.right(), -margins.bottom());
    int x = inside.x();
    int y = inside.y();
    int line_height = 0;
    for (QLayoutItem* item : items_)
    {
        const QSize hint = item->sizeHint();
        if (x + hint.width() > inside.right() + 1 && line_height > 0)
        {
            x = inside.x();
            y += line_height + vertical_;
            line_height = 0;
        }
        if (!test_only)
            item->setGeometry(QRect(QPoint(x, y), hint));
        x += hint.width() + horizontal_;
        line_height = qMax(line_height, hint.height());
    }
    return y + line_height - rect.y() + margins.bottom();
}

// -- other pieces -------------------------------------------------------------

NoteBanner* make_plain_note(QWidget* parent)
{
    auto* note = new NoteBanner(parent);
    note->set_text_format(Qt::PlainText);
    return note;
}

QFrame* make_permission_row(const QString& sentence, bool amber, bool allowed,
                            const QString& state_word, const QString& reason,
                            QWidget* parent)
{
    auto* row = new QFrame(parent);
    if (amber)
    {
        row->setProperty("note", QStringLiteral("warn"));
        polish(row);
    }
    auto* outer = new QVBoxLayout(row);
    outer->setContentsMargins(8, 6, 8, 6);
    outer->setSpacing(2);
    auto* line = new QHBoxLayout();
    line->setSpacing(8);

    auto* mark = new QLabel(row);
    mark->setFixedSize(16, 16);
    const QString icon =
        amber ? QStringLiteral("warning")
              : (allowed ? QStringLiteral("done") : QStringLiteral("minus"));
    mark->setPixmap(
        IconProvider::instance()
            .icon(icon, amber ? colour_role::noteWarnText : colour_role::ink2)
            .pixmap(16, 16));
    mark->setAttribute(Qt::WA_TransparentForMouseEvents);
    line->addWidget(mark, 0, Qt::AlignTop);

    QLabel* text = make_plain_label(sentence, row);
    line->addWidget(text, 1);
    if (!state_word.isEmpty())
    {
        QLabel* word = make_muted_label(state_word, row, text_style::caption);
        line->addWidget(word, 0, Qt::AlignTop);
    }
    outer->addLayout(line);
    if (!reason.isEmpty())
    {
        QLabel* why = make_muted_label(reason, row, text_style::caption, true);
        why->setContentsMargins(24, 0, 0, 0);
        outer->addWidget(why);
    }

    // A screen reader reads the whole row: the sentence, then its state.
    QString name = sentence;
    if (!state_word.isEmpty())
        name = QStringLiteral("%1, %2").arg(sentence, state_word);
    row->setAccessibleName(name);
    if (!reason.isEmpty())
        row->setAccessibleDescription(reason);
    row->setFocusPolicy(Qt::TabFocus);
    return row;
}

QString grouped_fingerprint(const QString& hex)
{
    QStringList groups;
    for (qsizetype index = 0; index < hex.size(); index += 4)
        groups << hex.mid(index, 4);
    return groups.join(QLatin1Char(' '));
}

QString spoken_fingerprint(const QString& hex)
{
    QStringList groups;
    for (qsizetype index = 0; index < hex.size(); index += 4)
    {
        QStringList letters;
        for (const QChar letter : hex.mid(index, 4))
            letters << QString(letter);
        groups << letters.join(QLatin1Char(' '));
    }
    return groups.join(QStringLiteral(", "));
}

void announce_polite(QWidget* widget, const QString& text)
{
    if (widget == nullptr || text.isEmpty())
        return;
    QAccessibleAnnouncementEvent event(widget, text);
    event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&event);
}

} // namespace vss::gui::support
