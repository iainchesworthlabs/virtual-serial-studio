//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent's server, its blocking client and the transport under them, over
// the real pipe or socket: the handshake and its failures, the limits (busy,
// a silent client, a flood, a client that never reads), events in batches, the
// revision, stopping under load and the transport on its own.  Every agent
// here has its own instance name, so none of them meets the user's agent or
// another test.  Nothing waits for a fixed time: each wait has a condition and
// a deadline.
//

#include <vss/agent/client.hpp>
#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/agent/proto/message.hpp>
#include <vss/agent/server.hpp>
#include <vss/core/bytes.hpp>
#include <vss/studio/studio.hpp>
#include <vss/testkit/harness.hpp>

#include "test_framework.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace agent = vss::agent;
using namespace std::chrono_literals;

namespace
{

using steady = std::chrono::steady_clock;

/// A thread that is joined when it goes out of scope.  `std::jthread` is not
/// in the libc++ of every supported Apple toolchain.
class joining_thread
{
public:
    template <class Function>
    explicit joining_thread(Function&& function)
        : thread_(std::forward<Function>(function))
    {
    }

    joining_thread(joining_thread&&) noexcept = default;
    joining_thread& operator=(joining_thread&&) = delete;

    ~joining_thread()
    {
        if (thread_.joinable())
            thread_.join();
    }

private:
    std::thread thread_;
};

/// Long enough for a sanitizer build on a loaded machine.  A test that passes
/// takes milliseconds.
constexpr std::chrono::milliseconds long_wait{20000};

vss::error_code code(vss::errc value)
{
    return vss::make_error_code(value);
}

vss::error_code code(agent::protocol_errc value)
{
    return agent::make_error_code(value);
}

// -- instance names ----------------------------------------------------------

std::string unique_instance_name()
{
    static const unsigned random = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    std::ostringstream out;
    out << "ts-" << std::hex << random << '-' << std::dec
        << counter.fetch_add(1);
    return out.str();
}

/// A name no other agent has.  Where the transport leaves a lock file beside
/// the socket, it is removed with the name.
class scoped_instance
{
public:
    scoped_instance() : name_(unique_instance_name()) {}

    ~scoped_instance()
    {
        try
        {
            const auto address = agent::local_address(name_);
            if (!address)
                return;
            std::filesystem::path lock(*address);
            if (lock.extension() != ".sock")
                return;
            lock.replace_extension(".lock");
            std::error_code ignored;
            std::filesystem::remove(lock, ignored);
        }
        catch (...)
        {
            // best effort: a lock file left behind fails no test, and a
            // destructor has nobody to tell.
        }
    }

    scoped_instance(const scoped_instance&) = delete;
    scoped_instance& operator=(const scoped_instance&) = delete;

    const std::string& name() const noexcept { return name_; }

private:
    std::string name_;
};

agent::server_options named(const scoped_instance& instance,
                            agent::server_options options)
{
    options.instance = instance.name();
    return options;
}

/// A studio and the server that serves it, started.  The studio is declared
/// first, so the server goes first.
struct fixture
{
    explicit fixture(agent::server_options options = {})
        : server(app, named(instance, std::move(options)))
    {
        const auto started = server.start();
        REQUIRE(started);
    }

    std::unique_ptr<agent::client> connect(const char* label = "test")
    {
        agent::client_options options;
        options.instance = instance.name();
        options.name = label;
        auto connected = agent::client::connect(options);
        REQUIRE(connected);
        return std::move(*connected);
    }

    scoped_instance instance;
    vss::studio app;
    agent::server server;
};

// -- waiting -----------------------------------------------------------------

template <class Predicate>
bool run_until(boost::asio::io_context& io, Predicate done,
               std::chrono::milliseconds limit)
{
    const auto deadline = steady::now() + limit;
    while (!done())
    {
        const auto now = steady::now();
        if (now >= deadline)
            return false;
        io.restart();
        io.run_one_for(deadline - now);
    }
    return true;
}

/// Reads batches until `visit` says it has what it wants, or the time is up.
template <class Visitor>
bool read_batches(agent::client& source, std::chrono::milliseconds limit,
                  Visitor visit)
{
    const auto deadline = steady::now() + limit;
    for (;;)
    {
        const auto now = steady::now();
        if (now >= deadline)
            return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const auto batch = source.next_events(std::min(left, 200ms));
        if (!batch)
        {
            if (batch.error() == code(vss::errc::timeout))
                continue;
            return false;
        }
        if (visit(*batch))
            return true;
    }
}

bool stopped_serving(fixture& env)
{
    return env.server.counters().clients == 0;
}

const agent::port_summary* find_port(const agent::list_reply& listed,
                                     vss::endpoint_id id)
{
    const auto found =
        std::ranges::find(listed.ports, id, &agent::port_summary::id);
    return found == listed.ports.end() ? nullptr : &*found;
}

/// True when the port has counted exactly `expected` bytes received.
bool port_rx_is(agent::client& source, vss::endpoint_id id,
                std::uint64_t expected)
{
    const auto listed = source.list();
    if (!listed)
        return false;
    const auto* port = find_port(*listed, id);
    return port != nullptr && port->bytes_rx == expected;
}

bool is_rx_data_on(const agent::event_record& event, vss::endpoint_id id)
{
    return event.kind == vss::event_kind::data && event.endpoint == id &&
           event.direction == vss::data_direction::rx;
}

// -- raw bytes ---------------------------------------------------------------

agent::request make_request(std::uint64_t id, agent::request_body body)
{
    agent::request value;
    value.id = id;
    value.body = std::move(body);
    return value;
}

vss::byte_buffer wire_of(const agent::frame& value)
{
    auto bytes = agent::encode_frame(value);
    REQUIRE(bytes);
    return std::move(*bytes);
}

vss::byte_buffer wire_of(const agent::request& value)
{
    return wire_of(agent::encode(value));
}

/// A frame with this header and no blob, as the wire carries it.
vss::byte_buffer wire_of_header(std::string header)
{
    agent::frame value;
    value.header = std::move(header);
    return wire_of(value);
}

void append(vss::byte_buffer& to, const vss::byte_buffer& from)
{
    to.insert(to.end(), from.begin(), from.end());
}

/// What a connection and its operations in flight share.  The handlers hold it
/// through a `shared_ptr`, so an operation can outlive the call that started
/// it, and `stream` is cleared before the stream is destroyed.
struct channel
{
    agent::local_stream* stream = nullptr;
    vss::byte_buffer inbound = vss::byte_buffer(std::size_t{64} * 1024);
    agent::frame_reader reader;
    bool reading = false;
    bool read_failed = false;
    vss::byte_buffer outbound;
    std::size_t written = 0;
    bool writing = false;
    bool write_failed = false;
};

void continue_write(const std::shared_ptr<channel>& state)
{
    if (state->stream == nullptr || state->written >= state->outbound.size())
    {
        state->writing = false;
        return;
    }
    state->stream->async_write_some(
        boost::asio::buffer(state->outbound.data() + state->written,
                            state->outbound.size() - state->written),
        [state](boost::system::error_code failed, std::size_t size)
        {
            if (failed)
            {
                state->write_failed = true;
                state->writing = false;
                return;
            }
            state->written += size;
            continue_write(state);
        });
}

void start_read(const std::shared_ptr<channel>& state)
{
    if (state->reading || state->read_failed || state->stream == nullptr)
        return;
    state->reading = true;
    state->stream->async_read_some(
        boost::asio::buffer(state->inbound),
        [state](boost::system::error_code failed, std::size_t size)
        {
            state->reading = false;
            if (failed)
            {
                state->read_failed = true;
                return;
            }
            state->reader.feed(vss::bytes_view(state->inbound.data(), size));
        });
}

/// A connection to the agent that speaks bytes, not messages, so a test can
/// send what a client library never would.  Its operations run only while one
/// of its own calls drives the io_context.
class raw_client
{
public:
    explicit raw_client(const std::string& instance)
        : state_(std::make_shared<channel>())
    {
        auto connected = agent::connect_local(io_.get_executor(), instance);
        REQUIRE(connected);
        stream_ = std::move(*connected);
        state_->stream = stream_.get();
    }

    ~raw_client() { state_->stream = nullptr; }

    raw_client(const raw_client&) = delete;
    raw_client& operator=(const raw_client&) = delete;

    /// Writes all of `bytes`; false when it did not finish in time.
    bool send(vss::byte_buffer bytes,
              std::chrono::milliseconds limit = long_wait)
    {
        send_nowait(std::move(bytes));
        const bool finished =
            run_until(io_, [this] { return !state_->writing; }, limit);
        return finished && !state_->write_failed;
    }

    /// Starts the write and returns.  It goes on while later calls drive the
    /// io_context; one write at a time.
    void send_nowait(vss::byte_buffer bytes)
    {
        REQUIRE(!state_->writing);
        state_->outbound = std::move(bytes);
        state_->written = 0;
        state_->write_failed = false;
        state_->writing = true;
        continue_write(state_);
    }

    /// The next whole frame, or nothing when the time is up, the connection
    /// ended or the bytes were not a frame.
    std::optional<agent::frame>
    read_frame(std::chrono::milliseconds limit = long_wait)
    {
        const auto deadline = steady::now() + limit;
        for (;;)
        {
            auto next = state_->reader.next();
            if (!next)
                return std::nullopt;
            if (next->has_value())
                return std::move(*next);
            if (state_->read_failed)
                return std::nullopt;
            start_read(state_);
            const auto now = steady::now();
            if (now >= deadline)
                return std::nullopt;
            io_.restart();
            io_.run_one_for(deadline - now);
        }
    }

    /// True when a read ended with an error: the agent closed the connection.
    bool wait_closed(std::chrono::milliseconds limit = long_wait)
    {
        return run_until(
            io_,
            [this]
            {
                start_read(state_);
                return state_->read_failed;
            },
            limit);
    }

private:
    // The io_context goes first in the list, so it is destroyed last: the
    // stream and the handlers it still owes are gone before it.
    boost::asio::io_context io_;
    std::unique_ptr<agent::local_stream> stream_;
    std::shared_ptr<channel> state_;
};

agent::reply read_reply(raw_client& raw)
{
    const auto received = raw.read_frame();
    REQUIRE(received.has_value());
    const auto message = agent::decode_server_message(*received);
    REQUIRE(message);
    const auto* answer = std::get_if<agent::reply>(&*message);
    REQUIRE(answer != nullptr);
    return *answer;
}

vss::error_code error_of(const agent::reply& answer)
{
    const auto* refused = std::get_if<agent::error_reply>(&answer.body);
    REQUIRE(refused != nullptr);
    return agent::from_wire(refused->error);
}

/// Says hello as a good client would, and reads the welcome.
void say_hello(raw_client& raw)
{
    const agent::hello_request greeting{agent::protocol_version, "raw"};
    REQUIRE(raw.send(wire_of(make_request(1, greeting))));
    const agent::reply welcome = read_reply(raw);
    REQUIRE_EQ(welcome.id, std::uint64_t{1});
    REQUIRE(std::holds_alternative<agent::hello_reply>(welcome.body));
}

/// The agent refused the connection: the error reply is the expected one, the
/// connection is closed and the server counted a protocol error.  Returns the
/// id of the reply.
std::uint64_t expect_refusal(fixture& env, raw_client& raw,
                             agent::protocol_errc expected,
                             std::uint64_t errors_before)
{
    const agent::reply answer = read_reply(raw);
    REQUIRE_EQ(error_of(answer), code(expected));
    REQUIRE(raw.wait_closed());
    REQUIRE(env.server.counters().protocol_errors > errors_before);
    return answer.id;
}

} // namespace

// -- the basic path ----------------------------------------------------------

TEST_CASE("agent server: two clients share ports and see each other's traffic")
{
    fixture env;
    auto first = env.connect("first");

    const agent::hello_reply& hello = first->agent();
    REQUIRE_EQ(hello.protocol, agent::protocol_version);
    REQUIRE(!hello.agent_version.empty());
    REQUIRE(std::ranges::find(hello.port_kinds, std::string("loopback")) !=
            hello.port_kinds.end());

    const auto a = first->create_port("a");
    const auto b = first->create_port("b");
    REQUIRE(a);
    REQUIRE(b);
    const auto linked = first->connect_ports(*a, *b, true);
    REQUIRE(linked);

    auto second = env.connect("second");
    const auto listed = second->list();
    REQUIRE(listed);
    REQUIRE_EQ(listed->ports.size(), std::size_t{2});
    const auto* port_a = find_port(*listed, *a);
    const auto* port_b = find_port(*listed, *b);
    REQUIRE(port_a != nullptr);
    REQUIRE(port_b != nullptr);
    REQUIRE_EQ(port_a->name, std::string("a"));
    REQUIRE_EQ(port_b->name, std::string("b"));
    REQUIRE(port_a->open);
    REQUIRE(port_b->open);
    REQUIRE_EQ(listed->links.size(), std::size_t{1});
    const agent::link_summary& link = listed->links.front();
    REQUIRE_EQ(link.id, *linked);
    REQUIRE(link.mode == vss::link_mode::one_way);
    REQUIRE_EQ(link.sources, std::vector<vss::endpoint_id>{*a});
    REQUIRE_EQ(link.targets, std::vector<vss::endpoint_id>{*b});

    REQUIRE(second->subscribe(true, 20));
    REQUIRE(first->write(*a, vss::to_bytes("hello")));

    std::string seen;
    const bool arrived = read_batches(
        *second, long_wait,
        [&](const agent::event_batch& received)
        {
            for (const agent::event_record& event : received.events)
            {
                if (is_rx_data_on(event, *b))
                    seen += vss::to_string(vss::bytes_view(event.data));
            }
            return seen.find("hello") != std::string::npos;
        });
    REQUIRE(arrived);
}

TEST_CASE("agent server: data events carry bytes only when asked")
{
    fixture env;
    auto maker = env.connect("maker");
    const auto a = maker->create_port("a");
    const auto b = maker->create_port("b");
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(maker->connect_ports(*a, *b, true));

    auto counting = env.connect("counting");
    auto tapping = env.connect("tapping");
    REQUIRE(counting->subscribe(false, 20));
    REQUIRE(tapping->subscribe(true, 20));
    REQUIRE(maker->write(*a, vss::to_bytes("hello")));

    std::optional<agent::event_record> counted;
    const bool counted_seen = read_batches(
        *counting, long_wait,
        [&](const agent::event_batch& received)
        {
            for (const agent::event_record& event : received.events)
            {
                if (is_rx_data_on(event, *b))
                    counted = event;
            }
            return counted.has_value();
        });
    REQUIRE(counted_seen);
    REQUIRE(counted.has_value());
    REQUIRE_EQ(counted->size, std::uint64_t{5});
    REQUIRE(counted->data.empty());

    std::optional<agent::event_record> tapped;
    const bool tapped_seen = read_batches(
        *tapping, long_wait,
        [&](const agent::event_batch& received)
        {
            for (const agent::event_record& event : received.events)
            {
                if (is_rx_data_on(event, *b))
                    tapped = event;
            }
            return tapped.has_value();
        });
    REQUIRE(tapped_seen);
    REQUIRE(tapped.has_value());
    REQUIRE_EQ(tapped->size, std::uint64_t{5});
    REQUIRE_EQ(vss::to_string(vss::bytes_view(tapped->data)),
               std::string("hello"));
}

TEST_CASE("agent server: engine errors keep their codes")
{
    fixture env;
    auto source = env.connect();

    const auto removed = source->remove_port(9999);
    REQUIRE(!removed);
    REQUIRE_EQ(removed.error(), code(vss::errc::not_found));
    REQUIRE(!source->last_error_message().empty());

    const auto written = source->write(9999, vss::to_bytes("x"));
    REQUIRE(!written);
    REQUIRE_EQ(written.error(), code(vss::errc::not_found));

    const auto unsupported = source->create_port("x", "pty");
    REQUIRE(!unsupported);
    REQUIRE_EQ(unsupported.error(), code(vss::errc::unsupported));

    const auto id = source->create_port("real");
    REQUIRE(id);
    const auto linked = source->connect_ports(*id, 9999, false);
    REQUIRE(!linked);
    REQUIRE_EQ(linked.error(), code(vss::errc::not_found));
    REQUIRE(!source->last_error_message().empty());

    // An error leaves the connection usable.
    REQUIRE(source->list());
}

// -- the handshake -----------------------------------------------------------

TEST_CASE("agent server: a hello of another protocol version is refused")
{
    fixture env;
    raw_client raw(env.instance.name());
    const auto before = env.server.counters().protocol_errors;
    const agent::hello_request greeting{agent::protocol_version + 1u, "raw"};
    REQUIRE(raw.send(wire_of(make_request(7, greeting))));
    const auto id = expect_refusal(
        env, raw, agent::protocol_errc::version_mismatch, before);
    REQUIRE_EQ(id, std::uint64_t{7});
}

TEST_CASE("agent server: a request before hello is refused")
{
    fixture env;
    raw_client raw(env.instance.name());
    const auto before = env.server.counters().protocol_errors;
    REQUIRE(raw.send(wire_of(make_request(3, agent::list_request{}))));
    expect_refusal(env, raw, agent::protocol_errc::handshake_required, before);
}

TEST_CASE("agent server: an oversized length is refused before any payload")
{
    fixture env;
    raw_client raw(env.instance.name());
    const auto before = env.server.counters().protocol_errors;
    REQUIRE(raw.send(vss::byte_buffer(4, vss::byte{0xff})));
    const auto id =
        expect_refusal(env, raw, agent::protocol_errc::oversized_frame, before);
    REQUIRE_EQ(id, std::uint64_t{0});
}

TEST_CASE("agent server: a header that is not JSON is refused")
{
    fixture env;
    raw_client raw(env.instance.name());
    const auto before = env.server.counters().protocol_errors;
    REQUIRE(raw.send(wire_of_header("this is not json")));
    expect_refusal(env, raw, agent::protocol_errc::malformed_message, before);
}

TEST_CASE("agent server: a second hello is refused")
{
    fixture env;
    raw_client raw(env.instance.name());
    say_hello(raw);
    const auto before = env.server.counters().protocol_errors;
    const agent::hello_request greeting{agent::protocol_version, "raw"};
    REQUIRE(raw.send(wire_of(make_request(2, greeting))));
    expect_refusal(env, raw, agent::protocol_errc::malformed_message, before);
}

TEST_CASE("agent server: an unknown operation costs an error, not the "
          "connection")
{
    fixture env;
    raw_client raw(env.instance.name());
    say_hello(raw);

    REQUIRE(raw.send(wire_of_header(R"({"id":5,"op":"no_such"})")));
    const agent::reply refused = read_reply(raw);
    REQUIRE_EQ(refused.id, std::uint64_t{5});
    REQUIRE_EQ(error_of(refused),
               code(agent::protocol_errc::unknown_operation));

    REQUIRE(raw.send(wire_of(make_request(6, agent::list_request{}))));
    const agent::reply listed = read_reply(raw);
    REQUIRE_EQ(listed.id, std::uint64_t{6});
    REQUIRE(std::holds_alternative<agent::list_reply>(listed.body));
}

TEST_CASE("agent server: a client that says nothing is closed")
{
    agent::server_options options;
    options.handshake_timeout = 200ms;
    fixture env(options);

    raw_client raw(env.instance.name());
    REQUIRE(raw.wait_closed(10s));
    REQUIRE_EQ(env.server.counters().handshake_timeouts, std::uint64_t{1});
    REQUIRE(vss::testkit::wait_until([&] { return stopped_serving(env); },
                                     long_wait));
}

// -- limits ------------------------------------------------------------------

TEST_CASE("agent server: the connection after the last one is told busy")
{
    agent::server_options options;
    options.max_clients = 1;
    fixture env(options);

    auto first = env.connect("first");

    agent::client_options second_options;
    second_options.instance = env.instance.name();
    const auto second = agent::client::connect(second_options);
    REQUIRE(!second);
    REQUIRE_EQ(second.error(), code(agent::protocol_errc::busy));
    REQUIRE_EQ(env.server.counters().refused_busy, std::uint64_t{1});

    first.reset();
    REQUIRE(vss::testkit::wait_until([&] { return stopped_serving(env); },
                                     long_wait));
    const auto third = agent::client::connect(second_options);
    REQUIRE(third);
}

TEST_CASE("agent server: a client that floods does not starve another")
{
    fixture env;
    raw_client raw(env.instance.name());
    say_hello(raw);

    constexpr std::uint64_t first_id = 2;
    constexpr std::uint64_t last_id = 5001;
    vss::byte_buffer flood;
    for (std::uint64_t id = first_id; id <= last_id; ++id)
        append(flood, wire_of(make_request(id, agent::list_request{})));
    raw.send_nowait(std::move(flood));

    // The flood is outstanding and unread; a normal client is served at once.
    auto other = env.connect("other");
    const auto made = other->create_port("flood-port");
    REQUIRE(made);
    const auto listed = other->list();
    REQUIRE(listed);
    REQUIRE(find_port(*listed, *made) != nullptr);

    for (std::uint64_t id = first_id; id <= last_id; ++id)
    {
        const agent::reply answer = read_reply(raw);
        REQUIRE_EQ(answer.id, id);
        REQUIRE(std::holds_alternative<agent::list_reply>(answer.body));
    }
}

// `subscribe` is answered on the agent's client thread, without waiting for
// the request pool, so the limit on waiting requests does not slow it.  The
// agent handles a number of such frames and then lets other clients in.
TEST_CASE("agent server: a flood the agent answers at once is served in turns")
{
    fixture env;
    raw_client raw(env.instance.name());
    say_hello(raw);

    constexpr std::uint64_t first_id = 2;
    constexpr std::uint64_t last_id = 1001;
    const agent::subscribe_request subscription{false, 10'000};
    vss::byte_buffer flood;
    for (std::uint64_t id = first_id; id <= last_id; ++id)
        append(flood, wire_of(make_request(id, subscription)));
    raw.send_nowait(std::move(flood));

    auto other = env.connect("other");
    REQUIRE(other->create_port("turn-port"));

    std::uint64_t next_id = first_id;
    while (next_id <= last_id)
    {
        const auto received = raw.read_frame();
        REQUIRE(received.has_value());
        const auto message = agent::decode_server_message(*received);
        REQUIRE(message);
        // A batch may arrive between two replies: the other client changed
        // the set of ports, and this one is subscribed.
        const auto* answer = std::get_if<agent::reply>(&*message);
        if (answer == nullptr)
            continue;
        REQUIRE_EQ(answer->id, next_id);
        REQUIRE(std::holds_alternative<agent::done_reply>(answer->body));
        ++next_id;
    }
}

TEST_CASE("agent server: a subscriber that does not read loses events and "
          "costs the hub nothing")
{
    agent::server_options options;
    options.max_queued_events = 8;
    fixture env(options);

    raw_client raw(env.instance.name());
    say_hello(raw);
    const agent::subscribe_request subscription{true, 10};
    REQUIRE(raw.send(wire_of(make_request(2, subscription))));
    const agent::reply subscribed = read_reply(raw);
    REQUIRE_EQ(subscribed.id, std::uint64_t{2});
    REQUIRE(std::holds_alternative<agent::done_reply>(subscribed.body));
    // From here the raw client reads nothing until the traffic is over.

    auto source = env.connect("source");
    const auto a = source->create_port("a");
    const auto b = source->create_port("b");
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(source->connect_ports(*a, *b, true));

    constexpr int writes = 500;
    constexpr std::size_t size = 64;
    const vss::byte_buffer payload = vss::testkit::make_pattern(size);
    for (int count = 0; count < writes; ++count)
        REQUIRE(source->write(*a, payload));

    // The engine carried everything while the subscriber was stuck.
    const std::uint64_t expected = std::uint64_t{writes} * size;
    REQUIRE(vss::testkit::wait_until(
        [&] { return port_rx_is(*source, *b, expected); }, long_wait));

    bool dropped = false;
    const auto deadline = steady::now() + long_wait;
    while (!dropped && steady::now() < deadline)
    {
        const auto received = raw.read_frame(1s);
        if (!received)
            continue;
        const auto message = agent::decode_server_message(*received);
        REQUIRE(message);
        const auto* batch = std::get_if<agent::event_batch>(&*message);
        REQUIRE(batch != nullptr);
        dropped = batch->dropped > 0;
    }
    REQUIRE(dropped);
    REQUIRE(vss::testkit::wait_until(
        [&] { return env.server.counters().dropped_events > 0; }, long_wait));
}

TEST_CASE("agent server: a client whose unread replies pile up is closed")
{
    agent::server_options options;
    options.max_unsent_bytes = std::size_t{16} * 1024;
    fixture env(options);

    auto source = env.connect("source");
    for (int count = 0; count < 60; ++count)
        REQUIRE(source->create_port("port-" + std::to_string(count)));

    raw_client raw(env.instance.name());
    say_hello(raw);
    vss::byte_buffer requests;
    for (std::uint64_t id = 2; id < 2002; ++id)
        append(requests, wire_of(make_request(id, agent::list_request{})));
    raw.send_nowait(std::move(requests));
    // The raw client reads nothing.

    REQUIRE(vss::testkit::wait_until(
        [&] { return env.server.counters().slow_clients_closed == 1; },
        long_wait));
    REQUIRE(source->list());
}

TEST_CASE("agent server: a client that disappears costs nothing")
{
    fixture env;
    auto writer = env.connect("writer");
    const auto a = writer->create_port("a");
    const auto b = writer->create_port("b");
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(writer->connect_ports(*a, *b, true));

    auto tap = env.connect("tap");
    REQUIRE(tap->subscribe(true, 10));
    REQUIRE_EQ(env.server.counters().clients, std::uint64_t{2});

    constexpr int total = 200;
    constexpr std::size_t size = 16;
    const vss::byte_buffer payload = vss::testkit::make_pattern(size);
    std::atomic<int> written{0};
    std::atomic<bool> failed{false};
    {
        joining_thread traffic(
            [&]
            {
                for (int count = 0; count < total; ++count)
                {
                    if (!writer->write(*a, payload))
                    {
                        failed.store(true);
                        return;
                    }
                    written.fetch_add(1);
                }
            });
        // The subscriber goes while the traffic runs.
        REQUIRE(vss::testkit::wait_until(
            [&] { return written.load() >= 20 || failed.load(); }, long_wait));
        tap.reset();
    }
    REQUIRE(!failed.load());
    REQUIRE(vss::testkit::wait_until(
        [&] { return env.server.counters().clients == 1; }, long_wait));
    const std::uint64_t expected = std::uint64_t{total} * size;
    REQUIRE(vss::testkit::wait_until(
        [&] { return port_rx_is(*writer, *b, expected); }, long_wait));
}

// -- events ------------------------------------------------------------------

TEST_CASE("agent server: events are sent in batches")
{
    fixture env;
    auto source = env.connect("source");
    const auto a = source->create_port("a");
    const auto b = source->create_port("b");
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(source->connect_ports(*a, *b, true));

    auto watcher = env.connect("watcher");
    REQUIRE(watcher->subscribe(false, 500));

    const vss::byte_buffer one_byte(1, vss::byte{0x41});
    for (int count = 0; count < 100; ++count)
        REQUIRE(source->write(*a, one_byte));

    // Each write is echoed once by `a` and once by `b`.  The engine may join
    // several small writes into one event when it is busy, so the number of
    // events varies; the bytes in them do not.
    std::uint64_t received_bytes = 0;
    std::size_t batches = 0;
    const bool enough = read_batches(
        *watcher, long_wait,
        [&](const agent::event_batch& received)
        {
            if (!received.events.empty())
                ++batches;
            for (const agent::event_record& event : received.events)
            {
                if (event.kind == vss::event_kind::data &&
                    event.direction == vss::data_direction::rx)
                    received_bytes += event.size;
            }
            return received_bytes >= 200;
        });
    REQUIRE(enough);
    REQUIRE(batches <= 20);
}

TEST_CASE("agent server: the revision tells one client what another did")
{
    fixture env;
    auto watcher = env.connect("watcher");
    auto maker = env.connect("maker");

    const auto before = watcher->list();
    REQUIRE(before);
    REQUIRE(watcher->subscribe(false, 20));

    const auto made = maker->create_port("new-port");
    REQUIRE(made);

    std::uint64_t announced = 0;
    const bool changed =
        read_batches(*watcher, long_wait,
                     [&](const agent::event_batch& received)
                     {
                         announced = received.revision;
                         return received.revision > before->revision;
                     });
    REQUIRE(changed);

    const auto after = watcher->list();
    REQUIRE(after);
    REQUIRE(find_port(*after, *made) != nullptr);
    REQUIRE(after->revision >= announced);
    REQUIRE(after->revision > before->revision);
}

// -- stopping ----------------------------------------------------------------

TEST_CASE("agent server: a shutdown request ends the wait for a stop")
{
    fixture env;
    REQUIRE(!env.server.wait_for_stop_request(0ms));

    auto source = env.connect();
    REQUIRE(source->shutdown());
    REQUIRE(env.server.wait_for_stop_request(10s));
}

TEST_CASE("agent server: request_stop ends the wait for a stop")
{
    fixture env;
    REQUIRE(!env.server.wait_for_stop_request(0ms));
    env.server.request_stop();
    REQUIRE(env.server.wait_for_stop_request(10s));
}

TEST_CASE("agent server: stopping closes the clients and frees the name")
{
    fixture env;
    auto first = env.connect("first");
    auto second = env.connect("second");
    REQUIRE(first->create_port("kept"));
    // A long interval, so that no batch is on its way when the server stops.
    REQUIRE(second->subscribe(false, 10000));

    env.server.stop();

    const auto listed = first->list();
    REQUIRE(!listed);
    REQUIRE_EQ(listed.error(), code(agent::protocol_errc::connection_closed));

    // Whatever was already sent may be read first; then the end comes.
    vss::error_code ended;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto batch = second->next_events(5s);
        if (!batch)
        {
            ended = batch.error();
            break;
        }
    }
    REQUIRE_EQ(ended, code(agent::protocol_errc::connection_closed));

    env.server.stop();
    REQUIRE_EQ(env.server.counters().clients, std::uint64_t{0});

    agent::client_options options;
    options.instance = env.instance.name();
    const auto refused = agent::client::connect(options);
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), code(agent::protocol_errc::not_running));

    // The same studio and name serve again.
    agent::server again(env.app, named(env.instance, {}));
    REQUIRE(again.start());
    const auto reconnected = agent::client::connect(options);
    REQUIRE(reconnected);
    REQUIRE((*reconnected)->list());
}

TEST_CASE("agent server: start twice, the same name twice, a bad name")
{
    fixture env;
    const auto again = env.server.start();
    REQUIRE(!again);
    REQUIRE_EQ(again.error(), code(vss::errc::already_open));

    agent::server rival(env.app, named(env.instance, {}));
    const auto taken = rival.start();
    REQUIRE(!taken);
    REQUIRE_EQ(taken.error(), code(agent::protocol_errc::already_running));

    agent::server_options bad;
    bad.instance = "bad/name";
    agent::server invalid(env.app, bad);
    const auto refused = invalid.start();
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), code(vss::errc::invalid_argument));

    // The first server is not disturbed by either.
    auto source = env.connect();
    REQUIRE(source->list());
}

// -- many clients, and a stop underneath -------------------------------------

namespace
{

struct stress_state
{
    std::atomic<bool> stopping{false};
    /// Calls that failed although the server was running.
    std::atomic<int> unexpected{0};
    std::atomic<int> finished{0};
    std::atomic<int> rounds{0};
};

/// Stops the server when a case ends early, so that threads which loop until a
/// call fails are not waited for for ever.
class stop_guard
{
public:
    stop_guard(agent::server& server, stress_state& state)
        : server_(server), state_(state)
    {
    }

    ~stop_guard()
    {
        state_.stopping.store(true);
        server_.stop();
    }

    stop_guard(const stop_guard&) = delete;
    stop_guard& operator=(const stop_guard&) = delete;

private:
    agent::server& server_;
    stress_state& state_;
};

bool stress_round(agent::client& source, vss::endpoint_id sink,
                  const std::string& name, const vss::byte_buffer& payload)
{
    const auto port = source.create_port(name);
    if (!port)
        return false;
    if (!source.connect_ports(*port, sink, true))
        return false;
    if (!source.write(*port, payload))
        return false;
    if (!source.list())
        return false;
    return static_cast<bool>(source.remove_port(*port));
}

/// `rounds` rounds, or rounds until a call fails when `rounds` is 0.
void stress_worker(agent::client& source, vss::endpoint_id sink, int worker,
                   int rounds, stress_state& state)
{
    const vss::byte_buffer payload = vss::testkit::make_pattern(32);
    for (int round = 0; rounds == 0 || round < rounds; ++round)
    {
        const std::string name =
            "w" + std::to_string(worker) + "-" + std::to_string(round);
        if (!stress_round(source, sink, name, payload))
        {
            if (!state.stopping.load())
                state.unexpected.fetch_add(1);
            break;
        }
        state.rounds.fetch_add(1);
    }
    state.finished.fetch_add(1);
}

/// Reads batches until a call fails, which is right only once the server is
/// stopping.
void stress_subscriber(agent::client& source, stress_state& state)
{
    for (;;)
    {
        const auto batch = source.next_events(50ms);
        if (batch || batch.error() == code(vss::errc::timeout))
            continue;
        if (!state.stopping.load())
            state.unexpected.fetch_add(1);
        return;
    }
}

} // namespace

TEST_CASE("agent server: stress with many clients and a stop underneath")
{
    constexpr int workers = 4;
    constexpr int rounds = 100;

    fixture env;
    stress_state state;
    auto setup = env.connect("setup");
    const auto sink = setup->create_port("sink");
    REQUIRE(sink);

    auto watcher = env.connect("watcher");
    REQUIRE(watcher->subscribe(true, 10));
    std::vector<joining_thread> observers;
    const stop_guard outer_guard(env.server, state);
    observers.emplace_back([&] { stress_subscriber(*watcher, state); });

    // First, workers that finish while the server runs.
    {
        std::vector<std::unique_ptr<agent::client>> clients;
        clients.reserve(static_cast<std::size_t>(workers));
        for (int worker = 0; worker < workers; ++worker)
            clients.push_back(env.connect("worker"));
        {
            std::vector<joining_thread> threads;
            threads.reserve(static_cast<std::size_t>(workers));
            for (int worker = 0; worker < workers; ++worker)
            {
                threads.emplace_back(
                    [&, worker]
                    {
                        const auto index = static_cast<std::size_t>(worker);
                        stress_worker(*clients[index], *sink, worker, rounds,
                                      state);
                    });
            }
        }
        REQUIRE_EQ(state.unexpected.load(), 0);
        REQUIRE_EQ(state.finished.load(), workers);
        REQUIRE_EQ(state.rounds.load(), workers * rounds);
    }

    // Then workers that never finish by themselves, and a stop underneath.
    {
        std::vector<std::unique_ptr<agent::client>> clients;
        clients.reserve(static_cast<std::size_t>(workers));
        for (int worker = 0; worker < workers; ++worker)
            clients.push_back(env.connect("endless"));
        std::vector<joining_thread> threads;
        threads.reserve(static_cast<std::size_t>(workers));
        const stop_guard inner_guard(env.server, state);
        for (int worker = 0; worker < workers; ++worker)
        {
            threads.emplace_back(
                [&, worker]
                {
                    const auto index = static_cast<std::size_t>(worker);
                    stress_worker(*clients[index], *sink, workers + worker, 0,
                                  state);
                });
        }

        const int started = state.rounds.load();
        REQUIRE(vss::testkit::wait_until(
            [&] { return state.rounds.load() >= started + 2 * workers; },
            long_wait));
        state.stopping.store(true);
        env.server.stop();

        // Every thread ends: its calls fail and none waits for ever.
        REQUIRE(vss::testkit::wait_until(
            [&] { return state.finished.load() == 2 * workers; }, long_wait));
    }

    REQUIRE_EQ(state.unexpected.load(), 0);
    REQUIRE_EQ(env.server.counters().clients, std::uint64_t{0});
}

// -- the transport on its own ------------------------------------------------

namespace
{

/// Writes a buffer to a stream and reads a number of bytes from it, both in
/// the background of the io_context.  It must be destroyed before the stream.
class transfer
{
public:
    explicit transfer(agent::local_stream& stream)
        : state_(std::make_shared<state>())
    {
        state_->stream = &stream;
    }

    ~transfer() { state_->stream = nullptr; }

    transfer(const transfer&) = delete;
    transfer& operator=(const transfer&) = delete;

    void write_all(vss::byte_buffer bytes)
    {
        state_->outbound = std::move(bytes);
        state_->written = 0;
        state_->writing = true;
        write_more(state_);
    }

    void read_until(std::size_t count)
    {
        state_->wanted = count;
        read_more(state_);
    }

    bool write_done() const { return !state_->writing; }
    bool read_done() const { return state_->received.size() >= state_->wanted; }
    bool failed() const { return state_->failed; }
    const vss::byte_buffer& received() const { return state_->received; }

private:
    struct state
    {
        agent::local_stream* stream = nullptr;
        vss::byte_buffer chunk = vss::byte_buffer(std::size_t{8} * 1024);
        vss::byte_buffer received;
        std::size_t wanted = 0;
        vss::byte_buffer outbound;
        std::size_t written = 0;
        bool writing = false;
        bool failed = false;
    };

    static void write_more(const std::shared_ptr<state>& shared)
    {
        if (shared->stream == nullptr ||
            shared->written >= shared->outbound.size())
        {
            shared->writing = false;
            return;
        }
        shared->stream->async_write_some(
            boost::asio::buffer(shared->outbound.data() + shared->written,
                                shared->outbound.size() - shared->written),
            [shared](boost::system::error_code error, std::size_t size)
            {
                if (error)
                {
                    shared->failed = true;
                    shared->writing = false;
                    return;
                }
                shared->written += size;
                write_more(shared);
            });
    }

    static void read_more(const std::shared_ptr<state>& shared)
    {
        if (shared->stream == nullptr ||
            shared->received.size() >= shared->wanted)
            return;
        shared->stream->async_read_some(
            boost::asio::buffer(shared->chunk),
            [shared](boost::system::error_code error, std::size_t size)
            {
                if (error)
                {
                    shared->failed = true;
                    return;
                }
                shared->received.insert(
                    shared->received.end(), shared->chunk.begin(),
                    shared->chunk.begin() + static_cast<std::ptrdiff_t>(size));
                read_more(shared);
            });
    }

    std::shared_ptr<state> state_;
};

struct accept_result
{
    bool done = false;
    boost::system::error_code failed;
    std::unique_ptr<agent::local_stream> stream;
};

std::shared_ptr<accept_result> accept_into(agent::local_listener& listener)
{
    auto result = std::make_shared<accept_result>();
    listener.async_accept(
        [result](boost::system::error_code failed,
                 std::unique_ptr<agent::local_stream> stream)
        {
            result->done = true;
            result->failed = failed;
            result->stream = std::move(stream);
        });
    return result;
}

boost::system::error_code aborted()
{
    return boost::system::error_code(boost::asio::error::operation_aborted);
}

} // namespace

TEST_CASE("agent transport: instance names")
{
    REQUIRE(agent::valid_instance_name(""));
    REQUIRE(agent::valid_instance_name("a"));
    REQUIRE(agent::valid_instance_name("A-b_9"));
    REQUIRE(agent::valid_instance_name(std::string(32, 'x')));

    REQUIRE(!agent::valid_instance_name(std::string(33, 'x')));
    REQUIRE(!agent::valid_instance_name("a b"));
    REQUIRE(!agent::valid_instance_name("a/b"));
    REQUIRE(!agent::valid_instance_name("a.b"));
    REQUIRE(!agent::valid_instance_name("\xC3\xBC"));
}

TEST_CASE("agent transport: a bad name is refused and an unused one is not "
          "running")
{
    boost::asio::io_context io;

    const auto address = agent::local_address("bad/name");
    REQUIRE(!address);
    REQUIRE_EQ(address.error(), code(vss::errc::invalid_argument));

    const auto listener = agent::listen_local(io.get_executor(), "bad/name");
    REQUIRE(!listener);
    REQUIRE_EQ(listener.error(), code(vss::errc::invalid_argument));

    const auto stream = agent::connect_local(io.get_executor(), "bad/name");
    REQUIRE(!stream);
    REQUIRE_EQ(stream.error(), code(vss::errc::invalid_argument));

    const scoped_instance unused;
    const auto missing = agent::connect_local(io.get_executor(), unused.name());
    REQUIRE(!missing);
    REQUIRE_EQ(missing.error(), code(agent::protocol_errc::not_running));
}

TEST_CASE("agent transport: bytes cross intact both ways and a closed "
          "listener goes")
{
    const scoped_instance instance;
    boost::asio::io_context io;

    auto listener = agent::listen_local(io.get_executor(), instance.name());
    REQUIRE(listener);
    const auto expected = agent::local_address(instance.name());
    REQUIRE(expected);
    REQUIRE_EQ((*listener)->address(), *expected);

    const auto accepted = accept_into(**listener);
    auto connected = agent::connect_local(io.get_executor(), instance.name());
    REQUIRE(connected);
    REQUIRE(run_until(io, [&] { return accepted->done; }, long_wait));
    REQUIRE(!accepted->failed);
    REQUIRE(accepted->stream != nullptr);

    {
        const vss::byte_buffer pattern = vss::testkit::make_pattern(100000);
        transfer client_side(**connected);
        transfer server_side(*accepted->stream);

        client_side.write_all(pattern);
        server_side.read_until(pattern.size());
        REQUIRE(run_until(
            io, [&] { return server_side.read_done() || server_side.failed(); },
            long_wait));
        REQUIRE(server_side.received() == pattern);
        REQUIRE(
            run_until(io, [&] { return client_side.write_done(); }, long_wait));
        REQUIRE(!client_side.failed());

        server_side.write_all(server_side.received());
        client_side.read_until(pattern.size());
        REQUIRE(run_until(
            io, [&] { return client_side.read_done() || client_side.failed(); },
            long_wait));
        REQUIRE(client_side.received() == pattern);
        REQUIRE(
            run_until(io, [&] { return server_side.write_done(); }, long_wait));
        REQUIRE(!server_side.failed());
    }

    // With the connection gone, closing the listener ends the name: a pending
    // accept is aborted and nobody can connect.
    (*connected).reset();
    accepted->stream.reset();
    const auto pending = accept_into(**listener);
    (*listener)->close();
    REQUIRE(run_until(io, [&] { return pending->done; }, long_wait));
    REQUIRE_EQ(pending->failed, aborted());
    REQUIRE(pending->stream == nullptr);

    const auto refused =
        agent::connect_local(io.get_executor(), instance.name());
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), code(agent::protocol_errc::not_running));
}

TEST_CASE("agent transport: a listener destroyed with an accept pending is "
          "safe")
{
    const scoped_instance instance;
    boost::asio::io_context io;

    auto listener = agent::listen_local(io.get_executor(), instance.name());
    REQUIRE(listener);
    const auto pending = accept_into(**listener);
    listener->reset();

    io.restart();
    io.poll();
    if (pending->done)
    {
        REQUIRE_EQ(pending->failed, aborted());
        REQUIRE(pending->stream == nullptr);
    }
}
