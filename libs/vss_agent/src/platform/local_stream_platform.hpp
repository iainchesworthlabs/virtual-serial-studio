//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The local transport of each platform: a named pipe on Windows
// (src/platform/windows) and a Unix domain socket on Linux and macOS
// (src/platform/posix, with the one call that differs in src/platform/linux
// and src/platform/macos).  CMake picks the files.
//

#pragma once

#include <vss/agent/local_stream.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace vss::agent::platform
{

// Each takes an instance name that `valid_instance_name` accepted.

result<std::string> local_address(std::string_view instance);

result<std::unique_ptr<local_listener>>
listen_local(const boost::asio::any_io_executor& executor,
             std::string_view instance);

result<std::unique_ptr<local_stream>>
connect_local(const boost::asio::any_io_executor& executor,
              std::string_view instance);

} // namespace vss::agent::platform
