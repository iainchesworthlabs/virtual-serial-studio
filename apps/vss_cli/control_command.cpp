//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli control ...` talks to the user's `vss_gate`.  Like `agent` it makes
// no studio of its own: it only asks the gateway, which asks the agent.
//

#include "control_command.hpp"

#include <vss/agent/proto/error.hpp>
#include <vss/control/client.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace vss_cli
{
namespace
{

using vss::control::client;

constexpr int exit_ok = 0;
constexpr int exit_refused = 1;
constexpr int exit_usage = 2;
constexpr int exit_not_running = 3;

struct control_arguments
{
    std::string action;
    std::string instance;
    bool valid = true;
};

control_arguments parse(const std::vector<std::string>& arguments)
{
    control_arguments parsed;
    constexpr std::string_view instance_option = "--instance=";
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string_view word = arguments[index];
        // An option this command does not have, or a second action.
        if (word.starts_with(instance_option))
            parsed.instance = word.substr(instance_option.size());
        else if (!word.starts_with("--") && parsed.action.empty())
            parsed.action = word;
        else
            parsed.valid = false;
    }
    return parsed;
}

int report(const char* what, const vss::error_code& error,
           const client* gateway)
{
    // The gateway's own words when it sent some; the code's otherwise.
    const std::string message =
        gateway != nullptr && !gateway->last_error_message().empty()
            ? gateway->last_error_message()
            : error.message();
    std::fprintf(stderr, "control %s failed: %s\n", what, message.c_str());
    return exit_refused;
}

/// The names of the scopes, or `nothing` when the connection holds none.
std::string join_scopes(const std::vector<std::string>& scopes)
{
    std::string joined;
    for (const std::string& name : scopes)
    {
        if (!joined.empty())
            joined += ", ";
        joined += name;
    }
    return joined.empty() ? "nothing" : joined;
}

int status(client& gateway)
{
    const auto state = gateway.status();
    if (!state)
        return report("status", state.error(), &gateway);
    const auto& hello = gateway.gateway();
    std::puts("Remote control: local only");
    std::printf("Gateway: %s, API %u\n", hello.version.c_str(), hello.api);
    std::printf("You are: %s\n", hello.principal.c_str());
    std::printf("You may: %s\n", join_scopes(hello.scopes).c_str());
    std::printf("Sessions: %llu\n",
                static_cast<unsigned long long>(state->sessions));
    if (state->agent_running)
        std::printf("Agent: running, %s\n", state->agent_version.c_str());
    else
        std::puts("Agent: not running");
    std::printf("Uptime: %llu s\n",
                static_cast<unsigned long long>(state->uptime_ms / 1000));
    return exit_ok;
}

int stop(client& gateway)
{
    const auto stopped = gateway.stop();
    if (!stopped)
        return report("stop", stopped.error(), &gateway);
    std::puts("Remote control stopped");
    return exit_ok;
}

} // namespace

const char* control_usage() noexcept
{
    return "  vss_cli control status [--instance=<name>]\n"
           "  vss_cli control stop [--instance=<name>]\n"
           "      Ask this user's control gateway, which is `vss_gate` and is\n"
           "      started by hand for now.  status says who you are to it and\n"
           "      what you may do; stop closes its sessions and ends it, and\n"
           "      closes no port.  Exit code 3 means no gateway is running.\n"
           "\n";
}

int command_control(const std::vector<std::string>& arguments)
{
    const control_arguments args = parse(arguments);
    if (!args.valid || (args.action != "status" && args.action != "stop"))
    {
        std::fputs(control_usage(), stderr);
        return exit_usage;
    }

    vss::control::client_options options;
    options.instance = args.instance;
    options.name = "vss_cli";
    options.admin = args.action == "stop";
    auto connected = client::connect(options);
    if (!connected)
    {
        if (connected.error() == vss::agent::protocol_errc::not_running)
        {
            std::puts("Remote control: off");
            return exit_not_running;
        }
        return report(args.action.c_str(), connected.error(), nullptr);
    }

    if (args.action == "stop")
        return stop(**connected);
    return status(**connected);
}

} // namespace vss_cli
