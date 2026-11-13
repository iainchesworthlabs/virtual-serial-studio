//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_gate - serves the control API to this user's programs over a local pipe
// or socket, and is itself a client of this user's agent.  The ports stay the
// agent's, so stopping the gateway closes none of them.  docs/remote-control.md
// has the design; this is its first slice.
//

#include "platform/stop_signals_platform.hpp"

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/control/gateway.hpp>
#include <vss/control/proto/policy.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <thread>

namespace
{

/// Exit codes, so a script and the tests can tell the cases apart.
constexpr int exit_ok = 0;
constexpr int exit_failed = 1;
constexpr int exit_usage = 2;
constexpr int exit_already_running = 3;

void print_usage()
{
    std::printf(
        "Virtual Serial Studio control gateway\n"
        "\n"
        "  vss_gate [--instance=<name>] [--agent-instance=<name>]\n"
        "           [--local-role=monitor|control] [--audit-dir=<path>]\n"
        "           [--token-file=<path>]\n"
        "      Serve the control API to this user's programs over a pipe or\n"
        "      socket that only this user can open.  Nothing listens on the\n"
        "      network.  It needs a running `vss_agent`, which owns the\n"
        "      ports.  Programs may monitor by default; --local-role=control\n"
        "      lets them change ports as well.\n"
        "      It runs until Ctrl-C, SIGTERM or `vss_cli control stop`.\n"
        "      Stopping it closes no port.  One gateway runs per user;\n"
        "      --instance names a separate one and --agent-instance the agent\n"
        "      it serves (letters, digits, _ and -), which the tests use.\n"
        "      --audit-dir keeps the audit log in another directory.\n"
        "      --token-file names the file that holds the records of the\n"
        "      tokens (their hashes, never the tokens); by default it is\n"
        "      tokens.json beside the audit log.\n"
        "\n"
        "  vss_gate --version\n"
        "\n"
        "Virtual Serial Studio is free software under the GNU General Public\n"
        "License, version 3 or later.  This program comes with ABSOLUTELY NO\n"
        "WARRANTY; see LICENSE for details.\n");
}

/// Calls `request_stop` on the gateway for Ctrl-C, SIGTERM and the system's
/// own stop signals.  The handler runs on a thread of this object, never in
/// the signal's own context.
class stop_signals
{
public:
    explicit stop_signals(vss::control::gateway& gateway)
        : signals_(io_, SIGINT, SIGTERM)
    {
        vss_agent::platform::add_stop_signals(signals_);
        signals_.async_wait(
            [&gateway](boost::system::error_code cancelled, int)
            {
                if (!cancelled)
                    gateway.request_stop();
            });
        thread_ = std::thread([this] { io_.run(); });
    }

    ~stop_signals()
    {
        io_.stop();
        if (thread_.joinable())
            thread_.join();
    }

    stop_signals(const stop_signals&) = delete;
    stop_signals& operator=(const stop_signals&) = delete;

private:
    boost::asio::io_context io_;
    boost::asio::signal_set signals_;
    std::thread thread_;
};

} // namespace

int main(int argc, char** argv)
try
{
    vss::control::gateway_options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        constexpr std::string_view instance_option = "--instance=";
        constexpr std::string_view agent_option = "--agent-instance=";
        constexpr std::string_view role_option = "--local-role=";
        constexpr std::string_view audit_option = "--audit-dir=";
        constexpr std::string_view token_option = "--token-file=";
        if (argument == "--help" || argument == "-h")
        {
            print_usage();
            return exit_ok;
        }
        if (argument == "--version")
        {
            std::printf("vss_gate %s\n",
                        vss::control::gateway_version().c_str());
            return exit_ok;
        }
        if (argument.starts_with(instance_option))
        {
            options.instance = argument.substr(instance_option.size());
            continue;
        }
        if (argument.starts_with(agent_option))
        {
            options.agent_instance = argument.substr(agent_option.size());
            continue;
        }
        if (argument.starts_with(role_option))
        {
            const std::string_view role = argument.substr(role_option.size());
            if (role == "monitor")
            {
                options.local_scopes = vss::control::role_monitor();
                continue;
            }
            if (role == "control")
            {
                options.local_scopes = vss::control::role_control();
                continue;
            }
            std::fprintf(stderr,
                         "vss_gate: a local role is monitor or control, "
                         "not %s\n",
                         std::string(role).c_str());
            return exit_usage;
        }
        if (argument.starts_with(audit_option))
        {
            options.audit_directory = argument.substr(audit_option.size());
            continue;
        }
        if (argument.starts_with(token_option))
        {
            options.token_file = argument.substr(token_option.size());
            continue;
        }
        std::fprintf(stderr, "vss_gate: unknown option %s\n", argv[index]);
        print_usage();
        return exit_usage;
    }
    if (!vss::agent::valid_instance_name(options.instance) ||
        !vss::agent::valid_instance_name(options.agent_instance))
    {
        std::fputs("vss_gate: an instance name is up to 32 letters, digits, "
                   "_ and -\n",
                   stderr);
        return exit_usage;
    }

    vss::control::gateway gateway(options);

    const auto started = gateway.start();
    if (!started)
    {
        if (started.error() == vss::agent::protocol_errc::already_running)
        {
            std::fputs("vss_gate: a gateway is already running for this "
                       "user\n",
                       stderr);
            return exit_already_running;
        }
        std::fprintf(stderr, "vss_gate: cannot start: %s\n",
                     started.error().message().c_str());
        return exit_failed;
    }

    {
        const stop_signals signals(gateway);
        std::printf("vss_gate %s listening on %s\n",
                    vss::control::gateway_version().c_str(),
                    gateway.address().c_str());
        std::printf("vss_gate administration on %s\n",
                    gateway.admin_address().c_str());
        std::fflush(stdout);
        gateway.wait_for_stop_request();
    }
    gateway.stop();
    std::puts("vss_gate stopped");
    return exit_ok;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "vss_gate: %s\n", error.what());
    return 1;
}
catch (...)
{
    std::fputs("vss_gate: unknown exception\n", stderr);
    return 1;
}
