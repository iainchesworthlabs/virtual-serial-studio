//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/proto/secret.hpp>

namespace vss::control
{

void secure_wipe(char* data, std::size_t size) noexcept
{
    // Written through a volatile pointer: the stores are observable, so the
    // compiler keeps them even when the memory is about to be freed.
    volatile char* const out = data;
    for (std::size_t index = 0; index < size; ++index)
        out[index] = '\0';
}

bool constant_time_equal(std::string_view first,
                         std::string_view second) noexcept
{
    if (first.size() != second.size())
        return false;
    // No early exit: the differences are gathered and looked at once, at
    // the end.  The accumulator is volatile so that the loop is not turned
    // into a comparison that stops at the first difference.
    volatile unsigned char difference = 0;
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        difference = static_cast<unsigned char>(
            difference | (static_cast<unsigned char>(first[index]) ^
                          static_cast<unsigned char>(second[index])));
    }
    return difference == 0;
}

} // namespace vss::control
