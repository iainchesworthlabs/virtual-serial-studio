//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Random bytes for keys on Linux: `getrandom` with no flags, which blocks only
// until the kernel's generator has been seeded and then never.  A call may
// return fewer bytes than asked for, or be interrupted by a signal, so it is
// repeated until the buffer is full.
//

#include "platform/private_storage_platform.hpp"

#include <sys/random.h>
#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vss::control::platform
{

error_code random_bytes(std::span<std::uint8_t> out)
{
    std::size_t done = 0;
    while (done < out.size())
    {
        const ssize_t got =
            ::getrandom(out.data() + done, out.size() - done, 0);
        if (got < 0)
        {
            const int failed = errno;
            if (failed == EINTR)
                continue;
            return error_code(failed, boost::system::system_category());
        }
        if (got == 0)
            return make_error_code(errc::io_error);
        done += static_cast<std::size_t>(got);
    }
    return {};
}

} // namespace vss::control::platform
