//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/transport.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vss_test_support
{

/// A transport whose operations a test completes by hand, so the order in
/// which things happen is the test's and not the scheduler's.
///
/// A read or a write handed to it stays pending until the test completes it
/// (#complete_read, #complete_write), except that #cancel completes the pending
/// reads with `operation_aborted`, as a real transport does, and that writes
/// complete on their own while #auto_complete_writes is on.  The handlers are
/// posted to the executor the transport was made with.  Every member is safe
/// from any thread.
class scripted_transport final : public vss::transport
{
public:
    explicit scripted_transport(const executor_type& executor)
        : executor_(executor)
    {
    }

    std::string describe() const override { return "scripted transport"; }
    bool is_open() const override { return open_.load(); }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override
    {
        std::lock_guard lock(mutex_);
        ++reads_armed_;
        pending_reads_.push_back({buffer, std::move(handler)});
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        const std::size_t size = buffer.size();
        {
            std::lock_guard lock(mutex_);
            const auto* first = static_cast<const char*>(buffer.data());
            written_.emplace_back(first, size);
            if (!auto_complete_writes_)
            {
                pending_writes_.push_back({size, std::move(handler)});
                return;
            }
        }
        boost::asio::post(executor_, [handler = std::move(handler), size]
                          { handler({}, size); });
    }

    void close() override { open_ = false; }

    void cancel() override
    {
        std::deque<pending_read> aborted;
        {
            std::lock_guard lock(mutex_);
            aborted.swap(pending_reads_);
        }
        for (auto& read : aborted)
        {
            boost::asio::post(
                executor_, [handler = std::move(read.handler)]
                { handler(boost::asio::error::operation_aborted, 0); });
        }
    }

    std::optional<vss::line_format> peer_format() const override
    {
        peer_format_calls_.fetch_add(1);
        return std::nullopt;
    }

    std::optional<std::size_t> client_count() const noexcept override
    {
        return std::size_t{1};
    }

    /// Completes the oldest pending read with `data`.  False when none is
    /// pending.
    bool complete_read(const std::string& data)
    {
        pending_read read;
        {
            std::lock_guard lock(mutex_);
            if (pending_reads_.empty())
                return false;
            read = std::move(pending_reads_.front());
            pending_reads_.pop_front();
        }
        const std::size_t size = std::min(data.size(), read.buffer.size());
        std::memcpy(read.buffer.data(), data.data(), size);
        boost::asio::post(executor_, [handler = std::move(read.handler), size]
                          { handler({}, size); });
        return true;
    }

    /// Completes the oldest pending read with the error `ec` and no bytes.
    /// False when none is pending.
    bool fail_read(boost::system::error_code ec)
    {
        pending_read read;
        {
            std::lock_guard lock(mutex_);
            if (pending_reads_.empty())
                return false;
            read = std::move(pending_reads_.front());
            pending_reads_.pop_front();
        }
        boost::asio::post(executor_, [handler = std::move(read.handler), ec]
                          { handler(ec, 0); });
        return true;
    }

    /// Completes the oldest pending write: with `ec`, and with all its bytes
    /// when `ec` is clear.  False when none is pending.
    bool complete_write(boost::system::error_code ec = {})
    {
        pending_write write;
        {
            std::lock_guard lock(mutex_);
            if (pending_writes_.empty())
                return false;
            write = std::move(pending_writes_.front());
            pending_writes_.pop_front();
        }
        const std::size_t size = ec ? std::size_t{0} : write.size;
        boost::asio::post(executor_, [handler = std::move(write.handler), ec,
                                      size] { handler(ec, size); });
        return true;
    }

    void auto_complete_writes(bool on)
    {
        std::lock_guard lock(mutex_);
        auto_complete_writes_ = on;
    }

    /// Reads handed to the transport since it was made.
    std::size_t reads_armed() const
    {
        std::lock_guard lock(mutex_);
        return reads_armed_;
    }

    /// Reads handed to the transport and not yet completed.
    std::size_t reads_pending() const
    {
        std::lock_guard lock(mutex_);
        return pending_reads_.size();
    }

    /// Writes handed to the transport and not yet completed.
    std::size_t writes_pending() const
    {
        std::lock_guard lock(mutex_);
        return pending_writes_.size();
    }

    /// The bytes of every write handed to the transport, one string each, in
    /// the order they came.
    std::vector<std::string> written() const
    {
        std::lock_guard lock(mutex_);
        return written_;
    }

    std::size_t peer_format_calls() const { return peer_format_calls_.load(); }

private:
    struct pending_read
    {
        boost::asio::mutable_buffer buffer;
        io_handler handler;
    };

    struct pending_write
    {
        std::size_t size = 0;
        io_handler handler;
    };

    executor_type executor_;
    std::atomic<bool> open_{true};
    mutable std::atomic<std::size_t> peer_format_calls_{0};
    mutable std::mutex mutex_;
    std::deque<pending_read> pending_reads_;
    std::deque<pending_write> pending_writes_;
    std::vector<std::string> written_;
    std::size_t reads_armed_ = 0;
    bool auto_complete_writes_ = false;
};

} // namespace vss_test_support
