//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli debug ...` run in the test's own process against a debug server of
// the test's own: the exit codes of a command line it does not understand, a
// studio that is not there, a person who refuses, and a reply it prints.
//

#include <vss/debug/server.hpp>
#include <vss/debug/studio_provider.hpp>
#include <vss/studio/studio.hpp>

#include "debug_command.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace dbg = vss::debug;

namespace
{

constexpr int exit_ok = 0;
constexpr int exit_usage = 2;
constexpr int exit_not_running = 3;

std::string unique_instance()
{
    static const unsigned random = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    std::ostringstream out;
    out << "cli-" << std::hex << random << '-' << std::dec
        << counter.fetch_add(1);
    return out.str();
}

struct debug_studio
{
    explicit debug_studio(bool person_allows)
    {
        provider = std::make_shared<dbg::studio_provider>(app, &log);
        dbg::server_options options;
        options.instance = unique_instance();
        options.source = provider;
        options.recorder = &log;
        options.instance_directory = scratch.path() / "instances";
        options.approve =
            [person_allows](const dbg::client_request& request,
                            std::function<void(dbg::approval)> answer)
        {
            dbg::approval reply;
            reply.granted = person_allows ? request.wanted : 0;
            answer(reply);
        };
        server = dbg::server::start(std::move(options)).value();
    }

    std::vector<std::string> words(std::vector<std::string> more) const
    {
        std::vector<std::string> out{"debug"};
        out.insert(out.end(), more.begin(), more.end());
        out.push_back("--instance-dir=" +
                      (scratch.path() / "instances").string());
        return out;
    }

    vss_test_support::scratch_directory scratch;
    vss::studio app;
    vss::trace::recorder log;
    std::shared_ptr<dbg::studio_provider> provider;
    std::unique_ptr<dbg::server> server;
};

} // namespace

TEST_CASE("cli debug: no command, an unknown command and bad options are usage")
{
    REQUIRE_EQ(vss_cli::command_debug({"debug"}), exit_usage);
    REQUIRE_EQ(vss_cli::command_debug({"debug", "bogus"}), exit_usage);
    REQUIRE_EQ(vss_cli::command_debug({"debug", "status", "--nonsense"}),
               exit_usage);
    REQUIRE_EQ(vss_cli::command_debug({"debug", "trace", "--endpoint=x"}),
               exit_usage);
    REQUIRE_EQ(vss_cli::command_debug({"debug", "status", "--pid"}),
               exit_usage);
    REQUIRE_EQ(vss_cli::command_debug({"debug", "run"}), exit_usage);
}

TEST_CASE("cli debug: with no studio that has debug access on it says so")
{
    vss_test_support::scratch_directory scratch;
    const std::string where =
        "--instance-dir=" + (scratch.path() / "none").string();
    REQUIRE_EQ(vss_cli::command_debug({"debug", "list", where}),
               exit_not_running);
    REQUIRE_EQ(vss_cli::command_debug({"debug", "status", where}),
               exit_not_running);
}

TEST_CASE("cli debug: it finds the one studio and prints its status")
{
    debug_studio world(true);
    REQUIRE_EQ(vss_cli::command_debug(world.words({"list"})), exit_ok);
    REQUIRE_EQ(vss_cli::command_debug(world.words({"status"})), exit_ok);
    REQUIRE_EQ(vss_cli::command_debug(world.words({"snapshot", "--json"})),
               exit_ok);
    REQUIRE_EQ(vss_cli::command_debug(world.words({"trace", "--last=5"})),
               exit_ok);
}

TEST_CASE("cli debug: an operation that needs more than reading is asked for")
{
    debug_studio world(true);
    auto run = world.words({"run", "stop_port", "port=nothing",
                            "--pid=" + world.server->instance()});
    // The person allowed it and the port does not exist: the operation ran
    // and said so; the command did too.
    REQUIRE_EQ(vss_cli::command_debug(run), exit_ok);
}

TEST_CASE("cli debug: a person who refuses ends it")
{
    debug_studio world(false);
    REQUIRE_EQ(vss_cli::command_debug(world.words({"status"})),
               exit_not_running);
}

TEST_CASE(
    "cli debug: a studio that is not asked for by name must be the only one")
{
    debug_studio first(true);
    // A second server publishing into the same directory.
    dbg::server_options options;
    options.instance = unique_instance();
    options.source = first.provider;
    options.recorder = &first.log;
    options.instance_directory = first.scratch.path() / "instances";
    options.approve = [](const dbg::client_request& request,
                         std::function<void(dbg::approval)> answer)
    {
        dbg::approval reply;
        reply.granted = request.wanted;
        answer(reply);
    };
    auto second = dbg::server::start(std::move(options));
    REQUIRE(second.has_value());
    REQUIRE_EQ(vss_cli::command_debug(first.words({"status"})), exit_usage);
    REQUIRE_EQ(vss_cli::command_debug(first.words(
                   {"status", "--pid=" + first.server->instance()})),
               exit_ok);
}
