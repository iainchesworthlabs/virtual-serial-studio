//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_CHECK_ROW_HPP
#define VSS_GUI_SUPPORT_CHECK_ROW_HPP

#include <QFrame>
#include <QString>

class QGridLayout;
class QLabel;
class QPushButton;

namespace vss::gui::support
{

/// The icon circle at the left of a setup or troubleshooting row.
enum class row_tone
{
    ok,
    warn,
    info,
    neutral
};

/// One icon, title, optional chip, one sentence and an optional action.
///
/// Used by the setup check (framed rows) and by "Why is nothing arriving"
/// (rows inside one card, separated by a line).  The amber highlight is the
/// same visual for both.
class CheckRowWidget final : public QFrame
{
    Q_OBJECT

public:
    struct spec
    {
        row_tone tone = row_tone::neutral;
        QString icon;
        QString title;
        QString chip;
        QString sentence;
        QString action_text;
        bool primary_action = false;
        bool highlight = false;
        /// False when the parent card already supplies the frame.
        bool framed = true;
    };

    explicit CheckRowWidget(const spec& row, QWidget* parent = nullptr);

    QPushButton* action_button() const noexcept { return action_; }
    QString title_text() const;
    /// The width this row's chip needs; 0 when it has none.
    int chip_width() const;
    /// Reserves the right-hand status column so chips of different rows share
    /// one right edge.  Rows without a chip reserve it too.
    void set_status_column_width(int width);

Q_SIGNALS:
    void action_triggered();

private:
    void apply_style();

    spec row_;
    QGridLayout* layout_ = nullptr;
    QWidget* chip_ = nullptr;
    QLabel* icon_holder_ = nullptr;
    QLabel* glyph_ = nullptr;
    QLabel* title_ = nullptr;
    QPushButton* action_ = nullptr;
};

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_CHECK_ROW_HPP
