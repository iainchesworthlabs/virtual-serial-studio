//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>

namespace vss::control
{

struct failure_limits
{
    /// Failures of one key that cost nothing.  The next one starts a delay.
    unsigned free_failures = 5;
    /// The delay after failure number `free_failures`; it doubles with each
    /// failure after that, up to `longest_delay`.
    std::chrono::milliseconds first_delay{1000};
    std::chrono::milliseconds longest_delay{std::chrono::minutes(5)};
    /// A key that has not failed for this long is forgotten.
    std::chrono::milliseconds quiet_reset{std::chrono::minutes(15)};
    /// Failures over all keys in any one second; at this many, every attempt
    /// is refused until the second has passed.
    unsigned global_per_second = 20;
    /// Keys remembered at once.  The one that failed longest ago is dropped
    /// for a new one, so a caller cannot make the table grow without end.
    std::size_t max_keys = 4096;
};

/// Slows down guessing (docs/remote-control.md, section 4, "Rate limiting").
///
/// A key is whatever failures are counted against: the source of a
/// connection, or the id of a token.  The caller asks `allowed` before it
/// checks a credential and reports the outcome with `failed` or `succeeded`.
///
///     failures of a key       what happens
///     1 .. free_failures - 1  nothing
///     free_failures           refused for first_delay
///     each one after          refused for twice as long, up to longest_delay
///
/// A failure while a key is being refused cannot happen: the caller does not
/// check a credential for a key that is not allowed.  Time is passed in, so
/// the rules can be tested without waiting.  Not safe for use from more than
/// one thread at a time.
class failure_limiter
{
public:
    using clock = std::chrono::steady_clock;

    explicit failure_limiter(failure_limits limits = {});
    ~failure_limiter();
    failure_limiter(const failure_limiter&) = delete;
    failure_limiter& operator=(const failure_limiter&) = delete;

    /// True when a credential for `key` may be checked at `now`: the key is
    /// not being refused, and fewer than `global_per_second` failures were
    /// reported in the second before `now`.
    bool allowed(std::string_view key, clock::time_point now);

    /// A credential for `key` was checked at `now` and was wrong.
    void failed(std::string_view key, clock::time_point now);

    /// A credential for `key` was right: its failures are forgotten.
    void succeeded(std::string_view key);

    /// When `key` may be tried again; `now` or earlier when it may already.
    clock::time_point retry_at(std::string_view key,
                               clock::time_point now) const;

    /// Keys remembered now.
    std::size_t tracked() const noexcept;

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace vss::control
