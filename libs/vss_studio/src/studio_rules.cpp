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

#include <vss/studio/studio.hpp>

#include <chrono>
#include <cstddef>
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

/// What this build cannot do for a set, found before anything is installed.
class install_check
{
public:
    install_check(const rules::compiled_rule_set& set, bool takes_faults,
                  std::vector<rules::problem>& problems)
        : set_(set), takes_faults_(takes_faults), problems_(problems)
    {
    }

    void run()
    {
        for (std::size_t i = 0; i < set_.rules.size(); ++i)
        {
            const rules::compiled_rule& rule = set_.rules[i];
            const std::string path = "rules[" + std::to_string(i) + "]";
            switch (rule.kind)
            {
            case rules::trigger_kind::bytes:
            case rules::trigger_kind::quiet:
            case rules::trigger_kind::signal:
                break;
            case rules::trigger_kind::lines:
                bad(path + ".when.lines",
                    "a rule on \"lines\" cannot be installed in this build");
                break;
            case rules::trigger_kind::port:
                bad(path + ".when.port",
                    "a rule on \"port\" cannot be installed in this build");
                break;
            }
            // The path the writer of the document will find the action at.
            for (std::size_t s = 0; s < rule.sequence.size(); ++s)
                actions(rule.sequence[s].actions,
                        rule.written_as_do
                            ? path + ".do"
                            : path + ".sequence[" + std::to_string(s) +
                                  "].do");
        }
        for (std::size_t i = 0; i < set_.timeline.size(); ++i)
            actions(set_.timeline[i].actions,
                    "timeline[" + std::to_string(i) + "].do");
    }

private:
    void bad(std::string path, std::string message)
    {
        problems_.push_back(
            rules::problem{std::move(path), std::move(message)});
    }

    void actions(const std::vector<rules::compiled_action>& list,
                 const std::string& path)
    {
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            const rules::compiled_action& action = list[i];
            const std::string here = path + "[" + std::to_string(i) + "]";
            switch (action.kind)
            {
            case rules::action_kind::ask:
                bad(here, "\"ask\" cannot be installed in this build");
                break;
            case rules::action_kind::raise_error:
                if (!takes_faults_)
                    bad(here + ".line_error",
                        "the port does not take line errors: only a fixture "
                        "and a port of the Windows driver do");
                break;
            case rules::action_kind::set_lines:
                for (const rules::line_value& line : action.lines)
                {
                    if ((line_mask(line.which) &
                         control_lines::input_bits()) == 0)
                        bad(here + ".lines",
                            "a rule sets the lines a device drives: CTS, "
                            "DSR, DCD and RI");
                }
                break;
            case rules::action_kind::reply:
            case rules::action_kind::pass:
            case rules::action_kind::set_state:
            case rules::action_kind::set_slot:
            case rules::action_kind::mark:
                break;
            }
        }
    }

    const rules::compiled_rule_set& set_;
    const bool takes_faults_;
    std::vector<rules::problem>& problems_;
};

} // namespace

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

    if (!set)
        return refuse(errc::invalid_argument);
    if (set->attach.as != rules::role::peer)
    {
        found.push_back(rules::problem{
            "attach.role",
            "only a \"peer\" set can be installed in this build"});
        return refuse(errc::unsupported);
    }
    const std::shared_ptr<endpoint> place =
        hub_->find_endpoint(set->attach.port);
    if (!place)
    {
        found.push_back(rules::problem{
            "attach.port", "there is no port named \"" + set->attach.port +
                               "\""});
        return refuse(errc::not_found);
    }
    const endpoint_id id = place->id();

    bool takes_faults = false;
    {
        std::lock_guard lock(record_mutex_);
        if (const auto it = records_.find(id); it != records_.end())
            takes_faults = !it->second.fixture.expired() ||
                           !it->second.driver.expired();
    }
    install_check(*set, takes_faults, found).run();
    if (!found.empty())
        return refuse(errc::unsupported);

    rules::scheduler_hooks hooks;
    hooks.now = [hub = hub_.get()] { return hub->now(); };
    hooks.poll_clock = static_cast<bool>(options_.hub.clock);
    hooks.act = [this, id, target = std::weak_ptr<endpoint>(place)](
                    const rules::planned_action& action)
    { carry_out_rule_action(id, target.lock(), action); };

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
        rules_.emplace(id, rules_record{made, installed.generation});

        // The set's run starts now.  It sees the chunks from the next one
        // the port reads: the stage is put on the endpoint's strand, between
        // two.  Still under the lock, so that a removal cannot come between
        // the record and the stage; neither call waits for anything.
        made->start(hub_->now());
        place->set_read_stage(made);
    }
    if (problems != nullptr)
        problems->clear();
    return installed;
}

result<void> studio::remove_rules(endpoint_id place)
{
    std::shared_ptr<rules::scheduler> removed;
    {
        std::lock_guard lock(rules_mutex_);
        const auto it = rules_.find(place);
        if (it == rules_.end())
            return make_error_code(errc::not_found);
        removed = std::move(it->second.scheduler);
        rules_.erase(it);
        rules_bytes_ -= removed->set().size_bytes;
        // Asked under the lock, so that the stage of a set installed right
        // after this one is not the one taken off.
        if (const auto endpoint = hub_->find_endpoint(place))
            endpoint->set_read_stage(nullptr);
    }
    // Nothing of the set is done after this returns.  The endpoint lets go
    // of the stage at its next turn; until then the stage ignores what it
    // is shown.
    removed->close();
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
    static_cast<void>(rules->resume(hub_->now()));
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
