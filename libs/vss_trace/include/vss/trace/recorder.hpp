//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::trace
{

/// One call that a transport or the studio made, as the recorder keeps it.
///
/// The record has fixed-size text so that recording never allocates.  Text
/// that does not fit is cut, never wrapped.  Nothing here holds the bytes of
/// traffic and nothing may: `args` is for a path, a number or a flag, and
/// never for data a port carried.
struct entry
{
    static constexpr std::size_t call_size_ = 40;
    static constexpr std::size_t args_size_ = 120;
    static constexpr std::size_t operation_size_ = 32;
    static constexpr std::size_t category_size_ = 16;
    static constexpr std::size_t message_size_ = 120;

    /// Counts the entries of this recorder from 1.
    std::uint64_t seq = 0;
    /// Nanoseconds on the steady clock, for ordering and durations.
    std::int64_t mono_ns = 0;
    /// Milliseconds since the Unix epoch, for a person.
    std::int64_t wall_ms = 0;
    /// A small number for the thread, as the recorder numbered it.
    std::uint32_t thread = 0;
    /// The port the call was for, or 0.
    std::uint64_t endpoint = 0;
    /// The entry this one came from, when an error was made from another
    /// error; 0 otherwise.
    std::uint64_t cause = 0;
    std::uint32_t duration_us = 0;
    /// The raw code: `GetLastError`, `errno`, the library's own number.
    std::int32_t code = 0;
    bool failed = false;
    std::array<char, call_size_> call{};
    std::array<char, args_size_> args{};
    std::array<char, operation_size_> operation{};
    std::array<char, category_size_> category{};
    std::array<char, message_size_> message{};

    std::string_view call_text() const noexcept;
    std::string_view args_text() const noexcept;
    std::string_view operation_text() const noexcept;
    std::string_view category_text() const noexcept;
    std::string_view message_text() const noexcept;
};

/// What `recorder::read` returns.
struct filter
{
    /// Only entries after this number.
    std::uint64_t after_seq = 0;
    /// Only this port; 0 means every port.
    std::uint64_t endpoint = 0;
    /// Only failed calls.
    bool failures_only = false;
    /// At most this many, the newest ones; 0 means no limit.
    std::size_t limit = 0;
};

/// A bounded ring of calls.  Two rings are kept: one for every call, which a
/// busy port fills quickly, and a longer-lived one for failures, so that the
/// one failure that matters is still there when a thousand good calls have
/// come after it.
///
/// Safe from any thread.  The cost of one entry is a clock read and a copy of
/// about 350 bytes under a short lock.
class recorder
{
public:
    static constexpr std::size_t default_capacity_ = 4096;
    static constexpr std::size_t default_failure_capacity_ = 512;

    explicit recorder(std::size_t capacity = default_capacity_,
                      std::size_t failure_capacity = default_failure_capacity_);
    ~recorder();
    recorder(const recorder&) = delete;
    recorder& operator=(const recorder&) = delete;

    /// The recorder the transports write to.  It lives to the end of the
    /// process.
    static recorder& global() noexcept;

    /// Adds an entry.  `seq`, `mono_ns`, `wall_ms` and `thread` are filled
    /// in here; `operation` is the one of the calling thread's innermost
    /// `operation_scope` when the entry has none.  Returns the number given.
    std::uint64_t add(entry value) noexcept;

    /// The entries that match, oldest first.
    std::vector<entry> read(const filter& what = {}) const;

    /// The newest failed call of a port (or of any port when `endpoint` is
    /// 0), if the ring still has one.
    std::optional<entry> last_failure(std::uint64_t endpoint = 0) const;

    /// The number of the newest entry: 0 when none was added.
    std::uint64_t last_seq() const noexcept;

    /// Entries that were pushed out of the ring since it was made.
    std::uint64_t overwritten() const noexcept;

    /// Empties both rings.  Numbers go on from where they were.
    void clear() noexcept;

private:
    class ring;
    std::unique_ptr<ring> all_;
    std::unique_ptr<ring> failures_;
    mutable std::mutex lock_;
    std::atomic<std::uint64_t> seq_{0};
};

/// Fills a fixed-size text field, cutting what does not fit.
template <std::size_t N>
void set_text(std::array<char, N>& field, std::string_view text) noexcept
{
    const std::size_t count = text.size() < N - 1 ? text.size() : N - 1;
    for (std::size_t index = 0; index < count; ++index)
        field[index] = text[index];
    for (std::size_t index = count; index < N; ++index)
        field[index] = '\0';
}

/// Names the studio operation that the calls of this thread belong to, for
/// as long as the scope lives: "restart_port", "add_monitor".  Entries that
/// have no port of their own get `endpoint` when the scope names one.  Scopes
/// nest, and the innermost name and port are the ones entries get.
class operation_scope
{
public:
    explicit operation_scope(std::string_view name,
                             std::uint64_t endpoint = 0) noexcept;
    ~operation_scope();
    operation_scope(const operation_scope&) = delete;
    operation_scope& operator=(const operation_scope&) = delete;

    /// The innermost name on this thread, or empty.
    static std::string_view current() noexcept;
    /// The number of the last failed call made on this thread inside the
    /// innermost scope, or 0.  An error that is made from another names it
    /// as its cause.
    static std::uint64_t last_failure() noexcept;

private:
    std::array<char, entry::operation_size_> previous_{};
    std::uint64_t previous_endpoint_ = 0;
    std::uint64_t previous_failure_ = 0;
};

/// One call being timed.  Make it before the call, finish it with `ok`,
/// `fail` or `finish` after.  A scope that is destroyed unfinished records
/// the call as successful, so an early return does not lose the line.
class call_scope
{
public:
    call_scope(std::string_view call, std::uint64_t endpoint,
               std::string_view args,
               recorder& target = recorder::global()) noexcept;
    ~call_scope();
    call_scope(const call_scope&) = delete;
    call_scope& operator=(const call_scope&) = delete;

    /// Records success.
    void ok() noexcept;
    /// Records the failure of this call with an error code, and returns the
    /// entry's number, which a later error can name as its cause.
    std::uint64_t fail(const boost::system::error_code& code,
                       std::uint64_t cause = 0) noexcept;
    /// Records a failure with a raw code the library did not turn into an
    /// error code.
    std::uint64_t fail(std::int32_t code, std::string_view category,
                       std::string_view message = {},
                       std::uint64_t cause = 0) noexcept;
    /// Records success when the code is clear and failure when it is not.
    /// Returns the entry's number for a failure and 0 for a success.
    std::uint64_t finish(const boost::system::error_code& code) noexcept;

private:
    recorder* target_;
    entry value_;
    std::chrono::steady_clock::time_point started_;
    bool done_ = false;
};

/// Records a call that already failed, when there was no time to measure.
std::uint64_t record_failure(std::string_view call, std::uint64_t endpoint,
                             std::string_view args,
                             const boost::system::error_code& code,
                             std::uint64_t cause = 0) noexcept;

/// Runs `body` as the studio operation `operation` on `endpoint` (0 when it
/// has none yet).  The calls the body makes are recorded under that name,
/// and when the body returns an error the operation is recorded as failed,
/// with the last failed call inside it as its cause.  Returns what `body`
/// returned.
template <class Body>
auto traced(std::string_view operation, std::uint64_t endpoint, Body&& body)
{
    operation_scope scope(operation, endpoint);
    auto out = std::forward<Body>(body)();
    if (!out)
        record_failure(operation, endpoint, {}, out.error(),
                       operation_scope::last_failure());
    return out;
}

/// Records a call that succeeded.
void record_success(std::string_view call, std::uint64_t endpoint,
                    std::string_view args) noexcept;

} // namespace vss::trace
