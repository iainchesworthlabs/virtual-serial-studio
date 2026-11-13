//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "panes/fixture_behaviour.hpp"
#include "panes/fixture_panel.hpp"
#include "panes/inspector_panel.hpp"
#include "panes/monitor_cell_delegate.hpp"
#include "panes/monitor_model.hpp"
#include "panes/monitor_panel.hpp"
#include "panes/port_delegate.hpp"
#include "panes/port_model.hpp"
#include "panes/ports_panel.hpp"
#include "panes/selection_service.hpp"
#include "panes/send_panel.hpp"
#include "widgets/notes.hpp"
#include "widgets/port_menu.hpp"
#include "widgets/surfaces.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTableView>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace
{

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

vss::gui::port_view make_port(vss::endpoint_id id, const QString& name,
                              const QString& path, vss::endpoint_kind kind,
                              bool open = true)
{
    vss::gui::port_view port;
    port.id = id;
    port.name = name;
    port.path = path;
    port.kind = kind;
    port.state = open ? vss::endpoint_state::open : vss::endpoint_state::closed;
    port.open = open;
    return port;
}

} // namespace

TEST_CASE("ports: state words, role text and filtering")
{
    vss::gui::PortModel model;
    auto terminal =
        make_port(1, QStringLiteral("terminal"), QStringLiteral("COM7"),
                  vss::endpoint_kind::virtual_port);
    terminal.peer = vss::gui::peer_state::not_open;
    terminal.stats.bytes_rx = 12;
    terminal.stats.bytes_tx = 2048;
    auto device = make_port(2, QStringLiteral("device"), QStringLiteral("COM3"),
                            vss::endpoint_kind::physical);
    device.serial.baud_rate = 115200;
    auto logger = make_port(3, QStringLiteral("logger"),
                            QStringLiteral("loopback:logger"),
                            vss::endpoint_kind::virtual_port);
    logger.studio_only = true;

    vss::gui::link_view link;
    link.id = 1;
    link.sources = {2};
    link.targets = {1};
    model.set_snapshot({terminal, device, logger}, {link});

    REQUIRE_EQ(model.rowCount(), 3);
    REQUIRE_EQ(vss::gui::PortModel::state_text(terminal),
               QStringLiteral("Waiting for program"));
    REQUIRE_EQ(vss::gui::PortModel::state_text(device), QStringLiteral("Open"));
    REQUIRE_EQ(vss::gui::PortModel::state_text(logger),
               QStringLiteral("Studio only"));
    REQUIRE(vss::gui::PortModel::role_text(terminal, {terminal, device, logger},
                                           {link})
                .contains(QStringLiteral("linked to device")));
    REQUIRE(vss::gui::PortModel::role_text(device, {terminal, device, logger},
                                           {link})
                .contains(QStringLiteral("shared with 1 programs")));
    REQUIRE(vss::gui::PortModel::serial_text(device).contains(
        QStringLiteral("115200")));

    const QModelIndex row = model.index(0, 0);
    REQUIRE(row.data(vss::gui::PortModel::port_id_role).toULongLong() == 1);
    REQUIRE(row.data(Qt::AccessibleTextRole)
                .toString()
                .contains(QStringLiteral("COM7")));
    REQUIRE(row.data(vss::gui::PortModel::bytes_tx_role)
                .toString()
                .contains(QStringLiteral("kB")));

    model.set_filter(QStringLiteral("COM3"));
    REQUIRE_EQ(model.rowCount(), 1);
    REQUIRE_EQ(model.port_id(0), vss::endpoint_id{2});
    REQUIRE_EQ(model.row_for_id(2), 0);
    model.set_filter(QStringLiteral("nope"));
    REQUIRE_EQ(model.rowCount(), 0);
}

TEST_CASE("ports: the other side's format, clients and a speed mismatch")
{
    using vss::gui::PortModel;

    // Nothing known: nothing said.
    auto plain = make_port(1, QStringLiteral("plain"), QStringLiteral("COM7"),
                           vss::endpoint_kind::virtual_port);
    REQUIRE_EQ(PortModel::status_text(plain), QString());
    REQUIRE(!PortModel::has_format_warning(plain));

    // A program set its speed on a port Studio made.
    auto program =
        make_port(2, QStringLiteral("program"), QStringLiteral("COM8"),
                  vss::endpoint_kind::virtual_port);
    program.peer_format = vss::line_format{19200, 7, vss::parity_mode::even,
                                           vss::stop_bits_mode::two};
    REQUIRE_EQ(PortModel::status_text(program),
               QStringLiteral("Program set 19200 7E2"));
    REQUIRE(!PortModel::has_format_warning(program));

    // A real device already shows its own settings, so the format is not
    // repeated for it.
    auto device = make_port(3, QStringLiteral("device"), QStringLiteral("COM3"),
                            vss::endpoint_kind::physical);
    device.peer_format = vss::line_format{9600, 8, vss::parity_mode::none,
                                          vss::stop_bits_mode::one};
    REQUIRE_EQ(PortModel::status_text(device), QString());

    // A listener with nobody connected still says so; one with clients counts
    // them.
    auto listener = make_port(4, QStringLiteral("listener"),
                              QStringLiteral("tcp-listen://127.0.0.1:2000"),
                              vss::endpoint_kind::tcp);
    listener.clients = 0;
    REQUIRE_EQ(PortModel::status_text(listener),
               QStringLiteral("No remote connected"));
    listener.clients = 3;
    REQUIRE_EQ(PortModel::status_text(listener),
               QStringLiteral("Clients connected: 3"));

    // A mismatch is a warning that names the others, after the rest.
    program.format_conflicts = {QStringLiteral("device"),
                                QStringLiteral("modem")};
    REQUIRE(PortModel::has_format_warning(program));
    REQUIRE_EQ(PortModel::status_text(program),
               QStringLiteral("Program set 19200 7E2. Differs from device, "
                              "modem in baud rate or framing"));

    // The row carries the same text for the list, the tooltip and a screen
    // reader.
    vss::gui::PortModel model;
    model.set_snapshot({program, listener}, {});
    const QModelIndex row = model.index(0, 0);
    REQUIRE_EQ(row.data(vss::gui::PortModel::status_role).toString(),
               PortModel::status_text(program));
    REQUIRE(row.data(vss::gui::PortModel::format_warning_role).toBool());
    REQUIRE(row.data(Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("Differs from device")));
    REQUIRE(row.data(Qt::AccessibleTextRole)
                .toString()
                .contains(QStringLiteral("Differs from device")));
    REQUIRE(!model.index(1, 0)
                 .data(vss::gui::PortModel::format_warning_role)
                 .toBool());
}

TEST_CASE("monitor: 10,000 row burst stays bounded and responsive")
{
    vss::gui::MonitorModel model;
    vss::gui::event_batch events;
    for (int index = 0; index < 10'000; ++index)
    {
        events.push_back(data_event(
            static_cast<vss::endpoint_id>(index % 3 + 1),
            QStringLiteral("terminal"),
            index % 2 == 0 ? vss::data_direction::rx : vss::data_direction::tx,
            QByteArrayLiteral("x")));
    }
    QElapsedTimer timer;
    timer.start();
    model.append_events(events);
    const qint64 elapsed = timer.elapsed();
    REQUIRE_EQ(model.rowCount(), 10'000);
    REQUIRE_EQ(model.total_rows(), 10'000);
    REQUIRE(elapsed < 5000);

    vss::gui::event_batch more;
    for (int index = 0; index < 25; ++index)
    {
        more.push_back(data_event(1, QStringLiteral("terminal"),
                                  vss::data_direction::rx,
                                  QByteArrayLiteral("y")));
    }
    model.append_events(more);
    REQUIRE_EQ(model.total_rows(), 10'000);
    REQUIRE_EQ(model.rowCount(), 10'000);
}

TEST_CASE("monitor: new traffic is added without resetting the model")
{
    // A reset makes the view lay out every row again and forget its scroll
    // position, for each batch of traffic. Rows must arrive as insertions at
    // the end and leave as removals from the front.
    vss::gui::MonitorModel model;
    int resets = 0;
    int inserted = 0;
    int removed = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset,
                     [&resets] { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted,
                     [&inserted](const QModelIndex&, int first, int last)
                     { inserted += last - first + 1; });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved,
                     [&removed](const QModelIndex&, int first, int last)
                     { removed += last - first + 1; });

    vss::gui::event_batch first;
    for (int index = 0; index < 5; ++index)
    {
        first.push_back(data_event(1, QStringLiteral("a"),
                                   vss::data_direction::rx,
                                   QByteArrayLiteral("x")));
    }
    model.append_events(first);
    REQUIRE_EQ(model.rowCount(), 5);
    REQUIRE_EQ(inserted, 5);
    REQUIRE_EQ(resets, 0);

    // Only one port is shown: a row of another port is kept but not listed,
    // and nothing is inserted for it.
    model.set_filters(1, true, true, true, {});
    const int resets_after_filter = resets;
    vss::gui::event_batch mixed;
    mixed.push_back(data_event(2, QStringLiteral("b"), vss::data_direction::rx,
                               QByteArrayLiteral("y")));
    mixed.push_back(data_event(1, QStringLiteral("a"), vss::data_direction::tx,
                               QByteArrayLiteral("z")));
    model.append_events(mixed);
    REQUIRE_EQ(model.total_rows(), 7);
    REQUIRE_EQ(model.rowCount(), 6);
    REQUIRE_EQ(inserted, 6);
    REQUIRE_EQ(resets, resets_after_filter);
    REQUIRE_EQ(model.row_at(5)->preview().toByteArray(),
               QByteArrayLiteral("z"));

    // Past the cap the oldest rows leave from the front, and the rows that
    // stay keep their order.
    model.set_filters(0, true, true, true, {});
    vss::gui::event_batch flood;
    for (int index = 0; index < vss::gui::MonitorModel::kMaxRows; ++index)
    {
        flood.push_back(data_event(1, QStringLiteral("a"),
                                   vss::data_direction::rx,
                                   QByteArray::number(index)));
    }
    const int resets_before_flood = resets;
    model.append_events(flood);
    REQUIRE_EQ(model.total_rows(), vss::gui::MonitorModel::kMaxRows);
    REQUIRE_EQ(model.rowCount(), vss::gui::MonitorModel::kMaxRows);
    REQUIRE(removed > 0);
    REQUIRE_EQ(resets, resets_before_flood);
    REQUIRE_EQ(model.row_at(0)->preview().toByteArray(),
               QByteArrayLiteral("0"));
    REQUIRE_EQ(model.row_at(vss::gui::MonitorModel::kMaxRows - 1)
                   ->preview()
                   .toByteArray(),
               QByteArray::number(vss::gui::MonitorModel::kMaxRows - 1));
}

TEST_CASE("monitor: ascii, hex, filtering, pause and drop rows")
{
    vss::gui::MonitorModel model;
    vss::gui::event_batch events;
    events.push_back(data_event(1, QStringLiteral("terminal"),
                                vss::data_direction::rx,
                                QByteArrayLiteral("Hi\r\n")));
    events.push_back(data_event(1, QStringLiteral("terminal"),
                                vss::data_direction::tx,
                                QByteArrayLiteral("AB")));
    events.push_back(data_event(2, QStringLiteral("device"),
                                vss::data_direction::rx,
                                QByteArrayLiteral("Z")));
    model.append_events(events);
    REQUIRE_EQ(model.rowCount(), 3);

    REQUIRE_EQ(model.index(0, 4).data().toString(), QStringLiteral("Hi\\r\\n"));
    model.set_hex(true);
    REQUIRE_EQ(model.index(0, 4).data().toString(),
               QStringLiteral("48 69 0D 0A"));
    REQUIRE_EQ(
        vss::gui::MonitorModel::ascii_preview(QByteArrayLiteral("a\x01")),
        QStringLiteral("a."));

    model.set_filters(0, true, false, true, {});
    REQUIRE_EQ(model.rowCount(), 2);
    model.set_filters(0, true, true, true, QStringLiteral("device"));
    REQUIRE_EQ(model.rowCount(), 1);
    REQUIRE_EQ(model.port_at(0), 2);

    model.set_filters(0, true, true, true, {});
    const int before = model.rowCount();
    model.set_paused(true);
    model.append_events(
        {data_event(1, QStringLiteral("terminal"), vss::data_direction::rx,
                    QByteArrayLiteral("paused"))});
    REQUIRE_EQ(model.rowCount(), before);
    model.set_paused(false);

    model.append_drop(QStringLiteral("terminal"), 42);
    model.set_filters(0, true, true, true, {});
    REQUIRE(model.rowCount() == before + 1);
    REQUIRE(model.index(model.rowCount() - 1, 4)
                .data()
                .toString()
                .contains(QStringLiteral("not the owner")));

    // A bus has no owner: its drops say the bus did not carry the bytes.
    model.append_drop(QStringLiteral("rs485"), 7, true);
    const QModelIndex bus_row = model.index(model.rowCount() - 1, 4);
    REQUIRE_EQ(bus_row.data().toString(),
               QStringLiteral("not carried by the bus"));
    REQUIRE_EQ(bus_row.data(Qt::ToolTipRole).toString(),
               QStringLiteral("Not carried by the bus."));
    REQUIRE_EQ(
        model.index(model.rowCount() - 2, 4).data(Qt::ToolTipRole).toString(),
        QStringLiteral("Not the owner, not forwarded."));
}

namespace
{

vss::gui::gui_event timed_event(vss::endpoint_id id, const QString& name,
                                vss::data_direction direction,
                                const QByteArray& data, vss::time_point when)
{
    auto event = data_event(id, name, direction, data);
    event.when = when;
    return event;
}

} // namespace

TEST_CASE("monitor: the capture export keeps every byte and the event time")
{
    vss::gui::MonitorModel model;
    const auto base = vss::clock_type::now();
    using namespace std::chrono_literals;

    // Longer than the 256 bytes the table shows.
    const QByteArray big(1000, 'q');
    vss::gui::event_batch events;
    events.push_back(timed_event(1, QStringLiteral("master"),
                                 vss::data_direction::rx, big, base + 100ms));
    events.push_back(timed_event(2, QStringLiteral("tap"),
                                 vss::data_direction::tx,
                                 QByteArrayLiteral("ab"), base + 350ms));
    model.append_events(events);
    model.append_drop(QStringLiteral("master"), 5);

    // The preview is cut for display; the capture is not.
    REQUIRE_EQ(model.row_at(0)->preview().size(), 256);
    const auto everything = model.export_capture();
    REQUIRE_EQ(everything.records.size(), std::size_t{2});
    REQUIRE_EQ(everything.records[0].data.size(), std::size_t{1000});
    REQUIRE_EQ(everything.records[0].port, std::string("master"));
    REQUIRE(everything.records[0].direction == vss::data_direction::rx);
    // Offsets are the engine's own times, 250 ms apart, from the first row.
    REQUIRE_EQ(everything.records[0].offset_ms, 0);
    REQUIRE_EQ(everything.records[1].offset_ms, 250);
    REQUIRE(everything.records[1].direction == vss::data_direction::tx);

    // The filters apply: only the tap.
    model.set_filters(2, true, true, true, {});
    const auto tap_only = model.export_capture();
    REQUIRE_EQ(tap_only.records.size(), std::size_t{1});
    REQUIRE_EQ(tap_only.records[0].port, std::string("tap"));
}

TEST_CASE("monitor: payloads kept for the capture export are bounded")
{
    vss::gui::MonitorModel model;
    const QByteArray chunk(1024LL * 1024, 'z');
    const auto when = vss::clock_type::now();
    vss::gui::event_batch events;
    for (int index = 0; index < 40; ++index)
    {
        events.push_back(timed_event(1, QStringLiteral("p"),
                                     vss::data_direction::rx, chunk, when));
    }
    model.append_events(events);

    // 40 MiB came in; 32 MiB is the most that is kept, newest first.
    REQUIRE_EQ(model.total_rows(), 32);
    REQUIRE_EQ(model.export_capture().records.size(), std::size_t{32});
}

TEST_CASE("monitor panel: Export capture is named and follows the rows")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::MonitorPanel panel(backend);
    panel.resize(900, 240);
    panel.set_advanced(true);
    panel.show();
    QApplication::processEvents();

    QPushButton* button = nullptr;
    for (QPushButton* candidate : panel.findChildren<QPushButton*>())
    {
        if (candidate->text() == QStringLiteral("Export capture"))
            button = candidate;
    }
    REQUIRE(button != nullptr);
    REQUIRE_EQ(button->accessibleName(), QStringLiteral("Export capture"));
    REQUIRE(!button->accessibleDescription().isEmpty());
    // Nothing to export yet, the same as the text export beside it.
    REQUIRE(!button->isEnabled());

    panel.handle_events(
        {data_event(1, QStringLiteral("terminal"), vss::data_direction::rx,
                    QByteArrayLiteral("x"))});
    QApplication::processEvents();
    REQUIRE(button->isEnabled());
}

TEST_CASE("monitor panel: heavy traffic is added to the table in steps")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::MonitorPanel panel(backend);
    panel.resize(900, 240);
    panel.show();
    QApplication::processEvents();
    auto* table = panel.findChild<QTableView*>();
    REQUIRE(table != nullptr);
    int insertions = 0;
    QObject::connect(table->model(), &QAbstractItemModel::rowsInserted,
                     [&insertions] { ++insertions; });

    // The first batch is shown at once.
    panel.set_commit_interval(60'000);
    const vss::gui::event_batch batch{data_event(1, QStringLiteral("terminal"),
                                                 vss::data_direction::rx,
                                                 QByteArrayLiteral("x"))};
    panel.handle_events(batch);
    REQUIRE_EQ(table->model()->rowCount(), 1);
    REQUIRE_EQ(insertions, 1);

    // Batches that follow before the interval is over wait, and arrive
    // together when it is.
    for (int index = 0; index < 20; ++index)
        panel.handle_events(batch);
    QApplication::processEvents();
    REQUIRE_EQ(table->model()->rowCount(), 1);
    panel.set_commit_interval(50);
    QElapsedTimer waited;
    waited.start();
    while (table->model()->rowCount() < 21 && waited.elapsed() < 10'000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    REQUIRE_EQ(table->model()->rowCount(), 21);
    REQUIRE_EQ(insertions, 2);

    // Pause shows what had arrived before it, and nothing after.
    panel.set_commit_interval(60'000);
    panel.handle_events(batch);
    REQUIRE_EQ(table->model()->rowCount(), 21);
    QPushButton* pause = nullptr;
    for (QPushButton* candidate : panel.findChildren<QPushButton*>())
    {
        if (candidate->text() == QStringLiteral("Pause") &&
            candidate->isVisible())
            pause = candidate;
    }
    REQUIRE(pause != nullptr);
    pause->click();
    REQUIRE_EQ(table->model()->rowCount(), 22);
    panel.handle_events(batch);
    panel.set_commit_interval(0);
    QApplication::processEvents();
    REQUIRE_EQ(table->model()->rowCount(), 22);
}

TEST_CASE("monitor: a long preview is drawn as Qt draws the whole text")
{
    // The cut is only made in a font whose characters all have one width.
    // Three fonts: the first with one width, the first the system calls
    // fixed-width (where only an emoji font is, its letters come from another
    // font and have many widths), and the application's own.
    QVector<QFont> fonts;
    bool one_width = false;
    bool called_fixed = false;
    for (const QString& family : QFontDatabase::families())
    {
        QFont font(family);
        font.setPixelSize(13);
        if (!one_width &&
            vss::gui::MonitorCellDelegate::uniform_advance(font) > 0.0)
        {
            one_width = true;
            fonts.push_back(font);
        }
        else if (!called_fixed && QFontDatabase::isFixedPitch(family))
        {
            called_fixed = true;
            fonts.push_back(font);
        }
    }
    fonts.push_back(QApplication::font());

    QString text;
    QString hex;
    for (int index = 0; index < 256; ++index)
    {
        text += QLatin1Char(static_cast<char>('A' + index % 26));
        hex += QStringLiteral("%1 ").arg(index, 2, 16, QLatin1Char('0'));
    }
    const QStringList previews{text, hex, QStringLiteral("short"),
                               text.left(90) + QChar(0x05e9) + text,
                               QString(600, QLatin1Char('i'))};
    QStandardItemModel model(static_cast<int>(previews.size()), 2);
    for (int row = 0; row < previews.size(); ++row)
    {
        model.setItem(row, 0, new QStandardItem(QStringLiteral("12:00:00")));
        model.setItem(row, 1, new QStandardItem(previews[row]));
    }

    QTableView table;
    table.setModel(&model);
    table.setWordWrap(false);
    table.horizontalHeader()->setStretchLastSection(true);
    table.setColumnWidth(0, 100);
    table.show();

    // The picture must be the one Qt's own delegate makes from the whole
    // text, whatever the font and the width of the cell.
    vss::gui::MonitorCellDelegate ours(1);
    QStyledItemDelegate plain;
    for (const QFont& font : fonts)
    {
        for (int row = 0; row < previews.size(); ++row)
            model.item(row, 1)->setFont(font);
        for (int width = 180; width <= 2400; width += 37)
        {
            table.resize(width, 200);
            QApplication::processEvents();
            table.setItemDelegate(&ours);
            const QImage cut = table.viewport()->grab().toImage();
            table.setItemDelegate(&plain);
            const QImage whole = table.viewport()->grab().toImage();
            REQUIRE(!cut.isNull());
            if (cut != whole)
            {
                std::printf(
                    "        differs in %s at width %d, advance %f",
                    qPrintable(QFontInfo(font).family()), width,
                    vss::gui::MonitorCellDelegate::uniform_advance(font));
                std::puts("");
            }
            REQUIRE(cut == whole);
        }
    }

    // And it is a cut: far fewer characters than the preview has.
    REQUIRE(vss::gui::MonitorCellDelegate::characters_kept(700, 7.0) < 150);
    REQUIRE(vss::gui::MonitorCellDelegate::characters_kept(700, 7.0) > 100);
}

TEST_CASE("monitor panel: the controls share the title row when it fits")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::Pane pane(QStringLiteral("Monitor"));
    auto* panel = new vss::gui::MonitorPanel(backend);
    pane.set_body(panel);
    panel->attach_header(pane.header());
    panel->set_advanced(true);
    pane.resize(1400, 260);
    pane.show();
    QApplication::processEvents();

    QPushButton* button = nullptr;
    for (QPushButton* candidate : pane.findChildren<QPushButton*>())
    {
        if (candidate->text() == QStringLiteral("Export capture"))
            button = candidate;
    }
    REQUIRE(button != nullptr);
    // Wide: the controls are inside the header, on the title's row.
    REQUIRE(panel->controls_in_header());
    REQUIRE(pane.header()->isAncestorOf(button));
    REQUIRE(button->isVisible());

    // Narrow: they drop to a row under it, and stay visible and whole.
    pane.resize(500, 260);
    QApplication::processEvents();
    REQUIRE(!panel->controls_in_header());
    REQUIRE(!pane.header()->isAncestorOf(button));
    REQUIRE(button->isVisible());
    REQUIRE(button->geometry().right() <= panel->width());

    // Back to wide, and Simple mode takes them out of the header again.
    pane.resize(1400, 260);
    QApplication::processEvents();
    REQUIRE(panel->controls_in_header());
    panel->set_advanced(false);
    QApplication::processEvents();
    REQUIRE(!panel->controls_in_header());
    REQUIRE(!button->isVisible());
}

TEST_CASE("monitor: a bus fault is logged with the engine's sentence")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::MonitorPanel panel(backend);
    panel.resize(900, 240);
    panel.set_advanced(true);
    panel.show();
    QApplication::processEvents();

    vss::gui::gui_event fault;
    fault.kind = vss::event_kind::bus_fault;
    fault.endpoint = 2;
    fault.name = QStringLiteral("p1");
    fault.message = QStringLiteral("sent 8 bytes while p0 was sending");
    panel.handle_events({fault});
    QApplication::processEvents();

    QString logged;
    for (QPlainTextEdit* edit : panel.findChildren<QPlainTextEdit*>())
        logged += edit->toPlainText();
    REQUIRE(logged.contains(QStringLiteral("p1")));
    REQUIRE(logged.contains(QStringLiteral("bus")));
    REQUIRE(
        logged.contains(QStringLiteral("sent 8 bytes while p0 was sending")));
}

TEST_CASE("send: hex validation, CRLF and the fake backend")
{
    vss::gui::FakeStudioBackend backend;
    const auto id = backend.create_virtual_port(
        QStringLiteral("send-test"), vss::virtual_port_kind::pty, true);
    REQUIRE(id.has_value());
    vss::gui::SendPanel panel(backend);
    panel.set_selected_port(*id);

    auto* hex = panel.findChild<QCheckBox*>();
    for (QCheckBox* box : panel.findChildren<QCheckBox*>())
    {
        if (box->text() == QStringLiteral("Hex"))
            hex = box;
    }
    REQUIRE(hex != nullptr);
    hex->setChecked(true);
    panel.set_text(QStringLiteral("48 65"));
    QApplication::processEvents();
    panel.send_now();
    REQUIRE_EQ(backend.sent().size(), 1);
    REQUIRE_EQ(backend.sent().front(), QByteArrayLiteral("He\r\n"));

    panel.set_text(QStringLiteral("4G"));
    QApplication::processEvents();
    QPushButton* send = nullptr;
    for (QPushButton* button : panel.findChildren<QPushButton*>())
    {
        if (button->text() == QStringLiteral("Send"))
            send = button;
    }
    REQUIRE(send != nullptr);
    REQUIRE(!send->isEnabled());
    bool found_error = false;
    for (QLabel* label : panel.findChildren<QLabel*>())
    {
        if (label->text().contains(QStringLiteral("Hex needs pairs")))
            found_error = true;
    }
    REQUIRE(found_error);
}

TEST_CASE("ports panel: selection stays in sync with the service")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::SelectionService selection;
    const auto first = backend.create_virtual_port(
        QStringLiteral("one"), vss::virtual_port_kind::pty, true);
    const auto second = backend.create_virtual_port(
        QStringLiteral("two"), vss::virtual_port_kind::pty, true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    vss::gui::PortsPanel panel(backend, selection);
    panel.resize(360, 300);
    panel.show();
    QApplication::processEvents();
    REQUIRE_EQ(selection.port(), *first);
    panel.select_port(*second);
    QApplication::processEvents();
    REQUIRE_EQ(selection.port(), *second);

    auto* list = panel.findChild<QListView*>();
    REQUIRE(list != nullptr);
    REQUIRE_EQ(list->model()->rowCount(), 2);
    panel.set_advanced(true);
    auto* table = panel.findChild<QTableView*>();
    REQUIRE(table != nullptr);
    REQUIRE_EQ(table->model()->rowCount(), 2);
}

TEST_CASE("ports panel: table columns resize, fit, stretch and persist")
{
    QSettings().remove(QStringLiteral("ui/portsColumnWidths"));
    vss::gui::FakeStudioBackend backend;
    vss::gui::SelectionService selection;
    REQUIRE(backend
                .create_virtual_port(QStringLiteral("one"),
                                     vss::virtual_port_kind::pty, true)
                .has_value());
    {
        vss::gui::PortsPanel panel(backend, selection);
        panel.resize(640, 300);
        panel.set_advanced(true);
        panel.show();
        QApplication::processEvents();
        QTableView* table = panel.table();
        QHeaderView* header = table->horizontalHeader();
        REQUIRE(table != nullptr);

        // Every column is user-resizable; the last takes the spare width.
        for (int column = 0; column < 4; ++column)
            REQUIRE_EQ(header->sectionResizeMode(column),
                       QHeaderView::Interactive);
        REQUIRE(header->stretchLastSection());
        REQUIRE(!header->sectionsMovable());
        REQUIRE_EQ(header->minimumSectionSize(),
                   vss::gui::PortsPanel::kMinimumColumnWidth);
        int total = 0;
        for (int column = 0; column < header->count(); ++column)
            total += header->sectionSize(column);
        REQUIRE_EQ(total, table->viewport()->width());

        // The State chip's longest text fits at the default width.
        REQUIRE(header->sectionSize(1) >=
                vss::gui::PortDelegate::state_chip_width(
                    vss::gui::PortModel::tr("Waiting for program")));

        // A drag changes one column and leaves the others alone.
        const int rx_before = header->sectionSize(2);
        header->resizeSection(1, 220);
        REQUIRE_EQ(header->sectionSize(1), 220);
        REQUIRE_EQ(header->sectionSize(2), rx_before);

        // Keyboard: Right selects the column, Ctrl+Alt+Right widens it.
        table->setFocus();
        table->setCurrentIndex(table->model()->index(0, 2));
        QKeyEvent widen(QEvent::KeyPress, Qt::Key_Right,
                        Qt::ControlModifier | Qt::AltModifier);
        QApplication::sendEvent(table, &widen);
        REQUIRE_EQ(header->sectionSize(2), rx_before + 16);
        QKeyEvent narrow(QEvent::KeyPress, Qt::Key_Left,
                         Qt::ControlModifier | Qt::AltModifier);
        for (int i = 0; i < 20; ++i)
            QApplication::sendEvent(table, &narrow);
        REQUIRE_EQ(header->sectionSize(2),
                   vss::gui::PortsPanel::kMinimumColumnWidth);
        header->resizeSection(2, 200);
        QKeyEvent fit(QEvent::KeyPress, Qt::Key_0,
                      Qt::ControlModifier | Qt::AltModifier);
        QApplication::sendEvent(table, &fit);
        REQUIRE(header->sectionSize(2) < 200);
        REQUIRE(header->sectionSize(2) >=
                vss::gui::PortsPanel::kMinimumColumnWidth);

        panel.save_column_widths();
    }

    // A new panel starts with the saved widths.
    vss::gui::PortsPanel again(backend, selection);
    again.set_advanced(true);
    REQUIRE_EQ(again.table()->horizontalHeader()->sectionSize(1), 220);
    QSettings().remove(QStringLiteral("ui/portsColumnWidths"));
}

TEST_CASE("ports panel: ports another program made are named, not shown")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::SelectionService selection;
    vss::gui::PortsPanel panel(backend, selection);
    panel.resize(360, 300);
    panel.show();
    QApplication::processEvents();
    REQUIRE(panel.other_program_note() != nullptr);
    REQUIRE(panel.other_program_note()->isHidden());

    vss::gui::device_view driver;
    driver.path = QStringLiteral("COM41");
    driver.hardware_id = QStringLiteral("VSSCOM\\PORT");
    driver.virtual_device = true;
    backend.set_devices({driver});

    // A change to the set of ports looks again at once.
    const auto created = backend.create_virtual_port(
        QStringLiteral("one"), vss::virtual_port_kind::loopback, true);
    REQUIRE(created.has_value());
    QApplication::processEvents();
    REQUIRE(!panel.other_program_note()->isHidden());
    REQUIRE(panel.other_program_note()->text().contains(
        QStringLiteral("Another program made COM41.")));
    REQUIRE_EQ(panel.other_program_note()->accessibleName(),
               panel.other_program_note()->text());
    auto* list = panel.findChild<QListView*>();
    REQUIRE(list != nullptr);
    REQUIRE_EQ(list->model()->rowCount(), 1);

    // Once this studio holds that port, it is no longer another program's.
    const auto attached =
        backend.attach_serial(QStringLiteral("held"), QStringLiteral("COM41"),
                              vss::serial_settings{}, true);
    REQUIRE(attached.has_value());
    QApplication::processEvents();
    REQUIRE(panel.other_program_note()->isHidden());
}

TEST_CASE("monitor panel: exact empty state and no row widgets")
{
    vss::gui::FakeStudioBackend backend;
    vss::gui::MonitorPanel panel(backend);
    panel.resize(900, 240);
    panel.show();
    QApplication::processEvents();
    bool empty = false;
    for (QLabel* label : panel.findChildren<QLabel*>())
    {
        if (label->text() ==
            QStringLiteral("No traffic yet. Send some bytes, or open the "
                           "port in a program."))
            empty = true;
    }
    REQUIRE(empty);
    auto* table = panel.findChild<QTableView*>();
    REQUIRE(table != nullptr);
    REQUIRE(table->viewport()->findChildren<QWidget*>().isEmpty());
    panel.set_advanced(true);
    QApplication::processEvents();
    REQUIRE(!panel.findChildren<QComboBox*>().isEmpty());
}

namespace
{

QPushButton* find_text_button(QWidget& parent, const QString& text)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>())
    {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

} // namespace

TEST_CASE("inspector: waiting sentence and control line chips")
{
    vss::gui::FakeStudioBackend backend;
    const auto id = backend.create_virtual_port(
        QStringLiteral("inspector-test"), vss::virtual_port_kind::pty, true);
    REQUIRE(id.has_value());
    backend.set_peer_state(*id, vss::gui::peer_state::not_open);
    vss::gui::InspectorPanel panel(backend);
    panel.set_selected_port(*id);
    QApplication::processEvents();

    bool waiting = false;
    for (QLabel* label : panel.findChildren<QLabel*>())
    {
        if (label->text().contains(QStringLiteral("No program has opened")))
            waiting = true;
    }
    REQUIRE(waiting);

    // The waiting note names the exact port. The path has one copy button,
    // and the old "Copy name" button is gone.
    bool names_port = false;
    for (QLabel* label : panel.findChildren<QLabel*>())
    {
        if (label->text().contains(QStringLiteral("has to open exactly")))
            names_port = true;
    }
    REQUIRE(names_port);
    REQUIRE(panel.findChild<QPushButton*>(
                QStringLiteral("inspectorCopyName")) == nullptr);
    REQUIRE(find_text_button(panel, QStringLiteral("Copy name")) == nullptr);
    auto* copy_path = panel.findChild<vss::gui::CopyButton*>();
    REQUIRE(copy_path != nullptr);
    REQUIRE(!copy_path->accessibleName().isEmpty());
    REQUIRE_EQ(copy_path->copy_text(),
               vss_test::require_value(backend.port(*id)).path);

    // The traffic row says that it shows totals.
    bool total = false;
    for (QLabel* label : panel.findChildren<QLabel*>())
    {
        if (label->text() == QStringLiteral("Total traffic"))
            total = true;
    }
    REQUIRE(total);

    bool has_dtr = false;
    for (QLabel* label : panel.findChildren<QLabel*>())
    {
        if (label->text().contains(QStringLiteral("DTR")))
            has_dtr = true;
    }
    REQUIRE(has_dtr);
}

namespace
{

QPushButton* find_button(QWidget& parent, const QString& text)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>())
    {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

vss::endpoint_id make_driver_port(vss::gui::FakeStudioBackend& backend)
{
    vss::gui::capabilities caps;
    caps.com_numbers = true;
    caps.release_port = true;
    caps.peer_open = true;
    backend.set_capabilities(caps);
    const auto id =
        backend.create_driver_port(QStringLiteral("meter"), true, 7);
    REQUIRE(id.has_value());
    return *id;
}

} // namespace

TEST_CASE("send: a running repeat stays on the port it started on")
{
    vss::gui::FakeStudioBackend backend;
    const auto first = backend.create_virtual_port(
        QStringLiteral("one"), vss::virtual_port_kind::pty, true);
    const auto second = backend.create_virtual_port(
        QStringLiteral("two"), vss::virtual_port_kind::pty, true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    vss::gui::SendPanel panel(backend);
    panel.set_advanced(true);
    panel.set_selected_port(*second);
    panel.set_text(QStringLiteral("ping"));
    panel.set_repeat(true, 50);
    REQUIRE(panel.repeating());
    REQUIRE_EQ(panel.repeat_port(), *second);
    REQUIRE(panel.repeat_text().contains(QStringLiteral("two")));

    // The selection moves to the other port: new sends follow it, the repeat
    // does not.
    panel.set_selected_port(*first);
    REQUIRE_EQ(panel.selected_port(), *first);
    REQUIRE(panel.target_text().contains(QStringLiteral("one")));
    REQUIRE_EQ(panel.repeat_port(), *second);
    REQUIRE(panel.repeat_text().contains(QStringLiteral("two")));

    panel.repeat_tick();
    REQUIRE_EQ(backend.sent_to().size(), 1);
    REQUIRE_EQ(backend.sent_to().back(), *second);
    panel.send_now();
    REQUIRE_EQ(backend.sent_to().back(), *first);
    panel.repeat_tick();
    REQUIRE_EQ(backend.sent_to().back(), *second);

    // Stop is offered next to the status.
    QPushButton* stop = find_button(panel, QStringLiteral("Stop repeat"));
    REQUIRE(stop != nullptr);
    stop->click();
    REQUIRE(!panel.repeating());
    REQUIRE(panel.repeat_text().isEmpty());
    const auto count = backend.sent_to().size();
    panel.repeat_tick();
    REQUIRE_EQ(backend.sent_to().size(), count);
}

TEST_CASE("send: a repeat stops when its port goes away")
{
    vss::gui::FakeStudioBackend backend;
    const auto first = backend.create_virtual_port(
        QStringLiteral("one"), vss::virtual_port_kind::pty, true);
    const auto second = backend.create_virtual_port(
        QStringLiteral("two"), vss::virtual_port_kind::pty, true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    vss::gui::SendPanel panel(backend);
    panel.set_advanced(true);
    panel.set_selected_port(*second);
    panel.set_text(QStringLiteral("ping"));
    panel.set_repeat(true, 50);
    REQUIRE(panel.repeating());

    QString error;
    QObject::connect(&panel, &vss::gui::SendPanel::error_message,
                     [&error](const QString& message) { error = message; });
    REQUIRE(backend.remove_port(*second).has_value());
    QApplication::processEvents();
    REQUIRE(!panel.repeating());
    REQUIRE(error.contains(QStringLiteral("two")));
    // It did not move to the other port.
    panel.repeat_tick();
    REQUIRE(backend.sent().empty());
}

TEST_CASE("send: a pinned target ignores the selection")
{
    vss::gui::FakeStudioBackend backend;
    const auto first = backend.create_virtual_port(
        QStringLiteral("one"), vss::virtual_port_kind::pty, true);
    const auto second = backend.create_virtual_port(
        QStringLiteral("two"), vss::virtual_port_kind::pty, true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    vss::gui::SendPanel panel(backend);
    panel.set_selected_port(*first);
    panel.set_pinned(true);
    panel.set_selected_port(*second);
    REQUIRE_EQ(panel.selected_port(), *first);
    REQUIRE(panel.target_text().contains(QStringLiteral("one")));
    // A menu action is explicit and wins over the pin.
    panel.send_from(*second);
    REQUIRE_EQ(panel.selected_port(), *second);
    REQUIRE(panel.target_text().contains(QStringLiteral("two")));
    panel.set_pinned(false);
    panel.set_selected_port(*first);
    REQUIRE_EQ(panel.selected_port(), *first);
}

TEST_CASE("send: the menu starts and stops a repeat on one port")
{
    vss::gui::FakeStudioBackend backend;
    const auto first = backend.create_virtual_port(
        QStringLiteral("one"), vss::virtual_port_kind::pty, true);
    const auto second = backend.create_virtual_port(
        QStringLiteral("two"), vss::virtual_port_kind::pty, true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    vss::gui::SendPanel panel(backend);
    // A repeat is an Advanced control.
    panel.toggle_repeat(*second);
    REQUIRE(!panel.repeating());

    panel.set_advanced(true);
    QString status;
    QObject::connect(&panel, &vss::gui::SendPanel::status_message,
                     [&status](const QString& message) { status = message; });
    panel.toggle_repeat(*second);
    // Nothing typed yet: it asks for the bytes and does not start.
    REQUIRE(!panel.repeating());
    REQUIRE(status.contains(QStringLiteral("Type the bytes")));
    REQUIRE_EQ(panel.selected_port(), *second);

    panel.set_text(QStringLiteral("ping"));
    panel.toggle_repeat(*second);
    REQUIRE_EQ(panel.repeat_port(), *second);
    panel.toggle_repeat(*first);
    REQUIRE_EQ(panel.repeat_port(), *first);
    panel.toggle_repeat(*first);
    REQUIRE(!panel.repeating());
}

TEST_CASE("fixture behaviours: the generator is bound to its fixture")
{
    vss::gui::FakeStudioBackend backend;
    const auto fixture = backend.create_mock_port(QStringLiteral("modem"),
                                                  vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    const auto other = backend.create_virtual_port(
        QStringLiteral("plain"), vss::virtual_port_kind::pty, true);
    REQUIRE(fixture.has_value());
    REQUIRE(other.has_value());

    vss::gui::FixtureBehaviours behaviours(backend);
    REQUIRE(behaviours.is_fixture(*fixture));
    REQUIRE(!behaviours.is_fixture(*other));

    QString error;
    QObject::connect(&behaviours, &vss::gui::FixtureBehaviours::error_message,
                     [&error](const QString& message) { error = message; });
    // Only a fixture has one.
    REQUIRE(!behaviours.start_generator(*other));
    REQUIRE(!error.isEmpty());

    vss::gui::generator_config config;
    config.kind = vss::gui::generator_kind::text_line;
    config.text = QStringLiteral("hi");
    config.per_second = 10;
    behaviours.set_config(*fixture, config);
    REQUIRE(behaviours.start_generator(*fixture));
    REQUIRE(behaviours.generating(*fixture));
    behaviours.tick(*fixture);
    REQUIRE_EQ(backend.sent().size(), 1);
    REQUIRE_EQ(backend.sent().front(), QByteArrayLiteral("hi\r\n"));
    REQUIRE_EQ(backend.sent_to().front(), *fixture);
    REQUIRE_EQ(behaviours.generated_bytes(*fixture), 4u);

    behaviours.stop_generator(*fixture);
    REQUIRE(!behaviours.generating(*fixture));
    behaviours.tick(*fixture);
    REQUIRE_EQ(backend.sent().size(), 1);
}

TEST_CASE("fixture behaviours: pattern, counter and random bytes")
{
    vss::gui::FakeStudioBackend backend;
    const auto fixture = backend.create_mock_port(QStringLiteral("modem"),
                                                  vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    REQUIRE(fixture.has_value());
    vss::gui::FixtureBehaviours behaviours(backend);

    QString error;
    QObject::connect(&behaviours, &vss::gui::FixtureBehaviours::error_message,
                     [&error](const QString& message) { error = message; });

    vss::gui::generator_config config;
    config.kind = vss::gui::generator_kind::fixed_pattern;
    config.text = QStringLiteral("5G");
    behaviours.set_config(*fixture, config);
    REQUIRE(!behaviours.start_generator(*fixture));
    REQUIRE(error.contains(QStringLiteral("hex")));

    config.text = QStringLiteral("55 AA");
    behaviours.set_config(*fixture, config);
    REQUIRE_EQ(behaviours.next_payload(*fixture), QByteArray::fromHex("55AA"));

    config.kind = vss::gui::generator_kind::counter;
    config.chunk = 3;
    behaviours.set_config(*fixture, config);
    REQUIRE_EQ(behaviours.next_payload(*fixture),
               QByteArray::fromHex("000102"));
    REQUIRE_EQ(behaviours.next_payload(*fixture),
               QByteArray::fromHex("030405"));

    config.kind = vss::gui::generator_kind::random_bytes;
    config.chunk = 16;
    behaviours.set_config(*fixture, config);
    REQUIRE_EQ(behaviours.next_payload(*fixture).size(), 16);
}

TEST_CASE("fixture behaviours: a removed fixture loses its generator")
{
    vss::gui::FakeStudioBackend backend;
    const auto fixture = backend.create_mock_port(QStringLiteral("modem"),
                                                  vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    REQUIRE(fixture.has_value());
    vss::gui::FixtureBehaviours behaviours(backend);
    REQUIRE(behaviours.start_generator(*fixture));
    REQUIRE(backend.remove_port(*fixture).has_value());
    QApplication::processEvents();
    REQUIRE(!behaviours.generating(*fixture));
}

TEST_CASE("fixture behaviours: echo is refused where the build has none")
{
    vss::gui::FakeStudioBackend backend;
    const auto fixture = backend.create_mock_port(QStringLiteral("modem"),
                                                  vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    REQUIRE(fixture.has_value());
    vss::gui::FixtureBehaviours behaviours(backend);

    REQUIRE(!behaviours.echo_available());
    QString error;
    QObject::connect(&behaviours, &vss::gui::FixtureBehaviours::error_message,
                     [&error](const QString& message) { error = message; });
    REQUIRE(!behaviours.set_echo(*fixture, true));
    REQUIRE(!error.isEmpty());
    REQUIRE(!behaviours.echoing(*fixture));

    vss::gui::capabilities caps;
    caps.fixture_echo = true;
    backend.set_capabilities(caps);
    REQUIRE(behaviours.echo_available());
    REQUIRE(behaviours.set_echo(*fixture, true));
    REQUIRE(behaviours.echoing(*fixture));
    behaviours.toggle_echo(*fixture);
    REQUIRE(!behaviours.echoing(*fixture));
}

TEST_CASE("fixture panel: start, stop and the degraded echo")
{
    vss::gui::FakeStudioBackend backend;
    const auto fixture = backend.create_mock_port(QStringLiteral("modem"),
                                                  vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    REQUIRE(fixture.has_value());
    vss::gui::FixtureBehaviours behaviours(backend);
    vss::gui::FixturePanel panel(behaviours);
    panel.set_port(*fixture);
    panel.show();

    // No echo in this build: shown, disabled, with the reason.
    REQUIRE(!panel.echo_box()->isEnabled());
    REQUIRE_EQ(panel.echo_box()->toolTip(),
               QStringLiteral("Not available in this build"));

    REQUIRE_EQ(panel.generator_button()->text(),
               QStringLiteral("Start generator"));
    panel.generator_button()->click();
    REQUIRE(behaviours.generating(*fixture));
    REQUIRE_EQ(panel.generator_button()->text(),
               QStringLiteral("Stop generator"));
    panel.generator_button()->click();
    REQUIRE(!behaviours.generating(*fixture));

    // The rate is written through to the behaviour.
    panel.rate_box()->setValue(20);
    REQUIRE_EQ(behaviours.config(*fixture).per_second, 20);
}

TEST_CASE("port menu: Send, Monitor, repeat and the fixture actions")
{
    vss::gui::port_menu_state plain;
    REQUIRE_EQ(vss::gui::port_menu_labels(plain),
               QStringList({QStringLiteral("Send bytes"),
                            QStringLiteral("Open in monitor")}));

    vss::gui::port_menu_state advanced;
    advanced.advanced = true;
    REQUIRE_EQ(vss::gui::port_menu_labels(advanced),
               QStringList({QStringLiteral("Send bytes"),
                            QStringLiteral("Start repeat"),
                            QStringLiteral("Open in monitor")}));
    advanced.repeating = true;
    REQUIRE(vss::gui::port_menu_labels(advanced).contains(
        QStringLiteral("Stop repeat")));

    vss::gui::port_menu_state fixture;
    fixture.advanced = true;
    fixture.fixture = true;
    REQUIRE_EQ(vss::gui::port_menu_labels(fixture),
               QStringList({QStringLiteral("Send bytes"),
                            QStringLiteral("Start repeat"),
                            QStringLiteral("Open in monitor"),
                            QStringLiteral("Start generator"),
                            QStringLiteral("Echo received bytes")}));
    fixture.generating = true;
    REQUIRE(vss::gui::port_menu_labels(fixture).contains(
        QStringLiteral("Stop generator")));

    // Echo is present but disabled when the build cannot do it.
    QMenu menu;
    vss::gui::add_port_menu_actions(&menu, &menu, fixture,
                                    [](vss::gui::port_menu_action) {});
    QAction* echo = nullptr;
    for (QAction* action : menu.actions())
    {
        if (action->text() == QStringLiteral("Echo received bytes"))
            echo = action;
    }
    REQUIRE(echo != nullptr);
    REQUIRE(!echo->isEnabled());
    REQUIRE_EQ(echo->toolTip(), QStringLiteral("Not available in this build"));
}

TEST_CASE("ports: a driver port shows both program states and nothing else")
{
    vss::gui::FakeStudioBackend backend;
    const auto id = make_driver_port(backend);

    // The backend says nothing about a program: the port is plainly open.
    auto port = vss_test::require_value(backend.port(id));
    REQUIRE(port.peer == vss::gui::peer_state::unknown);
    REQUIRE_EQ(vss::gui::PortModel::state_text(port), QStringLiteral("Open"));

    backend.set_peer_state(id, vss::gui::peer_state::not_open);
    port = vss_test::require_value(backend.port(id));
    REQUIRE_EQ(vss::gui::PortModel::state_text(port),
               QStringLiteral("Waiting for program"));
    REQUIRE(vss::gui::PortModel::state_kind(port) ==
            vss::gui::port_state_kind::waiting);

    backend.set_peer_state(id, vss::gui::peer_state::open);
    port = vss_test::require_value(backend.port(id));
    REQUIRE_EQ(vss::gui::PortModel::state_text(port),
               QStringLiteral("Program connected"));
    REQUIRE(vss::gui::PortModel::state_kind(port) ==
            vss::gui::port_state_kind::connected);
}

TEST_CASE("inspector: a driver port has a COM number that can be changed")
{
    vss::gui::FakeStudioBackend backend;
    const auto id = make_driver_port(backend);
    vss::gui::InspectorPanel panel(backend);
    vss::endpoint_id asked = 0;
    QObject::connect(&panel, &vss::gui::InspectorPanel::com_number_requested,
                     [&asked](vss::endpoint_id value) { asked = value; });
    panel.set_selected_port(id);
    panel.show();
    QApplication::processEvents();

    REQUIRE(panel.com_change_button()->isVisibleTo(&panel));
    REQUIRE(panel.com_change_button()->isEnabled());
    REQUIRE(!panel.com_change_button()->accessibleName().isEmpty());
    bool shows_number = false;
    for (QLabel* label : panel.findChildren<QLabel*>())
    {
        if (label->text() == QStringLiteral("COM7"))
            shows_number = true;
    }
    REQUIRE(shows_number);
    panel.com_change_button()->click();
    REQUIRE_EQ(asked, id);

    // Without the capability the button is there but off, with the reason.
    backend.set_capabilities({});
    panel.refresh();
    QApplication::processEvents();
    REQUIRE(!panel.com_change_button()->isEnabled());
    REQUIRE_EQ(panel.com_change_button()->toolTip(),
               QStringLiteral("Not available in this build"));

    // A port that is not a driver port has no COM number row.
    const auto other = backend.create_virtual_port(
        QStringLiteral("other"), vss::virtual_port_kind::loopback, true);
    REQUIRE(other.has_value());
    panel.set_selected_port(*other);
    QApplication::processEvents();
    REQUIRE(!panel.com_change_button()->isVisibleTo(&panel));
}

TEST_CASE("inspector: Release port is an Advanced action for a held port")
{
    vss::gui::FakeStudioBackend backend;
    const auto id = make_driver_port(backend);
    vss::gui::InspectorPanel panel(backend);
    vss::endpoint_id asked = 0;
    QObject::connect(&panel, &vss::gui::InspectorPanel::release_port_requested,
                     [&asked](vss::endpoint_id value) { asked = value; });
    panel.set_selected_port(id);
    panel.show();

    // Nobody has the port, or Advanced is off: no button.
    backend.set_peer_state(id, vss::gui::peer_state::open);
    panel.refresh();
    QApplication::processEvents();
    REQUIRE(!panel.release_button()->isVisibleTo(&panel));

    panel.set_advanced(true);
    QApplication::processEvents();
    REQUIRE(panel.release_button()->isVisibleTo(&panel));
    REQUIRE(panel.release_button()->isEnabled());
    REQUIRE(!panel.release_button()->accessibleName().isEmpty());
    panel.release_button()->click();
    REQUIRE_EQ(asked, id);

    backend.set_peer_state(id, vss::gui::peer_state::not_open);
    panel.refresh();
    QApplication::processEvents();
    REQUIRE(!panel.release_button()->isVisibleTo(&panel));

    // A port the studio cannot see into is never offered for release.
    backend.set_peer_state(id, vss::gui::peer_state::open);
    vss::gui::capabilities none;
    backend.set_capabilities(none);
    panel.refresh();
    QApplication::processEvents();
    REQUIRE(!panel.release_button()->isEnabled());
}
