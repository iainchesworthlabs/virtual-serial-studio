//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>
#include <vss/ports/listen_transport.hpp>
#include <vss/ports/native_lines.hpp>
#include <vss/ports/serial_transport.hpp>
#include <vss/trace/recorder.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/virtual_serial.hpp>

#include <utility>

namespace vss {
namespace vs = boost::virtual_serial;
namespace {

/// Opens a device through the serial library and records the call.  The
/// library makes several operating system calls inside `make_serial_port`
/// (CreateFile, then the line settings), and the error it returns is the raw
/// code of whichever failed; the recorder keeps that code with the target and
/// the baud rate asked for.
template <class Executor, class Configuration>
auto traced_open(const Executor& executor, const Configuration& configuration,
                 boost::system::error_code& ec)
{
    std::string args = configuration.target;
    args += " baud=" + std::to_string(configuration.baud_rate);
    trace::call_scope scope("serial.open", 0, args);
    auto port = vs::make_serial_port(executor, configuration, ec);
    scope.finish(ec);
    return port;
}

endpoint_kind map_kind(vs::backend_kind kind) noexcept
{
    switch (kind)
    {
    case vs::backend_kind::physical: return endpoint_kind::physical;
    case vs::backend_kind::mock: return endpoint_kind::mock;
    case vs::backend_kind::rfc2217: return endpoint_kind::rfc2217;
    case vs::backend_kind::tcp: return endpoint_kind::tcp;
    case vs::backend_kind::udp: return endpoint_kind::udp;
    }
    return endpoint_kind::physical;
}

/// A network client does not set the serial settings of the server's port: the
/// port belongs to the server, which has the device, and the client asks what
/// they are (RFC 2217 SET-BAUDRATE and the others, with the value zero).  A
/// URI that says `settings=own` keeps the old behaviour of setting them.
bool takes_device_settings(const vs::serial_config& configuration,
                           const std::string& uri) noexcept
{
    return configuration.backend == vs::backend_kind::rfc2217 &&
           uri.find("settings=own") == std::string::npos;
}

parity_mode from_vs(vs::parity_mode value) noexcept
{
    switch (value)
    {
    case vs::parity_mode::none: return parity_mode::none;
    case vs::parity_mode::odd: return parity_mode::odd;
    case vs::parity_mode::even: return parity_mode::even;
    case vs::parity_mode::mark: return parity_mode::mark;
    case vs::parity_mode::space: return parity_mode::space;
    }
    return parity_mode::none;
}

stop_bits_mode from_vs(vs::stop_bits_mode value) noexcept
{
    switch (value)
    {
    case vs::stop_bits_mode::one: return stop_bits_mode::one;
    case vs::stop_bits_mode::one_point_five:
        return stop_bits_mode::one_point_five;
    case vs::stop_bits_mode::two: return stop_bits_mode::two;
    }
    return stop_bits_mode::one;
}

vs::parity_mode to_vs(parity_mode value) noexcept
{
    switch (value)
    {
    case parity_mode::none: return vs::parity_mode::none;
    case parity_mode::odd: return vs::parity_mode::odd;
    case parity_mode::even: return vs::parity_mode::even;
    case parity_mode::mark: return vs::parity_mode::mark;
    case parity_mode::space: return vs::parity_mode::space;
    }
    return vs::parity_mode::none;
}

vs::stop_bits_mode to_vs(stop_bits_mode value) noexcept
{
    switch (value)
    {
    case stop_bits_mode::one: return vs::stop_bits_mode::one;
    case stop_bits_mode::one_point_five: return vs::stop_bits_mode::one_point_five;
    case stop_bits_mode::two: return vs::stop_bits_mode::two;
    }
    return vs::stop_bits_mode::one;
}

vs::flow_control_mode to_vs(flow_control_mode value) noexcept
{
    switch (value)
    {
    case flow_control_mode::none: return vs::flow_control_mode::none;
    case flow_control_mode::software: return vs::flow_control_mode::software;
    case flow_control_mode::hardware: return vs::flow_control_mode::hardware;
    }
    return vs::flow_control_mode::none;
}

} // namespace

vs::serial_config to_serial_config(const serial_settings& settings)
{
    vs::serial_config config;
    config.baud_rate = settings.baud_rate;
    config.data_bits = settings.data_bits;
    config.parity = to_vs(settings.parity);
    config.stop_bits = to_vs(settings.stop_bits);
    config.flow_control = to_vs(settings.flow_control);
    config.read_timeout = settings.read_timeout;
    config.write_timeout = settings.write_timeout;
    config.exclusive = settings.exclusive;
    return config;
}

result<endpoint_kind> classify_uri(std::string_view uri)
{
    VSS_PROFILE_ZONE("ports.serial.classify_uri");
    if (is_listen_uri(uri))
    {
        const auto address = parse_listen_uri(uri);
        if (!address)
            return address.error();
        switch (address->protocol)
        {
        case listen_protocol::tcp:
            return endpoint_kind::tcp;
        case listen_protocol::rfc2217:
            return endpoint_kind::rfc2217;
        case listen_protocol::udp:
            break;
        }
        return endpoint_kind::udp;
    }

    boost::system::error_code ec;
    const auto configuration = vs::parse_uri(std::string(uri), ec);
    if (ec)
        return ec;
    if (configuration.target.empty() && configuration.backend != vs::backend_kind::mock)
        return make_error_code(errc::invalid_uri);
    return map_kind(configuration.backend);
}

serial_transport::serial_transport(const executor_type& executor,
                                   std::string uri, error_code& ec)
    : executor_(executor)
    , uri_(std::move(uri))
{
    VSS_PROFILE_ZONE_NAMED(zone, "ports.serial.open");
    if (zone.active())
        zone.text(uri_);
    boost::system::error_code parse_ec;
    auto configuration = vs::parse_uri(uri_, parse_ec);
    if (parse_ec)
    {
        ec = parse_ec;
        return;
    }

    configuration.device_settings = takes_device_settings(configuration, uri_);
    kind_ = map_kind(configuration.backend);
    target_ = configuration.target;
    settings_.baud_rate = configuration.baud_rate;
    settings_.data_bits = configuration.data_bits;
    settings_.read_timeout = configuration.read_timeout;
    settings_.write_timeout = configuration.write_timeout;
    settings_.exclusive = configuration.exclusive;

    auto port = traced_open(executor_, configuration, parse_ec);
    if (parse_ec)
    {
        ec = parse_ec;
        return;
    }

    port_ = std::make_unique<vs::any_serial_port>(std::move(port));
    lines_.set(control_line::dtr, true);
    lines_.set(control_line::rts, true);
}

serial_transport::serial_transport(const executor_type& executor,
                                   std::string uri,
                                   const serial_settings& settings,
                                   error_code& ec)
    : executor_(executor)
    , uri_(std::move(uri))
{
    VSS_PROFILE_ZONE_NAMED(zone, "ports.serial.open");
    if (zone.active())
        zone.text(uri_);
    if (const error_code invalid = validate(settings))
    {
        ec = invalid;
        return;
    }

    boost::system::error_code parse_ec;
    auto configuration = vs::parse_uri(uri_, parse_ec);
    if (parse_ec)
    {
        ec = parse_ec;
        return;
    }

    configuration.device_settings = takes_device_settings(configuration, uri_);
    if (!configuration.device_settings)
    {
        configuration.baud_rate = settings.baud_rate;
        configuration.data_bits = settings.data_bits;
        configuration.parity = to_vs(settings.parity);
        configuration.stop_bits = to_vs(settings.stop_bits);
        configuration.flow_control = to_vs(settings.flow_control);
    }
    configuration.read_timeout = settings.read_timeout;
    configuration.write_timeout = settings.write_timeout;
    configuration.exclusive = settings.exclusive;

    kind_ = map_kind(configuration.backend);
    target_ = configuration.target;
    settings_ = settings;

    auto port = traced_open(executor_, configuration, parse_ec);
    if (parse_ec)
    {
        ec = parse_ec;
        return;
    }

    port_ = std::make_unique<vs::any_serial_port>(std::move(port));
    lines_.set(control_line::dtr, true);
    lines_.set(control_line::rts, true);
}

serial_transport::serial_transport(
    const executor_type& executor,
    std::unique_ptr<vs::any_serial_port> port, std::string uri,
    endpoint_kind kind, vs::mock_port* mock)
    : executor_(executor)
    , port_(std::move(port))
    , uri_(std::move(uri))
    , kind_(kind)
    , mock_(mock)
{
    lines_.set(control_line::dtr, true);
    lines_.set(control_line::rts, true);
}

std::shared_ptr<serial_transport> serial_transport::make_mock(
    const executor_type& executor, const serial_settings& settings,
    const std::string& target, error_code& ec)
{
    VSS_PROFILE_ZONE("ports.serial.make_mock");
    if (const error_code invalid = validate(settings))
    {
        ec = invalid;
        return nullptr;
    }

    auto configuration = to_serial_config(settings);
    configuration.backend = vs::backend_kind::mock;
    configuration.target = target;

    auto fixture = vs::make_mock_port(executor, configuration);
    auto port = std::make_unique<vs::any_serial_port>(executor, fixture);

    boost::system::error_code open_ec;
    port->open(target, open_ec);
    if (open_ec)
    {
        ec = open_ec;
        return nullptr;
    }

    auto self = std::shared_ptr<serial_transport>(new serial_transport(
        executor, std::move(port), "mock://" + target, endpoint_kind::mock,
        fixture.get()));
    self->target_ = target;
    self->settings_ = settings;
    return self;
}

serial_transport::~serial_transport()
{
    if (port_)
    {
        boost::system::error_code ignored;
        port_->close(ignored);
    }
}

std::string serial_transport::describe() const
{
    std::string out = to_string(kind_);
    if (!target_.empty())
        out += " " + target_;
    out += " (" + to_label(settings_) + ")";
    return out;
}

std::string serial_transport::public_path() const
{
    return target_;
}

bool serial_transport::is_open() const
{
    return port_ && port_->is_open();
}

void serial_transport::async_read_some(boost::asio::mutable_buffer buffer,
                                       io_handler handler)
{
    VSS_PROFILE_ZONE("ports.serial.arm_read");
    if (!port_)
    {
        boost::asio::post(executor_, [handler] {
            handler(make_error_code(errc::not_open), 0);
        });
        return;
    }
    // Pass the handler through untouched: the serial library completes it on
    // the handler's associated executor, which is the endpoint's strand.
    port_->async_read_some(buffer, std::move(handler));
}

void serial_transport::async_write_some(boost::asio::const_buffer buffer,
                                        io_handler handler)
{
    VSS_PROFILE_ZONE("ports.serial.arm_write");
    if (!port_)
    {
        boost::asio::post(executor_, [handler] {
            handler(make_error_code(errc::not_open), 0);
        });
        return;
    }
    port_->async_write_some(buffer, std::move(handler));
}

void serial_transport::async_write_break(const line_break&, io_handler handler)
{
    boost::asio::post(executor_, [handler = std::move(handler)]
                      { handler(boost::system::error_code{}, 0); });
}

void serial_transport::close()
{
    if (!port_)
        return;
    VSS_PROFILE_ZONE("ports.serial.close");
    boost::system::error_code closed;
    trace::call_scope scope("serial.close", 0, target_);
    port_->close(closed);
    scope.finish(closed);
}

bool serial_transport::is_line_error(boost::system::error_code ec) const noexcept
{
    // A fixture stops on a fault: that is what it is for.
    if (kind_ == endpoint_kind::mock || ec.category() != vs::error_category())
        return false;
    switch (static_cast<vs::error>(ec.value()))
    {
    case vs::error::framing_error:
    case vs::error::parity_error:
    case vs::error::overrun:
    case vs::error::break_detected:
        return true;
    default:
        return false;
    }
}

std::optional<line_format> serial_transport::peer_format() const
{
    if (kind_ != endpoint_kind::rfc2217 || !port_)
        return std::nullopt;
    auto* backend = vs::backend_cast<vs::rfc2217_port>(*port_);
    if (backend == nullptr)
        return std::nullopt;
    const vs::serial_config options = backend->options();
    // Only what the server said: before its answers, or when Studio set the
    // settings itself, the transport does not know what the device has.
    if (!options.device_settings || !backend->settings_known())
        return std::nullopt;
    line_format format;
    format.baud_rate = options.baud_rate;
    format.data_bits = options.data_bits;
    format.parity = from_vs(options.parity);
    format.stop_bits = from_vs(options.stop_bits);
    return format;
}

void serial_transport::cancel()
{
    if (!port_)
        return;
    boost::system::error_code ignored;
    port_->cancel(ignored);
}

bool serial_transport::supports_control_lines() const noexcept
{
    return kind_ == endpoint_kind::physical && port_
        && port_->native_handle() != 0 && native::supported();
}

void serial_transport::set_control_line(control_line line, bool state)
{
    if (kind_ != endpoint_kind::physical || !port_)
        return;
    VSS_PROFILE_ZONE("ports.serial.set_line");
    if (const auto written = native::write_line(port_->native_handle(), line, state);
        !written)
        return;

    lines_.set(line, state);
}

control_lines serial_transport::lines() const
{
    return lines_;
}

void serial_transport::refresh_lines()
{
    if (kind_ != endpoint_kind::physical || !port_)
        return;
    VSS_PROFILE_ZONE("ports.serial.refresh_lines");

    const auto status = native::read_modem_status(port_->native_handle());
    if (!status)
        return;

    lines_.set(control_line::cts, status->cts);
    lines_.set(control_line::dsr, status->dsr);
    lines_.set(control_line::dcd, status->dcd);
    lines_.set(control_line::ri, status->ri);
}

} // namespace vss
