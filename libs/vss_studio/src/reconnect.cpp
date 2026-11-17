//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "reconnector.hpp"

#include <vss/core/profile.hpp>

#include <algorithm>

namespace vss {

std::chrono::milliseconds backoff_delay(const reconnect_policy& policy,
                                        unsigned attempt) noexcept
{
    using std::chrono::milliseconds;
    const milliseconds floor{1};
    const milliseconds cap = std::max(policy.max_delay, floor);
    milliseconds delay = std::max(policy.first_delay, floor);
    for (unsigned step = 0; step < attempt && delay < cap; ++step)
        delay *= 2;
    return std::min(delay, cap);
}

reconnector::reconnector(attempt_fn attempt, note_fn note)
    : attempt_(std::move(attempt)), note_(std::move(note))
{
    thread_ = std::thread(
        [this]
        {
            VSS_PROFILE_THREAD("vss.reconnect");
            run();
        });
}

reconnector::~reconnector()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        waiting_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void reconnector::set_policy(const reconnect_policy& policy)
{
    {
        std::lock_guard lock(mutex_);
        policy_ = policy;
        if (!policy_.enabled)
            waiting_.clear();
    }
    wake_.notify_all();
}

reconnect_policy reconnector::policy() const
{
    std::lock_guard lock(mutex_);
    return policy_;
}

void reconnector::allow(endpoint_id id)
{
    std::lock_guard lock(mutex_);
    allowed_.insert(id);
}

void reconnector::disallow(endpoint_id id)
{
    std::lock_guard lock(mutex_);
    allowed_.erase(id);
    waiting_.erase(id);
}

void reconnector::lost(endpoint_id id)
{
    std::chrono::milliseconds delay{};
    {
        std::lock_guard lock(mutex_);
        if (!policy_.enabled || stopping_ || allowed_.count(id) == 0 ||
            waiting_.count(id) != 0)
            return;
        delay = backoff_delay(policy_, 0);
        waiting_[id] = entry{std::chrono::steady_clock::now() + delay, 0};
        VSS_PROFILE_PLOT("studio.reconnect_pending", waiting_.size());
    }
    VSS_PROFILE_MESSAGE("studio.reconnect.lost");
    announce(id, "connection lost; trying again in " +
                     std::to_string(delay.count()) + " ms");
    wake_.notify_all();
}

void reconnector::forget(endpoint_id id)
{
    std::lock_guard lock(mutex_);
    waiting_.erase(id);
}

std::size_t reconnector::pending() const
{
    std::lock_guard lock(mutex_);
    return waiting_.size();
}

reconnector::progress reconnector::status(endpoint_id id) const
{
    std::lock_guard lock(mutex_);
    progress out;
    if (const auto waiting = waiting_.find(id); waiting != waiting_.end())
    {
        out.retrying = true;
        out.attempts = waiting->second.attempts;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            waiting->second.due - std::chrono::steady_clock::now());
        out.next_in = std::max(left, std::chrono::milliseconds{0});
    }
    if (const auto code = last_codes_.find(id); code != last_codes_.end())
        out.last_error_code = code->second;
    if (const auto error = last_errors_.find(id); error != last_errors_.end())
        out.last_error = error->second;
    return out;
}

void reconnector::announce(endpoint_id id, const std::string& text) const
{
    if (note_)
        note_(id, text);
}

void reconnector::run()
{
    std::unique_lock lock(mutex_);
    while (!stopping_)
    {
        if (waiting_.empty())
        {
            wake_.wait(lock);
            continue;
        }

        const auto next = std::min_element(
            waiting_.begin(), waiting_.end(),
            [](const auto& left, const auto& right)
            { return left.second.due < right.second.due; });
        // Copy the time: the wait gives up the mutex, so disallow() or forget()
        // can erase this entry, and the wait reads its time again when it wakes.
        const auto due = next->second.due;
        if (due > std::chrono::steady_clock::now())
        {
            wake_.wait_until(lock, due);
            continue;
        }

        const endpoint_id id = next->first;
        const unsigned attempt = next->second.attempts;
        lock.unlock();
        VSS_PROFILE_MESSAGE("studio.reconnect.attempt");
        result<void> outcome = make_error_code(errc::internal);
        {
            VSS_PROFILE_ZONE("studio.reconnect.attempt");
            outcome = attempt_(id);
        }
        lock.lock();

        const auto found = waiting_.find(id);
        if (found == waiting_.end())
            continue; // forgotten while the try ran

        if (outcome)
        {
            waiting_.erase(found);
            last_errors_.erase(id);
            last_codes_.erase(id);
            VSS_PROFILE_PLOT("studio.reconnect_pending", waiting_.size());
            VSS_PROFILE_MESSAGE("studio.reconnect.succeeded");
            announce(id, "reconnected after " + std::to_string(attempt + 1) +
                             (attempt == 0 ? " try" : " tries"));
            continue;
        }

        const error_code error = outcome.error();
        if (error == make_error_code(errc::not_found) ||
            error == make_error_code(errc::unsupported))
        {
            waiting_.erase(found);
            continue;
        }

        last_errors_[id] = error.message();
        last_codes_[id] =
            std::string(error.category().name()) + ":" +
            std::to_string(error.value());
        const unsigned made = attempt + 1;
        if (policy_.max_attempts != 0 && made >= policy_.max_attempts)
        {
            waiting_.erase(found);
            VSS_PROFILE_MESSAGE("studio.reconnect.gave_up");
            announce(id, "giving up after " + std::to_string(made) +
                             " tries: " + error.message());
            continue;
        }

        const auto delay = backoff_delay(policy_, made);
        found->second.attempts = made;
        found->second.due = std::chrono::steady_clock::now() + delay;
        announce(id, "could not reconnect (" + error.message() +
                         "); trying again in " + std::to_string(delay.count()) +
                         " ms");
    }
}

} // namespace vss
