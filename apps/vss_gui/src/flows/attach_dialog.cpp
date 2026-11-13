//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/attach_dialog.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"
#include "widgets/pick_row.hpp"

#include <vss/core/profile.hpp>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <utility>
#include <vector>

namespace vss::gui
{
namespace
{

QString parity_letter(vss::parity_mode parity)
{
    switch (parity)
    {
    case vss::parity_mode::even:
        return QStringLiteral("E");
    case vss::parity_mode::odd:
        return QStringLiteral("O");
    case vss::parity_mode::mark:
        return QStringLiteral("M");
    case vss::parity_mode::space:
        return QStringLiteral("S");
    case vss::parity_mode::none:
        break;
    }
    return QStringLiteral("N");
}

QString stop_digits(vss::stop_bits_mode stop)
{
    switch (stop)
    {
    case vss::stop_bits_mode::two:
        return QStringLiteral("2");
    case vss::stop_bits_mode::one_point_five:
        return QStringLiteral("1.5");
    case vss::stop_bits_mode::one:
        break;
    }
    return QStringLiteral("1");
}

} // namespace

AttachDialog::AttachDialog(StudioBackend& backend, bool advanced,
                           platform_kind platform, QWidget* parent)
    : FlowDialog(
          tr("Attach a real device"),
          tr("Studio holds the real device open. Your programs connect to a "
             "new virtual port that Studio creates, and Studio passes the "
             "data between the two."),
          parent),
      backend_(backend), platform_(platform), advanced_(advanced)
{
    build();

    auto* cancel = add_footer_button(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* attach =
        add_footer_button(tr("Attach device"), QStringLiteral("primary"));
    connect(attach, &QPushButton::clicked, this, &AttachDialog::attach);

    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    refresh_devices();
    update_primary_state();
    finish_layout(800);

    QWidget* first_field = name_;
    for (const auto& row : rows_)
    {
        if (row.available && row.radio->isChecked())
        {
            first_field = row.radio;
            break;
        }
    }
    focus_first(first_field);

    QWidget::setTabOrder(refresh_, name_);
    QWidget::setTabOrder(name_, capture_);
    QWidget::setTabOrder(capture_, cancel);
    QWidget::setTabOrder(cancel, attach);
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this] { refresh_row_styles(); });
}

void AttachDialog::build()
{
    VSS_PROFILE_ZONE("gui.flows.attach.build");
    VSS_PROFILE_MESSAGE("gui.dialog.attach.opened");
    build_device_section();
    build_settings_section();

    name_ = new QLineEdit(QStringLiteral("device"), this);
    name_->setObjectName(QStringLiteral("attachName"));
    name_->setAccessibleName(tr("Name"));
    name_row_ = new FormRow(tr("Name"), name_, {}, this);
    body_layout()->addWidget(name_row_);
    capture_ = new QCheckBox(tr("Capture traffic in the monitor"), this);
    capture_->setObjectName(QStringLiteral("attachCapture"));
    capture_->setChecked(true);
    capture_->setAccessibleName(tr("Capture traffic in the monitor"));
    body_layout()->addWidget(capture_, 0, Qt::AlignLeft);

    result_ = new ResultSentence(this);
    result_->setObjectName(QStringLiteral("attachResult"));
    body_layout()->addWidget(result_);

    auto* address_header = new QHBoxLayout();
    auto* address_label = new QLabel(tr("Address"), this);
    address_label->setFont(
        flow_semibold(Theme::instance().font(text_style::caption)));
    address_header->addWidget(address_label);
    address_header->addStretch(1);
    if (advanced_)
    {
        edit_address_ = new QPushButton(tr("Edit address"), this);
        edit_address_->setProperty("role", QStringLiteral("quiet"));
        edit_address_->setProperty("size", QStringLiteral("small"));
        edit_address_->setAccessibleName(tr("Edit address"));
        edit_address_->setCursor(Qt::PointingHandCursor);
        polish(edit_address_);
        address_header->addWidget(edit_address_);
    }
    body_layout()->addLayout(address_header);

    address_ = new CodeBlock(this);
    address_->setObjectName(QStringLiteral("attachAddress"));
    body_layout()->addWidget(address_);

    address_edit_ = new QLineEdit(this);
    address_edit_->setObjectName(QStringLiteral("attachAddressEdit"));
    address_edit_->setFont(Theme::instance().font(text_style::data));
    address_edit_->setAccessibleName(tr("Address"));
    address_row_ = new FormRow(tr("Address"), address_edit_, {}, this);
    address_row_->setVisible(false);
    body_layout()->addWidget(address_row_);

    if (edit_address_ != nullptr)
    {
        connect(edit_address_, &QPushButton::clicked, this,
                [this]
                {
                    const bool showing = address_row_->isHidden();
                    address_row_->setVisible(showing);
                    address_->setVisible(!showing);
                    if (showing)
                    {
                        address_edit_->setText(current_uri());
                        focus_first(address_edit_);
                    }
                    else
                    {
                        update_address();
                    }
                });
    }

    connect(name_, &QLineEdit::textChanged, this,
            [this]
            {
                name_row_->clear_error();
                update_primary_state();
                update_result();
            });
    connect(capture_, &QCheckBox::toggled, this,
            [this] { update_primary_state(); });
    connect(address_edit_, &QLineEdit::textChanged, this,
            [this]
            {
                update_primary_state();
                update_result();
            });

    update_address();
    update_result();
    update_primary_state();
}

void AttachDialog::build_device_section()
{
    auto* header = new QHBoxLayout();
    auto* label = new QLabel(tr("Device"), this);
    label->setFont(flow_semibold(Theme::instance().font(text_style::caption)));
    header->addWidget(label);
    header->addStretch(1);
    refresh_ = new QPushButton(tr("Refresh"), this);
    refresh_->setObjectName(QStringLiteral("attachRefresh"));
    refresh_->setProperty("size", QStringLiteral("small"));
    refresh_->setAccessibleName(tr("Refresh"));
    refresh_->setIcon(IconProvider::instance().icon(QStringLiteral("refresh"),
                                                    colour_role::ink, 14));
    refresh_->setCursor(Qt::PointingHandCursor);
    polish(refresh_);
    header->addWidget(refresh_);
    body_layout()->addLayout(header);

    device_card_ = new QFrame(this);
    device_card_->setObjectName(QStringLiteral("attachDeviceList"));
    device_card_->setProperty("card", true);
    polish(device_card_);
    rows_layout_ = new QVBoxLayout(device_card_);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(0);
    body_layout()->addWidget(device_card_);

    empty_note_ = new NoteBanner(
        NoteBanner::kind::info,
        tr("No serial devices found. Plug one in, then choose Refresh."), this);
    empty_note_->setObjectName(QStringLiteral("attachEmpty"));
    empty_note_->setVisible(false);
    body_layout()->addWidget(empty_note_);

    auto* hint =
        new QLabel(tr("The list is read when this window opens. Plug a "
                      "device in, then choose Refresh."),
                   this);
    hint->setFont(Theme::instance().font(text_style::caption));
    hint->setProperty("muted", true);
    hint->setWordWrap(true);
    body_layout()->addWidget(hint);

    auto* manual_line = new QHBoxLayout();
    manual_toggle_ = new QPushButton(tr("Enter a path instead"), this);
    manual_toggle_->setProperty("role", QStringLiteral("quiet"));
    manual_toggle_->setProperty("size", QStringLiteral("small"));
    manual_toggle_->setAccessibleName(tr("Enter a path instead"));
    manual_toggle_->setCursor(Qt::PointingHandCursor);
    polish(manual_toggle_);
    manual_line->addWidget(manual_toggle_);
    manual_line->addStretch(1);
    body_layout()->addLayout(manual_line);

    manual_path_ = new QLineEdit(this);
    manual_path_->setObjectName(QStringLiteral("attachManualPath"));
    manual_path_->setFont(Theme::instance().font(text_style::data));
    manual_path_->setPlaceholderText(tr("COM3 or /dev/ttyUSB0"));
    manual_path_->setAccessibleName(tr("Device path"));
    manual_row_ = new FormRow(tr("Device path"), manual_path_,
                              tr("For example, COM3 or /dev/ttyUSB0."), this);
    manual_row_->setVisible(false);
    body_layout()->addWidget(manual_row_);

    connect(manual_toggle_, &QPushButton::clicked, this,
            [this]
            {
                manual_row_->setVisible(true);
                manual_toggle_->setVisible(false);
                focus_first(manual_path_);
            });
    connect(manual_path_, &QLineEdit::textChanged, this,
            [this]
            {
                manual_row_->clear_error();
                if (!selecting_ && !manual_path_->text().trimmed().isEmpty())
                    clear_device_selection();
                update_primary_state();
                update_address();
                update_result();
            });
    connect(refresh_, &QPushButton::clicked, this,
            &AttachDialog::refresh_devices);
}

void AttachDialog::build_settings_section()
{
    // A one-line summary, shown while the settings are collapsed.
    settings_summary_ = new QLabel(this);
    settings_summary_->setObjectName(QStringLiteral("attachSettingsSummary"));
    settings_summary_->setFont(Theme::instance().font(text_style::caption));
    settings_summary_->setProperty("muted", true);
    settings_summary_->setWordWrap(true);
    body_layout()->addWidget(settings_summary_);

    // The settings sit in a collapsible section, open by default only in
    // advanced mode.
    settings_ = new Disclosure(tr("Device settings"), this);
    settings_->setObjectName(QStringLiteral("attachSettings"));
    auto* content = new QWidget(settings_);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(10);

    // A shortcut that resets every setting to the common 115200 8N1 values
    // with no flow control.
    auto* defaults_line = new QHBoxLayout();
    defaults_line->addStretch(1);
    auto* defaults = new QPushButton(tr("Use common defaults"), content);
    defaults->setProperty("role", QStringLiteral("quiet"));
    defaults->setProperty("size", QStringLiteral("small"));
    defaults->setAccessibleName(tr("Use common defaults"));
    defaults->setCursor(Qt::PointingHandCursor);
    polish(defaults);
    defaults_line->addWidget(defaults);
    content_layout->addLayout(defaults_line);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);

    // Each combo starts on 115200 baud, 8 data bits, no parity, one stop bit
    // and no flow control.
    baud_ = new QComboBox(content);
    baud_->setObjectName(QStringLiteral("attachBaud"));
    const int bauds[] = {9600, 19200, 38400, 57600, 115200, 230400};
    for (const int baud : bauds)
        baud_->addItem(QString::number(baud), baud);
    baud_->setCurrentIndex(4);
    baud_->setAccessibleName(tr("Baud rate"));
    grid->addWidget(new FormRow(tr("Baud rate"), baud_, {}, content), 0, 0);

    data_bits_ = new QComboBox(content);
    data_bits_->setObjectName(QStringLiteral("attachDataBits"));
    data_bits_->addItem(QStringLiteral("7"), 7);
    data_bits_->addItem(QStringLiteral("8"), 8);
    data_bits_->setCurrentIndex(1);
    data_bits_->setAccessibleName(tr("Data bits"));
    grid->addWidget(new FormRow(tr("Data bits"), data_bits_, {}, content), 0,
                    1);

    parity_ = new QComboBox(content);
    parity_->setObjectName(QStringLiteral("attachParity"));
    parity_->addItem(tr("None"), static_cast<int>(vss::parity_mode::none));
    parity_->addItem(tr("Even"), static_cast<int>(vss::parity_mode::even));
    parity_->addItem(tr("Odd"), static_cast<int>(vss::parity_mode::odd));
    parity_->setAccessibleName(tr("Parity"));
    grid->addWidget(new FormRow(tr("Parity"), parity_, {}, content), 0, 2);

    stop_bits_ = new QComboBox(content);
    stop_bits_->setObjectName(QStringLiteral("attachStopBits"));
    stop_bits_->addItem(QStringLiteral("1"),
                        static_cast<int>(vss::stop_bits_mode::one));
    stop_bits_->addItem(QStringLiteral("2"),
                        static_cast<int>(vss::stop_bits_mode::two));
    stop_bits_->setAccessibleName(tr("Stop bits"));
    grid->addWidget(new FormRow(tr("Stop bits"), stop_bits_, {}, content), 1,
                    0);

    flow_ = new QComboBox(content);
    flow_->setObjectName(QStringLiteral("attachFlow"));
    flow_->addItem(tr("None"), static_cast<int>(vss::flow_control_mode::none));
    flow_->addItem(tr("Hardware (RTS and CTS)"),
                   static_cast<int>(vss::flow_control_mode::hardware));
    flow_->addItem(tr("Software (XON and XOFF)"),
                   static_cast<int>(vss::flow_control_mode::software));
    flow_->setAccessibleName(tr("Flow control"));
    grid->addWidget(new FormRow(tr("Flow control"), flow_, {}, content), 1, 1,
                    1, 2);
    content_layout->addLayout(grid);

    auto* hint = new QLabel(
        tr("Most devices use these settings. If you see garbled text, check "
           "the device's manual."),
        content);
    hint->setFont(Theme::instance().font(text_style::caption));
    hint->setProperty("muted", true);
    hint->setWordWrap(true);
    content_layout->addWidget(hint);
    settings_->set_content(content);
    body_layout()->addWidget(settings_);

    // The summary stands in for the full settings while they are collapsed.
    settings_->set_expanded(advanced_);
    settings_summary_->setVisible(!advanced_);
    connect(settings_, &Disclosure::expanded_changed, this,
            [this](bool expanded)
            {
                settings_summary_->setVisible(!expanded);
                update_summary();
            });
    // Reset to the same values the combos start with.
    connect(defaults, &QPushButton::clicked, this,
            [this]
            {
                baud_->setCurrentIndex(4);
                data_bits_->setCurrentIndex(1);
                parity_->setCurrentIndex(0);
                stop_bits_->setCurrentIndex(0);
                flow_->setCurrentIndex(0);
                update_summary();
            });
    // Any setting change refreshes the summary, the address and the result.
    const auto changed = [this]
    {
        update_summary();
        update_address();
        update_result();
    };
    connect(baud_, &QComboBox::currentIndexChanged, this, changed);
    connect(data_bits_, &QComboBox::currentIndexChanged, this, changed);
    connect(parity_, &QComboBox::currentIndexChanged, this, changed);
    connect(stop_bits_, &QComboBox::currentIndexChanged, this, changed);
    connect(flow_, &QComboBox::currentIndexChanged, this, changed);
    update_summary();
}

void AttachDialog::refresh_devices()
{
    VSS_PROFILE_ZONE("gui.flows.attach.refresh_devices");
    backend_.refresh();
    rebuild_rows();
}

void AttachDialog::rebuild_rows()
{
    VSS_PROFILE_ZONE("gui.flows.attach.rebuild_rows");
    // Suppress selection handling while the rows are rebuilt; selecting_ is
    // cleared again at the end.
    selecting_ = true;
    // Discard the old group and rows before listing the devices again.
    if (group_ != nullptr)
        group_->deleteLater();
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);

    while (QLayoutItem* item = rows_layout_->takeAt(0))
    {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    rows_.clear();
    selected_ = -1;

    // One row per device. Virtual ports and devices another program holds are
    // listed but cannot be selected.
    const auto devices = backend_.devices();
    for (int index = 0; index < static_cast<int>(devices.size()); ++index)
    {
        const device_view& device = devices.at(static_cast<std::size_t>(index));
        const bool is_virtual = device.com0com || device.virtual_device;
        const bool available = !is_virtual && !device.in_use;

        // Wording and chip for each kind of device, most specific kind first.
        QString description;
        QString chip_text;
        StatusChip::kind chip_kind = StatusChip::kind::healthy;
        if (device.com0com)
        {
            description =
                tr("com0com pair end. This is a virtual port, not a device.");
            chip_text = tr("Virtual");
            chip_kind = StatusChip::kind::studio_only;
        }
        else if (device.virtual_device)
        {
            description = tr("This is a virtual port, not a device.");
            chip_text = tr("Virtual");
            chip_kind = StatusChip::kind::studio_only;
        }
        else if (device.in_use)
        {
            description = tr("Serial device. Another program is using it.");
            chip_text = tr("In use");
            chip_kind = StatusChip::kind::waiting;
        }
        else
        {
            description = tr("Serial device");
            chip_text = tr("Available");
            chip_kind = StatusChip::kind::healthy;
        }

        // Build the row: radio button, path, description and status chip.
        auto* frame = new PickRow(device_card_);
        frame->setObjectName(QStringLiteral("attachDeviceRow"));
        frame->setCursor(available ? Qt::PointingHandCursor : Qt::ArrowCursor);
        auto* row_layout = new QHBoxLayout(frame);
        row_layout->setContentsMargins(14, 10, 14, 10);
        row_layout->setSpacing(12);

        auto* radio = new QRadioButton(frame);
        frame->set_radio(radio);
        radio->setObjectName(QStringLiteral("attachDeviceRadio"));
        radio->setAccessibleName(tr("%1, %2").arg(device.path, description));
        radio->setEnabled(available);
        radio->setMinimumSize(24, 24);
        group_->addButton(radio, index);
        row_layout->addWidget(radio, 0, Qt::AlignVCenter);

        auto* path = new QLabel(device.path, frame);
        path->setFont(flow_semibold(Theme::instance().font(text_style::data)));
        path->setAccessibleName(device.path);
        row_layout->addWidget(path, 0, Qt::AlignVCenter);

        auto* text = new QLabel(description, frame);
        text->setFont(Theme::instance().font(text_style::body));
        text->setWordWrap(true);
        if (!available)
            text->setProperty("muted", true);
        if (!device.description.isEmpty())
            frame->setToolTip(device.description);
        row_layout->addWidget(text, 1);

        auto* chip = new StatusChip(chip_text, chip_kind, frame);
        row_layout->addWidget(chip, 0, Qt::AlignVCenter);

        // A click anywhere on the row selects it, and a double-click moves on
        // to the next field.
        frame->pass_clicks_through();
        rows_layout_->addWidget(frame);
        rows_.push_back(device_row{frame, radio, device.path, available});
        connect(frame, &PickRow::activated, this,
                [this]
                {
                    name_->setFocus(Qt::OtherFocusReason);
                    name_->selectAll();
                });

        connect(radio, &QRadioButton::toggled, this,
                [this, index](bool checked)
                {
                    if (checked && !selecting_)
                        select_device(index);
                    refresh_row_styles();
                });
    }
    rows_layout_->addStretch(1);

    // With no devices, show the empty note instead of the list.
    const bool empty = rows_.isEmpty();
    empty_note_->setVisible(empty);
    device_card_->setVisible(!empty);
    refresh_row_styles();

    // Preselect the first available device. selecting_ stops this from
    // running select_device() for each toggle.
    selecting_ = true;
    for (int index = 0; index < rows_.size(); ++index)
    {
        if (rows_.at(index).available)
        {
            rows_.at(index).radio->setChecked(true);
            selected_ = index;
            break;
        }
    }
    selecting_ = false;
    // Bring the primary button, address and result up to date for the new
    // selection.
    update_primary_state();
    update_address();
    update_result();
}

void AttachDialog::select_device(int index)
{
    if (index < 0 || index >= rows_.size() || !rows_.at(index).available)
        return;
    selected_ = index;
    if (!manual_path_->text().isEmpty())
    {
        const QSignalBlocker blocker(manual_path_);
        manual_path_->clear();
    }
    refresh_row_styles();
    update_primary_state();
    update_address();
    update_result();
}

void AttachDialog::clear_device_selection()
{
    if (group_ == nullptr)
        return;
    group_->setExclusive(false);
    for (auto& row : rows_)
    {
        if (row.radio->isChecked())
            row.radio->setChecked(false);
    }
    group_->setExclusive(true);
    selected_ = -1;
    refresh_row_styles();
}

void AttachDialog::refresh_row_styles()
{
    for (int index = 0; index < rows_.size(); ++index)
    {
        const auto& row = rows_.at(index);
        const bool selected = index == selected_;
        row.frame->setStyleSheet(
            QStringLiteral("QFrame#attachDeviceRow { background: %1; "
                           "border-bottom: 1px solid %2; }")
                .arg(Theme::instance()
                         .colour(selected ? colour_role::accentSoft
                                          : colour_role::panel)
                         .name(QColor::HexRgb),
                     Theme::instance()
                         .colour(colour_role::line)
                         .name(QColor::HexRgb)));
    }
}

void AttachDialog::update_result()
{
    VSS_PROFILE_ZONE("gui.flows.attach.update_result");
    if (result_ == nullptr)
        return;
    const QString path = current_device_path();
    if (path.isEmpty())
    {
        result_->set_result_text(tr("Choose a device to attach."));
        return;
    }
    const vss::serial_settings serial = current_settings();
    result_->set_result_text(
        tr("Studio will hold <b>%1</b> open at %2 baud. Your programs should "
           "not open %1. They open the new virtual port that Studio creates, "
           "and Studio passes data between that port and %1.")
            .arg(path.toHtmlEscaped())
            .arg(serial.baud_rate),
        tr("Studio will hold %1 open at %2 baud. Your programs should not "
           "open %1. They open the new virtual port that Studio creates, and "
           "Studio passes data between that port and %1.")
            .arg(path)
            .arg(serial.baud_rate));
}

void AttachDialog::update_summary()
{
    if (settings_summary_ == nullptr)
        return;
    const vss::serial_settings serial = current_settings();
    QString flow;
    switch (serial.flow_control)
    {
    case vss::flow_control_mode::hardware:
        flow = tr("hardware flow control");
        break;
    case vss::flow_control_mode::software:
        flow = tr("software flow control");
        break;
    case vss::flow_control_mode::none:
        flow = tr("no flow control");
        break;
    }
    settings_summary_->setText(tr("%1 %2%3%4, %5")
                                   .arg(serial.baud_rate)
                                   .arg(serial.data_bits)
                                   .arg(parity_letter(serial.parity))
                                   .arg(stop_digits(serial.stop_bits))
                                   .arg(flow));
}

void AttachDialog::update_primary_state()
{
    bool enabled = selected_ >= 0 || !manual_path_->text().trimmed().isEmpty();
    if (address_row_ != nullptr && !address_row_->isHidden())
        enabled = !address_edit_->text().trimmed().isEmpty();
    enabled = enabled && name_ != nullptr && !name_->text().trimmed().isEmpty();
    set_primary_enabled(enabled);
}

void AttachDialog::update_address()
{
    if (address_ == nullptr)
        return;
    if (address_row_ != nullptr && !address_row_->isHidden())
        return;
    address_->set_text(current_uri());
}

QString AttachDialog::current_device_path() const
{
    if (address_row_ != nullptr && !address_row_->isHidden())
    {
        QString address = address_edit_->text().trimmed();
        if (address.isEmpty())
            return {};
        if (address.startsWith(QStringLiteral("serial://")))
            return address.mid(9).section(QLatin1Char('?'), 0, 0);
        return address;
    }
    QString path =
        manual_path_ != nullptr ? manual_path_->text().trimmed() : QString();
    if (path.startsWith(QStringLiteral("serial://")))
        path = path.mid(9).section(QLatin1Char('?'), 0, 0);
    if (!path.isEmpty())
        return path;
    if (selected_ >= 0 && selected_ < rows_.size())
        return rows_.at(selected_).path;
    return {};
}

QString AttachDialog::current_uri() const
{
    if (address_row_ != nullptr && !address_row_->isHidden())
    {
        const QString address = address_edit_->text().trimmed();
        if (!address.isEmpty())
            return address;
    }
    const QString path = current_device_path();
    if (path.isEmpty())
        return {};
    const unsigned baud = current_settings().baud_rate;
    return QStringLiteral("serial://%1?baud=%2").arg(path).arg(baud);
}

vss::serial_settings AttachDialog::current_settings() const
{
    vss::serial_settings settings;
    if (baud_ != nullptr)
        settings.baud_rate = baud_->currentData().toUInt();
    if (data_bits_ != nullptr)
        settings.data_bits = data_bits_->currentData().toUInt();
    if (parity_ != nullptr)
        settings.parity =
            static_cast<vss::parity_mode>(parity_->currentData().toInt());
    if (stop_bits_ != nullptr)
        settings.stop_bits =
            static_cast<vss::stop_bits_mode>(stop_bits_->currentData().toInt());
    if (flow_ != nullptr)
        settings.flow_control =
            static_cast<vss::flow_control_mode>(flow_->currentData().toInt());
    if (address_row_ != nullptr && !address_row_->isHidden())
    {
        // The address wins over the fields.  Pass the baud rate from its query
        // through the settings object too, because that object is applied
        // after the URI inside the engine.
        static const QRegularExpression baud(QStringLiteral("[?&]baud=(\\d+)"));
        const auto match = baud.match(address_edit_->text());
        if (match.hasMatch())
            settings.baud_rate = match.captured(1).toUInt();
    }
    return settings;
}

void AttachDialog::attach()
{
    VSS_PROFILE_ZONE("gui.flows.attach.attach");
    update_primary_state();
    if (name_->text().trimmed().isEmpty())
    {
        name_row_->set_error(tr("Enter a name for the port."));
        name_->setFocus(Qt::OtherFocusReason);
        return;
    }

    const QString uri = current_uri();
    if (uri.isEmpty())
    {
        manual_row_->setVisible(true);
        manual_toggle_->setVisible(false);
        manual_row_->set_error(tr("Choose a device or enter a path."));
        focus_first(manual_path_);
        return;
    }

    const QString device_path = current_device_path();
    const vss::serial_settings serial = current_settings();
    const bool capture = capture_->isChecked();
    const QString name = name_->text().trimmed();

    if (busy())
        return;

    // Opening the device and making the port take seconds where Windows
    // installs a device, so they run on a worker thread.
    enum class stage
    {
        done,
        physical,
        logical,
        link
    };
    struct outcome
    {
        stage failed = stage::done;
        vss::error_code error;
        vss::endpoint_id physical = 0;
        vss::endpoint_id logical = 0;
    };
    auto state = std::make_shared<outcome>();
    run_in_background(
        [this, state, uri, serial, capture, name]
        {
            const auto physical = backend_.attach_serial(
                name + QStringLiteral("-input"), uri, serial, capture);
            if (!physical)
            {
                state->failed = stage::physical;
                state->error = physical.error();
                return;
            }
            const auto logical = backend_.create_auto_port(name, capture);
            if (!logical)
            {
                static_cast<void>(backend_.remove_port(*physical));
                state->failed = stage::logical;
                state->error = logical.error();
                return;
            }
            const auto link = backend_.split(
                *physical, std::vector<vss::endpoint_id>{*logical}, {});
            if (!link)
            {
                static_cast<void>(backend_.remove_port(*logical));
                static_cast<void>(backend_.remove_port(*physical));
                state->failed = stage::link;
                state->error = link.error();
                return;
            }
            state->physical = *physical;
            state->logical = *logical;
        },
        [this, state, device_path]
        {
            backend_.refresh();
            if (state->failed == stage::done)
            {
                physical_port_ = state->physical;
                logical_port_ = state->logical;
                accept();
                return;
            }
            flow_error error;
            if (state->failed == stage::physical)
            {
                error.title = error_title(tr("open"), device_path);
                error.body = error_body(
                    state->error,
                    tr("Another program may be using the device, or it may "
                       "have been unplugged."),
                    tr(" Close the other program or check the cable, then "
                       "try again."));
            }
            else if (state->failed == stage::logical)
            {
                error.title =
                    tr("Could not create the port for %1").arg(device_path);
                error.body = error_body(
                    state->error,
                    tr("Studio opened the device but could not create the "
                       "port your programs open. Nothing was changed."),
                    tr(" Try again, or choose another device."));
            }
            else
            {
                error.title =
                    tr("Could not create the port for %1").arg(device_path);
                error.body = error_body(
                    state->error,
                    tr("Studio opened the device but could not give your "
                       "programs a port. Nothing was changed."),
                    tr(" Try again, or choose another device."));
            }
            error.details = error_text(state->error);
            show_error(error, device_path, true);
        },
        tr("Attaching"), setting_up_text());
}

void AttachDialog::show_error(const flow_error& error,
                              const QString& device_path, bool offer_another)
{
    Q_EMIT error_shown(error.title);
    ErrorDialog dialog(error.title, error.body, error.details, this);
    if (offer_another)
    {
        dialog.add_action(tr("Try again"), [&dialog] { dialog.accept(); });
        dialog.add_action(tr("Choose another"),
                          [&dialog, this, device_path]
                          {
                              dialog.accept();
                              QTimer::singleShot(
                                  0, this,
                                  [this, device_path]
                                  {
                                      if (selected_ >= 0 &&
                                          rows_.at(selected_).path ==
                                              device_path)
                                          clear_device_selection();
                                      focus_devices();
                                  });
                          });
    }
    dialog.exec();
}

void AttachDialog::focus_devices()
{
    for (const auto& row : rows_)
    {
        if (row.available)
        {
            row.radio->setFocus(Qt::OtherFocusReason);
            return;
        }
    }
    if (refresh_ != nullptr)
        refresh_->setFocus(Qt::OtherFocusReason);
}

QRadioButton* AttachDialog::device_radio(int index) const
{
    if (index < 0 || index >= rows_.size())
        return nullptr;
    return rows_.at(index).radio;
}

} // namespace vss::gui
