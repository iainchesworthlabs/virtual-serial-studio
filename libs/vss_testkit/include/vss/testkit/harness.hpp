//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_TESTKIT_HARNESS_HPP
#define VSS_TESTKIT_HARNESS_HPP

#include <vss/core/endpoint.hpp>
#include <vss/core/hub.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace vss::testkit {

/// Two serial ports joined by a null-modem cable.
struct serial_pair
{
    std::string a;
    std::string b;
    bool com0com = false;
};

/// Finds a usable connected pair: the `VSS_LOOPBACK_A` / `VSS_LOOPBACK_B`
/// environment variables first (as used on Linux CI with socat or ptys),
/// then a com0com pair on Windows.  Empty when the machine has neither.
std::optional<serial_pair> find_loopback_pair();

/// Reads an environment variable, or an empty string when it is unset.
///
/// Uses the non-deprecated spelling on every platform.
std::string environment(const char* name);

/// Polls `predicate` until it is true or the timeout expires.
bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout);

/// Waits until `endpoint` has counted at least `bytes` received.
bool wait_for_rx(const endpoint& endpoint, std::size_t bytes,
                 std::chrono::milliseconds timeout);

/// Makes a break of `length` at `endpoint`, as if its device had made it
/// beginning at `start` (the hub's clock, which is the steady clock unless the
/// hub was given another).  It is a `complete` item, in order with the bytes
/// the endpoint read before, so a test can put a break of a known length
/// between two chunks.  The existing fault injection `break` is the same item
/// with one character time and `measured` false.
void make_break(endpoint& endpoint, std::chrono::nanoseconds length,
                time_point start = clock_type::now(), bool measured = true);

/// Makes the two halves of a break at `endpoint` as a program that holds it
/// would: a `begin` at `start` and, after `length`, an `end` with the length.
/// Both are queued now, so the call does not wait for `length`.
void make_break_pair(endpoint& endpoint, std::chrono::nanoseconds length,
                     time_point start = clock_type::now());

/// Generates a payload with a repeating pattern, so truncation and
/// reordering are visible.
byte_buffer make_pattern(std::size_t size, unsigned seed = 1);

/// A deterministic binary payload including every byte value.
byte_buffer make_binary_payload(std::size_t size);

} // namespace vss::testkit

#endif // VSS_TESTKIT_HARNESS_HPP
