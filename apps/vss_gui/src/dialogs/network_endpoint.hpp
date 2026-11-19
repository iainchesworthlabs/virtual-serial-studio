//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_DIALOGS_NETWORK_ENDPOINT_HPP
#define VSS_GUI_DIALOGS_NETWORK_ENDPOINT_HPP

#include "backend/studio_backend.hpp"
#include "dialogs/network_uri.hpp"

#include <QDialog>
#include <QString>

#include <optional>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QWidget;

namespace vss::gui
{

class CodeBlock;
class FormRow;
class InlineError;
class NoteBanner;
class ResultSentence;
class SegmentedControl;

/// Topology, Share a device (Advanced), or Ports, Network endpoint.
/// "Put a device on the network": TCP, UDP and RFC 2217.
class NetworkEndpointDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit NetworkEndpointDialog(StudioBackend& backend,
                                   bool advanced = false,
                                   vss::endpoint_id preselected = 0,
                                   QWidget* parent = nullptr);

    void set_advanced(bool advanced);
    bool advanced() const noexcept { return advanced_; }

    void set_transport(network_transport transport);
    network_transport transport() const { return transport_; }
    void set_role(network_role role);
    network_role role() const { return role_; }
    bool server_available() const { return server_available_; }
    bool server_selected() const;

    QStringList device_paths() const;
    bool set_device_path(const QString& path);
    QString device_path() const;
    /// The port in Studio the network port will serve, or 0 when the choice is
    /// a real device or "A new port in Studio".
    vss::endpoint_id selected_port() const;
    bool new_port_selected() const;
    bool select_port(vss::endpoint_id id);
    bool select_new_port();

    /// Opens the dialog on a network port that is there, to change it: server
    /// or client, the address, the bind port and the rest of the address.  The
    /// port keeps its name and its links.  False for a port that is not a
    /// network port or has no address to show.
    bool edit_port(vss::endpoint_id id);
    vss::endpoint_id editing_port() const noexcept { return editing_; }

    void set_server_address(const QString& address);
    QString server_address() const;
    void set_listen_address(const QString& address);
    QString listen_address() const;
    void set_port(int port);
    int port() const;

    void set_address_override(const QString& address);
    void clear_address_override();
    bool override_active() const { return override_; }
    QString address_override() const;

    QString uri() const;
    QString source_uri() const;
    QString command() const;
    QString result_text() const;
    QString accessible_result_text() const;
    QString error_title() const;
    QString error_body() const;
    bool error_visible() const;
    QString last_error() const { return last_error_; }

    bool submit();
    /// Makes the network port although the remote end refused, and keeps
    /// trying.  Only after `submit` was refused by the remote end.
    bool submit_anyway();
    std::optional<vss::link_id> result_link() const { return result_link_; }
    int result_port() const { return result_port_; }

    QPushButton* primary_button() const { return primary_; }
    /// "Create anyway and keep trying", shown when a client was refused.
    QPushButton* anyway_button() const { return anyway_; }
    QPushButton* test_button() const { return test_; }

Q_SIGNALS:
    void advanced_requested();
    void bridge_started();

private:
    void build();
    void reload_devices();
    void update_role_ui();
    void update_result();
    void update_address_blocks();
    void update_warning();
    void show_firewall_dialog();
    void clear_error();
    bool create(bool keep_trying);
    bool show_create_error(const vss::error_code& error, int port_number);
    QString build_fields_uri() const;
    QString effective_uri() const;
    bool validate(QString& error) const;
    QString device_label(const device_view& device) const;

    StudioBackend& backend_;
    bool advanced_ = false;
    bool override_ = false;
    bool server_available_ = false;
    network_transport transport_ = network_transport::tcp;
    network_role role_ = network_role::server;
    std::vector<device_view> devices_;
    QString device_path_;
    vss::endpoint_id preselected_ = 0;
    vss::endpoint_id editing_ = 0;
    QString last_error_;

    QComboBox* device_combo_ = nullptr;
    FormRow* device_row_ = nullptr;
    SegmentedControl* transport_control_ = nullptr;
    QWidget* role_row_ = nullptr;
    QRadioButton* server_radio_ = nullptr;
    QRadioButton* client_radio_ = nullptr;
    QStackedWidget* address_stack_ = nullptr;
    QComboBox* listen_combo_ = nullptr;
    QLineEdit* listen_custom_ = nullptr;
    QLineEdit* address_edit_ = nullptr;
    QLineEdit* port_edit_ = nullptr;
    FormRow* port_row_ = nullptr;
    QLabel* hint_ = nullptr;
    NoteBanner* capability_note_ = nullptr;
    NoteBanner* firewall_note_ = nullptr;
    QPushButton* firewall_button_ = nullptr;
    NoteBanner* open_note_ = nullptr;
    ResultSentence* result_ = nullptr;
    InlineError* error_ = nullptr;
    CodeBlock* address_block_ = nullptr;
    CodeBlock* command_block_ = nullptr;
    QLineEdit* override_edit_ = nullptr;
    QPushButton* edit_address_ = nullptr;
    QWidget* override_panel_ = nullptr;
    QPushButton* test_ = nullptr;
    QPushButton* primary_ = nullptr;
    QPushButton* anyway_ = nullptr;
    QLabel* device_note_ = nullptr;

    std::optional<vss::link_id> result_link_;
    int result_port_ = 0;
};

} // namespace vss::gui

#endif // VSS_GUI_DIALOGS_NETWORK_ENDPOINT_HPP
