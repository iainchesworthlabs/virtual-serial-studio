//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/agent/local_stream.hpp>

#include "platform/local_stream_platform.hpp"

#include <algorithm>

namespace vss::agent
{

bool valid_instance_name(std::string_view name) noexcept
{
    constexpr std::size_t longest = 32;
    if (name.size() > longest)
        return false;
    return std::ranges::all_of(name,
                               [](char letter)
                               {
                                   return (letter >= 'A' && letter <= 'Z') ||
                                          (letter >= 'a' && letter <= 'z') ||
                                          (letter >= '0' && letter <= '9') ||
                                          letter == '_' || letter == '-';
                               });
}

result<std::string> local_address(std::string_view instance)
{
    return local_address(local_endpoint::agent, instance);
}

result<std::string> local_address(local_endpoint endpoint,
                                  std::string_view instance)
{
    if (!valid_instance_name(instance))
        return make_error_code(errc::invalid_argument);
    return platform::local_address(endpoint, instance);
}

result<std::unique_ptr<local_listener>>
listen_local(const boost::asio::any_io_executor& executor,
             std::string_view instance)
{
    return listen_local(executor, local_endpoint::agent, instance);
}

result<std::unique_ptr<local_listener>>
listen_local(const boost::asio::any_io_executor& executor,
             local_endpoint endpoint, std::string_view instance)
{
    if (!valid_instance_name(instance))
        return make_error_code(errc::invalid_argument);
    return platform::listen_local(executor, endpoint, instance);
}

result<std::unique_ptr<local_stream>>
connect_local(const boost::asio::any_io_executor& executor,
              std::string_view instance)
{
    return connect_local(executor, local_endpoint::agent, instance);
}

result<std::unique_ptr<local_stream>>
connect_local(const boost::asio::any_io_executor& executor,
              local_endpoint endpoint, std::string_view instance)
{
    if (!valid_instance_name(instance))
        return make_error_code(errc::invalid_argument);
    return platform::connect_local(executor, endpoint, instance);
}

} // namespace vss::agent
