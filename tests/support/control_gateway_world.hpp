//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A real control gateway for a test: `vss_agent` and `vss_gate` are started as
// children under an instance name of their own, and a few functions look at
// what the gateway says about the tokens and sessions of plugins through its
// administration endpoint.  For the tests of the control relay and of the
// control session extension point of the plugin host.
//

#pragma once

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/control/client.hpp>
#include <vss/control/proto/message.hpp>
#include <vss/control/proto/token.hpp>
#include <vss/plugin/host/control_relay.hpp>

#include "child_process.hpp"
#include "control_relay_rig.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vss_test_gateway
{

namespace control = vss::control;
using vss_test_support::child_process;
using vss_test_support::scratch_directory;

/// The longest any wait lasts.
inline constexpr std::chrono::milliseconds long_limit =
    vss_test_relay::long_limit;

/// An instance name no other test, run, agent or gateway uses.
inline std::string unique_instance()
{
    static const std::string prefix = []
    {
        std::random_device device;
        std::ostringstream text;
        text << "r" << std::hex << std::setw(8) << std::setfill('0')
             << device();
        return text.str();
    }();
    static unsigned counter = 0;
    return prefix + "-" + std::to_string(counter++);
}

/// Reads lines from `child` until one contains `marker`.
inline void wait_for_line(child_process& child, const std::string& marker,
                          const char* who)
{
    std::string seen;
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto line = child.read_line(long_limit);
        if (!line)
            break;
        seen.append(*line);
        seen.push_back('\n');
        if (line->find(marker) != std::string::npos)
            return;
    }
    throw vss_test::failure(std::string(who) + " did not say \"" + marker +
                            "\"; it said: " + seen);
}

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

private:
    std::string instance_;
    child_process child_;
};

class gate_process
{
public:
    explicit gate_process(const std::string& instance)
        : audit_dir_(scratch_.path() / "audit"),
          child_(VSS_GATE_PROGRAM,
                 {"--instance=" + instance, "--agent-instance=" + instance,
                  "--audit-dir=" + audit_dir_.string()})
    {
        wait_for_line(child_, "administration on", "the gateway");
    }

    child_process& child() noexcept { return child_; }

private:
    scratch_directory scratch_;
    std::filesystem::path audit_dir_;
    child_process child_;
};

/// An agent and a gateway for one test, and what a test needs to look at the
/// gateway.
class world
{
public:
    world() : agent_(unique_instance()), gate_(agent_.instance()) {}

    const std::string& instance() const noexcept { return agent_.instance(); }
    child_process& gate_child() noexcept { return gate_.child(); }

    /// The options of a relay for the plugin that the tests use.
    vss::plugin::host::control_relay_options options() const
    {
        vss::plugin::host::control_relay_options out;
        out.plugin_id = "com.example.remote";
        out.plugin_name = "Remote control";
        out.instance = instance();
        return out;
    }

    /// A connection to the administration endpoint, as `vss_cli` has.  The
    /// gateway has room for two there, and the relay uses one at times, so
    /// a connection that is refused for room is tried again.
    std::unique_ptr<control::client> admin() const
    {
        control::client_options options;
        options.instance = instance();
        options.name = "relay-test";
        options.admin = true;
        const auto deadline = std::chrono::steady_clock::now() + long_limit;
        for (;;)
        {
            auto connected = control::client::connect(options);
            if (connected)
                return std::move(*connected);
            if (connected.error() != control::api_errc::busy ||
                std::chrono::steady_clock::now() >= deadline)
            {
                throw vss_test::failure("cannot connect to the "
                                        "administration endpoint: " +
                                        connected.error().message());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }

private:
    agent_process agent_;
    gate_process gate_;
};

/// The tokens that relays made, as the gateway lists them.
inline std::vector<control::token_entry> plugin_tokens(const world& where)
{
    const auto admin = where.admin();
    auto listed = admin->list_tokens();
    if (!listed)
        throw vss_test::failure("tokens.list failed");
    std::vector<control::token_entry> out;
    for (control::token_entry& entry : listed->tokens)
    {
        if (entry.label.rfind("plugin:", 0) == 0)
            out.push_back(std::move(entry));
    }
    return out;
}

/// The sessions of tokens, as the gateway lists them.
inline std::vector<control::session_entry> token_sessions(const world& where)
{
    const auto admin = where.admin();
    auto listed = admin->list_sessions();
    if (!listed)
        throw vss_test::failure("sessions.list failed");
    std::vector<control::session_entry> out;
    for (control::session_entry& entry : listed->sessions)
    {
        if (entry.principal.rfind("token:", 0) == 0)
            out.push_back(std::move(entry));
    }
    return out;
}

/// Waits until every token of a relay is revoked, and none has a session.
inline void wait_until_revoked(const world& where, std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    for (;;)
    {
        const auto tokens = plugin_tokens(where);
        const bool all =
            tokens.size() == count &&
            std::ranges::all_of(tokens, [](const control::token_entry& entry)
                                { return entry.revoked; });
        if (all && token_sessions(where).empty())
            return;
        if (std::chrono::steady_clock::now() >= deadline)
            throw vss_test::failure("the tokens were not all revoked");
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
}

struct plugin_frame
{
    std::string header{};
    vss::byte_buffer blob{};
};

/// A request as a plugin would send it: the gateway's own header.
inline plugin_frame frame_of(std::uint64_t id, control::request_body body)
{
    control::request asked;
    asked.id = id;
    asked.body = std::move(body);
    control::frame encoded = control::encode(asked);
    return {std::move(encoded.header), std::move(encoded.blob)};
}

} // namespace vss_test_gateway
