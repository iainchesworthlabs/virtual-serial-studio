//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/transport.hpp>
#include <vss/ports/loopback_transport.hpp>

#include <boost/asio/post.hpp>

#include <memory>
#include <string>
#include <utility>

namespace vss_test_support {

/// One end of an in-memory null-modem cable: what is written to this end
/// becomes readable on the other end, and nothing is echoed back.  Two
/// endpoints on the two ends are like a port in Studio and the program that
/// has opened it.
// NOLINTBEGIN(misc-multiple-inheritance): enable_shared_from_this is a mixin, not a second
// interface.
class cable_end final : public vss::transport,
                        public std::enable_shared_from_this<cable_end>
{
// NOLINTEND(misc-multiple-inheritance)
public:
    static std::pair<std::shared_ptr<cable_end>, std::shared_ptr<cable_end>>
    make_pair(const executor_type& executor, const std::string& name)
    {
        auto first = std::shared_ptr<cable_end>(new cable_end(executor, name + "-a"));
        auto second = std::shared_ptr<cable_end>(new cable_end(executor, name + "-b"));
        first->peer_ = second;
        second->peer_ = first;
        return {first, second};
    }

    std::string describe() const override { return "cable " + name_; }
    bool is_open() const override { return inbox_->is_open(); }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override
    {
        inbox_->async_read_some(buffer, std::move(handler));
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        const std::size_t size = buffer.size();
        if (auto other = peer_.lock())
        {
            other->inbox_->inject(
                vss::bytes_view(static_cast<const vss::byte*>(buffer.data()), size));
        }
        boost::asio::post(executor_, [handler = std::move(handler), size] {
            handler({}, size);
        });
    }

    /// A break written to one end is a break received by the other, in
    /// order with the bytes written before it.
    bool can_deliver_break() const noexcept override { return true; }

    void async_write_break(const vss::line_break& item,
                           io_handler handler) override
    {
        if (auto other = peer_.lock())
        {
            // The same order as bytes: the other end's reader sees the bytes
            // already injected, then the break.
            other->inbox_->inject_break(item);
        }
        boost::asio::post(executor_, [handler = std::move(handler)]
                          { handler({}, 0); });
    }

    void set_break_sink(break_sink sink) override
    {
        inbox_->set_break_sink(std::move(sink));
    }

    void close() override { inbox_->close(); }
    void cancel() override { inbox_->cancel(); }

private:
    cable_end(const executor_type& executor, std::string name)
        : executor_(executor)
        , name_(std::move(name))
        , inbox_(vss::loopback_transport::create(executor, name_))
    {
    }

    executor_type executor_;
    std::string name_;
    std::shared_ptr<vss::loopback_transport> inbox_;
    std::weak_ptr<cable_end> peer_;
};

} // namespace vss_test_support

