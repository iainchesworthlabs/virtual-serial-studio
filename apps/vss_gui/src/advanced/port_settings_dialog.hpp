//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/studio_backend.hpp"

#include <vss/core/serial_settings.hpp>

#include <QDialog>
#include <QString>
#include <QVector>

#include <thread>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace vss::gui
{

class NoteBanner;

/// What the serial settings of a port do, which decides the dialog's note,
/// its fields and whether applying restarts the port.
enum class settings_kind
{
    /// A real device: opened with the settings.
    device,
    /// An RFC 2217 client: the remote device is asked to use them.
    remote,
    /// A mock fixture: behaves as a device set to them.
    fixture,
    /// A virtual port: the program sets its own speed; the values are saved.
    virtual_port,
    /// A loopback: no line speed; the values are saved.
    loopback,
    /// A TCP or UDP pipe: no line settings at all.
    network
};

settings_kind settings_kind_of(const port_view& port);
/// True when applying new settings restarts the port.
bool settings_restart(settings_kind kind);

/// "Settings for <name>": baud rate, data bits, parity, stop bits, flow
/// control and the timeouts, for one port or several.  See
/// `docs/gui-design/docs/screens/07-canvas-editor.md`.
///
/// Applying calls `StudioBackend::set_port_settings` on a worker thread,
/// because reopening a device can take a moment, then refreshes.
class PortSettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    PortSettingsDialog(StudioBackend& backend, QVector<vss::endpoint_id> ports,
                       QWidget* parent = nullptr);
    ~PortSettingsDialog() override;

    PortSettingsDialog(const PortSettingsDialog&) = delete;
    PortSettingsDialog& operator=(const PortSettingsDialog&) = delete;

    /// The settings the fields describe.
    vss::serial_settings settings() const;
    void set_settings(const vss::serial_settings& settings);
    /// The settings each port had when the dialog opened, by port name, for
    /// the undo step.
    QVector<QPair<QString, vss::serial_settings>> before() const
    {
        return before_;
    }
    QPushButton* apply_button() const noexcept { return apply_; }
    QString note_text() const;
    /// "Use these settings for every port linked to this one", or null when
    /// the port has none linked.
    QCheckBox* whole_set_check() const noexcept { return whole_set_; }
    QString summary_text() const;
    QString error_text() const;
    bool fields_enabled() const;
    bool timeouts_enabled() const;

    /// Applies the settings; the dialog closes when every port took them.
    void apply();

    void done(int result) override;

Q_SIGNALS:
    /// Emitted when the change could not be made, with the reason.
    void error_shown(const QString& text);

private:
    void build();
    void update_summary();
    QVector<vss::endpoint_id> set_members() const;
    void on_finished(const vss::error_code& error);

    StudioBackend& backend_;
    QVector<vss::endpoint_id> ports_;
    QVector<QPair<QString, vss::serial_settings>> before_;
    settings_kind kind_ = settings_kind::virtual_port;

    QCheckBox* whole_set_ = nullptr;
    QComboBox* baud_ = nullptr;
    QComboBox* data_bits_ = nullptr;
    QComboBox* parity_ = nullptr;
    QComboBox* stop_bits_ = nullptr;
    QComboBox* flow_ = nullptr;
    QSpinBox* read_timeout_ = nullptr;
    QSpinBox* write_timeout_ = nullptr;
    NoteBanner* note_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* apply_ = nullptr;

    bool busy_ = false;
    std::thread worker_;
};

} // namespace vss::gui
