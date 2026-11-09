//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>

#include <vss/studio/responder.hpp>

#include "timed_queue.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>

namespace vss {

namespace json = boost::json;
using std::chrono::milliseconds;

namespace {

/// An hour is longer than any delay a test or a bench needs, and it keeps a
/// typo (a missing decimal point) from scheduling a reply for next week.
constexpr std::int64_t max_milliseconds = 3'600'000;

result<responder_script> bad_script(std::string* detail, std::string message)
{
    if (detail != nullptr)
        *detail = std::move(message);
    return make_error_code(errc::config_error);
}

/// Complains about keys the schema does not have.
std::optional<std::string> unknown_key(const json::object& object,
                                       const std::set<std::string>& allowed,
                                       const std::string& where)
{
    for (const auto& entry : object)
    {
        if (allowed.count(std::string(entry.key())) == 0)
            return where + ": unknown key \"" + std::string(entry.key()) + "\"";
    }
    return std::nullopt;
}

/// Reads a hex string member.  Returns an error text, or nothing.
std::optional<std::string> hex_member(const json::object& object,
                                      const char* key, const std::string& where,
                                      bool required, byte_buffer& out)
{
    const auto* value = object.if_contains(key);
    if (value == nullptr)
    {
        if (required)
            return where + ": \"" + key + "\" is missing";
        return std::nullopt;
    }
    if (!value->is_string())
        return where + ": \"" + key + "\" must be a string of hex bytes";
    const auto parsed = from_hex(value->as_string().c_str());
    if (!parsed || parsed->empty())
        return where + ": \"" + key + "\" is not hex bytes (like \"02 31 03\")";
    out = *parsed;
    return std::nullopt;
}

std::optional<std::string> ms_member(const json::object& object, const char* key,
                                     const std::string& where, milliseconds& out)
{
    const auto* value = object.if_contains(key);
    if (value == nullptr)
        return std::nullopt;
    if (!value->is_int64() && !value->is_uint64())
        return where + ": \"" + key + "\" must be a whole number of milliseconds";
    const std::int64_t count = value->is_int64()
        ? value->as_int64()
        : static_cast<std::int64_t>(std::min<std::uint64_t>(
              value->as_uint64(), static_cast<std::uint64_t>(max_milliseconds + 1)));
    if (count < 0 || count > max_milliseconds)
        return where + ": \"" + key + "\" must be between 0 and 3600000";
    out = milliseconds(count);
    return std::nullopt;
}

} // namespace

result<responder_script> parse_responder_script(std::string_view text,
                                                std::string* detail)
{
    VSS_PROFILE_ZONE("studio.responder.parse_script");
    // Every failure goes through bad_script(), which stores a readable
    // message in `detail` for the caller and returns a config error.
    boost::system::error_code parse_error;
    const json::value root = json::parse(text, parse_error);
    if (parse_error)
        return bad_script(detail, "not JSON: " + parse_error.message());
    if (!root.is_object())
        return bad_script(detail, "the script must be a JSON object");
    const json::object& top = root.as_object();

    // Reject keys the schema does not have, so a misspelt key is reported
    // rather than silently ignored.  The version may be left out, but if it
    // is given it must be 1.
    if (const auto problem = unknown_key(
            top, {"version", "name", "description", "seed", "rules", "poll"},
            "script"))
        return bad_script(detail, *problem);
    if (const auto* version = top.if_contains("version");
        version != nullptr && (!version->is_int64() || version->as_int64() != 1))
        return bad_script(detail, "script: \"version\" must be 1");

    // The seed, if any, fixes the random jitter so a run can be repeated.
    // It may be written as a signed or an unsigned whole number.
    responder_script script;
    if (const auto* seed = top.if_contains("seed"))
    {
        if (!seed->is_uint64() && !seed->is_int64())
            return bad_script(detail, "script: \"seed\" must be a whole number");
        script.seed = seed->is_uint64()
            ? seed->as_uint64()
            : static_cast<std::uint64_t>(seed->as_int64());
    }

    // Rules: "when these bytes arrive, send those".  Errors name the rule
    // by its position, such as rules[2].
    if (const auto* rules = top.if_contains("rules"))
    {
        if (!rules->is_array())
            return bad_script(detail, "script: \"rules\" must be a list");
        std::size_t index = 0;
        for (const auto& item : rules->as_array())
        {
            const std::string where = "rules[" + std::to_string(index++) + "]";
            if (!item.is_object())
                return bad_script(detail, where + " must be an object");
            const json::object& object = item.as_object();
            if (const auto problem = unknown_key(
                    object,
                    {"name", "match", "mask", "reply", "delay_ms", "jitter_ms"},
                    where))
                return bad_script(detail, *problem);

            // The name is for display only.  Match and reply are required;
            // the mask is optional but must cover exactly the match bytes.
            responder_rule rule;
            if (const auto* name = object.if_contains("name");
                name != nullptr && name->is_string())
                rule.name = std::string(name->as_string().c_str());
            if (const auto problem =
                    hex_member(object, "match", where, true, rule.match))
                return bad_script(detail, *problem);
            if (const auto problem =
                    hex_member(object, "mask", where, false, rule.mask))
                return bad_script(detail, *problem);
            if (!rule.mask.empty() && rule.mask.size() != rule.match.size())
                return bad_script(detail,
                                  where + ": \"mask\" must have as many bytes "
                                          "as \"match\"");
            if (const auto problem =
                    hex_member(object, "reply", where, true, rule.reply))
                return bad_script(detail, *problem);
            // The reply delay and its jitter are optional, in milliseconds.
            if (const auto problem =
                    ms_member(object, "delay_ms", where, rule.delay))
                return bad_script(detail, *problem);
            if (const auto problem =
                    ms_member(object, "jitter_ms", where, rule.jitter))
                return bad_script(detail, *problem);
            script.rules.push_back(std::move(rule));
        }
    }

    // Poll: a list of timed steps for a port that acts as a master.  It may
    // repeat a number of cycles, one interval apart.
    if (const auto* poll_value = top.if_contains("poll"))
    {
        if (!poll_value->is_object())
            return bad_script(detail, "script: \"poll\" must be an object");
        const json::object& object = poll_value->as_object();
        if (const auto problem = unknown_key(
                object, {"start_ms", "interval_ms", "cycles", "steps"}, "poll"))
            return bad_script(detail, *problem);

        responder_poll poll;
        if (const auto problem = ms_member(object, "start_ms", "poll", poll.start))
            return bad_script(detail, *problem);
        if (const auto problem =
                ms_member(object, "interval_ms", "poll", poll.interval))
            return bad_script(detail, *problem);
        if (const auto* cycles = object.if_contains("cycles"))
        {
            if (!cycles->is_int64() || cycles->as_int64() < 0)
                return bad_script(detail, "poll: \"cycles\" must be 0 or more");
            poll.cycles = static_cast<std::uint64_t>(cycles->as_int64());
        }
        // More than one cycle needs a non-zero interval to space them out.
        if (poll.interval.count() == 0 && poll.cycles > 1)
            return bad_script(detail,
                              "poll: \"interval_ms\" is needed to repeat");

        // A poll with no steps would do nothing, so at least one is needed.
        const auto* steps = object.if_contains("steps");
        if (steps == nullptr || !steps->is_array() || steps->as_array().empty())
            return bad_script(detail, "poll: \"steps\" must be a list with at "
                                      "least one step");
        std::size_t index = 0;
        for (const auto& item : steps->as_array())
        {
            const std::string where = "poll.steps[" + std::to_string(index++) + "]";
            if (!item.is_object())
                return bad_script(detail, where + " must be an object");
            const json::object& step_object = item.as_object();
            if (const auto problem = unknown_key(
                    step_object, {"send", "at_ms", "jitter_ms"}, where))
                return bad_script(detail, *problem);
            // Each step sends some bytes, at an offset within the cycle plus
            // optional jitter.
            responder_poll_step step;
            if (const auto problem =
                    hex_member(step_object, "send", where, true, step.send))
                return bad_script(detail, *problem);
            if (const auto problem = ms_member(step_object, "at_ms", where, step.at))
                return bad_script(detail, *problem);
            if (const auto problem =
                    ms_member(step_object, "jitter_ms", where, step.jitter))
                return bad_script(detail, *problem);
            poll.steps.push_back(std::move(step));
        }
        script.poll = std::move(poll);
    }

    // A script with neither rules nor a poll would never do anything.
    if (script.rules.empty() && !script.poll)
        return bad_script(detail, "the script has no \"rules\" and no \"poll\"");
    return script;
}

result<responder_script> load_responder_script(const std::string& path,
                                               std::string* detail)
{
    VSS_PROFILE_ZONE("studio.responder.load_script");
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error || status.type() == std::filesystem::file_type::not_found)
        return bad_script(detail, "no such file");
    if (!std::filesystem::is_regular_file(status))
        return bad_script(detail, "not a regular file");
    if (std::filesystem::file_size(path, status_error) >
        (std::uintmax_t{1} << 20))
        return bad_script(detail, "the script is over 1 MiB");

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return bad_script(detail, "cannot be read");
    std::ostringstream text;
    text << stream.rdbuf();
    return parse_responder_script(text.str(), detail);
}

// -- the running responder ---------------------------------------------------------

struct responder::state : std::enable_shared_from_this<state>
{
    std::shared_ptr<endpoint> target;
    responder_script script;
    std::shared_ptr<detail::timed_queue> queue;
    time_point origin{};
    hub* engine = nullptr;
    std::atomic<subscriber_id> subscription{0};
    std::size_t longest_match = 0;

    std::mutex mutex; // the stream buffer and the generator
    byte_buffer buffer;
    std::mt19937_64 generator{std::random_device{}()};

    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> trigger_count{0};
    std::atomic<std::uint64_t> reply_count{0};
    std::atomic<std::uint64_t> poll_count{0};
    std::atomic<bool> poll_done{false};

    /// `delay` plus or minus up to `jitter`, never below zero.
    milliseconds jittered(milliseconds delay, milliseconds jitter)
    {
        if (jitter.count() == 0)
            return delay;
        std::lock_guard lock(mutex);
        std::uniform_int_distribution<std::int64_t> spread(-jitter.count(),
                                                           jitter.count());
        return milliseconds(std::max<std::int64_t>(0, delay.count() + spread(generator)));
    }

    void send_at(time_point when, const byte_buffer& data,
                 std::atomic<std::uint64_t>& counter)
    {
        auto self = shared_from_this();
        queue->at(when, [self, data, &counter] {
            if (self->stopped.load())
                return;
            self->target->write(data);
            counter.fetch_add(1);
        });
    }

    static bool matches_at(const byte_buffer& data, std::size_t position,
                           const responder_rule& rule)
    {
        if (position + rule.match.size() > data.size())
            return false;
        for (std::size_t i = 0; i < rule.match.size(); ++i)
        {
            const byte mask = rule.mask.empty() ? byte{0xff} : rule.mask[i];
            if ((data[position + i] & mask) != (rule.match[i] & mask))
                return false;
        }
        return true;
    }

    void on_event(const endpoint_event& event)
    {
        VSS_PROFILE_ZONE("studio.responder.match");
        if (stopped.load() || event.kind != event_kind::data || !event.data ||
            event.direction != data_direction::rx ||
            event.endpoint != target->id())
            return;

        // Find the triggers under the lock, schedule them outside it.
        std::vector<const responder_rule*> hits;
        {
            std::lock_guard lock(mutex);
            buffer.insert(buffer.end(), event.data->begin(), event.data->end());
            for (;;)
            {
                std::size_t found_at = buffer.size();
                const responder_rule* found = nullptr;
                for (std::size_t position = 0;
                     position < buffer.size() && found == nullptr; ++position)
                {
                    for (const auto& rule : script.rules)
                    {
                        if (matches_at(buffer, position, rule))
                        {
                            found = &rule;
                            found_at = position;
                            break;
                        }
                    }
                }
                if (found == nullptr)
                    break;
                hits.push_back(found);
                buffer.erase(buffer.begin(),
                             buffer.begin() + static_cast<std::ptrdiff_t>(
                                                  found_at + found->match.size()));
            }
            // Keep only what could still become the start of a match.
            if (longest_match > 1 && buffer.size() > longest_match - 1)
                buffer.erase(buffer.begin(),
                             buffer.end() - static_cast<std::ptrdiff_t>(longest_match - 1));
            else if (longest_match <= 1)
                buffer.clear();
        }

        for (const auto* rule : hits)
        {
            trigger_count.fetch_add(1);
            const auto delay = jittered(rule->delay, rule->jitter);
            send_at(event.when + delay, rule->reply, reply_count);
        }
    }

    /// Queues the steps of one cycle of the poll list.  The first action of a
    /// cycle queues the next, one interval ahead of its steps.
    void schedule_cycle(std::uint64_t cycle)
    {
        VSS_PROFILE_ZONE("studio.responder.poll_cycle");
        if (!script.poll)
            return;
        const auto& poll = *script.poll;
        const time_point begin =
            origin + poll.start +
            std::chrono::duration_cast<clock_type::duration>(
                poll.interval * static_cast<std::int64_t>(cycle));

        for (const auto& step : poll.steps)
        {
            const auto offset = jittered(step.at, step.jitter);
            send_at(begin + offset, step.send, poll_count);
        }

        const bool repeats = poll.interval.count() > 0 &&
                             (poll.cycles == 0 || cycle + 1 < poll.cycles);
        auto self = shared_from_this();
        if (repeats)
        {
            queue->at(begin, [self, cycle] {
                if (!self->stopped.load())
                    self->schedule_cycle(cycle + 1);
            });
        }
        else
        {
            // After the last step of the last cycle.
            milliseconds last{0};
            for (const auto& step : poll.steps)
                last = std::max(last, step.at + step.jitter);
            queue->at(begin + last, [self] { self->poll_done.store(true); });
        }
    }
};

responder::responder(std::shared_ptr<state> shared)
    : state_(std::move(shared))
{
}

responder::~responder()
{
    try
    {
        stop();
    }
    catch (...)
    {
        // best effort: a destructor must not throw, and nothing can act on a failed stop
    }
}

result<std::shared_ptr<responder>> responder::start(
    hub& engine, std::shared_ptr<endpoint> target, const responder_script& script)
{
    VSS_PROFILE_ZONE("studio.responder.start");
    if (!target || (script.rules.empty() && !script.poll))
        return make_error_code(errc::invalid_argument);
    for (const auto& rule : script.rules)
    {
        if (rule.match.empty() ||
            (!rule.mask.empty() && rule.mask.size() != rule.match.size()))
            return make_error_code(errc::invalid_argument);
    }
    if (script.poll && script.poll->steps.empty())
        return make_error_code(errc::invalid_argument);

    auto shared = std::make_shared<state>();
    shared->engine = &engine;
    shared->target = std::move(target);
    shared->script = script;
    shared->queue = std::make_shared<detail::timed_queue>(engine.executor());
    if (script.seed)
        shared->generator.seed(*script.seed);
    for (const auto& rule : shared->script.rules)
        shared->longest_match = std::max(shared->longest_match, rule.match.size());
    shared->origin = clock_type::now();

    if (!shared->script.rules.empty())
    {
        event_filter filter;
        filter.endpoint = shared->target->id();
        filter.kind = event_kind::data;
        // The sink owns the state, so a delivery that is running when the
        // responder is destroyed still has something to work on.
        shared->subscription = engine.subscribe(
            filter, [state = shared](const endpoint_event& event) {
                state->on_event(event);
            });
    }
    if (shared->script.poll)
        shared->schedule_cycle(0);
    else
        shared->poll_done.store(true);

    return std::shared_ptr<responder>(new responder(std::move(shared)));
}

void responder::stop()
{
    VSS_PROFILE_ZONE("studio.responder.stop");
    state_->stopped.store(true);
    // The hub must outlive the responder, or the responder be stopped first.
    if (const auto id = state_->subscription.exchange(0); id != 0)
        state_->engine->unsubscribe(id);
    state_->queue->stop();
}

std::uint64_t responder::matches() const
{
    return state_->trigger_count.load();
}

std::uint64_t responder::replies_sent() const
{
    return state_->reply_count.load();
}

std::uint64_t responder::polls_sent() const
{
    return state_->poll_count.load();
}

bool responder::poll_finished() const
{
    return state_->poll_done.load();
}

} // namespace vss
