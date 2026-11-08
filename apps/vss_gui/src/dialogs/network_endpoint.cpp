//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "dialogs/network_endpoint.hpp"

#include "dialogs/common.hpp"
#include "dialogs/network_uri.hpp"
#include "support/elevated_step.hpp"
#include "support/firewall_dialog.hpp"
#include "support/permission_rows.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QTextDocumentFragment>
#include <QVBoxLayout>

#include <vss/core/profile.hpp>
#include <vss/ports/permissions.hpp>

#include <algorithm>

namespace vss::gui
{
namespace
{

bool valid_ipv4(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return false;
    for (const QString& part : parts)
    {
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok || value < 0 || value > 255)
            return false;
    }
    return true;
}

bool valid_ipv6_ish(const QString& text)
{
    if (!text.contains(QLatin1Char(':')) || text.contains(QLatin1Char(' ')))
        return false;
    for (const QChar character : text)
    {
        if (!character.isLetterOrNumber() && character != QLatin1Char(':') &&
            character != QLatin1Char('.') && character != QLatin1Char('%'))
        {
            return false;
        }
    }
    return true;
}

bool valid_listen_host(const QString& text)
{
    return valid_ipv4(text) || valid_ipv6_ish(text);
}

bool valid_remote_host(const QString& text)
{
    return !text.isEmpty() && !text.contains(QLatin1Char(' ')) &&
           !text.contains(QLatin1Char('/'));
}

QString listen_host(QComboBox* combo, QLineEdit* custom)
{
    QString data = combo->currentData().toString();
    if (!data.isEmpty())
        return data;
    return custom == nullptr ? QString() : custom->text().trimmed();
}

bool looks_like_address_in_use(const vss::error_code& error)
{
    if (error == vss::errc::device_busy)
        return true;
    const QString message = QString::fromStdString(error.message()).toLower();
    return message.contains(QStringLiteral("in use")) ||
           message.contains(QStringLiteral("already in use")) ||
           error.value() == 10048 || error.value() == 98;
}

} // namespace

NetworkEndpointDialog::NetworkEndpointDialog(StudioBackend& backend,
                                             bool advanced,
                                             vss::endpoint_id preselected,
                                             QWidget* parent)
    : QDialog(parent), backend_(backend), advanced_(advanced),
      server_available_(backend.caps().network_server),
      role_(backend.caps().network_server ? network_role::server
                                          : network_role::client),
      preselected_(preselected)
{
    build();
    reload_devices();
    set_advanced(advanced);
    update_role_ui();
}

void NetworkEndpointDialog::build()
{
    VSS_PROFILE_ZONE("gui.dialogs.network_endpoint.build");
    VSS_PROFILE_MESSAGE("gui.dialog.network_endpoint.opened");
    prepare_dialog(this, tr("Put a device on the network"), 760, 760);
    setMinimumWidth(640);

    // The dialog is a fixed header, a scrolling body of settings and a footer
    // holding the buttons.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 0);
    outer->setSpacing(12);

    auto* header = new QVBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(4);
    header->addWidget(
        make_dialog_title(tr("Put a device on the network"), this));
    header->addWidget(make_dialog_subtitle(
        tr("Share a serial device with other computers over TCP, UDP or RFC "
           "2217."),
        this));
    outer->addLayout(header);

    // Everything the user can set lives in the scrolling body.
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAccessibleName(tr("Network settings"));
    auto* body = new QWidget(scroll);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    // Which real serial device to share.
    device_combo_ = new QComboBox(body);
    device_combo_->setObjectName(QStringLiteral("networkDeviceCombo"));
    device_row_ =
        new FormRow(tr("Device to share"), device_combo_,
                    tr("A real serial device plugged into this PC."), body);
    layout->addWidget(device_row_);

    // Transport choice. The ids 0, 1 and 2 are cast straight to
    // network_transport by the current_changed handler below.
    auto* transport_section = new QVBoxLayout();
    transport_section->setContentsMargins(0, 0, 0, 0);
    transport_section->setSpacing(6);
    transport_section->addWidget(make_field_label(tr("Share it using"), body));
    transport_control_ = new SegmentedControl(body);
    transport_control_->setObjectName(QStringLiteral("networkTransport"));
    transport_control_->setAccessibleName(tr("Share it using"));
    transport_control_->add_item(QStringLiteral("TCP"), 0);
    transport_control_->add_item(QStringLiteral("UDP"), 1);
    transport_control_->add_item(QStringLiteral("RFC 2217"), 2);
    transport_section->addWidget(transport_control_);
    layout->addLayout(transport_section);

    // Server or client role. update_role_ui() shows the matching address page.
    role_row_ = new QWidget(body);
    auto* role_layout = new QHBoxLayout(role_row_);
    role_layout->setContentsMargins(0, 0, 0, 0);
    role_layout->setSpacing(24);
    server_radio_ = new QRadioButton(
        tr("Wait for other computers to connect (server)"), role_row_);
    client_radio_ =
        new QRadioButton(tr("Connect to a server (client)"), role_row_);
    server_radio_->setAccessibleName(
        tr("Wait for other computers to connect (server)"));
    client_radio_->setAccessibleName(tr("Connect to a server (client)"));
    role_layout->addWidget(server_radio_);
    role_layout->addWidget(client_radio_);
    role_layout->addStretch(1);
    layout->addWidget(role_row_);

    // Address row: one page per role (listen address for a server, remote
    // address for a client) with the port field beside it.
    auto* address_grid = new QGridLayout();
    address_grid->setContentsMargins(0, 0, 0, 0);
    address_grid->setHorizontalSpacing(12);
    address_grid->setVerticalSpacing(0);
    address_stack_ = new QStackedWidget(body);
    auto* server_page = new QWidget(address_stack_);
    auto* server_layout = new QVBoxLayout(server_page);
    server_layout->setContentsMargins(0, 0, 0, 0);
    server_layout->setSpacing(0);
    listen_combo_ = new QComboBox(server_page);
    listen_combo_->setObjectName(QStringLiteral("networkListen"));
    listen_combo_->addItem(tr("All network interfaces (0.0.0.0)"),
                           QStringLiteral("0.0.0.0"));
    listen_combo_->addItem(tr("This PC only (127.0.0.1)"),
                           QStringLiteral("127.0.0.1"));
    listen_combo_->addItem(tr("A specific address"), QString());
    listen_custom_ = new QLineEdit(server_page);
    listen_custom_->setObjectName(QStringLiteral("networkListenCustom"));
    listen_custom_->setPlaceholderText(QStringLiteral("192.168.1.10"));
    listen_custom_->setAccessibleName(tr("A specific address"));
    listen_custom_->hide();
    auto* listen_host_widget = new QWidget(server_page);
    auto* listen_host_layout = new QVBoxLayout(listen_host_widget);
    listen_host_layout->setContentsMargins(0, 0, 0, 0);
    listen_host_layout->setSpacing(6);
    listen_host_layout->addWidget(listen_combo_);
    listen_host_layout->addWidget(listen_custom_);
    server_layout->addWidget(
        new FormRow(tr("Listen on"), listen_host_widget, {}, server_page));
    address_stack_->addWidget(server_page);

    auto* client_page = new QWidget(address_stack_);
    auto* client_layout = new QVBoxLayout(client_page);
    client_layout->setContentsMargins(0, 0, 0, 0);
    client_layout->setSpacing(0);
    address_edit_ = new QLineEdit(client_page);
    address_edit_->setObjectName(QStringLiteral("networkServerAddress"));
    address_edit_->setPlaceholderText(QStringLiteral("192.168.1.50"));
    client_layout->addWidget(
        new FormRow(tr("Server address"), address_edit_, {}, client_page));
    address_stack_->addWidget(client_page);
    address_grid->addWidget(address_stack_, 0, 0);

    port_edit_ = new QLineEdit(body);
    port_edit_->setObjectName(QStringLiteral("networkPort"));
    port_edit_->setText(QStringLiteral("4001"));
    port_edit_->setMaximumWidth(160);
    port_row_ = new FormRow(tr("Port"), port_edit_, {}, body);
    address_grid->addWidget(port_row_, 0, 1, Qt::AlignTop);
    address_grid->setColumnStretch(0, 1);
    layout->addLayout(address_grid);

    // Guidance, a live result sentence and the warnings that may apply.
    hint_ = new QLabel(
        tr("Choose This PC only (127.0.0.1) to test from this machine first."),
        body);
    hint_->setFont(Theme::instance().font(text_style::caption));
    hint_->setProperty("muted", true);
    hint_->setWordWrap(true);
    layout->addWidget(hint_);

    result_ = new ResultSentence(body);
    result_->setObjectName(QStringLiteral("networkResult"));
    layout->addWidget(result_);

    // Shown when this build cannot wait for connections (no server role).
    capability_note_ = new NoteBanner(NoteBanner::kind::warn, {}, body);
    capability_note_->set_text(
        tr("This build can connect to a server, but cannot wait for other "
           "computers to connect. Start the server on the other computer."));
    capability_note_->hide();
    layout->addWidget(capability_note_);

    // The firewall note and button are only shown on Windows when other
    // computers can reach the port; update_role_ui() decides.
    firewall_note_ = new NoteBanner(
        NoteBanner::kind::warn,
        tr("Allow Virtual Serial Studio through Windows Firewall, or other "
           "computers cannot connect."),
        body);
    firewall_note_->hide();
    layout->addWidget(firewall_note_);

    firewall_button_ = make_dialog_action_button(
        tr("Check Windows Firewall for this port"), "quiet", body);
    firewall_button_->setObjectName(QStringLiteral("networkFirewall"));
    firewall_button_->setAccessibleName(
        tr("Check Windows Firewall for this port"));
    if (backend_.caps().firewall_rule)
    {
        firewall_button_->setToolTip(
            tr("Shows what Windows Firewall does to this port. Nothing changes "
               "until you agree."));
    }
    else
    {
        firewall_button_->setToolTip(tr("Not available in this build"));
    }
    firewall_button_->hide();
    connect(firewall_button_, &QPushButton::clicked, this,
            [this] { show_firewall_dialog(); });
    layout->addWidget(firewall_button_, 0, Qt::AlignLeft);
    // RFC 2217 has no password, so warn when the address is open to anyone.
    open_note_ = new NoteBanner(
        NoteBanner::kind::warn,
        tr("RFC 2217 has no password. Anyone who can reach this port can use "
           "the device. Choose This PC only (127.0.0.1), or in Advanced add "
           "?allow=192.168.1.0/24 to the address to say who may connect."),
        body);
    open_note_->setObjectName(QStringLiteral("networkOpenWarning"));
    open_note_->hide();
    layout->addWidget(open_note_);

    // Address card: the generated address, an optional manual override and the
    // equivalent command line.
    auto* address_card = new QFrame(body);
    address_card->setProperty("card", true);
    polish(address_card);
    auto* address_layout = new QVBoxLayout(address_card);
    address_layout->setContentsMargins(14, 12, 14, 12);
    address_layout->setSpacing(10);
    auto* address_header = new QHBoxLayout();
    address_header->setContentsMargins(0, 0, 0, 0);
    address_header->setSpacing(8);
    address_header->addWidget(make_field_label(tr("Address"), address_card));
    address_header->addStretch(1);
    edit_address_ =
        make_dialog_action_button(tr("Edit address"), "quiet", address_card);
    edit_address_->setProperty("size", QStringLiteral("small"));
    edit_address_->setCheckable(true);
    polish(edit_address_);
    address_header->addWidget(edit_address_);
    address_layout->addLayout(address_header);

    address_block_ = new CodeBlock(address_card);
    address_block_->set_text(QStringLiteral("tcp-listen://0.0.0.0:4001"),
                             tr("Address"));
    address_layout->addWidget(address_block_);

    // Editor for overriding the generated address; while it is edited its
    // text wins over the fields above.
    override_panel_ = new QWidget(address_card);
    auto* override_layout = new QVBoxLayout(override_panel_);
    override_layout->setContentsMargins(0, 0, 0, 0);
    override_layout->setSpacing(4);
    override_edit_ = new QLineEdit(override_panel_);
    override_edit_->setObjectName(QStringLiteral("networkAddressOverride"));
    override_edit_->setAccessibleName(tr("Address"));
    override_edit_->setPlaceholderText(
        QStringLiteral("tcp://192.168.1.50:4001"));
    override_layout->addWidget(override_edit_);
    auto* override_hint = new QLabel(
        tr("The address wins over the fields above while it is edited."),
        override_panel_);
    override_hint->setFont(Theme::instance().font(text_style::caption));
    override_hint->setProperty("muted", true);
    override_hint->setWordWrap(true);
    override_layout->addWidget(override_hint);
    override_panel_->hide();
    address_layout->addWidget(override_panel_);

    address_layout->addWidget(
        make_field_label(tr("Same thing from the command line"), address_card));
    command_block_ = new CodeBlock(address_card);
    command_block_->set_text(QString(), tr("Command"));
    address_layout->addWidget(command_block_);
    layout->addWidget(address_card);

    // Errors from submitting appear at the end of the body.
    error_ = new InlineError(body);
    layout->addWidget(error_);
    layout->addStretch(1);

    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    // Footer: the test shortcut, then Cancel and the default Start bridge
    // button.
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    test_ = make_dialog_action_button(tr("Test from this PC first"), "quiet",
                                      footer);
    footer_layout->insertWidget(0, test_);
    auto* cancel = make_dialog_action_button(tr("Cancel"), nullptr, footer);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    footer_layout->addWidget(cancel);
    primary_ = make_dialog_action_button(tr("Start bridge"), "primary", footer);
    primary_->setDefault(true);
    connect(primary_, &QPushButton::clicked, this,
            [this] { static_cast<void>(submit()); });
    footer_layout->addWidget(primary_);
    outer->addWidget(footer);

    // Wire up the controls. Each edit clears any old error and refreshes the
    // result sentence and the address blocks.
    connect(transport_control_, &SegmentedControl::current_changed, this,
            [this](int id)
            {
                transport_ = static_cast<network_transport>(id);
                clear_error();
                update_role_ui();
            });
    connect(server_radio_, &QRadioButton::toggled, this,
            [this](bool checked)
            {
                if (checked)
                {
                    role_ = network_role::server;
                    update_role_ui();
                }
            });
    connect(client_radio_, &QRadioButton::toggled, this,
            [this](bool checked)
            {
                if (checked)
                {
                    role_ = network_role::client;
                    update_role_ui();
                }
            });
    connect(device_combo_, &QComboBox::currentIndexChanged, this,
            [this](int index)
            {
                device_path_ = index >= 0
                                   ? device_combo_->itemData(index).toString()
                                   : QString();
                device_row_->clear_error();
                clear_error();
                update_result();
                update_address_blocks();
            });
    connect(address_edit_, &QLineEdit::textChanged, this,
            [this](const QString&)
            {
                clear_error();
                update_result();
                update_address_blocks();
            });
    connect(listen_combo_, &QComboBox::currentIndexChanged, this,
            [this](int)
            {
                listen_custom_->setVisible(
                    listen_combo_->currentData().toString().isEmpty());
                clear_error();
                update_result();
                update_address_blocks();
            });
    connect(listen_custom_, &QLineEdit::textChanged, this,
            [this](const QString&)
            {
                clear_error();
                update_result();
                update_address_blocks();
            });
    connect(port_edit_, &QLineEdit::textChanged, this,
            [this](const QString&)
            {
                port_row_->clear_error();
                clear_error();
                update_result();
                update_address_blocks();
            });
    // Entering edit mode seeds the override with the current address; leaving
    // it drops the override so the fields rule again.
    connect(edit_address_, &QPushButton::toggled, this,
            [this](bool editing)
            {
                override_panel_->setVisible(editing);
                if (editing)
                {
                    override_edit_->setText(effective_uri());
                    override_edit_->selectAll();
                    override_edit_->setFocus(Qt::OtherFocusReason);
                }
                else
                {
                    override_ = false;
                    override_edit_->clear();
                }
                update_result();
                update_address_blocks();
            });
    connect(override_edit_, &QLineEdit::textEdited, this,
            [this](const QString&)
            {
                override_ = true;
                override_edit_->setProperty("invalid", false);
                polish(override_edit_);
                clear_error();
                update_result();
                update_address_blocks();
            });
    // Test from this PC first: switch to server mode on 127.0.0.1 without
    // firing the radio handler, then update the UI once.
    connect(test_, &QPushButton::clicked, this,
            [this]
            {
                if (!server_available_)
                    return;
                const QSignalBlocker blocker(server_radio_);
                server_radio_->setChecked(true);
                role_ = network_role::server;
                const int index =
                    listen_combo_->findData(QStringLiteral("127.0.0.1"));
                if (index >= 0)
                    listen_combo_->setCurrentIndex(index);
                update_role_ui();
            });
}

void NetworkEndpointDialog::reload_devices()
{
    VSS_PROFILE_ZONE("gui.dialogs.network_endpoint.reload_devices");
    devices_ = backend_.devices();
    device_combo_->clear();
    device_path_.clear();

    QString preferred;
    if (preselected_ != 0)
    {
        if (const auto port = backend_.port(preselected_))
            preferred = port->path;
    }
    preselected_ = 0;

    for (const device_view& device : devices_)
    {
        if (device.com0com)
            continue;
        const QString label = device_label(device);
        device_combo_->addItem(label, device.path);
        if (!preferred.isEmpty() &&
            device.path.compare(preferred, Qt::CaseInsensitive) == 0)
        {
            device_combo_->setCurrentIndex(device_combo_->count() - 1);
            device_path_ = device.path;
        }
    }
    if (device_path_.isEmpty() && device_combo_->count() > 0)
    {
        device_combo_->setCurrentIndex(0);
        device_path_ = device_combo_->itemData(0).toString();
    }
    if (device_combo_->count() == 0)
    {
        device_row_->set_error(
            tr("No serial device is available. Plug one in, then try again."));
    }
    update_result();
    update_address_blocks();
    update_role_ui();
}

QString NetworkEndpointDialog::device_label(const device_view& device) const
{
    const QString path = device.path;
    return tr("%1, %2 (%3)")
        .arg(path, tr("real device"),
             serial_short_label(vss::serial_settings{}));
}

void NetworkEndpointDialog::set_advanced(bool advanced)
{
    advanced_ = advanced;
    if (edit_address_ == nullptr)
        return;
    edit_address_->setVisible(advanced_);
    if (!advanced_)
    {
        const QSignalBlocker blocker(edit_address_);
        edit_address_->setChecked(false);
        override_panel_->hide();
        override_ = false;
        override_edit_->clear();
    }
    update_result();
    update_address_blocks();
}

void NetworkEndpointDialog::set_transport(network_transport transport)
{
    transport_ = transport;
    const QSignalBlocker blocker(transport_control_);
    transport_control_->set_current(static_cast<int>(transport));
    update_role_ui();
}

void NetworkEndpointDialog::set_role(network_role role)
{
    if (role == network_role::server && !server_available_)
        role = network_role::client;
    role_ = role;
    const QSignalBlocker server_blocker(server_radio_);
    const QSignalBlocker client_blocker(client_radio_);
    server_radio_->setChecked(role_ == network_role::server);
    client_radio_->setChecked(role_ == network_role::client);
    update_role_ui();
}

bool NetworkEndpointDialog::server_selected() const
{
    return server_available_ && role_ == network_role::server;
}

void NetworkEndpointDialog::update_role_ui()
{
    if (role_row_ == nullptr)
        return;
    role_row_->setVisible(true);

    server_radio_->setEnabled(server_available_);
    server_radio_->setToolTip(
        server_available_ ? QString() : tr("Not available in this build"));
    if (role_ == network_role::server && !server_available_)
        role_ = network_role::client;
    {
        const QSignalBlocker server_blocker(server_radio_);
        const QSignalBlocker client_blocker(client_radio_);
        server_radio_->setChecked(role_ == network_role::server);
        client_radio_->setChecked(role_ == network_role::client);
    }

    address_stack_->setCurrentIndex(server_selected() ? 0 : 1);
    capability_note_->setVisible(!server_available_);
    test_->setEnabled(server_available_);
    test_->setToolTip(server_available_ ? QString()
                                        : tr("Not available in this build"));
    if (primary_ != nullptr)
    {
        primary_->setEnabled(backend_.caps().bridge &&
                             device_combo_->count() > 0);
    }

    update_warning();
    update_result();
    update_address_blocks();
}

void NetworkEndpointDialog::update_warning()
{
    const bool windows = windows_platform(backend_.facts());
    // Windows Firewall only matters for a bind that other computers can reach.
    const bool reachable =
        server_selected() &&
        !vss::permissions::is_loopback_host(
            listen_host(listen_combo_, listen_custom_).toStdString());
    firewall_note_->setVisible(windows && reachable);
    firewall_button_->setVisible(windows && reachable);
    if (open_note_ != nullptr)
        open_note_->setVisible(rfc2217_server_is_open(effective_uri()));
}

void NetworkEndpointDialog::show_firewall_dialog()
{
    VSS_PROFILE_ZONE("gui.dialogs.network_endpoint.firewall_dialog");
    // Ask now, for the listener as it is in the form.  A transport without a
    // server role (RFC 2217) never gets here.
    const auto facts = vss::permissions::probe(
        support::permission_context_for(effective_uri()));
    const auto fact = support::firewall_fact(facts);
    if (!fact)
        return;
    support::FirewallDialog dialog(*fact,
                                   backend_.caps().firewall_rule &&
                                       support::firewall_step_available(),
                                   this);
    dialog.exec();
}

void NetworkEndpointDialog::update_result()
{
    VSS_PROFILE_ZONE("gui.dialogs.network_endpoint.update_result");
    if (result_ == nullptr)
        return;
    const QString device =
        device_path_.isEmpty() ? tr("the device") : device_path_;
    const QString host = server_selected()
                             ? listen_host(listen_combo_, listen_custom_)
                             : address_edit_->text().trimmed();
    int port_number = 0;
    parse_port(port_edit_->text(), port_number);

    QString rich;
    if (server_selected())
    {
        rich = tr("Other computers can connect to this PC on port %1 and talk "
                  "to %2. Data flows both ways, and the Monitor shows both "
                  "directions.")
                   .arg(bold_name(QString::number(port_number)),
                        bold_name(device));
    }
    else
    {
        rich = tr("Studio connects to %1 on port %2 and talks to %3. Data "
                  "flows both ways, and the Monitor shows both directions.")
                   .arg(host.isEmpty() ? tr("the server") : bold_name(host),
                        bold_name(QString::number(port_number)),
                        bold_name(device));
    }
    result_->set_result_text(
        rich, QTextDocumentFragment::fromHtml(rich).toPlainText());
}

QString NetworkEndpointDialog::build_fields_uri() const
{
    int port_number = 0;
    parse_port(port_edit_->text(), port_number);
    if (server_selected())
    {
        return build_network_uri(transport_, network_role::server,
                                 listen_host(listen_combo_, listen_custom_),
                                 port_number);
    }
    return build_network_uri(transport_, network_role::client,
                             address_edit_->text().trimmed(), port_number);
}

QString NetworkEndpointDialog::effective_uri() const
{
    if (override_ && override_edit_ != nullptr &&
        !override_edit_->text().trimmed().isEmpty())
    {
        return override_edit_->text().trimmed();
    }
    return build_fields_uri();
}

QString NetworkEndpointDialog::uri() const
{
    return effective_uri();
}

QString NetworkEndpointDialog::source_uri() const
{
    if (device_path_.isEmpty())
        return {};
    if (device_path_.contains(QStringLiteral("://")))
        return device_path_;
    return serial_uri(device_path_, vss::serial_settings{});
}

QString NetworkEndpointDialog::command() const
{
    return QStringLiteral("vss_cli bridge %1 %2")
        .arg(source_uri(), effective_uri());
}

void NetworkEndpointDialog::update_address_blocks()
{
    if (address_block_ == nullptr)
        return;
    address_block_->set_text(effective_uri(), tr("Address"));
    command_block_->set_text(command(), tr("Command"));
    if (!override_)
        override_edit_->setText(effective_uri());
    // The address decides whether other computers can reach the server.
    update_warning();
}

QString NetworkEndpointDialog::result_text() const
{
    return result_ == nullptr ? QString() : result_->text();
}

QString NetworkEndpointDialog::accessible_result_text() const
{
    return result_ == nullptr ? QString() : result_->accessibleName();
}

QString NetworkEndpointDialog::error_title() const
{
    return error_ == nullptr ? QString() : error_->title();
}

QString NetworkEndpointDialog::error_body() const
{
    return error_ == nullptr ? QString() : error_->body();
}

bool NetworkEndpointDialog::error_visible() const
{
    return error_ != nullptr && error_->is_showing();
}

bool NetworkEndpointDialog::validate(QString& error) const
{
    if (device_path_.isEmpty())
    {
        error = tr("Choose a device to share.");
        return false;
    }
    if (advanced_ && override_)
    {
        QString parse_error;
        if (!parse_network_uri(override_edit_->text(), &parse_error))
        {
            error = std::move(parse_error);
            return false;
        }
        return true;
    }

    int port_number = 0;
    if (!parse_port(port_edit_->text(), port_number))
    {
        error = network_port_error();
        return false;
    }
    if (server_selected())
    {
        const QString host = listen_host(listen_combo_, listen_custom_);
        if (!valid_listen_host(host))
        {
            error = network_address_error();
            return false;
        }
        return true;
    }
    const QString host = address_edit_->text().trimmed();
    if (!valid_remote_host(host))
    {
        error = network_address_error();
        return false;
    }
    return true;
}

bool NetworkEndpointDialog::submit()
{
    clear_error();
    if (!backend_.caps().bridge)
    {
        last_error_ = tr("Networking is not available in this build.");
        error_->show_error(tr("Could not start the bridge"), last_error_);
        return false;
    }

    QString validation_error;
    if (!validate(validation_error))
    {
        last_error_ = validation_error;
        if (validation_error == network_port_error())
            port_row_->set_error(validation_error);
        if (device_path_.isEmpty())
            device_row_->set_error(validation_error);
        error_->show_error(tr("Could not start the bridge"), validation_error);
        return false;
    }

    const QString source = source_uri();
    const QString target = effective_uri();
    const int port_number = network_port_from_uri(target);
    const auto linked = backend_.bridge(source, target);
    if (!linked)
    {
        last_error_ = error_text(linked.error());
        if (looks_like_address_in_use(linked.error()))
        {
            error_->show_error(
                tr("Could not start the bridge"),
                tr("Port %1 is already in use. Choose another port, or stop "
                   "the program using it.")
                    .arg(port_number),
                last_error_);
        }
        else
        {
            error_->show_error(
                tr("Could not start the bridge"),
                tr("The address may be in use, or the other computer refused "
                   "the connection. Check the address and try again."),
                last_error_);
        }
        return false;
    }

    result_link_ = *linked;
    result_port_ = port_number;
    Q_EMIT bridge_started();
    QDialog::accept();
    return true;
}

void NetworkEndpointDialog::clear_error()
{
    last_error_.clear();
    if (error_ != nullptr)
        error_->clear();
    if (port_row_ != nullptr)
        port_row_->clear_error();
    if (device_row_ != nullptr)
        device_row_->clear_error();
    if (override_edit_ != nullptr)
    {
        override_edit_->setProperty("invalid", false);
        polish(override_edit_);
    }
}

QStringList NetworkEndpointDialog::device_paths() const
{
    QStringList paths;
    for (int index = 0; index < device_combo_->count(); ++index)
        paths << device_combo_->itemData(index).toString();
    return paths;
}

bool NetworkEndpointDialog::set_device_path(const QString& path)
{
    for (int index = 0; index < device_combo_->count(); ++index)
    {
        if (device_combo_->itemData(index).toString() == path)
        {
            device_combo_->setCurrentIndex(index);
            device_path_ = path;
            return true;
        }
    }
    return false;
}

QString NetworkEndpointDialog::device_path() const
{
    return device_path_;
}

void NetworkEndpointDialog::set_server_address(const QString& address)
{
    if (role_ == network_role::server && server_available_)
        set_role(network_role::client);
    address_edit_->setText(address);
    update_role_ui();
}

QString NetworkEndpointDialog::server_address() const
{
    return address_edit_->text().trimmed();
}

void NetworkEndpointDialog::set_listen_address(const QString& address)
{
    const int index = listen_combo_->findData(address);
    if (index >= 0)
    {
        listen_combo_->setCurrentIndex(index);
    }
    else
    {
        listen_combo_->setCurrentIndex(listen_combo_->count() - 1);
        listen_custom_->setText(address);
    }
    update_role_ui();
}

QString NetworkEndpointDialog::listen_address() const
{
    return listen_host(listen_combo_, listen_custom_);
}

void NetworkEndpointDialog::set_port(int port)
{
    port_edit_->setText(QString::number(port));
}

int NetworkEndpointDialog::port() const
{
    int port_number = 0;
    parse_port(port_edit_->text(), port_number);
    return port_number;
}

void NetworkEndpointDialog::set_address_override(const QString& address)
{
    advanced_ = true;
    edit_address_->setVisible(true);
    const QSignalBlocker blocker(edit_address_);
    edit_address_->setChecked(true);
    override_panel_->show();
    override_edit_->setText(address);
    override_ = true;
    update_result();
    update_address_blocks();
}

void NetworkEndpointDialog::clear_address_override()
{
    const QSignalBlocker blocker(edit_address_);
    edit_address_->setChecked(false);
    override_panel_->hide();
    override_ = false;
    override_edit_->clear();
    update_result();
    update_address_blocks();
}

QString NetworkEndpointDialog::address_override() const
{
    return override_edit_->text().trimmed();
}

} // namespace vss::gui
