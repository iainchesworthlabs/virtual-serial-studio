//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_PORTS_RFC2217_HPP
#define VSS_PORTS_RFC2217_HPP

#include <vss/core/control_lines.hpp>
#include <vss/core/serial_settings.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::rfc2217
{

/// What one chunk of bytes from a client turned into.
struct feed_result
{
    /// The payload for the hub, with the Telnet commands taken out and every
    /// doubled 0xFF turned back into one.
    std::string data;
    /// Bytes to send back to the client: negotiation and acknowledgements.
    std::string reply;
    /// The client changed DTR, RTS or break.
    bool outputs_changed = false;
    /// The client turned break on (SET-CONTROL 5) or off (6), in the order it
    /// did, each with `at` the number of payload bytes in `data` that came
    /// before it, so a break keeps its place among the bytes.
    struct break_command
    {
        std::size_t at = 0;
        bool on = false;
    };
    std::vector<break_command> breaks;
    /// The client asked for the modem state (it set a mask), so the server
    /// should send it with `session::modem_state(inputs, true)`.
    bool modem_state_requested = false;
};

/// The server side of one RFC 2217 connection: a Telnet byte stream with the
/// COM-PORT option (Telnet Com Port Control, RFC 2217) negotiated on top.
///
/// A session holds no socket and does no I/O. Feed it what the client sent and
/// send what it returns, which keeps the protocol testable on its own. The
/// client's requests (baud rate, framing, flow control, DTR, RTS, break) are
/// accepted and remembered. Nothing here changes a real device: the session
/// answers as a serial port would, and the transport that owns it says which
/// lines the client drove.
class session
{
public:
    session();

    /// The negotiation to send as soon as the client connects.
    std::string greeting();

    feed_result feed(std::string_view bytes);

    /// Escapes the 0xFF bytes of `data` so a client reads them as data.
    static std::string encode(std::string_view data);

    /// The NOTIFY-MODEMSTATE for the peer's input lines, or empty when none of
    /// the lines the client asked about changed since the last one. `force`
    /// sends the current state whatever changed.
    std::string modem_state(control_lines inputs, bool force = false);

    /// The settings the client asked for, starting from 9600 8N1.
    const serial_settings& settings() const noexcept { return settings_; }
    /// DTR, RTS and break as the client set them.
    control_lines outputs() const noexcept { return outputs_; }
    /// True once the client has agreed to the COM-PORT option.
    bool com_port_active() const noexcept;

private:
    enum class parse_state
    {
        data,
        iac,
        verb,
        subnegotiation,
        subnegotiation_iac
    };

    enum class option_state
    {
        off,
        asked,
        on
    };

    struct option
    {
        option_state local = option_state::off;
        option_state remote = option_state::off;
    };

    void negotiate(unsigned char verb, unsigned char code, feed_result& result);
    void subnegotiate(const std::string& payload, feed_result& result);
    void command(unsigned char code, const std::string& value,
                 feed_result& result);
    /// Applies a SET-CONTROL request and returns the value to answer with.
    unsigned set_control(unsigned request, feed_result& result);
    option& option_for(unsigned char code);

    parse_state state_ = parse_state::data;
    unsigned char verb_ = 0;
    std::string subnegotiation_;
    option binary_;
    option suppress_go_ahead_;
    option com_port_;
    option unsupported_;
    serial_settings settings_;
    control_lines outputs_;
    control_lines last_inputs_;
    unsigned char modem_mask_ = 0xFF;
    unsigned char line_mask_ = 0;
};

/// What one chunk of bytes from an RFC 2217 server turned into.
struct client_feed
{
    /// The payload, with the Telnet commands taken out.
    std::string data;
    /// Bytes to send back to the server: negotiation answers.
    std::string reply;
    /// A NOTIFY-LINESTATE the server sent, with `at` the number of payload
    /// bytes in `data` before it, so a break keeps its place among the bytes.
    struct line_notice
    {
        std::size_t at = 0;
        unsigned char state = 0;
    };
    std::vector<line_notice> line_states;
    /// The last NOTIFY-MODEMSTATE in the chunk (state bits 0x10 to 0x80 and
    /// delta bits 0x01 to 0x08).
    std::optional<unsigned char> modem_state;
    /// The server agreed to the COM-PORT option, or refused it, in this
    /// chunk.
    bool com_port_changed = false;
};

/// The client side of one RFC 2217 connection: what Studio sends to a
/// serial-over-network server and how it reads the answers.
///
/// Like #session it holds no socket and does no I/O. The requests return the
/// bytes to send, and #feed reads what the server sent. A request is only
/// meaningful once #com_port_active is true; the transport that owns the
/// session waits for it.
class client_session
{
public:
    client_session() = default;

    /// The negotiation to send as soon as the connection is made: Studio
    /// offers and asks for COM-PORT, BINARY and SUPPRESS-GO-AHEAD.
    std::string greeting();

    client_feed feed(std::string_view bytes);

    /// Escapes the 0xFF bytes of `data` so a server reads them as data.
    static std::string encode(std::string_view data);

    /// True once the server has agreed to the COM-PORT option.
    bool com_port_active() const noexcept;
    /// True once the server has said it will not use the COM-PORT option.
    bool com_port_refused() const noexcept { return refused_; }

    std::string set_baud_rate(unsigned rate) const;
    std::string set_data_size(unsigned bits) const;
    std::string set_parity(parity_mode parity) const;
    std::string set_stop_size(stop_bits_mode stop) const;
    std::string set_flow_control(flow_control_mode flow) const;
    std::string set_dtr(bool on) const;
    std::string set_rts(bool on) const;
    std::string set_break(bool on) const;
    /// Which line-state bits the server notifies (0x10 is break).
    std::string set_line_state_mask(unsigned char mask) const;
    /// Which modem-state bits the server notifies. The server answers with
    /// the state as it is.
    std::string set_modem_state_mask(unsigned char mask) const;

    /// What the server says it is, from its SIGNATURE answer, or empty.
    const std::string& server_signature() const noexcept
    {
        return server_signature_;
    }

private:
    enum class parse_state
    {
        data,
        iac,
        verb,
        subnegotiation,
        subnegotiation_iac
    };

    struct option
    {
        bool asked = false;
        bool local = false;
        bool remote = false;
    };

    void negotiate(unsigned char verb, unsigned char code,
                   client_feed& result);
    void subnegotiate(const std::string& payload, client_feed& result);
    option& option_for(unsigned char code);

    parse_state state_ = parse_state::data;
    unsigned char verb_ = 0;
    std::string subnegotiation_;
    option binary_;
    option suppress_go_ahead_;
    option com_port_;
    option unsupported_;
    bool refused_ = false;
    std::string server_signature_;
};

} // namespace vss::rfc2217

#endif // VSS_PORTS_RFC2217_HPP
