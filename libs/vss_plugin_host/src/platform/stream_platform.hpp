//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A handle of a plugin's process as a stream that is read and written
// without a thread of its own.  One file for each system implements it.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/host/process.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>

#include <cstddef>
#include <functional>
#include <memory>

namespace vss::plugin::host
{

class byte_stream
{
public:
    using handler = std::function<void(error_code, std::size_t)>;

    byte_stream() = default;
    virtual ~byte_stream() = default;
    byte_stream(const byte_stream&) = delete;
    byte_stream& operator=(const byte_stream&) = delete;

    /// Reads what is there, at least one byte.  The end of the stream is
    /// an error.
    virtual void async_read_some(boost::asio::mutable_buffer buffer,
                                 handler done) = 0;
    /// Writes all of the buffer.
    virtual void async_write(boost::asio::const_buffer buffer,
                             handler done) = 0;
    /// Closes the handle.  Waiting reads and writes end with an error.
    virtual void close() noexcept = 0;
};

namespace platform
{

/// Takes over the host's end of a plugin's channel, or of its output
/// pipe.  The stream closes the handle.  When this fails the handle has
/// been closed.
result<std::unique_ptr<byte_stream>>
adopt_channel(const boost::asio::any_io_executor& executor,
              native_handle handle);
result<std::unique_ptr<byte_stream>>
adopt_output(const boost::asio::any_io_executor& executor,
             native_handle handle);

} // namespace platform

} // namespace vss::plugin::host
