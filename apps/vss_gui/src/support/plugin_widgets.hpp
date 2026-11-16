//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small widgets the Plugins page and its dialogs share. Every label here shows
// plain text: nothing a plugin says is ever read as markup.
//

#pragma once

#include "theme/theme.hpp"

#include <QLabel>
#include <QLayout>
#include <QList>
#include <QString>

class QBoxLayout;
class QCheckBox;
class QFrame;
class QWidget;

namespace vss::gui
{
class NoteBanner;
}

namespace vss::gui::support
{

/// A wrapping, plain-text label in the given text style. `plugin_text` marks
/// text that came from a plugin: it is cleaned and made breakable.
QLabel* make_plain_label(const QString& text, QWidget* parent,
                         text_style style = text_style::body,
                         bool plugin_text = false);
QLabel* make_muted_label(const QString& text, QWidget* parent,
                         text_style style = text_style::body,
                         bool plugin_text = false);
/// A fixed-font, selectable, plain-text label that never wraps.
QLabel* make_fixed_label(const QString& text, QWidget* parent,
                         bool large = false);

/// One line of plain text that is cut with "..." at the end when it does not
/// fit, with the whole text as its tooltip and accessible name.
class ElidedLabel final : public QLabel
{
public:
    explicit ElidedLabel(QWidget* parent = nullptr);
    void set_full_text(const QString& text);
    QString full_text() const { return full_; }

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void elide();

    QString full_;
};

/// Lays its items out from left to right and starts a new line when the next
/// one does not fit, so a row of buttons never makes a dialog wider than its
/// window at large text.
class FlowLayout final : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent, int horizontal_spacing = 8,
                        int vertical_spacing = 4);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

private:
    int lay_out(const QRect& rect, bool test_only) const;

    QList<QLayoutItem*> items_;
    int horizontal_ = 8;
    int vertical_ = 4;
};

/// A NoteBanner that shows plain text only.
NoteBanner* make_plain_note(QWidget* parent);

/// A row in a permission list: a mark, the sentence, and the muted second
/// line or word. Amber rows are drawn with the warning icon.
QFrame* make_permission_row(const QString& sentence, bool amber, bool allowed,
                            const QString& state_word, const QString& reason,
                            QWidget* parent);

/// `text` in groups of four, as a person reads a fingerprint.
QString grouped_fingerprint(const QString& hex);
/// The fingerprint a screen reader reads one character at a time, in groups.
QString spoken_fingerprint(const QString& hex);

/// Tells assistive technology about a change, politely, once.
void announce_polite(QWidget* widget, const QString& text);

} // namespace vss::gui::support
