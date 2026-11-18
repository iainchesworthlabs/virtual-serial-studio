//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/trace/recorder.hpp>

#include <algorithm>
#include <limits>

namespace vss::trace
{
namespace
{

template <std::size_t N>
std::string_view text_of(const std::array<char, N>& field) noexcept
{
    std::size_t size = 0;
    while (size < N && field[size] != '\0')
        ++size;
    return std::string_view(field.data(), size);
}

thread_local std::array<char, entry::operation_size> current_operation{};
thread_local std::uint64_t current_endpoint = 0;
thread_local std::uint64_t current_failure = 0;

std::uint32_t thread_number() noexcept
{
    static std::atomic<std::uint32_t> next{1};
    thread_local const std::uint32_t mine = next.fetch_add(1);
    return mine;
}

std::int64_t wall_now_ms() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

std::int64_t mono_now_ns() noexcept
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::string_view entry::call_text() const noexcept
{
    return text_of(call);
}
std::string_view entry::args_text() const noexcept
{
    return text_of(args);
}
std::string_view entry::operation_text() const noexcept
{
    return text_of(operation);
}
std::string_view entry::category_text() const noexcept
{
    return text_of(category);
}
std::string_view entry::message_text() const noexcept
{
    return text_of(message);
}

/// A fixed number of slots that are written in turn.
class recorder::ring
{
public:
    explicit ring(std::size_t capacity) : slots_(capacity == 0 ? 1 : capacity)
    {
    }

    void push(const entry& value) noexcept
    {
        if (count_ >= slots_.size())
            ++overwritten_;
        slots_[count_ % slots_.size()] = value;
        ++count_;
    }

    template <class Predicate>
    void collect(std::vector<entry>& out, Predicate&& wanted) const
    {
        const std::size_t held =
            count_ < slots_.size() ? count_ : slots_.size();
        const std::size_t first = count_ - held;
        for (std::size_t index = first; index < count_; ++index)
        {
            const entry& value = slots_[index % slots_.size()];
            if (wanted(value))
                out.push_back(value);
        }
    }

    std::uint64_t overwritten() const noexcept { return overwritten_; }

    void clear() noexcept { count_ = 0; }

private:
    std::vector<entry> slots_;
    std::size_t count_ = 0;
    std::uint64_t overwritten_ = 0;
};

recorder::recorder(std::size_t capacity, std::size_t failure_capacity)
    : all_(std::make_unique<ring>(capacity)),
      failures_(std::make_unique<ring>(failure_capacity))
{
}

recorder::~recorder() = default;

recorder& recorder::global() noexcept
{
    // Never destroyed: a thread that records while the process exits must
    // find a recorder.
    static recorder* const instance = new recorder();
    return *instance;
}

std::uint64_t recorder::add(entry value) noexcept
{
    try
    {
        value.mono_ns = mono_now_ns();
        value.wall_ms = wall_now_ms();
        value.thread = thread_number();
        if (value.operation[0] == '\0')
            value.operation = current_operation;
        if (value.endpoint == 0)
            value.endpoint = current_endpoint;

        const std::lock_guard<std::mutex> hold(lock_);
        value.seq = seq_.fetch_add(1) + 1;
        all_->push(value);
        if (value.failed)
        {
            failures_->push(value);
            current_failure = value.seq;
        }
        return value.seq;
    }
    catch (...)
    {
        return 0;
    }
}

std::vector<entry> recorder::read(const filter& what) const
{
    std::vector<entry> out;
    {
        const std::lock_guard<std::mutex> hold(lock_);
        const auto wanted = [&](const entry& value)
        {
            if (value.seq <= what.after_seq)
                return false;
            if (what.endpoint != 0 && value.endpoint != what.endpoint)
                return false;
            return true;
        };
        if (what.failures_only)
        {
            failures_->collect(out, wanted);
        }
        else
        {
            // The failure ring may hold a failure that the busy ring has
            // pushed out; merge by number and drop the repeats.
            all_->collect(out, wanted);
            const std::uint64_t oldest =
                out.empty() ? ~std::uint64_t{0} : out.front().seq;
            failures_->collect(out, [&](const entry& value)
                               { return wanted(value) && value.seq < oldest; });
            std::ranges::sort(out, {}, &entry::seq);
        }
    }
    if (what.limit != 0 && out.size() > what.limit)
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(
                                                 out.size() - what.limit));
    return out;
}

std::optional<entry> recorder::last_failure(std::uint64_t endpoint) const
{
    std::vector<entry> found;
    {
        const std::lock_guard<std::mutex> hold(lock_);
        failures_->collect(
            found, [&](const entry& value)
            { return endpoint == 0 || value.endpoint == endpoint; });
    }
    if (found.empty())
        return std::nullopt;
    return found.back();
}

std::uint64_t recorder::last_seq() const noexcept
{
    return seq_.load();
}

std::uint64_t recorder::overwritten() const noexcept
{
    const std::lock_guard<std::mutex> hold(lock_);
    return all_->overwritten();
}

void recorder::clear() noexcept
{
    const std::lock_guard<std::mutex> hold(lock_);
    all_->clear();
    failures_->clear();
}

operation_scope::operation_scope(std::string_view name,
                                 std::uint64_t endpoint) noexcept
    : previous_(current_operation), previous_endpoint_(current_endpoint),
      previous_failure_(current_failure)
{
    set_text(current_operation, name);
    current_endpoint = endpoint;
    current_failure = 0;
}

operation_scope::~operation_scope()
{
    current_operation = previous_;
    current_endpoint = previous_endpoint_;
    current_failure = previous_failure_;
}

std::string_view operation_scope::current() noexcept
{
    return text_of(current_operation);
}

std::uint64_t operation_scope::last_failure() noexcept
{
    return current_failure;
}

call_scope::call_scope(std::string_view call, std::uint64_t endpoint,
                       std::string_view args, recorder& target) noexcept
    : target_(&target), started_(std::chrono::steady_clock::now())
{
    value_.endpoint = endpoint;
    set_text(value_.call, call);
    set_text(value_.args, args);
}

call_scope::~call_scope()
{
    if (!done_)
        ok();
}

void call_scope::ok() noexcept
{
    if (done_)
        return;
    done_ = true;
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    value_.duration_us = static_cast<std::uint32_t>(std::min<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count(),
        std::numeric_limits<std::uint32_t>::max()));
    target_->add(value_);
}

std::uint64_t call_scope::fail(const boost::system::error_code& code,
                               std::uint64_t cause) noexcept
{
    std::string message;
    try
    {
        message = code.message();
    }
    catch (...)
    {
    }
    return fail(code.value(), code.category().name(), message, cause);
}

std::uint64_t call_scope::fail(std::int32_t code, std::string_view category,
                               std::string_view message,
                               std::uint64_t cause) noexcept
{
    if (done_)
        return 0;
    done_ = true;
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    value_.duration_us = static_cast<std::uint32_t>(std::min<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count(),
        std::numeric_limits<std::uint32_t>::max()));
    value_.failed = true;
    value_.code = code;
    value_.cause = cause;
    set_text(value_.category, category);
    set_text(value_.message, message);
    return target_->add(value_);
}

std::uint64_t call_scope::finish(const boost::system::error_code& code) noexcept
{
    if (!code)
    {
        ok();
        return 0;
    }
    return fail(code);
}

std::uint64_t record_failure(std::string_view call, std::uint64_t endpoint,
                             std::string_view args,
                             const boost::system::error_code& code,
                             std::uint64_t cause) noexcept
{
    call_scope scope(call, endpoint, args);
    return scope.fail(code, cause);
}

void record_success(std::string_view call, std::uint64_t endpoint,
                    std::string_view args) noexcept
{
    call_scope scope(call, endpoint, args);
    scope.ok();
}

} // namespace vss::trace
