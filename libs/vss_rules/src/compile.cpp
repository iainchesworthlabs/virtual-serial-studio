//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/rules/compile.hpp>

#include <algorithm>
#include <set>
#include <utility>

namespace vss::rules {

namespace {

/// Everything the checks share: the source, the set being built and the
/// list of what is wrong so far.
class compiler
{
public:
    compiler(const rule_set_source& source, std::vector<problem>& problems)
        : source_(source), problems_(problems)
    {
    }

    std::shared_ptr<const compiled_rule_set> run()
    {
        auto out = std::make_shared<compiled_rule_set>();
        out_ = out.get();
        header();
        for (std::size_t i = 0; i < source_.rules.size(); ++i)
            one_rule(source_.rules[i], "rules[" + std::to_string(i) + "]");
        for (std::size_t i = 0; i < source_.timeline.size(); ++i)
            one_timeline_entry(source_.timeline[i],
                               "timeline[" + std::to_string(i) + "]");
        if (out_->tight_actions > max_tight_actions)
            bad("", "more than " + std::to_string(max_tight_actions) +
                        " actions ask for \"tight\" precision");
        out_->size_bytes = size_of(*out_);
        if (out_->size_bytes > max_compiled_bytes)
            bad("", "the compiled set is over 1 MiB");
        return problems_.empty() ? out : nullptr;
    }

private:
    void bad(const std::string& path, std::string message)
    {
        problems_.push_back(problem{path, std::move(message)});
    }

    bool good_name(const std::string& name, const std::string& path,
                   const char* what)
    {
        if (name.empty())
        {
            bad(path, std::string(what) + " needs a name");
            return false;
        }
        if (name.size() > max_name_bytes)
        {
            bad(path, std::string(what) + " name is over " +
                          std::to_string(max_name_bytes) + " bytes");
            return false;
        }
        return true;
    }

    static std::size_t index_of(const std::vector<std::string>& names,
                                const std::string& name)
    {
        const auto found = std::find(names.begin(), names.end(), name);
        return found == names.end()
                   ? no_index
                   : static_cast<std::size_t>(found - names.begin());
    }

    // -- the header ----------------------------------------------------------

    void header()
    {
        out_->name = source_.name;
        out_->attach = source_.attach;
        out_->seed = source_.seed;
        out_->late_after = source_.late_after;
        out_->order = source_.order;

        if (source_.name.size() > max_name_bytes)
            bad("name", "the name is over " + std::to_string(max_name_bytes) +
                            " bytes");
        if (source_.attach.as == role::in_line)
        {
            if (source_.attach.from.empty() || source_.attach.to.empty())
                bad("attach",
                    "an inline set needs \"from\" and \"to\", the ports of "
                    "one direction of a link");
        }
        else if (source_.attach.port.empty())
            bad("attach", "the set needs the \"port\" it is attached to");
        if (source_.late_after.count() < 0 || source_.late_after > max_delay)
            bad("late_after_us", "must be between 0 and 60000000");

        if (source_.states.size() > max_states)
            bad("states", "more than " + std::to_string(max_states) +
                              " states");
        for (std::size_t i = 0; i < source_.states.size(); ++i)
        {
            const std::string path = "states[" + std::to_string(i) + "]";
            const std::string& name = source_.states[i];
            if (!good_name(name, path, "a state"))
                continue;
            if (index_of(out_->states, name) != no_index)
                bad(path, "the state \"" + name + "\" is listed twice");
            out_->states.push_back(name);
        }

        if (source_.slots.size() > max_slots)
            bad("slots", "more than " + std::to_string(max_slots) + " slots");
        for (const auto& [name, value] : source_.slots)
        {
            const std::string path = "slots." + name;
            if (!good_name(name, "slots", "a slot"))
                continue;
            if (index_of(out_->slot_names, name) != no_index)
                bad(path, "the slot is listed twice");
            if (value.size() > max_slot_bytes)
                bad(path, "a slot holds at most " +
                              std::to_string(max_slot_bytes) + " bytes");
            out_->slot_names.push_back(name);
            out_->slot_values.push_back(value);
        }

        if (source_.rules.size() > max_rules)
            bad("rules", "more than " + std::to_string(max_rules) + " rules");
        if (source_.timeline.size() > max_timeline)
            bad("timeline", "more than " + std::to_string(max_timeline) +
                                " entries");
        if (source_.rules.empty() && source_.timeline.empty())
            bad("", "the set has no \"rules\" and no \"timeline\"");
    }

    // -- one rule ------------------------------------------------------------

    void one_rule(const rule& in, const std::string& path)
    {
        compiled_rule out;
        out.name = in.name;
        if (good_name(in.name, path + ".name", "a rule") &&
            !rule_names_.insert(in.name).second)
            bad(path + ".name", "two rules are named \"" + in.name + "\"");

        const std::size_t pattern = one_trigger(in.when, path + ".when", out);

        if (in.sequence.empty())
            bad(path, "a rule needs a \"do\" list or a \"sequence\"");
        if (in.sequence.size() > max_sequence_steps)
            bad(path + ".sequence",
                "more than " + std::to_string(max_sequence_steps) + " steps");
        for (std::size_t i = 0; i < in.sequence.size(); ++i)
        {
            const std::string step_path =
                in.written_as_do
                    ? path
                    : path + ".sequence[" + std::to_string(i) + "]";
            const step& from = in.sequence[i];
            compiled_step to;
            to.chance_ppm = from.chance_ppm;
            if (from.chance_ppm > chance_always)
                bad(step_path + ".chance_ppm", "must be 0 to 1000000");
            to.actions = action_list(from.actions, step_path + ".do", pattern,
                                     false, true);
            out.sequence.push_back(std::move(to));
        }
        out.then = in.then;
        out.count = in.count;
        out_->rules.push_back(std::move(out));
    }

    /// Returns the length of the pattern, or zero for a rule with none.
    std::size_t one_trigger(const trigger& in, const std::string& path,
                            compiled_rule& out)
    {
        const int kinds = (in.bytes.empty() ? 0 : 1) +
                          (in.lines.empty() ? 0 : 1) + (in.port ? 1 : 0);
        if (kinds != 1)
            bad(path, "a rule matches exactly one of \"bytes\", \"lines\" "
                      "and \"port\"");

        if (!in.state.empty())
        {
            out.state = index_of(out_->states, in.state);
            if (out.state == no_index)
                bad(path + ".state",
                    "\"" + in.state + "\" is not in \"states\"");
        }

        if (!in.lines.empty())
        {
            out.kind = trigger_kind::lines;
            out.lines = in.lines;
            lines_once(in.lines, path + ".lines");
        }
        if (in.port)
        {
            out.kind = trigger_kind::port;
            out.port = *in.port;
        }
        if (in.bytes.empty())
        {
            if (in.gap.count() != 0 || !in.mask.empty())
                bad(path, "\"gap_us\" and \"mask\" need \"bytes\"");
            return 0;
        }

        out.kind = trigger_kind::bytes;
        if (in.bytes.size() > max_pattern_bytes)
        {
            bad(path + ".bytes", "a pattern has at most " +
                                     std::to_string(max_pattern_bytes) +
                                     " bytes");
            return 0;
        }
        if (!in.mask.empty() && in.mask.size() != in.bytes.size())
        {
            bad(path + ".mask",
                "\"mask\" must have as many bytes as \"bytes\"");
            return 0;
        }
        if (in.gap.count() < 0 || in.gap > max_delay)
            bad(path + ".gap_us", "must be between 0 and 60000000");
        out.gap = in.gap;
        out.where = in.where;
        if (in.gap.count() != 0 && in.where != gap_where::before)
            out_->longest_gap = std::max(out_->longest_gap, in.gap);

        out.length = in.bytes.size();
        out.words = (out.length + 63) / 64;
        out.accept.assign(256 * out.words, 0);
        for (std::size_t position = 0; position < out.length; ++position)
        {
            const byte mask = in.mask.empty() ? byte{0xff} : in.mask[position];
            const byte want = static_cast<byte>(in.bytes[position] & mask);
            for (std::size_t value = 0; value < 256; ++value)
            {
                if ((static_cast<byte>(value) & mask) == want)
                    out.accept[value * out.words + position / 64] |=
                        std::uint64_t{1} << (position % 64);
            }
        }
        out_->longest_pattern = std::max(out_->longest_pattern, out.length);
        return out.length;
    }

    void lines_once(const std::vector<line_value>& lines,
                    const std::string& path)
    {
        std::set<control_line> seen;
        for (const auto& entry : lines)
        {
            if (!seen.insert(entry.which).second)
                bad(path, "a line is named twice");
        }
    }

    // -- actions -------------------------------------------------------------

    /// `pattern` is the length of the rule's pattern, zero when the actions
    /// have no matched bytes to refer to.  `nested` is true inside an
    /// `on_timeout` list, and `timed` is false on the timeline, where the
    /// entry gives the time.
    std::vector<compiled_action> action_list(const std::vector<action>& in,
                                             const std::string& path,
                                             std::size_t pattern, bool nested,
                                             bool timed)
    {
        std::vector<compiled_action> out;
        if (in.size() > max_actions)
            bad(path, "more than " + std::to_string(max_actions) +
                          " actions in one list");
        for (std::size_t i = 0; i < in.size(); ++i)
            out.push_back(one_action(in[i],
                                     path + "[" + std::to_string(i) + "]",
                                     pattern, nested, timed));
        return out;
    }

    compiled_action one_action(const action& in, const std::string& path,
                               std::size_t pattern, bool nested, bool timed)
    {
        compiled_action out;
        out.kind = in.kind;
        out.when = in.when;
        out.fault = in.fault;
        out.lines = in.lines;
        out.error = in.error;
        out.first = in.first;
        out.last = in.last;
        out.text = in.text;
        out.timeout = in.timeout;

        const role as = source_.attach.as;
        const bool sends = as != role::in_line;
        switch (in.kind)
        {
        case action_kind::reply:
            if (!sends)
                bad(path, "an inline set cannot \"reply\"");
            out.parts = parts(in.parts, path + ".reply", pattern);
            one_fault(in.fault, path + ".fault");
            break;
        case action_kind::pass:
            if (sends)
                bad(path, "only an inline set passes, holds or changes the "
                          "bytes it matched");
            if (pattern == 0)
                bad(path, "this action needs a rule that matches \"bytes\"");
            one_fault(in.fault, path + ".fault");
            break;
        case action_kind::set_lines:
            if (!sends)
                bad(path, "an inline set cannot set \"lines\"");
            if (in.lines.empty())
                bad(path + ".lines", "no line is named");
            lines_once(in.lines, path + ".lines");
            break;
        case action_kind::raise_error:
            if (!sends)
                bad(path, "an inline set cannot raise a \"line_error\"");
            break;
        case action_kind::set_state:
            out.state = index_of(out_->states, in.state);
            if (out.state == no_index)
                bad(path + ".set_state",
                    "\"" + in.state + "\" is not in \"states\"");
            break;
        case action_kind::set_slot:
            out.slot = index_of(out_->slot_names, in.slot);
            if (out.slot == no_index)
                bad(path + ".set_slot",
                    "\"" + in.slot + "\" is not in \"slots\"");
            match_range(in.first, in.last, pattern, path + ".set_slot");
            break;
        case action_kind::mark:
            if (in.text.size() > max_text_bytes)
                bad(path + ".mark", "the text is over " +
                                        std::to_string(max_text_bytes) +
                                        " bytes");
            break;
        case action_kind::ask:
            if (nested)
                bad(path, "\"on_timeout\" cannot \"ask\" again");
            if (in.timeout < min_ask_timeout || in.timeout > max_delay)
                bad(path + ".timeout_us",
                    "must be between 2000 and 60000000: a plugin cannot be "
                    "asked for an answer in less than 2 ms");
            out.on_timeout = action_list(in.on_timeout, path + ".on_timeout",
                                         pattern, true, timed);
            break;
        }

        if (in.kind != action_kind::reply && in.kind != action_kind::pass &&
            in.fault.kind != fault_kind::none)
            bad(path + ".fault", "only a reply or passing bytes take a fault");

        const bool instant = in.kind == action_kind::set_state ||
                             in.kind == action_kind::set_slot;
        const bool plain = in.when.after == edge::match_end &&
                           in.when.delay.kind == delay_kind::fixed &&
                           in.when.delay.fixed.count() == 0;
        if (instant && (!plain || in.when.how != precision::normal))
            bad(path, "a state or a slot changes at the match and takes no "
                      "\"after\", \"delay\" or \"precision\"");
        else if (!timed && !plain)
            bad(path, "an action on the timeline happens at its entry's "
                      "\"at_us\" and takes no \"after\" or \"delay\"");
        else
            one_delay(in.when, path + ".delay");
        if (in.when.how == precision::tight)
            ++out_->tight_actions;
        return out;
    }

    void match_range(std::size_t first, std::size_t last, std::size_t pattern,
                     const std::string& path)
    {
        if (pattern == 0)
            bad(path, "bytes of the match need a rule that matches \"bytes\"");
        else if (first > last || last >= pattern)
            bad(path, "the range must lie inside the pattern's " +
                          std::to_string(pattern) + " bytes");
    }

    std::vector<compiled_part> parts(const std::vector<reply_part>& in,
                                     const std::string& path,
                                     std::size_t pattern)
    {
        std::vector<compiled_part> out;
        if (in.empty())
            bad(path, "a reply needs at least one part");
        if (in.size() > max_reply_parts)
            bad(path, "more than " + std::to_string(max_reply_parts) +
                          " parts");
        std::size_t fixed_bytes = 0;
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            const std::string part_path = path + "[" + std::to_string(i) + "]";
            compiled_part part{in[i], no_index};
            switch (in[i].kind)
            {
            case part_kind::literal:
                if (in[i].bytes.empty())
                    bad(part_path, "no bytes");
                fixed_bytes += in[i].bytes.size();
                break;
            case part_kind::slot:
                part.slot = index_of(out_->slot_names, in[i].slot);
                if (part.slot == no_index)
                    bad(part_path,
                        "\"" + in[i].slot + "\" is not in \"slots\"");
                break;
            case part_kind::match:
                match_range(in[i].first, in[i].last, pattern, part_path);
                break;
            case part_kind::check:
                if (!in[i].whole && in[i].first > in[i].last)
                    bad(part_path, "the range runs backwards");
                fixed_bytes += 2;
                break;
            }
            out.push_back(std::move(part));
        }
        if (fixed_bytes > max_reply_bytes)
            bad(path, "a reply has at most " +
                          std::to_string(max_reply_bytes) + " bytes");
        return out;
    }

    void one_fault(const fault_spec& in, const std::string& path)
    {
        if (in.kind != fault_kind::corrupt)
            return;
        if (in.xor_with.empty())
            bad(path + ".corrupt.xor", "no bytes");
        if (in.xor_with.size() > max_pattern_bytes)
            bad(path + ".corrupt.xor",
                "at most " + std::to_string(max_pattern_bytes) + " bytes");
    }

    void one_delay(const timing& when, const std::string& path)
    {
        const delay_spec& in = when.delay;
        const microseconds limit =
            when.after == edge::run_start ? max_run_time : max_delay;
        const auto within = [&](microseconds value)
        {
            return value.count() >= 0 && value <= limit;
        };
        const std::string range =
            "must be between 0 and " + std::to_string(limit.count());
        microseconds latest{0};

        switch (in.kind)
        {
        case delay_kind::fixed:
            if (!within(in.fixed))
                bad(path + ".fixed_us", range);
            latest = in.fixed;
            break;
        case delay_kind::uniform:
            if (!within(in.min) || !within(in.max) || in.min > in.max)
                bad(path + ".uniform",
                    "\"min_us\" and \"max_us\" " + range +
                        ", and \"min_us\" not above \"max_us\"");
            latest = in.max;
            break;
        case delay_kind::normal:
            if (!within(in.min) || !within(in.max) || in.min > in.max)
                bad(path + ".normal",
                    "\"min_us\" and \"max_us\" are required, " + range +
                        ", and \"min_us\" not above \"max_us\"");
            else if (in.mean < in.min || in.mean > in.max)
                bad(path + ".normal.mean_us",
                    "must lie between \"min_us\" and \"max_us\"");
            if (!within(in.sd))
                bad(path + ".normal.sd_us", range);
            latest = in.max;
            break;
        case delay_kind::exponential:
            if (!within(in.min) || !within(in.max) || in.min > in.max ||
                in.max.count() == 0)
                bad(path + ".exponential",
                    "\"max_us\" is required and " + range +
                        ", and \"min_us\" not above it");
            if (!within(in.mean) || in.mean.count() == 0)
                bad(path + ".exponential.mean_us",
                    "must be between 1 and " + std::to_string(limit.count()));
            latest = in.max;
            break;
        case delay_kind::table:
            if (in.values.empty() || in.values.size() > max_delay_table)
                bad(path + ".table", "needs 1 to " +
                                         std::to_string(max_delay_table) +
                                         " values");
            for (const microseconds value : in.values)
            {
                if (!within(value))
                {
                    bad(path + ".table", "every value " + range);
                    break;
                }
                latest = std::max(latest, value);
            }
            break;
        }
        if (when.after != edge::run_start)
            out_->latest_action = std::max(out_->latest_action, latest);
    }

    // -- the timeline --------------------------------------------------------

    void one_timeline_entry(const timeline_entry& in, const std::string& path)
    {
        compiled_timeline_entry out;
        out.at = in.at;
        if (in.at.count() < 0 || in.at > max_run_time)
            bad(path + ".at_us", "must be between 0 and " +
                                     std::to_string(max_run_time.count()));
        if (in.actions.empty())
            bad(path + ".do", "no action");
        out.actions = action_list(in.actions, path + ".do", 0, false, false);
        out_->timeline.push_back(std::move(out));
    }

    // -- the size ------------------------------------------------------------

    static std::size_t size_of(const std::vector<compiled_action>& actions)
    {
        std::size_t total = 0;
        for (const auto& item : actions)
        {
            total += sizeof(compiled_action) + item.text.size() +
                     item.fault.xor_with.size() +
                     item.lines.size() * sizeof(line_value) +
                     item.when.delay.values.size() * sizeof(microseconds);
            for (const auto& part : item.parts)
                total += sizeof(compiled_part) + part.part.bytes.size();
            total += size_of(item.on_timeout);
        }
        return total;
    }

    static std::size_t size_of(const compiled_rule_set& set)
    {
        std::size_t total = sizeof(compiled_rule_set);
        for (const auto& name : set.states)
            total += sizeof(std::string) + name.size();
        for (std::size_t i = 0; i < set.slot_names.size(); ++i)
            total += sizeof(std::string) + set.slot_names[i].size() +
                     max_slot_bytes;
        for (const auto& item : set.rules)
        {
            total += sizeof(compiled_rule) + item.name.size() +
                     item.accept.size() * sizeof(std::uint64_t);
            for (const auto& each : item.sequence)
                total += sizeof(compiled_step) + size_of(each.actions);
        }
        for (const auto& entry : set.timeline)
            total += sizeof(compiled_timeline_entry) + size_of(entry.actions);
        return total;
    }

    const rule_set_source& source_;
    std::vector<problem>& problems_;
    compiled_rule_set* out_ = nullptr;
    std::set<std::string> rule_names_;
};

} // namespace

result<std::shared_ptr<const compiled_rule_set>> compile(
    const rule_set_source& source, std::vector<problem>* problems)
{
    std::vector<problem> found;
    auto compiled = compiler(source, found).run();
    if (problems != nullptr)
        *problems = std::move(found);
    if (!compiled)
        return make_error_code(errc::config_error);
    return compiled;
}

} // namespace vss::rules
