//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/rfc2217_client.hpp>

#include <vss/core/profile.hpp>
#include <vss/ports/rfc2217.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/virtual_serial.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace vss {
namespace asio = boost::asio;
namespace vs = boost::virtual_serial;

namespace {

constexpr std::string_view scheme = "rfc2217://";

/// The bytes held for the endpoint to read. A server that is read faster than
/// the hub takes the bytes waits in the socket and so in its own buffer.
constexpr std::size_t receive_limit = std::size_t{1} << 20;

/// The line-state bits of RFC 2217 that report a fault of the line.
constexpr unsigned char line_state_overrun = 0x02;
constexpr unsigned char line_state_parity = 0x04;
constexpr unsigned char line_state_framing = 0x08;
constexpr unsigned char line_state_break = 0x10;
constexpr unsigned char line_state_mask =
    line_state_overrun | line_state_parity | line_state_framing |
    line_state_break;
/// Every modem-state bit and delta the server may notify.
constexpr unsigned char modem_mask_all = 0xFF;

parity_mode from_vs(vs::parity_mode value) noexcept
{
    switch (value)
    {
    case vs::parity_mode::none: return parity_mode::none;
    case vs::parity_mode::odd: return parity_mode::odd;
    case vs::parity_mode::even: return parity_mode::even;
    case vs::parity_mode::mark: return parity_mode::mark;
    case vs::parity_mode::space: return parity_mode::space;
    }
    return parity_mode::none;
}

stop_bits_mode from_vs(vs::stop_bits_mode value) noexcept
{
    switch (value)
    {
    case vs::stop_bits_mode::one: return stop_bits_mode::one;
    case vs::stop_bits_mode::one_point_five:
        return stop_bits_mode::one_point_five;
    case vs::stop_bits_mode::two: return stop_bits_mode::two;
    }
    return stop_bits_mode::one;
}

flow_control_mode from_vs(vs::flow_control_mode value) noexcept
{
    switch (value)
    {
    case vs::flow_control_mode::none: return flow_control_mode::none;
    case vs::flow_control_mode::software: return flow_control_mode::software;
    case vs::flow_control_mode::hardware: return flow_control_mode::hardware;
    }
    return flow_control_mode::none;
}

} // namespace

bool is_rfc2217_client_uri(std::string_view uri) noexcept
{
    return uri.starts_with(scheme);
}

struct rfc2217_client_transport::state
    : std::enable_shared_from_this<rfc2217_client_transport::state>
{
    /// A write or a command waiting to go to the socket.
    struct outgoing
    {
        std::string bytes;
        /// Called once the bytes are written; empty for a command of ours.
        io_handler handler;
        std::size_t count = 0;
    };

    /// A break or a fault the server reported, behind the payload bytes
    /// before it.
    struct mark
    {
        std::uint64_t at = 0;
        std::optional<line_break> item;
        error_code fault;
    };

    struct pending_read
    {
        asio::mutable_buffer buffer;
        io_handler handler;
    };

    explicit state(const executor_type& executor_in)
        : executor(executor_in)
        , strand(asio::make_strand(executor_in))
        , socket(executor_in)
        , break_timer(executor_in)
    {
    }

    // -- what the constructor sets ---------------------------------------

    executor_type executor;
    asio::strand<executor_type> strand;
    asio::ip::tcp::socket socket;
    asio::steady_timer break_timer;
    std::string uri;
    std::string target;

    // -- shared with other threads, under `mutex` ------------------------

    mutable std::mutex mutex;
    serial_settings settings;
    control_lines line_state;
    notice_sink notices;
    break_sink breaks;
    std::atomic<bool> open{false};

    // -- the strand's own ------------------------------------------------

    rfc2217::client_session session;
    std::array<char, 4096> buffer{};
    bool reading = false;
    bool configured = false;
    bool break_noticed = false;
    bool refusal_noticed = false;
    serial_settings sent;
    std::string received;
    std::deque<mark> marks;
    std::uint64_t arrived = 0;
    std::uint64_t consumed = 0;
    bool lines_dirty = false;
    std::optional<pending_read> reader;
    error_code failure;
    std::deque<outgoing> queue;
    bool writing = false;
    std::optional<io_handler> break_handler;

    // -- helpers -----------------------------------------------------------

    void say(std::string text)
    {
        notice_sink sink;
        {
            std::lock_guard lock(mutex);
            sink = notices;
        }
        if (sink)
            sink(std::move(text));
    }

    /// Calls `handler` on the transport's executor, so no completion runs
    /// inside the call that asked.
    void complete(io_handler handler, error_code ec, std::size_t count)
    {
        asio::post(executor,
                   [handler = std::move(handler), ec, count]() mutable
                   { handler(ec, count); });
    }

    void start()
    {
        open.store(true);
        asio::dispatch(strand,
                       [self = shared_from_this()]
                       {
                           self->send(self->session.greeting());
                           self->read_more();
                       });
    }

    // -- sending -----------------------------------------------------------

    void send(std::string bytes, io_handler handler = {}, std::size_t count = 0)
    {
        if (!open.load())
        {
            if (handler)
                complete(std::move(handler), asio::error::operation_aborted, 0);
            return;
        }
        queue.push_back({std::move(bytes), std::move(handler), count});
        write_next();
    }

    void write_next()
    {
        if (writing || queue.empty() || !open.load())
            return;
        writing = true;
        asio::async_write(
            socket, asio::buffer(queue.front().bytes),
            asio::bind_executor(
                strand,
                [self = shared_from_this()](error_code ec, std::size_t)
                {
                    self->writing = false;
                    if (ec)
                    {
                        self->fail(ec);
                        return;
                    }
                    auto done = std::move(self->queue.front());
                    self->queue.pop_front();
                    if (done.handler)
                        self->complete(std::move(done.handler), {}, done.count);
                    self->write_next();
                }));
    }

    /// The command bytes for what the server was last told and what it
    /// should be told now.
    void configure()
    {
        serial_settings wanted;
        control_lines lines;
        {
            std::lock_guard lock(mutex);
            wanted = settings;
            lines = line_state;
        }
        std::string out;
        if (!configured || wanted.baud_rate != sent.baud_rate)
            out += session.set_baud_rate(wanted.baud_rate);
        if (!configured || wanted.data_bits != sent.data_bits)
            out += session.set_data_size(wanted.data_bits);
        if (!configured || wanted.parity != sent.parity)
            out += session.set_parity(wanted.parity);
        if (!configured || wanted.stop_bits != sent.stop_bits)
            out += session.set_stop_size(wanted.stop_bits);
        if (!configured || wanted.flow_control != sent.flow_control)
            out += session.set_flow_control(wanted.flow_control);
        if (!configured)
        {
            out += session.set_dtr(lines.dtr());
            out += session.set_rts(lines.rts());
            out += session.set_line_state_mask(line_state_mask);
            out += session.set_modem_state_mask(modem_mask_all);
        }
        sent = wanted;
        configured = true;
        if (!out.empty())
            send(std::move(out));
    }

    void com_port_changed()
    {
        if (session.com_port_active())
        {
            configure();
            return;
        }
        if (session.com_port_refused() && !refusal_noticed)
        {
            refusal_noticed = true;
            say("The server does not offer RFC 2217, so the port is used as a "
                "plain byte stream. Speed, DTR, RTS and break are not sent.");
        }
    }

    // -- receiving ---------------------------------------------------------

    void read_more()
    {
        if (reading || !open.load() || received.size() >= receive_limit)
            return;
        reading = true;
        socket.async_read_some(
            asio::buffer(buffer),
            asio::bind_executor(
                strand,
                [self = shared_from_this()](error_code ec, std::size_t count)
                {
                    self->reading = false;
                    self->on_read(ec, count);
                }));
    }

    std::chrono::nanoseconds character_time()
    {
        std::lock_guard lock(mutex);
        return settings.character_duration();
    }

    void on_read(error_code ec, std::size_t count)
    {
        if (ec)
        {
            fail(ec);
            return;
        }
        auto fed = session.feed(std::string_view(buffer.data(), count));

        if (!fed.reply.empty())
            send(std::move(fed.reply));

        const std::uint64_t base = arrived;
        for (const auto& notice : fed.line_states)
        {
            // A fault of the line is a read that fails and carries on, as it
            // is for every serial port.
            const std::pair<unsigned char, vs::error> faults[] = {
                {line_state_framing, vs::error::framing_error},
                {line_state_parity, vs::error::parity_error},
                {line_state_overrun, vs::error::overrun}};
            for (const auto& [bit, fault] : faults)
            {
                if ((notice.state & bit) != 0)
                    marks.push_back(
                        {base + notice.at, std::nullopt, make_error_code(fault)});
            }
            if ((notice.state & line_state_break) == 0)
                continue;
            // The server says a break happened and not how long it was.
            line_break item;
            item.phase = break_phase::complete;
            item.start = clock_type::now();
            item.duration = character_time();
            item.measured = false;
            marks.push_back({base + notice.at, item, {}});
        }
        received += fed.data;
        arrived += fed.data.size();

        if (fed.modem_state)
        {
            const unsigned value = *fed.modem_state;
            std::lock_guard lock(mutex);
            const control_lines before = line_state;
            line_state.set(control_line::cts, (value & 0x10U) != 0);
            line_state.set(control_line::dsr, (value & 0x20U) != 0);
            line_state.set(control_line::ri, (value & 0x40U) != 0);
            line_state.set(control_line::dcd, (value & 0x80U) != 0);
            if (!(line_state == before))
                lines_dirty = true;
        }

        if (fed.com_port_changed)
            com_port_changed();

        deliver();
        read_more();
    }

    /// Gives the endpoint what it is waiting for, in order: the bytes, then
    /// the breaks that come after them. A read stops at a break, and the bytes
    /// behind it come with the next read once the break has been reported.
    void deliver()
    {
        VSS_PROFILE_ZONE("ports.rfc2217_client.deliver");
        if (!reader)
            return;

        std::size_t ahead = received.size();
        bool break_first = false;
        if (!marks.empty())
        {
            ahead = std::min<std::size_t>(
                ahead,
                static_cast<std::size_t>(marks.front().at - consumed));
            break_first = marks.front().at <= consumed;
        }

        if (ahead == 0 && !lines_dirty && !break_first)
        {
            if (failure && received.empty() && marks.empty())
            {
                auto taken = std::move(*reader);
                reader.reset();
                complete(std::move(taken.handler), failure, 0);
            }
            return;
        }

        if (break_first && marks.front().fault)
        {
            // A fault of the line is the next thing there is.
            auto taken = std::move(*reader);
            reader.reset();
            const error_code fault = marks.front().fault;
            marks.pop_front();
            complete(std::move(taken.handler), fault, 0);
            read_more();
            return;
        }

        auto taken = std::move(*reader);
        reader.reset();
        lines_dirty = false;

        const std::size_t count = std::min(taken.buffer.size(), ahead);
        std::copy_n(received.data(), count,
                    static_cast<char*>(taken.buffer.data()));
        received.erase(0, count);
        consumed += count;

        std::vector<line_break> due;
        while (!marks.empty() && marks.front().at <= consumed &&
               marks.front().item)
        {
            due.push_back(*marks.front().item);
            marks.pop_front();
        }

        break_sink sink;
        {
            std::lock_guard lock(mutex);
            sink = breaks;
        }
        // The breaks go in the same task as the completion, straight after
        // it, so they cannot overtake the bytes before them.
        asio::post(executor,
                   [handler = std::move(taken.handler), count,
                    due = std::move(due), sink = std::move(sink)]() mutable
                   {
                       handler({}, count);
                       if (sink)
                           for (const line_break& item : due)
                               sink(item);
                   });
        read_more();
    }

    // -- ending ------------------------------------------------------------

    /// The connection failed or ended: what is left to read is still read,
    /// then the read fails, and every write that has not finished does.
    void fail(error_code ec)
    {
        if (failure)
            return;
        failure = ec;
        open.store(false);
        error_code ignored;
        socket.close(ignored);
        break_timer.cancel();
        abort_writes(ec);
        deliver();
    }

    void abort_writes(error_code ec)
    {
        auto waiting = std::move(queue);
        queue.clear();
        for (auto& item : waiting)
            if (item.handler)
                complete(std::move(item.handler), ec, 0);
        if (break_handler)
        {
            complete(std::move(*break_handler), ec, 0);
            break_handler.reset();
        }
    }

    void shut()
    {
        if (!failure)
            failure = asio::error::operation_aborted;
        open.store(false);
        error_code ignored;
        socket.close(ignored);
        break_timer.cancel();
        received.clear();
        marks.clear();
        abort_writes(asio::error::operation_aborted);
        if (reader)
        {
            auto taken = std::move(*reader);
            reader.reset();
            complete(std::move(taken.handler), asio::error::operation_aborted,
                     0);
        }
    }

    // -- breaks --------------------------------------------------------------

    void send_break(const line_break& item, io_handler handler)
    {
        if (!open.load())
        {
            complete(std::move(handler), make_error_code(errc::not_open), 0);
            return;
        }
        if (!session.com_port_active())
        {
            if (!break_noticed)
            {
                break_noticed = true;
                say("A break was not sent: the server does not offer RFC "
                    "2217, so it cannot be told to send one.");
            }
            complete(std::move(handler), make_error_code(errc::unsupported), 0);
            return;
        }
        if (!break_noticed)
        {
            break_noticed = true;
            say("A break is sent to the server as break on, a wait and break "
                "off. The length on the wire is accurate to the network, not "
                "to the break.");
        }

        if (item.phase == break_phase::begin)
        {
            send(session.set_break(true), std::move(handler));
            return;
        }
        if (item.phase == break_phase::end)
        {
            send(session.set_break(false), std::move(handler));
            return;
        }

        // A whole break: on, a wait of its length, off. A length that was not
        // measured is one character time, which is also the shortest wait.
        auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(
            item.duration);
        wait = std::max(wait, character_time());
        send(session.set_break(true));
        break_handler = std::move(handler);
        break_timer.expires_after(wait);
        break_timer.async_wait(asio::bind_executor(
            strand,
            [self = shared_from_this()](error_code ec)
            {
                if (ec || !self->break_handler)
                    return;
                auto done = std::move(*self->break_handler);
                self->break_handler.reset();
                self->send(self->session.set_break(false), std::move(done));
            }));
    }

    // -- lines and format ----------------------------------------------------

    void push_line(control_line line, bool on)
    {
        if (!session.com_port_active() || !open.load())
            return;
        switch (line)
        {
        case control_line::dtr:
            send(session.set_dtr(on));
            break;
        case control_line::rts:
            send(session.set_rts(on));
            break;
        case control_line::break_signal:
            send(session.set_break(on));
            break;
        default:
            break;
        }
    }
};

namespace {

/// Resolves the host of `target` and connects to it.
error_code resolve_and_connect(asio::ip::tcp::socket& socket,
                               const transport::executor_type& executor,
                               const std::string& target)
{
    std::string host;
    std::string service;
    if (!vs::detail::split_host_service(target, host, service))
        return make_error_code(errc::invalid_uri);

    error_code ec;
    asio::ip::tcp::resolver resolver(executor);
    const auto results = resolver.resolve(host, service, ec);
    if (ec)
        return ec;
    asio::connect(socket, results, ec);
    if (ec)
    {
        error_code ignored;
        socket.close(ignored);
        return ec;
    }
    error_code ignored;
    socket.set_option(asio::ip::tcp::no_delay(true), ignored);
    socket.set_option(asio::socket_base::keep_alive(true), ignored);
    return {};
}

} // namespace

rfc2217_client_transport::rfc2217_client_transport(
    const executor_type& executor, std::string uri, error_code& ec)
    : rfc2217_client_transport(executor, std::move(uri), nullptr, ec)
{
}

rfc2217_client_transport::rfc2217_client_transport(
    const executor_type& executor, std::string uri,
    const serial_settings& settings, error_code& ec)
    : rfc2217_client_transport(executor, std::move(uri), &settings, ec)
{
}

rfc2217_client_transport::rfc2217_client_transport(
    const executor_type& executor, std::string uri,
    const serial_settings* settings, error_code& ec)
    : state_(std::make_shared<state>(executor))
{
    VSS_PROFILE_ZONE_NAMED(zone, "ports.rfc2217_client.open");
    if (zone.active())
        zone.text(uri);
    state_->uri = std::move(uri);

    if (!is_rfc2217_client_uri(state_->uri))
    {
        ec = make_error_code(errc::invalid_uri);
        return;
    }
    if (settings != nullptr)
    {
        if (const error_code invalid = validate(*settings))
        {
            ec = invalid;
            return;
        }
    }

    boost::system::error_code parse_ec;
    const auto configuration = vs::parse_uri(state_->uri, parse_ec);
    if (parse_ec)
    {
        ec = parse_ec;
        return;
    }

    serial_settings wanted;
    if (settings != nullptr)
    {
        wanted = *settings;
    }
    else
    {
        wanted.baud_rate = configuration.baud_rate;
        wanted.data_bits = configuration.data_bits;
        wanted.parity = from_vs(configuration.parity);
        wanted.stop_bits = from_vs(configuration.stop_bits);
        wanted.flow_control = from_vs(configuration.flow_control);
        wanted.read_timeout = configuration.read_timeout;
        wanted.write_timeout = configuration.write_timeout;
        wanted.exclusive = configuration.exclusive;
    }
    state_->target = configuration.target;
    state_->settings = wanted;
    state_->line_state.set(control_line::dtr, true);
    state_->line_state.set(control_line::rts, true);

    if (const error_code failed =
            resolve_and_connect(state_->socket, executor, state_->target))
    {
        ec = failed;
        return;
    }
    state_->start();
}

rfc2217_client_transport::~rfc2217_client_transport()
{
    close();
}

std::string rfc2217_client_transport::describe() const
{
    const serial_settings current = settings();
    return "rfc2217 " + impl().target + " (" + to_label(current) + ")";
}

std::string rfc2217_client_transport::public_path() const
{
    return impl().target;
}

bool rfc2217_client_transport::is_open() const
{
    return impl().open.load();
}

void rfc2217_client_transport::async_read_some(
    boost::asio::mutable_buffer buffer, io_handler handler)
{
    VSS_PROFILE_ZONE("ports.rfc2217_client.arm_read");
    auto self = state_;
    asio::dispatch(self->strand,
                   [self, buffer, handler = std::move(handler)]() mutable
                   {
                       if (self->reader)
                       {
                           self->complete(std::move(handler),
                                          make_error_code(errc::device_busy), 0);
                           return;
                       }
                       self->reader = state::pending_read{buffer,
                                                          std::move(handler)};
                       self->deliver();
                       if (self->reader && self->failure && !self->open.load() &&
                           self->received.empty() && self->marks.empty())
                       {
                           auto taken = std::move(*self->reader);
                           self->reader.reset();
                           self->complete(std::move(taken.handler),
                                          self->failure, 0);
                       }
                   });
}

void rfc2217_client_transport::async_write_some(
    boost::asio::const_buffer buffer, io_handler handler)
{
    VSS_PROFILE_ZONE("ports.rfc2217_client.arm_write");
    const std::size_t count = buffer.size();
    std::string escaped = rfc2217::client_session::encode(std::string_view(
        static_cast<const char*>(buffer.data()), count));
    auto self = state_;
    asio::dispatch(self->strand,
                   [self, escaped = std::move(escaped), count,
                    handler = std::move(handler)]() mutable
                   {
                       self->send(std::move(escaped), std::move(handler),
                                  count);
                   });
}

void rfc2217_client_transport::async_write_break(const line_break& item,
                                                 io_handler handler)
{
    auto self = state_;
    asio::dispatch(self->strand,
                   [self, item, handler = std::move(handler)]() mutable
                   { self->send_break(item, std::move(handler)); });
}

void rfc2217_client_transport::close()
{
    if (!state_)
        return;
    VSS_PROFILE_ZONE("ports.rfc2217_client.close");
    state_->open.store(false);
    auto self = state_;
    asio::dispatch(self->strand, [self] { self->shut(); });
}

void rfc2217_client_transport::cancel()
{
    auto self = state_;
    asio::dispatch(self->strand,
                   [self]
                   {
                       if (!self->reader)
                           return;
                       auto taken = std::move(*self->reader);
                       self->reader.reset();
                       self->complete(std::move(taken.handler),
                                      asio::error::operation_aborted, 0);
                   });
}

void rfc2217_client_transport::set_control_line(control_line line, bool on)
{
    if (line != control_line::dtr && line != control_line::rts &&
        line != control_line::break_signal)
        return; // CTS, DSR, DCD and RI are the server's to say
    {
        std::lock_guard lock(state_->mutex);
        state_->line_state.set(line, on);
    }
    auto self = state_;
    asio::dispatch(self->strand, [self, line, on] { self->push_line(line, on); });
}

control_lines rfc2217_client_transport::lines() const
{
    std::lock_guard lock(state_->mutex);
    return state_->line_state;
}

bool rfc2217_client_transport::is_line_error(
    boost::system::error_code ec) const noexcept
{
    if (ec.category() != vs::error_category())
        return false;
    switch (static_cast<vs::error>(ec.value()))
    {
    case vs::error::framing_error:
    case vs::error::parity_error:
    case vs::error::overrun:
        return true;
    default:
        return false;
    }
}

void rfc2217_client_transport::set_notice_sink(notice_sink sink)
{
    std::lock_guard lock(state_->mutex);
    state_->notices = std::move(sink);
}

void rfc2217_client_transport::set_break_sink(break_sink sink)
{
    std::lock_guard lock(state_->mutex);
    state_->breaks = std::move(sink);
}

void rfc2217_client_transport::apply_peer_format(const line_format& format)
{
    {
        std::lock_guard lock(state_->mutex);
        if (format.baud_rate != 0)
            state_->settings.baud_rate = format.baud_rate;
        if (format.framing_known)
        {
            state_->settings.data_bits = format.data_bits;
            state_->settings.parity = format.parity;
            state_->settings.stop_bits = format.stop_bits;
        }
    }
    auto self = state_;
    asio::dispatch(self->strand,
                   [self]
                   {
                       if (self->session.com_port_active() && self->configured)
                           self->configure();
                   });
}

serial_settings rfc2217_client_transport::settings() const
{
    std::lock_guard lock(state_->mutex);
    return state_->settings;
}

} // namespace vss
