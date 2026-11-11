//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli agent ...` talks to the user's `vss_agent`.  Unlike every other
// command of this program it makes no studio of its own: a port it creates
// belongs to the agent and is still there when this process has exited.
//

#include "agent_command.hpp"

#include <vss/agent/client.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/core/bytes.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss_cli
{
namespace
{

using vss::agent::client;

constexpr int exit_ok = 0;
constexpr int exit_refused = 1;
constexpr int exit_usage = 2;
constexpr int exit_not_running = 3;

struct agent_arguments
{
    std::string action;
    std::vector<std::string> names;
    std::string instance;
    int seconds = 0;
    bool one_way = false;
    bool hex = false;
    bool data = false;
    bool valid = true;
};

/// The whole of `text` as a number of seconds; nothing for anything else.
std::optional<int> parse_seconds(std::string_view text)
{
    int seconds = 0;
    const auto [stopped, failed] =
        std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (failed != std::errc{} || stopped != text.data() + text.size() ||
        seconds < 0)
        return std::nullopt;
    return seconds;
}

agent_arguments parse(const std::vector<std::string>& arguments)
{
    agent_arguments parsed;
    constexpr std::string_view instance_option = "--instance=";
    constexpr std::string_view seconds_option = "--seconds=";
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string_view word = arguments[index];
        if (word.starts_with(instance_option))
            parsed.instance = word.substr(instance_option.size());
        else if (word.starts_with(seconds_option))
        {
            const auto seconds =
                parse_seconds(word.substr(seconds_option.size()));
            parsed.seconds = seconds.value_or(0);
            parsed.valid = parsed.valid && seconds.has_value();
        }
        else if (word == "--one-way")
            parsed.one_way = true;
        else if (word == "--hex")
            parsed.hex = true;
        else if (word == "--data")
            parsed.data = true;
        else if (word.starts_with("--"))
            parsed.valid = false;
        else if (parsed.action.empty())
            parsed.action = word;
        else
            parsed.names.emplace_back(word);
    }
    return parsed;
}

int report(const char* what, const vss::error_code& error, const client* agent)
{
    // The agent's own words when it sent some; the code's otherwise.
    const std::string message =
        agent != nullptr && !agent->last_error_message().empty()
            ? agent->last_error_message()
            : error.message();
    std::fprintf(stderr, "agent %s failed: %s\n", what, message.c_str());
    return exit_refused;
}

/// The id of the port called `name`.
std::optional<vss::endpoint_id> find_port(const vss::agent::list_reply& listed,
                                          const std::string& name)
{
    for (const auto& port : listed.ports)
    {
        if (port.name == name)
            return port.id;
    }
    return std::nullopt;
}

void print_list(const vss::agent::list_reply& listed)
{
    std::printf("ports: %zu\n", listed.ports.size());
    for (const auto& port : listed.ports)
    {
        std::printf("  [%llu] %-14s %-10s %-24s %-8s rx=%llu tx=%llu\n",
                    static_cast<unsigned long long>(port.id), port.name.c_str(),
                    vss::to_string(port.kind), port.path.c_str(),
                    vss::to_string(port.state),
                    static_cast<unsigned long long>(port.bytes_rx),
                    static_cast<unsigned long long>(port.bytes_tx));
    }
    std::printf("links: %zu\n", listed.links.size());
    for (const auto& link : listed.links)
    {
        std::printf("  [%llu] %-8s %zu -> %zu  bytes=%llu\n",
                    static_cast<unsigned long long>(link.id),
                    vss::to_string(link.mode), link.sources.size(),
                    link.targets.size(),
                    static_cast<unsigned long long>(link.bytes));
    }
}

int list_ports(client& agent)
{
    const auto listed = agent.list();
    if (!listed)
        return report("ports", listed.error(), &agent);
    print_list(*listed);
    return exit_ok;
}

int create_port(client& agent, const agent_arguments& args)
{
    if (args.names.size() != 1)
        return exit_usage;
    const auto created = agent.create_port(args.names[0]);
    if (!created)
        return report("create-port", created.error(), &agent);
    std::printf("created port %s [%llu]\n", args.names[0].c_str(),
                static_cast<unsigned long long>(*created));
    return exit_ok;
}

int remove_port(client& agent, const agent_arguments& args)
{
    if (args.names.size() != 1)
        return exit_usage;
    const auto listed = agent.list();
    if (!listed)
        return report("remove-port", listed.error(), &agent);
    const auto port = find_port(*listed, args.names[0]);
    if (!port)
        return report("remove-port", vss::make_error_code(vss::errc::not_found),
                      nullptr);
    const auto removed = agent.remove_port(*port);
    if (!removed)
        return report("remove-port", removed.error(), &agent);
    std::printf("removed port %s\n", args.names[0].c_str());
    return exit_ok;
}

int connect_ports(client& agent, const agent_arguments& args)
{
    if (args.names.size() != 2)
        return exit_usage;
    const auto listed = agent.list();
    if (!listed)
        return report("connect", listed.error(), &agent);
    const auto first = find_port(*listed, args.names[0]);
    const auto second = find_port(*listed, args.names[1]);
    if (!first || !second)
        return report("connect", vss::make_error_code(vss::errc::not_found),
                      nullptr);
    const auto linked = agent.connect_ports(*first, *second, args.one_way);
    if (!linked)
        return report("connect", linked.error(), &agent);
    std::printf("linked %s %s %s [%llu]\n", args.names[0].c_str(),
                args.one_way ? "->" : "<->", args.names[1].c_str(),
                static_cast<unsigned long long>(*linked));
    return exit_ok;
}

int write_port(client& agent, const agent_arguments& args)
{
    if (args.names.size() != 2)
        return exit_usage;
    vss::byte_buffer bytes;
    if (args.hex)
    {
        auto parsed = vss::from_hex(args.names[1]);
        if (!parsed)
            return report("write", parsed.error(), nullptr);
        bytes = std::move(*parsed);
    }
    else
    {
        bytes = vss::to_bytes(args.names[1]);
    }
    const auto listed = agent.list();
    if (!listed)
        return report("write", listed.error(), &agent);
    const auto port = find_port(*listed, args.names[0]);
    if (!port)
        return report("write", vss::make_error_code(vss::errc::not_found),
                      nullptr);
    const auto written = agent.write(*port, bytes);
    if (!written)
        return report("write", written.error(), &agent);
    std::printf("wrote %zu bytes to %s\n", bytes.size(), args.names[0].c_str());
    return exit_ok;
}

/// Prints the batches of events as they arrive, for `seconds` or, with no
/// time given, until the agent goes away.
int watch(client& agent, const agent_arguments& args)
{
    constexpr std::uint32_t interval_ms = 100;
    const auto subscribed = agent.subscribe(args.data, interval_ms);
    if (!subscribed)
        return report("watch", subscribed.error(), &agent);
    const auto started = std::chrono::steady_clock::now();
    for (;;)
    {
        if (args.seconds > 0 && std::chrono::steady_clock::now() - started >=
                                    std::chrono::seconds(args.seconds))
            return exit_ok;
        const auto batch =
            agent.next_events(std::chrono::milliseconds(interval_ms));
        if (!batch)
        {
            if (batch.error() == vss::errc::timeout)
                continue;
            return report("watch", batch.error(), &agent);
        }
        for (const auto& event : batch->events)
        {
            std::printf("%-8s %-14s", vss::to_string(event.kind),
                        event.name.c_str());
            if (event.kind == vss::event_kind::data)
            {
                std::printf(" %s %llu bytes", vss::to_string(event.direction),
                            static_cast<unsigned long long>(event.size));
                if (!event.data.empty())
                    std::printf(": %s", vss::to_hex(event.data).c_str());
            }
            if (!event.message.empty())
                std::printf(" %s", event.message.c_str());
            if (!event.error.message.empty())
                std::printf(" (%s)", event.error.message.c_str());
            std::printf("\n");
        }
        if (batch->dropped != 0)
            std::printf("(%llu events were dropped: this reader was too "
                        "slow)\n",
                        static_cast<unsigned long long>(batch->dropped));
        std::fflush(stdout);
    }
}

int status(const client& agent)
{
    const auto& hello = agent.agent();
    std::printf("agent %s, protocol %u, process %llu\n",
                hello.agent_version.c_str(), hello.protocol,
                static_cast<unsigned long long>(hello.process_id));
    std::printf("port kinds:");
    for (const auto& kind : hello.port_kinds)
        std::printf(" %s", kind.c_str());
    std::printf("\n");
    return exit_ok;
}

int stop(client& agent)
{
    const auto stopped = agent.shutdown();
    if (!stopped)
        return report("stop", stopped.error(), &agent);
    std::puts("the agent is stopping");
    return exit_ok;
}

int run(client& agent, const agent_arguments& args)
{
    if (args.action == "status")
        return status(agent);
    if (args.action == "ports")
        return list_ports(agent);
    if (args.action == "create-port")
        return create_port(agent, args);
    if (args.action == "remove-port")
        return remove_port(agent, args);
    if (args.action == "connect")
        return connect_ports(agent, args);
    if (args.action == "write")
        return write_port(agent, args);
    if (args.action == "watch")
        return watch(agent, args);
    if (args.action == "stop")
        return stop(agent);
    return exit_usage;
}

} // namespace

const char* agent_usage() noexcept
{
    return "  vss_cli agent status|ports|stop [--instance=<name>]\n"
           "  vss_cli agent create-port <name>\n"
           "  vss_cli agent remove-port <name>\n"
           "  vss_cli agent connect <first> <second> [--one-way]\n"
           "  vss_cli agent write <port> <text> [--hex]\n"
           "  vss_cli agent watch [--data] [--seconds=N]\n"
           "      Work with the ports of this user's vss_agent, which keeps\n"
           "      them when this command has exited (start it with "
           "`vss_agent`).\n"
           "      create-port makes a loopback port; connect links two "
           "ports\n"
           "      both ways, or from the first to the second only; watch\n"
           "      prints the agent's events, and with --data the bytes.\n"
           "      Exit code 3 means no agent is running.\n"
           "\n";
}

int command_agent(const std::vector<std::string>& arguments)
{
    const agent_arguments args = parse(arguments);
    if (!args.valid || args.action.empty())
    {
        std::fputs(agent_usage(), stderr);
        return exit_usage;
    }

    vss::agent::client_options options;
    options.instance = args.instance;
    options.name = "vss_cli";
    auto connected = client::connect(options);
    if (!connected)
    {
        if (connected.error() == vss::agent::protocol_errc::not_running)
        {
            std::fputs("no agent is running for this user; start vss_agent\n",
                       stderr);
            return exit_not_running;
        }
        std::fprintf(stderr, "cannot reach the agent: %s\n",
                     connected.error().message().c_str());
        return exit_refused;
    }

    const int code = run(**connected, args);
    if (code == exit_usage)
        std::fputs(agent_usage(), stderr);
    return code;
}

} // namespace vss_cli
