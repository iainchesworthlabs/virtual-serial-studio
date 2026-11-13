//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control gateway as real processes: `vss_agent` and `vss_gate` are
// started as children, `vss_cli control ...` and `vss_cli agent ...` run as
// more children, and the in-process control client plays a script.  The
// claims under test are that the gateway shows a program the ports of the
// agent behind it, that it can be stopped without closing one of them, that
// it says plainly when it has no agent, and that what it did is in a log
// that verifies (docs/remote-control.md, sections 3, 9 and 13).
//
// Every test starts its own processes under its own instance name, so a test
// never meets another test or the agent and gateway of a person using this
// computer, and every gateway keeps its audit log in a scratch directory.
// Nothing here sleeps: a test waits for a line of output, for a process to
// exit or for a condition, always with a deadline.
//

#include <vss/control/audit_log.hpp>
#include <vss/control/client.hpp>
#include <vss/control/proto/error.hpp>
#include <vss/control/proto/message.hpp>

#include "child_process.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using vss_test_support::child_process;
using vss_test_support::scratch_directory;
using namespace std::chrono_literals;

namespace
{

/// The longest any wait for a process or a line may take.  Generous, for
/// slow machines and sanitizer builds; a passing test takes a second or two.
constexpr std::chrono::milliseconds long_limit{20000};

/// An instance name no other test, run, agent or gateway uses: up to 32
/// letters, digits, `_` and `-`.
std::string unique_instance()
{
    static const std::string prefix = []
    {
        std::random_device device;
        std::ostringstream text;
        text << "vsstc-" << std::hex << std::setw(8) << std::setfill('0')
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
void require_api_error(const vss::error_code& actual,
                       vss::control::api_errc expected, const char* what)
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

/// Everything in the file at `path`, empty when it cannot be read.
std::string read_text(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

/// Reads lines from `child` until one contains `marker`.  Throws, naming what
/// was said instead, when the output ends or the limit passes first.
void wait_for_line(child_process& child, const std::string& marker,
                   const char* who)
{
    std::string seen;
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto line = child.read_line(long_limit);
        if (!line)
            break;
        seen += *line + "\n";
        if (contains(*line, marker))
            return;
    }
    throw vss_test::failure(std::string(who) + " did not say \"" + marker +
                            "\"; it said: " + seen);
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
        wait_for_line(child_, "listening on", "the agent");
    }

    const std::string& instance() const noexcept { return instance_; }
    child_process& child() noexcept { return child_; }

private:
    std::string instance_;
    child_process child_;
};

/// `vss_gate` as a child process, serving the agent of the same instance name
/// and keeping its audit log in a directory of its own under a scratch
/// directory.  The constructor returns when the gateway says where it
/// listens.  The child is killed when this object goes, before the scratch
/// directory is removed.
class gate_process
{
public:
    explicit gate_process(std::string name)
        : instance_(std::move(name)), audit_dir_(scratch_.path() / "audit"),
          child_(VSS_GATE_PROGRAM,
                 {"--instance=" + instance_, "--agent-instance=" + instance_,
                  "--audit-dir=" + audit_dir_.string()})
    {
        wait_for_line(child_, "administration on", "the gateway");
    }

    const std::string& instance() const noexcept { return instance_; }
    const std::filesystem::path& audit_dir() const noexcept
    {
        return audit_dir_;
    }
    child_process& child() noexcept { return child_; }

private:
    std::string instance_;
    scratch_directory scratch_;
    std::filesystem::path audit_dir_;
    child_process child_;
};

/// How a `vss_cli` command ended.
struct cli_result
{
    std::optional<int> code;
    std::string output;
};

/// Runs `vss_cli <group> <words> --instance=<instance>` to its end.  `code` is
/// empty when it did not end within the limit (the child is then killed).
cli_result run_cli(const std::string& group, const std::string& instance,
                   const std::vector<std::string>& words)
{
    std::vector<std::string> arguments{group};
    for (const auto& word : words)
        arguments.push_back(word);
    arguments.push_back("--instance=" + instance);
    child_process child(VSS_AGENT_CLI_PROGRAM, arguments);
    cli_result outcome;
    outcome.code = child.wait(long_limit);
    outcome.output = child.read_all(5000ms);
    return outcome;
}

/// Makes the port `name` in the agent of the instance `instance`.
void create_port(const std::string& instance, const std::string& name)
{
    const auto made = run_cli("agent", instance, {"create-port", name});
    if (made.code != 0)
    {
        throw vss_test::failure("vss_cli agent create-port did not exit with "
                                "0; output: " +
                                made.output);
    }
}

/// Connects the in-process client to the gateway of the instance `name`.
std::unique_ptr<vss::control::client>
connect_client(const std::string& name,
               std::chrono::milliseconds timeout = 10000ms)
{
    vss::control::client_options options;
    options.instance = name;
    options.name = "system-test";
    options.timeout = timeout;
    auto connected = vss::control::client::connect(options);
    require_ok(connected, "connecting to the gateway");
    return std::move(*connected);
}

const vss::control::port_entry*
find_port(const vss::control::ports_reply& listed, const std::string& name)
{
    for (const auto& port : listed.ports)
    {
        if (port.name == name)
            return &port;
    }
    return nullptr;
}

} // namespace

TEST_CASE("system control: the gateway shows the ports of the agent")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    gate_process gate(instance);
    create_port(instance, "shown");

    const auto status = run_cli("control", instance, {"status"});
    if (status.code != 0)
    {
        throw vss_test::failure(
            "vss_cli control status did not exit with 0; output: " +
            status.output);
    }
    REQUIRE(contains(status.output, "Remote control: local only"));
    REQUIRE(contains(status.output, "You may: monitor"));
    REQUIRE(contains(status.output, "Agent: running"));

    auto script = connect_client(instance);
    const auto listed = script->list_ports();
    require_ok(listed, "listing the ports through the gateway");
    const auto* port = find_port(*listed, "shown");
    REQUIRE(port != nullptr);
    REQUIRE(port->id.starts_with("p_"));
}

TEST_CASE("system control: with no gateway the command says it is off")
{
    const std::string instance = unique_instance();

    const auto status = run_cli("control", instance, {"status"});
    REQUIRE(status.code == 3);
    REQUIRE(contains(status.output, "Remote control: off"));

    const auto stopped = run_cli("control", instance, {"stop"});
    REQUIRE(stopped.code == 3);
    REQUIRE(contains(stopped.output, "Remote control: off"));
}

TEST_CASE("system control: stopping the gateway closes no port")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    gate_process gate(instance);
    create_port(instance, "keepme");

    const auto stopped = run_cli("control", instance, {"stop"});
    if (stopped.code != 0)
    {
        throw vss_test::failure(
            "vss_cli control stop did not exit with 0; output: " +
            stopped.output);
    }
    REQUIRE(contains(stopped.output, "Remote control stopped"));
    REQUIRE(gate.child().wait(long_limit) == 0);
    REQUIRE(contains(gate.child().read_all(5000ms), "vss_gate stopped"));

    // The agent runs on, and the port is still in it.
    REQUIRE(!agent.child().wait(0ms).has_value());
    const auto ports = run_cli("agent", instance, {"ports"});
    REQUIRE(ports.code == 0);
    REQUIRE(contains(ports.output, "keepme"));

    // Nothing listens for the gateway any more.
    const auto again = run_cli("control", instance, {"status"});
    REQUIRE(again.code == 3);
}

TEST_CASE("system control: a second gateway for the same instance is refused")
{
    gate_process first(unique_instance());
    const std::string& instance = first.instance();

    // Its own audit directory, so that nothing but the name is shared.
    const scratch_directory other_scratch;
    child_process second(
        VSS_GATE_PROGRAM,
        {"--instance=" + instance, "--agent-instance=" + instance,
         "--audit-dir=" + (other_scratch.path() / "audit").string()});
    REQUIRE(second.wait(long_limit) == 3);
    REQUIRE(contains(second.read_all(5000ms), "already running"));

    // The first gateway is not disturbed.
    REQUIRE(!first.child().wait(0ms).has_value());
    auto script = connect_client(instance);
    const auto state = script->status();
    require_ok(state, "asking the first gateway for its status");
}

TEST_CASE("system control: a gateway with no agent says so")
{
    gate_process gate(unique_instance());
    const std::string& instance = gate.instance();

    const auto status = run_cli("control", instance, {"status"});
    REQUIRE(status.code == 0);
    REQUIRE(contains(status.output, "Remote control: local only"));
    REQUIRE(contains(status.output, "Agent: not running"));

    auto script = connect_client(instance);
    const auto state = script->status();
    require_ok(state, "asking for the status");
    REQUIRE(!state->agent_running);

    const auto listed = script->list_ports();
    REQUIRE(!listed);
    require_api_error(listed.error(), vss::control::api_errc::unavailable,
                      "list_ports() with no agent");
}

TEST_CASE("system control: the audit log verifies after a run")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    gate_process gate(instance);

    const auto status = run_cli("control", instance, {"status"});
    REQUIRE(status.code == 0);
    const auto stopped = run_cli("control", instance, {"stop"});
    REQUIRE(stopped.code == 0);
    REQUIRE(gate.child().wait(long_limit) == 0);

    const std::filesystem::path file = gate.audit_dir() / "audit.jsonl";
    REQUIRE(std::filesystem::exists(file));

    const auto verdict = vss::control::verify_audit_log(gate.audit_dir());
    require_ok(verdict, "verifying the audit log");
    if (!verdict->intact)
    {
        throw vss_test::failure("the audit log is not intact at line " +
                                std::to_string(verdict->line) + ": " +
                                verdict->problem);
    }
    REQUIRE(verdict->records >= 3U);

    const std::string text = read_text(file);
    REQUIRE(contains(text, "\"event\":\"session.opened\""));
    REQUIRE(contains(text, "\"event\":\"gateway.stopped\""));
}
