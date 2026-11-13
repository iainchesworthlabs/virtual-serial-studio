//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/rules/machine.hpp>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <utility>

namespace vss::rules {

namespace {

time_point after(time_point from, microseconds delay)
{
    return from + std::chrono::duration_cast<clock_type::duration>(delay);
}

std::uint16_t crc16_modbus(bytes_view data)
{
    std::uint16_t crc = 0xffff;
    for (const byte value : data)
    {
        crc = static_cast<std::uint16_t>(crc ^ value);
        for (int bit = 0; bit < 8; ++bit)
        {
            const bool low = (crc & 1U) != 0;
            crc = static_cast<std::uint16_t>(crc >> 1U);
            if (low)
                crc = static_cast<std::uint16_t>(crc ^ 0xa001U);
        }
    }
    return crc;
}

std::uint16_t crc16_ccitt(bytes_view data)
{
    std::uint16_t crc = 0xffff;
    for (const byte value : data)
    {
        crc = static_cast<std::uint16_t>(crc ^ (value << 8U));
        for (int bit = 0; bit < 8; ++bit)
        {
            const bool high = (crc & 0x8000U) != 0;
            crc = static_cast<std::uint16_t>(crc << 1U);
            if (high)
                crc = static_cast<std::uint16_t>(crc ^ 0x1021U);
        }
    }
    return crc;
}

/// Applies a fault to bytes that are about to be sent.
void apply_fault(const fault_spec& fault, byte_buffer& bytes,
                 unsigned& copies)
{
    switch (fault.kind)
    {
    case fault_kind::none:
        break;
    case fault_kind::drop:
        copies = 0;
        break;
    case fault_kind::corrupt:
        for (std::size_t i = 0;
             i < fault.xor_with.size() && fault.at + i < bytes.size(); ++i)
            bytes[fault.at + i] =
                static_cast<byte>(bytes[fault.at + i] ^ fault.xor_with[i]);
        break;
    case fault_kind::truncate:
        if (fault.keep < bytes.size())
            bytes.resize(fault.keep);
        break;
    case fault_kind::duplicate:
        copies = 2;
        break;
    }
}

} // namespace

void append_check(check_kind kind, bytes_view over, byte_buffer& to)
{
    switch (kind)
    {
    case check_kind::crc16_modbus:
    {
        const std::uint16_t crc = crc16_modbus(over);
        to.push_back(static_cast<byte>(crc & 0xffU));
        to.push_back(static_cast<byte>(crc >> 8U));
        break;
    }
    case check_kind::crc16_ccitt:
    {
        const std::uint16_t crc = crc16_ccitt(over);
        to.push_back(static_cast<byte>(crc >> 8U));
        to.push_back(static_cast<byte>(crc & 0xffU));
        break;
    }
    case check_kind::sum8:
        to.push_back(static_cast<byte>(
            std::accumulate(over.begin(), over.end(), 0U) & 0xffU));
        break;
    case check_kind::xor8:
    {
        unsigned value = 0;
        for (const byte each : over)
            value ^= each;
        to.push_back(static_cast<byte>(value));
        break;
    }
    case check_kind::lrc:
    {
        const unsigned sum = std::accumulate(over.begin(), over.end(), 0U);
        to.push_back(static_cast<byte>((0U - sum) & 0xffU));
        break;
    }
    }
}

machine::machine(std::shared_ptr<const compiled_rule_set> set,
                 std::uint64_t run_seed)
    : set_(std::move(set))
    , seed_(set_->seed.value_or(run_seed))
    , slots_(set_->slot_values)
{
    rules_.reserve(set_->rules.size());
    std::size_t words = 0;
    for (const auto& each : set_->rules)
    {
        rule_state runtime;
        runtime.first_word = words;
        runtime.source =
            generator(derive_stream(seed_, set_->name, each.name));
        runtime.step_runs.assign(each.sequence.size(), 0);
        words += each.words;
        rules_.push_back(std::move(runtime));
    }
    shift_.assign(words, 0);
}

std::vector<firing> machine::start(time_point origin)
{
    origin_ = origin;
    std::vector<std::size_t> order(set_->timeline.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [this](std::size_t a, std::size_t b)
                     {
                         return set_->timeline[a].at < set_->timeline[b].at;
                     });

    std::vector<firing> out;
    out.reserve(order.size());
    for (const std::size_t index : order)
    {
        const auto& entry = set_->timeline[index];
        firing entry_firing;
        entry_firing.step = index;
        entry_firing.offset = seen_;
        entry_firing.start = after(origin, entry.at);
        entry_firing.end = entry_firing.start;
        plan(entry.actions, nullptr, 0, {}, entry_firing);
        out.push_back(std::move(entry_firing));
    }
    return out;
}

void machine::feed(bytes_view data, time_point when, std::vector<firing>& out)
{
    tick(when, out);
    if (data.empty())
        return;

    const std::size_t longest = set_->longest_pattern;
    boundaries_.push_back(boundary{
        seen_, when,
        any_seen_ ? when - last_arrival_ : clock_type::duration::max()});
    // A match that ends from here on starts no earlier than `longest` bytes
    // back, so older chunk boundaries are not needed.
    const std::uint64_t oldest = seen_ >= longest ? seen_ - longest : 0;
    std::size_t stale = 0;
    while (stale + 1 < boundaries_.size() &&
           boundaries_[stale + 1].offset <= oldest)
        ++stale;
    boundaries_.erase(boundaries_.begin(),
                      boundaries_.begin() +
                          static_cast<std::ptrdiff_t>(stale));

    for (const byte value : data)
    {
        const std::uint64_t offset = seen_++;
        ++stats_.bytes_seen;
        // A byte ends the silence a rule was waiting for.
        waiting_.reset();
        if (longest == 0)
            continue;

        recent_.push_back(value);
        if (recent_.size() > 2 * longest)
            recent_.erase(recent_.begin(),
                          recent_.end() -
                              static_cast<std::ptrdiff_t>(longest));

        // Every rule sees every byte, so that a rule's partial match does
        // not depend on what the rules before it did with the byte.
        std::size_t now = no_index;
        std::size_t later = no_index;
        for (std::size_t index = 0; index < rules_.size(); ++index)
        {
            const compiled_rule& rule = set_->rules[index];
            if (rule.kind != trigger_kind::bytes || !advance(index, value))
                continue;
            if (rules_[index].finished ||
                (rule.state != no_index && rule.state != state_))
                continue;
            const std::uint64_t first = offset + 1 - rule.length;
            const bool gapped = rule.gap.count() != 0;
            if (gapped && rule.where != gap_where::after &&
                !gap_before_holds(rule, first))
                continue;
            std::size_t& best =
                gapped && rule.where != gap_where::before ? later : now;
            if (best == no_index || rule.length > set_->rules[best].length)
                best = index;
        }

        const std::size_t chosen = now != no_index ? now : later;
        if (chosen == no_index)
            continue;
        const compiled_rule& rule = set_->rules[chosen];
        const std::uint64_t first = offset + 1 - rule.length;
        const bytes_view matched(recent_.data() + recent_.size() -
                                     rule.length,
                                 rule.length);
        if (now != no_index)
        {
            fire(chosen, first, matched, time_of(first), when, out);
            consume();
        }
        else
        {
            waiting_ = waiting{chosen,
                               first,
                               time_of(first),
                               when,
                               after(when, rule.gap),
                               byte_buffer(matched.begin(), matched.end())};
        }
    }

    last_arrival_ = when;
    any_seen_ = true;
}

void machine::lines_changed(control_lines before, control_lines after_change,
                            time_point when, std::vector<firing>& out)
{
    for (std::size_t index = 0; index < rules_.size(); ++index)
    {
        const compiled_rule& rule = set_->rules[index];
        if (rule.kind != trigger_kind::lines || rules_[index].finished ||
            (rule.state != no_index && rule.state != state_))
            continue;
        bool all_hold = true;
        bool any_changed = false;
        for (const auto& wanted : rule.lines)
        {
            all_hold = all_hold &&
                       after_change.test(wanted.which) == wanted.high;
            any_changed = any_changed || before.test(wanted.which) !=
                                             after_change.test(wanted.which);
        }
        if (all_hold && any_changed)
        {
            fire_event_rule(index, when, out);
            return;
        }
    }
}

void machine::port_changed(port_event what, time_point when,
                           std::vector<firing>& out)
{
    for (std::size_t index = 0; index < rules_.size(); ++index)
    {
        const compiled_rule& rule = set_->rules[index];
        if (rule.kind != trigger_kind::port || rule.port != what ||
            rules_[index].finished ||
            (rule.state != no_index && rule.state != state_))
            continue;
        fire_event_rule(index, when, out);
        return;
    }
}

void machine::tick(time_point now, std::vector<firing>& out)
{
    if (!waiting_ || waiting_->deadline > now)
        return;
    waiting ready = std::move(*waiting_);
    waiting_.reset();
    const compiled_rule& rule = set_->rules[ready.rule];
    // A line rule may have changed the state while this one waited.
    if (rules_[ready.rule].finished ||
        (rule.state != no_index && rule.state != state_))
        return;
    fire(ready.rule, ready.offset, ready.bytes, ready.start, ready.end, out);
    consume();
}

std::optional<time_point> machine::next_deadline() const
{
    if (!waiting_)
        return std::nullopt;
    return waiting_->deadline;
}

bytes_view machine::slot(std::size_t index) const
{
    if (index >= slots_.size())
        return {};
    return slots_[index];
}

result<void> machine::set_slot(std::size_t index, bytes_view value)
{
    if (index >= slots_.size())
        return make_error_code(errc::not_found);
    if (value.size() > max_slot_bytes)
        return make_error_code(errc::invalid_argument);
    slots_[index].assign(value.begin(), value.end());
    return {};
}

result<void> machine::set_state(std::size_t index)
{
    if (index >= set_->states.size())
        return make_error_code(errc::not_found);
    state_ = index;
    return {};
}

byte_buffer machine::unmatched_tail() const
{
    const std::size_t longest = set_->longest_pattern;
    if (longest < 2)
        return {};
    const std::size_t keep = std::min(recent_.size(), longest - 1);
    return byte_buffer(recent_.end() - static_cast<std::ptrdiff_t>(keep),
                       recent_.end());
}

bool machine::advance(std::size_t rule, byte value)
{
    const compiled_rule& compiled = set_->rules[rule];
    std::uint64_t* bits = shift_.data() + rules_[rule].first_word;
    const std::uint64_t* accept =
        compiled.accept.data() + std::size_t{value} * compiled.words;
    // Shift-And: bit i is set when the last i + 1 bytes are the first i + 1
    // of the pattern.
    std::uint64_t carry = 1;
    for (std::size_t word = 0; word < compiled.words; ++word)
    {
        const std::uint64_t shifted = (bits[word] << 1U) | carry;
        carry = bits[word] >> 63U;
        bits[word] = shifted & accept[word];
    }
    const std::size_t last = compiled.length - 1;
    return ((bits[last / 64] >> (last % 64)) & 1U) != 0;
}

bool machine::gap_before_holds(const compiled_rule& rule,
                               std::uint64_t first_offset) const
{
    // Silence is known only at the start of a chunk: bytes inside one
    // arrived together.
    for (auto it = boundaries_.rbegin(); it != boundaries_.rend(); ++it)
    {
        if (it->offset == first_offset)
            return it->quiet >=
                   std::chrono::duration_cast<clock_type::duration>(rule.gap);
        if (it->offset < first_offset)
            break;
    }
    return false;
}

time_point machine::time_of(std::uint64_t offset) const
{
    for (auto it = boundaries_.rbegin(); it != boundaries_.rend(); ++it)
    {
        if (it->offset <= offset)
            return it->when;
    }
    return boundaries_.empty() ? time_point{} : boundaries_.front().when;
}

void machine::consume()
{
    std::fill(shift_.begin(), shift_.end(), std::uint64_t{0});
    recent_.clear();
    waiting_.reset();
}

void machine::fire_event_rule(std::size_t rule, time_point when,
                              std::vector<firing>& out)
{
    fire(rule, seen_, {}, when, when, out);
}

void machine::fire(std::size_t rule, std::uint64_t offset, bytes_view matched,
                   time_point start, time_point end, std::vector<firing>& out)
{
    const compiled_rule& compiled = set_->rules[rule];
    rule_state& runtime = rules_[rule];

    const std::uint64_t position = runtime.matches++;
    const std::uint64_t steps = compiled.sequence.size();
    std::uint64_t step = position;
    if (compiled.then == after_sequence::repeat)
        step = position % steps;
    else if (compiled.then == after_sequence::hold)
        step = std::min(position, steps - 1);
    runtime.finished =
        (compiled.count != 0 && runtime.matches >= compiled.count) ||
        (compiled.then == after_sequence::stop && runtime.matches >= steps);

    firing fired;
    fired.rule = rule;
    fired.step = static_cast<std::size_t>(step);
    fired.ordinal = runtime.matches;
    fired.offset = offset;
    fired.length = matched.size();
    fired.start = start;
    fired.end = end;
    ++stats_.firings;
    stats_.bytes_matched += matched.size();

    const compiled_step& chosen = compiled.sequence[fired.step];
    if (chosen.chance_ppm < chance_always &&
        runtime.source.between(0, chance_always - 1) >= chosen.chance_ppm)
    {
        fired.skipped = true;
        ++stats_.skipped;
    }
    else
    {
        plan(chosen.actions, &runtime, runtime.step_runs[fired.step]++, matched,
             fired);
    }
    out.push_back(std::move(fired));
}

void machine::plan(const std::vector<compiled_action>& actions,
                   rule_state* runtime, std::uint64_t position,
                   bytes_view matched, firing& into)
{
    // The timeline has no rule and draws nothing: its actions take no delay.
    generator unused;
    generator& source = runtime != nullptr ? runtime->source : unused;
    time_point previous = into.end;

    for (std::size_t index = 0; index < actions.size(); ++index)
    {
        const compiled_action& action = actions[index];
        planned_action planned;
        planned.kind = action.kind;
        planned.index = index;
        planned.how = action.when.how;
        planned.source = &action;
        planned.due = into.end;

        if (action.kind == action_kind::set_state)
        {
            // At a match the state changes now.  On the timeline it changes
            // at the entry's time, which the engine does with set_state().
            if (runtime != nullptr)
                state_ = action.state;
        }
        else if (action.kind == action_kind::set_slot)
        {
            if (action.slot < slots_.size() && action.last < matched.size())
                slots_[action.slot].assign(
                    matched.begin() +
                        static_cast<std::ptrdiff_t>(action.first),
                    matched.begin() +
                        static_cast<std::ptrdiff_t>(action.last + 1));
        }
        else
        {
            planned.drawn = draw_delay(action.when.delay, source, position);
            time_point from = into.end;
            if (action.when.after == edge::match_start)
                from = into.start;
            else if (action.when.after == edge::previous)
                from = previous;
            else if (action.when.after == edge::run_start)
                from = origin_;
            planned.due = after(from, planned.drawn);
            // The timeline and absolute times are not held back by replies.
            if (runtime != nullptr && set_->order == ordering::fifo &&
                action.when.after != edge::run_start)
            {
                planned.due = std::max(planned.due, last_due_);
                last_due_ = planned.due;
            }
            previous = planned.due;
        }

        if (action.kind == action_kind::reply)
        {
            planned.bytes = render(action, matched, planned.cut);
            planned.fault = action.fault.kind;
            apply_fault(action.fault, planned.bytes, planned.copies);
            ++stats_.replies;
            stats_.reply_bytes += planned.bytes.size() * planned.copies;
            if (action.fault.kind != fault_kind::none)
                ++stats_.faults;
        }
        else if (action.kind == action_kind::pass)
        {
            planned.bytes.assign(matched.begin(), matched.end());
            planned.fault = action.fault.kind;
            apply_fault(action.fault, planned.bytes, planned.copies);
            if (action.fault.kind != fault_kind::none)
                ++stats_.faults;
        }
        into.actions.push_back(std::move(planned));
    }
}

byte_buffer machine::render(const compiled_action& action, bytes_view matched,
                            bool& cut) const
{
    byte_buffer out;
    for (const compiled_part& each : action.parts)
    {
        const reply_part& part = each.part;
        switch (part.kind)
        {
        case part_kind::literal:
            out.insert(out.end(), part.bytes.begin(), part.bytes.end());
            break;
        case part_kind::slot:
            if (each.slot < slots_.size())
                out.insert(out.end(), slots_[each.slot].begin(),
                           slots_[each.slot].end());
            break;
        case part_kind::match:
            if (part.last < matched.size() && part.first <= part.last)
                out.insert(out.end(),
                           matched.begin() +
                               static_cast<std::ptrdiff_t>(part.first),
                           matched.begin() +
                               static_cast<std::ptrdiff_t>(part.last + 1));
            break;
        case part_kind::check:
        {
            std::size_t first = 0;
            std::size_t count = out.size();
            if (!part.whole)
            {
                // `last` may be past the end, and as large as a size can be.
                first = std::min(part.first, out.size());
                const std::size_t end =
                    part.last < out.size() ? part.last + 1 : out.size();
                count = end > first ? end - first : 0;
            }
            // The check is worked out before it is appended: `out` may move.
            byte_buffer value;
            append_check(part.check, bytes_view(out.data() + first, count),
                         value);
            out.insert(out.end(), value.begin(), value.end());
            break;
        }
        }
    }
    if (out.size() > max_reply_bytes)
    {
        out.resize(max_reply_bytes);
        cut = true;
    }
    return out;
}

} // namespace vss::rules
