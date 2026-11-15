//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/listen_transport.hpp>

#include <vss/core/profile.hpp>
#include <vss/ports/rfc2217.hpp>

#include "platform/listen_socket_platform.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace vss {

namespace asio = boost::asio;

namespace {

constexpr std::string_view tcp_scheme = "tcp-listen://";
constexpr std::string_view udp_scheme = "udp-listen://";
constexpr std::string_view rfc2217_scheme = "rfc2217-listen://";

/// Bytes held for the hub before the clients are told to wait.
constexpr std::size_t receive_limit = 1U << 20;
constexpr std::size_t chunk_size = 4096;
constexpr std::size_t udp_peer_limit = 16;
constexpr std::size_t allow_limit = 64;
constexpr std::string_view allow_key = "allow=";

/// The most handed to a socket in one operation. The write deadline starts
/// again after each, and the bytes of one hub write are not held by one
/// operation however large they are.
constexpr std::size_t write_slice = 64U * 1024U;

/// The shortest time between two notices of the same kind that a listener
/// cannot take clients. A script opening connections as fast as it can, or an
/// accept that keeps failing, would otherwise fill the log; the next notice
/// says how many were left out.
constexpr std::chrono::seconds notice_interval{10};

/// Lets a notice through at most once in the notice interval, and counts the
/// ones it holds back.
struct notice_throttle
{
    bool ready()
    {
        const auto now = std::chrono::steady_clock::now();
        if (used_ && now - last_ < notice_interval)
        {
            ++held_back_;
            return false;
        }
        used_ = true;
        last_ = now;
        return true;
    }

    /// How many were held back since the last call.
    std::size_t take_held_back() { return std::exchange(held_back_, 0); }

private:
    bool used_ = false;
    std::chrono::steady_clock::time_point last_{};
    std::size_t held_back_ = 0;
};

std::string describe_duration(std::chrono::milliseconds time)
{
    if (time.count() >= 1000 && time.count() % 1000 == 0)
        return std::to_string(time.count() / 1000) + " s";
    return std::to_string(time.count()) + " ms";
}

/// One entry of an allow list: an address and how many leading bits of it a
/// client must share.
struct allow_rule
{
    asio::ip::address base;
    unsigned prefix = 0;
};

std::optional<allow_rule> parse_allow_rule(std::string_view text)
{
    std::string_view address = text;
    std::string_view length;
    const auto slash = text.find('/');
    if (slash != std::string_view::npos)
    {
        address = text.substr(0, slash);
        length = text.substr(slash + 1);
        if (length.empty())
            return std::nullopt;
    }

    boost::system::error_code ec;
    const auto base = asio::ip::make_address(std::string(address), ec);
    if (ec)
        return std::nullopt;

    const unsigned most = base.is_v4() ? 32U : 128U;
    unsigned prefix = most;
    if (!length.empty())
    {
        const auto parsed = std::from_chars(
            length.data(), length.data() + length.size(), prefix);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != length.data() + length.size() || prefix > most)
            return std::nullopt;
    }
    return allow_rule{base, prefix};
}

/// An IPv4 address that arrived as IPv6 (`::ffff:a.b.c.d`) is the IPv4 one.
asio::ip::address plain_address(const asio::ip::address& address)
{
    if (address.is_v6() && address.to_v6().is_v4_mapped())
        return asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
    return address;
}

/// `address:port`, with brackets round an IPv6 address. An IPv4 address that
/// arrived as IPv6 is written as IPv4, as the allow list reads it.
std::string describe_endpoint(const asio::ip::tcp::endpoint& endpoint)
{
    const auto address = plain_address(endpoint.address());
    const std::string port = std::to_string(endpoint.port());
    if (address.is_v6())
        return '[' + address.to_string() + "]:" + port;
    return address.to_string() + ':' + port;
}

template <class Bytes>
bool same_prefix(const Bytes& left, const Bytes& right, unsigned bits)
{
    for (std::size_t index = 0; bits > 0; ++index)
    {
        const unsigned take = std::min(8U, bits);
        const unsigned mask = (0xFFU << (8U - take)) & 0xFFU;
        if (((left[index] ^ right[index]) & mask) != 0)
            return false;
        bits -= take;
    }
    return true;
}

bool rule_admits(const allow_rule& rule, const asio::ip::address& peer)
{
    const auto address = plain_address(peer);
    if (rule.base.is_v4() != address.is_v4())
        return false;
    if (address.is_v4())
        return same_prefix(rule.base.to_v4().to_bytes(),
                           address.to_v4().to_bytes(), rule.prefix);
    return same_prefix(rule.base.to_v6().to_bytes(), address.to_v6().to_bytes(),
                       rule.prefix);
}

/// The rules of a list, or nothing if an entry is not an address or a range.
std::optional<std::vector<allow_rule>>
parse_allow_list(const std::vector<std::string>& list)
{
    if (list.size() > allow_limit)
        return std::nullopt;
    std::vector<allow_rule> rules;
    for (const std::string& entry : list)
    {
        const auto rule = parse_allow_rule(entry);
        if (!rule)
            return std::nullopt;
        rules.push_back(*rule);
    }
    return rules;
}

bool rules_admit(const std::vector<allow_rule>& rules,
                 const asio::ip::address& peer)
{
    if (rules.empty())
        return true;
    for (const allow_rule& rule : rules)
    {
        if (rule_admits(rule, peer))
            return true;
    }
    return false;
}

} // namespace

std::chrono::milliseconds accept_retry_delay(std::size_t failures,
                                             const listen_limits& limits)
{
    // A first wait of nothing could never grow.
    auto delay =
        std::max(limits.accept_retry_first, std::chrono::milliseconds{1});
    for (std::size_t step = 0;
         step < failures && delay < limits.accept_retry_last; ++step)
        delay *= 2;
    return std::min(delay, limits.accept_retry_last);
}

bool allow_list_admits(const std::vector<std::string>& list,
                       std::string_view address)
{
    VSS_PROFILE_ZONE("ports.listen.allow_check");
    if (list.empty())
        return true;
    boost::system::error_code ec;
    const auto peer = asio::ip::make_address(std::string(address), ec);
    const auto rules = parse_allow_list(list);
    return !ec && rules && rules_admit(*rules, peer);
}

bool listens_on_loopback(const listen_address& address)
{
    if (address.host.empty())
        return false;
    boost::system::error_code ec;
    const auto ip = asio::ip::make_address(address.host, ec);
    return !ec && plain_address(ip).is_loopback();
}

bool is_listen_uri(std::string_view uri) noexcept
{
    return uri.starts_with(tcp_scheme) || uri.starts_with(udp_scheme) ||
           uri.starts_with(rfc2217_scheme);
}

result<listen_address> parse_listen_uri(std::string_view uri)
{
    VSS_PROFILE_ZONE("ports.listen.parse_uri");
    listen_address address;
    if (uri.starts_with(tcp_scheme))
    {
        address.protocol = listen_protocol::tcp;
        uri.remove_prefix(tcp_scheme.size());
    }
    else if (uri.starts_with(udp_scheme))
    {
        address.protocol = listen_protocol::udp;
        uri.remove_prefix(udp_scheme.size());
    }
    else if (uri.starts_with(rfc2217_scheme))
    {
        address.protocol = listen_protocol::rfc2217;
        uri.remove_prefix(rfc2217_scheme.size());
    }
    else
    {
        return make_error_code(errc::invalid_uri);
    }

    // After the port the URI may carry `?allow=list` and nothing else.
    std::vector<std::string> allow;
    if (const auto mark = uri.find('?'); mark != std::string_view::npos)
    {
        std::string_view query = uri.substr(mark + 1);
        uri = uri.substr(0, mark);
        if (!query.starts_with(allow_key))
            return make_error_code(errc::invalid_uri);
        query.remove_prefix(allow_key.size());
        while (true)
        {
            const auto comma = query.find(',');
            const auto entry = query.substr(0, comma);
            if (!parse_allow_rule(entry))
                return make_error_code(errc::invalid_uri);
            allow.emplace_back(entry);
            if (comma == std::string_view::npos)
                break;
            query.remove_prefix(comma + 1);
        }
        if (allow.size() > allow_limit)
            return make_error_code(errc::invalid_uri);
    }

    std::string_view host;
    std::string_view port;
    if (uri.starts_with('['))
    {
        const auto close = uri.find(']');
        if (close == std::string_view::npos || close + 1 >= uri.size() ||
            uri[close + 1] != ':')
        {
            return make_error_code(errc::invalid_uri);
        }
        host = uri.substr(1, close - 1);
        port = uri.substr(close + 2);
    }
    else
    {
        const auto colon = uri.rfind(':');
        if (colon == std::string_view::npos)
            return make_error_code(errc::invalid_uri);
        host = uri.substr(0, colon);
        port = uri.substr(colon + 1);
        if (host.find(':') != std::string_view::npos)
            return make_error_code(errc::invalid_uri);
    }

    unsigned value = 0;
    const auto parsed =
        std::from_chars(port.data(), port.data() + port.size(), value);
    if (port.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != port.data() + port.size() || value > 65535)
    {
        return make_error_code(errc::invalid_uri);
    }

    if (!host.empty())
    {
        boost::system::error_code ec;
        asio::ip::make_address(std::string(host), ec);
        if (ec)
            return make_error_code(errc::invalid_uri);
    }

    address.host = std::string(host);
    address.port = static_cast<std::uint16_t>(value);
    address.allow = std::move(allow);
    return address;
}

std::string make_listen_uri(const listen_address& address)
{
    std::string text;
    switch (address.protocol)
    {
    case listen_protocol::tcp:
        text = tcp_scheme;
        break;
    case listen_protocol::udp:
        text = udp_scheme;
        break;
    case listen_protocol::rfc2217:
        text = rfc2217_scheme;
        break;
    }
    if (address.host.find(':') != std::string::npos)
        text += '[' + address.host + ']';
    else
        text += address.host;
    text += ':' + std::to_string(address.port);
    for (std::size_t index = 0; index < address.allow.size(); ++index)
    {
        text += index == 0 ? "?allow=" : ",";
        text += address.allow[index];
    }
    return text;
}

/// Everything runs on one strand, so the sockets and the queues need no lock.
/// The handlers hold the state, which lets it outlive the transport object
/// while an operation is still outstanding.
class listen_transport::state : public std::enable_shared_from_this<state>
{
public:
    state(const executor_type& executor, listen_address address,
          listen_limits limits)
        : executor_(executor)
        , strand_(asio::make_strand(executor))
        , address_(std::move(address))
        , limits_(limits)
        , acceptor_(strand_)
        , datagrams_(strand_)
        , accept_timer_(strand_)
    {
    }

    error_code open()
    {
        VSS_PROFILE_ZONE("ports.listen.open");
        boost::system::error_code ec;
        const auto rules = parse_allow_list(address_.allow);
        if (!rules)
            return make_error_code(errc::invalid_uri);
        rules_ = *rules;
        asio::ip::address ip = asio::ip::address_v4::any();
        if (!address_.host.empty())
        {
            ip = asio::ip::make_address(address_.host, ec);
            if (ec)
                return make_error_code(errc::invalid_uri);
        }

        if (stream())
        {
            const asio::ip::tcp::endpoint where(ip, address_.port);
            acceptor_.open(where.protocol(), ec);
            if (ec)
                return ec;
            ec = detail::platform_prepare_listening_socket(
                static_cast<std::uintptr_t>(acceptor_.native_handle()));
            if (ec)
                return ec;
            acceptor_.bind(where, ec);
            if (ec)
                return ec;
            acceptor_.listen(asio::socket_base::max_listen_connections, ec);
            if (ec)
                return ec;
            port_.store(acceptor_.local_endpoint(ec).port());
        }
        else
        {
            const asio::ip::udp::endpoint where(ip, address_.port);
            datagrams_.open(where.protocol(), ec);
            if (ec)
                return ec;
            datagrams_.bind(where, ec);
            if (ec)
                return ec;
            port_.store(datagrams_.local_endpoint(ec).port());
        }
        open_.store(true);
        VSS_PROFILE_MESSAGE("ports.listen.opened");
        return {};
    }

    /// TCP and RFC 2217 are byte streams; UDP is datagrams.
    bool stream() const noexcept
    {
        return address_.protocol != listen_protocol::udp;
    }

    bool serial_role() const noexcept
    {
        return address_.protocol == listen_protocol::rfc2217;
    }

    void start()
    {
        if (stream())
            accept_next();
        else
            receive_next();
    }

    void read(const asio::mutable_buffer& buffer, io_handler handler)
    {
        asio::post(strand_,
                   [self = shared_from_this(), buffer,
                    handler = std::move(handler)]() mutable
                   {
                       VSS_PROFILE_ZONE("ports.listen.read");
                       if (self->closed_ || !self->open_.load())
                       {
                           self->complete(std::move(handler),
                                          asio::error::operation_aborted, 0);
                           return;
                       }
                       std::optional<io_handler> rejected;
                       {
                           std::lock_guard lock(self->read_mutex_);
                           if (self->pending_read_.has_value())
                               rejected = std::move(handler);
                           else
                               self->pending_read_ =
                                   pending{buffer, std::move(handler)};
                       }
                       if (rejected)
                       {
                           self->complete(std::move(*rejected),
                                          asio::error::already_started, 0);
                           return;
                       }
                       self->deliver();
                   });
    }

    void write(const asio::const_buffer& buffer, io_handler handler)
    {
        auto payload = std::make_shared<std::vector<char>>(
            static_cast<const char*>(buffer.data()),
            static_cast<const char*>(buffer.data()) + buffer.size());
        asio::post(strand_,
                   [self = shared_from_this(), payload,
                    handler = std::move(handler)]() mutable
                   {
                       VSS_PROFILE_ZONE("ports.listen.write");
                       self->send(payload, std::move(handler));
                   });
    }

    // close() and cancel() end a parked read at once, on the caller's thread,
    // and do not wait for the strand. The parked handler holds the endpoint,
    // the endpoint holds this transport and this state holds the handler: if
    // the strand never ran again (the hub stops its io_context when every
    // endpoint has closed), the cycle would never be broken.
    void close()
    {
        VSS_PROFILE_ZONE("ports.listen.close");
        open_.store(false);
        {
            // A notice after close would reach an endpoint that has let go.
            std::lock_guard lock(notice_mutex_);
            notice_sink_ = nullptr;
        }
        abort_pending_read();
        asio::post(strand_, [self = shared_from_this()] { self->shut_down(); });
    }

    void cancel()
    {
        abort_pending_read();
        // A read posted before this call has not reached the strand yet.
        asio::post(strand_,
                   [self = shared_from_this()] { self->abort_pending_read(); });
    }

    void set_notice_sink(transport::notice_sink sink)
    {
        std::lock_guard lock(notice_mutex_);
        if (open_.load())
            notice_sink_ = std::move(sink);
    }

    /// Only the RFC 2217 role has control lines.
    bool has_lines() const noexcept { return serial_role(); }

    void set_line(control_line line, bool value)
    {
        if (!serial_role())
            return;
        control_lines inputs;
        {
            std::lock_guard lock(lines_mutex_);
            switch (line)
            {
            case control_line::cts:
            case control_line::dsr:
            case control_line::dcd:
            case control_line::ri:
                inputs_.set(line, value);
                break;
            default:
                outputs_.set(line, value);
                break;
            }
            inputs = inputs_;
        }
        asio::post(strand_, [self = shared_from_this(), inputs]
                   { self->notify_modem(inputs); });
    }

    control_lines lines() const
    {
        std::lock_guard lock(lines_mutex_);
        return control_lines::from_bits(inputs_.bits | outputs_.bits);
    }

    bool is_open() const noexcept { return open_.load(); }
    std::uint16_t port() const noexcept { return port_.load(); }
    std::size_t peers() const noexcept { return peers_.load(); }
    std::size_t refused() const noexcept { return refused_.load(); }
    const listen_address& address() const noexcept { return address_; }

private:
    struct pending
    {
        asio::mutable_buffer buffer;
        io_handler handler;
    };

    struct outgoing
    {
        /// Shared by every client the bytes go to.
        std::shared_ptr<std::string> bytes;
        /// How much of it the socket has taken.
        std::size_t sent = 0;
        /// A reply of the RFC 2217 session, not payload.
        bool reply = false;
    };

    /// The hub's write that waits for a client to catch up.
    struct held
    {
        io_handler handler;
        std::size_t size = 0;
    };

    struct client
    {
        client(asio::ip::tcp::socket accepted, std::string address)
            : socket(std::move(accepted))
            , name(std::move(address))
            , deadline(socket.get_executor())
        {
        }

        asio::ip::tcp::socket socket;
        /// Where it connected from, for the notices.
        std::string name;
        std::array<char, chunk_size> buffer{};
        bool reading = false;
        /// Present for the RFC 2217 role: the Telnet and COM-PORT state.
        std::optional<rfc2217::session> session;
        /// A socket takes one write at a time, and payload and protocol
        /// replies share it.
        std::deque<outgoing> queue;
        /// Bytes in the queue the socket has not taken yet, and how many of
        /// them are replies of the RFC 2217 session.
        std::size_t queued = 0;
        std::size_t queued_replies = 0;
        bool writing = false;
        /// Set once the client is closed and forgotten. Handlers that were
        /// already waiting for it find out here.
        bool dropped = false;
        /// Fires when the write in progress has made no progress for the
        /// write deadline. The generation tells a stale wait from the
        /// current one.
        asio::steady_timer deadline;
        std::uint64_t deadline_generation = 0;
    };

    /// The parked read, removed from the state; empty when none is waiting.
    std::optional<pending> take_pending_read()
    {
        std::lock_guard lock(read_mutex_);
        std::optional<pending> taken = std::move(pending_read_);
        pending_read_.reset(); // a moved-from optional still holds a value
        return taken;
    }

    void abort_pending_read()
    {
        if (auto taken = take_pending_read())
            complete(std::move(taken->handler), asio::error::operation_aborted,
                     0);
    }

    void complete(io_handler handler, error_code ec, std::size_t count)
    {
        asio::post(executor_,
                   [handler = std::move(handler), ec, count]() mutable
                   { handler(ec, count); });
    }

    // -- TCP ---------------------------------------------------------------

    void accept_next()
    {
        acceptor_.async_accept(
            [self = shared_from_this()](error_code ec,
                                        asio::ip::tcp::socket socket)
            {
                VSS_PROFILE_ZONE("ports.tcp.accept");
                // A test can make accept fail; the connection is dropped.
                if (!ec && self->limits_.accept_fault)
                    ec = self->limits_.accept_fault();
                if (ec)
                {
                    if (!self->closed_ && ec != asio::error::operation_aborted)
                        self->accept_failed(ec);
                    return;
                }
                // An accept that finished just before close() ran its
                // handler after it: the socket is closed with the lambda.
                if (self->closed_)
                    return;
                self->accept_failures_ = 0;
                self->accept_failure_noticed_ = false;
                boost::system::error_code remote_ec;
                const auto remote = socket.remote_endpoint(remote_ec);
                if (remote_ec || !rules_admit(self->rules_, remote.address()))
                {
                    // Not on the allow list: closed before a byte is read.
                    boost::system::error_code ignored;
                    socket.close(ignored);
                    self->refused_.fetch_add(1);
                    VSS_PROFILE_MESSAGE("ports.tcp.refused");
                    self->accept_next();
                    return;
                }
                if (self->clients_.size() >= self->limits_.max_clients)
                {
                    boost::system::error_code ignored;
                    socket.close(ignored);
                    self->refused_.fetch_add(1);
                    VSS_PROFILE_MESSAGE("ports.tcp.full");
                    self->note_full(describe_endpoint(remote));
                    self->accept_next();
                    return;
                }
                // A reply must not wait for more data to share a packet with.
                boost::system::error_code ignored;
                socket.set_option(asio::ip::tcp::no_delay(true), ignored);
                // A client whose computer has gone is found in the end, and
                // does not keep one of the places for good.
                socket.set_option(asio::socket_base::keep_alive(true), ignored);
                auto made = std::make_shared<client>(std::move(socket),
                                                     describe_endpoint(remote));
                self->clients_.push_back(made);
                self->peers_.store(self->clients_.size());
                if (self->serial_role())
                {
                    made->session.emplace();
                    self->enqueue(made,
                                  std::make_shared<std::string>(
                                      made->session->greeting()),
                                  true);
                }
                VSS_PROFILE_PLOT("ports.tcp.clients", self->clients_.size());
                VSS_PROFILE_MESSAGE("ports.tcp.client_connected");
                self->read_client(made);
                self->accept_next();
            });
    }

    /// Accept failed, most likely for want of descriptors. The connection that
    /// could not be accepted is still in the backlog, so trying again at once
    /// fails again at once. Wait, for longer each time, and say so once.
    void accept_failed(error_code ec)
    {
        VSS_PROFILE_MESSAGE("ports.tcp.accept_failed");
        const auto delay = accept_retry_delay(accept_failures_, limits_);
        ++accept_failures_;
        if (!accept_failure_noticed_ && accept_notice_.ready())
        {
            accept_failure_noticed_ = true;
            notify("Accepting connections failed (" + ec.message() +
                   "). Trying again in " + describe_duration(delay) +
                   ", and waiting longer, up to " +
                   describe_duration(limits_.accept_retry_last) +
                   ", while it keeps failing.");
        }
        accept_timer_.expires_after(delay);
        accept_timer_.async_wait(
            [self = shared_from_this()](error_code wait_ec)
            {
                if (!wait_ec && !self->closed_)
                    self->accept_next();
            });
    }

    /// A connection was closed because the port has its most clients.
    void note_full(const std::string& name)
    {
        if (!full_notice_.ready())
            return;
        std::string text = "Refused client " + name +
                           ": this port already has " +
                           std::to_string(limits_.max_clients) + " clients.";
        if (const std::size_t left_out = full_notice_.take_held_back())
        {
            text += " " + std::to_string(left_out) +
                    " more were refused since the last notice.";
        }
        notify(std::move(text));
    }

    void notify(std::string text)
    {
        transport::notice_sink sink;
        {
            std::lock_guard lock(notice_mutex_);
            sink = notice_sink_;
        }
        if (sink)
            sink(std::move(text));
    }

    void read_client(const std::shared_ptr<client>& peer)
    {
        if (closed_ || peer->dropped || peer->reading)
            return;
        if (received_.size() >= receive_limit)
            return; // read_client runs again from deliver() once drained
        peer->reading = true;
        peer->socket.async_read_some(
            asio::buffer(peer->buffer),
            [self = shared_from_this(), peer](error_code ec, std::size_t count)
            {
                VSS_PROFILE_ZONE_NAMED(zone, "ports.tcp.receive");
                VSS_PROFILE_ZONE_VALUE(zone, count);
                peer->reading = false;
                if (ec || peer->dropped)
                {
                    self->drop(peer, {});
                    return;
                }
                if (peer->session)
                {
                    self->consume_serial(peer, count);
                }
                else
                {
                    self->received_.insert(
                        self->received_.end(), peer->buffer.begin(),
                        peer->buffer.begin() +
                            static_cast<std::ptrdiff_t>(count));
                }
                VSS_PROFILE_PLOT("ports.listen.received",
                                 self->received_.size());
                self->deliver();
                self->read_client(peer);
            });
    }

    /// What an RFC 2217 client sent: payload for the hub, replies for the
    /// client, and the lines it drove.
    void consume_serial(const std::shared_ptr<client>& peer, std::size_t count)
    {
        auto& slot = peer->session;
        if (!slot)
            return;
        VSS_PROFILE_ZONE("ports.rfc2217.consume");
        rfc2217::session& session = *slot;
        const auto fed = session.feed(
            std::string_view(peer->buffer.data(), count));
        received_.insert(received_.end(), fed.data.begin(), fed.data.end());
        if (!fed.reply.empty())
            enqueue(peer, std::make_shared<std::string>(fed.reply), true);
        if (fed.modem_state_requested)
        {
            const std::string text =
                session.modem_state(current_inputs(), true);
            if (!text.empty())
                enqueue(peer, std::make_shared<std::string>(text), true);
        }
        if (fed.outputs_changed)
        {
            {
                std::lock_guard lock(lines_mutex_);
                const control_lines driven = session.outputs();
                for (const control_line line :
                     {control_line::dtr, control_line::rts,
                      control_line::break_signal})
                    outputs_.set(line, driven.test(line));
            }
            // The endpoint learns of the change through a read that returns
            // no bytes; see deliver().
            std::lock_guard lock(read_mutex_);
            lines_dirty_ = true;
        }
    }

    control_lines current_inputs() const
    {
        std::lock_guard lock(lines_mutex_);
        return inputs_;
    }

    /// Queues bytes for one client. Payload is always queued: how far behind
    /// the clients are decides when the hub's write is done (see
    /// release_held), not whether a client gets the bytes. A reply of the
    /// RFC 2217 session is a request answered whether or not the client reads
    /// on, so a client whose replies pile up past the limit is disconnected.
    /// An empty queue of replies takes one whatever its size.
    void enqueue(const std::shared_ptr<client>& peer,
                 std::shared_ptr<std::string> bytes, bool reply = false)
    {
        if (peer->dropped || bytes->empty())
            return;
        const std::size_t size = bytes->size();
        if (reply && peer->queued_replies > 0 &&
            peer->queued_replies + size > limits_.max_client_queue_bytes)
        {
            drop(peer, behind_reason());
            return;
        }
        peer->queue.push_back(outgoing{std::move(bytes), 0, reply});
        peer->queued += size;
        if (reply)
            peer->queued_replies += size;
        if (!peer->writing)
            write_next(peer);
    }

    std::string behind_reason() const
    {
        return "it is more than " +
               std::to_string(limits_.max_client_queue_bytes) +
               " bytes behind in taking what this port sends.";
    }

    void write_next(const std::shared_ptr<client>& peer)
    {
        if (peer->dropped)
            return;
        if (peer->queue.empty())
        {
            peer->writing = false;
            ++peer->deadline_generation;
            peer->deadline.cancel();
            return;
        }
        peer->writing = true;
        const outgoing& item = peer->queue.front();
        const std::size_t take =
            std::min(write_slice, item.bytes->size() - item.sent);
        arm_deadline(peer);
        // Part of it may be taken: that is progress, and starts the deadline
        // again.
        peer->socket.async_write_some(
            asio::buffer(item.bytes->data() + item.sent, take),
            [self = shared_from_this(), peer,
             bytes = item.bytes](error_code ec, std::size_t count)
            {
                VSS_PROFILE_ZONE("ports.tcp.sent");
                if (peer->dropped)
                    return;
                if (ec)
                {
                    // Nothing more can be sent.
                    self->drop(peer, {});
                    return;
                }
                auto& front = peer->queue.front();
                front.sent += count;
                peer->queued -= count;
                if (front.reply)
                    peer->queued_replies -= count;
                if (front.sent >= front.bytes->size())
                    peer->queue.pop_front();
                // A client has fallen back to the limit, perhaps.
                self->release_held();
                self->write_next(peer);
            });
    }

    /// Starts the time a client has to take the next part.
    void arm_deadline(const std::shared_ptr<client>& peer)
    {
        const std::uint64_t generation = ++peer->deadline_generation;
        peer->deadline.expires_after(limits_.write_deadline);
        peer->deadline.async_wait(
            [self = shared_from_this(), peer, generation](error_code ec)
            {
                if (ec || peer->dropped ||
                    generation != peer->deadline_generation)
                    return;
                self->drop(peer,
                           "it took no data for " +
                               describe_duration(self->limits_.write_deadline) +
                               ".");
            });
    }

    /// Closes a client and forgets it. `reason`, when there is one, is a
    /// notice: the client did not leave of its own accord.
    void drop(const std::shared_ptr<client>& peer, const std::string& reason)
    {
        if (peer->dropped)
            return;
        VSS_PROFILE_ZONE("ports.tcp.drop");
        peer->dropped = true;
        boost::system::error_code ignored;
        peer->socket.close(ignored);
        peer->deadline.cancel();
        peer->queue.clear();
        peer->queued = 0;
        peer->queued_replies = 0;
        std::erase(clients_, peer);
        peers_.store(clients_.size());
        VSS_PROFILE_PLOT("ports.tcp.clients", clients_.size());
        VSS_PROFILE_MESSAGE("ports.tcp.client_dropped");
        if (!reason.empty())
            notify("Disconnected client " + peer->name + " because " + reason);
        // The one that held the hub's write back may have been this one, or
        // the last.
        release_held();
    }

    /// Completes the hub's write that is waiting for the clients, when they
    /// have taken enough. The source of the data is slowed to the pace of the
    /// fastest client: while every client is more than the limit behind, the
    /// write stays unfinished, the endpoint's own queue fills and the device
    /// waits, as it does for a port with one slow reader. Once one client is
    /// within the limit, the write is done and the clients still over it are
    /// the ones that do not keep up, and are disconnected. With no client left
    /// there is nobody to wait for.
    void release_held()
    {
        if (!held_write_)
            return;
        const std::size_t limit = limits_.max_client_queue_bytes;
        std::vector<std::shared_ptr<client>> behind;
        for (const auto& peer : clients_)
        {
            if (peer->queued > limit)
                behind.push_back(peer);
        }
        if (!clients_.empty() && behind.size() == clients_.size())
            return;
        // Taken first: drop() calls back into this function.
        held taken = std::move(*held_write_);
        held_write_.reset();
        for (const auto& peer : behind)
            drop(peer, behind_reason());
        complete(std::move(taken.handler), {}, taken.size);
    }

    // -- UDP ---------------------------------------------------------------

    void receive_next()
    {
        if (closed_)
            return;
        if (received_.size() >= receive_limit)
        {
            receiving_ = false;
            return;
        }
        receiving_ = true;
        datagrams_.async_receive_from(
            asio::buffer(datagram_buffer_), sender_,
            [self = shared_from_this()](error_code ec, std::size_t count)
            {
                VSS_PROFILE_ZONE_NAMED(zone, "ports.udp.receive");
                VSS_PROFILE_ZONE_VALUE(zone, count);
                self->receiving_ = false;
                if (ec == asio::error::operation_aborted || self->closed_)
                    return;
                if (!ec && !rules_admit(self->rules_, self->sender_.address()))
                {
                    self->refused_.fetch_add(1);
                }
                else if (!ec)
                {
                    self->remember(self->sender_);
                    self->received_.insert(
                        self->received_.end(), self->datagram_buffer_.begin(),
                        self->datagram_buffer_.begin() +
                            static_cast<std::ptrdiff_t>(count));
                    self->deliver();
                }
                self->receive_next();
            });
    }

    void remember(const asio::ip::udp::endpoint& sender)
    {
        std::erase(senders_, sender);
        senders_.push_front(sender);
        if (senders_.size() > udp_peer_limit)
            senders_.pop_back();
        peers_.store(senders_.size());
    }

    // -- shared ------------------------------------------------------------

    void deliver()
    {
        VSS_PROFILE_ZONE("ports.listen.deliver");
        std::optional<pending> taken;
        {
            std::lock_guard lock(read_mutex_);
            // A read with no bytes tells the endpoint that only the control
            // lines changed.
            if (pending_read_ && (!received_.empty() || lines_dirty_))
            {
                taken = std::move(pending_read_);
                pending_read_.reset();
                lines_dirty_ = false;
            }
        }
        if (taken)
        {
            const std::size_t count =
                std::min(taken->buffer.size(), received_.size());
            std::copy_n(received_.begin(), count,
                        static_cast<char*>(taken->buffer.data()));
            received_.erase(received_.begin(),
                            received_.begin() +
                                static_cast<std::ptrdiff_t>(count));
            complete(std::move(taken->handler), {}, count);
        }
        if (received_.size() < receive_limit)
        {
            if (stream())
            {
                const auto waiting = clients_;
                for (const auto& peer : waiting)
                    read_client(peer);
            }
            else if (!receiving_)
            {
                receive_next();
            }
        }
    }

    void send(const std::shared_ptr<std::vector<char>>& payload,
              io_handler handler)
    {
        VSS_PROFILE_ZONE("ports.listen.send");
        const std::size_t size = payload->size();
        if (closed_)
        {
            complete(std::move(handler), asio::error::operation_aborted, 0);
            return;
        }

        if (stream())
        {
            if (clients_.empty())
            {
                complete(std::move(handler), {}, size); // nobody to tell
                return;
            }
            // The endpoint sends one write at a time and waits for it.
            if (held_write_)
            {
                complete(std::move(handler), asio::error::already_started, 0);
                return;
            }
            const std::string_view view(payload->data(), payload->size());
            auto wire = std::make_shared<std::string>(
                serial_role() ? rfc2217::session::encode(view)
                              : std::string(view));
            // Payload is never refused, so clients_ does not change here.
            for (const auto& peer : clients_)
                enqueue(peer, wire);
            // Done when the clients have it queued, or, if they all lag, when
            // the first has caught up.
            held_write_ = held{std::move(handler), size};
            release_held();
            return;
        }

        if (senders_.empty())
        {
            complete(std::move(handler), {}, size);
            return;
        }
        auto remaining = std::make_shared<std::size_t>(senders_.size());
        auto shared_handler = std::make_shared<io_handler>(std::move(handler));
        const auto targets = std::vector<asio::ip::udp::endpoint>(
            senders_.begin(), senders_.end());
        for (const auto& target : targets)
        {
            datagrams_.async_send_to(
                asio::buffer(*payload), target,
                [self = shared_from_this(), payload, remaining, shared_handler,
                 size](error_code, std::size_t)
                {
                    if (--*remaining == 0)
                        self->complete(std::move(*shared_handler), {}, size);
                });
        }
    }

    /// Tells every RFC 2217 client the modem lines that changed.
    void notify_modem(control_lines inputs)
    {
        VSS_PROFILE_ZONE("ports.rfc2217.modem");
        const auto targets = clients_; // enqueue can drop a client
        for (const auto& peer : targets)
        {
            auto& slot = peer->session;
            if (!slot || !slot->com_port_active())
                continue;
            const std::string text = slot->modem_state(inputs);
            if (!text.empty())
                enqueue(peer, std::make_shared<std::string>(text), true);
        }
    }

    void shut_down()
    {
        if (closed_)
            return;
        VSS_PROFILE_ZONE("ports.listen.shutdown");
        VSS_PROFILE_MESSAGE("ports.listen.closed");
        closed_ = true;
        boost::system::error_code ignored;
        acceptor_.close(ignored);
        accept_timer_.cancel();
        datagrams_.close(ignored);
        for (const auto& peer : clients_)
        {
            peer->dropped = true;
            peer->socket.close(ignored);
            peer->deadline.cancel();
        }
        clients_.clear();
        peers_.store(0);
        if (held_write_)
        {
            complete(std::move(held_write_->handler),
                     asio::error::operation_aborted, 0);
            held_write_.reset();
        }
        abort_pending_read();
    }

    executor_type executor_;
    asio::strand<executor_type> strand_;
    listen_address address_;
    listen_limits limits_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::udp::socket datagrams_;
    /// Waits out a failed accept.
    asio::steady_timer accept_timer_;
    /// Accepts that failed in a row, and whether the first has been reported.
    std::size_t accept_failures_ = 0;
    bool accept_failure_noticed_ = false;
    /// Connections refused for lack of room since the last notice about it.
    notice_throttle full_notice_;
    /// The hub's write waiting for the clients; strand only. The endpoint has
    /// one write in flight, so there is at most one.
    std::optional<held> held_write_;
    notice_throttle accept_notice_;

    std::vector<std::shared_ptr<client>> clients_;
    std::deque<asio::ip::udp::endpoint> senders_;
    asio::ip::udp::endpoint sender_;
    std::array<char, 65536> datagram_buffer_{};
    bool receiving_ = false;

    std::deque<char> received_;
    /// Guarded by read_mutex_: close() and cancel() take it from any thread.
    std::mutex read_mutex_;
    std::optional<pending> pending_read_;
    /// A client changed DTR, RTS or break and the endpoint has not heard.
    bool lines_dirty_ = false;
    bool closed_ = false;
    /// The allow list, parsed when the transport is opened.
    std::vector<allow_rule> rules_;

    /// Control lines of the RFC 2217 role. Inputs come from the peers on the
    /// hub's links, outputs from what the clients commanded.
    mutable std::mutex lines_mutex_;
    control_lines inputs_;
    control_lines outputs_;

    /// Where notices go. close() clears it from any thread.
    std::mutex notice_mutex_;
    transport::notice_sink notice_sink_;

    std::atomic<bool> open_{false};
    std::atomic<std::uint16_t> port_{0};
    std::atomic<std::size_t> peers_{0};
    std::atomic<std::size_t> refused_{0};
};

listen_transport::listen_transport(const executor_type& executor,
                                   listen_address address, error_code& ec,
                                   listen_limits limits)
    : state_(std::make_shared<state>(executor, std::move(address), limits))
{
    ec = state_->open();
    if (!ec)
        state_->start();
}

listen_transport::~listen_transport()
{
    try
    {
        state_->close();
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
        // close() posts to the strand and can throw; a destructor must not.
    }
}

std::string listen_transport::describe() const
{
    const auto& address = state_->address();
    const std::string host = address.host.empty() ? "0.0.0.0" : address.host;
    std::string name = "TCP";
    if (address.protocol == listen_protocol::udp)
        name = "UDP";
    else if (address.protocol == listen_protocol::rfc2217)
        name = "RFC 2217";
    return name + " listening on " + host + ":" +
           std::to_string(state_->port()) + " (" +
           std::to_string(state_->peers()) +
           (address.protocol == listen_protocol::udp ? " senders)"
                                                     : " connected)");
}

std::string listen_transport::public_path() const
{
    auto address = state_->address();
    address.port = state_->port();
    return make_listen_uri(address);
}

bool listen_transport::is_open() const
{
    return state_->is_open();
}

void listen_transport::async_read_some(asio::mutable_buffer buffer,
                                       io_handler handler)
{
    state_->read(buffer, std::move(handler));
}

void listen_transport::async_write_some(asio::const_buffer buffer,
                                        io_handler handler)
{
    state_->write(buffer, std::move(handler));
}

void listen_transport::close()
{
    state_->close();
}

void listen_transport::cancel()
{
    state_->cancel();
}

bool listen_transport::supports_control_lines() const noexcept
{
    return state_->has_lines();
}

void listen_transport::set_control_line(control_line line, bool value)
{
    state_->set_line(line, value);
}

control_lines listen_transport::lines() const
{
    return state_->lines();
}

std::uint16_t listen_transport::local_port() const noexcept
{
    return state_->port();
}

std::size_t listen_transport::peer_count() const noexcept
{
    return state_->peers();
}

std::optional<std::size_t> listen_transport::client_count() const noexcept
{
    return state_->peers();
}

std::size_t listen_transport::refused_count() const noexcept
{
    return state_->refused();
}

void listen_transport::set_notice_sink(notice_sink sink)
{
    state_->set_notice_sink(std::move(sink));
}

} // namespace vss
