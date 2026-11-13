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
#include <vss/control/proto/secret.hpp>
#include <vss/control/proto/token.hpp>

#include "child_process.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
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
        text << "g" << std::hex << std::setw(8) << std::setfill('0')
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
    /// The scratch directory that holds the audit directory; a test keeps
    /// its own files here, outside the audit log.
    const std::filesystem::path& scratch_dir() const noexcept
    {
        return scratch_.path();
    }

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

/// Fails the test, with the output, unless the command ended with `expected`.
void require_exit(const cli_result& outcome, int expected, const char* what)
{
    if (outcome.code == expected)
        return;
    throw vss_test::failure(std::string(what) + " did not exit with " +
                            std::to_string(expected) +
                            "; output: " + outcome.output);
}

/// The first line of `output` that is a token.
std::optional<std::string> find_token(const std::string& output)
{
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() == vss::control::token_size &&
            vss::control::parse_token(vss::control::secret_string(line)))
            return line;
    }
    return std::nullopt;
}

/// The public id of a token.  Fails the test for text that is no token.
std::string id_of(const std::string& token)
{
    const auto parts =
        vss::control::parse_token(vss::control::secret_string(token));
    if (!parts)
        throw vss_test::failure("text that should be a token is not one");
    return parts->id;
}

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    stream.close();
    if (!stream)
    {
        throw vss_test::failure("cannot write the file " + path.string());
    }
}

/// `vss_cli control token create` with a label and a role; returns the token
/// it printed.
std::string create_token(const std::string& instance, const std::string& label,
                         const std::string& role)
{
    const auto made =
        run_cli("control", instance,
                {"token", "create", "--label=" + label, "--role=" + role});
    require_exit(made, 0, "vss_cli control token create");
    const auto token = find_token(made.output);
    if (!token)
    {
        throw vss_test::failure("token create printed no token; output: " +
                                made.output);
    }
    return *token;
}

/// A text that must not hold a secret, and what the secret looks like in it.
struct needle
{
    const char* kind;
    std::string text;
};

/// The bytes of `text` as hex digits.
std::string hex_of(const std::string& text, bool upper_case)
{
    const std::string_view digits =
        upper_case ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    for (const char letter : text)
    {
        const auto byte = static_cast<unsigned char>(letter);
        out += digits[static_cast<std::size_t>(byte >> 4)];
        out += digits[static_cast<std::size_t>(byte & 0x0fU)];
    }
    return out;
}

std::string upper_of(const std::string& text)
{
    std::string out = text;
    for (char& letter : out)
    {
        if (letter >= 'a' && letter <= 'z')
            letter = static_cast<char>(letter - 'a' + 'A');
    }
    return out;
}

/// The forms in which a token or its secret could leak.
std::vector<needle> needles_for(const std::string& token)
{
    const std::string secret =
        token.substr(token.size() - vss::control::token_secret_size);
    return {{"the whole token", token},
            {"the secret", secret},
            {"the secret in upper case", upper_of(secret)},
            {"the secret in hex", hex_of(secret, false)},
            {"the secret in upper case hex", hex_of(secret, true)}};
}

/// Fails the test, naming `where` and the form but not the secret, when
/// `text` holds any of the needles.
void require_absent(const std::vector<needle>& needles, const std::string& text,
                    const std::string& where)
{
    for (const needle& form : needles)
    {
        if (contains(text, form.text))
        {
            throw vss_test::failure(where + " holds " + form.kind);
        }
    }
}

/// Reads every file under `directory` in binary and requires that none holds
/// a needle.  Returns how many files it looked at.
std::size_t require_files_free_of(const std::filesystem::path& directory,
                                  const std::vector<needle>& needles)
{
    std::size_t looked_at = 0;
    for (const auto& item :
         std::filesystem::recursive_directory_iterator(directory))
    {
        if (!item.is_regular_file())
            continue;
        ++looked_at;
        require_absent(needles, read_text(item.path()),
                       "the file " + item.path().filename().string());
    }
    return looked_at;
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

TEST_CASE("system control: a token is made, listed and used")
{
    gate_process gate(unique_instance());
    const std::string& instance = gate.instance();

    const auto made =
        run_cli("control", instance,
                {"token", "create", "--label=rig", "--role=monitor"});
    require_exit(made, 0, "vss_cli control token create");
    REQUIRE(contains(made.output, "Token "));
    REQUIRE(contains(made.output, "created for \"rig\""));
    REQUIRE(contains(made.output, "Scopes: monitor"));
    REQUIRE(contains(made.output, "Expires: "));
    REQUIRE(contains(made.output, "only time"));
    const auto token = find_token(made.output);
    REQUIRE(token.has_value());
    REQUIRE(token->starts_with("vsst1_"));
    REQUIRE_EQ(token->size(), vss::control::token_size);
    const std::string id = id_of(*token);
    REQUIRE(contains(made.output, "Token " + id + " created"));

    const auto listed = run_cli("control", instance, {"token", "list"});
    require_exit(listed, 0, "vss_cli control token list");
    REQUIRE(contains(listed.output, id));
    REQUIRE(contains(listed.output, "rig"));
    REQUIRE(contains(listed.output, "monitor"));
    REQUIRE(contains(listed.output, "active"));

    const std::filesystem::path file = gate.scratch_dir() / "rig.token";
    write_text(file, *token + "\n");
    const auto status = run_cli("control", instance,
                                {"status", "--token-file=" + file.string()});
    require_exit(status, 0, "vss_cli control status --token-file");
    REQUIRE(contains(status.output, "You are: token:" + id));
    REQUIRE(contains(status.output, "You may: monitor"));
}

TEST_CASE("system control: a revoked token is refused")
{
    gate_process gate(unique_instance());
    const std::string& instance = gate.instance();

    const std::string token = create_token(instance, "short-lived", "monitor");
    const std::string id = id_of(token);
    const std::filesystem::path file = gate.scratch_dir() / "revoked.token";
    write_text(file, token + "\n");
    const std::string token_option = "--token-file=" + file.string();

    require_exit(run_cli("control", instance, {"status", token_option}), 0,
                 "vss_cli control status with the new token");

    const auto revoked = run_cli("control", instance, {"token", "revoke", id});
    require_exit(revoked, 0, "vss_cli control token revoke");
    REQUIRE(contains(revoked.output, "Token " + id + " revoked"));

    const auto refused = run_cli("control", instance, {"status", token_option});
    require_exit(refused, 1, "vss_cli control status with a revoked token");
    REQUIRE(!contains(refused.output, "You are:"));

    const auto listed = run_cli("control", instance, {"token", "list"});
    require_exit(listed, 0, "vss_cli control token list");
    REQUIRE(contains(listed.output, id));
    REQUIRE(contains(listed.output, "revoked"));

    const auto unknown =
        run_cli("control", instance, {"token", "revoke", "aaaaaaaaaaaaa"});
    require_exit(unknown, 1, "vss_cli control token revoke of an unknown id");
    REQUIRE(contains(unknown.output, "no token has that id"));
}

TEST_CASE("system control: token create refuses a bad command line")
{
    gate_process gate(unique_instance());
    const std::string& instance = gate.instance();

    const std::vector<std::vector<std::string>> unusable{
        {"token", "create", "--label=x", "--role=admin"},
        {"token", "create", "--label=x", "--role=monitor", "--scope=monitor"},
        {"token", "create", "--label=x"},
        {"token", "create", "--role=monitor"},
        {"token", "create", "--label=x", "--role=monitor", "--days=0"},
        {"token", "create", "--label=x", "--role=monitor", "--days=366"},
        {"token", "create", "--label=x", "--role=monitor", "--days=ten"}};
    for (const auto& words : unusable)
    {
        const auto outcome = run_cli("control", instance, words);
        require_exit(outcome, 2, "vss_cli control token create");
        REQUIRE(contains(outcome.output, "vss_cli control token create"));
    }

    // A scope that the gateway never hands out is its refusal, not a usage
    // error.
    const auto admin = run_cli(
        "control", instance, {"token", "create", "--label=x", "--scope=admin"});
    require_exit(admin, 1, "vss_cli control token create --scope=admin");
    REQUIRE(!find_token(admin.output).has_value());
}

TEST_CASE("system control: a token written to a file is not shown")
{
    gate_process gate(unique_instance());
    const std::string& instance = gate.instance();

    const std::filesystem::path file = gate.scratch_dir() / "out.token";
    const auto made = run_cli("control", instance,
                              {"token", "create", "--label=filed",
                               "--scope=monitor,monitor.data", "--days=7",
                               "--out=" + file.string()});
    require_exit(made, 0, "vss_cli control token create --out");
    REQUIRE(contains(made.output, "Token written to"));
    REQUIRE(!contains(made.output, "vsst1_"));
    REQUIRE(!contains(made.output, "only time"));

    const std::string text = read_text(file);
    REQUIRE_EQ(text.size(), vss::control::token_size + 1);
    REQUIRE(text.ends_with("\n"));
    const std::string token = text.substr(0, vss::control::token_size);
    const std::string id = id_of(token);

    // The file holds the real token: it opens a session.
    const auto status = run_cli("control", instance,
                                {"status", "--token-file=" + file.string()});
    require_exit(status, 0, "vss_cli control status --token-file");
    REQUIRE(contains(status.output, "You are: token:" + id));
    REQUIRE(contains(status.output, "monitor.data"));
}

TEST_CASE("system control: no log, output or file holds a token")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    gate_process gate(instance);

    // What every command but the one that makes the token printed.
    std::vector<std::pair<std::string, std::string>> printed;
    const auto run_and_keep =
        [&printed, &instance](const std::string& what,
                              const std::vector<std::string>& words,
                              int expected)
    {
        const auto outcome = run_cli("control", instance, words);
        require_exit(outcome, expected, what.c_str());
        printed.emplace_back(what, outcome.output);
    };

    const std::string token = create_token(instance, "canary", "control");
    const std::string id = id_of(token);
    const std::filesystem::path good_file = gate.scratch_dir() / "good.token";
    write_text(good_file, token + "\n");

    // The same token but for its last character, which is changed to
    // another one of the alphabet.
    std::string wrong = token;
    wrong.back() = wrong.back() == 'a' ? 'b' : 'a';
    const std::filesystem::path wrong_file = gate.scratch_dir() / "wrong.token";
    write_text(wrong_file, wrong + "\n");

    run_and_keep("status with the token",
                 {"status", "--token-file=" + good_file.string()}, 0);
    run_and_keep("status with a wrong token",
                 {"status", "--token-file=" + wrong_file.string()}, 1);
    run_and_keep("list", {"token", "list"}, 0);
    run_and_keep("revoke", {"token", "revoke", id}, 0);
    run_and_keep("status with the revoked token",
                 {"status", "--token-file=" + good_file.string()}, 1);
    run_and_keep("stop", {"stop"}, 0);

    REQUIRE(gate.child().wait(long_limit) == 0);
    const std::string gate_output = gate.child().read_all(5000ms);
    agent.child().request_stop();
    if (!agent.child().wait(long_limit).has_value())
    {
        agent.child().kill();
    }
    const std::string agent_output = agent.child().read_all(5000ms);

    const std::vector<needle> needles = needles_for(token);
    for (const auto& [what, text] : printed)
    {
        require_absent(needles, text, "the output of " + what);
    }
    require_absent(needles, gate_output, "the output of the gateway");
    require_absent(needles, agent_output, "the output of the agent");

    // No file the gateway keeps holds it either: the audit log has its id
    // and the token file a hash.
    const std::size_t looked_at =
        require_files_free_of(gate.audit_dir(), needles);
    REQUIRE(looked_at >= 2U);

    // The log that was searched is one that recorded the events.
    const std::string audit = read_text(gate.audit_dir() / "audit.jsonl");
    REQUIRE(contains(audit, "\"event\":\"auth.failed\""));
    REQUIRE(contains(audit, id));
    REQUIRE(contains(read_text(gate.audit_dir() / "tokens.json"), id));
}
