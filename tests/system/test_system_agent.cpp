//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent as real processes: `vss_agent` is started as a child, `vss_cli
// agent ...` runs as more children, and the in-process client library plays
// the second program.  The claim under test is that a port made through one
// client outlives it and that another client can see and use it (docs/agent.md,
// sections 2, 3, 4 and 9).
//
// Every test starts its own agent under its own instance name, so a test never
// meets another test or the agent of a person using this computer.  Nothing
// here sleeps: a test waits for a line of output, for a process to exit or
// for a condition, always with a deadline.  Two loopback ports linked both
// ways pass a byte back and forth for ever, so the ports are linked one way.
//

#include <vss/agent/client.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/message.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/event.hpp>
#include <vss/core/types.hpp>

#include "child_process.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using vss_test_support::child_process;
using namespace std::chrono_literals;

namespace
{

/// The longest any wait for a process or a line may take.  Generous, for
/// slow machines and sanitizer builds; a passing test takes a second or two.
constexpr std::chrono::milliseconds long_limit{20000};

/// An instance name no other test, run or agent uses: up to 32 letters,
/// digits, `_` and `-`.
std::string unique_instance()
{
    static const std::string prefix = []
    {
        std::random_device device;
        std::ostringstream text;
        text << "vsst-" << std::hex << std::setw(8) << std::setfill('0')
             << device();
        return text.str();
    }();
    static unsigned counter = 0;
    return prefix + "-" + std::to_string(counter++);
}

std::string describe(const vss::error_code& error)
{
    return std::string(error.category().name()) + ":" +
           std::to_string(error.value()) + " (" + error.message() + ")";
}

/// Fails the test with `what` and the error when `outcome` holds one.
template <class Outcome>
void require_ok(const Outcome& outcome, const char* what)
{
    if (!outcome)
    {
        throw vss_test::failure(std::string(what) +
                                " failed: " + describe(outcome.error()));
    }
}

/// Fails the test, naming what was seen, unless `actual` is `expected`.
void require_protocol_error(const vss::error_code& actual,
                            vss::agent::protocol_errc expected,
                            const char* what)
{
    if (actual == expected)
        return;
    throw vss_test::failure(std::string(what) + ": expected " +
                            describe(make_error_code(expected)) + ", got " +
                            describe(actual));
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

/// `vss_agent` as a child process, started under its own instance name.  The
/// constructor returns when the agent says it listens.  The child is killed
/// when this object goes, also when a requirement throws.
class agent_process
{
public:
    explicit agent_process(std::string name)
        : instance_(std::move(name)),
          child_(VSS_AGENT_PROGRAM, {"--instance=" + instance_})
    {
        wait_until_listening();
    }

    const std::string& instance() const noexcept { return instance_; }
    child_process& child() noexcept { return child_; }

private:
    void wait_until_listening()
    {
        std::string seen;
        const auto deadline = std::chrono::steady_clock::now() + long_limit;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto line = child_.read_line(long_limit);
            if (!line)
                break;
            seen += *line + "\n";
            if (contains(*line, "listening on"))
                return;
        }
        throw vss_test::failure("the agent did not say it listens; it said: " +
                                seen);
    }

    std::string instance_;
    child_process child_;
};

/// How a `vss_cli agent` command ended.
struct cli_result
{
    std::optional<int> code;
    std::string output;
};

/// The argument list of `vss_cli agent <words> --instance=<instance>`.
std::vector<std::string> agent_arguments(const std::string& name,
                                         std::vector<std::string> words)
{
    std::vector<std::string> arguments{"agent"};
    for (auto& word : words)
        arguments.push_back(std::move(word));
    arguments.push_back("--instance=" + name);
    return arguments;
}

/// Runs `vss_cli agent <words>` to its end.  `code` is empty when it did not
/// end within the limit (the child is then killed).
cli_result run_cli(const std::string& name, std::vector<std::string> words)
{
    child_process child(VSS_AGENT_CLI_PROGRAM,
                        agent_arguments(name, std::move(words)));
    cli_result result;
    result.code = child.wait(long_limit);
    result.output = child.read_all(5000ms);
    return result;
}

vss::agent::client_options make_options(const std::string& name,
                                        std::chrono::milliseconds timeout)
{
    vss::agent::client_options options;
    options.instance = name;
    options.name = "system-test";
    options.timeout = timeout;
    return options;
}

/// Connects the in-process client to the agent of the instance `name`.
std::unique_ptr<vss::agent::client>
connect_client(const std::string& name,
               std::chrono::milliseconds timeout = 10000ms)
{
    auto connected = vss::agent::client::connect(make_options(name, timeout));
    require_ok(connected, "connecting to the agent");
    return std::move(*connected);
}

const vss::agent::port_summary* find_port(const vss::agent::list_reply& listed,
                                          const std::string& name)
{
    for (const auto& port : listed.ports)
    {
        if (port.name == name)
            return &port;
    }
    return nullptr;
}

/// The ids of the ports `a` and `b`.
struct port_pair
{
    vss::endpoint_id a = 0;
    vss::endpoint_id b = 0;
};

/// Requires that the agent holds exactly the open ports `a` and `b` and one
/// one-way link from `a` to `b`, and returns the ports' ids.
port_pair require_ports_a_and_b(vss::agent::client& peer)
{
    const auto listed = peer.list();
    require_ok(listed, "listing the ports");
    REQUIRE_EQ(listed->ports.size(), 2U);
    const auto* a = find_port(*listed, "a");
    const auto* b = find_port(*listed, "b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->open);
    REQUIRE(b->open);
    REQUIRE_NE(a->id, b->id);

    REQUIRE_EQ(listed->links.size(), 1U);
    const auto& link = listed->links.front();
    REQUIRE(link.mode == vss::link_mode::one_way);
    REQUIRE_EQ(link.sources.size(), 1U);
    REQUIRE_EQ(link.targets.size(), 1U);
    REQUIRE_EQ(link.sources.front(), a->id);
    REQUIRE_EQ(link.targets.front(), b->id);
    return {a->id, b->id};
}

/// Makes the ports `a` and `b` and a one-way link from `a` to `b`, in one
/// connection.
void make_ports_a_and_b(vss::agent::client& peer)
{
    const auto a = peer.create_port("a");
    require_ok(a, "creating port a");
    const auto b = peer.create_port("b");
    require_ok(b, "creating port b");
    const auto linked = peer.connect_ports(*a, *b, true);
    require_ok(linked, "linking a to b");
}

/// Reads batches of events from a subscribed `peer` until the bytes of the
/// data events with direction rx on `port` contain `expected`, or the
/// deadline passes.  The bytes seen so far are returned.
std::string read_rx_bytes(vss::agent::client& peer, vss::endpoint_id port,
                          const std::string& expected)
{
    std::string seen;
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    while (!contains(seen, expected) &&
           std::chrono::steady_clock::now() < deadline)
    {
        const auto batch = peer.next_events(200ms);
        if (!batch)
        {
            if (batch.error() == vss::errc::timeout)
                continue;
            throw vss_test::failure("reading events failed: " +
                                    describe(batch.error()));
        }
        for (const auto& event : batch->events)
        {
            if (event.kind != vss::event_kind::data ||
                event.direction != vss::data_direction::rx ||
                event.endpoint != port)
            {
                continue;
            }
            seen += vss::to_string(event.data);
        }
    }
    return seen;
}

/// Subscribes `reader` with data, writes `text` to `from` and requires that
/// the bytes arrive as rx data events on `to`.  With `exact` they are all
/// the bytes that arrive; without, an earlier write may have left some.
void require_write_travels(vss::agent::client& reader, vss::endpoint_id from,
                           vss::endpoint_id to, const std::string& text,
                           bool exact)
{
    const auto subscribed = reader.subscribe(true, 20);
    require_ok(subscribed, "subscribing");
    const auto written = reader.write(from, vss::as_bytes(text));
    require_ok(written, "writing");
    const std::string seen = read_rx_bytes(reader, to, text);
    if (exact)
        REQUIRE_EQ(seen, text);
    else
        REQUIRE(contains(seen, text));
}

} // namespace

TEST_CASE("agent system: a port outlives the client that made it")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();

    // Client A: three separate programs, each gone before the next starts.
    const std::vector<std::vector<std::string>> commands{
        {"create-port", "a"},
        {"create-port", "b"},
        {"connect", "a", "b", "--one-way"}};
    for (const auto& words : commands)
    {
        const auto result = run_cli(instance, words);
        if (result.code != 0)
        {
            throw vss_test::failure(
                "vss_cli agent " + words.front() +
                " did not exit with 0; output: " + result.output);
        }
    }

    // Client B: no client is connected now, and the ports are still there.
    {
        auto reader = connect_client(instance);
        const port_pair ports = require_ports_a_and_b(*reader);
        require_write_travels(*reader, ports.a, ports.b, "ping", true);
    }

    // A fourth program sees them too.
    const auto listed = run_cli(instance, {"ports"});
    REQUIRE(listed.code == 0);
    REQUIRE(contains(listed.output, " a "));
    REQUIRE(contains(listed.output, " b "));

    // And a clean stop.
    const auto stopped = run_cli(instance, {"stop"});
    REQUIRE(stopped.code == 0);
    REQUIRE(agent.child().wait(long_limit) == 0);
    REQUIRE(contains(agent.child().read_all(5000ms), "vss_agent stopped"));
}

TEST_CASE("agent system: killing a client leaves the agent and its ports")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();

    {
        auto maker = connect_client(instance);
        make_ports_a_and_b(*maker);
    }

    {
        child_process watcher(
            VSS_AGENT_CLI_PROGRAM,
            agent_arguments(instance,
                            std::vector<std::string>{"watch", "--data"}));

        // The watcher subscribes a moment after it starts, so write until it
        // prints a data event.
        auto writer = connect_client(instance);
        const port_pair ports = require_ports_a_and_b(*writer);
        bool printed = false;
        const auto deadline = std::chrono::steady_clock::now() + long_limit;
        while (!printed && std::chrono::steady_clock::now() < deadline)
        {
            const auto written = writer->write(ports.a, vss::as_bytes("ping"));
            require_ok(written, "writing");
            const auto line = watcher.read_line(200ms);
            printed = line && contains(*line, "data");
        }
        REQUIRE(printed);

        watcher.kill();
    }

    // The agent runs on, with the ports, the link and a working path.
    REQUIRE(!agent.child().wait(0ms).has_value());
    auto reader = connect_client(instance);
    const port_pair ports = require_ports_a_and_b(*reader);
    require_write_travels(*reader, ports.a, ports.b, "pong", false);
}

TEST_CASE("agent system: a killed agent is noticed and a new one starts clean")
{
    const std::string instance = unique_instance();
    agent_process first(instance);

    auto survivor = connect_client(instance, 5000ms);
    const auto created = survivor->create_port("a");
    require_ok(created, "creating port a");

    first.child().kill();

    const auto started = std::chrono::steady_clock::now();
    const auto listed = survivor->list();
    const auto waited = std::chrono::steady_clock::now() - started;
    REQUIRE(!listed);
    require_protocol_error(listed.error(),
                           vss::agent::protocol_errc::connection_closed,
                           "list() after the agent was killed");
    // It failed at once or within its timeout; it did not hang.
    REQUIRE(waited < 15s);

    const auto again =
        vss::agent::client::connect(make_options(instance, 5000ms));
    REQUIRE(!again);
    require_protocol_error(again.error(),
                           vss::agent::protocol_errc::not_running,
                           "connecting to a killed agent");

    const auto cli = run_cli(instance, {"ports"});
    REQUIRE(cli.code == 3);

    // A new agent under the same name replaces whatever the old one left (a
    // socket file on Linux and macOS) and starts with nothing.
    agent_process second(instance);
    auto fresh = connect_client(instance);
    const auto empty = fresh->list();
    require_ok(empty, "listing the ports of the new agent");
    REQUIRE(empty->ports.empty());
    REQUIRE(empty->links.empty());
}

TEST_CASE("agent system: a second agent for the same instance is refused")
{
    agent_process first(unique_instance());
    const std::string& instance = first.instance();

    child_process second(VSS_AGENT_PROGRAM, {"--instance=" + instance});
    REQUIRE(second.wait(long_limit) == 3);
    REQUIRE(contains(second.read_all(5000ms), "already running"));

    // The first agent is not disturbed.
    auto peer = connect_client(instance);
    const auto created = peer->create_port("a");
    require_ok(created, "creating port a");
    const auto listed = peer->list();
    require_ok(listed, "listing the ports");
    REQUIRE_EQ(listed->ports.size(), 1U);
}

TEST_CASE("agent system: with no agent the command says so")
{
    const auto result = run_cli(unique_instance(), {"ports"});
    REQUIRE(result.code == 3);
    REQUIRE(contains(result.output, "no agent is running"));
}

TEST_CASE("agent system: a stop signal ends the agent cleanly")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();

    {
        auto maker = connect_client(instance);
        const auto created = maker->create_port("a");
        require_ok(created, "creating port a");
    }

    if (!agent.child().request_stop())
    {
        VSS_SKIP("the stop signal could not be delivered to the agent; on "
                 "Windows this test needs a console to send CTRL_BREAK to");
    }
    REQUIRE(agent.child().wait(long_limit) == 0);
    REQUIRE(contains(agent.child().read_all(5000ms), "vss_agent stopped"));

    const auto again =
        vss::agent::client::connect(make_options(instance, 5000ms));
    REQUIRE(!again);
    require_protocol_error(again.error(),
                           vss::agent::protocol_errc::not_running,
                           "connecting to a stopped agent");
}

TEST_CASE("agent system: bad command lines are refused with code 2")
{
    {
        child_process bad(VSS_AGENT_PROGRAM, {"--instance=bad/name"});
        REQUIRE(bad.wait(long_limit) == 2);
    }
    {
        child_process bad(VSS_AGENT_PROGRAM, {"--no-such-option"});
        REQUIRE(bad.wait(long_limit) == 2);
    }

    const std::string instance = unique_instance();
    {
        // No action; no agent is needed to find that out.
        child_process bad(VSS_AGENT_CLI_PROGRAM,
                          {"agent", "--instance=" + instance});
        REQUIRE(bad.wait(long_limit) == 2);
    }

    // A command with a missing argument, against an agent that is running.
    agent_process agent(instance);
    const auto result = run_cli(instance, {"create-port"});
    REQUIRE(result.code == 2);
}
