//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_PORTS_LOOPBACK_TRANSPORT_HPP
#define VSS_PORTS_LOOPBACK_TRANSPORT_HPP

#include <vss/core/bytes.hpp>
#include <vss/core/transport.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace vss {

/// An in-memory duplex byte pipe.
///
/// Data written by the hub becomes readable on the same transport, and the
/// control lines are fully modelled.  It is the "no hardware at all" device
/// used by the demo scenarios, by the GUI before a real port is selected and
/// by the unit tests, and it behaves like a null-modem cable plugged into
/// itself.
// NOLINTNEXTLINE(misc-multiple-inheritance): shared_from_this is a mixin.
class loopback_transport final : public transport,
                                 public std::enable_shared_from_this<loopback_transport>
{
public:
    explicit loopback_transport(const executor_type& executor,
                                std::string name = "loopback");

    static std::shared_ptr<loopback_transport> create(
        const executor_type& executor, std::string name = "loopback");

    std::string describe() const override;
    std::string public_path() const override { return "loopback:" + name_; }
    bool is_open() const override { return open_.load(); }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override;
    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override;

    void close() override;
    void cancel() override;

    bool supports_control_lines() const noexcept override { return true; }
    /// The lines of a loopback port are set through its endpoint, so the hub
    /// knows of a change before any byte that follows it.
    bool reports_peer_rts() const noexcept override { return true; }
    void set_control_line(control_line line, bool state) override;
    control_lines lines() const override;

    /// Injects bytes as if the far end had sent them.
    void inject(bytes_view data);
    /// Number of bytes currently waiting for a reader.
    std::size_t available() const;

private:
    void finish_pending_read();
    /// Removes and returns the parked read. The caller holds `mutex_`.
    io_handler take_pending_read();

    executor_type executor_;
    std::string name_;
    mutable std::mutex mutex_;
    std::deque<byte> buffer_;
    io_handler pending_read_;
    boost::asio::mutable_buffer read_buffer_{};
    control_lines lines_;
    std::atomic<bool> open_{true};
};

} // namespace vss

#endif // VSS_PORTS_LOOPBACK_TRANSPORT_HPP
