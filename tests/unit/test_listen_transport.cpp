//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/ports/listen_transport.hpp>

#include "test_framework.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace vss;
namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

/// An io_context that runs on its own thread, like the hub's.
struct loop
{
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{
        io.get_executor()};
    std::thread thread{[this] { io.run(); }};

    ~loop()
    {
        guard.reset();
        try
        {
            io.stop();
        }
        catch (...)
        {
            // best effort: a destructor must not throw, and nothing can act on a failed stop
        }
        thread.join();
    }
};

struct read_result
{
    boost::system::error_code error;
    std::string text;
};

/// One read from the transport, waited for up to `wait`.
read_result read_from(listen_transport& transport,
                      std::chrono::milliseconds wait = 3000ms)
{
    auto promise = std::make_shared<std::promise<read_result>>();
    auto buffer = std::make_shared<std::array<char, 256>>();
    transport.async_read_some(
        asio::buffer(*buffer),
        [promise, buffer](boost::system::error_code ec, std::size_t count)
        { promise->set_value({ec, std::string(buffer->data(), count)}); });
    auto future = promise->get_future();
    if (future.wait_for(wait) != std::future_status::ready)
    {
        transport.cancel();
        future.wait();
        return {asio::error::timed_out, {}};
    }
    return future.get();
}

/// A write through the transport, waited for.
std::size_t write_to(listen_transport& transport, const std::string& text)
{
    auto promise = std::make_shared<std::promise<std::size_t>>();
    transport.async_write_some(
        asio::buffer(text),
        [promise](boost::system::error_code, std::size_t count)
        { promise->set_value(count); });
    auto future = promise->get_future();
    REQUIRE(future.wait_for(3000ms) == std::future_status::ready);
    return future.get();
}

bool wait_for_peers(const listen_transport& transport, std::size_t count)
{
    for (int attempt = 0; attempt < 300; ++attempt)
    {
        if (transport.peer_count() == count)
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

std::unique_ptr<listen_transport> listen_on(loop& running,
                                            listen_protocol protocol,
                                            listen_limits limits = {})
{
    listen_address address;
    address.protocol = protocol;
    address.host = "127.0.0.1";
    address.port = 0;
    error_code ec;
    auto transport = std::make_unique<listen_transport>(
        running.io.get_executor(), address, ec, limits);
    REQUIRE(!ec);
    REQUIRE(transport->local_port() != 0);
    return transport;
}

asio::ip::tcp::socket connect_to(asio::io_context& io, std::uint16_t port)
{
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    return socket;
}

std::string receive(asio::ip::tcp::socket& socket, std::size_t count)
{
    std::string text(count, '\0');
    asio::read(socket, asio::buffer(text));
    return text;
}

} // namespace

TEST_CASE("listen: parsing the URIs")
{
    const auto tcp = parse_listen_uri("tcp-listen://127.0.0.1:4001");
    REQUIRE(tcp.has_value());
    REQUIRE(tcp->protocol == listen_protocol::tcp);
    REQUIRE_EQ(tcp->host, std::string("127.0.0.1"));
    REQUIRE_EQ(tcp->port, std::uint16_t{4001});

    const auto all = parse_listen_uri("udp-listen://:5000");
    REQUIRE(all.has_value());
    REQUIRE(all->protocol == listen_protocol::udp);
    REQUIRE(all->host.empty());

    const auto six = parse_listen_uri("tcp-listen://[::1]:4001");
    REQUIRE(six.has_value());
    REQUIRE_EQ(six->host, std::string("::1"));

    REQUIRE_EQ(make_listen_uri(*tcp),
               std::string("tcp-listen://127.0.0.1:4001"));
    REQUIRE_EQ(make_listen_uri(*six), std::string("tcp-listen://[::1]:4001"));
    REQUIRE_EQ(make_listen_uri(*all), std::string("udp-listen://:5000"));

    REQUIRE(is_listen_uri("tcp-listen://x"));
    REQUIRE(!is_listen_uri("tcp://127.0.0.1:4001"));
    for (const char* bad :
         {"tcp-listen://", "tcp-listen://127.0.0.1", "tcp-listen://127.0.0.1:",
          "tcp-listen://127.0.0.1:70000", "tcp-listen://host.example:4001",
          "tcp-listen://[::1:4001", "tcp-listen://::1:4001",
          "tcp-listen://127.0.0.1:40x1", "tcp-listen://127.0.0.1:4001/path",
          "serial://COM3"})
    {
        REQUIRE(!parse_listen_uri(bad).has_value());
    }
}

TEST_CASE("listen: bytes from a TCP client are read and the reply reaches it")
{
    loop running;
    auto transport = listen_on(running, listen_protocol::tcp);
    asio::io_context client_io;
    auto client = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 1));

    asio::write(client, asio::buffer(std::string("hello")));
    const auto got = read_from(*transport);
    REQUIRE(!got.error);
    REQUIRE_EQ(got.text, std::string("hello"));

    REQUIRE_EQ(write_to(*transport, "world"), std::size_t{5});
    REQUIRE_EQ(receive(client, 5), std::string("world"));
}

TEST_CASE("listen: a write goes to every connected client")
{
    loop running;
    auto transport = listen_on(running, listen_protocol::tcp);
    asio::io_context client_io;
    auto first = connect_to(client_io, transport->local_port());
    auto second = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 2));

    REQUIRE_EQ(write_to(*transport, "all"), std::size_t{3});
    REQUIRE_EQ(receive(first, 3), std::string("all"));
    REQUIRE_EQ(receive(second, 3), std::string("all"));

    asio::write(second, asio::buffer(std::string("two")));
    REQUIRE_EQ(read_from(*transport).text, std::string("two"));
}

TEST_CASE("listen: with nobody connected a write is dropped without an error")
{
    loop running;
    auto transport = listen_on(running, listen_protocol::tcp);
    REQUIRE_EQ(transport->peer_count(), std::size_t{0});
    REQUIRE_EQ(write_to(*transport, "nobody"), std::size_t{6});
}

TEST_CASE("listen: a client that leaves is forgotten")
{
    loop running;
    auto transport = listen_on(running, listen_protocol::tcp);
    asio::io_context client_io;
    {
        auto client = connect_to(client_io, transport->local_port());
        REQUIRE(wait_for_peers(*transport, 1));
    }
    REQUIRE(wait_for_peers(*transport, 0));

    auto again = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 1));
    asio::write(again, asio::buffer(std::string("back")));
    REQUIRE_EQ(read_from(*transport).text, std::string("back"));
}

TEST_CASE("listen: a port that is taken is reported")
{
    loop running;
    auto first = listen_on(running, listen_protocol::tcp);

    listen_address address;
    address.host = "127.0.0.1";
    address.port = first->local_port();
    error_code ec;
    listen_transport second(running.io.get_executor(), address, ec);
    REQUIRE(ec == asio::error::address_in_use);
}

TEST_CASE("listen: closing aborts a read that is waiting")
{
    loop running;
    auto transport = listen_on(running, listen_protocol::tcp);

    auto promise = std::make_shared<std::promise<boost::system::error_code>>();
    auto buffer = std::make_shared<std::array<char, 16>>();
    transport->async_read_some(
        asio::buffer(*buffer),
        [promise, buffer](boost::system::error_code ec, std::size_t)
        { promise->set_value(ec); });
    transport->close();

    auto future = promise->get_future();
    REQUIRE(future.wait_for(3000ms) == std::future_status::ready);
    REQUIRE(future.get() == asio::error::operation_aborted);
    REQUIRE(!transport->is_open());
}

TEST_CASE("listen: close frees a parked read even if the loop never runs again")
{
    // The hub stops its io_context once every endpoint has closed, so the
    // strand may never run another handler. An endpoint's read handler holds
    // the endpoint, which holds the transport: if close() left that handler
    // parked, the transport and the handler would keep each other alive.
    std::weak_ptr<listen_transport> weak;
    {
        asio::io_context io;
        listen_address address;
        address.host = "127.0.0.1";
        error_code ec;
        auto transport =
            std::make_shared<listen_transport>(io.get_executor(), address, ec);
        REQUIRE(!ec);
        weak = transport;

        auto buffer = std::make_shared<std::array<char, 16>>();
        transport->async_read_some(
            asio::buffer(*buffer),
            [transport, buffer](boost::system::error_code, std::size_t) {});
        io.poll(); // the strand parks the read

        transport->close();
        transport.reset();
        // io is not run again; destroying it drops what is still queued.
    }
    REQUIRE(weak.expired());
}

TEST_CASE("listen: cancel completes a parked read at once")
{
    asio::io_context io;
    listen_address address;
    address.host = "127.0.0.1";
    error_code ec;
    listen_transport transport(io.get_executor(), address, ec);
    REQUIRE(!ec);

    bool done = false;
    boost::system::error_code result;
    std::array<char, 16> buffer{};
    transport.async_read_some(asio::buffer(buffer),
                              [&](boost::system::error_code error, std::size_t)
                              {
                                  done = true;
                                  result = error;
                              });
    io.poll();
    REQUIRE(!done);

    transport.cancel();
    io.poll();
    REQUIRE(done);
    REQUIRE(result == asio::error::operation_aborted);
    REQUIRE(transport.is_open());
}

TEST_CASE("listen: datagrams are read and answered")
{
    loop running;
    auto transport = listen_on(running, listen_protocol::udp);

    asio::io_context client_io;
    asio::ip::udp::socket client(client_io, asio::ip::udp::v4());
    const asio::ip::udp::endpoint server(asio::ip::make_address("127.0.0.1"),
                                         transport->local_port());
    client.send_to(asio::buffer(std::string("ping")), server);
    const auto got = read_from(*transport);
    REQUIRE(!got.error);
    REQUIRE_EQ(got.text, std::string("ping"));
    REQUIRE_EQ(transport->peer_count(), std::size_t{1});

    REQUIRE_EQ(write_to(*transport, "pong"), std::size_t{4});
    std::array<char, 16> reply{};
    asio::ip::udp::endpoint from;
    const std::size_t count = client.receive_from(asio::buffer(reply), from);
    REQUIRE_EQ(std::string(reply.data(), count), std::string("pong"));
}

TEST_CASE("listen: describe and public_path name the bound address")
{
    loop running;
    auto transport = listen_on(running, listen_protocol::tcp);
    const std::string expected =
        "tcp-listen://127.0.0.1:" + std::to_string(transport->local_port());
    REQUIRE_EQ(transport->public_path(), expected);
    REQUIRE(transport->describe().find("TCP listening on 127.0.0.1") !=
            std::string::npos);
}

namespace
{

/// True once `done` holds, waited for up to three seconds.
template <class Done> bool wait_for(Done done)
{
    for (int attempt = 0; attempt < 300; ++attempt)
    {
        if (done())
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

} // namespace

TEST_CASE("listen: the allow list parses, prints and admits")
{
    const auto parsed = parse_listen_uri(
        "rfc2217-listen://0.0.0.0:7001?allow=192.168.1.0/24,10.0.0.5,fd00::/8");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->protocol == listen_protocol::rfc2217);
    REQUIRE_EQ(parsed->allow.size(), std::size_t{3});
    REQUIRE_EQ(parsed->allow[0], std::string("192.168.1.0/24"));
    REQUIRE_EQ(make_listen_uri(*parsed),
               std::string("rfc2217-listen://0.0.0.0:7001"
                           "?allow=192.168.1.0/24,10.0.0.5,fd00::/8"));
    const auto round_trip = parse_listen_uri(make_listen_uri(*parsed));
    REQUIRE(round_trip.has_value());
    REQUIRE(*round_trip == *parsed);

    // IPv6 hosts and every scheme take the list.
    REQUIRE(parse_listen_uri("tcp-listen://[::1]:4001?allow=::1").has_value());
    REQUIRE(
        parse_listen_uri("udp-listen://:5000?allow=10.0.0.0/8").has_value());
    // No list, no query.
    REQUIRE(parse_listen_uri("tcp-listen://127.0.0.1:4001")->allow.empty());

    for (const char* bad :
         {"tcp-listen://127.0.0.1:4001?", "tcp-listen://127.0.0.1:4001?allow=",
          "tcp-listen://127.0.0.1:4001?allow=,",
          "tcp-listen://127.0.0.1:4001?x=1",
          "tcp-listen://127.0.0.1:4001?allow=10.0.0.0/33",
          "tcp-listen://127.0.0.1:4001?allow=10.0.0.0/",
          "tcp-listen://127.0.0.1:4001?allow=10.0.0.0/x",
          "tcp-listen://127.0.0.1:4001?allow=fd00::/129",
          "tcp-listen://127.0.0.1:4001?allow=host.example",
          "tcp-listen://127.0.0.1:4001?allow=10.0.0.1&allow=10.0.0.2",
          "tcp-listen://127.0.0.1:4001?allow=10.0.0.1,"})
    {
        REQUIRE(!parse_listen_uri(bad).has_value());
    }

    const std::vector<std::string> list = {"192.168.1.0/24", "10.0.0.5",
                                           "fd00::/8"};
    REQUIRE(allow_list_admits({}, "203.0.113.9"));
    REQUIRE(allow_list_admits(list, "192.168.1.200"));
    REQUIRE(!allow_list_admits(list, "192.168.2.1"));
    REQUIRE(allow_list_admits(list, "10.0.0.5"));
    REQUIRE(!allow_list_admits(list, "10.0.0.6"));
    REQUIRE(allow_list_admits(list, "fd12::1"));
    REQUIRE(!allow_list_admits(list, "fe80::1"));
    // An IPv4 address that arrives as IPv6 is the IPv4 address.
    REQUIRE(allow_list_admits(list, "::ffff:192.168.1.7"));
    REQUIRE(!allow_list_admits(list, "::ffff:192.168.3.7"));
    REQUIRE(!allow_list_admits(list, "not an address"));
    REQUIRE(allow_list_admits({"0.0.0.0/0"}, "198.51.100.1"));
    REQUIRE(allow_list_admits({"10.1.2.3/32"}, "10.1.2.3"));
    REQUIRE(!allow_list_admits({"10.1.2.3/32"}, "10.1.2.4"));
    // A prefix that is not a whole number of bytes.
    REQUIRE(allow_list_admits({"172.16.0.0/12"}, "172.31.255.255"));
    REQUIRE(!allow_list_admits({"172.16.0.0/12"}, "172.32.0.1"));

    listen_address address;
    REQUIRE(!listens_on_loopback(address)); // every interface
    address.host = "127.0.0.1";
    REQUIRE(listens_on_loopback(address));
    address.host = "127.8.9.10";
    REQUIRE(listens_on_loopback(address));
    address.host = "::1";
    REQUIRE(listens_on_loopback(address));
    address.host = "192.168.1.10";
    REQUIRE(!listens_on_loopback(address));
    address.host = "0.0.0.0";
    REQUIRE(!listens_on_loopback(address));
}

TEST_CASE("listen: a TCP client outside the allow list is closed at once")
{
    loop running;
    listen_address address;
    address.host = "127.0.0.1";
    address.allow = {"10.0.0.0/8"};
    error_code ec;
    listen_transport refusing(running.io.get_executor(), address, ec);
    REQUIRE(!ec);

    asio::io_context client_io;
    auto outsider = connect_to(client_io, refusing.local_port());
    boost::system::error_code closed;
    std::array<char, 8> buffer{};
    outsider.read_some(asio::buffer(buffer), closed);
    REQUIRE(closed == asio::error::eof ||
            closed == asio::error::connection_reset);
    REQUIRE(wait_for([&] { return refusing.refused_count() == 1; }));
    REQUIRE_EQ(refusing.peer_count(), std::size_t{0});

    // The listener goes on: a client on the list is served.
    address.allow = {"127.0.0.0/8"};
    listen_transport admitting(running.io.get_executor(), address, ec);
    REQUIRE(!ec);
    auto member = connect_to(client_io, admitting.local_port());
    REQUIRE(wait_for_peers(admitting, 1));
    asio::write(member, asio::buffer(std::string("in")));
    REQUIRE_EQ(read_from(admitting).text, std::string("in"));
    REQUIRE_EQ(admitting.refused_count(), std::size_t{0});

    // A list that is not valid cannot be opened.
    address.allow = {"nonsense"};
    listen_transport broken(running.io.get_executor(), address, ec);
    REQUIRE(ec == errc::invalid_uri);
}

TEST_CASE("listen: an RFC 2217 server applies the allow list as well")
{
    loop running;
    listen_address address;
    address.protocol = listen_protocol::rfc2217;
    address.host = "127.0.0.1";
    address.allow = {"::1"};
    error_code ec;
    listen_transport server(running.io.get_executor(), address, ec);
    REQUIRE(!ec);
    asio::io_context client_io;
    auto outsider = connect_to(client_io, server.local_port());
    boost::system::error_code closed;
    std::array<char, 8> buffer{};
    outsider.read_some(asio::buffer(buffer), closed);
    // Closed before the greeting: nothing was sent.
    REQUIRE(closed);
    REQUIRE(wait_for([&] { return server.refused_count() == 1; }));
}

TEST_CASE("listen: datagrams from outside the allow list are ignored")
{
    loop running;
    listen_address address;
    address.protocol = listen_protocol::udp;
    address.host = "127.0.0.1";
    address.allow = {"10.0.0.0/8"};
    error_code ec;
    listen_transport transport(running.io.get_executor(), address, ec);
    REQUIRE(!ec);

    asio::io_context client_io;
    asio::ip::udp::socket client(client_io, asio::ip::udp::v4());
    client.send_to(
        asio::buffer(std::string("no")),
        {asio::ip::make_address("127.0.0.1"), transport.local_port()});
    REQUIRE(wait_for([&] { return transport.refused_count() == 1; }));
    REQUIRE_EQ(transport.peer_count(), std::size_t{0});
    REQUIRE_EQ(read_from(transport, 200ms).error, asio::error::timed_out);
}

TEST_CASE("listen: the client count is available through the transport interface")
{
    loop running;
    auto transport = listen_on(running, listen_protocol::tcp);
    const vss::transport& as_transport = *transport;

    // A listener always has a count, even when it is zero; that is how "no
    // remote connected" differs from "not a listener".
    REQUIRE(as_transport.client_count().has_value());
    REQUIRE_EQ(*as_transport.client_count(), std::size_t{0});

    asio::io_context client_io;
    auto first = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 1));
    REQUIRE_EQ(*as_transport.client_count(), std::size_t{1});

    auto second = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 2));
    REQUIRE_EQ(*as_transport.client_count(), std::size_t{2});

    first.close();
    REQUIRE(wait_for_peers(*transport, 1));
    REQUIRE_EQ(*as_transport.client_count(), std::size_t{1});
}

namespace
{

/// What a transport said through its notice sink.
struct notice_log
{
    std::mutex mutex;
    std::vector<std::string> lines;

    void add(std::string text)
    {
        std::lock_guard lock(mutex);
        lines.push_back(std::move(text));
    }

    /// How many notices contain `part`.
    std::size_t count(const std::string& part)
    {
        std::lock_guard lock(mutex);
        return static_cast<std::size_t>(std::count_if(
            lines.begin(), lines.end(), [&](const std::string& line)
            { return line.find(part) != std::string::npos; }));
    }

    /// True once exactly `times` notices contain `part`. A transport counts a
    /// client before it says so, so a test waits for the notice.
    bool wait_for_count(const std::string& part, std::size_t times = 1)
    {
        return wait_for([&] { return count(part) == times; });
    }
};

std::shared_ptr<notice_log> collect_notices(listen_transport& transport)
{
    auto log = std::make_shared<notice_log>();
    transport.set_notice_sink([log](std::string text)
                              { log->add(std::move(text)); });
    return log;
}

/// A client with a small receive buffer, so that one that does not read
/// stops the server's writes after a little data.
asio::ip::tcp::socket connect_small(asio::io_context& io, std::uint16_t port)
{
    asio::ip::tcp::socket socket(io, asio::ip::tcp::v4());
    socket.set_option(asio::socket_base::receive_buffer_size(4096));
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    return socket;
}

/// `address:port` of the client, as the transport names it.
std::string name_of(const asio::ip::tcp::socket& client)
{
    return "127.0.0.1:" + std::to_string(client.local_endpoint().port());
}

/// `size` bytes of a pattern that tells where in the stream they belong.
std::string pattern(std::size_t offset, std::size_t size)
{
    std::string text(size, '\0');
    for (std::size_t index = 0; index < size; ++index)
        text[index] = static_cast<char>('a' + (offset + index) % 26);
    return text;
}

/// Appends what `socket` has now to `into`, without waiting.
void drain(asio::ip::tcp::socket& socket, std::string& into)
{
    std::array<char, 65536> buffer{};
    boost::system::error_code ec;
    while (socket.available(ec) > 0 && !ec)
    {
        const std::size_t count = socket.read_some(asio::buffer(buffer), ec);
        into.append(buffer.data(), count);
    }
}

/// True once the server has closed `socket`, waited for up to three seconds.
bool closed_by_server(asio::ip::tcp::socket& socket)
{
    socket.non_blocking(true);
    return wait_for(
        [&]
        {
            std::array<char, 8> buffer{};
            boost::system::error_code ec;
            socket.read_some(asio::buffer(buffer), ec);
            return ec == asio::error::eof ||
                   ec == asio::error::connection_reset;
        });
}

/// Reads until `into` holds `total` bytes; false if that takes over three
/// seconds.
bool receive_until(asio::ip::tcp::socket& socket, std::string& into,
                   std::size_t total)
{
    return wait_for(
        [&]
        {
            drain(socket, into);
            return into.size() >= total;
        });
}

using write_outcome = std::pair<boost::system::error_code, std::size_t>;

/// A write through the transport that is not waited for.
std::shared_future<write_outcome> start_write(listen_transport& transport,
                                              const std::string& text)
{
    auto promise = std::make_shared<std::promise<write_outcome>>();
    transport.async_write_some(
        asio::buffer(text),
        [promise](boost::system::error_code ec, std::size_t count)
        { promise->set_value({ec, count}); });
    return promise->get_future().share();
}

/// Writes parts until one is not finished after half a second. The port holds
/// a write while every client is behind, so that is the held one. Empty if
/// nothing was held.
std::shared_future<write_outcome> fill_until_held(listen_transport& transport,
                                                  std::size_t& sent)
{
    constexpr std::size_t part = 16 * 1024;
    constexpr std::size_t most = std::size_t{256} * 1024 * 1024;
    while (sent < most)
    {
        auto outcome = start_write(transport, pattern(sent, part));
        sent += part;
        if (outcome.wait_for(500ms) != std::future_status::ready)
            return outcome;
    }
    return {};
}

/// A client that reads on its own thread, `chunk` bytes and then a pause, so
/// that it is steady and slower than a source that writes at once.
class steady_reader
{
public:
    steady_reader(asio::ip::tcp::socket& socket, std::size_t total,
                  std::size_t chunk)
        : socket_(socket)
        , thread_(
              [this, total, chunk]
              {
                  std::string buffer(chunk, '\0');
                  while (got_.size() < total)
                  {
                      boost::system::error_code ec;
                      const std::size_t count =
                          socket_.read_some(asio::buffer(buffer), ec);
                      if (ec)
                      {
                          failed_ = true;
                          return;
                      }
                      got_.append(buffer.data(), count);
                      std::this_thread::sleep_for(2ms);
                  }
              })
    {
    }

    ~steady_reader()
    {
        // Wakes a read that is waiting for data that will not come.
        boost::system::error_code ignored;
        socket_.shutdown(asio::socket_base::shutdown_both, ignored);
        if (thread_.joinable())
            thread_.join();
    }

    steady_reader(const steady_reader&) = delete;
    steady_reader& operator=(const steady_reader&) = delete;

    /// What it read, once it has read all it was to.
    const std::string& finish()
    {
        if (thread_.joinable())
            thread_.join();
        return got_;
    }

    bool failed() const { return failed_; }

private:
    asio::ip::tcp::socket& socket_;
    std::string got_;
    std::atomic<bool> failed_{false};
    std::thread thread_;
};

} // namespace

TEST_CASE("listen: a client that stops reading is dropped and the other keeps "
          "receiving")
{
    loop running;
    listen_limits limits;
    limits.max_client_queue_bytes = std::size_t{256} * 1024;
    auto transport = listen_on(running, listen_protocol::tcp, limits);
    const auto notices = collect_notices(*transport);

    asio::io_context client_io;
    auto stuck = connect_small(client_io, transport->local_port());
    auto good = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 2));
    const std::string stuck_name = name_of(stuck);

    // The stuck client never reads. Every write must still complete, and the
    // other client must receive every byte, until the stuck one is dropped.
    constexpr std::size_t part = 16 * 1024;
    constexpr std::size_t most = std::size_t{128} * 1024 * 1024;
    std::string got;
    std::size_t sent = 0;
    while (transport->peer_count() == 2 && sent < most)
    {
        REQUIRE_EQ(write_to(*transport, pattern(sent, part)), part);
        sent += part;
        drain(good, got);
    }
    REQUIRE(wait_for_peers(*transport, 1));
    REQUIRE(sent < most);
    REQUIRE(receive_until(good, got, sent));
    REQUIRE(got == pattern(0, sent));

    REQUIRE(notices->wait_for_count("Disconnected client " + stuck_name));
    REQUIRE(notices->wait_for_count("bytes behind"));

    // The port goes on serving the one that is left.
    REQUIRE_EQ(write_to(*transport, "after"), std::size_t{5});
    REQUIRE(receive_until(good, got, sent + 5));
    REQUIRE_EQ(got.substr(sent), std::string("after"));
}

TEST_CASE("listen: a client that reads steadily, slower than the source, "
          "gets everything and is not dropped")
{
    loop running;
    listen_limits limits;
    limits.max_client_queue_bytes = std::size_t{64} * 1024;
    auto transport = listen_on(running, listen_protocol::tcp, limits);
    const auto notices = collect_notices(*transport);

    asio::io_context client_io;
    auto slow = connect_small(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 1));

    // The source writes at once and each write waits for the one before, as
    // the endpoint does. The writes finish at the pace of the reader.
    constexpr std::size_t total = std::size_t{4} * 1024 * 1024;
    constexpr std::size_t part = 16 * 1024;
    steady_reader reader(slow, total, 256 * 1024);
    for (std::size_t sent = 0; sent < total; sent += part)
        REQUIRE_EQ(write_to(*transport, pattern(sent, part)), part);
    const std::string& got = reader.finish();
    REQUIRE(!reader.failed());
    REQUIRE(got == pattern(0, total));
    REQUIRE_EQ(transport->peer_count(), std::size_t{1});
    REQUIRE_EQ(notices->count("Disconnected client"), std::size_t{0});
}

TEST_CASE("listen: with a stuck client and a slow one, the stuck one is dropped "
          "and the slow one gets everything")
{
    loop running;
    listen_limits limits;
    limits.max_client_queue_bytes = std::size_t{64} * 1024;
    auto transport = listen_on(running, listen_protocol::tcp, limits);
    const auto notices = collect_notices(*transport);

    asio::io_context client_io;
    auto stuck = connect_small(client_io, transport->local_port());
    auto slow = connect_small(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 2));
    const std::string stuck_name = name_of(stuck);

    constexpr std::size_t total = std::size_t{4} * 1024 * 1024;
    constexpr std::size_t part = 16 * 1024;
    steady_reader reader(slow, total, 256 * 1024);
    for (std::size_t sent = 0; sent < total; sent += part)
        REQUIRE_EQ(write_to(*transport, pattern(sent, part)), part);
    const std::string& got = reader.finish();
    REQUIRE(!reader.failed());
    REQUIRE(got == pattern(0, total));
    REQUIRE(wait_for_peers(*transport, 1));
    REQUIRE(notices->wait_for_count("Disconnected client " + stuck_name));
    REQUIRE_EQ(notices->count("Disconnected client"), std::size_t{1});
}

TEST_CASE("listen: a write waits while every client is behind and finishes "
          "when one catches up")
{
    loop running;
    listen_limits limits;
    limits.max_client_queue_bytes = std::size_t{64} * 1024;
    auto transport = listen_on(running, listen_protocol::tcp, limits);
    const auto notices = collect_notices(*transport);

    asio::io_context client_io;
    auto first = connect_small(client_io, transport->local_port());
    auto second = connect_small(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 2));
    const std::string second_name = name_of(second);

    std::size_t sent = 0;
    auto held = fill_until_held(*transport, sent);
    REQUIRE(held.valid());
    REQUIRE_EQ(transport->peer_count(), std::size_t{2});

    // The first starts to read: the write is done, and the second, which is
    // still behind, is the one that held the others back.
    std::string got;
    REQUIRE(wait_for(
        [&]
        {
            drain(first, got);
            return held.wait_for(0ms) == std::future_status::ready;
        }));
    REQUIRE(!held.get().first);
    REQUIRE(wait_for_peers(*transport, 1));
    REQUIRE(notices->wait_for_count("Disconnected client " + second_name));
    REQUIRE_EQ(notices->count("Disconnected client"), std::size_t{1});
}

TEST_CASE("listen: a write that waits is finished when the last client leaves")
{
    loop running;
    listen_limits limits;
    limits.max_client_queue_bytes = std::size_t{64} * 1024;
    auto transport = listen_on(running, listen_protocol::tcp, limits);

    asio::io_context client_io;
    auto only = connect_small(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 1));

    std::size_t sent = 0;
    auto held = fill_until_held(*transport, sent);
    REQUIRE(held.valid());

    only.close();
    REQUIRE(held.wait_for(3000ms) == std::future_status::ready);
    REQUIRE(!held.get().first);
    REQUIRE(wait_for_peers(*transport, 0));
}

TEST_CASE("listen: a write that waits is aborted when the port closes")
{
    loop running;
    listen_limits limits;
    limits.max_client_queue_bytes = std::size_t{64} * 1024;
    auto transport = listen_on(running, listen_protocol::tcp, limits);

    asio::io_context client_io;
    auto only = connect_small(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 1));

    std::size_t sent = 0;
    auto held = fill_until_held(*transport, sent);
    REQUIRE(held.valid());

    transport->close();
    REQUIRE(held.wait_for(3000ms) == std::future_status::ready);
    REQUIRE(held.get().first == asio::error::operation_aborted);
}

TEST_CASE("listen: a client that takes no data for the deadline is dropped")
{
    loop running;
    listen_limits limits;
    // A queue bound far above what is written, so only the deadline acts.
    limits.max_client_queue_bytes = std::size_t{1} << 30;
    limits.write_deadline = 300ms;
    auto transport = listen_on(running, listen_protocol::tcp, limits);
    const auto notices = collect_notices(*transport);

    asio::io_context client_io;
    auto stuck = connect_small(client_io, transport->local_port());
    auto good = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 2));
    const std::string stuck_name = name_of(stuck);

    // About 8 MB a second: the socket buffers fill in well under a second and
    // the queue stays far below its bound while the deadline runs out.
    constexpr std::size_t part = 16 * 1024;
    const auto give_up = std::chrono::steady_clock::now() + 20s;
    std::string got;
    std::size_t sent = 0;
    while (transport->peer_count() == 2 &&
           std::chrono::steady_clock::now() < give_up)
    {
        REQUIRE_EQ(write_to(*transport, pattern(sent, part)), part);
        sent += part;
        drain(good, got);
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(wait_for_peers(*transport, 1));
    REQUIRE(receive_until(good, got, sent));
    REQUIRE(got == pattern(0, sent));
    REQUIRE(notices->wait_for_count("Disconnected client " + stuck_name));
    REQUIRE(notices->wait_for_count("took no data for 300 ms"));
}

TEST_CASE("listen: a lone client that takes no data is dropped by the deadline "
          "and the write that waited is finished")
{
    loop running;
    listen_limits limits;
    limits.max_client_queue_bytes = std::size_t{64} * 1024;
    limits.write_deadline = 300ms;
    auto transport = listen_on(running, listen_protocol::tcp, limits);
    const auto notices = collect_notices(*transport);

    asio::io_context client_io;
    auto stuck = connect_small(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 1));

    // Each write finishes: while the client is behind it waits, and the
    // deadline then drops the client, which leaves nobody to wait for.
    constexpr std::size_t part = 16 * 1024;
    std::size_t sent = 0;
    while (transport->peer_count() == 1 && sent < std::size_t{64} * 1024 * 1024)
    {
        REQUIRE_EQ(write_to(*transport, pattern(sent, part)), part);
        sent += part;
    }
    REQUIRE(wait_for_peers(*transport, 0));
    REQUIRE(notices->wait_for_count("took no data for 300 ms"));
}

TEST_CASE("listen: a client with nothing to take is not dropped by the "
          "deadline")
{
    loop running;
    listen_limits limits;
    limits.write_deadline = 100ms;
    auto transport = listen_on(running, listen_protocol::tcp, limits);

    asio::io_context client_io;
    auto client = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 1));

    REQUIRE_EQ(write_to(*transport, "a"), std::size_t{1});
    REQUIRE_EQ(receive(client, 1), std::string("a"));

    // Four deadlines go by with nothing queued for it.
    std::this_thread::sleep_for(400ms);
    REQUIRE_EQ(transport->peer_count(), std::size_t{1});
    REQUIRE_EQ(write_to(*transport, "b"), std::size_t{1});
    REQUIRE_EQ(receive(client, 1), std::string("b"));
    REQUIRE_EQ(transport->peer_count(), std::size_t{1});
}

TEST_CASE("listen: a client over the limit is closed at once with a notice")
{
    loop running;
    listen_limits limits;
    limits.max_clients = 2;
    auto transport = listen_on(running, listen_protocol::tcp, limits);
    const auto notices = collect_notices(*transport);

    asio::io_context client_io;
    auto first = connect_to(client_io, transport->local_port());
    auto second = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 2));

    auto over = connect_to(client_io, transport->local_port());
    REQUIRE(closed_by_server(over));
    REQUIRE(wait_for([&] { return transport->refused_count() == 1; }));
    REQUIRE_EQ(transport->peer_count(), std::size_t{2});
    REQUIRE(notices->wait_for_count("Refused client 127.0.0.1:"));
    REQUIRE(notices->wait_for_count("already has 2 clients"));

    // A flood of connections is counted but does not fill the log.
    auto again = connect_to(client_io, transport->local_port());
    REQUIRE(closed_by_server(again));
    REQUIRE(wait_for([&] { return transport->refused_count() == 2; }));
    REQUIRE_EQ(notices->count("Refused client"), std::size_t{1});

    // The clients that were there are served, and a place that is freed is
    // taken by the next.
    asio::write(first, asio::buffer(std::string("one")));
    REQUIRE_EQ(read_from(*transport).text, std::string("one"));
    second.close();
    REQUIRE(wait_for_peers(*transport, 1));
    auto third = connect_to(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 2));
}

TEST_CASE("listen: an RFC 2217 client that sends requests and never reads is "
          "dropped")
{
    loop running;
    listen_limits limits;
    limits.max_client_queue_bytes = std::size_t{64} * 1024;
    auto transport = listen_on(running, listen_protocol::rfc2217, limits);
    const auto notices = collect_notices(*transport);

    asio::io_context client_io;
    auto client = connect_small(client_io, transport->local_port());
    REQUIRE(wait_for_peers(*transport, 1));

    // IAC SB COM-PORT SIGNATURE IAC SE: six bytes that ask for a 27 byte
    // reply.
    const std::string request = {static_cast<char>(0xFF),
                                 static_cast<char>(0xFA),
                                 0x2C,
                                 0x00,
                                 static_cast<char>(0xFF),
                                 static_cast<char>(0xF0)};
    std::string block;
    for (int index = 0; index < 2000; ++index)
        block += request;

    // Without a bound the replies pile up in the server for as long as the
    // client sends: 32 MB of requests would be 144 MB of replies.
    constexpr std::size_t most = std::size_t{32} * 1024 * 1024;
    std::size_t sent = 0;
    while (transport->peer_count() == 1 && sent < most)
    {
        boost::system::error_code ec;
        asio::write(client, asio::buffer(block), ec);
        if (ec)
            break;
        sent += block.size();
    }
    REQUIRE(wait_for_peers(*transport, 0));
    REQUIRE(sent < most);
    REQUIRE(notices->wait_for_count("Disconnected client 127.0.0.1:"));
    REQUIRE(notices->wait_for_count("bytes behind"));
}

TEST_CASE("listen: a failed accept waits longer each time, up to a limit")
{
    const listen_limits limits;
    REQUIRE(accept_retry_delay(0, limits) == 100ms);
    REQUIRE(accept_retry_delay(1, limits) == 200ms);
    REQUIRE(accept_retry_delay(2, limits) == 400ms);
    REQUIRE(accept_retry_delay(5, limits) == 3200ms);
    REQUIRE(accept_retry_delay(6, limits) == 5000ms);
    REQUIRE(accept_retry_delay(1000000, limits) == 5000ms);

    // A first wait of nothing still waits, and still grows.
    listen_limits none;
    none.accept_retry_first = 0ms;
    none.accept_retry_last = 1000ms;
    REQUIRE(accept_retry_delay(0, none) == 1ms);
    REQUIRE(accept_retry_delay(3, none) == 8ms);
}

TEST_CASE("listen: an accept that keeps failing waits longer, says so once and "
          "accepts again after a success")
{
    struct attempts
    {
        std::mutex mutex;
        std::vector<std::chrono::steady_clock::time_point> times;
    };
    auto seen = std::make_shared<attempts>();

    loop running;
    listen_limits limits;
    limits.accept_retry_first = 40ms;
    limits.accept_retry_last = 160ms;
    // The first three accepts fail, the fourth works, the fifth and sixth
    // fail and the rest work. A failed accept loses the connection.
    limits.accept_fault = [seen]() -> error_code
    {
        std::lock_guard lock(seen->mutex);
        seen->times.push_back(std::chrono::steady_clock::now());
        const std::size_t number = seen->times.size();
        if (number <= 3 || number == 5 || number == 6)
            return asio::error::no_descriptors;
        return {};
    };
    auto transport = listen_on(running, listen_protocol::tcp, limits);
    const auto notices = collect_notices(*transport);

    const auto attempts_made = [&]
    {
        std::lock_guard lock(seen->mutex);
        return seen->times.size();
    };

    asio::io_context client_io;
    std::vector<asio::ip::tcp::socket> clients;
    for (int index = 0; index < 4; ++index)
        clients.push_back(connect_to(client_io, transport->local_port()));
    REQUIRE(wait_for_peers(*transport, 1));
    REQUIRE(notices->wait_for_count("Accepting connections failed"));

    for (int index = 0; index < 3; ++index)
        clients.push_back(connect_to(client_io, transport->local_port()));
    REQUIRE(wait_for([&] { return attempts_made() == 7; }));
    REQUIRE(wait_for_peers(*transport, 2));

    // The waits grew, 40 ms, 80 ms and 160 ms, and started again after the
    // accept that worked. A timer does not fire early; the slack is for the
    // clock.
    std::vector<std::chrono::steady_clock::time_point> times;
    {
        std::lock_guard lock(seen->mutex);
        times = seen->times;
    }
    const auto gap = [&](std::size_t from)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            times[from + 1] - times[from]);
    };
    REQUIRE(gap(0) >= 30ms);
    REQUIRE(gap(1) >= 70ms);
    REQUIRE(gap(2) >= 150ms);
    REQUIRE(gap(4) >= 30ms);

    // Two runs of failures, one notice: the second came within ten seconds.
    REQUIRE_EQ(notices->count("Accepting connections failed"), std::size_t{1});
}
