//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/license/words.hpp>

namespace vss::license
{

std::string_view state_word(state value) noexcept
{
    switch (value)
    {
    case state::none:
        return "none";
    case state::valid:
        return "valid";
    case state::expiring:
        return "expiring";
    case state::grace:
        return "grace";
    case state::expired:
        return "expired";
    case state::not_yet_valid:
        return "not yet valid";
    case state::clock_suspect:
        return "clock suspect";
    case state::invalid:
        return "invalid";
    }
    return "unknown";
}

std::string_view state_name(state value) noexcept
{
    switch (value)
    {
    case state::none:
        return "none";
    case state::valid:
        return "valid";
    case state::expiring:
        return "expiring";
    case state::grace:
        return "grace";
    case state::expired:
        return "expired";
    case state::not_yet_valid:
        return "not_yet_valid";
    case state::clock_suspect:
        return "clock_suspect";
    case state::invalid:
        return "invalid";
    }
    return "invalid";
}

std::string_view edition_name(const status& current) noexcept
{
    if (!current.active() || !current.value)
        return "community";
    return current.value->edition == edition::enterprise ? "enterprise" : "pro";
}

std::string_view reason_sentence(reason value) noexcept
{
    switch (value)
    {
    case reason::ok:
        return "ok";
    case reason::no_licence:
        return "there is no licence";
    case reason::expired:
        return "the licence has ended";
    case reason::not_yet_valid:
        return "the licence has not started yet";
    case reason::clock_suspect:
        return "the date is before a time that has certainly passed";
    case reason::malformed:
        return "the file is damaged or is not a licence";
    case reason::newer_version:
        return "the licence needs a newer version";
    case reason::bad_signature:
        return "the signature does not verify";
    case reason::revoked:
        return "the licence or its key was withdrawn";
    case reason::wrong_computer:
        return "the licence does not name this computer";
    case reason::not_in_licence:
        return "the licence does not include the feature";
    }
    return "unknown";
}

} // namespace vss::license
