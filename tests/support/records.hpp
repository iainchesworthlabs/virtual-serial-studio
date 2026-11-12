//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Numbered four-byte records for the stress tests: who sent a message and its
// number, so a receiver can tell the order and the sender of each, and spot a
// message that was lost, repeated or torn.

#pragma once

#include "support/test_support.hpp"

#include <map>
#include <string>
#include <vector>

namespace vss_test_support::records
{

/// One message is four bytes: who sent it, then its number in three digits of
/// base 64.
inline constexpr std::size_t record_size = 4;
inline constexpr int last_number = 64 * 64 * 64 - 1;

inline std::string record(char sender, int number)
{
    std::string out(record_size, sender);
    out[1] = static_cast<char>('0' + ((number >> 12) & 63));
    out[2] = static_cast<char>('0' + ((number >> 6) & 63));
    out[3] = static_cast<char>('0' + (number & 63));
    return out;
}

/// The numbers `heard` holds for each sender, in the order they arrived.
/// Fails the test if a message was torn.
inline std::map<char, std::vector<int>> by_sender(const std::string& heard)
{
    REQUIRE_EQ(heard.size() % record_size, std::size_t{0});
    std::map<char, std::vector<int>> out;
    for (std::size_t at = 0; at < heard.size(); at += record_size)
    {
        const int number = ((heard[at + 1] - '0') << 12) |
                           ((heard[at + 2] - '0') << 6) | (heard[at + 3] - '0');
        REQUIRE(number >= 0 && number <= last_number);
        out[heard[at]].push_back(number);
    }
    return out;
}

/// In order, nothing twice.
inline bool strictly_rising(const std::vector<int>& numbers)
{
    for (std::size_t at = 1; at < numbers.size(); ++at)
    {
        if (numbers[at] <= numbers[at - 1])
            return false;
    }
    return true;
}

/// Every number from zero to `count - 1`, in order, and then the last one.
inline bool complete(const std::vector<int>& numbers, int count)
{
    if (numbers.size() != static_cast<std::size_t>(count) + 1)
        return false;
    for (int at = 0; at < count; ++at)
    {
        if (numbers[static_cast<std::size_t>(at)] != at)
            return false;
    }
    return numbers.back() == last_number;
}

/// True once `port` has been written the last message of `sender`.
inline bool has_last(const mock_endpoint& port, char sender)
{
    const auto heard = by_sender(port.written());
    const auto found = heard.find(sender);
    return found != heard.end() && found->second.back() == last_number;
}

} // namespace vss_test_support::records
