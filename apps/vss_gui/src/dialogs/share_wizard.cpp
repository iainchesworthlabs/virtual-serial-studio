//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "dialogs/share_wizard.hpp"

#include "backend/share_edit.hpp"
#include "dialogs/common.hpp"
#include "dialogs/network_uri.hpp"
#include "dialogs/share_plan.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"
#include "widgets/pick_row.hpp"

#include <vss/core/profile.hpp>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{
namespace
{

StatusChip::kind chip_kind_for_peer(peer_state peer)
{
    switch (peer)
    {
    case peer_state::not_open:
        return StatusChip::kind::waiting;
    case peer_state::open:
        return StatusChip::kind::connected;
    case peer_state::unknown:
        return StatusChip::kind::healthy;
    }
    return StatusChip::kind::healthy;
}

StatusChip* state_chip(QWidget* host)
{
    if (host == nullptr)
        return nullptr;
    return dynamic_cast<StatusChip*>(
        host->findChild<QWidget*>(QStringLiteral("stateChip")));
}

QString peer_text(peer_state peer)
{
    switch (peer)
    {
    case peer_state::not_open:
        return ShareWizard::tr("Waiting for program");
    case peer_state::open:
        return ShareWizard::tr("Program connected");
    case peer_state::unknown:
        return ShareWizard::tr("Open");
    }
    return ShareWizard::tr("Open");
}

/// A device radio row with the mono path, a description and a state chip.
///
/// A click anywhere on an available row selects it, and a double-click goes
/// to the next step (`PickRow`).
class DeviceRow final : public PickRow
{
public:
    DeviceRow(const device_view& device, const QString& description,
              const QString& chip_text, StatusChip::kind chip, bool selectable,
              int index, QWidget* parent)
        : PickRow(parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(10);

        auto* radio = new QRadioButton(this);
        set_radio(radio);
        radio->setAccessibleName(ShareWizard::tr("%1, %2, %3")
                                     .arg(device.path, description, chip_text));
        radio->setEnabled(selectable);
        layout->addWidget(radio, 0, Qt::AlignVCenter);

        auto* column = new QVBoxLayout();
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        auto* path = new QLabel(device.path, this);
        path->setFont(semibold(Theme::instance().font(text_style::data)));
        auto* detail = new QLabel(description, this);
        detail->setFont(Theme::instance().font(text_style::body));
        detail->setProperty("muted", true);
        detail->setWordWrap(true);
        column->addWidget(path);
        column->addWidget(detail);
        layout->addLayout(column, 1);

        chip_ = new StatusChip(chip_text, chip, this);
        layout->addWidget(chip_, 0, Qt::AlignVCenter);
        setAccessibleName(radio->accessibleName());
        setEnabled(selectable);
        setCursor(selectable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        index_ = index;
        pass_clicks_through();
    }

    int index() const { return index_; }

private:
    StatusChip* chip_ = nullptr;
    int index_ = 0;
};

QWidget* aligned_cell(QWidget* widget, Qt::Alignment alignment, QWidget* parent)
{
    auto* host = new QWidget(parent);
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(0);
    if (alignment & Qt::AlignRight)
        layout->addStretch(1);
    layout->addWidget(widget);
    if (!(alignment & Qt::AlignRight))
        layout->addStretch(1);
    return host;
}

QTableWidgetItem* mono_item(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setFont(Theme::instance().font(text_style::data));
    item->setForeground(Theme::instance().colour(colour_role::ink));
    return item;
}

} // namespace

ShareWizard::ShareWizard(StudioBackend& backend, bool advanced, QWidget* parent)
    : QDialog(parent), backend_(backend), advanced_(advanced)
{
    const int configured_baud =
        QSettings().value(QStringLiteral("ui/defaultBaud"), 115200).toInt();
    if (configured_baud >= 50 && configured_baud <= 40000000)
        settings_.baud_rate = static_cast<unsigned>(configured_baud);
    build();
    refresh_devices();
    set_step(0);
}

void ShareWizard::build()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.build");
    VSS_PROFILE_MESSAGE("gui.dialog.share_wizard.opened");
    prepare_dialog(this, tr("Share a device with several programs"), 760, 700);
    // prepare_dialog fixes a minimum height.  Drop it so the layout's own
    // minimum applies: the page scrolls and the footer is always on screen.
    setMinimumSize(640, 0);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 0);
    outer->setSpacing(12);

    auto* header = new QVBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(4);
    title_ =
        make_dialog_title(tr("Share a device with several programs"), this);
    subtitle_ = make_dialog_subtitle(
        tr("Studio opens the device once, then gives each program its own "
           "port."),
        this);
    header->addWidget(title_);
    header->addWidget(subtitle_);
    outer->addLayout(header);

    stepper_ = new Stepper(this);
    stepper_->set_steps({tr("Device"), tr("Programs"), tr("Ready")});
    outer->addWidget(stepper_);

    error_ = new InlineError(this);
    outer->addWidget(error_);

    pages_ = new QStackedWidget(this);
    pages_->addWidget(build_device_page());
    pages_->addWidget(build_programs_page());
    pages_->addWidget(build_ready_page());

    // The pages scroll, so a small screen or large text never pushes the
    // footer buttons out of the dialog.
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("shareScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setFocusPolicy(Qt::NoFocus);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->verticalScrollBar()->setAccessibleName(tr("Scroll"));
    scroll->horizontalScrollBar()->setAccessibleName(tr("Scroll"));
    scroll->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: 0; }"));
    scroll->setWidget(pages_);
    pages_->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    outer->addWidget(scroll, 1);

    build_footer();

    connect(&backend_, &StudioBackend::ports_changed, this,
            [this] { refresh_step3_states(); });
    connect(&backend_, &StudioBackend::links_changed, this,
            [this] { refresh_step3_states(); });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { update_warning(); });
}

QWidget* ShareWizard::build_device_page()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    // Header row: the question on the left, a Refresh button on the right.
    auto* which = new QHBoxLayout();
    which->setContentsMargins(0, 0, 0, 0);
    which->setSpacing(8);
    which->addWidget(make_field_label(tr("Which device?"), page));
    which->addStretch(1);
    refresh_ = make_dialog_action_button(tr("Refresh"), "quiet", page);
    refresh_->setProperty("size", QStringLiteral("small"));
    refresh_->setIcon(IconProvider::instance().icon(
        QStringLiteral("refresh"), colour_role::accentStrong, 16));
    polish(refresh_);
    refresh_->setAccessibleName(tr("Refresh the device list"));
    connect(refresh_, &QPushButton::clicked, this,
            [this] { refresh_devices(); });
    which->addWidget(refresh_);
    layout->addLayout(which);

    // Scrolling list of devices; update_device_list() fills device_layout_
    // with one row per device.
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumHeight(150);
    scroll->setMaximumHeight(220);
    scroll->setAccessibleName(tr("Devices"));
    auto* card = new QFrame(scroll);
    card->setProperty("card", true);
    polish(card);
    device_layout_ = new QVBoxLayout(card);
    device_layout_->setContentsMargins(0, 0, 0, 0);
    device_layout_->setSpacing(0);
    device_card_ = card;
    scroll->setWidget(card);
    layout->addWidget(scroll);

    // Shown instead of the list when no serial device can be chosen.
    no_devices_ = new NoteBanner(
        NoteBanner::kind::warn,
        tr("No serial device is available. Plug one in, then choose Refresh."),
        page);
    no_devices_->hide();
    layout->addWidget(no_devices_);

    // Serial settings card: a one-line summary with a Change toggle that
    // reveals the full editor.
    auto* settings_card = new QFrame(page);
    settings_card->setProperty("card", true);
    polish(settings_card);
    auto* settings_layout = new QVBoxLayout(settings_card);
    settings_layout->setContentsMargins(14, 12, 14, 12);
    settings_layout->setSpacing(8);
    auto* summary_row = new QHBoxLayout();
    summary_row->setContentsMargins(0, 0, 0, 0);
    summary_row->setSpacing(10);
    auto* summary_column = new QVBoxLayout();
    summary_column->setContentsMargins(0, 0, 0, 0);
    summary_column->setSpacing(0);
    auto* summary_title = new QLabel(tr("Device settings"), settings_card);
    summary_title->setFont(semibold(Theme::instance().font(text_style::body)));
    settings_summary_ = new QLabel(settings_card);
    settings_summary_->setFont(Theme::instance().font(text_style::body));
    settings_summary_->setProperty("muted", true);
    settings_summary_->setWordWrap(true);
    summary_column->addWidget(summary_title);
    summary_column->addWidget(settings_summary_);
    summary_row->addLayout(summary_column, 1);
    change_settings_ =
        make_dialog_action_button(tr("Change"), "quiet", settings_card);
    change_settings_->setProperty("size", QStringLiteral("small"));
    polish(change_settings_);
    change_settings_->setCheckable(true);
    change_settings_->setAccessibleName(tr("Change device settings"));
    summary_row->addWidget(change_settings_, 0, Qt::AlignVCenter);
    settings_layout->addLayout(summary_row);

    settings_panel_ = new QWidget(settings_card);
    auto* grid = new QGridLayout(settings_panel_);
    grid->setContentsMargins(0, 4, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    // Common baud rates are offered, but any rate in range can be typed.
    baud_ = new QComboBox(settings_panel_);
    baud_->setEditable(true);
    baud_->addItems({QStringLiteral("9600"), QStringLiteral("19200"),
                     QStringLiteral("38400"), QStringLiteral("57600"),
                     QStringLiteral("115200"), QStringLiteral("230400"),
                     QStringLiteral("460800"), QStringLiteral("921600")});
    baud_->setCurrentText(QString::number(settings_.baud_rate));
    baud_->setValidator(new QIntValidator(50, 40000000, baud_));
    data_bits_ = new QComboBox(settings_panel_);
    for (int bits = 5; bits <= 8; ++bits)
        data_bits_->addItem(QString::number(bits), bits);
    data_bits_->setCurrentIndex(
        data_bits_->findData(static_cast<int>(settings_.data_bits)));
    parity_ = new QComboBox(settings_panel_);
    parity_->addItem(tr("None"), static_cast<int>(vss::parity_mode::none));
    parity_->addItem(tr("Odd"), static_cast<int>(vss::parity_mode::odd));
    parity_->addItem(tr("Even"), static_cast<int>(vss::parity_mode::even));
    parity_->addItem(tr("Mark"), static_cast<int>(vss::parity_mode::mark));
    parity_->addItem(tr("Space"), static_cast<int>(vss::parity_mode::space));
    parity_->setCurrentIndex(
        parity_->findData(static_cast<int>(settings_.parity)));
    stop_bits_ = new QComboBox(settings_panel_);
    stop_bits_->addItem(QStringLiteral("1"),
                        static_cast<int>(vss::stop_bits_mode::one));
    stop_bits_->addItem(QStringLiteral("1.5"),
                        static_cast<int>(vss::stop_bits_mode::one_point_five));
    stop_bits_->addItem(QStringLiteral("2"),
                        static_cast<int>(vss::stop_bits_mode::two));
    stop_bits_->setCurrentIndex(
        stop_bits_->findData(static_cast<int>(settings_.stop_bits)));
    flow_ = new QComboBox(settings_panel_);
    flow_->addItem(tr("None"), static_cast<int>(vss::flow_control_mode::none));
    flow_->addItem(tr("Hardware (RTS and CTS)"),
                   static_cast<int>(vss::flow_control_mode::hardware));
    flow_->addItem(tr("Software (XON and XOFF)"),
                   static_cast<int>(vss::flow_control_mode::software));
    flow_->setCurrentIndex(
        flow_->findData(static_cast<int>(settings_.flow_control)));
    // Lay the five settings out in a grid; flow control spans both columns.
    grid->addWidget(new FormRow(tr("Baud rate"), baud_, {}, settings_panel_), 0,
                    0);
    grid->addWidget(
        new FormRow(tr("Data bits"), data_bits_, {}, settings_panel_), 0, 1);
    grid->addWidget(new FormRow(tr("Parity"), parity_, {}, settings_panel_), 1,
                    0);
    grid->addWidget(
        new FormRow(tr("Stop bits"), stop_bits_, {}, settings_panel_), 1, 1);
    grid->addWidget(new FormRow(tr("Flow control"), flow_, {}, settings_panel_),
                    2, 0, 1, 2);
    settings_layout->addWidget(settings_panel_);
    settings_panel_->hide();
    layout->addWidget(settings_card);
    update_settings_summary();

    // Copy every combo back into settings_ whenever one changes. A baud rate
    // that does not parse keeps the previous value.
    const auto read_settings = [this]
    {
        bool ok = false;
        const unsigned baud = baud_->currentText().trimmed().toUInt(&ok);
        if (ok)
            settings_.baud_rate = baud;
        settings_.data_bits =
            static_cast<unsigned>(data_bits_->currentData().toInt());
        settings_.parity =
            static_cast<vss::parity_mode>(parity_->currentData().toInt());
        settings_.stop_bits =
            static_cast<vss::stop_bits_mode>(stop_bits_->currentData().toInt());
        settings_.flow_control =
            static_cast<vss::flow_control_mode>(flow_->currentData().toInt());
        update_settings_summary();
        update_step();
    };
    connect(change_settings_, &QPushButton::toggled, this,
            [this](bool open) { settings_panel_->setVisible(open); });
    connect(baud_, &QComboBox::currentTextChanged, this,
            [read_settings](const QString&) { read_settings(); });
    connect(data_bits_, &QComboBox::currentIndexChanged, this,
            [read_settings](int) { read_settings(); });
    connect(parity_, &QComboBox::currentIndexChanged, this,
            [read_settings](int) { read_settings(); });
    connect(stop_bits_, &QComboBox::currentIndexChanged, this,
            [read_settings](int) { read_settings(); });
    connect(flow_, &QComboBox::currentIndexChanged, this,
            [read_settings](int) { read_settings(); });

    // Advanced option: use a network device instead of a local serial one.
    auto* network_row = new QFrame(page);
    network_row->setProperty("dashed", true);
    polish(network_row);
    auto* network_layout = new QHBoxLayout(network_row);
    network_layout->setContentsMargins(12, 8, 12, 8);
    network_layout->setSpacing(8);
    auto* chevron = new QLabel(network_row);
    chevron->setPixmap(
        IconProvider::instance()
            .icon(QStringLiteral("chevron-down"), colour_role::ink2, 16)
            .pixmap(16, 16));
    network_layout->addWidget(chevron);
    network_toggle_ = make_dialog_action_button(tr("A network device instead"),
                                                "quiet", network_row);
    network_toggle_->setAccessibleName(tr("A network device instead"));
    network_layout->addWidget(network_toggle_);
    network_layout->addWidget(new Badge(tr("ADVANCED"), network_row));
    network_layout->addStretch(1);
    auto* network_hint =
        new QLabel(tr("TCP, UDP or RFC 2217 address."), network_row);
    network_hint->setFont(Theme::instance().font(text_style::caption));
    network_hint->setProperty("muted", true);
    network_layout->addWidget(network_hint);
    layout->addWidget(network_row);
    // Choosing a network device needs advanced mode, so switch it on and tell
    // the owner before toggling.
    connect(network_toggle_, &QPushButton::clicked, this,
            [this]
            {
                if (!advanced_)
                {
                    set_advanced(true);
                    Q_EMIT advanced_requested();
                }
                network_mode_ = !network_mode_;
                update_network_row();
                update_step();
            });

    // Address entry, shown only in network mode; typing clears its error.
    network_panel_ = new QWidget(page);
    auto* network_panel_layout = new QVBoxLayout(network_panel_);
    network_panel_layout->setContentsMargins(0, 0, 0, 0);
    network_panel_layout->setSpacing(6);
    network_address_ = new QLineEdit(network_panel_);
    network_address_->setPlaceholderText(
        QStringLiteral("tcp://192.168.1.50:4001"));
    network_row_ = new FormRow(
        tr("Device address"), network_address_,
        tr("The address of the serial server, using TCP, UDP or RFC 2217."),
        network_panel_);
    network_panel_layout->addWidget(network_row_);
    network_panel_->hide();
    layout->addWidget(network_panel_);

    auto* info = new NoteBanner(NoteBanner::kind::info, {}, page);
    info->setObjectName(QStringLiteral("shareStep1Info"));
    layout->addWidget(info);
    connect(network_address_, &QLineEdit::textChanged, this,
            [this](const QString&)
            {
                network_row_->clear_error();
                update_step();
            });
    layout->addStretch(1);

    // The note text follows the selected device; keep a handle through the
    // object name and update it from update_step().
    Q_UNUSED(info);
    return page;
}

void ShareWizard::update_step1_info()
{
    auto* info = findChild<NoteBanner*>(QStringLiteral("shareStep1Info"));
    if (info == nullptr)
        return;
    const QString name = source_name();
    const QString rich =
        tr("Your programs will open the ports Studio creates, not %1 itself. "
           "Next you choose how many programs there are. You can add more "
           "afterwards.")
            .arg(bold_name(name));
    step1_info_plain_ =
        tr("Your programs will open the ports Studio creates, not %1 itself. "
           "Next you choose how many programs there are. You can add more "
           "afterwards.")
            .arg(name);
    info->set_text(rich, step1_info_plain_);
}

QWidget* ShareWizard::build_programs_page()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    // Count row: a minus/plus spinner with the upper limit beside it.
    auto* count_row = new QHBoxLayout();
    count_row->setContentsMargins(0, 0, 0, 0);
    count_row->setSpacing(10);
    count_row->addWidget(make_field_label(tr("How many programs?"), page));
    auto* spin = new QFrame(page);
    spin->setProperty("spin", true);
    polish(spin);
    auto* spin_layout = new QHBoxLayout(spin);
    spin_layout->setContentsMargins(0, 0, 0, 0);
    spin_layout->setSpacing(0);
    auto* minus = new QToolButton(spin);
    minus->setText(QStringLiteral("-"));
    minus->setAccessibleName(tr("Fewer programs"));
    minus->setFixedSize(34, 34);
    auto* plus = new QToolButton(spin);
    plus->setText(QStringLiteral("+"));
    plus->setAccessibleName(tr("More programs"));
    plus->setFixedSize(34, 34);
    programs_value_ = new QLabel(QStringLiteral("3"), spin);
    programs_value_->setObjectName(QStringLiteral("shareProgramCount"));
    programs_value_->setFont(
        semibold(Theme::instance().font(text_style::data)));
    programs_value_->setAlignment(Qt::AlignCenter);
    programs_value_->setFixedWidth(44);
    spin_layout->addWidget(minus);
    spin_layout->addWidget(programs_value_);
    spin_layout->addWidget(plus);
    count_row->addWidget(spin);
    auto* limit = new QLabel(tr("Up to 32"), page);
    limit->setFont(Theme::instance().font(text_style::caption));
    limit->setProperty("muted", true);
    count_row->addWidget(limit);
    count_row->addStretch(1);
    layout->addLayout(count_row);
    auto* later = new QLabel(
        tr("You can add another program after sharing, and remove any "
           "program's port later with Remove port."),
        page);
    later->setObjectName(QStringLiteral("shareCountLater"));
    later->setFont(Theme::instance().font(text_style::caption));
    later->setProperty("muted", true);
    later->setWordWrap(true);
    layout->addWidget(later);
    // Both buttons hand the new count to set_programs().
    connect(minus, &QToolButton::clicked, this,
            [this] { set_programs(programs_ - 1); });
    connect(plus, &QToolButton::clicked, this,
            [this] { set_programs(programs_ + 1); });

    // Read-only table of what each program will open; update_programs_table()
    // fills it in.
    layout->addWidget(
        make_field_label(tr("What each program will open"), page));
    programs_table_ = new QTableWidget(0, 3, page);
    programs_table_->setObjectName(QStringLiteral("shareProgramsTable"));
    programs_table_->setAccessibleName(tr("What each program will open"));
    programs_table_->setHorizontalHeaderLabels(
        {tr("Program name"), tr("Port to open"),
         tr("Can other programs open it?")});
    programs_table_->verticalHeader()->hide();
    programs_table_->setSelectionMode(QAbstractItemView::NoSelection);
    programs_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    programs_table_->setShowGrid(false);
    programs_table_->setAlternatingRowColors(false);
    programs_table_->verticalHeader()->setDefaultSectionSize(40);
    // The name column takes the spare width; the other two stay fixed.
    programs_table_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    programs_table_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Fixed);
    programs_table_->horizontalHeader()->resizeSection(1, 160);
    programs_table_->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Fixed);
    programs_table_->horizontalHeader()->resizeSection(2, 210);
    programs_table_->setMinimumHeight(160);
    programs_table_->setMaximumHeight(360);
    programs_table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft |
                                                             Qt::AlignVCenter);
    auto* table_card = new QFrame(page);
    table_card->setProperty("card", true);
    polish(table_card);
    auto* table_card_layout = new QVBoxLayout(table_card);
    table_card_layout->setContentsMargins(0, 0, 0, 0);
    table_card_layout->setSpacing(0);
    table_card_layout->addWidget(programs_table_);
    layout->addWidget(table_card);

    // Warning panel with two ways out; update_warning() decides when it shows.
    warn_note_ = new QFrame(page);
    warn_note_->setProperty("note", QStringLiteral("warn"));
    warn_note_->setFrameShape(QFrame::NoFrame);
    auto* warn_layout = new QVBoxLayout(warn_note_);
    warn_layout->setContentsMargins(10, 8, 10, 8);
    warn_layout->setSpacing(8);
    auto* warn_row = new QHBoxLayout();
    warn_row->setContentsMargins(0, 0, 0, 0);
    warn_row->setSpacing(8);
    auto* warn_icon = new QLabel(warn_note_);
    warn_icon->setPixmap(
        IconProvider::instance()
            .icon(QStringLiteral("warning"), colour_role::amber, 16)
            .pixmap(16, 16));
    warn_row->addWidget(warn_icon, 0, Qt::AlignTop);
    warn_text_ = new QLabel(warn_note_);
    warn_text_->setWordWrap(true);
    warn_text_->setTextFormat(Qt::RichText);
    warn_text_->setFont(Theme::instance().font(text_style::body));
    warn_row->addWidget(warn_text_, 1);
    warn_layout->addLayout(warn_row);
    auto* warn_buttons = new QHBoxLayout();
    warn_buttons->setContentsMargins(24, 0, 0, 0);
    warn_buttons->setSpacing(8);
    create_pair_ = make_dialog_action_button(tr("Create another pair"), "quiet",
                                             warn_note_);
    create_pair_->setProperty("size", QStringLiteral("small"));
    polish(create_pair_);
    keep_only_ = make_dialog_action_button(tr("Keep the third as Studio only"),
                                           "quiet", warn_note_);
    keep_only_->setProperty("size", QStringLiteral("small"));
    polish(keep_only_);
    warn_buttons->addWidget(create_pair_);
    warn_buttons->addWidget(keep_only_);
    warn_buttons->addStretch(1);
    warn_layout->addLayout(warn_buttons);
    // Each button resolves the warning; their results are not needed here.
    connect(create_pair_, &QPushButton::clicked, this,
            [this] { static_cast<void>(create_another_pair()); });
    connect(keep_only_, &QPushButton::clicked, this,
            [this] { static_cast<void>(acknowledge_studio_only()); });
    layout->addWidget(warn_note_);
    warn_note_->hide();

    // Step 2 banner; update_warning() sets its text.
    share_info_ = new NoteBanner(NoteBanner::kind::info, {}, page);
    share_info_->setObjectName(QStringLiteral("shareStep2Info"));
    layout->addWidget(share_info_);
    layout->addStretch(1);
    return page;
}

QWidget* ShareWizard::build_ready_page()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    ready_table_ = new QTableWidget(0, 4, page);
    ready_table_->setObjectName(QStringLiteral("shareReadyTable"));
    ready_table_->setAccessibleName(tr("Shared ports"));
    ready_table_->setHorizontalHeaderLabels(
        {tr("Program"), tr("Open this port"), tr("State"), {}});
    ready_table_->verticalHeader()->hide();
    ready_table_->setSelectionMode(QAbstractItemView::NoSelection);
    ready_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ready_table_->setShowGrid(false);
    ready_table_->verticalHeader()->setDefaultSectionSize(42);
    ready_table_->horizontalHeader()->setSectionResizeMode(0,
                                                           QHeaderView::Fixed);
    ready_table_->horizontalHeader()->resizeSection(0, 150);
    ready_table_->horizontalHeader()->setSectionResizeMode(1,
                                                           QHeaderView::Fixed);
    ready_table_->horizontalHeader()->resizeSection(1, 190);
    ready_table_->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch);
    ready_table_->horizontalHeader()->setSectionResizeMode(3,
                                                           QHeaderView::Fixed);
    ready_table_->horizontalHeader()->resizeSection(3, 150);
    ready_table_->setMinimumHeight(160);
    ready_table_->setMaximumHeight(420);
    ready_table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft |
                                                          Qt::AlignVCenter);
    auto* table_card = new QFrame(page);
    table_card->setProperty("card", true);
    polish(table_card);
    auto* table_card_layout = new QVBoxLayout(table_card);
    table_card_layout->setContentsMargins(0, 0, 0, 0);
    table_card_layout->setSpacing(0);
    table_card_layout->addWidget(ready_table_);
    layout->addWidget(table_card);

    ready_note_ = new NoteBanner(NoteBanner::kind::ok, {}, page);
    ready_note_->setObjectName(QStringLiteral("shareStep3Note"));
    layout->addWidget(ready_note_);
    layout->addStretch(1);
    return page;
}

void ShareWizard::build_footer()
{
    QHBoxLayout* layout = nullptr;
    QFrame* footer = make_dialog_footer(this, layout);

    add_program_ =
        make_dialog_action_button(tr("Add another program"), "quiet", footer);
    connect(add_program_, &QPushButton::clicked, this,
            [this] { static_cast<void>(add_another_program()); });
    layout->insertWidget(0, add_program_);

    back_ = make_dialog_action_button(tr("Back"), nullptr, footer);
    connect(back_, &QPushButton::clicked, this,
            [this] { static_cast<void>(back()); });
    layout->addWidget(back_);

    cancel_ = make_dialog_action_button(tr("Cancel"), nullptr, footer);
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancel_);

    next_ = make_dialog_action_button(tr("Next"), "primary", footer);
    connect(next_, &QPushButton::clicked, this,
            [this] { static_cast<void>(next()); });
    layout->addWidget(next_);

    primary_ = make_dialog_action_button(tr("Share device"), "primary", footer);
    connect(primary_, &QPushButton::clicked, this,
            [this] { static_cast<void>(share()); });
    layout->addWidget(primary_);

    open_monitor_ =
        make_dialog_action_button(tr("Open monitor"), nullptr, footer);
    connect(open_monitor_, &QPushButton::clicked, this,
            [this]
            {
                Q_EMIT open_monitor_requested();
                accept();
            });
    layout->addWidget(open_monitor_);

    done_ = make_dialog_action_button(tr("Done"), "primary", footer);
    connect(done_, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(done_);

    auto* outer = qobject_cast<QVBoxLayout*>(this->layout());
    outer->addWidget(footer);
}

void ShareWizard::refresh_devices()
{
    devices_ = backend_.devices();
    update_device_list();
    update_warning();
}

void ShareWizard::update_device_list()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.update_device_list");
    // Rebuild the list from scratch: drop the old rows and radio group first.
    while (QLayoutItem* item = device_layout_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    delete device_group_;
    device_group_ = new QButtonGroup(this);
    device_group_->setExclusive(true);

    // Paths Studio already uses; those devices cannot be shared again.
    QStringList claimed;
    for (const port_view& port : backend_.ports())
        claimed << port.path;

    // Whether any device can be chosen, and the first one that can.
    bool has_available = false;
    QString first_available;
    for (int index = 0; index < static_cast<int>(devices_.size()); ++index)
    {
        const device_view& device =
            devices_.at(static_cast<std::size_t>(index));
        QString description = device.description;
        QString chip_text;
        StatusChip::kind chip = StatusChip::kind::healthy;
        bool selectable = true;

        // Classify each device. Virtual ports and devices Studio already
        // uses are listed but cannot be selected.
        if (device.com0com)
        {
            description = tr("com0com pair end. This is a virtual port, not a "
                             "device.");
            chip_text = tr("Virtual");
            chip = StatusChip::kind::studio_only;
            selectable = false;
        }
        else if (device.virtual_device)
        {
            description = tr("This is a virtual port, not a device.");
            chip_text = tr("Virtual");
            chip = StatusChip::kind::studio_only;
            selectable = false;
        }
        else if (claimed.contains(device.path, Qt::CaseInsensitive))
        {
            description = tr("Serial device. Studio is already using it.");
            chip_text = tr("In use");
            chip = StatusChip::kind::waiting;
            selectable = false;
        }
        else
        {
            description = tr("Serial device");
            chip_text = tr("Available");
        }

        // Note the first choosable device, to use as the default.
        if (selectable)
        {
            has_available = true;
            if (first_available.isEmpty())
                first_available = device.path;
        }

        auto* row = new DeviceRow(device, description, chip_text, chip,
                                  selectable, index, device_card_);
        device_layout_->addWidget(row);
        // Only selectable rows join the radio group; activating one goes
        // straight to the next step.
        if (selectable)
        {
            device_group_->addButton(row->radio(), index);
            connect(row, &PickRow::activated, this,
                    [this] { static_cast<void>(next()); });
        }
        // Ticking a radio chooses that device, unless a network address is
        // in use.
        if (row->radio()->isEnabled())
        {
            connect(
                row->radio(), &QRadioButton::toggled, this,
                [this, index](bool checked)
                {
                    if (!checked || network_mode_)
                        return;
                    if (index >= 0 && index < static_cast<int>(devices_.size()))
                    {
                        selected_device_ =
                            devices_.at(static_cast<std::size_t>(index)).path;
                        network_row_->clear_error();
                        clear_error();
                        update_step1_info();
                        update_step();
                    }
                });
        }
    }

    // With nothing to choose, clear the selection and show the empty note.
    if (!has_available)
    {
        selected_device_.clear();
        no_devices_->show();
    }
    else
    {
        no_devices_->hide();
        // Keep the earlier choice if it is still a free real device; otherwise
        // fall back to the first available one.
        bool found = false;
        for (QAbstractButton* button : device_group_->buttons())
        {
            if (button->isChecked())
                found = true;
        }
        const bool selected_still_available = [this]()
        {
            for (const device_view& device : devices_)
            {
                if (device.path == selected_device_ && !device.com0com &&
                    !device.virtual_device)
                    return true;
            }
            return false;
        }();
        if (!found || !selected_still_available)
        {
            for (QAbstractButton* button : device_group_->buttons())
            {
                const int index = device_group_->id(button);
                if (index >= 0 && index < static_cast<int>(devices_.size()) &&
                    devices_.at(static_cast<std::size_t>(index)).path ==
                        first_available)
                {
                    // Block signals so the default choice does not run the
                    // toggled handler.
                    const QSignalBlocker blocker(button);
                    button->setChecked(true);
                    selected_device_ = std::move(first_available);
                    break;
                }
            }
        }
    }

    if (advanced_)
        set_advanced(true);
    // Refresh the info banner and step state for the final selection.
    update_step1_info();
    update_step();
}

void ShareWizard::update_network_row()
{
    const bool active = network_mode_;
    network_panel_->setVisible(active);
    network_toggle_->setText(active ? tr("Use a device instead")
                                    : tr("A network device instead"));
    network_toggle_->setAccessibleName(network_toggle_->text());
    device_card_->setEnabled(!active);
    refresh_->setEnabled(!active);
    if (!active)
    {
        network_row_->clear_error();
        network_address_->clear();
    }
    update_step1_info();
}

void ShareWizard::update_settings_summary()
{
    settings_summary_->setText(serial_settings_summary(settings_));
}

QString ShareWizard::device_settings_summary() const
{
    return settings_summary_ == nullptr ? QString() : settings_summary_->text();
}

void ShareWizard::set_serial_settings(const vss::serial_settings& settings)
{
    const QSignalBlocker baud_blocker(baud_);
    const QSignalBlocker data_blocker(data_bits_);
    const QSignalBlocker parity_blocker(parity_);
    const QSignalBlocker stop_blocker(stop_bits_);
    const QSignalBlocker flow_blocker(flow_);
    settings_ = settings;
    if (baud_ != nullptr)
        baud_->setCurrentText(QString::number(settings_.baud_rate));
    if (data_bits_ != nullptr)
        data_bits_->setCurrentIndex(
            data_bits_->findData(static_cast<int>(settings_.data_bits)));
    if (parity_ != nullptr)
        parity_->setCurrentIndex(
            parity_->findData(static_cast<int>(settings_.parity)));
    if (stop_bits_ != nullptr)
        stop_bits_->setCurrentIndex(
            stop_bits_->findData(static_cast<int>(settings_.stop_bits)));
    if (flow_ != nullptr)
        flow_->setCurrentIndex(
            flow_->findData(static_cast<int>(settings_.flow_control)));
    update_settings_summary();
    update_step();
}

void ShareWizard::set_network_source(const QString& uri)
{
    advanced_ = true;
    network_mode_ = true;
    network_address_->setText(uri);
    update_network_row();
    update_step();
}

void ShareWizard::update_step()
{
    const bool has_source = [this]
    {
        if (network_mode_)
            return parse_network_uri(network_address_->text()).has_value();
        return !selected_device_.isEmpty();
    }();

    if (step_ == 0)
    {
        title_->setText(tr("Share a device with several programs"));
        subtitle_->setText(
            tr("Studio opens the device once, then gives each program its own "
               "port."));
        update_step1_info();
    }
    else if (step_ == 1)
    {
        title_->setText(
            tr("Share %1 with several programs").arg(source_name()));
        subtitle_->setText(
            tr("Each program gets its own port. Studio copies the device's "
               "data to all of them."));
        update_warning();
    }
    else
    {
        title_->setText(step3_title_);
        subtitle_->setText(tr("Open one of these ports in each program."));
    }

    if (next_ != nullptr)
        next_->setEnabled(has_source);
    if (primary_ != nullptr)
        primary_->setEnabled(has_source && backend_.caps().share);
    update_footer();
}

void ShareWizard::update_footer()
{
    const bool source_step = step_ == 0;
    const bool programs_step = step_ == 1;
    const bool ready_step = step_ == 2;

    if (next_ != nullptr)
        next_->setVisible(source_step);
    if (back_ != nullptr)
        back_->setVisible(programs_step);
    if (cancel_ != nullptr)
        cancel_->setVisible(!ready_step);
    if (primary_ != nullptr)
        primary_->setVisible(programs_step);
    if (add_program_ != nullptr)
        add_program_->setVisible(ready_step);
    if (open_monitor_ != nullptr)
        open_monitor_->setVisible(ready_step);
    if (done_ != nullptr)
        done_->setVisible(ready_step);

    QPushButton* default_button = nullptr;
    if (source_step)
        default_button = next_;
    else if (programs_step)
        default_button = primary_;
    else
        default_button = done_;
    for (QPushButton* button : {next_, primary_, done_})
    {
        if (button != nullptr)
            button->setDefault(button == default_button);
    }
}

void ShareWizard::set_step(int step)
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.set_step");
    step_ = std::clamp(step, 0, 2);
    pages_->setCurrentIndex(step_);
    stepper_->set_current(step_);
    update_step();
    fit_to_screen(760, step_ == 1 ? 760 : (step_ == 2 ? 640 : 700));
    QTimer::singleShot(
        0, this,
        [this]
        {
            if (step_ == 0)
            {
                if (network_mode_)
                    network_address_->setFocus(Qt::OtherFocusReason);
                else if (!device_group_->buttons().isEmpty())
                    device_group_->buttons().first()->setFocus(
                        Qt::OtherFocusReason);
            }
            else if (step_ == 1)
            {
                auto* edit =
                    qobject_cast<QLineEdit*>(programs_table_->cellWidget(0, 0));
                if (edit != nullptr)
                    edit->setFocus(Qt::OtherFocusReason);
            }
        });
}

void ShareWizard::fit_to_screen(int width, int height)
{
    const QScreen* screen = parentWidget() != nullptr
                                ? parentWidget()->screen()
                                : QGuiApplication::primaryScreen();
    if (screen != nullptr)
    {
        const QRect available = screen->availableGeometry();
        // Leave room for the title bar and the taskbar.
        height = std::min(height, std::max(320, available.height() - 80));
        width =
            std::min(width, std::max(minimumWidth(), available.width() - 40));
    }
    resize(width, height);
}

int ShareWizard::current_step() const
{
    return step_;
}

bool ShareWizard::next()
{
    if (!validate_source())
        return false;
    clear_error();
    set_step(1);
    return true;
}

bool ShareWizard::back()
{
    if (step_ == 1)
    {
        set_step(0);
        return true;
    }
    return false;
}

bool ShareWizard::validate_source()
{
    if (network_mode_)
    {
        QString error;
        const auto parsed = parse_network_uri(network_address_->text(), &error);
        if (!parsed)
        {
            network_row_->set_error(error);
            return false;
        }
        network_row_->clear_error();
        return true;
    }
    if (selected_device_.isEmpty())
    {
        no_devices_->show();
        return false;
    }
    return true;
}

QString ShareWizard::source_name() const
{
    if (network_mode_)
    {
        const auto parsed = parse_network_uri(network_address_->text());
        return parsed ? parsed->host : network_address_->text().trimmed();
    }
    return selected_device_.isEmpty() ? tr("the device") : selected_device_;
}

QString ShareWizard::source_uri() const
{
    if (network_mode_)
        return network_address_->text().trimmed();
    return serial_uri(selected_device_, settings_);
}

QString ShareWizard::program_default(int index) const
{
    switch (index)
    {
    case 0:
        return tr("terminal");
    case 1:
        return tr("test-tool");
    case 2:
        return tr("logger");
    default:
        return tr("program-%1").arg(index + 1);
    }
}

QStringList ShareWizard::program_names() const
{
    QStringList names;
    for (int row = 0; row < programs_table_->rowCount(); ++row)
    {
        QString value;
        if (auto* edit =
                qobject_cast<QLineEdit*>(programs_table_->cellWidget(row, 0)))
        {
            value = edit->text().trimmed();
        }
        names << (value.isEmpty() ? program_default(row) : value);
    }
    return names;
}

void ShareWizard::update_programs_table()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.update_programs_table");
    if (programs_table_ == nullptr)
        return;
    const QStringList previous = program_names();
    const QStringList paths = planned_paths();

    const QSignalBlocker blocker(programs_table_);
    programs_table_->setRowCount(programs_);
    for (int row = 0; row < programs_; ++row)
    {
        auto* edit =
            qobject_cast<QLineEdit*>(programs_table_->cellWidget(row, 0));
        if (edit == nullptr)
        {
            edit = new QLineEdit(programs_table_);
            edit->setAccessibleName(tr("Name of program %1").arg(row + 1));
            edit->setMinimumHeight(30);
            programs_table_->setCellWidget(row, 0, edit);
        }
        const QString name =
            row < previous.size() ? previous.at(row) : program_default(row);
        if (edit->text() != name)
            edit->setText(name);
        edit->setAccessibleName(tr("Name of program %1").arg(row + 1));

        programs_table_->setItem(row, 1, mono_item(paths.value(row)));

        auto* chip = state_chip(programs_table_->cellWidget(row, 2));
        const bool openable = row < plan_.openable;
        const QString chip_text = openable ? tr("Yes") : tr("No, Studio only");
        if (chip == nullptr)
        {
            chip = new StatusChip(chip_text,
                                  openable ? StatusChip::kind::healthy
                                           : StatusChip::kind::waiting,
                                  programs_table_);
            chip->setObjectName(QStringLiteral("stateChip"));
            programs_table_->setCellWidget(
                row, 2, aligned_cell(chip, Qt::AlignLeft, programs_table_));
        }
        else
        {
            chip->set_text(chip_text);
            chip->set_kind(openable ? StatusChip::kind::healthy
                                    : StatusChip::kind::waiting);
        }
    }
}

void ShareWizard::update_warning()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.update_warning");
    if (warn_note_ == nullptr)
        return;
    const setup_facts facts = backend_.facts();
    const bool windows = windows_platform(facts);
    free_pair_paths_ = free_com0com_pair_paths(devices_, backend_.ports());
    if (windows && facts.driver_found)
    {
        // Driver ports are made on demand, so the com0com free-pair count
        // does not apply.
        int held = 0;
        for (const port_view& port : backend_.ports())
        {
            if (port.kind == vss::endpoint_kind::virtual_port &&
                !port.studio_only)
                ++held;
        }
        free_pair_paths_.clear();
        plan_ = make_driver_share_plan(programs_, held);
    }
    else
    {
        plan_ = make_share_plan(
            programs_, static_cast<int>(free_pair_paths_.size()), windows);
    }
    const bool show = plan_.needs_warning() && !acknowledged_;
    warn_text_->setText(share_warning_rich(plan_));
    warn_text_->setAccessibleName(share_warning_plain(plan_));
    warn_note_->setAccessibleName(
        tr("Warning: %1").arg(share_warning_plain(plan_)));
    warn_note_->setVisible(show);

    if (create_pair_ != nullptr)
    {
        const bool capable = backend_.caps().com0com_pair_creation;
        // More pairs cannot raise the Studio driver's per-user limit.
        create_pair_->setVisible(!plan_.studio_driver);
        create_pair_->setEnabled(capable);
        create_pair_->setToolTip(
            capable ? tr("Windows will ask for administrator permission.")
                    : tr("Not available in this build"));
    }
    if (keep_only_ != nullptr)
    {
        keep_only_->setText(plan_.studio_only == 1
                                ? tr("Keep the %1 as Studio only")
                                      .arg(ordinal_word(plan_.openable + 1))
                                : tr("Keep the remaining %1 as Studio only")
                                      .arg(plan_.studio_only));
    }

    const QString name = source_name();
    if (programs_ == 1)
    {
        share_info_plain_ =
            tr("Everything %1 receives is copied to 1 port. Whatever the "
               "program writes goes back to %1.")
                .arg(name);
        share_info_->set_text(
            tr("Everything %1 receives is copied to 1 port. Whatever the "
               "program writes goes back to %1.")
                .arg(bold_name(name)),
            share_info_plain_);
    }
    else
    {
        share_info_plain_ =
            tr("Everything %1 receives is copied to all %2 ports. Whatever a "
               "program writes goes back to %1.")
                .arg(name)
                .arg(programs_);
        share_info_->set_text(
            tr("Everything %1 receives is copied to all %2 ports. Whatever a "
               "program writes goes back to %1.")
                .arg(bold_name(name))
                .arg(programs_),
            share_info_plain_);
    }

    update_programs_table();
    if (programs_value_ != nullptr)
        programs_value_->setText(QString::number(programs_));
}

QStringList ShareWizard::planned_paths() const
{
    return planned_share_paths(plan_, free_pair_paths_);
}

void ShareWizard::set_programs(int programs)
{
    const int clamped = std::clamp(programs, 1, 32);
    if (clamped == programs_)
        return;
    programs_ = clamped;
    acknowledged_ = false;
    clear_error();
    update_warning();
    update_step();
}

bool ShareWizard::acknowledge_studio_only()
{
    acknowledged_ = true;
    update_warning();
    return true;
}

bool ShareWizard::create_another_pair()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.create_another_pair");
    if (!backend_.caps().com0com_pair_creation)
        return false;
    clear_error();
    const auto created = backend_.create_com0com_pair();
    if (!created)
    {
        last_error_ = error_text(created.error());
        error_->show_error(
            tr("Could not create another pair"),
            tr("Windows did not create the driver pair. Allow administrator "
               "permission, then try again."),
            last_error_);
        return false;
    }
    acknowledged_ = false;
    devices_ = backend_.devices();
    update_device_list();
    update_warning();
    update_step();
    return true;
}

void ShareWizard::undo_share()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.undo_share");
    if (shared_.link != 0)
    {
        const auto disconnected = backend_.disconnect(shared_.link);
        if (!disconnected && disconnected.error() != vss::errc::not_found)
            last_error_ = error_text(disconnected.error());
    }
    for (const vss::endpoint_id id : listener_ids_)
    {
        const auto removed = backend_.remove_port(id);
        if (!removed && removed.error() != vss::errc::not_found)
            last_error_ = error_text(removed.error());
    }
    if (shared_.source != 0)
    {
        const auto removed = backend_.remove_port(shared_.source);
        if (!removed && removed.error() != vss::errc::not_found)
            last_error_ = error_text(removed.error());
    }
    shared_ = {};
    listener_ids_.clear();
    shared_programs_.clear();
}

bool ShareWizard::share()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.share");
    if (step_ != 1)
        return false;
    if (!validate_source())
    {
        set_step(0);
        return false;
    }
    if (!backend_.caps().share)
    {
        error_->show_error(tr("Sharing is not available in this build"),
                           tr("Nothing was changed."));
        return false;
    }

    clear_error();
    const QStringList names = program_names();
    const bool replacing = shared_.source != 0;
    if (replacing)
        undo_share();

    sharing_ = true;
    const auto result = backend_.share_serial(source_uri(), programs_);
    sharing_ = false;
    if (!result)
    {
        last_error_ = error_text(result.error());
        error_->show_error(
            tr("Could not share %1").arg(source_name()),
            replacing ? tr("The previous share was removed. Check the device, "
                           "then try again.")
                      : tr("The setup is unchanged. Check the device, then try "
                           "again."),
            last_error_);
        return false;
    }

    shared_ = *result;
    listener_ids_.clear();
    for (const vss::endpoint_id id : shared_.listeners)
        listener_ids_.push_back(id);
    shared_programs_ = names;
    step3_title_ = shared_title(programs_);
    set_step(2);
    update_ready_table();
    update_ready_note();
    return true;
}

QString ShareWizard::shared_title(int programs) const
{
    return programs == 1 ? tr("%1 is shared with 1 program").arg(source_name())
                         : tr("%1 is shared with %2 programs")
                               .arg(source_name())
                               .arg(programs);
}

bool ShareWizard::add_another_program()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.add_program");
    if (step_ != 2 || shared_.source == 0)
        return false;
    if (listener_ids_.size() >= 32)
    {
        error_->show_error(
            tr("Studio can share a device with at most 32 programs"),
            tr("Remove a program's port first if you need a different one."));
        return false;
    }

    // The share stays as it is. The device and the programs' ports are not
    // touched, so a program that has its port open keeps it.
    clear_error();
    share_ref share;
    share.link = shared_.link;
    share.device = shared_.source;
    share.programs = listener_ids_;
    const auto added = vss::gui::add_program_port(
        backend_, share, next_program_name(backend_.ports()), true);
    if (!added)
    {
        last_error_ = error_text(added.error());
        error_->show_error(
            tr("Could not add another program"),
            tr("The share is as it was. Check the device, then try again."),
            last_error_);
        return false;
    }

    shared_.link = added->link;
    shared_.listeners.push_back(added->port);
    listener_ids_.push_back(added->port);
    shared_programs_ << program_default(static_cast<int>(listener_ids_.size()) -
                                        1);
    programs_ = static_cast<int>(listener_ids_.size());
    step3_title_ = shared_title(programs_);
    update_step();
    update_ready_table();
    update_ready_note();
    return true;
}

bool ShareWizard::accept_share()
{
    if (step_ != 2)
        return false;
    accept();
    return true;
}

int ShareWizard::listener_count() const
{
    return static_cast<int>(listener_ids_.size());
}

QString ShareWizard::row_program(int index) const
{
    if (ready_table_ == nullptr || index < 0 ||
        index >= ready_table_->rowCount())
    {
        return {};
    }
    QTableWidgetItem* item = ready_table_->item(index, 0);
    return item == nullptr ? QString() : item->text();
}

QString ShareWizard::row_path(int index) const
{
    if (ready_table_ == nullptr || index < 0 ||
        index >= ready_table_->rowCount())
    {
        return {};
    }
    QTableWidgetItem* item = ready_table_->item(index, 1);
    return item == nullptr ? QString() : item->text();
}

QString ShareWizard::row_state(int index) const
{
    if (ready_table_ == nullptr || index < 0 ||
        index >= ready_table_->rowCount())
    {
        return {};
    }
    QWidget* host = ready_table_->cellWidget(index, 2);
    StatusChip* chip = state_chip(host);
    return chip == nullptr ? QString() : chip->text();
}

void ShareWizard::update_ready_table()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.update_ready_table");
    if (ready_table_ == nullptr)
        return;
    ready_table_->setRowCount(static_cast<int>(listener_ids_.size()));
    for (int row = 0; row < listener_ids_.size(); ++row)
    {
        const vss::endpoint_id id = listener_ids_.at(row);
        const auto port = backend_.port(id);
        const QString program =
            shared_programs_.value(row, program_default(row));
        const QString path = port ? port->path : QString();
        const bool studio_only = port ? port->studio_only : true;
        const peer_state peer = port ? port->peer : peer_state::unknown;
        const QString state = studio_only ? tr("Studio only") : peer_text(peer);

        auto* program_item = new QTableWidgetItem(program);
        program_item->setFlags(program_item->flags() & ~Qt::ItemIsEditable);
        program_item->setFont(
            semibold(Theme::instance().font(text_style::body)));
        ready_table_->setItem(row, 0, program_item);

        auto* path_item = mono_item(path);
        path_item->setFont(semibold(Theme::instance().font(text_style::data)));
        ready_table_->setItem(row, 1, path_item);

        auto* chip = new StatusChip(state,
                                    studio_only ? StatusChip::kind::studio_only
                                                : chip_kind_for_peer(peer),
                                    ready_table_);
        chip->setObjectName(QStringLiteral("stateChip"));
        ready_table_->setCellWidget(
            row, 2, aligned_cell(chip, Qt::AlignLeft, ready_table_));

        if (studio_only)
        {
            auto* label =
                new QLabel(tr("Not for other programs"), ready_table_);
            label->setFont(Theme::instance().font(text_style::caption));
            label->setProperty("muted", true);
            ready_table_->setCellWidget(
                row, 3, aligned_cell(label, Qt::AlignRight, ready_table_));
        }
        else
        {
            auto* copy =
                make_dialog_action_button(tr("Copy"), "quiet", ready_table_);
            copy->setProperty("size", QStringLiteral("small"));
            copy->setIcon(IconProvider::instance().icon(
                QStringLiteral("copy"), colour_role::accentStrong, 16));
            polish(copy);
            copy->setAccessibleName(tr("Copy %1").arg(path));
            copy->setToolTip(copy->accessibleName());
            connect(copy, &QPushButton::clicked, this,
                    [this, path]
                    {
                        QGuiApplication::clipboard()->setText(path);
                        ToastManager::instance().show(
                            this, tr("%1 copied.").arg(path));
                    });
            ready_table_->setCellWidget(
                row, 3, aligned_cell(copy, Qt::AlignRight, ready_table_));
        }
    }
}

void ShareWizard::update_ready_note()
{
    VSS_PROFILE_ZONE("gui.dialogs.share_wizard.update_ready_note");
    if (ready_note_ == nullptr)
        return;
    const QString name = source_name();
    QString plain;
    if (listener_ids_.size() == 1)
    {
        plain = tr("Everything %1 receives is now copied to 1 port.").arg(name);
    }
    else
    {
        plain = tr("Everything %1 receives is now copied to all %2 ports.")
                    .arg(name)
                    .arg(listener_ids_.size());
    }

    bool waiting_known = true;
    bool any_outside = false;
    for (const vss::endpoint_id id : listener_ids_)
    {
        const auto port = backend_.port(id);
        if (!port || port->studio_only)
            continue;
        any_outside = true;
        if (port->peer == peer_state::unknown)
            waiting_known = false;
    }
    waiting_known = waiting_known && any_outside;
    if (waiting_known)
    {
        plain += QLatin1Char(' ');
        plain += tr("The Waiting labels change to Program connected as each "
                    "program opens their port.");
    }
    done_note_plain_ = plain;

    QString rich;
    if (listener_ids_.size() == 1)
    {
        rich = tr("Everything %1 receives is now copied to 1 port.")
                   .arg(bold_name(name));
    }
    else
    {
        rich = tr("Everything %1 receives is now copied to all %2 ports.")
                   .arg(bold_name(name))
                   .arg(listener_ids_.size());
    }
    if (waiting_known)
    {
        rich += QLatin1Char(' ');
        rich += tr("The Waiting labels change to Program connected as each "
                   "program opens their port.");
    }
    ready_note_->set_text(rich, plain);
}

void ShareWizard::refresh_step3_states()
{
    if (step_ != 2 || sharing_)
        return;
    for (int row = 0; row < listener_ids_.size(); ++row)
    {
        const auto port = backend_.port(listener_ids_.at(row));
        QWidget* host = ready_table_->cellWidget(row, 2);
        StatusChip* chip = state_chip(host);
        if (chip == nullptr || !port)
            continue;
        const QString state =
            port->studio_only ? tr("Studio only") : peer_text(port->peer);
        chip->set_text(state);
        chip->set_kind(port->studio_only ? StatusChip::kind::studio_only
                                         : chip_kind_for_peer(port->peer));
    }
    update_ready_note();
}

QString ShareWizard::warning_text() const
{
    return share_warning_plain(plan_);
}

bool ShareWizard::warning_visible() const
{
    return warn_note_ != nullptr && !warn_note_->isHidden();
}

QString ShareWizard::share_info_text() const
{
    return share_info_plain_;
}

QString ShareWizard::step1_info_text() const
{
    return step1_info_plain_;
}

QString ShareWizard::done_note_text() const
{
    return done_note_plain_;
}

QString ShareWizard::step3_title() const
{
    return step3_title_;
}

bool ShareWizard::primary_enabled() const
{
    if (step_ == 0)
        return next_ != nullptr && next_->isEnabled();
    if (step_ == 1)
        return primary_ != nullptr && primary_->isEnabled();
    return done_ != nullptr && done_->isEnabled();
}

bool ShareWizard::error_visible() const
{
    return error_ != nullptr && error_->is_showing();
}

void ShareWizard::set_advanced(bool advanced)
{
    advanced_ = advanced;
}

void ShareWizard::clear_error()
{
    last_error_.clear();
    if (error_ != nullptr)
        error_->clear();
}

} // namespace vss::gui
