//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/testkit/harness.hpp>

#include "platform/environment_platform.hpp"

#include <vss/ports/com0com.hpp>
#include <vss/ports/serial_enumeration.hpp>

#include <thread>

namespace vss::testkit
{

std::string environment(const char* name)
{
    return detail::platform_environment(name);
}

std::optional<serial_pair> find_loopback_pair()
{
    serial_pair pair;
    pair.a = environment("VSS_LOOPBACK_A");
    pair.b = environment("VSS_LOOPBACK_B");
    if (!pair.a.empty() && !pair.b.empty())
        return pair;

    const auto devices = enumerate_serial_devices();
    const auto pairs = group_com0com_pairs(devices);
    if (!pairs.empty())
    {
        pair.a = pairs.front().a;
        pair.b = pairs.front().b;
        pair.com0com = true;
        return pair;
    }

    return std::nullopt;
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

bool wait_for_rx(const endpoint& endpoint, std::size_t bytes,
                 std::chrono::milliseconds timeout)
{
    return wait_until([&endpoint, bytes]
                      { return endpoint.stats().bytes_rx >= bytes; }, timeout);
}

void make_break(endpoint& endpoint, std::chrono::nanoseconds length,
                time_point start, bool measured)
{
    line_break item;
    item.phase = break_phase::complete;
    item.start = start;
    item.duration = length;
    item.measured = measured;
    endpoint.receive_break(item);
}

void make_break_pair(endpoint& endpoint, std::chrono::nanoseconds length,
                     time_point start)
{
    line_break item;
    item.measured = true;
    item.phase = break_phase::begin;
    item.start = start;
    endpoint.receive_break(item);
    item.phase = break_phase::end;
    item.duration = length;
    endpoint.receive_break(item);
}

byte_buffer make_pattern(std::size_t size, unsigned seed)
{
    byte_buffer data;
    data.reserve(size);
    unsigned state = seed == 0 ? 1u : seed;
    for (std::size_t index = 0; index < size; ++index)
    {
        state = state * 1664525u + 1013904223u;
        data.push_back(static_cast<byte>((state >> 16) & 0xff));
    }
    return data;
}

byte_buffer make_binary_payload(std::size_t size)
{
    byte_buffer data;
    data.reserve(size);
    for (std::size_t index = 0; index < size; ++index)
        data.push_back(static_cast<byte>(index & 0xff));
    return data;
}

} // namespace vss::testkit
