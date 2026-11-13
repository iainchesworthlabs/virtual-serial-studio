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
#include <vss/control/proto/secret.hpp>
#include <vss/control/proto/token.hpp>
#include <vss/control/token_store.hpp>
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
#include <iterator>
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
        wire_of_header(R"({"id":1,"op":"tokens.forge"})"),
        // Operations that exist, and that only administration may use.
        wire_of_header(R"({"id":1,"op":"tokens.list"})"),
        wire_of_header(
            R"({"id":1,"op":"tokens.create","label":"x","scopes":["monitor"],)"
            R"("life_s":0})"),
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
    REQUIRE(raw.send(wire_of_header(R"({"id":5,"op":"ports.rename"})")));
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

// -- tokens ------------------------------------------------------------------

namespace
{

/// A token made through the administration endpoint, as a person would.
control::token_created_reply make_token(fixture& env, control::scope_set scopes,
                                        std::uint64_t life_s = 0)
{
    auto admin = env.connect(true, "token maker");
    auto made =
        admin->create_token("test token", control::scope_names(scopes), life_s);
    REQUIRE(made);
    return std::move(*made);
}

/// Connects to the public endpoint as a token.
vss::result<std::unique_ptr<control::client>>
connect_with(fixture& env, const control::secret_string& token)
{
    control::client_options options;
    options.instance = env.instance.name();
    options.name = "token client";
    options.token = token;
    return control::client::connect(options);
}

vss::byte_buffer hello_with(const control::secret_string& token)
{
    control::hello_request greeting;
    greeting.client = "raw token";
    greeting.token = token;
    return wire_of(control::request{1, greeting});
}

/// The token with its last character changed: the right id, a wrong secret.
control::secret_string wrong_secret(const control::secret_string& token)
{
    std::string text(token.expose());
    text.back() = text.back() == 'a' ? 'b' : 'a';
    return control::secret_string(text);
}

/// The token with the first character of its id changed: no such token.
control::secret_string wrong_id(const control::secret_string& token)
{
    std::string text(token.expose());
    char& first = text[control::token_prefix.size() + 1];
    first = first == 'a' ? 'b' : 'a';
    return control::secret_string(text);
}

/// The bytes of every file in the gateway's directory.
std::string everything_kept(const std::filesystem::path& directory)
{
    std::string out;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        std::ifstream file(entry.path(), std::ios::binary);
        out.append(std::istreambuf_iterator<char>(file),
                   std::istreambuf_iterator<char>());
        out.push_back('\n');
    }
    return out;
}

bool holds(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

/// The time of a clock a test moves by hand.  The test moves it on its own
/// thread while the gateway reads it on another.
class moved_time
{
public:
    moved_time() : ticks_(control::wall_clock::now().time_since_epoch().count())
    {
    }

    moved_time& operator+=(control::wall_clock::duration step) noexcept
    {
        ticks_.fetch_add(step.count());
        return *this;
    }

    control::wall_clock::time_point get() const noexcept
    {
        return control::wall_clock::time_point(
            control::wall_clock::duration(ticks_.load()));
    }

private:
    std::atomic<control::wall_clock::rep> ticks_;
};

/// A clock a test moves by hand, for a gateway.
struct shared_time
{
    std::shared_ptr<moved_time> now = std::make_shared<moved_time>();

    control::token_store::time_source source() const
    {
        return [shared = now] { return shared->get(); };
    }
};

} // namespace

TEST_CASE("control gateway: a token is its own principal with its own scopes")
{
    fixture env;
    REQUIRE(env.backend->app.create_virtual_port(
        "tokened", vss::virtual_port_kind::loopback));
    const control::token_created_reply made =
        make_token(env, control::role_monitor());
    REQUIRE(control::parse_token(made.token).has_value());
    REQUIRE(made.entry.scopes == std::vector<std::string>{"monitor"});
    REQUIRE(made.entry.transports == std::vector<std::string>{"local"});
    REQUIRE(!made.entry.revoked);
    REQUIRE(made.entry.expires_ms > made.entry.created_ms);

    auto connected = connect_with(env, made.token);
    REQUIRE(connected);
    control::client& client = **connected;
    REQUIRE_EQ(client.gateway().principal, "token:" + made.entry.id);
    REQUIRE(client.gateway().scopes == std::vector<std::string>{"monitor"});
    const auto listed = client.list_ports();
    REQUIRE(listed);
    REQUIRE_EQ(listed->ports.size(), std::size_t{1});

    // A token that may record and nothing else is a principal that cannot
    // read the ports: its scopes are its own, not the local user's.
    const control::token_created_reply narrow =
        make_token(env, control::scope_set{control::scope::record});
    auto limited = connect_with(env, narrow.token);
    REQUIRE(limited);
    REQUIRE((*limited)->gateway().scopes == std::vector<std::string>{"record"});
    const auto refused = (*limited)->list_ports();
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), code(control::api_errc::forbidden));

    const auto opened =
        records_of(audit_records(env.audit_directory), "session.opened");
    bool seen = false;
    for (const json::object& record : opened)
    {
        if (text_of(record, "principal") == "token:" + made.entry.id)
            seen = true;
    }
    REQUIRE(seen);
}

TEST_CASE("control gateway: the endpoint's ceiling cuts a token down")
{
    control::gateway_options options;
    options.local_ceiling = control::role_monitor();
    fixture env(options);
    const control::token_created_reply made =
        make_token(env, control::role_control());
    auto connected = connect_with(env, made.token);
    REQUIRE(connected);
    REQUIRE((*connected)->gateway().scopes ==
            std::vector<std::string>{"monitor"});
}

TEST_CASE("control gateway: tokens are made, listed and revoked on the "
          "administration endpoint only")
{
    fixture env;
    const control::token_created_reply made =
        make_token(env, control::role_control_apply());

    // The local user on the public endpoint, and a token with every scope a
    // token can have.
    auto local = env.connect();
    auto tokened = connect_with(env, made.token);
    REQUIRE(tokened);
    for (control::client* client : {local.get(), tokened->get()})
    {
        const auto created = client->create_token("sneaky", {"monitor"});
        REQUIRE(!created);
        REQUIRE_EQ(created.error(), code(control::api_errc::forbidden));
        const auto listed = client->list_tokens();
        REQUIRE(!listed);
        REQUIRE_EQ(listed.error(), code(control::api_errc::forbidden));
        const auto revoked = client->revoke_token(made.entry.id);
        REQUIRE(!revoked);
        REQUIRE_EQ(revoked.error(), code(control::api_errc::forbidden));
    }
    REQUIRE_EQ(env.gate.counters().forbidden, std::uint64_t{6});

    // Nothing came of it: one token, still good.
    auto admin = env.connect(true);
    const auto listed = admin->list_tokens();
    REQUIRE(listed);
    REQUIRE_EQ(listed->tokens.size(), std::size_t{1});
    REQUIRE_EQ(listed->tokens[0].id, made.entry.id);
    REQUIRE(!listed->tokens[0].revoked);
    REQUIRE(listed->tokens[0].last_used_ms != 0);
    REQUIRE((*tokened)->status());
}

TEST_CASE("control gateway: no token holds admin, and a bad request makes no "
          "token")
{
    fixture env;
    auto admin = env.connect(true);
    const auto invalid = [&](const std::vector<std::string>& scopes,
                             std::uint64_t life_s, const std::string& label)
    {
        const auto made = admin->create_token(label, scopes, life_s);
        return !made &&
               made.error() == code(control::api_errc::invalid_argument);
    };
    REQUIRE(invalid({"admin"}, 0, "x"));
    REQUIRE(invalid({"monitor", "admin"}, 0, "x"));
    REQUIRE(invalid({"monitor", "root"}, 0, "x"));
    REQUIRE(invalid({}, 0, "x"));
    REQUIRE(invalid({"monitor"}, 366ULL * 24 * 3600, "x"));
    REQUIRE(invalid({"monitor"}, 0, std::string(200, 'x')));
    const auto listed = admin->list_tokens();
    REQUIRE(listed);
    REQUIRE(listed->tokens.empty());

    const auto missing = admin->revoke_token("aaaaaaaaaaaaa");
    REQUIRE(!missing);
    REQUIRE_EQ(missing.error(), code(control::api_errc::not_found));

    // Each attempt is in the audit log with how it ended.
    const auto operations =
        records_of(audit_records(env.audit_directory), "operation");
    REQUIRE_EQ(operations.size(), std::size_t{7});
    REQUIRE_EQ(text_of(operations[0], "operation"),
               std::string("tokens.create"));
    REQUIRE_EQ(text_of(operations[0], "outcome"),
               std::string("invalid_argument"));
    REQUIRE_EQ(text_of(operations[6], "outcome"), std::string("not_found"));
}

TEST_CASE("control gateway: every wrong token gets the same answer, and the "
          "audit log knows why")
{
    const shared_time time;
    control::gateway_options options;
    options.clock = time.source();
    options.audit_burst_period = 0ms;
    // Enough free failures that none of these is slowed down.
    options.auth_limits.free_failures = 100;
    options.auth_limits.global_per_second = 1000;
    fixture env(options);

    const control::token_created_reply good =
        make_token(env, control::role_monitor());
    const control::token_created_reply revoked =
        make_token(env, control::role_monitor());
    const control::token_created_reply ended =
        make_token(env, control::role_monitor(), 60);
    REQUIRE(env.connect(true)->revoke_token(revoked.entry.id));
    *time.now += std::chrono::seconds(120);

    struct attempt
    {
        control::secret_string token;
        bool admin;
        std::string reason;
    };
    const std::vector<attempt> attempts{
        {wrong_secret(good.token), false, "wrong_secret"},
        {wrong_id(good.token), false, "unknown"},
        {control::secret_string(std::string_view("not-a-token")), false,
         "malformed"},
        {control::secret_string(std::string(control::token_size, 'a')), false,
         "malformed"},
        {revoked.token, false, "revoked"},
        {ended.token, false, "expired"},
        // A good token, on the endpoint that takes none.
        {good.token, true, "token_on_admin_endpoint"}};

    const control::api_error expected =
        control::to_api_error(control::api_errc::unauthenticated);
    for (const attempt& tried : attempts)
    {
        raw_client raw(env.instance.name(), tried.admin);
        REQUIRE(raw.send(hello_with(tried.token)));
        const auto received = raw.read_frame();
        REQUIRE(received.has_value());
        // The reply does not hold what was presented.
        REQUIRE(!holds(received->header, tried.token.expose()));
        const auto answer = control::decode_reply(*received);
        REQUIRE(answer);
        REQUIRE_EQ(answer->id, std::uint64_t{1});
        // The same code, the same words, every time.
        REQUIRE(error_of(*answer) == expected);
        REQUIRE(raw.wait_closed());
    }
    REQUIRE_EQ(env.gate.counters().auth_failures, attempts.size());
    REQUIRE(wait_until([&] { return env.gate.counters().connections == 0; }));

    const auto failed =
        records_of(audit_records(env.audit_directory), "auth.failed");
    REQUIRE_EQ(failed.size(), attempts.size());
    for (std::size_t index = 0; index < attempts.size(); ++index)
    {
        REQUIRE_EQ(text_of(failed[index], "reason"), attempts[index].reason);
    }
    // The id of a token is public and is recorded; the rest of it is not.
    REQUIRE_EQ(text_of(failed[0], "token"), good.entry.id);
    REQUIRE_EQ(text_of(failed[2], "token"), std::string());

    // The good token still works where it belongs.
    REQUIRE(connect_with(env, good.token));
}

TEST_CASE("control gateway: wrong tokens slow down whoever presents them")
{
    control::gateway_options options;
    options.auth_limits.free_failures = 3;
    options.auth_limits.first_delay = 400ms;
    fixture env(options);
    const control::token_created_reply made =
        make_token(env, control::role_monitor());

    for (int index = 0; index < 3; ++index)
    {
        const auto refused = connect_with(env, wrong_secret(made.token));
        REQUIRE(!refused);
        REQUIRE_EQ(refused.error(), code(control::api_errc::unauthenticated));
    }
    // The third failure started a delay.  Until it has passed even the right
    // token is refused, and with the same answer.
    const auto slowed = connect_with(env, made.token);
    REQUIRE(!slowed);
    REQUIRE_EQ(slowed.error(), code(control::api_errc::unauthenticated));
    const auto failed =
        records_of(audit_records(env.audit_directory), "auth.failed");
    REQUIRE(!failed.empty());

    // A connection without a token is not part of this.
    REQUIRE(env.connect()->status());

    // Trying while slowed down is not a failure, so the delay ends.
    REQUIRE(
        wait_until([&] { return connect_with(env, made.token).has_value(); }));
}

TEST_CASE("control gateway: revoking a token closes its sessions at once")
{
    fixture env;
    const control::token_created_reply made =
        make_token(env, control::role_monitor());
    const control::token_created_reply other =
        make_token(env, control::role_monitor());

    raw_client session(env.instance.name());
    REQUIRE(session.send(hello_with(made.token)));
    const control::reply welcome = read_reply(session);
    REQUIRE(std::holds_alternative<control::hello_reply>(welcome.body));
    auto bystander = connect_with(env, other.token);
    REQUIRE(bystander);

    auto admin = env.connect(true);
    REQUIRE(admin->revoke_token(made.entry.id));
    // Without the session sending anything more.
    REQUIRE(session.wait_closed());

    const auto again = connect_with(env, made.token);
    REQUIRE(!again);
    REQUIRE_EQ(again.error(), code(control::api_errc::unauthenticated));
    // Another token's session is not touched.
    REQUIRE((*bystander)->status());

    const auto listed = admin->list_tokens();
    REQUIRE(listed);
    REQUIRE_EQ(listed->tokens.size(), std::size_t{2});
    REQUIRE(listed->tokens[0].revoked);
    REQUIRE(!listed->tokens[1].revoked);

    const auto operations =
        records_of(audit_records(env.audit_directory), "operation");
    REQUIRE_EQ(text_of(operations.back(), "operation"),
               std::string("tokens.revoke"));
    REQUIRE_EQ(text_of(operations.back(), "token"), made.entry.id);
    REQUIRE_EQ(number_of(operations.back(), "sessions_closed"),
               std::uint64_t{1});
}

TEST_CASE("control gateway: a token that ends, ends its session at the next "
          "request")
{
    const shared_time time;
    control::gateway_options options;
    options.clock = time.source();
    fixture env(options);
    const control::token_created_reply made =
        make_token(env, control::role_monitor(), 60);

    auto connected = connect_with(env, made.token);
    REQUIRE(connected);
    REQUIRE((*connected)->status());

    *time.now += std::chrono::seconds(61);
    const auto after = (*connected)->status();
    REQUIRE(!after);
    REQUIRE_EQ(after.error(), code(control::api_errc::unauthenticated));
    REQUIRE(wait_until([&] { return env.gate.counters().sessions == 0; }));

    const auto closed =
        records_of(audit_records(env.audit_directory), "session.closed");
    // The session of whoever made the token closes on its own time, so the
    // token's record is looked for by who it was, not by where it stands.
    const auto ended_record = std::ranges::find_if(
        closed, [](const json::object& record)
        { return text_of(record, "principal").rfind("token:", 0) == 0; });
    REQUIRE(ended_record != closed.end());
    REQUIRE_EQ(text_of(*ended_record, "reason"), std::string("token_expired"));
}

TEST_CASE("control gateway: tokens and revocations outlive the gateway")
{
    fixture env;
    const control::token_created_reply kept =
        make_token(env, control::role_monitor());
    const control::token_created_reply gone =
        make_token(env, control::role_monitor());
    REQUIRE(env.connect(true)->revoke_token(gone.entry.id));
    env.gate.stop();

    control::gateway next(env.prepared({}));
    REQUIRE(next.start());
    REQUIRE(connect_with(env, kept.token));
    const auto refused = connect_with(env, gone.token);
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), code(control::api_errc::unauthenticated));
}

TEST_CASE("control gateway: a gateway whose token file cannot be trusted "
          "does not start, and leaves the file alone")
{
    const scoped_instance instance;
    const vss_test_support::scratch_directory scratch;
    control::gateway_options options;
    options.instance = instance.name();
    options.audit_directory = scratch.path() / "audit";
    {
        control::gateway first(options);
        REQUIRE(first.start());
    }
    const std::filesystem::path file = options.audit_directory / "tokens.json";
    {
        std::ofstream damaged(file, std::ios::binary | std::ios::trunc);
        damaged << "{\"version\":1,\"tokens\":";
    }
    control::gateway second(options);
    REQUIRE(!second.start());
    std::ifstream kept(file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(kept),
                           std::istreambuf_iterator<char>()};
    REQUIRE_EQ(text, std::string("{\"version\":1,\"tokens\":"));
}

TEST_CASE("control gateway: no token is written anywhere the gateway keeps "
          "things")
{
    control::gateway_options options;
    options.audit_burst_period = 0ms;
    fixture env(options);
    // The whole life of a token: made, used, presented wrongly, used for
    // what it may not do, revoked, presented again.
    const control::token_created_reply made =
        make_token(env, control::role_monitor());
    const auto parts = control::parse_token(made.token);
    REQUIRE(parts.has_value());
    const control::secret_string wrong = wrong_secret(made.token);
    {
        auto connected = connect_with(env, made.token);
        REQUIRE(connected);
        REQUIRE((*connected)->status());
        REQUIRE(!(*connected)->stop());
        REQUIRE(!connect_with(env, wrong));
        REQUIRE(env.connect(true)->revoke_token(made.entry.id));
        REQUIRE(!connect_with(env, made.token));
    }
    env.gate.stop();

    const std::string kept = everything_kept(env.audit_directory);
    // The log did record all of it, under the token's public id.
    REQUIRE(holds(kept, "\"event\":\"auth.failed\""));
    REQUIRE(holds(kept, "\"event\":\"session.opened\""));
    REQUIRE(holds(kept, "tokens.revoke"));
    REQUIRE(holds(kept, made.entry.id));

    const auto hex = [](std::string_view text)
    {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string out;
        for (const char letter : text)
        {
            const auto value = static_cast<unsigned char>(letter);
            out.push_back(digits[static_cast<std::size_t>(value >> 4)]);
            out.push_back(digits[static_cast<std::size_t>(value & 0x0fu)]);
        }
        return out;
    };
    const auto upper = [](std::string_view text)
    {
        std::string out(text);
        for (char& letter : out)
        {
            if (letter >= 'a' && letter <= 'z')
                letter = static_cast<char>(letter - 'a' + 'A');
        }
        return out;
    };
    for (const control::secret_string* secret :
         {&parts->secret, &made.token, &wrong})
    {
        const std::string_view text = secret->expose();
        REQUIRE(!holds(kept, text));
        REQUIRE(!holds(kept, upper(text)));
        REQUIRE(!holds(kept, hex(text)));
        // Not even the half of it.
        REQUIRE(!holds(kept, text.substr(text.size() - 26)));
    }
    REQUIRE(!holds(kept, "vsst1_"));
    require_intact(env.audit_directory);
}

// -- streams -----------------------------------------------------------------

namespace
{

/// Ports behind the gateway and a way to move bytes through them: `a` sends
/// to `b` and `c` sends to `d`.  It talks to the agent itself, as Studio
/// would, so the gateway under test only ever watches.
struct traffic_rig
{
    explicit traffic_rig(const fixture& env)
    {
        agent::client_options options;
        options.instance = env.instance.name();
        options.name = "maker";
        auto connected = agent::client::connect(options);
        REQUIRE(connected);
        maker = std::move(*connected);
        a = make("a");
        b = make("b");
        c = make("c");
        d = make("d");
        REQUIRE(maker->connect_ports(a, b, true));
        REQUIRE(maker->connect_ports(c, d, true));
    }

    vss::endpoint_id make(const char* name) const
    {
        const auto made = maker->create_port(name);
        REQUIRE(made);
        return *made;
    }

    void send(vss::endpoint_id from, std::string_view text) const
    {
        REQUIRE(maker->write(from, vss::to_bytes(text)));
    }

    std::unique_ptr<agent::client> maker;
    vss::endpoint_id a = 0;
    vss::endpoint_id b = 0;
    vss::endpoint_id c = 0;
    vss::endpoint_id d = 0;
};

control::gateway_options with_local_scopes(control::scope_set scopes)
{
    control::gateway_options options;
    options.local_scopes = scopes;
    return options;
}

control::scope_set monitor_with_data()
{
    return {control::scope::monitor, control::scope::monitor_data};
}

control::stream_options every(std::uint32_t interval_ms,
                              std::vector<std::string> ports = {})
{
    control::stream_options options;
    options.interval_ms = interval_ms;
    options.ports = std::move(ports);
    return options;
}

bool is_rx_on(const control::event_entry& event, vss::endpoint_id port)
{
    return event.kind == "data" && event.direction == "rx" &&
           event.port == control::port_id_text(port);
}

/// Reads batches until `visit` says it has what it wants, or the time is up.
/// Every batch must follow on from the one before.
template <class Visitor>
bool read_batches(control::client& source, std::uint64_t& next_seq,
                  Visitor visit, std::chrono::milliseconds limit = long_wait)
{
    const auto deadline = steady::now() + limit;
    for (;;)
    {
        const auto now = steady::now();
        if (now >= deadline)
            return false;
        const auto batch = source.next_events(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  now));
        if (!batch)
            return false;
        REQUIRE_EQ(batch->seq, next_seq);
        ++next_seq;
        if (visit(*batch))
            return true;
    }
}

std::uint64_t steady_ns()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            steady::now().time_since_epoch())
            .count());
}

std::uint64_t wall_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace

TEST_CASE("control gateway: a monitor is told that bytes moved and is not "
          "given them")
{
    fixture env;
    const traffic_rig rig(env);
    auto watcher = env.connect(false, "watcher");
    const auto& features = watcher->gateway().features;
    REQUIRE(std::ranges::find(features, "events") != features.end());
    REQUIRE(std::ranges::find(features, "traffic") != features.end());

    const auto started = watcher->subscribe_events(every(20));
    REQUIRE(started);
    REQUIRE_EQ(started->interval_ms, std::uint32_t{20});
    REQUIRE_EQ(started->next_seq, std::uint64_t{1});
    REQUIRE(wait_until([&] { return env.gate.counters().streams == 1; }));

    const std::uint64_t before_ns = steady_ns();
    const std::uint64_t before_ms = wall_ms();
    rig.send(rig.a, "hello");

    std::optional<control::event_entry> seen;
    control::event_batch carrier;
    std::uint64_t next_seq = started->next_seq;
    REQUIRE(read_batches(*watcher, next_seq,
                         [&](const control::event_batch& batch)
                         {
                             for (const control::event_entry& event :
                                  batch.events)
                             {
                                 // Whatever the kind, an events stream has no
                                 // bytes in it.
                                 REQUIRE(event.data.empty());
                                 if (is_rx_on(event, rig.b))
                                 {
                                     seen = event;
                                     carrier = batch;
                                 }
                             }
                             return seen.has_value();
                         }));
    const std::uint64_t after_ns = steady_ns();
    REQUIRE(seen.has_value());
    REQUIRE_EQ(seen->size, std::uint64_t{5});
    REQUIRE_EQ(seen->name, std::string("b"));
    REQUIRE(seen->error.empty());
    // The time is the engine's, on the one clock of this computer, and the
    // batch says how that clock stood against the time of day.
    REQUIRE(seen->mono_ns >= before_ns);
    REQUIRE(seen->mono_ns <= after_ns);
    REQUIRE(carrier.mono_ns >= seen->mono_ns);
    REQUIRE(carrier.mono_ns <= after_ns);
    REQUIRE(carrier.wall_ms >= before_ms);
    REQUIRE(carrier.wall_ms <= wall_ms());
    REQUIRE_EQ(carrier.dropped, std::uint64_t{0});

    // The bytes need their own scope, and asking costs only an error.
    const auto bytes = watcher->subscribe_traffic(every(20));
    REQUIRE(!bytes);
    REQUIRE_EQ(bytes.error(), code(control::api_errc::forbidden));
    REQUIRE(watcher->status());
    REQUIRE(env.gate.counters().events_sent >= 1);
}

TEST_CASE("control gateway: the traffic stream carries the bytes, and only "
          "the traffic")
{
    fixture env(with_local_scopes(monitor_with_data()));
    const traffic_rig rig(env);
    auto tap = env.connect(false, "tap");
    const auto started = tap->subscribe_traffic(every(20));
    REQUIRE(started);

    rig.send(rig.a, "hello");
    // A port made now is an event, and not traffic.
    const vss::endpoint_id extra = rig.make("extra");
    rig.send(rig.c, std::string_view("\x00\xff\r\n", 4));

    std::string to_b;
    std::string to_d;
    std::uint64_t next_seq = started->next_seq;
    REQUIRE(read_batches(
        *tap, next_seq,
        [&](const control::event_batch& batch)
        {
            for (const control::event_entry& event : batch.events)
            {
                REQUIRE_EQ(event.kind, std::string("data"));
                REQUIRE(event.port != control::port_id_text(extra));
                REQUIRE_EQ(event.data.size(), event.size);
                const std::string text =
                    vss::to_string(vss::bytes_view(event.data));
                if (is_rx_on(event, rig.b))
                    to_b += text;
                if (is_rx_on(event, rig.d))
                    to_d += text;
            }
            return to_b.size() >= 5 && to_d.size() >= 4;
        }));
    REQUIRE_EQ(to_b, std::string("hello"));
    REQUIRE_EQ(to_d, std::string("\x00\xff\r\n", 4));
}

TEST_CASE("control gateway: a stream that names ports hears of no other")
{
    fixture env(with_local_scopes(monitor_with_data()));
    const traffic_rig rig(env);
    auto tap = env.connect(false, "tap");
    // Events of `d`, bytes of `b`: two lists, kept apart.
    const auto events =
        tap->subscribe_events(every(20, {control::port_id_text(rig.d)}));
    REQUIRE(events);
    const auto traffic = tap->subscribe_traffic(every(
        20, {control::port_id_text(rig.b), control::port_id_text(rig.b)}));
    REQUIRE(traffic);

    rig.send(rig.a, "for b");
    rig.send(rig.c, "for d");

    bool b_seen = false;
    bool d_seen = false;
    std::uint64_t next_seq = events->next_seq;
    REQUIRE(read_batches(
        *tap, next_seq,
        [&](const control::event_batch& batch)
        {
            for (const control::event_entry& event : batch.events)
            {
                const bool about_b = event.port == control::port_id_text(rig.b);
                const bool about_d = event.port == control::port_id_text(rig.d);
                // Nothing about `a`, `c` or no port at all.
                REQUIRE((about_b || about_d));
                if (about_b)
                {
                    // Only its traffic, with the bytes.
                    REQUIRE_EQ(event.kind, std::string("data"));
                    REQUIRE_EQ(event.data.size(), event.size);
                    b_seen = b_seen || is_rx_on(event, rig.b);
                }
                else
                {
                    // Its events, and no bytes: `d` is not in that list.
                    REQUIRE(event.data.empty());
                    d_seen = d_seen || is_rx_on(event, rig.d);
                }
            }
            return b_seen && d_seen;
        }));
}

TEST_CASE("control gateway: a port that is not one costs an error, not the "
          "session")
{
    fixture env;
    auto watcher = env.connect();
    for (const char* const bad : {"COM3", "p_", "p_-1", "", "p_1 ", "b"})
    {
        const auto refused = watcher->subscribe_events(every(20, {bad}));
        REQUIRE(!refused);
        REQUIRE_EQ(refused.error(), code(control::api_errc::invalid_argument));
    }
    REQUIRE_EQ(env.gate.counters().streams, std::uint64_t{0});
    REQUIRE(watcher->status());
    // A port that does not exist is not an error: it may be made later.
    REQUIRE(watcher->subscribe_events(every(20, {"p_999999"})));
}

TEST_CASE("control gateway: the pace of a stream is kept inside its limits")
{
    fixture env;
    auto watcher = env.connect();
    const auto fast = watcher->subscribe_events(every(0));
    REQUIRE(fast);
    REQUIRE_EQ(fast->interval_ms, control::min_stream_interval_ms);
    const auto slow = watcher->subscribe_events(
        every(std::numeric_limits<std::uint32_t>::max()));
    REQUIRE(slow);
    REQUIRE_EQ(slow->interval_ms, control::max_stream_interval_ms);
    const auto usual = watcher->subscribe_events();
    REQUIRE(usual);
    REQUIRE_EQ(usual->interval_ms, control::default_stream_interval_ms);
    REQUIRE_EQ(env.gate.counters().streams, std::uint64_t{1});
}

TEST_CASE("control gateway: a stream that is ended sends nothing more")
{
    fixture env;
    const traffic_rig rig(env);
    auto watcher = env.connect(false, "watcher");
    const auto started = watcher->subscribe_events(every(10));
    REQUIRE(started);
    rig.send(rig.a, "one");
    std::uint64_t next_seq = started->next_seq;
    REQUIRE(read_batches(*watcher, next_seq,
                         [&](const control::event_batch& batch)
                         {
                             return std::ranges::any_of(
                                 batch.events,
                                 [&](const control::event_entry& event)
                                 { return is_rx_on(event, rig.b); });
                         }));

    REQUIRE(watcher->unsubscribe_events());
    REQUIRE(wait_until([&] { return env.gate.counters().streams == 0; }));
    // Ending what is not there is not an error.
    REQUIRE(watcher->unsubscribe_events());

    // What was already on its way when the stream ended may still arrive.
    // After that there is nothing, whatever the ports do.
    while (watcher->next_events(100ms))
    {
    }
    const std::uint64_t sent = env.gate.counters().events_sent;
    rig.send(rig.a, "two");
    rig.send(rig.a, "three");
    const auto quiet = watcher->next_events(300ms);
    REQUIRE(!quiet);
    REQUIRE_EQ(quiet.error(), vss::make_error_code(vss::errc::timeout));
    REQUIRE_EQ(env.gate.counters().events_sent, sent);
    REQUIRE(watcher->status());

    // It can be started again, and goes on counting where it was.
    const auto again = watcher->subscribe_events(every(10));
    REQUIRE(again);
    REQUIRE(again->next_seq >= next_seq);
    next_seq = again->next_seq;
    rig.send(rig.a, "four");
    REQUIRE(read_batches(*watcher, next_seq,
                         [&](const control::event_batch& batch)
                         {
                             return std::ranges::any_of(
                                 batch.events,
                                 [&](const control::event_entry& event)
                                 { return is_rx_on(event, rig.b); });
                         }));

    watcher.reset();
    REQUIRE(wait_until([&] { return env.gate.counters().sessions == 0; }));
    const auto records = audit_records(env.audit_directory);
    const auto starts = records_of(records, "stream.started");
    const auto stops = records_of(records, "stream.stopped");
    REQUIRE_EQ(starts.size(), std::size_t{2});
    REQUIRE_EQ(stops.size(), std::size_t{1});
    REQUIRE_EQ(text_of(starts.front(), "stream"), std::string("events"));
    REQUIRE_EQ(number_of(starts.front(), "interval_ms"), std::uint64_t{10});
    REQUIRE_EQ(text_of(stops.front(), "reason"), std::string("unsubscribed"));
    const auto closed = records_of(records, "session.closed");
    REQUIRE(number_of(closed.back(), "events_sent") >= 2);
    require_intact(env.audit_directory);
}

TEST_CASE("control gateway: when the ports change a stream says so, with or "
          "without events")
{
    fixture env;
    const traffic_rig rig(env);
    auto watcher = env.connect();
    const auto before = watcher->list_ports();
    REQUIRE(before);
    // A stream that names a port nothing happens to.
    const auto started = watcher->subscribe_events(every(10, {"p_999999"}));
    REQUIRE(started);

    // Events of the ports the rig just made may still be on their way; they
    // are about other ports, so what arrives for them is a batch with no
    // events in it, which the reads below take in their stride.
    std::uint64_t next_seq = started->next_seq;
    std::uint64_t told = before->revision;
    rig.make("late");
    REQUIRE(read_batches(*watcher, next_seq,
                         [&](const control::event_batch& batch)
                         {
                             REQUIRE(batch.events.empty());
                             told = batch.revision;
                             return told != before->revision;
                         }));
    const auto after = watcher->list_ports();
    REQUIRE(after);
    REQUIRE_EQ(told, after->revision);
    REQUIRE_EQ(after->ports.size(), before->ports.size() + 1);
}

TEST_CASE("control gateway: a session that can only look has an agent "
          "connection that can only look")
{
    {
        fixture env;
        auto watcher = env.connect();
        REQUIRE(watcher->status());
        const agent::server_counters seen = env.backend->server.counters();
        REQUIRE_EQ(seen.clients, std::uint64_t{1});
        REQUIRE_EQ(seen.read_only_clients, std::uint64_t{1});
    }
    {
        // The bytes are still only looking.
        fixture env(with_local_scopes(monitor_with_data()));
        auto tap = env.connect();
        REQUIRE(tap->subscribe_traffic(every(20)));
        REQUIRE_EQ(env.backend->server.counters().read_only_clients,
                   std::uint64_t{1});
    }
    {
        fixture env(with_local_scopes(control::role_control()));
        auto driver = env.connect();
        REQUIRE(driver->status());
        const agent::server_counters seen = env.backend->server.counters();
        REQUIRE_EQ(seen.clients, std::uint64_t{1});
        REQUIRE_EQ(seen.read_only_clients, std::uint64_t{0});
    }
    {
        fixture env;
        auto admin = env.connect(true);
        REQUIRE(admin->status());
        REQUIRE_EQ(env.backend->server.counters().read_only_clients,
                   std::uint64_t{0});
    }
}

TEST_CASE("control gateway: a subscriber that does not read loses events, is "
          "told how many and keeps its session")
{
    fixture env(with_local_scopes(monitor_with_data()));
    const traffic_rig rig(env);
    raw_client raw(env.instance.name());
    say_hello(raw, "sleeper");
    REQUIRE(raw.send(wire_of(
        control::request{2, control::traffic_subscribe_request{every(10)}})));
    const control::reply subscribed = read_reply(raw);
    const auto* started =
        std::get_if<control::subscribed_reply>(&subscribed.body);
    REQUIRE(started != nullptr);

    // It reads nothing while the ports are busy, until the gateway has had
    // to let events go.
    const std::string chunk(1024, 'x');
    const auto deadline = steady::now() + long_wait;
    while (env.gate.counters().events_dropped == 0 && steady::now() < deadline)
        rig.send(rig.a, chunk);
    REQUIRE(env.gate.counters().events_dropped > 0);
    // Batches never take more than their share of what may wait for one
    // client, so a busy port does not close whoever watches it.
    REQUIRE_EQ(env.gate.counters().slow_clients_closed, std::uint64_t{0});
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{1});

    // Now it reads.  The batches are numbered without a gap, and one of them
    // says that events were lost.
    std::uint64_t next_seq = started->next_seq;
    std::uint64_t lost = 0;
    while (lost == 0)
    {
        const auto received = raw.read_frame();
        REQUIRE(received.has_value());
        const auto message = control::decode_server_message(*received);
        REQUIRE(message);
        const auto* batch = std::get_if<control::event_batch>(&*message);
        REQUIRE(batch != nullptr);
        REQUIRE_EQ(batch->seq, next_seq);
        ++next_seq;
        lost += batch->dropped;
    }
    REQUIRE(lost > 0);

    // The session is as good as it was.
    REQUIRE(raw.send(wire_of(control::request{3, control::status_request{}})));
    for (;;)
    {
        const auto received = raw.read_frame();
        REQUIRE(received.has_value());
        const auto message = control::decode_server_message(*received);
        REQUIRE(message);
        if (const auto* answer = std::get_if<control::reply>(&*message))
        {
            REQUIRE_EQ(answer->id, std::uint64_t{3});
            REQUIRE(
                std::holds_alternative<control::status_reply>(answer->body));
            break;
        }
    }
}

TEST_CASE("control gateway: a stream has an allowance of bytes, and what is "
          "over it is dropped and told")
{
    control::gateway_options options = with_local_scopes(monitor_with_data());
    options.max_event_bytes_per_second = 4000;
    fixture env(options);
    const traffic_rig rig(env);
    auto tap = env.connect(false, "tap");
    const auto started = tap->subscribe_traffic(every(10));
    REQUIRE(started);

    // Much more than a second's allowance, at once.
    const std::string chunk(1024, 'y');
    for (int index = 0; index < 64; ++index)
        rig.send(rig.a, chunk);

    std::uint64_t next_seq = started->next_seq;
    std::uint64_t lost = 0;
    std::size_t bytes = 0;
    REQUIRE(read_batches(*tap, next_seq,
                         [&](const control::event_batch& batch)
                         {
                             lost += batch.dropped;
                             for (const control::event_entry& event :
                                  batch.events)
                                 bytes += event.data.size();
                             return lost > 0;
                         }));
    // The reader was never behind: it was the allowance that ran out.
    REQUIRE(lost > 0);
    REQUIRE(bytes < std::size_t{64} * 1024);
    REQUIRE(env.gate.counters().events_dropped >= lost);
    REQUIRE(tap->status());
}

TEST_CASE("control gateway: when the agent goes away a stream's session is "
          "ended and told why")
{
    fixture env;
    auto watcher = env.connect(false, "watcher");
    auto other = env.connect(false, "other");
    REQUIRE(watcher->subscribe_events(every(10)));
    REQUIRE(other->status());

    env.stop_agent();
    std::optional<vss::error_code> ended;
    const auto deadline = steady::now() + long_wait;
    while (!ended && steady::now() < deadline)
    {
        const auto batch = watcher->next_events(long_wait);
        if (!batch)
            ended = batch.error();
    }
    REQUIRE(ended == code(control::api_errc::unavailable));
    REQUIRE(wait_until([&] { return env.gate.counters().sessions == 1; }));
    REQUIRE_EQ(env.gate.counters().agent_lost, std::uint64_t{1});
    REQUIRE_EQ(env.gate.counters().streams, std::uint64_t{0});

    // A session without a stream is told when it asks, and stays.
    const auto refused = other->subscribe_events(every(10));
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), code(control::api_errc::unavailable));
    const auto state = other->status();
    REQUIRE(state);
    REQUIRE(!state->agent_running);

    // And when the agent is back, so is the stream for whoever asks.
    env.start_agent();
    REQUIRE(other->subscribe_events(every(10)));

    const auto closed =
        records_of(audit_records(env.audit_directory), "session.closed");
    REQUIRE_EQ(closed.size(), std::size_t{1});
    REQUIRE_EQ(text_of(closed.front(), "reason"), std::string("agent_lost"));
}

TEST_CASE("control gateway: a token that ends, ends its stream without "
          "waiting for a request")
{
    const shared_time time;
    control::gateway_options options;
    options.clock = time.source();
    fixture env(options);
    const control::token_created_reply made =
        make_token(env, control::role_monitor(), 60);
    auto connected = connect_with(env, made.token);
    REQUIRE(connected);
    auto& watcher = **connected;
    REQUIRE(watcher.subscribe_events(every(10)));

    // It sends nothing from here on: only the stream is running.
    *time.now += std::chrono::seconds(61);
    std::optional<vss::error_code> ended;
    const auto deadline = steady::now() + long_wait;
    while (!ended && steady::now() < deadline)
    {
        const auto batch = watcher.next_events(long_wait);
        if (!batch)
            ended = batch.error();
    }
    REQUIRE(ended == code(control::api_errc::unauthenticated));
    REQUIRE(wait_until([&] { return env.gate.counters().sessions == 0; }));
    const auto closed =
        records_of(audit_records(env.audit_directory), "session.closed");
    // The session of whoever made the token closes on its own time, so the
    // token's record is looked for by who it was, not by where it stands.
    const auto ended_record = std::ranges::find_if(
        closed, [](const json::object& record)
        { return text_of(record, "principal").rfind("token:", 0) == 0; });
    REQUIRE(ended_record != closed.end());
    REQUIRE_EQ(text_of(*ended_record, "reason"), std::string("token_expired"));
}

TEST_CASE("control gateway: revoking a token ends its stream")
{
    control::gateway_options options;
    options.local_ceiling = control::every_scope();
    fixture env(options);
    const traffic_rig rig(env);
    const control::token_created_reply made =
        make_token(env, monitor_with_data());
    auto connected = connect_with(env, made.token);
    REQUIRE(connected);
    auto& tap = **connected;
    const auto started = tap.subscribe_traffic(every(10));
    REQUIRE(started);
    rig.send(rig.a, "seen");
    std::uint64_t next_seq = started->next_seq;
    REQUIRE(read_batches(tap, next_seq,
                         [&](const control::event_batch& batch)
                         {
                             return std::ranges::any_of(
                                 batch.events,
                                 [&](const control::event_entry& event)
                                 { return is_rx_on(event, rig.b); });
                         }));

    auto admin = env.connect(true);
    REQUIRE(admin->revoke_token(made.entry.id));
    REQUIRE(wait_until([&] { return env.gate.counters().streams == 0; }));
    rig.send(rig.a, "unseen");
    std::string after;
    for (;;)
    {
        const auto batch = tap.next_events(long_wait);
        if (!batch)
            break;
        for (const control::event_entry& event : batch->events)
            after += vss::to_string(vss::bytes_view(event.data));
    }
    REQUIRE(!holds(after, "unseen"));
}

TEST_CASE("control gateway: the administration endpoint sees who is "
          "connected and can close a session")
{
    fixture env;
    auto admin = env.connect(true, "admin tool");
    auto watcher = env.connect(false, "watcher");
    auto idle = env.connect(false, "idle");
    REQUIRE(watcher->subscribe_events(every(20)));
    REQUIRE(watcher->status());
    const std::uint64_t now_ms = wall_ms();

    const auto listed = admin->list_sessions();
    REQUIRE(listed);
    REQUIRE_EQ(listed->sessions.size(), std::size_t{3});
    const auto find =
        [&](std::string_view name) -> const control::session_entry&
    {
        const auto found = std::ranges::find(listed->sessions, name,
                                             &control::session_entry::client);
        REQUIRE(found != listed->sessions.end());
        return *found;
    };
    const control::session_entry& watching = find("watcher");
    REQUIRE_EQ(watching.principal.rfind("local:", 0), std::size_t{0});
    REQUIRE_EQ(watching.transport, std::string("local"));
    REQUIRE(watching.events);
    REQUIRE(!watching.traffic);
    REQUIRE_EQ(watching.requests, std::uint64_t{2});
    REQUIRE(watching.process != 0);
    REQUIRE(watching.opened_ms <= now_ms);
    REQUIRE(watching.opened_ms + 60000 > now_ms);
    REQUIRE(!find("idle").events);
    REQUIRE_EQ(find("idle").requests, std::uint64_t{0});
    REQUIRE_EQ(find("admin tool").transport, std::string("local-admin"));
    // In the order they came.
    REQUIRE(std::ranges::is_sorted(listed->sessions, {},
                                   &control::session_entry::number));

    // No scope reaches either operation from the public endpoint.
    const auto peeked = watcher->list_sessions();
    REQUIRE(!peeked);
    REQUIRE_EQ(peeked.error(), code(control::api_errc::forbidden));
    const auto shoved = idle->kick_session(watching.number);
    REQUIRE(!shoved);
    REQUIRE_EQ(shoved.error(), code(control::api_errc::forbidden));

    const auto missing = admin->kick_session(999999);
    REQUIRE(!missing);
    REQUIRE_EQ(missing.error(), code(control::api_errc::not_found));
    REQUIRE(admin->kick_session(watching.number));
    REQUIRE(wait_until([&] { return env.gate.counters().sessions == 2; }));
    REQUIRE_EQ(env.gate.counters().streams, std::uint64_t{0});
    REQUIRE(!watcher->status());
    REQUIRE(idle->status());
    const auto fewer = admin->list_sessions();
    REQUIRE(fewer);
    REQUIRE_EQ(fewer->sessions.size(), std::size_t{2});

    const auto records = audit_records(env.audit_directory);
    std::vector<json::object> kicks;
    for (const json::object& record : records_of(records, "operation"))
    {
        if (text_of(record, "operation") == "sessions.kick")
            kicks.push_back(record);
    }
    REQUIRE_EQ(kicks.size(), std::size_t{2});
    REQUIRE_EQ(text_of(kicks.front(), "outcome"), std::string("not_found"));
    REQUIRE_EQ(text_of(kicks.back(), "outcome"), std::string("ok"));
    REQUIRE_EQ(number_of(kicks.back(), "target"), watching.number);
    const auto closed = records_of(records, "session.closed");
    REQUIRE_EQ(text_of(closed.back(), "reason"), std::string("kicked"));
    require_intact(env.audit_directory);
}

TEST_CASE("control gateway: stress with busy streams and a stop underneath")
{
    fixture env(with_local_scopes(monitor_with_data()));
    const traffic_rig rig(env);
    std::atomic<bool> finished{false};
    std::atomic<std::uint64_t> batches{0};
    {
        std::vector<joining_thread> readers;
        readers.reserve(4);
        for (int index = 0; index < 4; ++index)
        {
            readers.emplace_back(
                [&env, &finished, &batches, index]
                {
                    control::client_options options;
                    options.instance = env.instance.name();
                    options.name = "reader";
                    auto connected = control::client::connect(options);
                    if (!connected)
                        return;
                    auto& reader = **connected;
                    // Two take the bytes, two do not, and one of each keeps
                    // asking for other things meanwhile.
                    if (index % 2 == 0)
                        (void)reader.subscribe_traffic(every(10));
                    else
                        (void)reader.subscribe_events(every(10));
                    while (!finished.load())
                    {
                        const auto batch = reader.next_events(20ms);
                        if (batch)
                            batches.fetch_add(1);
                        else if (batch.error() !=
                                 vss::make_error_code(vss::errc::timeout))
                            return;
                        if (index < 2)
                            (void)reader.list_ports();
                    }
                });
        }
        const std::string chunk(512, 'z');
        const auto until = steady::now() + 400ms;
        while (steady::now() < until)
            rig.send(rig.a, chunk);
        REQUIRE(wait_until([&] { return batches.load() > 0; }));
        // The gateway goes while they are all at it.
        env.gate.stop();
        finished.store(true);
    }
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{0});
    REQUIRE_EQ(env.gate.counters().connections, std::uint64_t{0});
}

// -- what changes the ports --------------------------------------------------

namespace
{

vss::error_code engine(vss::errc value)
{
    return vss::make_error_code(value);
}

/// The audit records of one operation, oldest first.
std::vector<json::object> operations_named(const fixture& env,
                                           std::string_view name)
{
    std::vector<json::object> out;
    for (const json::object& record :
         records_of(audit_records(env.audit_directory), "operation"))
    {
        if (text_of(record, "operation") == name)
            out.push_back(record);
    }
    return out;
}

} // namespace

TEST_CASE("control gateway: a controller makes ports, links them and sends "
          "bytes through")
{
    fixture env(with_local_scopes(control::role_control()));
    auto driver = env.connect(false, "driver");
    const auto& features = driver->gateway().features;
    REQUIRE(std::ranges::find(features, "ports.write") != features.end());
    REQUIRE(std::ranges::find(features, "ports.send") != features.end());

    const auto left = driver->create_port("left");
    const auto right = driver->create_port("right", "loopback");
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(control::parse_port_id(*left).has_value());
    REQUIRE(*left != *right);
    const auto link = driver->connect_ports(*left, *right, true);
    REQUIRE(link);
    REQUIRE_EQ(link->rfind("l_", 0), std::size_t{0});

    const auto listed = driver->list_ports();
    REQUIRE(listed);
    REQUIRE_EQ(listed->ports.size(), std::size_t{2});
    REQUIRE_EQ(listed->links.size(), std::size_t{1});
    REQUIRE_EQ(listed->links.front().id, *link);
    REQUIRE_EQ(listed->links.front().sources, std::vector<std::string>{*left});
    REQUIRE_EQ(listed->links.front().targets, std::vector<std::string>{*right});

    // Another session watches the bytes arrive.
    auto tap = env.connect(false, "tap");
    const auto started = tap->subscribe_traffic(every(10, {*right}));
    REQUIRE(started);
    const std::string sent("\x00\xffhello\r\n", 9);
    REQUIRE(driver->send(*left, vss::to_bytes(sent)));
    std::string seen;
    std::uint64_t next_seq = started->next_seq;
    REQUIRE(read_batches(
        *tap, next_seq,
        [&](const control::event_batch& batch)
        {
            for (const control::event_entry& event : batch.events)
            {
                if (event.direction == "rx")
                    seen += vss::to_string(vss::bytes_view(event.data));
            }
            return seen.size() >= sent.size();
        }));
    REQUIRE_EQ(seen, sent);

    REQUIRE(driver->remove_port(*left));
    const auto fewer = driver->list_ports();
    REQUIRE(fewer);
    REQUIRE_EQ(fewer->ports.size(), std::size_t{1});
    REQUIRE_EQ(fewer->ports.front().id, *right);

    // A session that may change things is not held to looking at the agent.
    REQUIRE_EQ(env.backend->server.counters().read_only_refused,
               std::uint64_t{0});
}

TEST_CASE("control gateway: a monitor can change nothing, and the bytes have "
          "a scope of their own")
{
    {
        fixture env(with_local_scopes(monitor_with_data()));
        const traffic_rig rig(env);
        auto watcher = env.connect();
        const std::string a = control::port_id_text(rig.a);
        const std::string b = control::port_id_text(rig.b);
        const auto before = watcher->list_ports();
        REQUIRE(before);

        const auto made = watcher->create_port("mine");
        REQUIRE(!made);
        REQUIRE_EQ(made.error(), code(control::api_errc::forbidden));
        const auto removed = watcher->remove_port(a);
        REQUIRE(!removed);
        REQUIRE_EQ(removed.error(), code(control::api_errc::forbidden));
        const auto linked = watcher->connect_ports(b, a, false);
        REQUIRE(!linked);
        REQUIRE_EQ(linked.error(), code(control::api_errc::forbidden));
        const auto written = watcher->send(a, vss::to_bytes("x"));
        REQUIRE(!written);
        REQUIRE_EQ(written.error(), code(control::api_errc::forbidden));

        const auto after = watcher->list_ports();
        REQUIRE(after);
        REQUIRE(*after == *before);
        REQUIRE_EQ(env.gate.counters().forbidden, std::uint64_t{4});
        // Refused by the gateway: the agent was never asked.
        REQUIRE_EQ(env.backend->server.counters().read_only_refused,
                   std::uint64_t{0});
    }
    {
        // May arrange ports, may not speak on them.
        fixture env(with_local_scopes(
            {control::scope::monitor, control::scope::control_ports}));
        auto arranger = env.connect();
        const auto port = arranger->create_port("quiet");
        REQUIRE(port);
        const auto written = arranger->send(*port, vss::to_bytes("x"));
        REQUIRE(!written);
        REQUIRE_EQ(written.error(), code(control::api_errc::forbidden));
    }
    {
        // May speak, may not arrange.
        fixture env(with_local_scopes(
            {control::scope::monitor, control::scope::control_data}));
        const traffic_rig rig(env);
        auto speaker = env.connect();
        REQUIRE(
            speaker->send(control::port_id_text(rig.a), vss::to_bytes("x")));
        const auto made = speaker->create_port("no");
        REQUIRE(!made);
        REQUIRE_EQ(made.error(), code(control::api_errc::forbidden));
    }
}

TEST_CASE("control gateway: what the engine refuses comes back by name and "
          "costs only an error")
{
    fixture env(with_local_scopes(control::role_control()));
    auto driver = env.connect();
    const auto port = driver->create_port("only");
    REQUIRE(port);

    const auto gone = driver->remove_port("p_999999");
    REQUIRE(!gone);
    REQUIRE_EQ(gone.error(), engine(vss::errc::not_found));
    const auto nowhere = driver->send("p_999999", vss::to_bytes("x"));
    REQUIRE(!nowhere);
    REQUIRE_EQ(nowhere.error(), engine(vss::errc::not_found));
    const auto half = driver->connect_ports(*port, "p_999999", false);
    REQUIRE(!half);
    REQUIRE_EQ(half.error(), engine(vss::errc::not_found));
    const auto odd = driver->create_port("x", "no-such-kind");
    REQUIRE(!odd);
    REQUIRE_EQ(odd.error(), engine(vss::errc::unsupported));
    // The message is the gateway's: nothing of the engine's wording crosses.
    REQUIRE_EQ(driver->last_error_message(),
               std::string("the engine refused the request"));

    // An id that is not one is the caller's mistake, and is not sent on.
    for (const char* const bad : {"COM3", "p_", "left", "", "p_-1"})
    {
        const auto removed = driver->remove_port(bad);
        REQUIRE(!removed);
        REQUIRE_EQ(removed.error(), code(control::api_errc::invalid_argument));
        const auto written = driver->send(bad, vss::to_bytes("x"));
        REQUIRE(!written);
        REQUIRE_EQ(written.error(), code(control::api_errc::invalid_argument));
        const auto linked = driver->connect_ports(*port, bad, false);
        REQUIRE(!linked);
        REQUIRE_EQ(linked.error(), code(control::api_errc::invalid_argument));
    }
    // Nothing to send, and too much.
    const auto nothing = driver->send(*port, vss::bytes_view());
    REQUIRE(!nothing);
    REQUIRE_EQ(nothing.error(), code(control::api_errc::invalid_argument));
    const vss::byte_buffer most(control::max_send_size, vss::byte{0x41});
    REQUIRE(driver->send(*port, vss::bytes_view(most)));
    const vss::byte_buffer over(control::max_send_size + 1, vss::byte{0x41});
    const auto too_much = driver->send(*port, vss::bytes_view(over));
    REQUIRE(!too_much);
    REQUIRE_EQ(too_much.error(), code(control::api_errc::limit_exceeded));

    // After all of it the session is as good as it was.
    const auto listed = driver->list_ports();
    REQUIRE(listed);
    REQUIRE_EQ(listed->ports.size(), std::size_t{1});
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{1});
}

TEST_CASE("control gateway: without an agent a change says so, and nothing "
          "is half done")
{
    fixture env(with_local_scopes(control::role_control()), false);
    auto driver = env.connect();
    const auto made = driver->create_port("early");
    REQUIRE(!made);
    REQUIRE_EQ(made.error(), code(control::api_errc::unavailable));
    const auto written = driver->send("p_1", vss::to_bytes("x"));
    REQUIRE(!written);
    REQUIRE_EQ(written.error(), code(control::api_errc::unavailable));

    env.start_agent();
    const auto later = driver->create_port("later");
    REQUIRE(later);
    const auto listed = driver->list_ports();
    REQUIRE(listed);
    REQUIRE_EQ(listed->ports.size(), std::size_t{1});
}

TEST_CASE("control gateway: every change is in the audit log, and the bytes "
          "of a send are not")
{
    fixture env(with_local_scopes(control::role_control()));
    auto driver = env.connect(false, "driver");
    const auto left = driver->create_port("left");
    const auto right = driver->create_port("right");
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(driver->connect_ports(*left, *right, false));
    const std::string canary = "CANARY-7f3a9c-do-not-log";
    REQUIRE(driver->send(*left, vss::to_bytes(canary)));
    REQUIRE(!driver->remove_port("p_999999"));
    REQUIRE(!driver->remove_port("not a port\n{\"event\":\"forged\"}"));
    REQUIRE(driver->remove_port(*left));
    driver.reset();
    REQUIRE(wait_until([&] { return env.gate.counters().sessions == 0; }));

    const auto creates = operations_named(env, "ports.create");
    REQUIRE_EQ(creates.size(), std::size_t{2});
    REQUIRE_EQ(text_of(creates.front(), "name"), std::string("left"));
    REQUIRE_EQ(text_of(creates.front(), "port"), *left);
    REQUIRE_EQ(text_of(creates.front(), "outcome"), std::string("ok"));
    REQUIRE_EQ(text_of(creates.front(), "principal").rfind("local:", 0),
               std::size_t{0});

    const auto links = operations_named(env, "links.connect");
    REQUIRE_EQ(links.size(), std::size_t{1});
    REQUIRE_EQ(text_of(links.front(), "first"), *left);
    REQUIRE_EQ(text_of(links.front(), "second"), *right);
    REQUIRE_EQ(text_of(links.front(), "one_way"), std::string("no"));

    const auto sends = operations_named(env, "ports.send");
    REQUIRE_EQ(sends.size(), std::size_t{1});
    REQUIRE_EQ(text_of(sends.front(), "port"), *left);
    REQUIRE_EQ(number_of(sends.front(), "bytes"), canary.size());

    const auto removes = operations_named(env, "ports.remove");
    REQUIRE_EQ(removes.size(), std::size_t{3});
    REQUIRE_EQ(text_of(removes[0], "outcome"), std::string("engine.not_found"));
    // An id that was not one is not written down as it came.
    REQUIRE_EQ(text_of(removes[1], "outcome"), std::string("invalid_argument"));
    REQUIRE(text_of(removes[1], "port").empty());
    REQUIRE_EQ(text_of(removes[2], "outcome"), std::string("ok"));

    // The bytes that were sent are nowhere the gateway keeps anything.
    REQUIRE(!holds(everything_kept(env.audit_directory), canary));
    REQUIRE(!holds(everything_kept(env.audit_directory), "forged"));
    require_intact(env.audit_directory);
}

TEST_CASE("control gateway: a token is held to its scopes when it changes "
          "ports")
{
    fixture env;
    const control::token_created_reply arranger = make_token(
        env, {control::scope::monitor, control::scope::control_ports});
    auto connected = connect_with(env, arranger.token);
    REQUIRE(connected);
    auto& client = **connected;
    const auto port = client.create_port("by token");
    REQUIRE(port);
    const auto written = client.send(*port, vss::to_bytes("x"));
    REQUIRE(!written);
    REQUIRE_EQ(written.error(), code(control::api_errc::forbidden));

    // The record names the token, not the local user.
    const auto creates = operations_named(env, "ports.create");
    REQUIRE_EQ(creates.size(), std::size_t{1});
    REQUIRE_EQ(text_of(creates.front(), "principal"),
               "token:" + arranger.entry.id);

    // And a ceiling takes the scope away again.
    control::gateway_options capped;
    capped.local_ceiling = control::role_monitor();
    fixture strict(capped);
    const control::token_created_reply same = make_token(
        strict, {control::scope::monitor, control::scope::control_ports});
    auto held = connect_with(strict, same.token);
    REQUIRE(held);
    const auto refused = (*held)->create_port("no");
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), code(control::api_errc::forbidden));
}

// -- waiting for bytes -------------------------------------------------------

namespace
{

control::expect_bytes_request expecting(const std::string& port,
                                        std::string_view pattern,
                                        std::uint32_t timeout_ms = 20000)
{
    control::expect_bytes_request out;
    out.port = port;
    out.pattern = vss::to_bytes(pattern);
    out.timeout_ms = timeout_ms;
    return out;
}

/// Two linked ports made through the gateway: what is sent to `left`
/// arrives at `right`.
struct made_pair
{
    explicit made_pair(control::client& driver)
    {
        const auto first = driver.create_port("left");
        const auto second = driver.create_port("right");
        REQUIRE(first);
        REQUIRE(second);
        left = *first;
        right = *second;
        REQUIRE(driver.connect_ports(left, right, true));
    }

    std::string left;
    std::string right;
};

} // namespace

TEST_CASE("control gateway: bytes that are sent are waited for in the same "
          "request, and found")
{
    fixture env(with_local_scopes(control::role_control()));
    auto driver = env.connect(false, "harness");
    const made_pair pair(*driver);
    const auto& features = driver->gateway().features;
    REQUIRE(std::ranges::find(features, "expect.bytes") != features.end());

    const std::uint64_t before = steady_ns();
    control::expect_bytes_request wanted = expecting(pair.right, "OK\r\n");
    wanted.send_port = pair.left;
    wanted.send = vss::to_bytes("noise OK\r\n and more");
    const auto found = driver->expect_bytes(wanted);
    REQUIRE(found);
    // Six bytes went by before the pattern began.
    REQUIRE_EQ(found->offset, std::uint64_t{6});
    REQUIRE_EQ(found->events_lost, std::uint64_t{0});
    REQUIRE(found->mono_ns >= before);
    REQUIRE(found->mono_ns <= steady_ns());

    // The session is as it was: it can ask again, and it has no stream.
    REQUIRE_EQ(env.gate.counters().streams, std::uint64_t{0});
    wanted.send = vss::to_bytes("OK\r\n");
    const auto again = driver->expect_bytes(wanted);
    REQUIRE(again);
    REQUIRE_EQ(again->offset, std::uint64_t{0});
    REQUIRE(driver->list_ports());

    // Binary patterns, and a pattern as long as one may be.
    std::string longest(control::max_pattern_size, '\0');
    for (std::size_t index = 0; index < longest.size(); ++index)
        longest[index] = static_cast<char>(index & 0xffu);
    control::expect_bytes_request binary = expecting(pair.right, longest);
    binary.send_port = pair.left;
    binary.send = vss::to_bytes(std::string("\xff\xfe", 2) + longest);
    const auto exact = driver->expect_bytes(binary);
    REQUIRE(exact);
    REQUIRE_EQ(exact->offset, std::uint64_t{2});
}

TEST_CASE("control gateway: a pattern that arrives in pieces is found")
{
    fixture env(with_local_scopes(control::role_control()));
    auto harness = env.connect(false, "harness");
    auto other = env.connect(false, "sender");
    const made_pair pair(*harness);

    // Another session sends the pattern a byte at a time while this one
    // waits: every chunk boundary falls inside the pattern.
    std::atomic<bool> waiting{false};
    vss::result<control::matched_reply> found =
        code(control::api_errc::internal);
    {
        const joining_thread waiter(
            [&]
            {
                waiting.store(true);
                found = harness->expect_bytes(expecting(pair.right, "READY"));
            });
        REQUIRE(wait_until([&] { return waiting.load(); }));
        // The wait is armed when the gateway has it in the audit log's
        // sight: until then nothing sent would be seen, so the sender
        // repeats the whole pattern until the waiter has its answer.
        const auto deadline = steady::now() + long_wait;
        std::size_t round = 0;
        while (env.gate.counters().sessions == 2 && steady::now() < deadline)
        {
            const std::string piece(1, "xREADY"[round % 6]);
            REQUIRE(other->send(pair.left, vss::to_bytes(piece)));
            ++round;
            if (round % 6 == 0 &&
                !operations_named(env, "expect.bytes").empty())
                break;
        }
    }
    REQUIRE(found);
    // It began at a byte that follows an `x`: every sixth byte is one.
    REQUIRE_EQ(found->offset % 6, std::uint64_t{1});
}

TEST_CASE("control gateway: a pattern that does not come is a timeout, and "
          "the session stays")
{
    fixture env(with_local_scopes(control::role_control()));
    auto driver = env.connect();
    const made_pair pair(*driver);

    control::expect_bytes_request wanted = expecting(pair.right, "NEVER", 150);
    wanted.send_port = pair.left;
    wanted.send = vss::to_bytes("something else");
    const auto started = steady::now();
    const auto missed = driver->expect_bytes(wanted);
    REQUIRE(!missed);
    REQUIRE_EQ(missed.error(), code(control::api_errc::timeout));
    REQUIRE(steady::now() - started >= 150ms);

    REQUIRE(driver->status());
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{1});
    const auto records = operations_named(env, "expect.bytes");
    REQUIRE_EQ(records.size(), std::size_t{1});
    REQUIRE_EQ(text_of(records[0], "outcome"), std::string("timeout"));
    REQUIRE_EQ(number_of(records[0], "sent_bytes"), std::uint64_t{14});
}

TEST_CASE("control gateway: waiting for bytes needs the right to read them, "
          "and sending with it the right to send")
{
    {
        // A monitor without the bytes.
        fixture env;
        const traffic_rig rig(env);
        auto watcher = env.connect();
        const auto refused = watcher->expect_bytes(
            expecting(control::port_id_text(rig.b), "x", 100));
        REQUIRE(!refused);
        REQUIRE_EQ(refused.error(), code(control::api_errc::forbidden));
    }
    {
        // May read the bytes, may not send: waiting works, sending with it
        // does not, and nothing was sent.
        fixture env(with_local_scopes(monitor_with_data()));
        const traffic_rig rig(env);
        auto watcher = env.connect();
        const std::string a = control::port_id_text(rig.a);
        const std::string b = control::port_id_text(rig.b);

        control::expect_bytes_request with_send = expecting(b, "PING", 200);
        with_send.send_port = a;
        with_send.send = vss::to_bytes("PING");
        const auto refused = watcher->expect_bytes(with_send);
        REQUIRE(!refused);
        REQUIRE_EQ(refused.error(), code(control::api_errc::forbidden));
        const auto listed = watcher->list_ports();
        REQUIRE(listed);
        for (const control::port_entry& port : listed->ports)
        {
            REQUIRE_EQ(port.bytes_rx, std::uint64_t{0});
            REQUIRE_EQ(port.bytes_tx, std::uint64_t{0});
        }

        // Waiting alone: someone else sends.
        vss::result<control::matched_reply> found =
            code(control::api_errc::internal);
        {
            const joining_thread waiter(
                [&] { found = watcher->expect_bytes(expecting(b, "PONG")); });
            const auto deadline = steady::now() + long_wait;
            while (operations_named(env, "expect.bytes").size() < 2 &&
                   steady::now() < deadline)
                rig.send(rig.a, "PONG");
        }
        REQUIRE(found);
        // The session only looks, so its connection to the agent does too.
        REQUIRE_EQ(env.backend->server.counters().read_only_refused,
                   std::uint64_t{0});
    }
}

TEST_CASE("control gateway: an expect.bytes that asks for what it may not "
          "have costs an error, not the session")
{
    fixture env(with_local_scopes(control::role_control()));
    auto driver = env.connect();
    const made_pair pair(*driver);
    const auto refused_with = [&](const control::expect_bytes_request& wanted)
    {
        const auto answer = driver->expect_bytes(wanted);
        REQUIRE(!answer);
        return answer.error();
    };
    const auto invalid = code(control::api_errc::invalid_argument);

    REQUIRE_EQ(refused_with(expecting("COM3", "x")), invalid);
    REQUIRE_EQ(refused_with(expecting(pair.right, "")), invalid);
    REQUIRE_EQ(
        refused_with(expecting(
            pair.right, std::string(control::max_pattern_size + 1, 'a'))),
        invalid);
    REQUIRE_EQ(refused_with(expecting(pair.right, "x", 0)), invalid);
    REQUIRE_EQ(refused_with(expecting(pair.right, "x",
                                      control::max_expect_timeout_ms + 1)),
               invalid);
    control::expect_bytes_request sideways = expecting(pair.right, "x");
    sideways.direction = "both";
    REQUIRE_EQ(refused_with(sideways), invalid);
    control::expect_bytes_request bad_target = expecting(pair.right, "x");
    bad_target.send_port = "left";
    bad_target.send = vss::to_bytes("x");
    REQUIRE_EQ(refused_with(bad_target), invalid);
    control::expect_bytes_request too_much = expecting(pair.right, "x");
    too_much.send_port = pair.left;
    too_much.send.assign(control::max_send_size + 1, vss::byte{0x41});
    REQUIRE_EQ(refused_with(too_much), code(control::api_errc::limit_exceeded));
    // A port to send to that is not there: the engine says so, and the wait
    // ends at once and not at its time.
    control::expect_bytes_request nowhere = expecting(pair.right, "x", 20000);
    nowhere.send_port = "p_999999";
    nowhere.send = vss::to_bytes("x");
    const auto started = steady::now();
    REQUIRE_EQ(refused_with(nowhere), engine(vss::errc::not_found));
    REQUIRE(steady::now() - started < 10000ms);

    REQUIRE(driver->status());
    REQUIRE_EQ(env.gate.counters().sessions, std::uint64_t{1});
    // And the next one works.
    control::expect_bytes_request good = expecting(pair.right, "fine");
    good.send_port = pair.left;
    good.send = vss::to_bytes("fine");
    REQUIRE(driver->expect_bytes(good));
}

TEST_CASE("control gateway: one wait at a time for a connection")
{
    fixture env(with_local_scopes(control::role_control()));
    const traffic_rig rig(env);
    raw_client raw(env.instance.name());
    say_hello(raw, "two waits");
    const std::string b = control::port_id_text(rig.b);
    REQUIRE(
        raw.send(wire_of(control::request{2, expecting(b, "first", 20000)})));
    REQUIRE(
        raw.send(wire_of(control::request{3, expecting(b, "second", 20000)})));
    // The second is refused while the first waits.
    const control::reply refused = read_reply(raw);
    REQUIRE_EQ(refused.id, std::uint64_t{3});
    REQUIRE_EQ(error_of(refused).code, std::string("conflict"));

    // The first still waits and is answered when its bytes come; other
    // requests are served meanwhile.
    REQUIRE(raw.send(wire_of(control::request{4, control::status_request{}})));
    const control::reply state = read_reply(raw);
    REQUIRE_EQ(state.id, std::uint64_t{4});
    control::reply found;
    const auto deadline = steady::now() + long_wait;
    for (;;)
    {
        REQUIRE(steady::now() < deadline);
        rig.send(rig.a, "first");
        const auto received = raw.read_frame(200ms);
        if (!received)
            continue;
        const auto message = control::decode_reply(*received);
        REQUIRE(message);
        found = *message;
        break;
    }
    REQUIRE_EQ(found.id, std::uint64_t{2});
    REQUIRE(std::holds_alternative<control::matched_reply>(found.body));
}

TEST_CASE("control gateway: waiting for bytes leaves a stream of the "
          "client's as it was, and gives it no bytes it may not have")
{
    fixture env(with_local_scopes(control::role_control()));
    auto driver = env.connect(false, "harness");
    const made_pair pair(*driver);
    // Events only: this connection has not asked for the traffic stream.
    const auto started = driver->subscribe_events(every(10));
    REQUIRE(started);

    control::expect_bytes_request wanted = expecting(pair.right, "MARK");
    wanted.send_port = pair.left;
    wanted.send = vss::to_bytes("MARK");
    REQUIRE(driver->expect_bytes(wanted));

    // The stream went on through it, in order, and carried no bytes even
    // while the gateway itself was reading them.
    bool data_seen = false;
    std::uint64_t next_seq = started->next_seq;
    REQUIRE(read_batches(*driver, next_seq,
                         [&](const control::event_batch& batch)
                         {
                             for (const control::event_entry& event :
                                  batch.events)
                             {
                                 REQUIRE(event.data.empty());
                                 data_seen = data_seen || event.kind == "data";
                             }
                             return data_seen;
                         }));
    REQUIRE_EQ(env.gate.counters().streams, std::uint64_t{1});
    // And it still runs afterwards.
    REQUIRE(driver->send(pair.left, vss::to_bytes("after")));
    REQUIRE(read_batches(*driver, next_seq,
                         [&](const control::event_batch& batch)
                         {
                             return std::ranges::any_of(
                                 batch.events,
                                 [](const control::event_entry& event)
                                 { return event.kind == "data"; });
                         }));
}

TEST_CASE("control gateway: neither the pattern nor the bytes sent are in "
          "the audit log")
{
    fixture env(with_local_scopes(control::role_control()));
    auto driver = env.connect();
    const made_pair pair(*driver);
    const std::string secret = "PATTERN-5d1e-do-not-log";
    const std::string stimulus = "STIMULUS-9a7c-do-not-log " + secret;
    control::expect_bytes_request wanted = expecting(pair.right, secret);
    wanted.send_port = pair.left;
    wanted.send = vss::to_bytes(stimulus);
    REQUIRE(driver->expect_bytes(wanted));
    driver.reset();
    REQUIRE(wait_until([&] { return env.gate.counters().sessions == 0; }));

    const auto records = operations_named(env, "expect.bytes");
    REQUIRE_EQ(records.size(), std::size_t{1});
    REQUIRE_EQ(text_of(records.front(), "port"), pair.right);
    REQUIRE_EQ(number_of(records.front(), "pattern_bytes"), secret.size());
    REQUIRE_EQ(number_of(records.front(), "sent_bytes"), stimulus.size());
    REQUIRE_EQ(text_of(records.front(), "outcome"), std::string("matched"));
    const std::string kept = everything_kept(env.audit_directory);
    REQUIRE(!holds(kept, "PATTERN-5d1e"));
    REQUIRE(!holds(kept, "STIMULUS-9a7c"));
    require_intact(env.audit_directory);
}

// -- runs --------------------------------------------------------------------

namespace
{

std::size_t pieces_of(std::string_view text, std::string_view piece)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(piece); at != std::string_view::npos;
         at = text.find(piece, at + piece.size()))
        ++count;
    return count;
}

} // namespace

TEST_CASE("control gateway: a run notes how each wait ended, and reports it")
{
    fixture env(with_local_scopes(control::role_control()));
    auto driver = env.connect(false, "harness");
    const made_pair pair(*driver);
    const auto& features = driver->gateway().features;
    REQUIRE(std::ranges::find(features, "runs") != features.end());

    const auto begun = driver->begin_run("smoke test");
    REQUIRE(begun);
    REQUIRE(begun->open);
    REQUIRE_EQ(begun->name, std::string("smoke test"));
    REQUIRE_EQ(begun->run.rfind("r_", 0), std::size_t{0});
    REQUIRE_EQ(begun->tests, std::uint64_t{0});

    control::expect_bytes_request good = expecting(pair.right, "OK");
    good.send_port = pair.left;
    good.send = vss::to_bytes("OK");
    good.label = "the device answers";
    REQUIRE(driver->expect_bytes(good));

    control::expect_bytes_request late = expecting(pair.right, "NEVER", 100);
    late.send_port = pair.left;
    late.send = vss::to_bytes("no");
    REQUIRE(!driver->expect_bytes(late));

    control::expect_bytes_request broken = expecting(pair.right, "x");
    broken.send_port = "p_999999";
    broken.send = vss::to_bytes("x");
    broken.label = "send to nowhere";
    REQUIRE(!driver->expect_bytes(broken));

    REQUIRE(
        driver->record_in_run({"visual check", "skipped", "nobody looked", 0}));
    REQUIRE(driver->record_in_run({"voltage", "passed", "", 40}));

    // A report of a run that is still open.
    const auto early = driver->report_junit();
    REQUIRE(early);
    REQUIRE_EQ(pieces_of(*early, "<testcase"), std::size_t{5});

    const auto ended = driver->end_run();
    REQUIRE(ended);
    REQUIRE(!ended->open);
    REQUIRE_EQ(ended->run, begun->run);
    REQUIRE_EQ(ended->tests, std::uint64_t{5});
    REQUIRE_EQ(ended->failures, std::uint64_t{1});
    REQUIRE_EQ(ended->errors, std::uint64_t{1});
    REQUIRE_EQ(ended->skipped, std::uint64_t{1});

    const auto report = driver->report_junit(begun->run);
    REQUIRE(report);
    const std::string& xml = *report;
    REQUIRE(xml.find("<testsuite name=\"smoke test\" tests=\"5\" "
                     "failures=\"1\" errors=\"1\" skipped=\"1\"") !=
            std::string::npos);
    REQUIRE(xml.find("<testcase name=\"the device answers\" "
                     "classname=\"smoke test\"") != std::string::npos);
    // A wait without a label is named after its port.
    REQUIRE(xml.find("<testcase name=\"expect " + pair.right + "\"") !=
            std::string::npos);
    REQUIRE(xml.find("<failure message=\"the bytes did not arrive within "
                     "100 ms\"/>") != std::string::npos);
    REQUIRE(xml.find("<error message=\"engine.not_found\"/>") !=
            std::string::npos);
    REQUIRE(xml.find("<skipped message=\"nobody looked\"/>") !=
            std::string::npos);
    REQUIRE_EQ(pieces_of(xml, "<testcase"), std::size_t{5});

    // After the end nothing more is noted, and the report stays.
    REQUIRE(driver->expect_bytes(good));
    const auto no_run = driver->record_in_run({"late", "passed", "", 0});
    REQUIRE(!no_run);
    REQUIRE_EQ(no_run.error(), code(control::api_errc::not_found));
    const auto again = driver->end_run();
    REQUIRE(!again);
    REQUIRE_EQ(again.error(), code(control::api_errc::not_found));
    const auto same = driver->report_junit();
    REQUIRE(same);
    REQUIRE_EQ(*same, xml);

    const auto records = audit_records(env.audit_directory);
    const auto started = records_of(records, "run.started");
    const auto finished = records_of(records, "run.ended");
    REQUIRE_EQ(started.size(), std::size_t{1});
    REQUIRE_EQ(text_of(started.front(), "name"), std::string("smoke test"));
    REQUIRE_EQ(text_of(started.front(), "run"), begun->run);
    REQUIRE_EQ(finished.size(), std::size_t{1});
    REQUIRE_EQ(number_of(finished.front(), "tests"), std::uint64_t{5});
    REQUIRE_EQ(number_of(finished.front(), "failures"), std::uint64_t{1});
    require_intact(env.audit_directory);
}

TEST_CASE("control gateway: a run belongs to a principal, across "
          "connections, and to nobody else")
{
    fixture env(with_local_scopes(control::role_control()));
    const control::token_created_reply other =
        make_token(env, control::role_monitor());

    // The local user begins a run on one connection and fills and ends it
    // on others, as a harness does that connects for each step.
    {
        auto first = env.connect(false, "step 1");
        REQUIRE(first->begin_run("in steps"));
    }
    REQUIRE(wait_until([&] { return env.gate.counters().sessions == 0; }));
    {
        auto second = env.connect(false, "step 2");
        REQUIRE(second->record_in_run({"step two", "passed", "", 5}));
        // One open run for a principal.
        const auto twice = second->begin_run("another");
        REQUIRE(!twice);
        REQUIRE_EQ(twice.error(), code(control::api_errc::conflict));
    }
    auto third = env.connect(false, "step 3");
    const auto ended = third->end_run();
    REQUIRE(ended);
    REQUIRE_EQ(ended->tests, std::uint64_t{1});

    // A token is another principal: it sees no run, cannot end or fill
    // this one, and cannot read its report, by id or as "the latest".
    auto connected = connect_with(env, other.token);
    REQUIRE(connected);
    auto& stranger = **connected;
    for (const std::string& run : {std::string(), ended->run})
    {
        const auto peeked = stranger.report_junit(run);
        REQUIRE(!peeked);
        REQUIRE_EQ(peeked.error(), code(control::api_errc::not_found));
    }
    const auto ended_twice = stranger.end_run();
    REQUIRE(!ended_twice);
    REQUIRE_EQ(ended_twice.error(), code(control::api_errc::not_found));
    // It may keep a run of its own, with its own number.
    const auto own = stranger.begin_run("the token's");
    REQUIRE(own);
    REQUIRE(own->run != ended->run);
    const auto own_report = stranger.report_junit();
    REQUIRE(own_report);
    REQUIRE(own_report->find("the token&apos;s") != std::string::npos);
    REQUIRE_EQ(own_report->find("in steps"), std::string::npos);

    // The administration endpoint reads any run by its id.
    auto admin = env.connect(true);
    const auto read = admin->report_junit(ended->run);
    REQUIRE(read);
    REQUIRE(read->find("name=\"in steps\"") != std::string::npos);
}

TEST_CASE("control gateway: what a harness calls its checks cannot change "
          "the report or the audit log")
{
    fixture env;
    auto harness = env.connect(false, "harness");
    const std::string attack =
        "\"/></testsuite><testsuite name=\"forged\">\n{\"event\":\"forged\"}";
    REQUIRE(harness->begin_run(attack));
    REQUIRE(harness->record_in_run({attack, "failed", attack, 1}));
    const auto ended = harness->end_run();
    REQUIRE(ended);
    const auto report = harness->report_junit();
    REQUIRE(report);
    // One suite, one case, one failure, and the forged text only as text.
    REQUIRE_EQ(pieces_of(*report, "<testsuite "), std::size_t{1});
    REQUIRE_EQ(pieces_of(*report, "</testsuite>"), std::size_t{1});
    REQUIRE_EQ(pieces_of(*report, "<testcase"), std::size_t{1});
    REQUIRE_EQ(pieces_of(*report, "<"), std::size_t{8});
    REQUIRE(report->find("&quot;/&gt;&lt;/testsuite&gt;") != std::string::npos);
    // The audit log has one line for each record, none of them the
    // client's, and still verifies.
    REQUIRE(records_of(audit_records(env.audit_directory), "forged").empty());
    REQUIRE_EQ(
        records_of(audit_records(env.audit_directory), "run.started").size(),
        std::size_t{1});
    require_intact(env.audit_directory);
}

TEST_CASE("control gateway: a run has limits, and a wrong record costs only "
          "an error")
{
    fixture env;
    auto harness = env.connect();
    const auto invalid = code(control::api_errc::invalid_argument);

    const auto unnamed = harness->begin_run("");
    REQUIRE(!unnamed);
    REQUIRE_EQ(unnamed.error(), invalid);
    const auto long_name =
        harness->begin_run(std::string(control::max_run_name_size + 1, 'n'));
    REQUIRE(!long_name);
    REQUIRE_EQ(long_name.error(), invalid);
    const auto nothing = harness->report_junit();
    REQUIRE(!nothing);
    REQUIRE_EQ(nothing.error(), code(control::api_errc::not_found));
    REQUIRE(!harness->report_junit("r_999"));
    REQUIRE(!harness->report_junit("not a run"));

    REQUIRE(harness->begin_run(std::string(control::max_run_name_size, 'n')));
    for (const control::runs_record_request& bad :
         {control::runs_record_request{"a", "PASSED", "", 0},
          control::runs_record_request{"a", "ok", "", 0},
          control::runs_record_request{"a", "", "", 0},
          control::runs_record_request{"", "passed", "", 0},
          control::runs_record_request{
              std::string(control::max_run_name_size + 1, 'c'), "passed", "",
              0},
          control::runs_record_request{
              "a", "failed",
              std::string(control::max_case_message_size + 1, 'm'), 0}})
    {
        const auto refused = harness->record_in_run(bad);
        REQUIRE(!refused);
        REQUIRE_EQ(refused.error(), invalid);
    }
    // A run keeps so many checks and says so for one more.  The requests go
    // slowly enough to stay inside the session's allowance.
    control::gateway_options roomy;
    roomy.requests_per_second = 1e9;
    roomy.request_burst = 1e9;
    fixture big(roomy);
    auto filler = big.connect();
    REQUIRE(filler->begin_run("full"));
    for (std::size_t index = 0; index < control::max_run_cases; ++index)
        REQUIRE(filler->record_in_run({"c", "passed", "", 0}));
    const auto over = filler->record_in_run({"c", "passed", "", 0});
    REQUIRE(!over);
    REQUIRE_EQ(over.error(), code(control::api_errc::limit_exceeded));
    const auto full = filler->end_run();
    REQUIRE(full);
    REQUIRE_EQ(full->tests, std::uint64_t{control::max_run_cases});
    const auto report = filler->report_junit();
    REQUIRE(report);
    REQUIRE(report->size() <= control::max_report_size);

    REQUIRE(harness->status());
}

TEST_CASE("control gateway: only so many runs are kept, and an open one is "
          "never the one to go")
{
    fixture env;
    auto harness = env.connect();
    std::vector<std::string> ids;
    ids.reserve(10);
    for (int index = 0; index < 10; ++index)
    {
        const auto begun = harness->begin_run("run " + std::to_string(index));
        REQUIRE(begun);
        ids.push_back(begun->run);
        REQUIRE(harness->end_run());
    }
    // The two oldest went to make room; the eight newest can be read.
    REQUIRE(!harness->report_junit(ids[0]));
    REQUIRE(!harness->report_junit(ids[1]));
    for (std::size_t index = 2; index < ids.size(); ++index)
        REQUIRE(harness->report_junit(ids[index]));
    const auto latest = harness->report_junit();
    REQUIRE(latest);
    REQUIRE(latest->find("name=\"run 9\"") != std::string::npos);
}
