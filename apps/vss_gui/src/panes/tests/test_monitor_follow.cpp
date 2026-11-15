//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Monitor's Follow: the view follows new rows only while Follow is ticked,
// the user's own scrolling and selecting untick it, going back to the end
// ticks it, and with it unticked the view stays on the rows the user is
// looking at, also when the history is full and the oldest rows leave. Every
// step goes through the events a person's mouse, wheel and keyboard make.
//

#include "test_framework.hpp"

#include "qt_input.hpp"

#include "backend/fake_studio_backend.hpp"
#include "panes/monitor_model.hpp"
#include "panes/monitor_panel.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QTableView>
#include <QWheelEvent>

#include <algorithm>

namespace
{

/// A Monitor panel that numbers the rows it is given, so a row can be told
/// from any other by its Preview.
struct follow_fixture
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::MonitorPanel panel{backend};
    vss::endpoint_id port = 0;
    int next = 0;

    follow_fixture()
    {
        const auto id = backend.create_virtual_port(
            QStringLiteral("device"), vss::virtual_port_kind::pty, true);
        REQUIRE(id.has_value());
        port = *id;
        panel.set_commit_interval(0);
        panel.set_advanced(true);
        panel.set_show_advanced_tabs(false);
        panel.resize(900, 400);
        panel.show();
        pump();
    }

    static void pump()
    {
        QApplication::processEvents();
        QApplication::processEvents();
    }

    QTableView* table() { return panel.table(); }

    /// Gives the panel `rows` new rows, as one batch of traffic.
    void feed(int rows)
    {
        vss::gui::event_batch events;
        for (int index = 0; index < rows; ++index)
        {
            vss::gui::gui_event event;
            event.kind = vss::event_kind::data;
            event.endpoint = port;
            event.name = QStringLiteral("device");
            event.direction = vss::data_direction::rx;
            event.data = vss::gui::event_payload(
                QByteArray::number(next++).rightJustified(8, '0'));
            events.push_back(event);
        }
        panel.handle_events(events);
        pump();
    }

    /// Fills the history to its limit, and one batch beyond so rows have
    /// begun to leave.
    void fill_history()
    {
        while (next < vss::gui::MonitorModel::kMaxRows)
            feed(500);
        feed(100);
        REQUIRE_EQ(table()->model()->rowCount(),
                   vss::gui::MonitorModel::kMaxRows);
    }

    /// The two Follow boxes, the one in the Simple bar and the one in the
    /// Advanced bar.
    QList<QCheckBox*> follow_boxes()
    {
        QList<QCheckBox*> boxes;
        for (QCheckBox* box : panel.findChildren<QCheckBox*>())
        {
            if (box->text() == QStringLiteral("Follow"))
                boxes.push_back(box);
        }
        REQUIRE_EQ(boxes.size(), 2);
        return boxes;
    }

    /// The box a person sees: the Advanced bar's.
    QCheckBox* visible_follow()
    {
        for (QCheckBox* box : follow_boxes())
        {
            if (box->isVisibleTo(&panel))
                return box;
        }
        REQUIRE(false);
        return nullptr;
    }

    /// True when both boxes show Follow as `on`.
    bool follow_is(bool on)
    {
        for (QCheckBox* box : follow_boxes())
        {
            if (box->isChecked() != on)
                return false;
        }
        return true;
    }

    QString preview(int row)
    {
        return table()->model()->index(row, 4).data().toString();
    }

    QString first_visible()
    {
        const int row = table()->rowAt(0);
        REQUIRE(row >= 0);
        return preview(row);
    }

    bool at_end()
    {
        return table()->verticalScrollBar()->value() ==
               table()->verticalScrollBar()->maximum();
    }

    /// Where the row is in the viewport.
    int row_top(int row) { return table()->rowViewportPosition(row); }

    /// Shows the row at the top, the way a script would and not a person: it
    /// is not something Follow is told about.
    void show_at_top(int row)
    {
        table()->scrollTo(table()->model()->index(row, 4),
                          QAbstractItemView::PositionAtTop);
        pump();
    }

    /// A click on the row, with the mouse.
    void click_row(int row)
    {
        vss_test::click_at(
            table()->viewport(),
            QPoint(8, row_top(row) + table()->rowHeight(row) / 2));
        pump();
    }

    /// One notch of the wheel, up or down.
    void wheel(int notches)
    {
        const QPoint at(20, 40);
        QWheelEvent event(QPointF(at),
                          QPointF(table()->viewport()->mapToGlobal(at)),
                          QPoint(), QPoint(0, 120 * notches), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(table()->viewport(), &event);
        pump();
    }

    void key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QKeyEvent press(QEvent::KeyPress, key, modifiers);
        QApplication::sendEvent(table(), &press);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers);
        QApplication::sendEvent(table(), &release);
        pump();
    }
};

/// What a person is looking at: the first row shown, and the selected row and
/// where it is on the screen.
struct view_state
{
    QString first;
    QString selected;
    int selected_top = 0;

    bool operator==(const view_state&) const = default;
};

view_state state_of(follow_fixture& fixture)
{
    // The view lays its rows out when the event loop is idle; make it do so
    // now, so what is read is what a person would see.
    fixture.table()->doItemsLayout();
    view_state state;
    state.first = fixture.first_visible();
    const QModelIndex current = fixture.table()->currentIndex();
    if (current.isValid())
    {
        state.selected = fixture.preview(current.row());
        state.selected_top = fixture.row_top(current.row());
    }
    return state;
}

std::string describe(const view_state& state)
{
    return state.first.toStdString() + " / " + state.selected.toStdString() +
           " at " + std::to_string(state.selected_top);
}

/// Feeds `batches` batches of `rows` rows and requires the view to be exactly
/// as it was.
void require_view_stays(follow_fixture& fixture, const view_state& before,
                        int batches, int rows)
{
    for (int batch = 0; batch < batches; ++batch)
    {
        fixture.feed(rows);
        const view_state now = state_of(fixture);
        if (!(now == before))
            throw vss_test::failure("the view moved: was " + describe(before) +
                                    ", is " + describe(now) + " after batch " +
                                    std::to_string(batch + 1));
    }
}

} // namespace

TEST_CASE("monitor follow: selecting a row keeps the view where it is")
{
    follow_fixture fixture;
    fixture.feed(400);
    REQUIRE(fixture.follow_is(true));
    REQUIRE(fixture.at_end());

    fixture.show_at_top(150);
    fixture.click_row(fixture.table()->rowAt(0) + 4);
    // The click was the user moving away from the end.
    REQUIRE(fixture.follow_is(false));
    const view_state before = state_of(fixture);
    REQUIRE(!before.selected.isEmpty());

    require_view_stays(fixture, before, 5, 50);
    REQUIRE(fixture.follow_is(false));
    REQUIRE(!fixture.at_end());
}

TEST_CASE("monitor follow: the wheel up stops the view following")
{
    follow_fixture fixture;
    fixture.feed(400);
    REQUIRE(fixture.at_end());
    fixture.wheel(1);
    REQUIRE(fixture.follow_is(false));
    REQUIRE(!fixture.at_end());
    const view_state before = state_of(fixture);

    require_view_stays(fixture, before, 5, 50);
    REQUIRE(fixture.follow_is(false));
}

TEST_CASE("monitor follow: the wheel back down to the end follows again")
{
    follow_fixture fixture;
    fixture.feed(400);
    fixture.wheel(1);
    REQUIRE(fixture.follow_is(false));
    for (int notch = 0; notch < 4 && !fixture.at_end(); ++notch)
        fixture.wheel(-1);
    REQUIRE(fixture.at_end());
    REQUIRE(fixture.follow_is(true));
    fixture.feed(50);
    REQUIRE(fixture.at_end());
}

TEST_CASE(
    "monitor follow: the scroll bar and the keys leave the end and return")
{
    follow_fixture fixture;
    fixture.feed(400);
    QScrollBar* bar = fixture.table()->verticalScrollBar();

    // A page up with the scroll bar's own action, as a click above the handle
    // makes it.
    bar->triggerAction(QAbstractSlider::SliderPageStepSub);
    fixture.pump();
    REQUIRE(fixture.follow_is(false));
    bar->triggerAction(QAbstractSlider::SliderToMaximum);
    fixture.pump();
    REQUIRE(fixture.follow_is(true));

    for (const int key : {Qt::Key_Up, Qt::Key_PageUp, Qt::Key_Home})
    {
        fixture.table()->setCurrentIndex(
            fixture.table()->model()->index(399, 4));
        fixture.key(Qt::Key_End);
        REQUIRE(fixture.follow_is(true));
        fixture.key(key);
        REQUIRE(fixture.follow_is(false));
    }
    // Selecting a row that is not the last, from the keyboard.
    fixture.key(Qt::Key_End, Qt::ControlModifier);
    REQUIRE(fixture.follow_is(true));
    fixture.key(Qt::Key_Up);
    REQUIRE(fixture.follow_is(false));
}

TEST_CASE("monitor follow: Ctrl+End follows the newest rows again")
{
    follow_fixture fixture;
    fixture.feed(400);
    fixture.wheel(3);
    REQUIRE(fixture.follow_is(false));
    fixture.feed(50);
    REQUIRE(!fixture.at_end());

    fixture.key(Qt::Key_End, Qt::ControlModifier);
    REQUIRE(fixture.follow_is(true));
    REQUIRE(fixture.at_end());
    fixture.feed(50);
    REQUIRE(fixture.at_end());
    REQUIRE_EQ(
        fixture.table()->rowAt(fixture.table()->viewport()->height() - 1),
        fixture.table()->model()->rowCount() - 1);
}

TEST_CASE("monitor follow: ticking the box follows the newest rows again")
{
    follow_fixture fixture;
    fixture.feed(400);
    fixture.wheel(3);
    REQUIRE(fixture.follow_is(false));
    fixture.feed(50);
    REQUIRE(!fixture.at_end());

    // The one box a person sees, and the other follows it.
    fixture.visible_follow()->click();
    fixture.pump();
    REQUIRE(fixture.follow_is(true));
    // It scrolls to the end at once, and again with the next rows.
    REQUIRE(fixture.at_end());
    fixture.feed(50);
    REQUIRE(fixture.at_end());

    // Unticking leaves the view where it is.
    fixture.visible_follow()->click();
    fixture.pump();
    REQUIRE(fixture.follow_is(false));
    const view_state before = state_of(fixture);
    // At the very end the table is bottom aligned with a part of a row above
    // the first whole row; with new rows below, Qt lines the first row up with
    // the top edge, which is a shift of less than one row. The scroll bar's
    // position, and so the rows shown, do not change.
    const int position = fixture.table()->verticalScrollBar()->value();
    for (int batch = 0; batch < 3; ++batch)
    {
        fixture.feed(50);
        REQUIRE_EQ(fixture.table()->verticalScrollBar()->value(), position);
        REQUIRE(fixture.first_visible().toInt() - before.first.toInt() <= 1);
        REQUIRE(fixture.first_visible().toInt() >= before.first.toInt());
    }

    // The Simple bar's box is the same setting.
    fixture.panel.set_advanced(false);
    fixture.pump();
    fixture.visible_follow()->click();
    fixture.pump();
    REQUIRE(fixture.follow_is(true));
    REQUIRE(fixture.at_end());
}

TEST_CASE("monitor follow: with the history full the view stays on its rows")
{
    follow_fixture fixture;
    fixture.fill_history();
    // Follow is unticked with the box, as the owner did.
    fixture.visible_follow()->click();
    fixture.pump();
    REQUIRE(!fixture.visible_follow()->isChecked());

    fixture.show_at_top(5000);
    fixture.click_row(fixture.table()->rowAt(0) + 3);
    const view_state before = state_of(fixture);
    REQUIRE(!before.selected.isEmpty());

    // Ten batches of a hundred rows: a thousand rows leave the front of the
    // history, and the view still shows the same rows in the same place.
    require_view_stays(fixture, before, 10, 100);
    REQUIRE(fixture.follow_is(false));
    REQUIRE_EQ(fixture.table()->model()->rowCount(),
               vss::gui::MonitorModel::kMaxRows);
}

TEST_CASE("monitor follow: scrolling up with the history full stays put too")
{
    follow_fixture fixture;
    fixture.fill_history();
    REQUIRE(fixture.follow_is(true));
    REQUIRE(fixture.at_end());
    // Follow is unticked by scrolling up, with the wheel.
    fixture.wheel(5);
    REQUIRE(fixture.follow_is(false));
    fixture.click_row(fixture.table()->rowAt(0) + 3);
    const view_state before = state_of(fixture);
    REQUIRE(!before.selected.isEmpty());

    require_view_stays(fixture, before, 10, 100);
    REQUIRE(fixture.follow_is(false));
}

TEST_CASE("monitor follow: a view in pixel scroll mode stays put too")
{
    follow_fixture fixture;
    fixture.table()->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    fixture.fill_history();
    fixture.visible_follow()->click();
    fixture.pump();
    REQUIRE(fixture.follow_is(false));
    fixture.table()->verticalScrollBar()->setValue(
        fixture.table()->rowHeight(0) * 5000 + 7);
    fixture.pump();
    fixture.click_row(fixture.table()->rowAt(0) + 3);
    const view_state before = state_of(fixture);
    const int offset = fixture.table()->verticalScrollBar()->value() %
                       fixture.table()->rowHeight(0);
    require_view_stays(fixture, before, 10, 100);
    REQUIRE_EQ(fixture.table()->verticalScrollBar()->value() %
                   fixture.table()->rowHeight(0),
               offset);
}

TEST_CASE("monitor follow: rows that leave under the view do not throw it away")
{
    follow_fixture fixture;
    fixture.fill_history();
    fixture.visible_follow()->click();
    fixture.pump();
    REQUIRE(fixture.follow_is(false));
    fixture.show_at_top(0);
    fixture.click_row(4);
    const QString chosen = fixture.preview(4);
    REQUIRE_EQ(fixture.table()->currentIndex().row(), 4);

    // Two thousand rows leave the front, and the rows the user was looking at
    // with them. The view is at the top of what is left, not at the bottom,
    // and Follow stays unticked.
    for (int batch = 0; batch < 20; ++batch)
        fixture.feed(100);
    REQUIRE(fixture.follow_is(false));
    REQUIRE_EQ(fixture.table()->rowAt(0), 0);
    REQUIRE(!fixture.at_end());
    // The chosen row is gone, and nothing is selected in its place: the next
    // row is not the one the user chose.
    REQUIRE(!fixture.table()->selectionModel()->hasSelection());
    REQUIRE(!fixture.table()->currentIndex().isValid());
    for (int row = 0; row < 40; ++row)
        REQUIRE(fixture.preview(row) != chosen);
}

TEST_CASE("monitor follow: pause, resume and clear leave Follow as it is")
{
    follow_fixture fixture;
    fixture.feed(400);
    fixture.wheel(3);
    REQUIRE(fixture.follow_is(false));

    QPushButton* pause = nullptr;
    QPushButton* clear = nullptr;
    for (QPushButton* button : fixture.panel.findChildren<QPushButton*>())
    {
        if (button->text() == QStringLiteral("Pause") &&
            button->isVisibleTo(&fixture.panel))
            pause = button;
        if (button->text() == QStringLiteral("Clear") &&
            button->isVisibleTo(&fixture.panel))
            clear = button;
    }
    REQUIRE(pause != nullptr);
    REQUIRE(clear != nullptr);
    pause->click();
    fixture.pump();
    fixture.feed(50);
    REQUIRE_EQ(fixture.table()->model()->rowCount(), 400);
    REQUIRE(fixture.follow_is(false));
    pause->click();
    fixture.pump();
    REQUIRE(fixture.follow_is(false));
    clear->click();
    fixture.pump();
    REQUIRE(fixture.follow_is(false));
    REQUIRE_EQ(fixture.table()->model()->rowCount(), 0);
}

TEST_CASE("monitor follow: following is not taken for the user scrolling")
{
    // The scroll bar's range grows with every batch, and Follow scrolls the
    // view itself: neither is the user leaving the end.
    follow_fixture fixture;
    for (int batch = 0; batch < 30; ++batch)
    {
        fixture.feed(40);
        REQUIRE(fixture.follow_is(true));
        REQUIRE(fixture.at_end());
    }
    fixture.fill_history();
    for (int batch = 0; batch < 5; ++batch)
    {
        fixture.feed(100);
        REQUIRE(fixture.follow_is(true));
        REQUIRE(fixture.at_end());
    }
}
