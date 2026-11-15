//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Monitor's rows, header and columns: how tall and wide they are, and
// that the Density setting reaches them. Sizes are worked out from the fonts
// at run time and never compared with a pixel count, except for the 24 px
// target size.
//

#include "test_framework.hpp"

#include "qt_input.hpp"

#include "backend/fake_studio_backend.hpp"
#include "language/language_choice.hpp"
#include "panes/monitor_model.hpp"
#include "panes/monitor_panel.hpp"
#include "panes/ports_panel.hpp"
#include "theme/theme.hpp"
#include "widgets/surfaces.hpp"

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QProcess>
#include <QSettings>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTranslator>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{

/// One data event as the engine reports it.
vss::gui::gui_event data_event(vss::endpoint_id id, const QString& name,
                               vss::data_direction direction,
                               const QByteArray& data)
{
    vss::gui::gui_event event;
    event.kind = vss::event_kind::data;
    event.endpoint = id;
    event.name = name;
    event.direction = direction;
    event.data = vss::gui::event_payload(data);
    return event;
}

/// The frame of the Monitor design's first mock-up: a device on `onetouch`
/// sends one message as many short reads, and each read is sent on to
/// `port-2`.
vss::gui::event_batch sample_traffic(vss::endpoint_id onetouch,
                                     vss::endpoint_id port_2)
{
    const QList<QByteArray> reads = {
        QByteArray::fromHex("03"),   QByteArray::fromHex("00"),
        QByteArray::fromHex("00"),   QByteArray::fromHex("1002"),
        QByteArray::fromHex("60"),   QByteArray::fromHex("41"),
        QByteArray::fromHex("0000"), QByteArray::fromHex("12"),
        QByteArray::fromHex("7F"),   QByteArray::fromHex("01"),
        QByteArray::fromHex("0000"), QByteArray::fromHex("3C"),
        QByteArray::fromHex("0002"), QByteArray::fromHex("1003")};
    vss::gui::event_batch events;
    int step = 0;
    for (const QByteArray& read : reads)
    {
        ++step;
        for (const bool received : {true, false})
        {
            auto event = data_event(received ? onetouch : port_2,
                                    received ? QStringLiteral("onetouch")
                                             : QStringLiteral("port-2"),
                                    received ? vss::data_direction::rx
                                             : vss::data_direction::tx,
                                    read);
            event.when += std::chrono::milliseconds(step);
            events.push_back(event);
        }
    }
    return events;
}

/// A Monitor panel with the sample traffic in it, the way the shell shows it.
struct monitor_fixture
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::MonitorPanel panel{backend};
    vss::endpoint_id onetouch = 0;
    vss::endpoint_id port_2 = 0;

    monitor_fixture()
    {
        const auto first = backend.create_virtual_port(
            QStringLiteral("onetouch"), vss::virtual_port_kind::pty, true);
        const auto second = backend.create_virtual_port(
            QStringLiteral("port-2"), vss::virtual_port_kind::pty, true);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        onetouch = *first;
        port_2 = *second;
        panel.set_commit_interval(0);
        panel.set_advanced(true);
        panel.set_show_advanced_tabs(false);
        panel.resize(1500, 420);
        panel.show();
        QApplication::processEvents();
        panel.handle_events(sample_traffic(onetouch, port_2));
        // A dropped-bytes row, for its tag.
        static_cast<vss::gui::MonitorModel*>(panel.table()->model())
            ->append_drop(QStringLiteral("onetouch"), 5);
        QApplication::processEvents();
    }

    QTableView* table() { return panel.table(); }
};

/// Puts the density and the theme back when a test ends, however it ends.
struct theme_guard
{
    theme_guard()
    {
        QSettings().remove(QStringLiteral("ui/monitorColumnWidths"));
    }

    ~theme_guard()
    {
        vss::gui::Theme& theme = vss::gui::Theme::instance();
        theme.set_density(vss::gui::density_mode::comfortable);
        theme.set_mode(vss::gui::theme_mode::light);
        QSettings().remove(QStringLiteral("ui/monitorColumnWidths"));
        QApplication::processEvents();
    }

    theme_guard(const theme_guard&) = delete;
    theme_guard& operator=(const theme_guard&) = delete;
};

/// The theme's font for `style`, made as many times larger as the table's text
/// is larger than the body text.
QFont cell_font(const QTableView* table, vss::gui::text_style style)
{
    const vss::gui::Theme& theme = vss::gui::Theme::instance();
    QFont font = theme.font(style);
    font.setPointSizeF(font.pointSizeF() * table->font().pointSizeF() /
                       theme.font(vss::gui::text_style::body).pointSizeF());
    return font;
}

/// The words of the direction tags are in the semi-bold caption font.
QFont tag_font(const QTableView* table)
{
    QFont font = cell_font(table, vss::gui::text_style::caption);
    font.setWeight(QFont::DemiBold);
    return font;
}

/// The rule of the design (D2) written out, with the 24 px floor in both
/// densities until the owner answers question 1.
int expected_row_height(const QTableView* table, vss::gui::density_mode mode)
{
    const vss::gui::Theme& theme = vss::gui::Theme::instance();
    const int text = std::max(
        QFontMetrics(cell_font(table, vss::gui::text_style::data)).height(),
        QFontMetrics(cell_font(table, vss::gui::text_style::caption)).height());
    const int pad = mode == vss::gui::density_mode::compact ? theme.space(0) / 2
                                                            : theme.space(0);
    return std::max(theme.metric(QStringLiteral("minTarget")), text + 2 * pad);
}

/// D3: the header's own text and a little above and below, at least 24 px.
int expected_header_height(const QTableView* table)
{
    const vss::gui::Theme& theme = vss::gui::Theme::instance();
    return std::max(theme.metric(QStringLiteral("minTarget")),
                    QFontMetrics(table->horizontalHeader()->font()).height() +
                        2 * theme.space(0));
}

/// What the delegate of a column asks for a cell: its text, in the cell's
/// font, with its padding.
int cell_hint(QTableView* table, int row, int column)
{
    QStyleOptionViewItem option;
    option.initFrom(table);
    option.widget = table;
    QAbstractItemDelegate* delegate = table->itemDelegateForColumn(column);
    if (delegate == nullptr)
        delegate = table->itemDelegate();
    return delegate->sizeHint(option, table->model()->index(row, column))
        .width();
}

/// Nothing clipped: every heading has the width the header asks for, and the
/// cells of the first four columns have the width their delegate asks for.
/// (Preview is cut on purpose, with an ellipsis.)
void require_nothing_clipped(QTableView* table)
{
    QHeaderView* header = table->horizontalHeader();
    for (int column = 0; column < header->count(); ++column)
        REQUIRE(header->sectionSize(column) >= header->sectionSizeHint(column));
    for (int row = 0; row < table->model()->rowCount(); ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            REQUIRE(cell_hint(table, row, column) <=
                    header->sectionSize(column));
        }
    }
}

/// The words of the three tags, as the model shows them.
QStringList tag_words()
{
    return {vss::gui::MonitorModel::tr("RX"), vss::gui::MonitorModel::tr("TX"),
            vss::gui::MonitorModel::tr("DROP")};
}

/// Makes the text of the panel `factor` times as large as the body text. The
/// style sheet that a change of density applies again takes back a font that
/// `vss_test::scale_text` set on a widget (a person's text size comes through
/// `Theme` and stays), so the test sets it again after each change.
void keep_text_scaled(vss::gui::MonitorPanel& panel, double factor)
{
    if (factor == 1.0)
        return;
    const double body = vss::gui::Theme::instance()
                            .font(vss::gui::text_style::body)
                            .pointSizeF();
    if (panel.table()->font().pointSizeF() < body * factor * 0.99)
        vss_test::scale_text(&panel, factor);
    QApplication::processEvents();
}

/// A key press with Ctrl and Alt, sent to the table the way a person's would.
void press_ctrl_alt(QTableView* table, int key)
{
    QKeyEvent press(QEvent::KeyPress, key,
                    Qt::ControlModifier | Qt::AltModifier);
    QApplication::sendEvent(table, &press);
    QApplication::processEvents();
}

} // namespace

TEST_CASE("monitor panel: review pictures in both themes and both densities")
{
    // For a person to look at, never committed: written beside the build, in
    // gui-shots/monitor-density. VSS_MONITOR_SHOT_TAG names the set, so a run
    // before a change and a run after it do not overwrite each other.
    const QString tag = qEnvironmentVariableIsSet("VSS_MONITOR_SHOT_TAG")
                            ? qEnvironmentVariable("VSS_MONITOR_SHOT_TAG")
                            : QStringLiteral("after");
    const theme_guard guard;
    vss::gui::FakeStudioBackend backend;
    const auto onetouch = backend.create_virtual_port(
        QStringLiteral("onetouch"), vss::virtual_port_kind::pty, true);
    const auto port_2 = backend.create_virtual_port(
        QStringLiteral("port-2"), vss::virtual_port_kind::pty, true);
    REQUIRE(onetouch.has_value());
    REQUIRE(port_2.has_value());

    vss::gui::Pane pane(QStringLiteral("Monitor"));
    auto* panel = new vss::gui::MonitorPanel(backend);
    pane.set_body(panel);
    panel->attach_header(pane.header());
    panel->set_advanced(true);
    panel->set_show_advanced_tabs(false);
    panel->set_commit_interval(0);
    pane.resize(1100, 360);
    pane.show();
    QApplication::processEvents();
    panel->handle_events(sample_traffic(*onetouch, *port_2));
    QApplication::processEvents();

    // Hex shows the bytes of the frame; ASCII would show dots.
    for (QCheckBox* box : pane.findChildren<QCheckBox*>())
    {
        if (box->text() == QStringLiteral("Hex"))
            box->setChecked(true);
    }
    QApplication::processEvents();
    auto* table = pane.findChild<QTableView*>();
    REQUIRE(table != nullptr);
    REQUIRE_EQ(table->model()->rowCount(), 28);

    const QString directory =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("../gui-shots/monitor-density"));
    REQUIRE(QDir().mkpath(directory));

    vss::gui::Theme& theme = vss::gui::Theme::instance();
    for (const auto density :
         {vss::gui::density_mode::comfortable, vss::gui::density_mode::compact})
    {
        for (const auto mode :
             {vss::gui::theme_mode::light, vss::gui::theme_mode::dark})
        {
            theme.set_density(density);
            theme.set_mode(mode);
            QApplication::processEvents();
            const bool compact = density == vss::gui::density_mode::compact;
            const bool dark = mode == vss::gui::theme_mode::dark;
            const QString name =
                QStringLiteral("monitor-%1-%2-%3.png")
                    .arg(tag,
                         compact ? QStringLiteral("compact")
                                 : QStringLiteral("comfortable"),
                         dark ? QStringLiteral("dark")
                              : QStringLiteral("light"));
            REQUIRE(pane.grab().save(QDir(directory).filePath(name)));
            if (!dark)
            {
                const int header = table->horizontalHeader()->height();
                const int row = table->rowHeight(0);
                // The two panes of the design: the screenshot's row area
                // (308 px) and the default dock (196 px less its 44 px pane
                // header).
                std::printf("[monitor %s %s] header %d px, row %d px, rows in "
                            "308 px: %d, rows in 152 px: %d\n",
                            qPrintable(tag),
                            compact ? "compact" : "comfortable", header, row,
                            (308 - header) / row, (152 - header) / row);
            }
        }
    }
}

TEST_CASE("monitor panel: rows and header are as tall as their text needs")
{
    // T-D1.
    const theme_guard guard;
    monitor_fixture fixture;
    QTableView* table = fixture.table();
    const vss::gui::Theme& theme = vss::gui::Theme::instance();
    REQUIRE_EQ(theme.metric(QStringLiteral("minTarget")), 24);

    for (const auto mode :
         {vss::gui::density_mode::comfortable, vss::gui::density_mode::compact})
    {
        vss::gui::Theme::instance().set_density(mode);
        QApplication::processEvents();
        const int row = table->rowHeight(0);
        REQUIRE_EQ(row, expected_row_height(table, mode));
        // The 24 px floor applies in both densities (open question 1 of the
        // Monitor design asks whether Compact may go lower).
        REQUIRE(row >= 24);
        REQUIRE_EQ(table->verticalHeader()->defaultSectionSize(), row);
        REQUIRE_EQ(table->horizontalHeader()->height(),
                   expected_header_height(table));
        REQUIRE(table->horizontalHeader()->height() >= 24);
        // Every row has one height.
        REQUIRE_EQ(table->rowHeight(table->model()->rowCount() - 1), row);
        REQUIRE_EQ(table->verticalHeader()->sectionResizeMode(0),
                   QHeaderView::Fixed);
    }

    // The rows that fit in the two panes of the design, against today's 28 px
    // rows under a 28 px header (10 and 4).
    vss::gui::Theme::instance().set_density(
        vss::gui::density_mode::comfortable);
    QApplication::processEvents();
    const int header = table->horizontalHeader()->height();
    const int row = table->rowHeight(0);
    const int in_screenshot = (308 - header) / row;
    const int in_dock = (152 - header) / row;
    std::printf("[monitor after] header %d px, row %d px, rows in 308 px: %d "
                "(before: 28, 28, 10), rows in 152 px: %d (before: 4)\n",
                header, row, in_screenshot, in_dock);
    REQUIRE(row < 28);
    REQUIRE(in_screenshot > (308 - 28) / 28);
    REQUIRE(in_dock >= (152 - 28) / 28);
}

TEST_CASE("monitor panel: at 200 percent text nothing is clipped")
{
    // T-D2.
    const theme_guard guard;
    for (const double factor : {1.0, 2.0})
    {
        monitor_fixture fixture;
        fixture.panel.resize(2600, 500);
        if (factor != 1.0)
            vss_test::scale_text(&fixture.panel, factor);
        QApplication::processEvents();
        QTableView* table = fixture.table();
        const QFont data = cell_font(table, vss::gui::text_style::data);
        if (factor != 1.0)
        {
            REQUIRE(data.pointSizeF() >=
                    factor * vss::gui::Theme::instance()
                                 .font(vss::gui::text_style::data)
                                 .pointSizeF());
        }
        REQUIRE_EQ(
            table->rowHeight(0),
            expected_row_height(table, vss::gui::density_mode::comfortable));
        REQUIRE(table->rowHeight(0) >= QFontMetrics(data).height());
        REQUIRE(table->horizontalHeader()->sectionSize(0) >=
                QFontMetrics(data).horizontalAdvance(
                    QStringLiteral("00:00:00.000")));
        const QFontMetrics tag(tag_font(table));
        for (const QString& word : tag_words())
        {
            REQUIRE(table->horizontalHeader()->sectionSize(2) >=
                    tag.horizontalAdvance(word) + 12);
        }
        require_nothing_clipped(table);

        // The controls of the bar stay inside it and keep their minimum size.
        auto* filter = fixture.panel.findChild<QComboBox*>();
        REQUIRE(filter != nullptr);
        QWidget* bar = filter->parentWidget();
        REQUIRE(bar != nullptr);
        for (QWidget* control :
             bar->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        {
            if (!control->isVisibleTo(bar))
                continue;
            REQUIRE(bar->rect().contains(control->geometry()));
            REQUIRE(control->width() >= control->minimumSizeHint().width());
        }
    }
}

namespace
{

/// The catalogues of the eight languages.
QStringList catalogue_codes()
{
    return {QStringLiteral("de"),    QStringLiteral("es"), QStringLiteral("fr"),
            QStringLiteral("he"),    QStringLiteral("yi"), QStringLiteral("ja"),
            QStringLiteral("zh_CN"), QStringLiteral("ar")};
}

/// Compiles the catalogue of `code`, installs it and removes it again when it
/// goes out of scope.
class catalogue_in_use
{
public:
    explicit catalogue_in_use(const QString& code)
    {
        const QString qm = QStringLiteral(VSS_I18N_SCRATCH_DIR "/monitor_") +
                           code + QStringLiteral(".qm");
        QFile::remove(qm);
        QProcess lrelease;
        lrelease.start(QStringLiteral(VSS_LRELEASE_EXE),
                       {QStringLiteral(VSS_TRANSLATIONS_DIR "/vss_gui_") +
                            code + QStringLiteral(".ts"),
                        QStringLiteral("-qm"), qm});
        REQUIRE(lrelease.waitForFinished(120000));
        REQUIRE_EQ(lrelease.exitCode(), 0);
        translator_ = vss::gui::make_catalogue_translator(qm, &owner_);
        REQUIRE(translator_ != nullptr);
        QCoreApplication::installTranslator(translator_);
        QApplication::processEvents();
    }

    ~catalogue_in_use()
    {
        QCoreApplication::removeTranslator(translator_);
        QApplication::processEvents();
    }

    catalogue_in_use(const catalogue_in_use&) = delete;
    catalogue_in_use& operator=(const catalogue_in_use&) = delete;

private:
    QObject owner_;
    QTranslator* translator_ = nullptr;
};

} // namespace

TEST_CASE("monitor panel: no heading or tag is clipped in any language")
{
    // T-D3: the headings and the DROP tag in each of the eight catalogues.
    const theme_guard guard;
    for (const QString& code : catalogue_codes())
    {
        try
        {
            const catalogue_in_use catalogue(code);
            monitor_fixture fixture;
            QTableView* table = fixture.table();
            QHeaderView* header = table->horizontalHeader();
            // The headings are in the language.
            REQUIRE_EQ(table->model()
                           ->headerData(2, Qt::Horizontal, Qt::DisplayRole)
                           .toString(),
                       vss::gui::MonitorModel::tr("Direction"));
            require_nothing_clipped(table);
            const QFontMetrics tag(tag_font(table));
            for (const QString& word : tag_words())
                REQUIRE(header->sectionSize(2) >=
                        tag.horizontalAdvance(word) + 12);
            // The last row is the DROP row.
            REQUIRE_EQ(table->model()
                           ->index(table->model()->rowCount() - 1, 2)
                           .data()
                           .toString(),
                       vss::gui::MonitorModel::tr("DROP"));
        }
        catch (const vss_test::failure& failed)
        {
            throw vss_test::failure(code.toStdString() + ": " + failed.what());
        }
    }
}

TEST_CASE("monitor panel: a change of language refits the columns")
{
    const theme_guard guard;
    monitor_fixture fixture;
    QTableView* table = fixture.table();
    const QString english =
        table->model()->headerData(2, Qt::Horizontal).toString();
    {
        const catalogue_in_use catalogue(QStringLiteral("de"));
        REQUIRE(table->model()->headerData(2, Qt::Horizontal).toString() !=
                english);
        require_nothing_clipped(table);
    }
    REQUIRE_EQ(table->model()->headerData(2, Qt::Horizontal).toString(),
               english);
    require_nothing_clipped(table);
}

TEST_CASE("monitor panel: the Density setting changes the Monitor")
{
    // T-D4: no call to `set_compact`; the theme alone does it.
    const theme_guard guard;
    for (const double factor : {1.0, 2.0})
    {
        monitor_fixture fixture;
        if (factor != 1.0)
            vss_test::scale_text(&fixture.panel, factor);
        QApplication::processEvents();
        QTableView* table = fixture.table();
        vss::gui::Theme& theme = vss::gui::Theme::instance();

        theme.set_density(vss::gui::density_mode::comfortable);
        QApplication::processEvents();
        keep_text_scaled(fixture.panel, factor);
        const int comfortable_row = table->rowHeight(0);
        const int comfortable_time = table->columnWidth(0);
        REQUIRE_EQ(
            comfortable_row,
            expected_row_height(table, vss::gui::density_mode::comfortable));

        theme.set_density(vss::gui::density_mode::compact);
        QApplication::processEvents();
        keep_text_scaled(fixture.panel, factor);
        const int compact_row = table->rowHeight(0);
        REQUIRE_EQ(compact_row,
                   expected_row_height(table, vss::gui::density_mode::compact));
        REQUIRE(compact_row >= 24);
        REQUIRE(compact_row <= comfortable_row);
        // Compact has less space at the sides of a cell, so the same text
        // asks for a narrower column.
        REQUIRE(table->columnWidth(0) < comfortable_time);
        if (factor > 1.0)
        {
            // With larger text the rows are taller than the floor, so the
            // density shows in the row height too.
            REQUIRE(compact_row < comfortable_row);
        }
        require_nothing_clipped(table);

        // The shell's small-window layout does not change the row height.
        fixture.panel.set_compact(true);
        QApplication::processEvents();
        REQUIRE_EQ(table->rowHeight(0), compact_row);
        fixture.panel.set_compact(false);
        QApplication::processEvents();
        REQUIRE_EQ(table->rowHeight(0), compact_row);

        theme.set_density(vss::gui::density_mode::comfortable);
        QApplication::processEvents();
        keep_text_scaled(fixture.panel, factor);
        REQUIRE_EQ(table->rowHeight(0), comfortable_row);
        REQUIRE_EQ(table->columnWidth(0), comfortable_time);
    }
}

TEST_CASE("monitor panel: columns can be resized and the widths are kept")
{
    // T-D5.
    const theme_guard guard;
    int saved_width = 0;
    int default_width = 0;
    {
        monitor_fixture fixture;
        QTableView* table = fixture.table();
        QHeaderView* header = table->horizontalHeader();
        for (int column = 0; column < 4; ++column)
        {
            REQUIRE_EQ(header->sectionResizeMode(column),
                       QHeaderView::Interactive);
        }
        REQUIRE_EQ(header->sectionResizeMode(4), QHeaderView::Stretch);
        REQUIRE_EQ(header->minimumSectionSize(),
                   vss::gui::PortsPanel::kMinimumColumnWidth);
        REQUIRE_EQ(vss::gui::MonitorPanel::kMinimumColumnWidth, 48);
        REQUIRE(!header->sectionsMovable());
        REQUIRE_EQ(header->accessibleName(),
                   vss::gui::MonitorPanel::tr("Traffic table columns"));

        // The keyboard: Right chooses the column, Ctrl+Alt+Right widens it by
        // 16 px, Ctrl+Alt+Left narrows it, Ctrl+Alt+0 fits it.
        QString message;
        QObject::connect(&fixture.panel,
                         &vss::gui::MonitorPanel::status_message,
                         [&message](const QString& text) { message = text; });
        table->setFocus();
        table->setCurrentIndex(table->model()->index(0, 1));
        QApplication::processEvents();
        default_width = header->sectionSize(1);
        press_ctrl_alt(table, Qt::Key_Right);
        REQUIRE_EQ(header->sectionSize(1), default_width + 16);
        REQUIRE(message.contains(QString::number(default_width + 16)));
        press_ctrl_alt(table, Qt::Key_Left);
        press_ctrl_alt(table, Qt::Key_Left);
        REQUIRE_EQ(header->sectionSize(1), default_width - 16);
        for (int step = 0; step < 20; ++step)
            press_ctrl_alt(table, Qt::Key_Left);
        REQUIRE_EQ(header->sectionSize(1),
                   vss::gui::PortsPanel::kMinimumColumnWidth);
        press_ctrl_alt(table, Qt::Key_0);
        REQUIRE_EQ(header->sectionSize(1), default_width);
        // Preview takes the rest and has no width to change.
        table->setCurrentIndex(table->model()->index(0, 4));
        const int preview = header->sectionSize(4);
        press_ctrl_alt(table, Qt::Key_Right);
        REQUIRE_EQ(header->sectionSize(4), preview);

        // A drag of the divider.
        header->resizeSection(0, 150);
        QApplication::processEvents();
        saved_width = header->sectionSize(0);
        REQUIRE_EQ(saved_width, 150);
        fixture.panel.save_column_widths();
    }

    // A new panel keeps the width the user chose and the default of the rest.
    {
        monitor_fixture fixture;
        QTableView* table = fixture.table();
        QHeaderView* header = table->horizontalHeader();
        REQUIRE(std::abs(header->sectionSize(0) - saved_width) <= 1);
        REQUIRE_EQ(header->sectionSize(1), default_width);
        require_nothing_clipped(table);
    }

    // The width is kept as a number of characters, so it still fits when the
    // text is twice as large, and so does the heading.
    {
        monitor_fixture fixture;
        vss_test::scale_text(&fixture.panel, 2.0);
        QApplication::processEvents();
        QTableView* table = fixture.table();
        QHeaderView* header = table->horizontalHeader();
        REQUIRE(header->sectionSize(0) >= saved_width * 2 - 16);
        require_nothing_clipped(table);
    }

    // A width saved as nonsense is ignored.
    QSettings().setValue(QStringLiteral("ui/monitorColumnWidths"),
                         QStringList{QStringLiteral("x"), QStringLiteral("-3"),
                                     QStringLiteral("0"),
                                     QStringLiteral("1e9")});
    {
        monitor_fixture fixture;
        QTableView* table = fixture.table();
        REQUIRE_EQ(table->horizontalHeader()->sectionSize(1), default_width);
        require_nothing_clipped(table);
    }
}
