//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/send_panel.hpp"
#include "language/bidi.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"

#include <vss/core/profile.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

bool parse_hex(const QString& text, QByteArray& out)
{
    QString digits;
    for (const QChar ch : text)
    {
        if (ch.isSpace())
            continue;
        if (!ch.isDigit() && (ch.toLower() < QLatin1Char('a') ||
                              ch.toLower() > QLatin1Char('f')))
            return false;
        digits.append(ch);
    }
    if (digits.size() % 2 != 0)
        return false;
    for (qsizetype index = 0; index < digits.size(); index += 2)
    {
        bool ok = false;
        const int value = digits.mid(index, 2).toInt(&ok, 16);
        if (!ok)
            return false;
        out.append(static_cast<char>(value));
    }
    return true;
}

} // namespace

SendPanel::SendPanel(StudioBackend& backend, QWidget* parent)
    : QWidget(parent), backend_(backend)
{
    build();
    connect(&backend_, &StudioBackend::ports_changed, this,
            [this] { refresh_ports(); });
    refresh_ports();
    load_saved_sends();
    validate_hex();
}

void SendPanel::build()
{
    VSS_PROFILE_ZONE("gui.panes.send.build");
    setAccessibleName(tr("Send"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Heading row: which port the bytes will leave from, and a pin to keep
    // that choice when the selection moves elsewhere.
    auto* heading = new QHBoxLayout();
    heading->setSpacing(8);
    target_ = new QLabel(this);
    target_->setFont(Theme::instance().font(text_style::caption));
    target_->setWordWrap(true);
    target_->setAccessibleName(tr("Sending from"));
    heading->addWidget(target_, 1);
    pin_ = new QCheckBox(tr("Pin"), this);
    pin_->setToolTip(tr("Keep sending from this port when you select another."));
    pin_->setAccessibleDescription(
        tr("Keep sending from this port when you select another."));
    heading->addWidget(pin_);
    layout->addLayout(heading);

    // Port chooser and Send button share a row. The combo may shrink so a
    // narrow pane is not forced wider by a long port name.
    auto* top = new QHBoxLayout();
    top->setSpacing(8);
    ports_ = new QComboBox(this);
    ports_->setAccessibleName(tr("Send from port"));
    ports_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    ports_->setMinimumContentsLength(8);
    top->addWidget(ports_, 1);
    send_ = new QPushButton(tr("Send"), this);
    send_->setIcon(IconProvider::instance().icon(QStringLiteral("send"),
                                                 colour_role::onAccent, 16));
    send_->setProperty("role", QStringLiteral("primary"));
    polish(send_);
    top->addWidget(send_);
    layout->addLayout(top);

    // The input is read as text or, with Hex ticked, as hex pairs. The error
    // label stays hidden until validate_hex() finds bad hex.
    input_ = new QLineEdit(this);
    keep_left_to_right(input_);
    input_->setAccessibleName(tr("Bytes to send"));
    input_->setPlaceholderText(tr("Type text, or hex if Hex is ticked"));
    input_->setFont(Theme::instance().font(text_style::data));
    layout->addWidget(input_);

    error_ = new QLabel(this);
    error_->setFont(Theme::instance().font(text_style::caption));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: %1;")
                              .arg(Theme::instance()
                                       .colour(colour_role::bad)
                                       .name(QColor::HexRgb)));
    error_->setVisible(false);
    layout->addWidget(error_);

    // Options row. Repeat and its interval are advanced-only; set_advanced()
    // decides their visibility at the end of this function.
    auto* options = new QHBoxLayout();
    options->setSpacing(16);
    hex_ = new QCheckBox(tr("Hex"), this);
    crlf_ = new QCheckBox(tr("Add CRLF"), this);
    crlf_->setChecked(true);
    options->addWidget(hex_);
    options->addWidget(crlf_);
    repeat_ = new QCheckBox(tr("Repeat"), this);
    interval_ = new QSpinBox(this);
    interval_->setRange(50, 5000);
    interval_->setValue(1000);
    interval_->setSuffix(tr(" ms"));
    interval_->setAccessibleName(tr("Repeat interval"));
    interval_->setVisible(false);
    options->addWidget(repeat_);
    options->addWidget(interval_);
    options->addStretch(1);
    layout->addLayout(options);

    // A status row for a running repeat, with a button to stop it. It is
    // hidden until a repeat starts (see update_repeat_status).
    repeat_row_ = new QWidget(this);
    auto* repeat_layout = new QHBoxLayout(repeat_row_);
    repeat_layout->setContentsMargins(0, 0, 0, 0);
    repeat_layout->setSpacing(8);
    repeat_status_ = new QLabel(repeat_row_);
    repeat_status_->setFont(Theme::instance().font(text_style::caption));
    repeat_status_->setWordWrap(true);
    repeat_status_->setAccessibleName(tr("Repeat status"));
    repeat_layout->addWidget(repeat_status_, 1);
    repeat_stop_ = new QPushButton(tr("Stop repeat"), repeat_row_);
    repeat_stop_->setAccessibleName(tr("Stop repeat"));
    repeat_layout->addWidget(repeat_stop_);
    repeat_row_->setVisible(false);
    layout->addWidget(repeat_row_);

    // Saved sends are filled in by load_saved_sends() and are advanced-only.
    auto* saved_row = new QHBoxLayout();
    saved_row->setSpacing(8);
    saved_label_ = new QLabel(tr("Saved sends"), this);
    saved_label_->setFont(Theme::instance().font(text_style::caption));
    saved_label_->setProperty("muted", true);
    polish(saved_label_);
    saved_ = new QComboBox(this);
    saved_->setAccessibleName(tr("Saved sends"));
    saved_row->addWidget(saved_label_);
    saved_row->addWidget(saved_, 1);
    layout->addLayout(saved_row);
    layout->addStretch(1);

    // The repeat timer is only started by start_repeat(); here it just takes
    // the spin box's initial interval.
    repeat_timer_ = new QTimer(this);
    repeat_timer_->setInterval(interval_->value());
    connect(repeat_timer_, &QTimer::timeout, this, [this] { send_repeat(); });

    // Wiring. Anything that changes what would be sent re-validates the hex
    // and recomputes whether Send may be pressed.
    connect(send_, &QPushButton::clicked, this, [this] { send_now(); });
    connect(input_, &QLineEdit::textChanged, this,
            [this]
            {
                validate_hex();
                update_send_enabled();
            });
    connect(input_, &QLineEdit::returnPressed, this, [this] { send_now(); });
    connect(hex_, &QCheckBox::toggled, this,
            [this]
            {
                validate_hex();
                update_send_enabled();
            });
    // Ticking Repeat starts it at once; unticking stops it. The interval box
    // is only shown in advanced mode while Repeat is on.
    connect(repeat_, &QCheckBox::toggled, this,
            [this](bool on)
            {
                interval_->setVisible(advanced_ && on);
                if (on)
                    start_repeat();
                else
                    stop_repeat();
            });
    connect(interval_, &QSpinBox::valueChanged, this,
            [this](int value)
            {
                repeat_timer_->setInterval(value);
                update_repeat_status();
            });
    connect(repeat_stop_, &QPushButton::clicked, this,
            [this] { stop_repeat(); });
    connect(ports_, &QComboBox::currentIndexChanged, this,
            [this]
            {
                update_target();
                update_send_enabled();
            });
    connect(saved_, qOverload<int>(&QComboBox::activated), this,
            [this](int index)
            {
                if (index >= 0)
                    input_->setText(saved_->itemData(index).toString());
            });
    // Start in the simple layout, which hides the advanced-only controls.
    set_advanced(false);
}

void SendPanel::set_advanced(bool advanced)
{
    advanced_ = advanced;
    repeat_->setVisible(advanced);
    interval_->setVisible(advanced && repeat_->isChecked());
    saved_label_->setVisible(advanced);
    saved_->setVisible(advanced);
    if (!advanced)
    {
        repeat_->setChecked(false);
        stop_repeat();
    }
}

void SendPanel::refresh_ports()
{
    VSS_PROFILE_ZONE("gui.panes.send.refresh_ports");
    if (repeat_port_ != 0 && !backend_.port(repeat_port_))
    {
        const QString name = repeat_name_;
        stop_repeat();
        Q_EMIT error_message(
            tr("The repeat stopped because %1 is gone.").arg(name));
    }

    QStringList signature;
    for (const auto& port : backend_.ports())
        signature << QStringLiteral("%1|%2|%3")
                         .arg(static_cast<qulonglong>(port.id))
                         .arg(port.name, port.path);
    if (signature == port_signature_)
    {
        update_target();
        update_send_enabled();
        return;
    }
    port_signature_ = signature;

    const auto previous = selected_port();
    refreshing_ = true;
    ports_->clear();
    for (const auto& port : backend_.ports())
    {
        ports_->addItem(port.path.isEmpty()
                            ? port.name
                            : tr("%1 (%2)").arg(port.name, port.path),
                        QVariant::fromValue<qulonglong>(port.id));
    }
    if (previous != 0)
    {
        const int index =
            ports_->findData(QVariant::fromValue<qulonglong>(previous));
        if (index >= 0)
            ports_->setCurrentIndex(index);
    }
    refreshing_ = false;
    update_target();
    update_send_enabled();
}

vss::endpoint_id SendPanel::selected_port() const
{
    if (ports_ == nullptr || ports_->currentIndex() < 0)
        return 0;
    return static_cast<vss::endpoint_id>(ports_->currentData().toULongLong());
}

void SendPanel::set_selected_port(vss::endpoint_id id)
{
    // Only new sends follow the selection.  A pinned target stays put, and a
    // running repeat is bound to the port it started on (`repeat_port_`).
    if (pinned())
        return;
    const int index = ports_->findData(QVariant::fromValue<qulonglong>(id));
    if (index >= 0)
    {
        const QSignalBlocker blocker(ports_);
        ports_->setCurrentIndex(index);
    }
}

QString SendPanel::text() const
{
    return input_->text();
}

void SendPanel::validate_hex()
{
    VSS_PROFILE_ZONE("gui.panes.send.validate_hex");
    if (!hex_->isChecked())
    {
        error_->setVisible(false);
        input_->setProperty("invalid", false);
        polish(input_);
        return;
    }
    QByteArray bytes;
    if (input_->text().trimmed().isEmpty() || parse_hex(input_->text(), bytes))
    {
        error_->setVisible(false);
        input_->setProperty("invalid", false);
        polish(input_);
        return;
    }
    error_->setText(tr("Hex needs pairs of digits, such as 48 65 6C 6C 6F."));
    error_->setVisible(true);
    input_->setProperty("invalid", true);
    polish(input_);
}

void SendPanel::update_send_enabled()
{
    const bool has_port = selected_port() != 0;
    const bool has_text = !input_->text().isEmpty();
    send_->setEnabled(has_port && has_text && error_->isHidden());
    send_->setToolTip(has_port ? QString() : tr("Select a port to send to."));
}

QByteArray SendPanel::payload(bool& ok) const
{
    QByteArray bytes;
    if (hex_->isChecked())
    {
        ok = parse_hex(input_->text(), bytes);
    }
    else
    {
        bytes = input_->text().toUtf8();
        ok = true;
    }
    if (crlf_->isChecked())
        bytes.append("\r\n");
    return bytes;
}

void SendPanel::send_now()
{
    if (refreshing_)
        return;
    const vss::endpoint_id id = selected_port();
    if (id == 0)
        return;
    send_to(id);
}

SendPanel::send_outcome SendPanel::send_to(vss::endpoint_id id)
{
    VSS_PROFILE_ZONE("gui.panes.send.send");
    bool ok = false;
    const QByteArray bytes = payload(ok);
    if (!ok)
    {
        validate_hex();
        return send_outcome::skipped;
    }
    if (bytes.isEmpty())
        return send_outcome::skipped;
    const auto sent_result = backend_.send(id, bytes);
    if (!sent_result)
    {
        Q_EMIT error_message(error_text(sent_result.error()));
        return send_outcome::failed;
    }
    const QString name = port_label(id);
    remember_send(input_->text());
    Q_EMIT sent(static_cast<int>(bytes.size()), name);
    Q_EMIT status_message(
        tr("Queued %n bytes to %1.", "", static_cast<int>(bytes.size()))
            .arg(name));
    return send_outcome::sent;
}

void SendPanel::send_repeat()
{
    if (refreshing_ || repeat_port_ == 0 || input_->text().isEmpty())
        return;
    // The bytes go to the port the repeat started on, whatever is selected now.
    if (send_to(repeat_port_) == send_outcome::failed)
        stop_repeat();
}

void SendPanel::start_repeat()
{
    VSS_PROFILE_ZONE("gui.panes.send.start_repeat");
    if (!repeat_->isChecked())
        return;
    const vss::endpoint_id id = selected_port();
    if (id == 0)
    {
        const QSignalBlocker blocker(repeat_);
        repeat_->setChecked(false);
        interval_->setVisible(false);
        return;
    }
    repeat_port_ = id;
    repeat_name_ = port_label(id);
    repeat_timer_->start(interval_->value());
    update_repeat_status();
    Q_EMIT repeat_changed(id);
}

void SendPanel::stop_repeat()
{
    const bool was_running = repeat_port_ != 0;
    repeat_timer_->stop();
    repeat_port_ = 0;
    if (repeat_->isChecked())
    {
        const QSignalBlocker blocker(repeat_);
        repeat_->setChecked(false);
        interval_->setVisible(false);
    }
    update_repeat_status();
    repeat_name_.clear();
    if (was_running)
        Q_EMIT repeat_changed(0);
}

void SendPanel::send_from(vss::endpoint_id id)
{
    refresh_ports();
    const int index = ports_->findData(QVariant::fromValue<qulonglong>(id));
    if (index >= 0 && ports_->currentIndex() != index)
        ports_->setCurrentIndex(index);
    focus_input();
}

void SendPanel::toggle_repeat(vss::endpoint_id id)
{
    if (!advanced_)
        return;
    if (repeat_port_ == id && id != 0)
    {
        stop_repeat();
        return;
    }
    send_from(id);
    if (selected_port() != id)
        return;
    if (input_->text().isEmpty())
    {
        Q_EMIT status_message(
            tr("Type the bytes to repeat, then start the repeat."));
        return;
    }
    if (repeating())
        stop_repeat();
    repeat_->setChecked(true);
}

void SendPanel::focus_input()
{
    input_->setFocus(Qt::OtherFocusReason);
}

bool SendPanel::pinned() const
{
    return pin_ != nullptr && pin_->isChecked();
}

void SendPanel::set_pinned(bool pinned)
{
    pin_->setChecked(pinned);
}

void SendPanel::set_repeat(bool on, int interval_ms)
{
    interval_->setValue(interval_ms);
    repeat_->setChecked(on);
}

QString SendPanel::port_label(vss::endpoint_id id) const
{
    const auto port = backend_.port(id);
    if (!port)
        return {};
    return port->path.isEmpty()
               ? isolate_name(port->name)
               : tr("%1 (%2)")
                     .arg(isolate_name(port->name), isolate_name(port->path));
}

QString SendPanel::target_text() const
{
    return target_->text();
}

QString SendPanel::repeat_text() const
{
    return repeat_port_ == 0 ? QString() : repeat_status_->text();
}

void SendPanel::update_target()
{
    if (target_ == nullptr)
        return;
    target_->setText(selected_port() == 0
                         ? tr("Sending from: no port selected")
                         : tr("Sending from: %1").arg(ports_->currentText()));
}

void SendPanel::update_repeat_status()
{
    if (repeat_row_ == nullptr)
        return;
    repeat_row_->setVisible(repeat_port_ != 0);
    if (repeat_port_ == 0)
    {
        repeat_status_->clear();
        return;
    }
    const QString label = port_label(repeat_port_);
    repeat_status_->setText(
        tr("Repeating every %1 ms from %2.")
            .arg(interval_->value())
            .arg(label.isEmpty() ? repeat_name_ : label));
}

void SendPanel::remember_send(const QString& text)
{
    VSS_PROFILE_ZONE("gui.panes.send.remember");
    if (text.isEmpty())
        return;
    QSettings settings;
    QStringList saved =
        settings.value(QStringLiteral("ui/savedSends")).toStringList();
    if (!saved.isEmpty() && saved.first() == text)
        return;
    saved.removeAll(text);
    saved.prepend(text);
    while (saved.size() > 12)
        saved.removeLast();
    settings.setValue(QStringLiteral("ui/savedSends"), saved);
    load_saved_sends();
}

void SendPanel::load_saved_sends()
{
    VSS_PROFILE_ZONE("gui.panes.send.load_saved");
    const QSignalBlocker blocker(saved_);
    saved_->clear();
    const QStringList saved =
        QSettings().value(QStringLiteral("ui/savedSends")).toStringList();
    for (const QString& text : saved)
    {
        const QString label =
            text.size() > 48 ? text.left(45) + QStringLiteral("...") : text;
        saved_->addItem(label, text);
    }
    saved_->setEnabled(!saved.isEmpty());
}

} // namespace vss::gui
