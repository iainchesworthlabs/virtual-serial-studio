//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/waiting_transport.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>

#include <utility>

namespace vss
{

waiting_transport::waiting_transport(const executor_type& executor,
                                     std::string uri)
    : executor_(executor), uri_(std::move(uri))
{
}

std::string waiting_transport::describe() const
{
    return "waiting for " + uri_;
}

void waiting_transport::async_read_some(boost::asio::mutable_buffer,
                                        io_handler handler)
{
    fail(std::move(handler));
}

void waiting_transport::async_write_some(boost::asio::const_buffer,
                                         io_handler handler)
{
    fail(std::move(handler));
}

void waiting_transport::fail(io_handler handler)
{
    boost::asio::post(executor_, [handler = std::move(handler)]() mutable
                      { handler(boost::asio::error::not_connected, 0); });
}

} // namespace vss
