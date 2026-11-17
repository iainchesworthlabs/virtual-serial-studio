//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_PORTS_SERIAL_TRANSPORT_HPP
#define VSS_PORTS_SERIAL_TRANSPORT_HPP

#include <vss/core/control_lines.hpp>
#include <vss/core/serial_settings.hpp>
#include <vss/core/transport.hpp>
#include <vss/core/types.hpp>

#include <boost/virtual_serial.hpp>

#include <memory>
#include <string>

namespace vss {

/// A transport backed by `boost::virtual_serial::any_serial_port`.
///
/// This is the bridge between the studio and the serial library: the same
/// class carries a real UART, an RFC 2217 session, a raw TCP/UDP bridge and
/// the scriptable mock device.  Which one it is comes from the URI:
///
///     serial://COM3?baud=115200
///     rfc2217://lab-host:7001?baud=115200
///     tcp://192.168.1.50:4001
///     udp://192.168.1.50:4002
///     mock://scripted
class serial_transport final : public transport
{
public:
    /// Opens the port named by `uri`.
    serial_transport(const executor_type& executor, std::string uri,
                     error_code& ec);

    /// Opens the port named by `uri`, then applies `settings` on top of the
    /// URI query.
    serial_transport(const executor_type& executor, std::string uri,
                     const serial_settings& settings, error_code& ec);

    /// Creates a scriptable mock device.  The returned transport keeps the
    /// fixture alive; #mock() exposes it for injection.
    static std::shared_ptr<serial_transport> make_mock(
        const executor_type& executor, const serial_settings& settings,
        const std::string& target, error_code& ec);

    ~serial_transport() override;

    std::string describe() const override;
    std::string public_path() const override;
    bool is_open() const override;

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override;
    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override;
    void close() override;
    void cancel() override;

    bool is_line_error(boost::system::error_code ec) const noexcept override;
    std::optional<line_format> peer_format() const override;

    bool supports_control_lines() const noexcept override;
    void set_control_line(control_line line, bool state) override;
    control_lines lines() const override;
    void refresh_lines() override;

    endpoint_kind kind() const noexcept { return kind_; }
    const std::string& uri() const noexcept { return uri_; }

    /// The OS handle behind a physical port, or 0.  Exposed so control line
    /// helpers and diagnostics can reach the device directly.
    std::intptr_t native_handle() const noexcept
    {
        return port_ ? port_->native_handle() : 0;
    }

    /// The mock fixture, or nullptr for every other backend.
    boost::virtual_serial::mock_port* mock() const noexcept { return mock_; }

private:
    serial_transport(const executor_type& executor,
                     std::unique_ptr<boost::virtual_serial::any_serial_port> port,
                     std::string uri, endpoint_kind kind,
                     boost::virtual_serial::mock_port* mock);

    executor_type executor_;
    std::unique_ptr<boost::virtual_serial::any_serial_port> port_;
    std::string uri_;
    std::string target_;
    serial_settings settings_;
    endpoint_kind kind_ = endpoint_kind::physical;
    boost::virtual_serial::mock_port* mock_ = nullptr;
    control_lines lines_;
};

/// Converts the studio settings into the serial library's configuration.
boost::virtual_serial::serial_config to_serial_config(
    const serial_settings& settings);

/// The backend a URI selects, for labelling and validation.
result<endpoint_kind> classify_uri(std::string_view uri);

} // namespace vss

#endif // VSS_PORTS_SERIAL_TRANSPORT_HPP
