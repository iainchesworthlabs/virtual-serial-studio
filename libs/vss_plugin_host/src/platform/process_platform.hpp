//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What starting a plugin's process asks of the operating system.  One file
// for each system implements it, and CMake builds the one that fits.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/host/process.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vss::plugin::host
{

/// A started process, as one system holds it.  Its destructor does what
/// `~plugin_process` promises.
class plugin_process::impl
{
public:
    impl() = default;
    virtual ~impl() = default;
    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    virtual sandbox_report sandbox() const = 0;
    virtual std::uint64_t id() const noexcept = 0;
    virtual native_handle channel() const noexcept = 0;
    virtual native_handle output() const noexcept = 0;
    virtual native_handle take_channel() noexcept = 0;
    virtual native_handle take_output() noexcept = 0;
    virtual bool request_stop() noexcept = 0;
    virtual void kill() noexcept = 0;
    virtual std::optional<exit_status> poll() noexcept = 0;
    virtual std::optional<exit_status>
    wait(std::chrono::milliseconds limit) noexcept = 0;
};

namespace platform
{

/// Starts the process of a request that `detail::valid_request` accepted.
result<std::unique_ptr<plugin_process::impl>>
start_process(const process_request& request);

/// The part of a plugin's environment that names places of this system:
/// `PATH`, and on Windows `SystemRoot`.  Asked of the system, never read
/// from the host's environment.
std::vector<std::pair<std::string, std::string>> system_environment();

/// What this system can apply to a plugin.
sandbox_report available_sandbox();

} // namespace platform

} // namespace vss::plugin::host
