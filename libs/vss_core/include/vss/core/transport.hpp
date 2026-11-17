//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_CORE_TRANSPORT_HPP
#define VSS_CORE_TRANSPORT_HPP

#include <vss/core/control_lines.hpp>
#include <vss/core/error.hpp>
#include <vss/core/line_break.hpp>
#include <vss/core/serial_settings.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace vss {

/// The byte channel below an endpoint.
///
/// A transport is deliberately *not* a serial port abstraction: it is the
/// smallest thing the router needs, so a pseudo terminal, a named pipe, an
/// in-memory loopback or `boost::virtual_serial::any_serial_port` all fit
/// behind it.
///
/// Threading: every completion handler runs on the executor the transport was
/// constructed with.  Implementations must tolerate `close()` from another
/// thread while an operation is outstanding, and must complete pending
/// handlers exactly once with `boost::asio::error::operation_aborted`.
class transport
{
public:
    using executor_type = boost::asio::any_io_executor;
    using io_handler =
        std::function<void(boost::system::error_code, std::size_t)>;

    virtual ~transport() = default;

    /// Human readable transport description shown in the GUI.
    virtual std::string describe() const = 0;

    /// OS path other applications use, when there is one.
    virtual std::string public_path() const { return {}; }

    virtual bool is_open() const = 0;

    virtual void async_read_some(boost::asio::mutable_buffer buffer,
                                 io_handler handler) = 0;
    virtual void async_write_some(boost::asio::const_buffer buffer,
                                  io_handler handler) = 0;

    virtual void close() = 0;
    virtual void cancel() {}

    /// True when a read that completed with `ec` reports a fault of the line
    /// (framing, parity, overrun, break) and not the end of the channel.  The
    /// transport is still open and the next read carries on, so the endpoint
    /// logs and counts the fault and reads again.
    virtual bool is_line_error(boost::system::error_code /*ec*/) const noexcept
    {
        return false;
    }

    virtual bool supports_control_lines() const noexcept { return false; }
    virtual void set_control_line(control_line, bool) {}
    virtual control_lines lines() const { return {}; }

    /// True when the RTS in `lines()` is the transmitter switch of whatever
    /// is on the other side, and the endpoint learns of every change of it
    /// before it reads the bytes sent after the change.  Only then can a
    /// half-duplex bus gate the member by its RTS
    /// (`half_duplex_options::rts_gating`) without muting it by mistake.
    /// False for every transport that cannot promise both.
    virtual bool reports_peer_rts() const noexcept { return false; }

    /// Re-reads the lines from the device, for transports that poll.
    virtual void refresh_lines() {}

    /// The baud rate and framing the program on the other side of this
    /// transport has set, when the transport can tell: the driver reports what
    /// the program chose with `SetCommState`, a pseudo terminal what it set
    /// with `tcsetattr`.  Empty when it cannot, or when no program has chosen
    /// yet.  May be called from any thread.
    virtual std::optional<line_format> peer_format() const { return std::nullopt; }

    /// Tells the transport the baud rate and framing that the program on the
    /// far side of a link has set, for a transport that can pass them on: an
    /// RFC 2217 client sends them to its server, so the device there runs as
    /// the program expects.  Called when the source of a link reports a
    /// format (#peer_format) and when a link to it is made, from any thread.
    /// Most transports have no use for it and ignore it.
    virtual void apply_peer_format(const line_format& /*format*/) {}

    /// How many remote clients are connected, for a transport that waits for
    /// them (a listener).  Empty for every other kind, so "none connected" and
    /// "not a listener" stay different answers.  May be called from any thread.
    virtual std::optional<std::size_t> client_count() const noexcept
    {
        return std::nullopt;
    }

    /// Something a transport has to say about its device that belongs in the
    /// log, such as that a program opened or closed it.  The endpoint hands the
    /// transport a function that logs one line against it; the transport may
    /// call it from any thread, and must stop calling it when it closes.  Most
    /// transports have nothing to say and ignore it.
    using notice_sink = std::function<void(std::string)>;
    // The overrides take the sink by value and keep it, so the base does too.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    virtual void set_notice_sink(notice_sink) {}

    /// Where a transport reports a break it received from its device or its
    /// program, with the time the break began and how long it lasted.
    using break_sink = std::function<void(const line_break&)>;

    /// Gives the transport the function to report breaks to.  The sink only
    /// posts to the endpoint's strand, and the transport keeps the order of
    /// the stream with it: it calls the sink in the same task, and straight
    /// after, the read completion that carried the bytes before the break,
    /// and the break is never reported before that completion nor after the
    /// one that carries the bytes behind it.  With no bytes to ride, it
    /// completes the read it holds with zero bytes (the endpoint takes that
    /// for a change of lines and reads on) and calls the sink after it.  A
    /// transport that cannot see a break ignores the sink.
    // The overrides take the sink by value and keep it, so the base does too.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    virtual void set_break_sink(break_sink) {}

    /// The time, on the hub clock, at which the sender made the bytes of the
    /// read that just completed, when the transport knows it (the driver port
    /// stamps each WriteFile).  The endpoint asks once, in the task that
    /// carries the completion, and uses it as the chunk's time in place of the
    /// time of the read, never later than that.  Empty means the read time.
    virtual std::optional<time_point> read_stamp() const noexcept
    {
        return std::nullopt;
    }

    /// True when the transport can show a break to the program or device on
    /// its far side.  False means a break written to it is not delivered:
    /// the endpoint says so once in the log and counts it as dropped.  A
    /// transport that can raise a break overrides this and
    /// #async_write_break together.
    virtual bool can_deliver_break() const noexcept { return false; }

    /// Shows `item` to the far side, in order with the bytes written before
    /// and after it.  Called only when #can_deliver_break is true, one at a
    /// time and in step with #async_write_some.  `handler` is called once
    /// with the result and zero bytes.  A `begin` raises the break, an `end`
    /// ends it, and a `complete` is both, lasting `item.duration`.
    virtual void async_write_break(const line_break& /*item*/, io_handler handler)
    {
        handler(make_error_code(errc::unsupported), 0);
    }
};

} // namespace vss

#endif // VSS_CORE_TRANSPORT_HPP
