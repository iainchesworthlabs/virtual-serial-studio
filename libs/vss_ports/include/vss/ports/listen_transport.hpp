//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/core/transport.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vss
{

enum class listen_protocol
{
    tcp,
    udp,
    /// TCP carrying Telnet with the COM-PORT option: an RFC 2217 access
    /// server.
    rfc2217,
};

/// Where a listening endpoint waits for other computers.
struct listen_address
{
    listen_protocol protocol = listen_protocol::tcp;
    /// An IP address; empty means every IPv4 interface.
    std::string host;
    /// 0 asks the system for a free port.
    std::uint16_t port = 0;
    /// Who may connect: IP addresses and CIDR ranges such as `192.168.1.0/24`
    /// or `fd00::/8`. Empty admits everyone. A TCP or RFC 2217 client outside
    /// the list is closed at once, and a UDP datagram from outside it is
    /// ignored.
    std::vector<std::string> allow;

    friend bool operator==(const listen_address&,
                           const listen_address&) = default;
};

/// What a listening port puts up with from its clients. The defaults suit the
/// studio; a test can lower them.
struct listen_limits
{
    /// Clients connected at once. A connection over the limit is closed as
    /// soon as it is accepted. Each client costs a descriptor, a 4 KiB read
    /// buffer and up to #max_client_queue_bytes of queued output, so 32 is
    /// at most 32 MiB of queues. That is more computers than one serial
    /// device is shared with, and few enough that a script opening
    /// connections cannot use up the process's descriptors.
    std::size_t max_clients = 32;

    /// Bytes waiting to be sent to one client, payload and RFC 2217 replies
    /// together. A client that falls further behind is disconnected, so one
    /// client that stops reading cannot hold memory without limit or stop the
    /// others. The endpoint holds up to 8 MiB for the whole port
    /// (`endpoint_options::max_write_queue_bytes`); one client may be 1 MiB
    /// behind, which is about 90 seconds of a serial line running flat out at
    /// 115200 baud.
    std::size_t max_client_queue_bytes = std::size_t{1} << 20;

    /// How long a client with data waiting may take none of it. Not a limit on
    /// how long a transfer lasts: every part the client takes starts the time
    /// again. A hung program is found in half a minute, and a slow link is
    /// not mistaken for one, since a part is 64 KiB at most.
    std::chrono::milliseconds write_deadline{30000};

    /// The wait after the first failed accept (for example when the process is
    /// out of descriptors). It doubles on each further failure up to the
    /// last. Without a wait, a connection that cannot be accepted stays in the
    /// backlog and accept fails again at once, using a whole processor.
    std::chrono::milliseconds accept_retry_first{100};
    std::chrono::milliseconds accept_retry_last{5000};
};

/// The wait before accept is tried again after `failures` failures in a row
/// (0 for the first): the first wait, doubled each time, up to the last.
std::chrono::milliseconds accept_retry_delay(std::size_t failures,
                                             const listen_limits& limits);

/// True for `tcp-listen://`, `udp-listen://` and `rfc2217-listen://`, the
/// schemes of an endpoint that waits for others to connect instead of
/// connecting itself.
bool is_listen_uri(std::string_view uri) noexcept;

/// Parses `tcp-listen://host:port`, `udp-listen://host:port` and
/// `rfc2217-listen://host:port`. The host is
/// an IP address, `[::1]` for IPv6, or empty for every IPv4 interface. The
/// only thing a URI may add is `?allow=a,b,c`, a comma separated list of
/// addresses and CIDR ranges (see #listen_address::allow). Anything else after
/// the port is an error.
result<listen_address> parse_listen_uri(std::string_view uri);

/// True when `list` admits `address` (an IP address in text form). An empty
/// list admits everyone and a bad address is never admitted. An IPv4 address
/// written as IPv6 (`::ffff:192.0.2.1`) counts as the IPv4 address.
bool allow_list_admits(const std::vector<std::string>& list,
                       std::string_view address);

/// True when the address listens on this computer only: `127.0.0.0/8` or
/// `::1`. An empty host (every interface) is not.
bool listens_on_loopback(const listen_address& address);

/// The URI for an address, the inverse of parse_listen_uri.
std::string make_listen_uri(const listen_address& address);

/// A byte channel that waits for other computers.
///
/// TCP: up to #listen_limits::max_clients clients may connect. What they send
/// is read by the hub in the order it arrives, and what the hub writes goes to
/// every client that is connected at that moment. With no client connected a
/// write succeeds and the bytes are dropped, as on a serial port with nothing
/// plugged in. A write completes when its bytes are queued for the clients,
/// not when they have been taken. A client that falls behind
/// #listen_limits::max_client_queue_bytes, or takes nothing for
/// #listen_limits::write_deadline, is disconnected with a notice (see
/// #transport::set_notice_sink); the others are not held back.
///
/// UDP: datagrams from anyone are read, and a write is sent to every sender
/// heard from lately (up to sixteen).
///
/// RFC 2217: TCP clients speak Telnet and negotiate the COM-PORT option, so a
/// serial-over-network client (pySerial's `rfc2217://`, a terminal server
/// driver) can open the endpoint as a serial port. Payload bytes travel as
/// they do for TCP, with 0xFF escaped. The client's baud rate, framing, flow
/// control, DTR, RTS and break are accepted and remembered but change no real
/// device; DTR and RTS are the endpoint's output lines, and CTS, DSR, DCD and
/// RI that reach the endpoint are sent to the clients as modem state.
class listen_transport final : public transport
{
public:
    /// Binds and starts listening. `ec` is set when the address is in use or
    /// cannot be bound.
    listen_transport(const executor_type& executor, listen_address address,
                     error_code& ec, listen_limits limits = {});
    ~listen_transport() override;

    listen_transport(const listen_transport&) = delete;
    listen_transport& operator=(const listen_transport&) = delete;

    std::string describe() const override;
    std::string public_path() const override;
    bool is_open() const override;

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override;
    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override;

    void close() override;
    void cancel() override;

    /// True for the RFC 2217 role, which has control lines.
    bool supports_control_lines() const noexcept override;
    void set_control_line(control_line line, bool state) override;
    control_lines lines() const override;

    /// The port actually bound, which differs from the request when that was 0.
    std::uint16_t local_port() const noexcept;

    /// TCP clients connected now, or UDP senders heard from.
    std::size_t peer_count() const noexcept;

    /// The same count, as #transport::client_count.
    std::optional<std::size_t> client_count() const noexcept override;

    /// TCP connections closed, and UDP datagrams ignored, because the allow
    /// list did not admit the sender, and TCP connections closed because the
    /// port already had its most clients.
    std::size_t refused_count() const noexcept;

    /// Reports the clients the transport disconnects or refuses, and a failing
    /// accept, by their addresses. Called on the transport's strand; not called
    /// after close().
    void set_notice_sink(notice_sink sink) override;

private:
    class state;
    std::shared_ptr<state> state_;
};

} // namespace vss
