//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QAbstractButton>
#include <QFrame>
#include <QLabel>
#include <QString>

class QHBoxLayout;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace vss::gui
{

/// The header of a pane: a title, an optional count badge, and the pane's own
/// actions on the right.  It replaces a dock title bar, so it has no float or
/// close buttons.  See `docs/10-design-language.md`, rule 2.
class PaneHeader final : public QFrame
{
    Q_OBJECT

public:
    explicit PaneHeader(const QString& title, QWidget* parent = nullptr);

    void set_title(const QString& title);
    QString title() const;

    /// Shows the count badge next to the title; a negative count hides it.
    void set_count(int count);

    /// Adds a widget to the right-hand action area, after any added before.
    void add_action(QWidget* widget);

    /// Adds a widget straight after the title and count, on the left.
    void add_leading(QWidget* widget);

    /// Puts `widget` after the title and count and gives it all the spare
    /// width, or removes the fill widget when `widget` is null.  The caller
    /// owns the widget and re-parents it when it takes it out.
    void set_fill(QWidget* widget);

    /// Width taken by the margins, title and count badge.
    int lead_width() const;

    QLabel* title_label() const noexcept { return title_; }

private:
    void apply_theme();

    QHBoxLayout* layout_ = nullptr;
    QWidget* fill_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* count_ = nullptr;
    int leading_index_ = 2;
};

/// A rounded pane surface on the window ground, with a header and a body.
class Pane final : public QFrame
{
    Q_OBJECT

public:
    explicit Pane(const QString& title, QWidget* parent = nullptr);

    PaneHeader* header() const noexcept { return header_; }

    /// Puts `body` under the header.  The pane takes ownership.
    void set_body(QWidget* body);
    QWidget* body() const noexcept { return body_; }

private:
    QVBoxLayout* layout_ = nullptr;
    PaneHeader* header_ = nullptr;
    QWidget* body_ = nullptr;
};

/// The tinted square that starts a port or device row and shows its kind.
class KindTile final : public QFrame
{
    Q_OBJECT

public:
    enum class tint
    {
        accent,
        violet,
        ok,
        neutral
    };

    KindTile(const QString& icon, tint value, QWidget* parent = nullptr);

    void set_icon(const QString& icon);
    void set_tint(tint value);
    tint current_tint() const noexcept { return tint_; }

private:
    void refresh();

    QLabel* glyph_ = nullptr;
    QString icon_;
    tint tint_ = tint::accent;
};

/// A large choice or job button: a kind tile, a title and a short
/// description on a rounded card.  It is a real button (Space and Enter
/// press it), may be checkable for a pick list, and shows hover, pressed,
/// checked and keyboard focus states.
class ActionTile final : public QAbstractButton
{
    Q_OBJECT

public:
    ActionTile(const QString& icon, KindTile::tint tint, const QString& title,
               const QString& description, QWidget* parent = nullptr);

    void set_description(const QString& description);
    QString description() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    KindTile* tile_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* description_ = nullptr;
    bool hovered_ = false;
};

/// A small capitals label that sits above a grouped section.
class GroupLabel final : public QLabel
{
    Q_OBJECT

public:
    explicit GroupLabel(const QString& text, QWidget* parent = nullptr);
};

/// The 1 px `line` rule between two rows of a grouped section.
QFrame* make_group_divider(QWidget* parent);

/// A sunken, rounded section that holds related rows with hairline dividers
/// between them.  See `docs/10-design-language.md`, rule 10.
class Group final : public QFrame
{
    Q_OBJECT

public:
    explicit Group(QWidget* parent = nullptr);

    /// Adds a row, with a divider above it if it is not the first.
    void add_row(QWidget* row);

    /// Adds a label and value row: the label in the caption ink, the value
    /// filling the rest.  Returns the row.
    QWidget* add_field(const QString& label, QWidget* value);

    QVBoxLayout* rows() const noexcept { return layout_; }

private:
    QVBoxLayout* layout_ = nullptr;
    int count_ = 0;
};

/// The frame of a dialog or wizard page: a title, a one-line subtitle, a
/// scrolling body and a right-aligned footer.  See rule 11.
class DialogFrame final : public QWidget
{
    Q_OBJECT

public:
    explicit DialogFrame(QWidget* parent = nullptr);

    void set_title(const QString& title);
    void set_subtitle(const QString& subtitle);

    /// The body layout.  It scrolls when the dialog is too short for it.
    QVBoxLayout* body() const noexcept { return body_; }

    /// Adds a footer button.  Add the primary button last so it sits on the
    /// far right.
    void add_footer_button(QPushButton* button);

    /// Adds a widget on the left of the footer, such as a Help button.
    void add_footer_leading(QWidget* widget);

    QLabel* title_label() const noexcept { return title_; }
    QLabel* subtitle_label() const noexcept { return subtitle_; }

private:
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QVBoxLayout* body_ = nullptr;
    QHBoxLayout* footer_ = nullptr;
    int leading_ = 0;
};

} // namespace vss::gui
