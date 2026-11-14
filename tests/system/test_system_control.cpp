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
    /// `more` are further options of `vss_gate`, such as `--local-role=`.
    explicit gate_process(std::string name,
                          const std::vector<std::string>& more = {})
        : instance_(std::move(name)), audit_dir_(scratch_.path() / "audit"),
          child_(VSS_GATE_PROGRAM, arguments_for(instance_, audit_dir_, more))
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
    static std::vector<std::string>
    arguments_for(const std::string& instance,
                  const std::filesystem::path& audit_dir,
                  const std::vector<std::string>& more)
    {
        std::vector<std::string> arguments{"--instance=" + instance,
                                           "--agent-instance=" + instance,
                                           "--audit-dir=" + audit_dir.string()};
        arguments.insert(arguments.end(), more.begin(), more.end());
        return arguments;
    }

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

/// Starts `vss_cli <group> <words> --instance=<instance>` and leaves it
/// running.  The child is killed when the pointer goes.
std::unique_ptr<child_process> start_cli(const std::string& group,
                                         const std::string& instance,
                                         const std::vector<std::string>& words)
{
    std::vector<std::string> arguments{group};
    for (const auto& word : words)
        arguments.push_back(word);
    arguments.push_back("--instance=" + instance);
    return std::make_unique<child_process>(VSS_AGENT_CLI_PROGRAM, arguments);
}

/// Runs `vss_cli <group> <words> --instance=<instance>` to its end.  `code` is
/// empty when it did not end within the limit (the child is then killed).
cli_result run_cli(const std::string& group, const std::string& instance,
                   const std::vector<std::string>& words)
{
    const auto child = start_cli(group, instance, words);
    cli_result outcome;
    // The output is read first: a command that says more than a pipe holds,
    // as the usage text does, waits for its reader before it ends.
    outcome.output = child->read_all(long_limit);
    outcome.code = child->wait(long_limit);
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

// -- watching ----------------------------------------------------------------

/// The words of a line, which are separated by single spaces.
std::vector<std::string> words_of(const std::string& line)
{
    std::vector<std::string> words;
    std::istringstream stream(line);
    std::string word;
    while (stream >> word)
        words.push_back(word);
    return words;
}

/// The lines of `text` without their line endings.
std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

bool all_digits(const std::string& text)
{
    return !text.empty() &&
           text.find_first_not_of("0123456789") == std::string::npos;
}

/// A line of `control watch`:
/// `<batch> <time ns> <kind> <port> <direction> <size> <hex>`.
struct event_line
{
    std::string batch;
    std::string time_ns;
    std::string kind;
    std::string port;
    std::string direction;
    std::string size;
    std::string bytes;
};

/// The line as an event; fails the test for a line that is not one.
event_line parse_event_line(const std::string& line)
{
    const std::vector<std::string> words = words_of(line);
    if (words.size() != 7 || !all_digits(words[0]) || !all_digits(words[1]) ||
        !all_digits(words[5]))
    {
        throw vss_test::failure("not a line of control watch: \"" + line +
                                "\"");
    }
    return {words[0], words[1], words[2], words[3],
            words[4], words[5], words[6]};
}

/// The events in the output of `control watch`, which has nothing else.
std::vector<event_line> parse_events(const std::string& output)
{
    std::vector<event_line> events;
    for (const std::string& line : lines_of(output))
        events.push_back(parse_event_line(line));
    return events;
}

/// True when an event is `size` bytes of `direction` on `port`, and `bytes`
/// is what the line says about them.
bool has_bytes(const std::vector<event_line>& events, const std::string& port,
               const std::string& direction, const std::string& size,
               const std::string& bytes)
{
    for (const event_line& event : events)
    {
        if (event.kind == "data" && event.port == port &&
            event.direction == direction && event.size == size &&
            event.bytes == bytes)
            return true;
    }
    return false;
}

/// The rows of `control sessions`, each split into its words: number,
/// principal, transport, events, traffic, requests, events sent, events
/// lost, process and the name of the client.
std::vector<std::vector<std::string>> session_rows(const std::string& output)
{
    std::vector<std::vector<std::string>> rows;
    for (const std::string& line : lines_of(output))
    {
        std::vector<std::string> row = words_of(line);
        if (row.size() < 10 || !all_digits(row[0]))
        {
            throw vss_test::failure("not a line of control sessions: \"" +
                                    line + "\"");
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

/// The row of the session with this number, if it is listed.
std::optional<std::vector<std::string>> find_session(const std::string& output,
                                                     const std::string& number)
{
    for (const auto& row : session_rows(output))
    {
        if (row[0] == number)
            return row;
    }
    return std::nullopt;
}

/// Asks `control sessions` until at least `count` sessions of programs that
/// are not administration have a stream.  Returns their numbers.  This is
/// what a test waits for before it makes something happen: a watch that has
/// not subscribed yet would miss it.
std::vector<std::string> wait_for_streams(const std::string& instance,
                                          std::size_t count)
{
    std::string seen;
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto listed = run_cli("control", instance, {"sessions"});
        require_exit(listed, 0, "vss_cli control sessions");
        seen = listed.output;
        std::vector<std::string> numbers;
        for (const auto& row : session_rows(seen))
        {
            if (row[2] != "local-admin" && (row[3] == "yes" || row[4] == "yes"))
                numbers.push_back(row[0]);
        }
        if (numbers.size() >= count)
            return numbers;
    }
    throw vss_test::failure("no " + std::to_string(count) +
                            " sessions with a stream; the last list: " + seen);
}

/// Makes the ports `first` and `second` and links them one way, from the
/// first to the second.
void make_linked_pair(const std::string& instance, const std::string& first,
                      const std::string& second)
{
    create_port(instance, first);
    create_port(instance, second);
    require_exit(
        run_cli("agent", instance, {"connect", first, second, "--one-way"}), 0,
        "vss_cli agent connect");
}

/// The id of the port `name`, which is `p_` and a number.
std::string id_of_port(const std::string& instance, const std::string& name)
{
    auto script = connect_client(instance);
    const auto listed = script->list_ports();
    require_ok(listed, "listing the ports through the gateway");
    const auto* port = find_port(*listed, name);
    if (port == nullptr)
    {
        throw vss_test::failure("the gateway lists no port called " + name);
    }
    return port->id;
}

/// Writes `bytes_hex` to the port `name` through the agent.
void write_hex(const std::string& instance, const std::string& name,
               const std::string& bytes_hex)
{
    require_exit(
        run_cli("agent", instance, {"write", name, bytes_hex, "--hex"}), 0,
        "vss_cli agent write");
}

/// Waits for `watch` to end and returns what it printed.  Fails the test
/// unless it ended with `expected`.
std::string finish_watch(child_process& watch, int expected, const char* what)
{
    const std::optional<int> code = watch.wait(long_limit);
    const std::string output = watch.read_all(5000ms);
    if (code != expected)
    {
        throw vss_test::failure(std::string(what) + " did not exit with " +
                                std::to_string(expected) +
                                "; output: " + output);
    }
    return output;
}

// -- changing ports ----------------------------------------------------------

/// The one line of `output`, which is an id: `p_` or `l_` and a number.  Fails
/// the test for anything else.
std::string id_line(const cli_result& outcome, const std::string& prefix,
                    const char* what)
{
    require_exit(outcome, 0, what);
    const std::vector<std::string> lines = lines_of(outcome.output);
    if (lines.size() != 1 || !lines[0].starts_with(prefix) ||
        !all_digits(lines[0].substr(prefix.size())))
    {
        throw vss_test::failure(
            std::string(what) +
            " did not print one id; output: " + outcome.output);
    }
    return lines[0];
}

/// Makes the port `name` with `vss_cli control port create`, with the options
/// in `more`, and returns its id.
std::string control_create_port(const std::string& instance,
                                const std::string& name,
                                const std::vector<std::string>& more = {})
{
    std::vector<std::string> words{"port", "create", name};
    words.insert(words.end(), more.begin(), more.end());
    return id_line(run_cli("control", instance, words), "p_",
                   "vss_cli control port create");
}

/// True when `vss_cli agent ports` lists a port called `name`.
bool agent_lists_port(const std::string& instance, const std::string& name)
{
    const auto ports = run_cli("agent", instance, {"ports"});
    require_exit(ports, 0, "vss_cli agent ports");
    return contains(ports.output, name);
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
    REQUIRE(text.ends_with('\n'));
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

TEST_CASE("system control: watch --data prints the bytes of linked ports")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    gate_process gate(instance, {"--local-role=control"});
    make_linked_pair(instance, "a", "b");
    const std::string a_id = id_of_port(instance, "a");
    const std::string b_id = id_of_port(instance, "b");

    // One watch hears every port; the other only the second one.
    const auto everything = start_cli(
        "control", instance, {"watch", "--data", "--count=4", "--interval=20"});
    const auto only_b = start_cli(
        "control", instance,
        {"watch", "--data", "--ports=" + b_id, "--count=2", "--interval=20"});
    REQUIRE_EQ(wait_for_streams(instance, 2).size(), std::size_t{2});

    // Bytes that a program that reads lines would trip over.
    const std::string sent = "00ff0a20";
    write_hex(instance, "a", sent);

    const auto all_events = parse_events(
        finish_watch(*everything, 0, "vss_cli control watch --data"));
    REQUIRE_EQ(all_events.size(), std::size_t{4});
    REQUIRE(has_bytes(all_events, a_id, "tx", "4", sent));
    REQUIRE(has_bytes(all_events, b_id, "rx", "4", sent));

    const auto b_events =
        parse_events(finish_watch(*only_b, 0, "vss_cli control watch --ports"));
    REQUIRE_EQ(b_events.size(), std::size_t{2});
    for (const event_line& event : b_events)
    {
        REQUIRE_EQ(event.port, b_id);
    }
    REQUIRE(has_bytes(b_events, b_id, "rx", "4", sent));
}

TEST_CASE("system control: a monitor watches without the bytes")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    // The local user holds the scope monitor and nothing else.
    gate_process gate(instance, {"--local-role=monitor"});
    make_linked_pair(instance, "a", "b");
    const std::string b_id = id_of_port(instance, "b");

    const auto refused =
        run_cli("control", instance, {"watch", "--data", "--count=1"});
    require_exit(refused, 1, "vss_cli control watch --data as a monitor");
    REQUIRE(contains(refused.output, "forbidden"));
    REQUIRE(contains(refused.output, "control watch failed"));
    // A refusal is a message on the error output and no event line.
    REQUIRE(!contains(refused.output, " data "));

    const auto watch =
        start_cli("control", instance, {"watch", "--count=4", "--interval=20"});
    REQUIRE_EQ(wait_for_streams(instance, 1).size(), std::size_t{1});
    write_hex(instance, "a", "68656c6c6f");

    const auto events =
        parse_events(finish_watch(*watch, 0, "vss_cli control watch"));
    REQUIRE_EQ(events.size(), std::size_t{4});
    bool saw_b = false;
    for (const event_line& event : events)
    {
        // The bytes moved, and the line says how many, and not which.
        REQUIRE_EQ(event.kind, std::string("data"));
        REQUIRE_EQ(event.size, std::string("5"));
        REQUIRE_EQ(event.bytes, std::string("-"));
        saw_b = saw_b || (event.port == b_id && event.direction == "rx");
    }
    REQUIRE(saw_b);
}

TEST_CASE("system control: sessions lists a watch and kick ends it")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    gate_process gate(instance);

    const auto watch =
        start_cli("control", instance, {"watch", "--interval=20"});
    const std::vector<std::string> numbers = wait_for_streams(instance, 1);
    REQUIRE_EQ(numbers.size(), std::size_t{1});
    const std::string& number = numbers[0];

    const auto listed = run_cli("control", instance, {"sessions"});
    require_exit(listed, 0, "vss_cli control sessions");
    const auto row = find_session(listed.output, number);
    REQUIRE(row.has_value());
    REQUIRE_EQ((*row)[2], std::string("local"));
    REQUIRE_EQ((*row)[3], std::string("yes"));
    REQUIRE_EQ((*row)[4], std::string("no"));
    REQUIRE_EQ(row->back(), std::string("vss_cli"));
    // The one that asked is there too, on the administration endpoint.
    bool saw_admin = false;
    for (const auto& other : session_rows(listed.output))
    {
        saw_admin = saw_admin || other[2] == "local-admin";
    }
    REQUIRE(saw_admin);

    const auto kicked = run_cli("control", instance, {"kick", number});
    require_exit(kicked, 0, "vss_cli control kick");
    REQUIRE(kicked.output.empty());

    // The watch is over, with an error and not as if it were done.
    const std::optional<int> code = watch->wait(long_limit);
    const std::string output = watch->read_all(5000ms);
    REQUIRE(code.has_value());
    REQUIRE(*code != 0);
    REQUIRE(contains(output, "control watch failed"));

    // Nothing lists the session any more.
    bool listed_still = true;
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    while (listed_still && std::chrono::steady_clock::now() < deadline)
    {
        const auto again = run_cli("control", instance, {"sessions"});
        require_exit(again, 0, "vss_cli control sessions");
        listed_still = find_session(again.output, number).has_value();
    }
    REQUIRE(!listed_still);

    // The number is gone, so kicking it says so, and a word that is no
    // number is not a command.
    const auto missing = run_cli("control", instance, {"kick", number});
    require_exit(missing, 1, "vss_cli control kick of a session that is gone");
    REQUIRE(contains(missing.output, "no session has that number"));
    const auto unusable = run_cli("control", instance, {"kick", "first"});
    require_exit(unusable, 2, "vss_cli control kick of a word");
    REQUIRE(contains(unusable.output, "vss_cli control kick"));
}

TEST_CASE("system control: ports are made, linked, written to and removed")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    gate_process gate(instance, {"--local-role=control"});

    const std::string a_id =
        control_create_port(instance, "alpha", {"--kind=loopback"});
    const std::string b_id = control_create_port(instance, "beta");
    REQUIRE(a_id != b_id);
    REQUIRE(agent_lists_port(instance, "alpha"));
    REQUIRE(agent_lists_port(instance, "beta"));

    const auto linked =
        run_cli("control", instance, {"link", a_id, b_id, "--one-way"});
    const std::string link_id =
        id_line(linked, "l_", "vss_cli control link --one-way");
    REQUIRE(!link_id.empty());

    // The watch hears only the second port; it is subscribed when its
    // session shows a stream, and not before.
    const auto watch = start_cli(
        "control", instance,
        {"watch", "--data", "--ports=" + b_id, "--count=1", "--interval=20"});
    REQUIRE_EQ(wait_for_streams(instance, 1).size(), std::size_t{1});

    const std::string sent = "00ff68690d0a";
    const auto wrote =
        run_cli("control", instance, {"send", a_id, "--hex=" + sent});
    require_exit(wrote, 0, "vss_cli control send");
    REQUIRE(wrote.output.empty());

    const auto events =
        parse_events(finish_watch(*watch, 0, "vss_cli control watch --data"));
    REQUIRE_EQ(events.size(), std::size_t{1});
    REQUIRE_EQ(events[0].port, b_id);
    // A loopback port both receives the bytes and gives them back, so the
    // first event on it may be either; the bytes are the same.
    REQUIRE_EQ(events[0].kind, std::string("data"));
    REQUIRE_EQ(events[0].size, std::string("6"));
    REQUIRE_EQ(events[0].bytes, sent);

    // Removing a port succeeds once and says no the second time, with the
    // name of the engine's error.
    const auto removed = run_cli("control", instance, {"port", "remove", a_id});
    require_exit(removed, 0, "vss_cli control port remove");
    REQUIRE(removed.output.empty());
    REQUIRE(!agent_lists_port(instance, "alpha"));
    REQUIRE(agent_lists_port(instance, "beta"));

    const auto again = run_cli("control", instance, {"port", "remove", a_id});
    require_exit(again, 1, "vss_cli control port remove of a removed port");
    REQUIRE(contains(again.output, "control port remove failed"));
    REQUIRE(contains(again.output, "engine.not_found"));

    // An id that is no id is the gateway's refusal, not the engine's.
    const auto not_an_id =
        run_cli("control", instance, {"port", "remove", "alpha"});
    require_exit(not_an_id, 1, "vss_cli control port remove of a name");
    REQUIRE(contains(not_an_id.output, "invalid_argument"));
}

TEST_CASE("system control: a monitor can change nothing")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    // The default role holds the scope monitor and nothing else.
    gate_process gate(instance);

    const std::vector<std::vector<std::string>> refused{
        {"port", "create", "denied"},
        {"port", "remove", "p_1"},
        {"link", "p_1", "p_2"},
        {"send", "p_1", "--hex=00"}};
    for (const auto& words : refused)
    {
        const auto outcome = run_cli("control", instance, words);
        require_exit(outcome, 1, "vss_cli control as a monitor");
        REQUIRE(contains(outcome.output, "forbidden"));
        REQUIRE(contains(outcome.output, "control " + words[0]));
    }
    REQUIRE(!agent_lists_port(instance, "denied"));

    // The gateway is still there, and so is its answer to a monitor.
    const auto status = run_cli("control", instance, {"status"});
    require_exit(status, 0, "vss_cli control status");
    REQUIRE(contains(status.output, "You may: monitor"));
}

TEST_CASE("system control: expect waits for the bytes that a send brings")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    gate_process gate(instance, {"--local-role=control"});

    const std::string a_id =
        control_create_port(instance, "alpha", {"--kind=loopback"});
    const std::string b_id = control_create_port(instance, "beta");
    const auto linked =
        run_cli("control", instance, {"link", a_id, b_id, "--one-way"});
    static_cast<void>(id_line(linked, "l_", "vss_cli control link --one-way"));

    // `noise OK` and a line ending is sent to the first port; the pattern
    // is `OK` and a line ending, so six bytes go by before it.  The gateway
    // listens before it sends, so nothing is missed.
    const auto matched =
        run_cli("control", instance,
                {"expect", b_id, "--pattern=4f4b0d0a", "--send-to=" + a_id,
                 "--send=6e6f697365204f4b0d0a", "--timeout=20000"});
    require_exit(matched, 0, "vss_cli control expect");
    const std::vector<std::string> lines = lines_of(matched.output);
    REQUIRE_EQ(lines.size(), std::size_t{1});
    const std::vector<std::string> words = words_of(lines[0]);
    REQUIRE_EQ(words.size(), std::size_t{4});
    REQUIRE_EQ(words[0], std::string("matched"));
    REQUIRE_EQ(words[1], std::string("6"));
    REQUIRE(all_digits(words[2]));
    REQUIRE_EQ(words[3], std::string("0"));

    // A pattern that does not come is a failure of the command with the
    // name of the gateway's answer.
    const auto missed =
        run_cli("control", instance,
                {"expect", b_id, "--pattern=4e45564552", "--send-to=" + a_id,
                 "--send=00", "--timeout=200"});
    require_exit(missed, 1, "vss_cli control expect of a pattern that is late");
    REQUIRE(contains(missed.output, "control expect failed"));
    REQUIRE(contains(missed.output, "timeout"));
    REQUIRE(!contains(missed.output, "matched"));
}

TEST_CASE("system control: a monitor may not wait for bytes")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    // The default role holds the scope monitor, which is not monitor.data.
    gate_process gate(instance);

    const auto refused =
        run_cli("control", instance,
                {"expect", "p_1", "--pattern=00", "--timeout=100"});
    require_exit(refused, 1, "vss_cli control expect as a monitor");
    REQUIRE(contains(refused.output, "forbidden"));
    REQUIRE(contains(refused.output, "control expect failed"));
}

TEST_CASE("system control: a run is filled by separate commands and comes "
          "back as a JUnit report")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    gate_process gate(instance, {"--local-role=control"});

    const std::string a_id =
        control_create_port(instance, "alpha", {"--kind=loopback"});
    const std::string b_id = control_create_port(instance, "beta");
    const auto linked =
        run_cli("control", instance, {"link", a_id, b_id, "--one-way"});
    static_cast<void>(id_line(linked, "l_", "vss_cli control link --one-way"));

    // Every step is a process of its own: the run belongs to the user, not
    // to a connection.
    const std::string run_id =
        id_line(run_cli("control", instance, {"run", "begin", "smoke"}), "r_",
                "vss_cli control run begin");

    const auto answered =
        run_cli("control", instance,
                {"expect", b_id, "--pattern=4f4b", "--send-to=" + a_id,
                 "--send=4f4b", "--timeout=20000", "--label=it answers"});
    require_exit(answered, 0, "vss_cli control expect --label");

    const auto silent =
        run_cli("control", instance,
                {"expect", b_id, "--pattern=4e4f", "--send-to=" + a_id,
                 "--send=00", "--timeout=200"});
    REQUIRE(silent.code.has_value() && silent.code != 0);

    const auto noted = run_cli("control", instance,
                               {"run", "record", "by hand", "--outcome=skipped",
                                "--message=nobody looked"});
    require_exit(noted, 0, "vss_cli control run record");
    REQUIRE(noted.output.empty());

    // The command reports the counts and does not judge them: the exit code
    // is 0 although one check failed.
    const auto ended = run_cli("control", instance, {"run", "end"});
    require_exit(ended, 0, "vss_cli control run end");
    const std::vector<std::string> end_lines = lines_of(ended.output);
    REQUIRE_EQ(end_lines.size(), std::size_t{1});
    const std::vector<std::string> end_words = words_of(end_lines[0]);
    REQUIRE_EQ(end_words.size(), std::size_t{6});
    REQUIRE_EQ(end_words[0], run_id);
    REQUIRE_EQ(end_words[1], std::string("tests=3"));
    REQUIRE_EQ(end_words[2], std::string("failures=1"));
    REQUIRE_EQ(end_words[3], std::string("errors=0"));
    REQUIRE_EQ(end_words[4], std::string("skipped=1"));
    const std::string duration_key = "duration_ms=";
    REQUIRE(end_words[5].starts_with(duration_key));
    REQUIRE(all_digits(end_words[5].substr(duration_key.size())));

    // The report in a file, and the same report on the screen.
    const std::filesystem::path file = gate.scratch_dir() / "report.xml";
    const auto written = run_cli("control", instance,
                                 {"run", "report", "--out=" + file.string()});
    require_exit(written, 0, "vss_cli control run report --out");
    REQUIRE(written.output.empty());
    const std::string xml = read_text(file);
    REQUIRE(contains(xml, "<testsuite name=\"smoke\" tests=\"3\" "
                          "failures=\"1\" errors=\"0\" skipped=\"1\""));
    REQUIRE(contains(xml, "<testcase name=\"it answers\""));
    REQUIRE(contains(xml, "<skipped message=\"nobody looked\"/>"));

    const auto shown = run_cli("control", instance, {"run", "report", run_id});
    require_exit(shown, 0, "vss_cli control run report <id>");
    // A text stream on Windows ends a line with a carriage return and a line
    // feed; the report holds no carriage return of its own.
    std::string shown_text = shown.output;
    std::erase(shown_text, '\r');
    REQUIRE_EQ(shown_text, xml);

    // The run is closed: it cannot be ended twice, and a new one may begin.
    const auto again = run_cli("control", instance, {"run", "end"});
    require_exit(again, 1, "vss_cli control run end of a closed run");
    REQUIRE(contains(again.output, "not_found"));
    REQUIRE(contains(again.output, "control run end failed"));

    // One run is open at a time.
    static_cast<void>(
        id_line(run_cli("control", instance, {"run", "begin", "x"}), "r_",
                "vss_cli control run begin"));
    const auto second = run_cli("control", instance, {"run", "begin", "y"});
    require_exit(second, 1, "vss_cli control run begin of a second run");
    REQUIRE(contains(second.output, "conflict"));
}

TEST_CASE("system control: the commands on ports refuse a bad command line")
{
    // No gateway runs: a command line that is wrong ends in a usage error
    // before anything is connected, where a good one would find that no
    // gateway runs and exit with 3.
    const std::string instance = unique_instance();

    const std::vector<std::vector<std::string>> unusable{
        {"send", "p_1", "--hex=abc"},
        {"send", "p_1", "--hex="},
        {"send", "p_1", "--hex=zz"},
        {"send", "p_1", "--hex=00ff", "--hex=00ff"},
        {"send", "p_1", "--hex=0x00"},
        {"send", "p_1", "--hex=00 ff"},
        {"send", "p_1"},
        {"send", "--hex=00"},
        {"send", "p_1", "p_2", "--hex=00"},
        {"send", "p_1", "--hex=00", "--one-way"},
        {"send", "p_1", "--hex=00", "--kind=loopback"},
        {"port", "create"},
        {"port", "create", "a", "b"},
        {"port", "create", "a", "--kind="},
        {"port", "create", "a", "--hex=00"},
        {"port", "create", "a", "--one-way"},
        {"port", "remove"},
        {"port", "remove", "p_1", "p_2"},
        {"port", "remove", "p_1", "--kind=loopback"},
        {"port", "frobnicate", "p_1"},
        {"link", "p_1"},
        {"link"},
        {"link", "p_1", "p_2", "p_3"},
        {"link", "p_1", "p_2", "--hex=00"},
        {"link", "p_1", "p_2", "--kind=loopback"},
        {"link", "p_1", "p_2", "--one-way", "--one-way"},
        {"status", "--one-way"},
        {"sessions", "--hex=00"},
        {"watch", "--kind=loopback"},
        {"token", "list", "--one-way"},
        {"expect", "p_1", "--pattern=abc"},
        {"expect", "p_1", "--pattern="},
        {"expect", "p_1", "--pattern=zz"},
        {"expect", "p_1", "--pattern=00", "--pattern=00"},
        {"expect", "p_1"},
        {"expect", "p_1", "--timeout=100"},
        {"expect", "--pattern=00"},
        {"expect", "p_1", "p_2", "--pattern=00"},
        {"expect", "p_1", "--pattern=00", "--send=00"},
        {"expect", "p_1", "--pattern=00", "--send-to=p_2"},
        {"expect", "p_1", "--pattern=00", "--send-to=", "--send=00"},
        {"expect", "p_1", "--pattern=00", "--send-to=p_2", "--send="},
        {"expect", "p_1", "--pattern=00", "--send-to=p_2", "--send=0"},
        {"expect", "p_1", "--pattern=00", "--timeout=0"},
        {"expect", "p_1", "--pattern=00", "--timeout=x"},
        {"expect", "p_1", "--pattern=00", "--timeout="},
        {"expect", "p_1", "--pattern=00", "--timeout=-5"},
        {"expect", "p_1", "--pattern=00", "--timeout=100", "--timeout=100"},
        {"expect", "p_1", "--pattern=00", "--tx", "--tx"},
        {"expect", "p_1", "--pattern=00", "--hex=00"},
        {"expect", "p_1", "--pattern=00", "--kind=loopback"},
        {"expect", "p_1", "--pattern=00", "--one-way"},
        {"expect", "p_1", "--pattern=00", "--data"},
        {"expect", "p_1", "--pattern=00", "--ports=p_1"},
        {"send", "p_1", "--hex=00", "--pattern=00"},
        {"send", "p_1", "--hex=00", "--send=00"},
        {"link", "p_1", "p_2", "--send-to=p_3"},
        {"port", "remove", "p_1", "--timeout=100"},
        {"watch", "--tx"},
        {"status", "--timeout=100"},
        {"sessions", "--pattern=00"},
        {"kick", "1", "--tx"},
        {"token", "list", "--send=00"},
        {"run"},
        {"run", "begin"},
        {"run", "begin", "a", "b"},
        {"run", "begin", ""},
        {"run", "frobnicate"},
        {"run", "end", "extra"},
        {"run", "record", "x"},
        {"run", "record", "x", "--outcome=maybe"},
        {"run", "record", "x", "--outcome="},
        {"run", "record", "x", "--outcome=PASSED"},
        {"run", "record", "--outcome=passed"},
        {"run", "record", "x", "--outcome=passed", "--outcome=failed"},
        {"run", "record", "x", "--outcome=passed", "--duration=x"},
        {"run", "record", "x", "--outcome=passed", "--duration=-1"},
        {"run", "record", "x", "--outcome=passed", "--out=f.xml"},
        {"run", "report", "r_1", "r_2"},
        {"run", "report", "--out="},
        {"run", "report", "--outcome=passed"},
        {"run", "report", "--out=f.xml", "--out=g.xml"},
        {"run", "begin", "x", "--out=f.xml"},
        {"run", "begin", "x", "--message=m"},
        {"run", "begin", "x", "--label=l"},
        {"run", "begin", "x", "--pattern=00"},
        {"run", "end", "--duration=5"},
        {"expect", "p_1", "--pattern=00", "--label="},
        {"expect", "p_1", "--pattern=00", "--label=a", "--label=b"},
        {"expect", "p_1", "--pattern=00", "--outcome=passed"},
        {"expect", "p_1", "--pattern=00", "--out=f.xml"},
        {"expect", "p_1", "--pattern=00", "--role=monitor"},
        {"send", "p_1", "--hex=00", "--label=l"},
        {"status", "--outcome=passed"},
        {"token", "list", "--message=m"}};
    for (const auto& words : unusable)
    {
        const auto outcome = run_cli("control", instance, words);
        require_exit(outcome, 2, "vss_cli control with a bad command line");
        REQUIRE(contains(outcome.output, "vss_cli control"));
    }

    // The same words, right, get as far as looking for the gateway.
    const std::vector<std::vector<std::string>> usable{
        {"send", "p_1", "--hex=00FFab"},
        {"port", "create", "a", "--kind=loopback"},
        {"port", "remove", "p_1"},
        {"link", "p_1", "p_2", "--one-way"},
        {"expect", "p_1", "--pattern=00"},
        {"expect", "p_1", "--pattern=00FFab", "--timeout=100000", "--tx"},
        {"expect", "p_1", "--pattern=00", "--send-to=p_2", "--send=AB"},
        {"expect", "p_1", "--pattern=00", "--label=it answers"},
        {"run", "begin", "smoke"},
        {"run", "end"},
        {"run", "record", "by hand", "--outcome=skipped"},
        {"run", "record", "by hand", "--outcome=failed",
         "--message=", "--duration=0"},
        {"run", "report"},
        {"run", "report", "r_1"},
        {"run", "report", "--out=f.xml"},
        {"run", "report", "r_1", "--out=f.xml"}};
    for (const auto& words : usable)
    {
        const auto outcome = run_cli("control", instance, words);
        require_exit(outcome, 3, "vss_cli control with no gateway");
        REQUIRE(contains(outcome.output, "Remote control: off"));
    }
}

TEST_CASE("system control: a token with control.ports makes ports and no more")
{
    agent_process agent(unique_instance());
    const std::string& instance = agent.instance();
    // The local user is a monitor; the token is what may arrange ports.
    gate_process gate(instance);

    const std::filesystem::path file = gate.scratch_dir() / "ports.token";
    const auto made =
        run_cli("control", instance,
                {"token", "create", "--label=t",
                 "--scope=monitor,control.ports", "--out=" + file.string()});
    require_exit(made, 0, "vss_cli control token create --out");
    const std::string token_option = "--token-file=" + file.string();

    const std::string id =
        control_create_port(instance, "tokport", {token_option});
    REQUIRE(agent_lists_port(instance, "tokport"));
    const std::string other_id =
        control_create_port(instance, "tokother", {token_option});
    const auto linked =
        run_cli("control", instance, {"link", id, other_id, token_option});
    static_cast<void>(id_line(linked, "l_", "vss_cli control link"));

    // Bytes need control.data, which the token does not hold.
    const auto sent =
        run_cli("control", instance, {"send", id, "--hex=00", token_option});
    require_exit(sent, 1, "vss_cli control send with a token");
    REQUIRE(contains(sent.output, "forbidden"));
    REQUIRE(contains(sent.output, "control send failed"));

    // And the local user may not do what the token may.
    const auto refused = run_cli("control", instance, {"port", "remove", id});
    require_exit(refused, 1, "vss_cli control port remove as a monitor");
    REQUIRE(contains(refused.output, "forbidden"));
    REQUIRE(agent_lists_port(instance, "tokport"));

    const auto removed =
        run_cli("control", instance, {"port", "remove", id, token_option});
    require_exit(removed, 0, "vss_cli control port remove with a token");
    REQUIRE(!agent_lists_port(instance, "tokport"));
}
