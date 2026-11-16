//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The channel is a socket and the output is a pipe.  The socket is
// written through Asio's socket, which never raises SIGPIPE when the
// plugin has gone.
//

#include "platform/stream_platform.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/host/process.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/write.hpp>

#include <unistd.h>

#include <memory>
#include <utility>

namespace vss::plugin::host::platform
{

namespace
{

class socket_stream final : public byte_stream
{
public:
    explicit socket_stream(const boost::asio::any_io_executor& executor)
        : socket_(executor)
    {
    }

    error_code adopt(native_handle handle)
    {
        error_code failed;
        socket_.assign(boost::asio::local::stream_protocol(),
                       static_cast<int>(handle), failed);
        if (failed)
            ::close(static_cast<int>(handle));
        return failed;
    }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         handler done) override
    {
        socket_.async_read_some(buffer, std::move(done));
    }

    void async_write(boost::asio::const_buffer buffer, handler done) override
    {
        boost::asio::async_write(socket_, buffer, std::move(done));
    }

    void close() noexcept override
    {
        error_code ignored;
        socket_.close(ignored);
    }

private:
    boost::asio::local::stream_protocol::socket socket_;
};

class pipe_stream final : public byte_stream
{
public:
    explicit pipe_stream(const boost::asio::any_io_executor& executor)
        : descriptor_(executor)
    {
    }

    error_code adopt(native_handle handle)
    {
        error_code failed;
        descriptor_.assign(static_cast<int>(handle), failed);
        if (failed)
            ::close(static_cast<int>(handle));
        return failed;
    }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         handler done) override
    {
        descriptor_.async_read_some(buffer, std::move(done));
    }

    void async_write(boost::asio::const_buffer buffer, handler done) override
    {
        boost::asio::async_write(descriptor_, buffer, std::move(done));
    }

    void close() noexcept override
    {
        error_code ignored;
        descriptor_.close(ignored);
    }

private:
    boost::asio::posix::stream_descriptor descriptor_;
};

} // namespace

result<std::unique_ptr<byte_stream>>
adopt_channel(const boost::asio::any_io_executor& executor,
              native_handle handle)
{
    auto stream = std::make_unique<socket_stream>(executor);
    if (const error_code failed = stream->adopt(handle))
        return failed;
    return std::unique_ptr<byte_stream>(std::move(stream));
}

result<std::unique_ptr<byte_stream>>
adopt_output(const boost::asio::any_io_executor& executor, native_handle handle)
{
    auto stream = std::make_unique<pipe_stream>(executor);
    if (const error_code failed = stream->adopt(handle))
        return failed;
    return std::unique_ptr<byte_stream>(std::move(stream));
}

} // namespace vss::plugin::host::platform
