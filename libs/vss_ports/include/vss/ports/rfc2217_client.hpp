//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/control_lines.hpp>
#include <vss/core/serial_settings.hpp>
#include <vss/core/transport.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>

namespace vss {

/// True for an `rfc2217://host:port` address, the one this transport opens.
bool is_rfc2217_client_uri(std::string_view uri) noexcept;

/// A serial port on another computer, reached as an RFC 2217 client
/// (Telnet Com Port Control): `rfc2217://host:port?baud=115200&data=8&parity=none&stop=1`.
///
/// Studio connects to the address the user gave and to nothing else. The
/// transport negotiates the COM-PORT option, and when the server agrees it
///
/// - sends the baud rate, data bits, parity, stop bits and flow control
///   (SET-BAUDRATE, SET-DATASIZE, SET-PARITY, SET-STOPSIZE, SET-CONTROL), and
///   again whenever #apply_peer_format changes them;
/// - sets DTR and RTS on the server when the endpoint does (SET-CONTROL);
/// - asks for the modem lines (SET-MODEMSTATE-MASK) and shows CTS, DSR, DCD
///   and RI as the endpoint's inputs from NOTIFY-MODEMSTATE;
/// - asks for the faults of the line state (SET-LINESTATE-MASK 0x1E): a
///   NOTIFY-LINESTATE with the break bit set is one break item that did not
///   measure its length, in order with the bytes before it, and one with a
///   framing, parity or overrun bit is a fault of the line;
/// - sends a break item as SET-CONTROL break on, a wait of the item's length
///   on a timer, and break off.
///
/// A server that refuses the COM-PORT option is used as a plain byte stream
/// and the log says so. Payload bytes travel as in Telnet, with 0xFF doubled.
///
/// The connection is made when the transport is made, as it is for `tcp://`:
/// the constructor waits for the connect and reports a failure in `ec`. A
/// connection that is lost completes the read with an error, and the studio
/// opens the address again by its reconnect policy, as for every network
/// client.
class rfc2217_client_transport final : public transport
{
public:
    /// Connects to the address of `uri`. The settings come from its query.
    rfc2217_client_transport(const executor_type& executor, std::string uri,
                             error_code& ec);
    /// Connects, then applies `settings` on top of the URI's query.
    rfc2217_client_transport(const executor_type& executor, std::string uri,
                             const serial_settings& settings, error_code& ec);
    ~rfc2217_client_transport() override;

    rfc2217_client_transport(const rfc2217_client_transport&) = delete;
    rfc2217_client_transport& operator=(const rfc2217_client_transport&) =
        delete;

    std::string describe() const override;
    std::string public_path() const override;
    bool is_open() const override;

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override;
    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override;
    void close() override;
    void cancel() override;

    /// DTR and RTS are sent to the server. CTS, DSR, DCD and RI are what the
    /// server last said.
    bool supports_control_lines() const noexcept override { return true; }
    void set_control_line(control_line line, bool state) override;
    control_lines lines() const override;

    /// A framing, parity or overrun notice from the server is a fault of the
    /// line: the read fails with it and the next read carries on.
    bool is_line_error(boost::system::error_code ec) const noexcept override;

    void set_notice_sink(notice_sink sink) override;
    void set_break_sink(break_sink sink) override;
    bool can_deliver_break() const noexcept override { return true; }
    void async_write_break(const line_break& item, io_handler handler) override;

    /// Sends the baud rate and framing of `format` to the server when it
    /// differs from what was sent: what a program set on the other side of
    /// the link (#transport::apply_peer_format), so the server's port runs as
    /// that program expects.
    void apply_peer_format(const line_format& format) override;

    /// The settings the transport sends, as they are now.
    serial_settings settings() const;

private:
    struct state;

    rfc2217_client_transport(const executor_type& executor, std::string uri,
                             const serial_settings* settings, error_code& ec);
    state& impl() const noexcept { return *state_; }

    std::shared_ptr<state> state_;
};

} // namespace vss
