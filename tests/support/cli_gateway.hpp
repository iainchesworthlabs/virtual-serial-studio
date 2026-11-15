//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A gateway of the test's own, with no agent behind it, and `vss_cli control
// ...` run in the test's process against it.  The commands that make and list
// tokens need nothing else.
//

#pragma once

#include <vss/agent/local_stream.hpp>
#include <vss/control/client.hpp>
#include <vss/control/gateway.hpp>

#include "control_command.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vss_test_support
{

class cli_gateway
{
public:
    cli_gateway() : instance_(unique_name())
    {
        vss::control::gateway_options options;
        options.instance = instance_;
        options.agent_instance = instance_;
        options.audit_directory = scratch_.path() / "audit";
        gate_ = std::make_unique<vss::control::gateway>(std::move(options));
        const auto started = gate_->start();
        REQUIRE(started);
    }

    ~cli_gateway()
    {
        gate_.reset();
        // Where the transport leaves lock files beside the sockets, they go
        // with the name.
        for (const vss::agent::local_endpoint endpoint :
             {vss::agent::local_endpoint::agent,
              vss::agent::local_endpoint::control,
              vss::agent::local_endpoint::control_admin})
        {
            const auto address =
                vss::agent::local_address(endpoint, instance_);
            if (!address)
                continue;
            std::filesystem::path lock(*address);
            if (lock.extension() != ".sock")
                continue;
            lock.replace_extension(".lock");
            std::error_code ignored;
            std::filesystem::remove(lock, ignored);
        }
    }

    cli_gateway(const cli_gateway&) = delete;
    cli_gateway& operator=(const cli_gateway&) = delete;

    /// A directory that is removed with the gateway, for the files a test
    /// asks `vss_cli` to make.  It is an ordinary directory, with the access
    /// of any directory the system makes.
    const std::filesystem::path& directory() const noexcept
    {
        return scratch_.path();
    }

    const std::string& instance() const noexcept { return instance_; }

    /// `vss_cli control <words...> --instance=<this gateway>`; the exit code.
    int run(std::vector<std::string> words) const
    {
        std::vector<std::string> arguments{"control"};
        for (std::string& word : words)
            arguments.push_back(std::move(word));
        arguments.push_back("--instance=" + instance_);
        return vss_cli::command_control(arguments);
    }

    /// The number of tokens the gateway keeps, revoked ones included.
    std::size_t token_count() const
    {
        vss::control::client_options options;
        options.instance = instance_;
        options.name = "cli gateway test";
        options.admin = true;
        auto connected = vss::control::client::connect(options);
        REQUIRE(connected);
        const auto listed = (*connected)->list_tokens();
        REQUIRE(listed);
        return listed->tokens.size();
    }

    /// The tokens that are not revoked.
    std::size_t live_token_count() const
    {
        vss::control::client_options options;
        options.instance = instance_;
        options.name = "cli gateway test";
        options.admin = true;
        auto connected = vss::control::client::connect(options);
        REQUIRE(connected);
        const auto listed = (*connected)->list_tokens();
        REQUIRE(listed);
        std::size_t live = 0;
        for (const auto& entry : listed->tokens)
        {
            if (!entry.revoked)
                ++live;
        }
        return live;
    }

private:
    static std::string unique_name()
    {
        static const unsigned random = std::random_device{}();
        static std::atomic<unsigned> counter{0};
        std::ostringstream out;
        out << "k" << std::hex << random << '-' << std::dec
            << counter.fetch_add(1);
        return out.str();
    }

    std::string instance_;
    scratch_directory scratch_;
    std::unique_ptr<vss::control::gateway> gate_;
};

} // namespace vss_test_support
