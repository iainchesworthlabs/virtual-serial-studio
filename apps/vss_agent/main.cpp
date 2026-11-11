//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_agent - owns one studio for the user who starts it and serves it to
// that user's programs over a local pipe or socket, so the ports live as long
// as the agent does and not as long as the window or the command that made
// them.  docs/agent.md has the design; this is its first slice.
//

#include "platform/stop_signals_platform.hpp"

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/agent/server.hpp>
#include <vss/studio/studio.hpp>

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
        "Virtual Serial Studio agent\n"
        "\n"
        "  vss_agent [--instance=<name>]\n"
        "      Keep this user's virtual ports and links running, and serve\n"
        "      them to `vss_cli agent ...` over a pipe or socket that only\n"
        "      this user can open.  Nothing listens on the network.\n"
        "      It runs until Ctrl-C, SIGTERM or `vss_cli agent stop`, and\n"
        "      closes every port when it stops.  One agent runs per user;\n"
        "      --instance names a separate one (letters, digits, _ and -),\n"
        "      which the tests use.\n"
        "\n"
        "  vss_agent --version\n"
        "\n"
        "Virtual Serial Studio is free software under the GNU General Public\n"
        "License, version 3 or later.  This program comes with ABSOLUTELY NO\n"
        "WARRANTY; see LICENSE for details.\n");
}

/// Calls `request_stop` on the server for Ctrl-C, SIGTERM and the system's
/// own stop signals.  The handler runs on a thread of this object, never in
/// the signal's own context.
class stop_signals
{
public:
    explicit stop_signals(vss::agent::server& server)
        : signals_(io_, SIGINT, SIGTERM)
    {
        vss_agent::platform::add_stop_signals(signals_);
        signals_.async_wait(
            [&server](boost::system::error_code cancelled, int)
            {
                if (!cancelled)
                    server.request_stop();
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
    std::string instance;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        constexpr std::string_view instance_option = "--instance=";
        if (argument == "--help" || argument == "-h")
        {
            print_usage();
            return exit_ok;
        }
        if (argument == "--version")
        {
            std::printf("vss_agent %s\n", vss::agent::agent_version().c_str());
            return exit_ok;
        }
        if (argument.starts_with(instance_option))
        {
            instance = argument.substr(instance_option.size());
            continue;
        }
        std::fprintf(stderr, "vss_agent: unknown option %s\n", argv[index]);
        print_usage();
        return exit_usage;
    }
    if (!vss::agent::valid_instance_name(instance))
    {
        std::fputs("vss_agent: an instance name is up to 32 letters, digits, "
                   "_ and -\n",
                   stderr);
        return exit_usage;
    }

    // The studio is made first and destroyed last: the server stops before
    // the hub does, and the ports close when the studio goes.
    vss::studio app;
    vss::agent::server_options options;
    options.instance = instance;
    vss::agent::server server(app, options);

    const auto started = server.start();
    if (!started)
    {
        if (started.error() == vss::agent::protocol_errc::already_running)
        {
            std::fputs("vss_agent: an agent is already running for this "
                       "user\n",
                       stderr);
            return exit_already_running;
        }
        std::fprintf(stderr, "vss_agent: cannot start: %s\n",
                     started.error().message().c_str());
        return exit_failed;
    }

    {
        const stop_signals signals(server);
        std::printf("vss_agent %s listening on %s\n",
                    vss::agent::agent_version().c_str(),
                    server.address().c_str());
        std::fflush(stdout);
        server.wait_for_stop_request();
    }
    server.stop();
    std::puts("vss_agent stopped");
    return exit_ok;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "vss_agent: %s\n", error.what());
    return 1;
}
catch (...)
{
    std::fputs("vss_agent: unknown exception\n", stderr);
    return 1;
}
