//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Random bytes for keys on macOS: `getentropy`, which gives at most 256 bytes
// at a time, so a longer buffer is filled in pieces.
//

#include "platform/private_storage_platform.hpp"

#include <sys/random.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vss::control::platform
{

error_code random_bytes(std::span<std::uint8_t> out)
{
    constexpr std::size_t longest_request = 256;
    std::size_t done = 0;
    while (done < out.size())
    {
        const std::size_t left = out.size() - done;
        const std::size_t count =
            left < longest_request ? left : longest_request;
        if (::getentropy(out.data() + done, count) != 0)
            return error_code(errno, boost::system::system_category());
        done += count;
    }
    return {};
}

} // namespace vss::control::platform
