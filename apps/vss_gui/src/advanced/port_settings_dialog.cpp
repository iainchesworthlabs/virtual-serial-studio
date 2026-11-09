//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/port_settings_dialog.hpp"

#include "advanced/chrome.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QColor>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace vss::gui
{
namespace
{

constexpr int kMinBaud = 50;
constexpr int kMaxBaud = 4'000'000;
constexpr int kMaxTimeout = 60'000;

QComboBox* make_combo(const QString& name, QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    combo->setAccessibleName(name);
    combo->setObjectName(name);
    return combo;
}

void select_data(QComboBox* combo, int value)
{
    const int index = combo->findData(value);
    if (index >= 0)
        combo->setCurrentIndex(index);
}

QString flow_text(vss::flow_control_mode mode)
{
    switch (mode)
    {
    case vss::flow_control_mode::none:
        return PortSettingsDialog::tr("no flow control");
    case vss::flow_control_mode::hardware:
        return PortSettingsDialog::tr("hardware flow control");
    case vss::flow_control_mode::software:
        return PortSettingsDialog::tr("software flow control");
    }
    return {};
}

} // namespace

settings_kind settings_kind_of(const port_view& port)
{
    switch (port.kind)
    {
    case vss::endpoint_kind::physical:
        return settings_kind::device;
    case vss::endpoint_kind::rfc2217:
        return settings_kind::remote;
    case vss::endpoint_kind::mock:
        return settings_kind::fixture;
    case vss::endpoint_kind::tcp:
    case vss::endpoint_kind::udp:
        return settings_kind::network;
    case vss::endpoint_kind::virtual_port:
        break;
    }
    return port.path.startsWith(QStringLiteral("loopback:"))
               ? settings_kind::loopback
               : settings_kind::virtual_port;
}

bool settings_restart(settings_kind kind)
{
    return kind == settings_kind::device || kind == settings_kind::remote ||
           kind == settings_kind::fixture;
}

PortSettingsDialog::PortSettingsDialog(StudioBackend& backend,
                                       QVector<vss::endpoint_id> ports,
                                       QWidget* parent)
    : QDialog(parent), backend_(backend), ports_(std::move(ports))
{
    bool first = true;
    for (const vss::endpoint_id id : ports_)
    {
        const auto view = backend_.port(id);
        if (!view)
            continue;
        before_.append({view->name, view->serial});
        const settings_kind kind = settings_kind_of(*view);
        // Several ports: the one that does the most decides what is shown.
        if (first || (settings_restart(kind) && !settings_restart(kind_)) ||
            kind == settings_kind::network)
            kind_ = kind;
        first = false;
    }
    build();
    if (!before_.isEmpty())
        set_settings(before_.first().second);
    update_summary();
}

PortSettingsDialog::~PortSettingsDialog()
{
    if (worker_.joinable())
        worker_.join();
}

void PortSettingsDialog::build()
{
    VSS_PROFILE_ZONE("gui.advanced.port_settings.build");
    VSS_PROFILE_MESSAGE("gui.dialog.port_settings.opened");
    // The title names the port, or counts the ports when several are edited
    // together.
    const QString title = before_.size() == 1
                              ? tr("Settings for %1").arg(before_.first().first)
                              : tr("Settings for %n ports", nullptr,
                                   static_cast<int>(before_.size()));
    setWindowTitle(title);
    setAccessibleName(title);
    setObjectName(QStringLiteral("portSettingsDialog"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    setMinimumWidth(std::max(520, Theme::instance().metric(QStringLiteral(
                                      "layout.dialogWidths.small"))));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 0);
    layout->setSpacing(14);
    layout->addWidget(make_header(title, {}, this, false, {}));

    // The note says what the values mean for this kind of port, since a
    // device, a remote, a fixture and a virtual port treat them differently.
    QString note;
    switch (kind_)
    {
    case settings_kind::device:
        note = tr("Studio opens the device with these settings.");
        break;
    case settings_kind::remote:
        note = tr("Studio asks the remote device to use these settings.");
        break;
    case settings_kind::fixture:
        note = tr("The fixture behaves as a device set to these values.");
        break;
    case settings_kind::virtual_port:
        note = tr("A program that opens this port sets its own speed. These "
                  "are the values saved with the setup.");
        // For a single port, also report what the connected program last
        // set, when the backend can tell.
        if (ports_.size() == 1)
        {
            const auto view = backend_.port(ports_.first());
            if (view && view->peer_format && backend_.caps().line_format)
            {
                note += QLatin1Char(' ') +
                        tr("Program set %1.")
                            .arg(QString::fromStdString(
                                vss::to_label(*view->peer_format)));
            }
        }
        break;
    case settings_kind::loopback:
        note = tr("A loopback has no line speed. These values are saved with "
                  "the setup.");
        break;
    case settings_kind::network:
        note = tr("A network port has no line settings.");
        break;
    }
    note_ = new NoteBanner(NoteBanner::kind::info, note, this);
    note_->setObjectName(QStringLiteral("portSettingsNote"));
    layout->addWidget(note_);

    // The fields.  Baud rate is an editable combo, so a rate outside the list
    // can be typed; the validator keeps it within the allowed range.
    auto* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);

    baud_ = make_combo(tr("Baud rate"), this);
    baud_->setEditable(true);
    for (const int value : {300, 1200, 2400, 4800, 9600, 19200, 38400, 57600,
                            115200, 230400, 460800, 921600})
        baud_->addItem(QString::number(value), value);
    baud_->setValidator(new QIntValidator(kMinBaud, kMaxBaud, baud_));
    baud_->lineEdit()->setAccessibleName(tr("Baud rate"));

    data_bits_ = make_combo(tr("Data bits"), this);
    for (const int bits : {5, 6, 7, 8})
        data_bits_->addItem(QString::number(bits), bits);

    parity_ = make_combo(tr("Parity"), this);
    parity_->addItem(tr("None"), static_cast<int>(vss::parity_mode::none));
    parity_->addItem(tr("Odd"), static_cast<int>(vss::parity_mode::odd));
    parity_->addItem(tr("Even"), static_cast<int>(vss::parity_mode::even));
    parity_->addItem(tr("Mark"), static_cast<int>(vss::parity_mode::mark));
    parity_->addItem(tr("Space"), static_cast<int>(vss::parity_mode::space));

    stop_bits_ = make_combo(tr("Stop bits"), this);
    stop_bits_->addItem(QStringLiteral("1"),
                        static_cast<int>(vss::stop_bits_mode::one));
    stop_bits_->addItem(QStringLiteral("1.5"),
                        static_cast<int>(vss::stop_bits_mode::one_point_five));
    stop_bits_->addItem(QStringLiteral("2"),
                        static_cast<int>(vss::stop_bits_mode::two));

    flow_ = make_combo(tr("Flow control"), this);
    flow_->addItem(tr("None"), static_cast<int>(vss::flow_control_mode::none));
    flow_->addItem(tr("Hardware (RTS/CTS)"),
                   static_cast<int>(vss::flow_control_mode::hardware));
    flow_->addItem(tr("Software (XON/XOFF)"),
                   static_cast<int>(vss::flow_control_mode::software));

    // Both timeouts are spin boxes in milliseconds; 0 is shown as "Wait".
    const auto make_timeout = [this](const QString& name)
    {
        auto* spin = new QSpinBox(this);
        spin->setObjectName(name);
        spin->setAccessibleName(name);
        spin->setRange(0, kMaxTimeout);
        spin->setSuffix(tr(" ms"));
        spin->setSpecialValueText(tr("Wait"));
        return spin;
    };
    read_timeout_ = make_timeout(tr("Read timeout"));
    write_timeout_ = make_timeout(tr("Write timeout"));

    // Lay the fields out two to a row, in the order listed.
    const QVector<QPair<QString, QWidget*>> rows = {
        {tr("Baud rate"), baud_},
        {tr("Data bits"), data_bits_},
        {tr("Parity"), parity_},
        {tr("Stop bits"), stop_bits_},
        {tr("Flow control"), flow_},
        {tr("Read timeout"), read_timeout_},
        {tr("Write timeout"), write_timeout_}};
    int row = 0;
    for (const auto& [label, field] : rows)
    {
        grid->addWidget(new FormRow(label, field, {}, this), row / 2, row % 2);
        ++row;
    }
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);

    // A one-line summary of the chosen values, refreshed by update_summary().
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("portSettingsSummary"));
    summary_->setFont(Theme::instance().font(text_style::data));
    set_ink2(summary_);
    layout->addWidget(summary_);

    // Only kinds of port that must be restarted to take new settings carry
    // the warning about bytes in flight.
    if (settings_restart(kind_))
    {
        auto* warn = new NoteBanner(
            NoteBanner::kind::warn,
            tr("Restarting the port drops bytes that are in flight. Programs "
               "that have it open keep their handle."),
            this);
        layout->addWidget(warn);
    }

    // Hidden until an apply fails.  The colour is set by style sheet, so it
    // is re-applied whenever the theme changes.
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("portSettingsError"));
    error_->setWordWrap(true);
    error_->setFont(Theme::instance().font(text_style::caption));
    const auto apply_error_colour = [this]
    {
        error_->setStyleSheet(QStringLiteral("color: %1")
                                  .arg(Theme::instance()
                                           .colour(colour_role::bad)
                                           .name(QColor::HexRgb)));
    };
    apply_error_colour();
    connect(&Theme::instance(), &Theme::theme_changed, error_,
            [apply_error_colour] { apply_error_colour(); });
    error_->hide();
    layout->addWidget(error_);
    layout->addStretch(1);

    // Footer: Cancel, and Apply as the default button.  The label says
    // whether applying restarts the port.
    QHBoxLayout* footer_layout = nullptr;
    auto* footer = make_footer(this, footer_layout);
    auto* cancel = make_button(tr("Cancel"), nullptr, footer);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    footer_layout->addWidget(cancel);
    apply_ = make_button(settings_restart(kind_) ? tr("Apply and restart")
                                                 : tr("Apply"),
                         "primary", footer);
    apply_->setObjectName(QStringLiteral("portSettingsApply"));
    apply_->setDefault(true);
    connect(apply_, &QPushButton::clicked, this, &PortSettingsDialog::apply);
    footer_layout->addWidget(apply_);
    layout->addWidget(footer);

    // What a kind of port can use.
    const bool any = kind_ != settings_kind::network;
    for (QWidget* field :
         {static_cast<QWidget*>(baud_), static_cast<QWidget*>(data_bits_),
          static_cast<QWidget*>(parity_), static_cast<QWidget*>(stop_bits_),
          static_cast<QWidget*>(flow_)})
        field->setEnabled(any);
    // Timeouts apply only to a device or a remote port.
    const bool timeouts =
        kind_ == settings_kind::device || kind_ == settings_kind::remote;
    read_timeout_->setEnabled(timeouts);
    write_timeout_->setEnabled(timeouts);

    // Without backend support Apply is disabled with the standard reason;
    // for a network port it is disabled with the note as the reason.
    if (!backend_.caps().port_settings)
        degrade(apply_);
    else if (!any)
        degrade(apply_, note);

    // Any change, including typing a baud rate, refreshes the summary.
    for (QComboBox* combo : {baud_, data_bits_, parity_, stop_bits_, flow_})
    {
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this] { update_summary(); });
    }
    connect(baud_, &QComboBox::editTextChanged, this,
            [this] { update_summary(); });
    baud_->setFocus(Qt::OtherFocusReason);
}

vss::serial_settings PortSettingsDialog::settings() const
{
    vss::serial_settings out;
    bool ok = false;
    const int baud = baud_->currentText().trimmed().toInt(&ok);
    out.baud_rate = ok && baud > 0 ? static_cast<unsigned>(baud) : 0U;
    out.data_bits = static_cast<unsigned>(data_bits_->currentData().toInt());
    out.parity = static_cast<vss::parity_mode>(parity_->currentData().toInt());
    out.stop_bits =
        static_cast<vss::stop_bits_mode>(stop_bits_->currentData().toInt());
    out.flow_control =
        static_cast<vss::flow_control_mode>(flow_->currentData().toInt());
    out.read_timeout = std::chrono::milliseconds(read_timeout_->value());
    out.write_timeout = std::chrono::milliseconds(write_timeout_->value());
    if (!before_.isEmpty())
        out.exclusive = before_.first().second.exclusive;
    return out;
}

void PortSettingsDialog::set_settings(const vss::serial_settings& settings)
{
    const int baud = static_cast<int>(settings.baud_rate);
    const int index = baud_->findData(baud);
    if (index >= 0)
        baud_->setCurrentIndex(index);
    else
        baud_->setEditText(QString::number(baud));
    select_data(data_bits_, static_cast<int>(settings.data_bits));
    select_data(parity_, static_cast<int>(settings.parity));
    select_data(stop_bits_, static_cast<int>(settings.stop_bits));
    select_data(flow_, static_cast<int>(settings.flow_control));
    read_timeout_->setValue(static_cast<int>(settings.read_timeout.count()));
    write_timeout_->setValue(static_cast<int>(settings.write_timeout.count()));
    update_summary();
}

void PortSettingsDialog::update_summary()
{
    if (summary_ == nullptr)
        return;
    const vss::serial_settings value = settings();
    const bool valid = !vss::validate(value);
    summary_->setText(
        valid ? tr("%1, %2").arg(QString::fromStdString(vss::to_label(value)),
                                 flow_text(value.flow_control))
              : tr("The baud rate must be a whole number from %1 to %2.")
                    .arg(kMinBaud)
                    .arg(kMaxBaud));
    if (apply_ != nullptr && backend_.caps().port_settings &&
        kind_ != settings_kind::network)
        apply_->setEnabled(valid && !busy_);
}

QString PortSettingsDialog::note_text() const
{
    return note_ == nullptr ? QString() : note_->text();
}

QString PortSettingsDialog::summary_text() const
{
    return summary_ == nullptr ? QString() : summary_->text();
}

QString PortSettingsDialog::error_text() const
{
    return error_ == nullptr || error_->isHidden() ? QString() : error_->text();
}

bool PortSettingsDialog::fields_enabled() const
{
    return baud_->isEnabled();
}

bool PortSettingsDialog::timeouts_enabled() const
{
    return read_timeout_->isEnabled();
}

void PortSettingsDialog::apply()
{
    VSS_PROFILE_ZONE("gui.advanced.port_settings.apply");
    if (busy_ || !apply_->isEnabled())
        return;
    const vss::serial_settings value = settings();
    if (vss::validate(value))
        return;

    if (worker_.joinable())
        worker_.join();
    busy_ = true;
    apply_->setEnabled(false);
    error_->hide();

    // The worker calls the backend only and posts the outcome back; the
    // destructor joins it before the dialog goes.
    worker_ = std::thread(
        [this, value, ports = ports_]
        {
            vss::error_code error;
            for (const vss::endpoint_id id : ports)
            {
                if (const auto done = backend_.set_port_settings(id, value);
                    !done)
                {
                    error = done.error();
                    break;
                }
            }
            QMetaObject::invokeMethod(
                this, [this, error] { on_finished(error); },
                Qt::QueuedConnection);
        });
}

void PortSettingsDialog::on_finished(const vss::error_code& error)
{
    VSS_PROFILE_ZONE("gui.advanced.port_settings.finished");
    if (worker_.joinable())
        worker_.join();
    busy_ = false;
    backend_.refresh();
    if (error)
    {
        const QString text = tr("The port could not take these settings: %1")
                                 .arg(vss::gui::error_text(error));
        error_->setText(text);
        error_->show();
        update_summary();
        Q_EMIT error_shown(text);
        return;
    }
    accept();
}

void PortSettingsDialog::done(int result)
{
    // A reopen cannot be taken back half way, so the dialog waits for it.
    if (busy_)
        return;
    QDialog::done(result);
}

} // namespace vss::gui
