//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Response timing rules on a studio's ports: installing a compiled set at
// its place, carrying out what its rules plan, and removing it.  The
// matcher and the scheduler are in vss_rules; what is here is the part that
// knows the ports.
//

#include <vss/core/bytes.hpp>
#include <vss/studio/rules_build.hpp>
#include <vss/studio/studio.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss {

namespace
{

/// The studio's fault for a rule's line error.
fault_kind fault_for(rules::line_error error)
{
    switch (error)
    {
    case rules::line_error::parity:
        return fault_kind::parity;
    case rules::line_error::framing:
        return fault_kind::framing;
    case rules::line_error::overrun:
        return fault_kind::overrun;
    case rules::line_error::break_signal:
        return fault_kind::break_condition;
    case rules::line_error::hang_up:
        return fault_kind::hangup;
    case rules::line_error::disconnect:
        break;
    }
    return fault_kind::disconnect;
}

/// How a set is named in a line of the Log: `rule set "meter"`, or `rule set`
/// when it has no name.  The name is any text of the document's, so its line
/// feeds and other control characters are written out or replaced: a name
/// must not be able to start a line of its own.
std::string label_of(std::string_view name)
{
    if (name.empty())
        return "rule set";
    return "rule set \"" + to_printable(as_bytes(name)) + "\"";
}

/// Says `message` in the Log, on the port.  Posts to the hub's dispatcher, so
/// it may be called from any thread, and from a scheduler's strand.
void say(hub& engine, endpoint_id id, std::string port, std::string message)
{
    endpoint_event event;
    event.kind = event_kind::log_message;
    event.endpoint = id;
    event.name = std::move(port);
    event.message = std::move(message);
    event.when = engine.now();
    engine.publish(std::move(event));
}

/// The late actions of a set that have not been told yet.  Owned by the
/// `done` hook of one scheduler, which calls it one action at a time.
struct late_report
{
    std::uint64_t count = 0;
    clock_type::duration worst{0};
    std::optional<time_point> told_at;
};

/// How seldom a set says that actions were late.
constexpr clock_type::duration late_line_interval = std::chrono::seconds(1);

/// Gives a scheduler's hooks what makes a suspension and a late action
/// visible: a line in the Log on the port.  Both hooks run on the scheduler's
/// strand and only publish.  They hold the hub as `hooks.now` does: a
/// scheduler is closed before the hub goes.
void observe_set(rules::scheduler_hooks& hooks, hub* engine, endpoint_id id,
                 const std::weak_ptr<endpoint>& port,
                 const std::string& set_name, std::uint32_t firings_per_second)
{
    const std::string label = label_of(set_name);
    const std::uint32_t limit = std::max<std::uint32_t>(firings_per_second, 1);

    hooks.suspended =
        [engine, id, port, label, limit](rules::suspend_reason why, time_point)
    {
        const std::shared_ptr<endpoint> place = port.lock();
        if (!place)
            return;
        std::string text = label + " suspended";
        if (why == rules::suspend_reason::budget)
            text += ": it fired more than " + std::to_string(limit) +
                    " times a second. It stays suspended until it is "
                    "resumed.";
        else
            text += ".";
        say(*engine, id, place->name(), std::move(text));
    };

    hooks.done =
        [engine, id, port, label, state = std::make_shared<late_report>()](
            const rules::action_report& report)
    {
        if (!report.late)
            return;
        ++state->count;
        state->worst = std::max(state->worst, report.done - report.due);
        if (state->told_at &&
            report.done - *state->told_at < late_line_interval)
            return;
        const std::shared_ptr<endpoint> place = port.lock();
        if (!place)
            return;
        const auto worst =
            std::chrono::duration_cast<std::chrono::microseconds>(state->worst)
                .count();
        say(*engine, id, place->name(),
            label + ": " + std::to_string(state->count) +
                " action(s) late, the worst by " + std::to_string(worst) +
                " microseconds");
        state->told_at = report.done;
        state->count = 0;
        state->worst = clock_type::duration{0};
    };
}

} // namespace

result<studio::rules_plan> studio::plan_rules(
    const rules::compiled_rule_set* set, std::vector<rules::problem>& found)
{
    if (set == nullptr)
        return make_error_code(errc::invalid_argument);
    // The role is looked at before the port: the port may be there or not.
    if (set->attach.as != rules::role::peer)
    {
        check_rules_for_build(*set, found);
        return make_error_code(errc::unsupported);
    }
    rules_plan plan;
    plan.place = hub_->find_endpoint(set->attach.port);
    if (!plan.place)
    {
        found.push_back(rules::problem{
            "attach.port", "there is no port named \"" + set->attach.port +
                               "\""});
        return make_error_code(errc::not_found);
    }
    const endpoint_id id = plan.place->id();

    bool takes_faults = false;
    {
        std::lock_guard lock(record_mutex_);
        if (const auto it = records_.find(id); it != records_.end())
            takes_faults = !it->second.fixture.expired() ||
                           !it->second.driver.expired();
    }
    check_rules_for_build(*set, found, takes_faults);
    if (!found.empty())
        return make_error_code(errc::unsupported);

    plan.hooks.now = [hub = hub_.get()] { return hub->now(); };
    plan.hooks.poll_clock = static_cast<bool>(options_.hub.clock);
    plan.hooks.act = [this, id, target = std::weak_ptr<endpoint>(plan.place)](
                         const rules::planned_action& action)
    { carry_out_rule_action(id, target.lock(), action); };
    return plan;
}

result<rules_installation> studio::install_rules(
    std::shared_ptr<const rules::compiled_rule_set> set,
    const rules_install_options& options,
    std::vector<rules::problem>* problems)
{
    std::vector<rules::problem> found;
    const auto refuse = [&](errc code) -> result<rules_installation>
    {
        if (problems != nullptr)
            *problems = std::move(found);
        return make_error_code(code);
    };

    result<rules_plan> plan = plan_rules(set.get(), found);
    if (!plan)
    {
        if (problems != nullptr)
            *problems = std::move(found);
        return plan.error();
    }
    const std::shared_ptr<endpoint> place = std::move(plan->place);
    const endpoint_id id = place->id();
    rules::scheduler_hooks hooks = std::move(plan->hooks);
    observe_set(hooks, hub_.get(), id, place, set->name,
                options.limits.firings_per_second);
    const std::string label = label_of(set->name);
    const std::size_t rule_count = set->rules.size();

    rules_installation installed;
    installed.place = id;
    std::shared_ptr<rules::scheduler> made;
    {
        std::lock_guard lock(rules_mutex_);
        if (rules_.contains(id))
        {
            found.push_back(rules::problem{
                "attach.port", "\"" + set->attach.port +
                                   "\" has a rule set already"});
            return refuse(errc::already_exists);
        }
        if (rules_.size() >= rules::max_hub_sets)
        {
            found.push_back(rules::problem{
                "", "the studio has " + std::to_string(rules::max_hub_sets) +
                        " rule sets already"});
            return refuse(errc::queue_full);
        }
        if (set->size_bytes > rules::max_hub_bytes - rules_bytes_)
        {
            found.push_back(rules::problem{
                "", "the rule sets of the studio would take more than " +
                        std::to_string(rules::max_hub_bytes) + " bytes"});
            return refuse(errc::queue_full);
        }
        rules_bytes_ += set->size_bytes;
        made = std::make_shared<rules::scheduler>(
            hub_->executor(), std::move(set), options.run_seed,
            std::move(hooks), options.limits);
        installed.generation = next_rules_generation_++;
        installed.seed = made->seed();
        rules_.emplace(id, rules_record{made, installed.generation, {}});

        // The set's run starts now.  It sees the chunks from the next one
        // the port reads: the stage is put on the endpoint's strand, between
        // two.  Still under the lock, so that a removal cannot come between
        // the record and the stage; neither call waits for anything.
        made->start(hub_->now());
        place->set_read_stage(made);
    }
    say(*hub_, id, place->name(),
        label + " installed: generation " +
            std::to_string(installed.generation) + ", seed " +
            std::to_string(installed.seed) + ", " + std::to_string(rule_count) +
            " rule(s)");
    if (problems != nullptr)
        problems->clear();
    return installed;
}

result<rules_installation> studio::replace_rules(
    std::shared_ptr<const rules::compiled_rule_set> set,
    const rules_replace_options& options,
    std::vector<rules::problem>* problems)
{
    std::vector<rules::problem> found;
    const auto refuse = [&](std::error_code code) -> result<rules_installation>
    {
        if (problems != nullptr)
            *problems = std::move(found);
        return code;
    };

    result<rules_plan> plan = plan_rules(set.get(), found);
    if (!plan)
        return refuse(plan.error());
    const endpoint_id id = plan->place->id();
    observe_set(plan->hooks, hub_.get(), id, plan->place, set->name,
                options.limits.firings_per_second);
    const std::string label = label_of(set->name);
    std::uint64_t replaced_generation = 0;

    rules_installation installed;
    installed.place = id;
    // Sets replaced earlier that have nothing left to do.  Closed after the
    // lock is let go: a close waits for an action under way.
    std::vector<std::shared_ptr<rules::scheduler>> finished;
    {
        std::lock_guard lock(rules_mutex_);
        const auto it = rules_.find(id);
        if (it == rules_.end())
        {
            found.push_back(rules::problem{
                "attach.port", "\"" + set->attach.port +
                                   "\" has no rule set to replace"});
            return refuse(make_error_code(errc::not_found));
        }
        rules_record& record = it->second;
        const std::size_t others =
            rules_bytes_ - record.scheduler->set().size_bytes;
        if (set->size_bytes > rules::max_hub_bytes - others)
        {
            found.push_back(rules::problem{
                "", "the rule sets of the studio would take more than " +
                        std::to_string(rules::max_hub_bytes) + " bytes"});
            return refuse(make_error_code(errc::queue_full));
        }
        rules_bytes_ = others + set->size_bytes;

        const auto made = std::make_shared<rules::scheduler>(
            hub_->executor(), std::move(set), options.run_seed,
            std::move(plan->hooks), options.limits);
        // From here the old set hands what it is shown to the new one, so
        // it does not matter which of the two the port shows a chunk to
        // before its stage is changed, at its next turn.
        made->start_in_place_of(*record.scheduler, hub_->now(),
                                options.keep_actions, options.carry_state);
        plan->place->set_read_stage(made);

        std::erase_if(record.replaced,
                      [&](std::shared_ptr<rules::scheduler>& each)
                      {
                          if (each->waiting() != 0)
                              return false;
                          finished.push_back(std::move(each));
                          return true;
                      });
        record.replaced.push_back(std::move(record.scheduler));
        record.scheduler = made;
        replaced_generation = record.generation;
        record.generation = next_rules_generation_++;
        installed.generation = record.generation;
        installed.seed = made->seed();
    }
    for (const auto& each : finished)
        each->close();
    say(*hub_, id, plan->place->name(),
        label + " installed in place of generation " +
            std::to_string(replaced_generation) + ": generation " +
            std::to_string(installed.generation) + ", seed " +
            std::to_string(installed.seed));
    if (problems != nullptr)
        problems->clear();
    return installed;
}

result<void> studio::remove_rules(endpoint_id place)
{
    std::shared_ptr<rules::scheduler> removed;
    std::vector<std::shared_ptr<rules::scheduler>> replaced;
    std::uint64_t generation = 0;
    std::optional<std::string> port_name;
    {
        std::lock_guard lock(rules_mutex_);
        const auto it = rules_.find(place);
        if (it == rules_.end())
            return make_error_code(errc::not_found);
        removed = std::move(it->second.scheduler);
        replaced = std::move(it->second.replaced);
        generation = it->second.generation;
        rules_.erase(it);
        rules_bytes_ -= removed->set().size_bytes;
        // Asked under the lock, so that the stage of a set installed right
        // after this one is not the one taken off.
        if (const auto endpoint = hub_->find_endpoint(place))
        {
            endpoint->set_read_stage(nullptr);
            port_name = endpoint->name();
        }
    }
    // Nothing of the set is done after this returns.  The endpoint lets go
    // of the stage at its next turn; until then the stage ignores what it
    // is shown.
    removed->close();
    for (const auto& each : replaced)
        each->close();
    if (port_name)
        say(*hub_, place, *port_name,
            label_of(removed->set().name) + " removed: generation " +
                std::to_string(generation));
    return {};
}

std::shared_ptr<rules::scheduler> studio::rules_of(endpoint_id place) const
{
    std::lock_guard lock(rules_mutex_);
    const auto it = rules_.find(place);
    return it == rules_.end() ? nullptr : it->second.scheduler;
}

result<void> studio::raise_signal(endpoint_id place, std::string_view signal,
                                  bytes_view carried, time_point cause)
{
    const std::shared_ptr<rules::scheduler> rules = rules_of(place);
    if (!rules)
        return make_error_code(errc::not_found);
    const std::vector<std::string>& declared = rules->set().signals;
    std::size_t index = 0;
    while (index < declared.size() && declared[index] != signal)
        ++index;
    if (index == declared.size())
        return make_error_code(errc::not_found);
    if (carried.size() > rules::max_signal_bytes)
        return make_error_code(errc::invalid_argument);
    // The cause is an event the engine reported, so it is on the hub's
    // clock and in the past.  One from the future, or a stale one, is a
    // mistake of the caller and is not acted on.
    const time_point now = hub_->now();
    if (cause > now ||
        now - cause >
            std::chrono::duration_cast<clock_type::duration>(
                rules::max_signal_age))
        return make_error_code(errc::invalid_argument);
    return rules->signal(index, carried, cause, now);
}

result<void> studio::suspend_rules(endpoint_id place)
{
    const std::shared_ptr<rules::scheduler> rules = rules_of(place);
    if (!rules)
        return make_error_code(errc::not_found);
    rules->suspend(hub_->now());
    return {};
}

result<void> studio::resume_rules(endpoint_id place)
{
    const std::shared_ptr<rules::scheduler> rules = rules_of(place);
    if (!rules)
        return make_error_code(errc::not_found);
    if (rules->resume(hub_->now()))
    {
        if (const auto endpoint = hub_->find_endpoint(place))
            say(*hub_, place, endpoint->name(),
                label_of(rules->set().name) + " resumed");
    }
    return {};
}

result<rules::scheduler_stats> studio::rule_stats(endpoint_id place) const
{
    const std::shared_ptr<rules::scheduler> rules = rules_of(place);
    if (!rules)
        return make_error_code(errc::not_found);
    return rules->stats();
}

std::optional<std::uint64_t> studio::rules_generation(endpoint_id place) const
{
    std::lock_guard lock(rules_mutex_);
    const auto it = rules_.find(place);
    if (it == rules_.end())
        return std::nullopt;
    return it->second.generation;
}

void studio::close_all_rules()
{
    std::unordered_map<endpoint_id, rules_record> all;
    {
        std::lock_guard lock(rules_mutex_);
        all.swap(rules_);
        rules_bytes_ = 0;
    }
    for (auto& [id, record] : all)
    {
        record.scheduler->close();
        for (const auto& each : record.replaced)
            each->close();
        if (const auto endpoint = hub_->find_endpoint(id))
            endpoint->set_read_stage(nullptr);
    }
}

void studio::carry_out_rule_action(endpoint_id id,
                                   const std::shared_ptr<endpoint>& port,
                                   const rules::planned_action& action)
{
    if (!port)
        return;
    switch (action.kind)
    {
    case rules::action_kind::reply:
        // A reply is one write, so it is never mixed with another's bytes.
        for (unsigned copy = 0; copy < action.copies; ++copy)
            port->write(bytes_view(action.bytes));
        break;
    case rules::action_kind::set_lines:
        if (action.source != nullptr)
        {
            control_lines lines = port->lines();
            for (const rules::line_value& line : action.source->lines)
                lines.set(line.which, line.high);
            port->set_peer_inputs(lines);
        }
        break;
    case rules::action_kind::raise_error:
        if (action.source != nullptr)
            static_cast<void>(
                inject_fault(id, fault_for(action.source->error)));
        break;
    case rules::action_kind::mark:
        if (action.source != nullptr)
        {
            endpoint_event event;
            event.kind = event_kind::log_message;
            event.endpoint = id;
            event.name = port->name();
            event.message = action.source->text;
            event.when = hub_->now();
            hub_->publish(std::move(event));
        }
        break;
    case rules::action_kind::pass:
    case rules::action_kind::set_state:
    case rules::action_kind::set_slot:
    case rules::action_kind::ask:
        break;
    }
}

} // namespace vss
