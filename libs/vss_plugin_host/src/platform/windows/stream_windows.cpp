//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Both handles of a plugin are pipes opened for overlapped I/O.
//

#include "platform/stream_platform.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/host/process.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/windows/stream_handle.hpp>
#include <boost/asio/write.hpp>

#include <windows.h>

#include <memory>
#include <utility>

namespace vss::plugin::host::platform
{

namespace
{

HANDLE as_handle(native_handle handle) noexcept
{
    // The number is a HANDLE the operating system gave out.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<HANDLE>(handle);
}

class pipe_stream final : public byte_stream
{
public:
    explicit pipe_stream(const boost::asio::any_io_executor& executor)
        : handle_(executor)
    {
    }

    error_code adopt(native_handle handle)
    {
        error_code failed;
        handle_.assign(as_handle(handle), failed);
        if (failed)
            ::CloseHandle(as_handle(handle));
        return failed;
    }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         handler done) override
    {
        handle_.async_read_some(buffer, std::move(done));
    }

    void async_write(boost::asio::const_buffer buffer, handler done) override
    {
        boost::asio::async_write(handle_, buffer, std::move(done));
    }

    void close() noexcept override
    {
        error_code ignored;
        handle_.close(ignored);
    }

private:
    boost::asio::windows::stream_handle handle_;
};

result<std::unique_ptr<byte_stream>>
adopt(const boost::asio::any_io_executor& executor, native_handle handle)
{
    auto stream = std::make_unique<pipe_stream>(executor);
    if (const error_code failed = stream->adopt(handle))
        return failed;
    return std::unique_ptr<byte_stream>(std::move(stream));
}

} // namespace

result<std::unique_ptr<byte_stream>>
adopt_channel(const boost::asio::any_io_executor& executor,
              native_handle handle)
{
    return adopt(executor, handle);
}

result<std::unique_ptr<byte_stream>>
adopt_output(const boost::asio::any_io_executor& executor, native_handle handle)
{
    return adopt(executor, handle);
}

} // namespace vss::plugin::host::platform
