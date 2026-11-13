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

result<std::string> local_address(local_endpoint endpoint,
                                  std::string_view instance);

/// The word for `endpoint` in a pipe name or a file name.  No instance name
/// can turn one endpoint's address into another's: an instance name is added
/// after a `-`, and none of these words is another one followed by `-`.
/// The words are short because the path of a Unix socket may have only about
/// a hundred bytes, and a temporary directory on macOS uses half of them.
constexpr std::string_view endpoint_word(local_endpoint endpoint) noexcept
{
    switch (endpoint)
    {
    case local_endpoint::agent:
        return "agent";
    case local_endpoint::control:
        return "control";
    case local_endpoint::control_admin:
        return "admin";
    }
    return "agent";
}

result<std::unique_ptr<local_listener>>
listen_local(const boost::asio::any_io_executor& executor,
             local_endpoint endpoint, std::string_view instance);

result<std::unique_ptr<local_stream>>
connect_local(const boost::asio::any_io_executor& executor,
              local_endpoint endpoint, std::string_view instance);

} // namespace vss::agent::platform
