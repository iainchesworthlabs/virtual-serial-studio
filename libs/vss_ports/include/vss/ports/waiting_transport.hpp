//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/transport.hpp>

#include <atomic>
#include <string>

namespace vss
{

/// A transport that is not connected to anything yet.  It stands in for a
/// network client whose remote end refused the connection when the port was
/// made: every read and write fails with `not_connected` at once, so the
/// endpoint stops as a lost connection does and the studio's reconnector
/// opens the real address in its place when it can.  It counts as open until
/// it is closed, so an endpoint can be started on it.
class waiting_transport final : public transport
{
public:
    waiting_transport(const executor_type& executor, std::string uri);

    std::string describe() const override;
    bool is_open() const override { return open_.load(); }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override;
    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override;

    void close() override { open_.store(false); }

private:
    void fail(io_handler handler);

    executor_type executor_;
    std::string uri_;
    std::atomic<bool> open_{true};
};

} // namespace vss
