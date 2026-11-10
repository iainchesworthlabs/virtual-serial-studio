//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/agent/proto/error.hpp>

#include <string>
#include <utility>

namespace vss::agent
{
namespace
{

class protocol_category_impl final : public boost::system::error_category
{
public:
    const char* name() const noexcept override { return "vss.agent"; }

    std::string message(int value) const override
    {
        switch (static_cast<protocol_errc>(value))
        {
        case protocol_errc::success:
            return "success";
        case protocol_errc::malformed_frame:
            return "malformed frame";
        case protocol_errc::oversized_frame:
            return "frame is too large";
        case protocol_errc::malformed_message:
            return "malformed message";
        case protocol_errc::unknown_operation:
            return "unknown operation";
        case protocol_errc::handshake_required:
            return "the first request must be hello";
        case protocol_errc::version_mismatch:
            return "the agent speaks another protocol version; restart it";
        case protocol_errc::busy:
            return "the agent has too many clients";
        case protocol_errc::connection_closed:
            return "the connection to the agent closed";
        case protocol_errc::not_same_user:
            return "the other end runs as another user";
        case protocol_errc::already_running:
            return "an agent is already running";
        case protocol_errc::not_running:
            return "no agent is running";
        }
        return "unknown agent protocol error";
    }
};

constexpr int last_vss_value = std::to_underlying(errc::internal);
constexpr int last_protocol_value =
    std::to_underlying(protocol_errc::not_running);

} // namespace

const boost::system::error_category& protocol_category() noexcept
{
    static const protocol_category_impl instance;
    return instance;
}

boost::system::error_code make_error_code(protocol_errc value) noexcept
{
    return {std::to_underlying(value), protocol_category()};
}

wire_error to_wire(const error_code& error)
{
    wire_error out;
    out.category = error.category().name();
    out.value = error.value();
    out.message = error.message();
    return out;
}

error_code from_wire(const wire_error& error)
{
    if (error.category == vss::error_category().name())
    {
        if (error.value < 0 || error.value > last_vss_value)
            return vss::make_error_code(errc::internal);
        return {error.value, vss::error_category()};
    }
    if (error.category == protocol_category().name())
    {
        if (error.value < 0 || error.value > last_protocol_value)
            return vss::make_error_code(errc::internal);
        return {error.value, protocol_category()};
    }
    if (error.category == boost::system::system_category().name())
        return {error.value, boost::system::system_category()};
    if (error.category == boost::system::generic_category().name())
        return {error.value, boost::system::generic_category()};
    return vss::make_error_code(errc::io_error);
}

} // namespace vss::agent
