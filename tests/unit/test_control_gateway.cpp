//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control gateway over the real pipe or socket, with a real agent behind
// it in the same process: who a connection is taken to be, what it may do,
// every way of asking for something before or without the right to it, the
// limits, a hostile client, and what the audit log says afterwards
// (docs/remote-control.md, section 15).  Every gateway and agent here has its
// own instance name and its own audit directory, so none of them meets the
// user's own or another test.  Nothing waits for a fixed time: each wait has
// a condition and a deadline.
//

#include <vss/agent/client.hpp>
#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/agent/server.hpp>
#include <vss/control/audit_log.hpp>
#include <vss/control/client.hpp>
#include <vss/control/gateway.hpp>
#include <vss/control/proto/message.hpp>
#include <vss/core/bytes.hpp>
#include <vss/studio/studio.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace agent = vss::agent;
namespace control = vss::control;
namespace json = boost::json;
using namespace std::chrono_literals;

namespace
{

using steady = std::chrono::steady_clock;

/// Long enough for a sanitizer build on a loaded machine.  A test that passes
/// takes milliseconds.
constexpr std::chrono::milliseconds long_wait{20000};

vss::error_code code(control::api_errc value)
{
    return control::make_error_code(value);
}

vss::error_code code(agent::protocol_errc value)
{
    return agent::make_error_code(value);
}

/// A thread that is joined when it goes out of scope.
class joining_thread
{
public:
    explicit joining_thread(std::function<void()> function)
        : thread_(std::move(function))
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

// -- instance names ----------------------------------------------------------

std::string unique_instance_name()
{
    static const unsigned random = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    std::ostringstream out;
    out << "c" << std::hex << random << '-' << std::dec << counter.fetch_add(1);
    return out.str();
}

/// A name no other gateway or agent has.  Where the transport leaves lock
/// files beside the sockets, they are removed with the name.
class scoped_instance
{
public:
    scoped_instance() : name_(unique_instance_name()) {}

    ~scoped_instance()
    {
        try
        {
            for (const agent::local_endpoint endpoint :
                 {agent::local_endpoint::agent, agent::local_endpoint::control,
                  agent::local_endpoint::control_admin})
            {
                const auto address = agent::local_address(endpoint, name_);
                if (!address)
                    continue;
                std::filesystem::path lock(*address);
                if (lock.extension() != ".sock")
                    continue;
                lock.replace_extension(".lock");
                std::error_code ignored;
                std::filesystem::remove(lock, ignored);
            }
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

/// The agent a gateway serves: a studio and its server, in this process.
struct agent_side
{
    explicit agent_side(const std::string& instance)
        : server(app, options_for(instance))
    {
        const auto started = server.start();
        REQUIRE(started);
    }

    static agent::server_options options_for(const std::string& instance)
    {
        agent::server_options options;
        options.instance = instance;
        return options;
    }

    vss::studio app;
    agent::server server;
};

/// A gateway with its own name and audit directory, started, and the agent
/// behind it unless the test wants none.
struct fixture
{
    explicit fixture(control::gateway_options options = {},
                     bool with_agent = true)
        : audit_directory(scratch.path() / "audit"),
          gate(prepared(std::move(options)))
    {
        if (with_agent)
            start_agent();
        const auto started = gate.start();
        REQUIRE(started);
    }

    control::gateway_options prepared(control::gateway_options options) const
    {
        options.instance = instance.name();
        options.agent_instance = instance.name();
        options.audit_directory = audit_directory;
        return options;
    }

    void start_agent()
    {
        backend = std::make_unique<agent_side>(instance.name());
    }
    void stop_agent() { backend.reset(); }

    std::unique_ptr<control::client> connect(bool admin = false,
                                             const char* label = "test")
    {
        control::client_options options;
        options.instance = instance.name();
        options.name = label;
        options.admin = admin;
        auto connected = control::client::connect(options);
        REQUIRE(connected);
        return std::move(*connected);
    }

    scoped_instance instance;
    vss_test_support::scratch_directory scratch;
    std::filesystem::path audit_directory;
    // The agent is declared before the gateway, so the gateway goes first.
    std::unique_ptr<agent_side> backend;
    control::gateway gate;
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

template <class Predicate>
bool wait_until(Predicate done, std::chrono::milliseconds limit = long_wait)
{
    const auto deadline = steady::now() + limit;
    while (!done())
    {
        if (steady::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// -- raw bytes ---------------------------------------------------------------

vss::byte_buffer wire_of(const control::frame& value)
{
    auto bytes = agent::encode_frame(value);
    REQUIRE(bytes);
    return std::move(*bytes);
}

vss::byte_buffer wire_of(const control::request& value)
{
    return wire_of(control::encode(value));
}

/// A frame with this header and no blob, as the wire carries it.
vss::byte_buffer wire_of_header(std::string header)
{
    control::frame value;
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

/// A connection to the gateway that speaks bytes, not messages, so a test can
/// send what a client library never would.  Its operations run only while one
/// of its own calls drives the io_context.
class raw_client
{
public:
    explicit raw_client(const std::string& instance, bool admin = false)
        : state_(std::make_shared<channel>())
    {
        auto connected =
            agent::connect_local(io_.get_executor(),
                                 admin ? agent::local_endpoint::control_admin
                                       : agent::local_endpoint::control,
                                 instance);
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
    std::optional<control::frame>
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

    /// True when a read ended with an error: the gateway closed the
    /// connection.
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

    /// Who the operating system says serves the other end.
    agent::peer_identity server()
    {
        const auto identity = stream_->peer();
        REQUIRE(identity);
        return *identity;
    }

private:
    // The io_context goes first in the list, so it is destroyed last: the
    // stream and the handlers it still owes are gone before it.
    boost::asio::io_context io_;
    std::unique_ptr<agent::local_stream> stream_;
    std::shared_ptr<channel> state_;
};

control::reply read_reply(raw_client& raw)
{
    const auto received = raw.read_frame();
    REQUIRE(received.has_value());
    const auto message = control::decode_reply(*received);
    REQUIRE(message);
    return *message;
}

control::api_error error_of(const control::reply& answer)
{
    const auto* refused = std::get_if<control::error_reply>(&answer.body);
    REQUIRE(refused != nullptr);
    return refused->error;
}

/// Says hello as a good client would, and returns the welcome.
control::hello_reply say_hello(raw_client& raw, const char* name = "raw")
{
    const control::hello_request greeting{control::api_version, name};
    REQUIRE(raw.send(wire_of(control::request{1, greeting})));
    const control::reply welcome = read_reply(raw);
    REQUIRE_EQ(welcome.id, std::uint64_t{1});
    const auto* body = std::get_if<control::hello_reply>(&welcome.body);
    REQUIRE(body != nullptr);
    return *body;
}

/// The gateway answered with this error and closed the connection.
void expect_refused_and_closed(raw_client& raw, control::api_errc expected)
{
    const control::reply answer = read_reply(raw);
    REQUIRE_EQ(error_of(answer).code,
               std::string(control::code_name(expected)));
    REQUIRE(raw.wait_closed());
}

bool has_scope(const control::hello_reply& welcome, std::string_view name)
{
    return std::ranges::find(welcome.scopes, name) != welcome.scopes.end();
}

// -- the audit log -----------------------------------------------------------

/// Every record of the audit log, oldest first.  Each line must be one JSON
/// object: a line that is not would mean something a client sent became a
/// line of its own.
std::vector<json::object> audit_records(const std::filesystem::path& directory)
{
    std::vector<json::object> out;
    std::ifstream file(directory / "audit.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(file, line))
    {
        boost::system::error_code failed;
        json::value parsed = json::parse(line, failed);
        REQUIRE(!failed);
        REQUIRE(parsed.is_object());
        out.push_back(std::move(parsed.as_object()));
    }
    return out;
}

std::string text_of(const json::object& record, std::string_view key)
{
    const json::value* const item = record.if_contains(key);
    if (item == nullptr || !item->is_string())
        return {};
    return std::string(item->get_string().c_str());
}

std::uint64_t number_of(const json::object& record, std::string_view key)
{
    const json::value* const item = record.if_contains(key);
    if (item == nullptr)
        return 0;
    if (item->is_uint64())
        return item->get_uint64();
    if (item->is_int64() && item->get_int64() >= 0)
        return static_cast<std::uint64_t>(item->get_int64());
    return 0;
}

std::vector<json::object> records_of(const std::vector<json::object>& all,
                                     std::string_view event)
{
    std::vector<json::object> out;
    for (const json::object& record : all)
    {
        if (text_of(record, "event") == event)
            out.push_back(record);
    }
    return out;
}

void require_intact(const std::filesystem::path& directory)
{
    const auto verdict = control::verify_audit_log(directory);
    REQUIRE(verdict);
    REQUIRE(verdict->intact);
}

} // namespace

// -- who a connection is -----------------------------------------------------

TEST_CASE("control gateway: a local connection is this user and may monitor")
{
    fixture env;
    const auto port = env.backend->app.create_virtual_port(
        "alpha", vss::virtual_port_kind::loopback);
    REQUIRE(port);

    raw_client raw(env.instance.name());
    // The client asks the kernel who serves the pipe or socket, too.
    const agent::peer_identity server = raw.server();
    REQUIRE(server.same_user);
    REQUIRE(!server.user.empty());

    const control::hello_reply welcome = say_hello(raw);
    REQUIRE_EQ(welcome.api, control::api_version);
    REQUIRE_EQ(welcome.version, control::gateway_version());
    REQUIRE_EQ(welcome.principal, "local:" + server.user);
    REQUIRE(welcome.scopes == std::vector<std::string>{"monitor"});

    auto client = env.connect();
    const auto status = client->status();
    REQUIRE(status);
    REQUIRE(status->agent_running);
    REQUIRE_EQ(status->agent_version, agent::agent_version());
    REQUIRE_EQ(status->sessions, std::uint64_t{2});

    const auto listed = client->list_ports();
    REQUIRE(listed);
    REQUIRE_EQ(listed->ports.size(), std::size_t{1});
    REQUIRE_EQ(listed->ports[0].name, std::string("alpha"));
    REQUIRE_EQ(listed->ports[0].id, control::port_id_text((*port)->id()));
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{2});
}

TEST_CASE("control gateway: the control role is a setting, and admin is "
          "never one")
{
    control::gateway_options options;
    options.local_scopes = control::role_control();
    {
        fixture env(options);
        raw_client raw(env.instance.name());
        const control::hello_reply welcome = say_hello(raw);
        REQUIRE(welcome.scopes ==
                control::scope_names(control::role_control()));
        REQUIRE(!has_scope(welcome, "admin"));
    }

    // Even a gateway that is told to give the public endpoint everything
    // does not give it admin, and does not let it stop the gateway.
    options.local_scopes = control::every_scope();
    fixture env(options);
    raw_client raw(env.instance.name());
    const control::hello_reply welcome = say_hello(raw);
    REQUIRE(has_scope(welcome, "control.apply"));
    REQUIRE(!has_scope(welcome, "admin"));

    REQUIRE(raw.send(
        wire_of(control::request{2, control::endpoint_stop_request{}})));
    const control::reply answer = read_reply(raw);
    REQUIRE_EQ(answer.id, std::uint64_t{2});
    REQUIRE_EQ(error_of(answer).code, std::string("forbidden"));
    REQUIRE(!env.gate.wait_for_stop_request(0ms));
}

TEST_CASE("control gateway: the administration endpoint holds admin and can "
          "stop the gateway")
{
    fixture env;
    raw_client raw(env.instance.name(), true);
    const control::hello_reply welcome = say_hello(raw);
    REQUIRE(has_scope(welcome, "admin"));
    REQUIRE(has_scope(welcome, "monitor"));

    auto admin = env.connect(true);
    REQUIRE(!env.gate.wait_for_stop_request(0ms));
    const auto stopped = admin->stop();
    REQUIRE(stopped);
    REQUIRE(env.gate.wait_for_stop_request(long_wait));
}

// -- asking before, or without, the right to ---------------------------------

TEST_CASE("control gateway: before hello every request is refused the same "
          "way")
{
    fixture env;
    const std::vector<vss::byte_buffer> attempts{
        wire_of(control::request{1, control::status_request{}}),
        wire_of(control::request{1, control::ports_list_request{}}),
        wire_of(control::request{1, control::endpoint_stop_request{}}),
        // An operation that does not exist: the caller does not learn that.
        wire_of_header(R"({"id":1,"op":"tokens.create"})"),
        // Not a request at all.
        wire_of_header(R"({"hello":true})"), wire_of_header("[]"),
        // A hello with no version.
        wire_of_header(R"({"id":1,"op":"hello"})")};

    for (const bool admin : {false, true})
    {
        for (const vss::byte_buffer& attempt : attempts)
        {
            raw_client raw(env.instance.name(), admin);
            REQUIRE(raw.send(attempt));
            const control::reply answer = read_reply(raw);
            const control::api_error refused = error_of(answer);
            REQUIRE_EQ(refused.code, std::string("unauthenticated"));
            // The same words every time: nothing about what was wrong.
            REQUIRE(refused ==
                    control::to_api_error(control::api_errc::unauthenticated));
            REQUIRE(raw.wait_closed());
        }
    }
    REQUIRE(!env.gate.wait_for_stop_request(0ms));
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{0});
    REQUIRE(wait_until([&] { return env.gate.counters().connections == 0; }));
    // None of it reached an operation.
    REQUIRE(
        records_of(audit_records(env.audit_directory), "operation").empty());
    REQUIRE(records_of(audit_records(env.audit_directory), "session.opened")
                .empty());
}

TEST_CASE("control gateway: a request sent with a refused hello is never "
          "run")
{
    fixture env;
    raw_client raw(env.instance.name(), true);
    // A hello for an API this gateway does not speak, and in the same write
    // the request that would stop it.
    vss::byte_buffer both = wire_of(control::request{
        1, control::hello_request{control::api_version + 1, "future"}});
    append(both,
           wire_of(control::request{2, control::endpoint_stop_request{}}));
    REQUIRE(raw.send(std::move(both)));

    const control::reply answer = read_reply(raw);
    REQUIRE_EQ(answer.id, std::uint64_t{1});
    REQUIRE_EQ(error_of(answer).code, std::string("unsupported"));
    REQUIRE(raw.wait_closed());
    REQUIRE(!env.gate.wait_for_stop_request(0ms));
}

TEST_CASE("control gateway: a second hello cannot change who a connection is")
{
    fixture env;
    raw_client raw(env.instance.name());
    say_hello(raw);
    const control::hello_request again{control::api_version, "again"};
    REQUIRE(raw.send(wire_of(control::request{2, again})));
    const control::reply answer = read_reply(raw);
    REQUIRE_EQ(answer.id, std::uint64_t{2});
    REQUIRE_EQ(error_of(answer).code, std::string("invalid_argument"));
    REQUIRE(raw.wait_closed());
}

TEST_CASE("control gateway: what a caller may not do is refused, counted and "
          "recorded, and the session goes on")
{
    fixture env;
    auto client = env.connect();
    const auto refused = client->stop();
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), code(control::api_errc::forbidden));
    REQUIRE(!env.gate.wait_for_stop_request(0ms));
    REQUIRE_EQ(env.gate.counters().forbidden, std::uint64_t{1});

    // The session is still good for what it may do.
    REQUIRE(client->status());

    const auto forbidden =
        records_of(audit_records(env.audit_directory), "request.forbidden");
    REQUIRE_EQ(forbidden.size(), std::size_t{1});
    REQUIRE_EQ(text_of(forbidden[0], "operation"),
               std::string("endpoint.stop"));
    REQUIRE_EQ(text_of(forbidden[0], "missing"), std::string("admin"));
    REQUIRE_EQ(text_of(forbidden[0], "principal"), client->gateway().principal);
}

TEST_CASE("control gateway: a flood of refused requests cannot fill the "
          "audit log")
{
    control::gateway_options options;
    options.audit_burst_period = 60000ms;
    fixture env(options);
    constexpr std::uint64_t attempts = 40;
    {
        auto client = env.connect();
        for (std::uint64_t index = 0; index < attempts; ++index)
            REQUIRE(!client->stop());
    }
    REQUIRE_EQ(env.gate.counters().forbidden, attempts);
    REQUIRE(wait_until([&] { return env.gate.counters().connections == 0; }));

    // One record for the first, and the rest as a number when the session
    // ended.  Nothing was lost and nothing was written forty times.
    const auto records = audit_records(env.audit_directory);
    const auto forbidden = records_of(records, "request.forbidden");
    REQUIRE_EQ(forbidden.size(), std::size_t{1});
    const auto closed = records_of(records, "session.closed");
    REQUIRE_EQ(closed.size(), std::size_t{1});
    REQUIRE_EQ(number_of(closed[0], "forbidden_unrecorded"), attempts - 1);
    require_intact(env.audit_directory);
}

// -- the protocol ------------------------------------------------------------

TEST_CASE("control gateway: a hello may be small, and what follows may be "
          "large")
{
    fixture env;
    {
        // 5000 bytes where 4096 are allowed.
        raw_client raw(env.instance.name());
        const control::hello_request big{control::api_version,
                                         std::string(1000, 'x')};
        control::frame padded = control::encode(control::request{1, big});
        padded.header.insert(padded.header.size() - 1,
                             R"(,"pad":")" + std::string(5000, 'p') + R"(")");
        REQUIRE(raw.send(wire_of(padded)));
        expect_refused_and_closed(raw, control::api_errc::limit_exceeded);
    }

    // After hello the same connection may send a frame of 100 KiB.
    raw_client raw(env.instance.name());
    say_hello(raw);
    control::frame large =
        control::encode(control::request{2, control::status_request{}});
    large.header.insert(large.header.size() - 1,
                        R"(,"pad":")" +
                            std::string(std::size_t{100} * 1024, 'p') + R"(")");
    REQUIRE(raw.send(wire_of(large)));
    const control::reply answer = read_reply(raw);
    REQUIRE_EQ(answer.id, std::uint64_t{2});
    REQUIRE(std::holds_alternative<control::status_reply>(answer.body));
}

TEST_CASE("control gateway: a length that cannot be right ends the "
          "connection")
{
    fixture env;
    raw_client raw(env.instance.name());
    say_hello(raw);
    // A frame of 2 MiB is announced; nothing more is sent.
    REQUIRE(raw.send(
        {vss::byte{0x00}, vss::byte{0x00}, vss::byte{0x20}, vss::byte{0x00}}));
    expect_refused_and_closed(raw, control::api_errc::limit_exceeded);
}

TEST_CASE("control gateway: an operation this gateway does not have costs an "
          "error, not the session")
{
    fixture env;
    raw_client raw(env.instance.name());
    say_hello(raw);
    REQUIRE(raw.send(wire_of_header(R"({"id":5,"op":"ports.create"})")));
    const control::reply answer = read_reply(raw);
    REQUIRE_EQ(answer.id, std::uint64_t{5});
    REQUIRE_EQ(error_of(answer).code, std::string("unsupported"));

    REQUIRE(raw.send(wire_of(control::request{6, control::status_request{}})));
    const control::reply next = read_reply(raw);
    REQUIRE_EQ(next.id, std::uint64_t{6});
    REQUIRE(std::holds_alternative<control::status_reply>(next.body));
}

TEST_CASE("control gateway: a request that is not one ends the session")
{
    fixture env;
    for (const char* header :
         {R"({"id":0,"op":"status"})", R"({"op":"status"})", "{}",
          R"({"id":3,"op":"status","x":[[[[[[[[1]]]]]]]]})"})
    {
        raw_client raw(env.instance.name());
        say_hello(raw);
        REQUIRE(raw.send(wire_of_header(header)));
        expect_refused_and_closed(raw, control::api_errc::invalid_argument);
    }
    // A request with bytes attached, which no request of this version has.
    raw_client raw(env.instance.name());
    say_hello(raw);
    control::frame carried =
        control::encode(control::request{9, control::status_request{}});
    carried.blob.assign(64, vss::byte{0x55});
    REQUIRE(raw.send(wire_of(carried)));
    expect_refused_and_closed(raw, control::api_errc::invalid_argument);
}

TEST_CASE("control gateway: a client that says nothing is closed")
{
    control::gateway_options options;
    options.handshake_timeout = 200ms;
    fixture env(options);
    raw_client raw(env.instance.name());
    REQUIRE(raw.wait_closed());
    REQUIRE_EQ(env.gate.counters().handshake_timeouts, std::uint64_t{1});
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{0});

    const auto rejected =
        records_of(audit_records(env.audit_directory), "connection.rejected");
    REQUIRE_EQ(rejected.size(), std::size_t{1});
    REQUIRE_EQ(text_of(rejected[0], "reason"),
               std::string("handshake_timeout"));
}

// -- limits ------------------------------------------------------------------

TEST_CASE("control gateway: one principal has so many sessions, and the "
          "administration endpoint is counted apart")
{
    control::gateway_options options;
    options.max_sessions_per_principal = 2;
    fixture env(options);
    auto first = env.connect();
    auto second = env.connect();

    {
        raw_client third(env.instance.name());
        const control::hello_request greeting{control::api_version, "third"};
        REQUIRE(third.send(wire_of(control::request{1, greeting})));
        expect_refused_and_closed(third, control::api_errc::busy);
    }
    REQUIRE_EQ(env.gate.counters().refused_busy, std::uint64_t{1});
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{2});

    // A full public endpoint can still be administered.
    auto admin = env.connect(true);
    REQUIRE(admin->status());

    // A session that ends makes room.
    first.reset();
    REQUIRE(wait_until([&] { return env.gate.counters().sessions == 2; }));
    auto again = env.connect();
    REQUIRE(again->status());
}

TEST_CASE("control gateway: only so many connections may wait to say hello")
{
    control::gateway_options options;
    options.max_unauthenticated = 2;
    fixture env(options);
    raw_client first(env.instance.name());
    raw_client second(env.instance.name());
    REQUIRE(wait_until([&] { return env.gate.counters().connections == 2; }));

    raw_client third(env.instance.name());
    expect_refused_and_closed(third, control::api_errc::busy);

    // The ones that were waiting are not disturbed.
    say_hello(first);
    say_hello(second);
}

TEST_CASE("control gateway: requests faster than the allowance are refused "
          "and may be tried again")
{
    control::gateway_options options;
    options.requests_per_second = 0.001;
    options.request_burst = 3.0;
    fixture env(options);
    raw_client raw(env.instance.name());
    say_hello(raw);

    constexpr std::uint64_t sent = 10;
    vss::byte_buffer burst;
    for (std::uint64_t index = 0; index < sent; ++index)
        append(burst, wire_of(control::request{100 + index,
                                               control::status_request{}}));
    REQUIRE(raw.send(std::move(burst)));

    std::uint64_t answered = 0;
    std::uint64_t limited = 0;
    for (std::uint64_t index = 0; index < sent; ++index)
    {
        const control::reply answer = read_reply(raw);
        if (std::holds_alternative<control::status_reply>(answer.body))
        {
            ++answered;
            continue;
        }
        const control::api_error refused = error_of(answer);
        REQUIRE_EQ(refused.code, std::string("limit_exceeded"));
        REQUIRE(refused.retryable);
        ++limited;
    }
    REQUIRE_EQ(answered, std::uint64_t{3});
    REQUIRE_EQ(limited, sent - 3);
    REQUIRE_EQ(env.gate.counters().rate_limited, sent - 3);

    // Another session has its own allowance.
    auto other = env.connect();
    REQUIRE(other->status());
}

TEST_CASE("control gateway: a client whose unread replies pile up is closed")
{
    control::gateway_options options;
    options.max_unsent_bytes = std::size_t{64} * 1024;
    options.requests_per_second = 1e9;
    options.request_burst = 1e9;
    fixture env(options);
    raw_client raw(env.instance.name());
    say_hello(raw);

    // Requests the gateway answers at once, and nobody reads the answers.
    const vss::byte_buffer one =
        wire_of_header(R"({"id":7,"op":"no.such.operation"})");
    vss::byte_buffer flood;
    for (int index = 0; index < 4000; ++index)
        append(flood, one);
    const auto deadline = steady::now() + long_wait;
    while (env.gate.counters().slow_clients_closed == 0 &&
           steady::now() < deadline)
    {
        if (!raw.send(flood, 200ms))
            break;
    }
    REQUIRE(wait_until(
        [&] { return env.gate.counters().slow_clients_closed == 1; }));
    REQUIRE(wait_until([&] { return env.gate.counters().connections == 0; }));

    // The gateway still serves the others.
    auto other = env.connect();
    REQUIRE(other->status());
}

// -- the agent behind it -----------------------------------------------------

TEST_CASE("control gateway: without an agent it says so, and recovers when "
          "one starts")
{
    fixture env({}, false);
    auto client = env.connect();
    const auto status = client->status();
    REQUIRE(status);
    REQUIRE(!status->agent_running);
    REQUIRE(status->agent_version.empty());

    const auto listed = client->list_ports();
    REQUIRE(!listed);
    REQUIRE_EQ(listed.error(), code(control::api_errc::unavailable));

    env.start_agent();
    REQUIRE(env.backend->app.create_virtual_port(
        "late", vss::virtual_port_kind::loopback));
    const auto after = client->list_ports();
    REQUIRE(after);
    REQUIRE_EQ(after->ports.size(), std::size_t{1});

    // The agent goes away under the session, and comes back.
    env.stop_agent();
    const auto gone = client->list_ports();
    REQUIRE(!gone);
    REQUIRE_EQ(gone.error(), code(control::api_errc::unavailable));
    const auto quiet = client->status();
    REQUIRE(quiet);
    REQUIRE(!quiet->agent_running);

    env.start_agent();
    const auto back = client->list_ports();
    REQUIRE(back);
    REQUIRE(back->ports.empty());
}

TEST_CASE("control gateway: what the agent says is not passed on as it is")
{
    fixture env;
    // A name that is not UTF-8 cannot be made through the studio, so the
    // public ids are what is checked: none is a number the engine uses or a
    // path.
    const auto first = env.backend->app.create_virtual_port(
        "one", vss::virtual_port_kind::loopback);
    const auto second = env.backend->app.create_virtual_port(
        "two", vss::virtual_port_kind::loopback);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(
        env.backend->app.engine().redirect((*first)->id(), (*second)->id()));

    auto client = env.connect();
    const auto listed = client->list_ports();
    REQUIRE(listed);
    REQUIRE_EQ(listed->ports.size(), std::size_t{2});
    for (const control::port_entry& port : listed->ports)
    {
        REQUIRE(std::string_view(port.id).starts_with("p_"));
        REQUIRE(!port.kind.empty());
        REQUIRE(!port.state.empty());
    }
    REQUIRE_EQ(listed->links.size(), std::size_t{1});
    REQUIRE(std::string_view(listed->links[0].id).starts_with("l_"));
    REQUIRE_EQ(listed->links[0].sources.size(), std::size_t{1});
    REQUIRE_EQ(listed->links[0].sources[0],
               control::port_id_text((*first)->id()));
    REQUIRE_EQ(listed->links[0].targets[0],
               control::port_id_text((*second)->id()));
}

// -- starting and stopping ---------------------------------------------------

TEST_CASE("control gateway: one gateway for each name, and the name is free "
          "again afterwards")
{
    fixture env;
    control::gateway second(env.prepared({}));
    const auto refused = second.start();
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), code(agent::protocol_errc::already_running));

    const auto twice = env.gate.start();
    REQUIRE(!twice);

    env.gate.stop();
    env.gate.stop();
    control::gateway third(env.prepared({}));
    REQUIRE(third.start());
    REQUIRE(!third.address().empty());
    REQUIRE(!third.admin_address().empty());
    REQUIRE_NE(third.address(), third.admin_address());
}

TEST_CASE("control gateway: stopping closes the sessions")
{
    fixture env;
    auto client = env.connect();
    REQUIRE(client->status());
    env.gate.stop();
    const auto after = client->status();
    REQUIRE(!after);
    REQUIRE_EQ(after.error(), code(agent::protocol_errc::connection_closed));
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{0});

    control::client_options options;
    options.instance = env.instance.name();
    const auto late = control::client::connect(options);
    REQUIRE(!late);
    REQUIRE_EQ(late.error(), code(agent::protocol_errc::not_running));
}

TEST_CASE("control gateway: a gateway whose audit directory is in the way "
          "does not start")
{
    const scoped_instance instance;
    const vss_test_support::scratch_directory scratch;
    // A file where the directory should be.
    const std::filesystem::path blocked = scratch.path() / "audit";
    {
        std::ofstream file(blocked);
        file << "not a directory";
    }
    control::gateway_options options;
    options.instance = instance.name();
    options.audit_directory = blocked;
    control::gateway gate(options);
    REQUIRE(!gate.start());

    // Nothing is left listening: no gateway serves what it cannot record.
    control::client_options client;
    client.instance = instance.name();
    const auto connected = control::client::connect(client);
    REQUIRE(!connected);
    REQUIRE_EQ(connected.error(), code(agent::protocol_errc::not_running));
}

// -- the audit log -----------------------------------------------------------

TEST_CASE("control gateway: the audit log says who came, what they did and "
          "that it ended")
{
    fixture env;
    std::string principal;
    std::uint64_t own_process = 0;
    {
        agent::client_options how;
        how.instance = env.instance.name();
        const auto direct = agent::client::connect(how);
        REQUIRE(direct);
        // The agent runs in this process, so its number is this process's.
        own_process = (*direct)->agent().process_id;
    }
    {
        auto reader = env.connect(false, "reader 1.0");
        principal = reader->gateway().principal;
        REQUIRE(reader->list_ports());
        REQUIRE(!reader->stop());
    }
    {
        auto admin = env.connect(true, "admin tool");
        REQUIRE(admin->stop());
    }
    REQUIRE(env.gate.wait_for_stop_request(long_wait));
    env.gate.stop();

    require_intact(env.audit_directory);
    const auto records = audit_records(env.audit_directory);
    REQUIRE_EQ(text_of(records.front(), "event"),
               std::string("gateway.started"));
    REQUIRE_EQ(text_of(records.front(), "local_scopes"),
               std::string("monitor"));
    REQUIRE_EQ(text_of(records.back(), "event"),
               std::string("gateway.stopped"));

    const auto opened = records_of(records, "session.opened");
    REQUIRE_EQ(opened.size(), std::size_t{2});
    REQUIRE_EQ(text_of(opened[0], "principal"), principal);
    REQUIRE_EQ(text_of(opened[0], "transport"), std::string("local"));
    REQUIRE_EQ(text_of(opened[0], "scopes"), std::string("monitor"));
    REQUIRE_EQ(text_of(opened[0], "client"), std::string("reader 1.0"));
    REQUIRE_EQ(number_of(opened[0], "process"), own_process);
    REQUIRE(!text_of(opened[0], "program").empty());
    REQUIRE_EQ(text_of(opened[1], "transport"), std::string("local-admin"));
    REQUIRE(text_of(opened[1], "scopes").find("admin") != std::string::npos);

    REQUIRE_EQ(records_of(records, "request.forbidden").size(), std::size_t{1});
    const auto operations = records_of(records, "operation");
    REQUIRE_EQ(operations.size(), std::size_t{1});
    REQUIRE_EQ(text_of(operations[0], "operation"),
               std::string("endpoint.stop"));
    REQUIRE_EQ(number_of(operations[0], "session"),
               number_of(opened[1], "session"));
    REQUIRE_EQ(records_of(records, "session.closed").size(), std::size_t{2});
}

TEST_CASE("control gateway: what a client calls itself cannot write a line "
          "of the audit log")
{
    fixture env;
    const std::string forged =
        "x\"}\n{\"seq\":1,\"event\":\"gateway.stopped\",\"prev\":\"\"}\r\n"
        "\x1b[2J\xff";
    {
        raw_client raw(env.instance.name());
        say_hello(raw, forged.c_str());
    }
    REQUIRE(wait_until([&] { return env.gate.counters().connections == 0; }));

    require_intact(env.audit_directory);
    // Every line is still one record, and none of them is the forged one.
    const auto records = audit_records(env.audit_directory);
    REQUIRE(records_of(records, "gateway.stopped").empty());
    const auto opened = records_of(records, "session.opened");
    REQUIRE_EQ(opened.size(), std::size_t{1});
    // It is there as what it is: text a client sent.
    REQUIRE(text_of(opened[0], "client").find("gateway.stopped") !=
            std::string::npos);
}

// -- under load --------------------------------------------------------------

TEST_CASE("control gateway: stress with many sessions and a stop underneath")
{
    control::gateway_options options;
    options.max_sessions = 16;
    options.max_sessions_per_principal = 16;
    options.max_unauthenticated = 16;
    fixture env(options);
    REQUIRE(env.backend->app.create_virtual_port(
        "busy", vss::virtual_port_kind::loopback));

    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> answered{0};
    {
        std::vector<joining_thread> workers;
        workers.reserve(6);
        for (int worker = 0; worker < 6; ++worker)
        {
            workers.emplace_back(
                [&, worker]
                {
                    while (running.load())
                    {
                        control::client_options how;
                        how.instance = env.instance.name();
                        how.name = "stress";
                        // Every third one tries what it may not.
                        auto connected = control::client::connect(how);
                        if (!connected)
                            continue;
                        for (int turn = 0; turn < 20 && running.load(); ++turn)
                        {
                            const bool fine =
                                turn % 3 == worker % 3
                                    ? !(*connected)->stop()
                                    : (*connected)->list_ports().has_value();
                            if (!fine)
                                break;
                            answered.fetch_add(1);
                        }
                    }
                });
        }
        REQUIRE(wait_until([&] { return answered.load() >= 300; }));
        // The gateway is stopped while the sessions are in full flow.
        env.gate.stop();
        running.store(false);
    }
    REQUIRE_EQ(env.gate.counters().connections, std::uint64_t{0});
    REQUIRE(!env.gate.wait_for_stop_request(0ms) ||
            env.gate.wait_for_stop_request(0ms));
    require_intact(env.audit_directory);
}

// -- the transport under it --------------------------------------------------

TEST_CASE("control transport: three endpoints, three addresses, and no "
          "instance name turns one into another")
{
    using agent::local_endpoint;
    const auto plain = agent::local_address(local_endpoint::control, "");
    const auto admin = agent::local_address(local_endpoint::control_admin, "");
    const auto studio = agent::local_address(local_endpoint::agent, "");
    REQUIRE(plain);
    REQUIRE(admin);
    REQUIRE(studio);
    REQUIRE_NE(*plain, *admin);
    REQUIRE_NE(*plain, *studio);
    REQUIRE_NE(*admin, *studio);
    REQUIRE_EQ(*studio, *agent::local_address(""));

    // The names a caller could choose to try to reach another endpoint.
    for (const char* name : {"admin", "_admin", "control", "control_admin",
                             "agent", "-admin", "admin-", "control-admin"})
    {
        for (const local_endpoint endpoint :
             {local_endpoint::agent, local_endpoint::control,
              local_endpoint::control_admin})
        {
            const auto named = agent::local_address(endpoint, name);
            REQUIRE(named);
            REQUIRE_NE(*named, *plain);
            REQUIRE_NE(*named, *admin);
            REQUIRE_NE(*named, *studio);
        }
    }
    REQUIRE(!agent::local_address(local_endpoint::control, "a/b"));
    REQUIRE(!agent::local_address(local_endpoint::control_admin, ".."));
}

TEST_CASE("control transport: each end learns from the system who the other "
          "is")
{
    const scoped_instance instance;
    boost::asio::io_context io;
    auto listener = agent::listen_local(
        io.get_executor(), agent::local_endpoint::control, instance.name());
    REQUIRE(listener);

    std::unique_ptr<agent::local_stream> accepted;
    bool done = false;
    bool accept_failed = false;
    (*listener)->async_accept(
        [&](boost::system::error_code failed,
            std::unique_ptr<agent::local_stream> stream)
        {
            accept_failed = static_cast<bool>(failed);
            accepted = std::move(stream);
            done = true;
        });
    auto connected = agent::connect_local(
        io.get_executor(), agent::local_endpoint::control, instance.name());
    REQUIRE(connected);
    REQUIRE(run_until(io, [&] { return done; }, long_wait));
    REQUIRE(!accept_failed);
    REQUIRE(accepted != nullptr);

    // Before a byte has crossed: the answer is the kernel's, not the peer's.
    const auto client = accepted->peer();
    REQUIRE(client);
    const auto server = (*connected)->peer();
    REQUIRE(server);
    REQUIRE(client->same_user);
    REQUIRE(server->same_user);
    REQUIRE(!client->user.empty());
    REQUIRE_EQ(client->user, server->user);
    // Both ends are this process.
    REQUIRE(client->process_id != 0);
    REQUIRE_EQ(client->process_id, server->process_id);
    REQUIRE(!client->program.empty());
    REQUIRE_EQ(client->program, server->program);
}
