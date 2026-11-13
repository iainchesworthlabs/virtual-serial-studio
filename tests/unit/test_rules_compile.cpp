//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The checks of compile(), one case for each, on sets built as data so that a
// case says exactly what it gets wrong, and the compiled form: the bounds, the
// names turned into indices and the table of bytes each pattern accepts.
//

#include "test_framework.hpp"

#include <vss/rules/compile.hpp>
#include <vss/rules/model.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace vss;
using namespace vss::rules;

namespace
{

constexpr const char* first_action = "rules[0].sequence[0].do[0]";

byte_buffer buf(std::initializer_list<int> values)
{
    byte_buffer out;
    for (const int value : values)
        out.push_back(static_cast<byte>(value));
    return out;
}

std::string describe(const std::vector<problem>& problems)
{
    std::string out;
    for (const auto& each : problems)
        out += "\n  " + each.path + ": " + each.message;
    return out;
}

action mark(std::string text = "m")
{
    action out;
    out.kind = action_kind::mark;
    out.text = std::move(text);
    return out;
}

action reply_of(byte_buffer literal)
{
    action out;
    out.kind = action_kind::reply;
    reply_part part;
    part.bytes = std::move(literal);
    out.parts.push_back(std::move(part));
    return out;
}

action of_kind(action_kind kind)
{
    action out;
    out.kind = kind;
    return out;
}

rule rule_of(std::string name, byte_buffer pattern, std::vector<action> list)
{
    rule out;
    out.name = std::move(name);
    out.when.bytes = std::move(pattern);
    step one;
    one.actions = std::move(list);
    out.sequence.push_back(std::move(one));
    return out;
}

rule_set_source set_of(role as = role::peer)
{
    rule_set_source out;
    out.attach.as = as;
    if (as == role::in_line)
    {
        out.attach.from = "a";
        out.attach.to = "b";
    }
    else
    {
        out.attach.port = "p";
    }
    return out;
}

/// A set of one rule `r` that matches {1, 2, 3} and does `list`.
rule_set_source with_actions(std::vector<action> list, role as = role::peer)
{
    rule_set_source out = set_of(as);
    out.rules.push_back(rule_of("r", buf({1, 2, 3}), std::move(list)));
    return out;
}

rule_set_source with_action(action one, role as = role::peer)
{
    std::vector<action> list;
    list.push_back(std::move(one));
    return with_actions(std::move(list), as);
}

std::shared_ptr<const compiled_rule_set>
compile_ok(const rule_set_source& source)
{
    std::vector<problem> problems;
    const auto set = compile(source, &problems);
    if (!set)
        throw vss_test::failure("rejected:" + describe(problems));
    REQUIRE(problems.empty());
    return *set;
}

std::vector<problem> refuse(const rule_set_source& source)
{
    std::vector<problem> problems;
    const auto set = compile(source, &problems);
    REQUIRE(!set);
    REQUIRE(set.error() == make_error_code(errc::config_error));
    REQUIRE(!problems.empty());
    for (const auto& each : problems)
        REQUIRE(!each.message.empty());
    return problems;
}

int count_at(const std::vector<problem>& problems, const std::string& path)
{
    int found = 0;
    for (const auto& each : problems)
    {
        if (each.path == path)
            ++found;
    }
    return found;
}

/// The set is refused, and a problem is at `path`.
void require_problem(const rule_set_source& source, const std::string& path)
{
    const auto problems = refuse(source);
    if (count_at(problems, path) == 0)
        throw vss_test::failure("expected a problem at \"" + path +
                                "\", got:" + describe(problems));
}

/// The set is refused for that one reason alone.
void require_only(const rule_set_source& source, const std::string& path)
{
    const auto problems = refuse(source);
    if (problems.size() != 1 || problems[0].path != path)
        throw vss_test::failure("expected one problem at \"" + path +
                                "\", got:" + describe(problems));
}

std::string action_path(const std::string& suffix)
{
    return std::string(first_action) + suffix;
}

// The accept table says, for a position and a value, whether the position
// takes the value.
bool accepts(const compiled_rule& rule, std::size_t position, unsigned value)
{
    const std::uint64_t word = rule.accept[value * rule.words + position / 64];
    return ((word >> (position % 64)) & 1U) != 0;
}

} // namespace

// -- the compiled form -------------------------------------------------------

TEST_CASE("rules: compile accepts a valid set and resolves its names")
{
    rule_set_source source = set_of(role::station);
    source.name = "meter";
    source.seed = 99;
    source.late_after = microseconds(500);
    source.order = ordering::independent;
    source.states = {"idle", "busy"};
    source.slots.emplace_back("reading", buf({0x12, 0x34}));
    source.slots.emplace_back("empty", byte_buffer{});

    action state = of_kind(action_kind::set_state);
    state.state = "busy";
    action slot = of_kind(action_kind::set_slot);
    slot.slot = "empty";
    slot.first = 1;
    slot.last = 2;
    action reply;
    reply.kind = action_kind::reply;
    reply_part from_slot;
    from_slot.kind = part_kind::slot;
    from_slot.slot = "reading";
    reply.parts.push_back(from_slot);
    reply_part check;
    check.kind = part_kind::check;
    check.check = check_kind::lrc;
    reply.parts.push_back(check);
    source.rules.push_back(rule_of("a", buf({1, 2, 3}), {reply, state, slot}));
    source.rules.back().when.state = "idle";
    source.rules.back().then = after_sequence::hold;
    source.rules.back().count = 4;

    rule by_lines;
    by_lines.name = "b";
    by_lines.when.lines.push_back(line_value{control_line::dtr, true});
    step on_lines;
    on_lines.actions.push_back(mark("dtr"));
    on_lines.chance_ppm = 500'000;
    by_lines.sequence.push_back(on_lines);
    source.rules.push_back(by_lines);

    rule by_port;
    by_port.name = "c";
    by_port.when.port = port_event::closed;
    step on_port;
    on_port.actions.push_back(mark("closed"));
    by_port.sequence.push_back(on_port);
    source.rules.push_back(by_port);

    timeline_entry entry;
    entry.at = microseconds(1000);
    entry.actions.push_back(mark("tick"));
    source.timeline.push_back(entry);

    const auto set = compile_ok(source);
    REQUIRE_EQ(set->name, std::string("meter"));
    REQUIRE(set->attach.as == role::station);
    REQUIRE_EQ(set->attach.port, std::string("p"));
    REQUIRE(set->seed.has_value());
    REQUIRE_EQ(set->seed.value_or(0), std::uint64_t{99});
    REQUIRE(set->late_after == microseconds(500));
    REQUIRE(set->order == ordering::independent);
    REQUIRE_EQ(set->states.size(), std::size_t{2});
    REQUIRE_EQ(set->slot_names.size(), std::size_t{2});
    REQUIRE_EQ(set->slot_names[0], std::string("reading"));
    REQUIRE(set->slot_values[0] == buf({0x12, 0x34}));
    REQUIRE(set->slot_values[1].empty());

    REQUIRE_EQ(set->rules.size(), std::size_t{3});
    const compiled_rule& first = set->rules[0];
    REQUIRE(first.kind == trigger_kind::bytes);
    REQUIRE_EQ(first.length, std::size_t{3});
    REQUIRE_EQ(first.words, std::size_t{1});
    REQUIRE_EQ(first.accept.size(), std::size_t{256});
    REQUIRE_EQ(first.state, std::size_t{0});
    REQUIRE(first.then == after_sequence::hold);
    REQUIRE_EQ(first.count, std::uint64_t{4});
    REQUIRE_EQ(first.sequence.size(), std::size_t{1});
    const auto& actions = first.sequence[0].actions;
    REQUIRE_EQ(actions.size(), std::size_t{3});
    REQUIRE_EQ(actions[0].parts[0].slot, std::size_t{0});
    REQUIRE_EQ(actions[0].parts[1].slot, no_index);
    REQUIRE_EQ(actions[1].state, std::size_t{1});
    REQUIRE_EQ(actions[2].slot, std::size_t{1});

    REQUIRE(set->rules[1].kind == trigger_kind::lines);
    REQUIRE_EQ(set->rules[1].length, std::size_t{0});
    REQUIRE_EQ(set->rules[1].lines.size(), std::size_t{1});
    REQUIRE_EQ(set->rules[1].state, no_index);
    REQUIRE_EQ(set->rules[1].sequence[0].chance_ppm, std::uint32_t{500'000});
    REQUIRE(set->rules[2].kind == trigger_kind::port);
    REQUIRE(set->rules[2].port == port_event::closed);

    REQUIRE_EQ(set->timeline.size(), std::size_t{1});
    REQUIRE(set->timeline[0].at == microseconds(1000));
}

TEST_CASE("rules: compile gives the bounds the engine needs")
{
    rule_set_source source = set_of(role::peer);

    // A pattern of 5 bytes with a gap after it, a pattern of 70 bytes with a
    // gap before it only, and a pattern of 9 bytes with a gap on both sides.
    action fixed = mark();
    fixed.when.delay.fixed = microseconds(100);
    action uniform = mark();
    uniform.when.delay.kind = delay_kind::uniform;
    uniform.when.delay.min = microseconds(10);
    uniform.when.delay.max = microseconds(500);
    uniform.when.how = precision::tight;
    source.rules.push_back(
        rule_of("a", buf({1, 2, 3, 4, 5}), {fixed, uniform}));
    source.rules.back().when.gap = microseconds(3000);
    source.rules.back().when.where = gap_where::after;

    action table = mark();
    table.when.delay.kind = delay_kind::table;
    table.when.delay.values = {microseconds(50), microseconds(700),
                               microseconds(20)};
    table.when.how = precision::tight;
    source.rules.push_back(rule_of("b", byte_buffer(70, byte{0x55}), {table}));
    source.rules.back().when.gap = microseconds(9000);
    source.rules.back().when.where = gap_where::before;

    // Time from the start of the run does not count towards the latest.
    action absolute = mark();
    absolute.when.after = edge::run_start;
    absolute.when.delay.fixed = microseconds(5'000'000);
    source.rules.push_back(
        rule_of("c", buf({9, 9, 9, 9, 9, 9, 9, 9, 9}), {absolute}));
    source.rules.back().when.gap = microseconds(4000);
    source.rules.back().when.where = gap_where::both;

    action normal = mark();
    normal.when.delay.kind = delay_kind::normal;
    normal.when.delay.mean = microseconds(300);
    normal.when.delay.sd = microseconds(30);
    normal.when.delay.min = microseconds(100);
    normal.when.delay.max = microseconds(650);
    action exponential = mark();
    exponential.when.delay.kind = delay_kind::exponential;
    exponential.when.delay.mean = microseconds(100);
    exponential.when.delay.max = microseconds(699);
    source.rules.push_back(rule_of("d", buf({7}), {normal, exponential}));

    const auto set = compile_ok(source);
    REQUIRE_EQ(set->longest_pattern, std::size_t{70});
    REQUIRE(set->latest_action == microseconds(700));
    REQUIRE(set->longest_gap == microseconds(4000));
    REQUIRE_EQ(set->tight_actions, std::size_t{2});
    REQUIRE(set->size_bytes > 0);
    REQUIRE(set->size_bytes <= max_compiled_bytes);
}

TEST_CASE("rules: compile bounds of a set with nothing to bound are zero")
{
    rule_set_source source = set_of(role::peer);
    timeline_entry entry;
    entry.at = microseconds(5);
    entry.actions.push_back(mark());
    source.timeline.push_back(entry);
    const auto set = compile_ok(source);
    REQUIRE_EQ(set->longest_pattern, std::size_t{0});
    REQUIRE(set->latest_action == microseconds(0));
    REQUIRE(set->longest_gap == microseconds(0));
    REQUIRE_EQ(set->tight_actions, std::size_t{0});
    REQUIRE(set->size_bytes > 0);
}

TEST_CASE("rules: compile counts the size of the set, which grows with it")
{
    const auto small = compile_ok(with_action(mark()));
    rule_set_source bigger = with_action(mark());
    bigger.slots.emplace_back("s", buf({1}));
    bigger.rules.push_back(rule_of("second", byte_buffer(200, byte{3}),
                                   {mark("a long text to hold"), mark()}));
    const auto big = compile_ok(bigger);
    REQUIRE(big->size_bytes > small->size_bytes);
}

TEST_CASE("rules: compile builds a table of the bytes each position accepts")
{
    rule_set_source source = set_of(role::peer);
    rule one = rule_of("m", buf({0x12, 0x34, 0xab, 0x00, 0xff}), {mark()});
    one.when.mask = buf({0xff, 0xf0, 0x0f, 0x00, 0xff});
    source.rules.push_back(one);
    source.rules.push_back(rule_of("p", buf({0x41, 0x42}), {mark()}));
    const auto set = compile_ok(source);

    const compiled_rule& masked = set->rules[0];
    REQUIRE_EQ(masked.length, std::size_t{5});
    REQUIRE_EQ(masked.words, std::size_t{1});
    REQUIRE_EQ(masked.accept.size(), std::size_t{256});
    for (unsigned value = 0; value < 256; ++value)
    {
        REQUIRE_EQ(accepts(masked, 0, value), value == 0x12);
        REQUIRE_EQ(accepts(masked, 1, value), (value & 0xf0U) == 0x30);
        REQUIRE_EQ(accepts(masked, 2, value), (value & 0x0fU) == 0x0b);
        REQUIRE(accepts(masked, 3, value));
        REQUIRE_EQ(accepts(masked, 4, value), value == 0xff);
        // Positions past the pattern take nothing.
        REQUIRE(!accepts(masked, 5, value));
        REQUIRE(!accepts(masked, 63, value));
    }

    // With no mask, every bit counts.
    const compiled_rule& exact = set->rules[1];
    for (unsigned value = 0; value < 256; ++value)
    {
        REQUIRE_EQ(accepts(exact, 0, value), value == 0x41);
        REQUIRE_EQ(accepts(exact, 1, value), value == 0x42);
    }
}

TEST_CASE("rules: compile builds the table of a pattern longer than 64 bytes")
{
    rule_set_source source = set_of(role::peer);
    byte_buffer pattern;
    byte_buffer mask;
    for (std::size_t i = 0; i < 130; ++i)
    {
        pattern.push_back(static_cast<byte>(i));
        mask.push_back(i % 3 == 0 ? byte{0xf0} : byte{0xff});
    }
    rule one = rule_of("long", pattern, {mark()});
    one.when.mask = mask;
    source.rules.push_back(one);
    source.rules.push_back(
        rule_of("exact64", byte_buffer(64, byte{7}), {mark()}));
    source.rules.push_back(
        rule_of("exact65", byte_buffer(65, byte{8}), {mark()}));
    source.rules.push_back(
        rule_of("exact256", byte_buffer(256, byte{9}), {mark()}));
    const auto set = compile_ok(source);

    const compiled_rule& long_rule = set->rules[0];
    REQUIRE_EQ(long_rule.length, std::size_t{130});
    REQUIRE_EQ(long_rule.words, std::size_t{3});
    REQUIRE_EQ(long_rule.accept.size(), std::size_t{256} * 3);
    for (const std::size_t position :
         {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{62},
          std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{66},
          std::size_t{127}, std::size_t{128}, std::size_t{129}})
    {
        for (unsigned value = 0; value < 256; ++value)
        {
            const unsigned care = position % 3 == 0 ? 0xf0U : 0xffU;
            const unsigned want = static_cast<unsigned>(position) & care;
            REQUIRE_EQ(accepts(long_rule, position, value),
                       (value & care) == want);
        }
    }
    for (unsigned value = 0; value < 256; ++value)
        REQUIRE(!accepts(long_rule, 130, value));

    REQUIRE_EQ(set->rules[1].words, std::size_t{1});
    REQUIRE_EQ(set->rules[2].words, std::size_t{2});
    REQUIRE_EQ(set->rules[3].words, std::size_t{4});
    REQUIRE(accepts(set->rules[2], 64, 8));
    REQUIRE(!accepts(set->rules[2], 64, 7));
    REQUIRE(accepts(set->rules[3], 255, 9));
    REQUIRE_EQ(set->longest_pattern, std::size_t{256});
}

TEST_CASE("rules: compile takes the limits as they are, one over is refused")
{
    // A set at every limit compiles.
    rule_set_source source = set_of(role::peer);
    for (std::size_t i = 0; i < max_states; ++i)
        source.states.push_back("s" + std::to_string(i));
    for (std::size_t i = 0; i < max_slots; ++i)
        source.slots.emplace_back("t" + std::to_string(i), byte_buffer{});
    step full;
    full.actions.assign(max_actions, mark());
    rule at_limit = rule_of("only", buf({1}), {mark()});
    at_limit.sequence.assign(max_sequence_steps, full);
    source.rules.push_back(at_limit);
    compile_ok(source);
    source.slots.back().second.assign(max_slot_bytes, byte{1});
    compile_ok(source);

    rule_set_source many = set_of(role::peer);
    for (std::size_t i = 0; i < max_rules; ++i)
        many.rules.push_back(
            rule_of("r" + std::to_string(i), buf({1}), {mark()}));
    compile_ok(many);
    many.rules.push_back(rule_of("extra", buf({1}), {mark()}));
    require_problem(many, "rules");

    rule_set_source timed = set_of(role::peer);
    timeline_entry entry;
    entry.actions.push_back(mark());
    timed.timeline.assign(max_timeline, entry);
    compile_ok(timed);
    timed.timeline.push_back(entry);
    require_problem(timed, "timeline");
}

// -- one case for each check -------------------------------------------------

TEST_CASE("rules: compile refuses a set with no rules and no timeline")
{
    require_only(set_of(role::peer), "");
}

TEST_CASE("rules: compile refuses a long set name")
{
    rule_set_source source = with_action(mark());
    source.name = std::string(max_name_bytes, 'n');
    compile_ok(source);
    source.name.push_back('n');
    require_only(source, "name");
}

TEST_CASE("rules: compile refuses a set attached to nothing")
{
    rule_set_source inline_set = with_action(mark(), role::in_line);
    inline_set.rules[0].sequence[0].actions[0] = of_kind(action_kind::pass);
    compile_ok(inline_set);
    inline_set.attach.from.clear();
    require_only(inline_set, "attach");
    inline_set.attach.from = "a";
    inline_set.attach.to.clear();
    require_only(inline_set, "attach");

    rule_set_source peer = with_action(mark(), role::peer);
    peer.attach.port.clear();
    require_only(peer, "attach");
    rule_set_source station = with_action(mark(), role::station);
    station.attach.port.clear();
    require_only(station, "attach");
}

TEST_CASE("rules: compile refuses a late_after outside 0 to 60 s")
{
    rule_set_source source = with_action(mark());
    source.late_after = microseconds(60'000'000);
    compile_ok(source);
    source.late_after = microseconds(60'000'001);
    require_only(source, "late_after_us");
    source.late_after = microseconds(-1);
    require_only(source, "late_after_us");
}

TEST_CASE("rules: compile refuses bad states")
{
    rule_set_source source = with_action(mark());
    source.states = {"a", "b"};
    compile_ok(source);

    source.states = {"a", "a"};
    require_only(source, "states[1]");
    source.states = {"a", ""};
    require_only(source, "states[1]");
    source.states = {"a", std::string(max_name_bytes + 1, 'x')};
    require_only(source, "states[1]");

    source.states.clear();
    for (std::size_t i = 0; i < max_states + 1; ++i)
        source.states.push_back("s" + std::to_string(i));
    require_only(source, "states");
}

TEST_CASE("rules: compile refuses bad slots")
{
    rule_set_source source = with_action(mark());
    source.slots.emplace_back("a", buf({1}));
    source.slots.emplace_back("b", buf({2}));
    compile_ok(source);

    rule_set_source twice = source;
    twice.slots.emplace_back("a", buf({3}));
    require_only(twice, "slots.a");

    rule_set_source nameless = source;
    nameless.slots.emplace_back("", buf({3}));
    require_only(nameless, "slots");

    rule_set_source long_name = source;
    long_name.slots.emplace_back(std::string(max_name_bytes + 1, 'x'),
                                 buf({3}));
    require_only(long_name, "slots");

    rule_set_source big = source;
    big.slots[1].second.assign(max_slot_bytes + 1, byte{0});
    require_only(big, "slots.b");

    rule_set_source many = with_action(mark());
    for (std::size_t i = 0; i < max_slots + 1; ++i)
        many.slots.emplace_back("s" + std::to_string(i), byte_buffer{});
    require_only(many, "slots");
}

TEST_CASE("rules: compile refuses bad rule names")
{
    rule_set_source source = with_action(mark());
    source.rules.push_back(rule_of("second", buf({4}), {mark()}));
    compile_ok(source);

    rule_set_source twice = source;
    twice.rules[1].name = "r";
    require_only(twice, "rules[1].name");

    rule_set_source nameless = source;
    nameless.rules[1].name.clear();
    require_only(nameless, "rules[1].name");

    rule_set_source long_name = source;
    long_name.rules[1].name = std::string(max_name_bytes + 1, 'x');
    require_only(long_name, "rules[1].name");
}

TEST_CASE("rules: compile refuses a trigger that is not exactly one kind")
{
    rule_set_source none = with_action(mark());
    none.rules[0].when.bytes.clear();
    require_only(none, "rules[0].when");

    rule_set_source two = with_action(mark());
    two.rules[0].when.port = port_event::opened;
    require_only(two, "rules[0].when");

    rule_set_source lines_and_bytes = with_action(mark());
    lines_and_bytes.rules[0].when.lines.push_back(
        line_value{control_line::cts, true});
    require_only(lines_and_bytes, "rules[0].when");

    rule_set_source three = lines_and_bytes;
    three.rules[0].when.port = port_event::closed;
    require_only(three, "rules[0].when");
}

TEST_CASE("rules: compile refuses an unknown state of a trigger")
{
    rule_set_source source = with_action(mark());
    source.states = {"idle"};
    source.rules[0].when.state = "idle";
    compile_ok(source);
    source.rules[0].when.state = "gone";
    require_only(source, "rules[0].when.state");
}

TEST_CASE("rules: compile refuses a line named twice in a trigger")
{
    rule_set_source source = set_of(role::peer);
    rule by_lines;
    by_lines.name = "l";
    by_lines.when.lines = {line_value{control_line::dtr, true},
                           line_value{control_line::rts, false}};
    step one;
    one.actions.push_back(mark());
    by_lines.sequence.push_back(one);
    source.rules.push_back(by_lines);
    compile_ok(source);
    source.rules[0].when.lines.push_back(line_value{control_line::dtr, false});
    require_only(source, "rules[0].when.lines");
}

TEST_CASE("rules: compile refuses a gap or a mask with no pattern")
{
    rule_set_source source = set_of(role::peer);
    rule by_port;
    by_port.name = "p";
    by_port.when.port = port_event::opened;
    step one;
    one.actions.push_back(mark());
    by_port.sequence.push_back(one);
    source.rules.push_back(by_port);
    compile_ok(source);

    rule_set_source gapped = source;
    gapped.rules[0].when.gap = microseconds(10);
    require_only(gapped, "rules[0].when");

    rule_set_source masked = source;
    masked.rules[0].when.mask = buf({0xff});
    require_only(masked, "rules[0].when");
}

TEST_CASE("rules: compile refuses a pattern over 256 bytes")
{
    rule_set_source source = with_action(mark());
    source.rules[0].when.bytes.assign(max_pattern_bytes, byte{1});
    compile_ok(source);
    source.rules[0].when.bytes.push_back(1);
    require_only(source, "rules[0].when.bytes");
}

TEST_CASE("rules: compile refuses a mask of another length")
{
    rule_set_source source = with_action(mark());
    source.rules[0].when.mask = buf({0xff, 0xff, 0xff});
    compile_ok(source);
    source.rules[0].when.mask = buf({0xff, 0xff});
    require_only(source, "rules[0].when.mask");
    source.rules[0].when.mask = buf({0xff, 0xff, 0xff, 0xff});
    require_only(source, "rules[0].when.mask");
}

TEST_CASE("rules: compile refuses a gap outside 0 to 60 s")
{
    rule_set_source source = with_action(mark());
    source.rules[0].when.gap = microseconds(60'000'000);
    compile_ok(source);
    source.rules[0].when.gap = microseconds(60'000'001);
    require_only(source, "rules[0].when.gap_us");
    source.rules[0].when.gap = microseconds(-1);
    require_only(source, "rules[0].when.gap_us");
}

TEST_CASE("rules: compile refuses a rule with no step")
{
    rule_set_source source = with_action(mark());
    source.rules[0].sequence.clear();
    require_only(source, "rules[0]");
}

TEST_CASE("rules: compile refuses more than 64 steps")
{
    rule_set_source source = with_action(mark());
    // A copy: assign() must not be given an element of the vector it fills.
    const step one = source.rules[0].sequence[0];
    source.rules[0].sequence.assign(max_sequence_steps + 1, one);
    require_only(source, "rules[0].sequence");
}

TEST_CASE("rules: compile refuses a chance over a million parts")
{
    rule_set_source source = with_action(mark());
    source.rules[0].sequence[0].chance_ppm = chance_always;
    compile_ok(source);
    source.rules[0].sequence[0].chance_ppm = chance_always + 1;
    require_only(source, "rules[0].sequence[0].chance_ppm");
}

TEST_CASE("rules: compile refuses more than 16 actions in a list")
{
    rule_set_source source = with_action(mark());
    source.rules[0].sequence[0].actions.assign(max_actions + 1, mark());
    require_only(source, "rules[0].sequence[0].do");
}

TEST_CASE("rules: compile refuses an action the role does not have")
{
    // An inline set cannot reply, set lines or raise an error.
    require_problem(with_action(reply_of(buf({1})), role::in_line),
                    first_action);
    action lines = of_kind(action_kind::set_lines);
    lines.lines.push_back(line_value{control_line::dcd, true});
    require_problem(with_action(lines, role::in_line), first_action);
    require_problem(
        with_action(of_kind(action_kind::raise_error), role::in_line),
        first_action);

    // A peer and a station cannot pass.
    for (const role as : {role::peer, role::station})
        require_problem(with_action(of_kind(action_kind::pass), as),
                        first_action);

    // Both can mark, change state and ask.
    action state = of_kind(action_kind::set_state);
    state.state = "x";
    rule_set_source inline_set = with_action(mark(), role::in_line);
    inline_set.states = {"x"};
    inline_set.rules[0].sequence[0].actions.push_back(state);
    action ask = of_kind(action_kind::ask);
    ask.timeout = microseconds(5000);
    inline_set.rules[0].sequence[0].actions.push_back(ask);
    compile_ok(inline_set);
}

TEST_CASE("rules: compile refuses passing bytes with no pattern")
{
    rule_set_source source = set_of(role::in_line);
    rule by_port;
    by_port.name = "p";
    by_port.when.port = port_event::opened;
    step one;
    one.actions.push_back(of_kind(action_kind::pass));
    by_port.sequence.push_back(one);
    source.rules.push_back(by_port);
    require_only(source, first_action);

    // With a pattern it is accepted.
    compile_ok(with_action(of_kind(action_kind::pass), role::in_line));
}

TEST_CASE("rules: compile refuses set_lines with no line or a repeated one")
{
    action lines = of_kind(action_kind::set_lines);
    require_only(with_action(lines), action_path(".lines"));
    lines.lines = {line_value{control_line::dcd, true},
                   line_value{control_line::dsr, true}};
    compile_ok(with_action(lines));
    lines.lines.push_back(line_value{control_line::dcd, false});
    require_only(with_action(lines), action_path(".lines"));
}

TEST_CASE("rules: compile refuses set_state to a state that is not declared")
{
    action state = of_kind(action_kind::set_state);
    state.state = "busy";
    rule_set_source source = with_action(state);
    require_only(source, action_path(".set_state"));
    source.states = {"idle", "busy"};
    compile_ok(source);
}

TEST_CASE("rules: compile refuses set_slot to a slot that is not declared")
{
    action slot = of_kind(action_kind::set_slot);
    slot.slot = "reading";
    slot.first = 0;
    slot.last = 1;
    rule_set_source source = with_action(slot);
    require_only(source, action_path(".set_slot"));
    source.slots.emplace_back("reading", byte_buffer{});
    compile_ok(source);
}

TEST_CASE("rules: compile refuses a range outside the pattern")
{
    action slot = of_kind(action_kind::set_slot);
    slot.slot = "s";
    slot.first = 0;
    slot.last = 2;
    rule_set_source source = with_action(slot);
    source.slots.emplace_back("s", byte_buffer{});
    compile_ok(source);

    source.rules[0].sequence[0].actions[0].last = 3;
    require_only(source, action_path(".set_slot"));
    source.rules[0].sequence[0].actions[0].first = 2;
    source.rules[0].sequence[0].actions[0].last = 1;
    require_only(source, action_path(".set_slot"));

    // A rule with no bytes has nothing to copy from.
    rule_set_source no_bytes = set_of(role::peer);
    no_bytes.slots.emplace_back("s", byte_buffer{});
    rule by_port;
    by_port.name = "p";
    by_port.when.port = port_event::opened;
    step one;
    one.actions.push_back(slot);
    by_port.sequence.push_back(one);
    no_bytes.rules.push_back(by_port);
    require_only(no_bytes, action_path(".set_slot"));
}

TEST_CASE("rules: compile refuses a mark text over 256 bytes")
{
    compile_ok(with_action(mark(std::string(max_text_bytes, 't'))));
    require_only(with_action(mark(std::string(max_text_bytes + 1, 't'))),
                 action_path(".mark"));
}

TEST_CASE("rules: compile refuses an ask that cannot be answered in time")
{
    action ask = of_kind(action_kind::ask);
    ask.timeout = microseconds(min_ask_timeout);
    compile_ok(with_action(ask));
    ask.timeout = max_delay;
    compile_ok(with_action(ask));
    ask.timeout = microseconds(1999);
    require_only(with_action(ask), action_path(".timeout_us"));
    ask.timeout = microseconds(0);
    require_only(with_action(ask), action_path(".timeout_us"));
    ask.timeout = microseconds(60'000'001);
    require_only(with_action(ask), action_path(".timeout_us"));
}

TEST_CASE("rules: compile checks the actions inside on_timeout")
{
    action ask = of_kind(action_kind::ask);
    ask.timeout = microseconds(5000);
    ask.on_timeout.push_back(reply_of(buf({0xee})));
    ask.on_timeout.push_back(mark());
    compile_ok(with_action(ask));

    // An ask inside an ask is refused.
    action again = of_kind(action_kind::ask);
    again.timeout = microseconds(5000);
    action outer = ask;
    outer.on_timeout.push_back(again);
    require_only(with_action(outer), action_path(".on_timeout[2]"));

    // A fault in a reply of the list, and a bad delay, are found too.
    action bad_delay = mark();
    bad_delay.when.delay.fixed = microseconds(60'000'001);
    action broken = ask;
    broken.on_timeout.push_back(bad_delay);
    require_only(with_action(broken),
                 action_path(".on_timeout[2].delay.fixed_us"));

    // The list is limited like any other.
    action long_list = ask;
    long_list.on_timeout.assign(max_actions + 1, mark());
    require_only(with_action(long_list), action_path(".on_timeout"));
}

TEST_CASE("rules: compile refuses a fault on an action that takes none")
{
    action lines = of_kind(action_kind::set_lines);
    lines.lines.push_back(line_value{control_line::dcd, true});
    lines.fault.kind = fault_kind::drop;
    require_only(with_action(lines), action_path(".fault"));

    action text = mark();
    text.fault.kind = fault_kind::duplicate;
    require_only(with_action(text), action_path(".fault"));

    // A reply and a pass take one.
    action reply = reply_of(buf({1}));
    reply.fault.kind = fault_kind::drop;
    compile_ok(with_action(reply));
    action pass = of_kind(action_kind::pass);
    pass.fault.kind = fault_kind::truncate;
    compile_ok(with_action(pass, role::in_line));
}

TEST_CASE("rules: compile refuses a time on a state or a slot change")
{
    action state = of_kind(action_kind::set_state);
    state.state = "x";
    rule_set_source source = with_action(state);
    source.states = {"x"};
    compile_ok(source);

    const auto refused = [&](auto change)
    {
        action copy = state;
        change(copy);
        rule_set_source other = source;
        other.rules[0].sequence[0].actions[0] = copy;
        require_only(other, first_action);
    };
    refused([](action& each) { each.when.after = edge::match_start; });
    refused([](action& each) { each.when.after = edge::previous; });
    refused([](action& each) { each.when.after = edge::run_start; });
    refused([](action& each) { each.when.delay.fixed = microseconds(1); });
    refused([](action& each) { each.when.delay.kind = delay_kind::uniform; });
    refused([](action& each) { each.when.how = precision::tight; });

    action slot = of_kind(action_kind::set_slot);
    slot.slot = "s";
    slot.when.delay.fixed = microseconds(1);
    rule_set_source slots = with_action(slot);
    slots.slots.emplace_back("s", byte_buffer{});
    require_only(slots, first_action);
}

TEST_CASE("rules: compile refuses a time on an action of the timeline")
{
    rule_set_source source = set_of(role::peer);
    timeline_entry entry;
    entry.at = microseconds(1000);
    entry.actions.push_back(mark());
    source.timeline.push_back(entry);
    compile_ok(source);

    source.timeline[0].actions[0].when.delay.fixed = microseconds(1);
    require_only(source, "timeline[0].do[0]");
    source.timeline[0].actions[0].when.delay.fixed = microseconds(0);
    source.timeline[0].actions[0].when.after = edge::match_start;
    require_only(source, "timeline[0].do[0]");

    // Precision is not a time: tight is allowed on the timeline.
    source.timeline[0].actions[0].when.after = edge::match_end;
    source.timeline[0].actions[0].when.how = precision::tight;
    compile_ok(source);
}

TEST_CASE("rules: compile refuses more than 16 tight actions")
{
    action tight = mark();
    tight.when.how = precision::tight;
    rule_set_source source = with_action(tight);
    source.rules[0].sequence[0].actions.assign(max_tight_actions, tight);
    compile_ok(source);
    // One more, in another rule: over the limit of tight actions in the set.
    source.rules.push_back(rule_of("second", buf({4}), {tight}));
    require_only(source, "");
    REQUIRE_EQ(compile_ok(with_action(tight))->tight_actions, std::size_t{1});
}

TEST_CASE("rules: compile refuses a reply with no parts or too many")
{
    action reply = of_kind(action_kind::reply);
    require_only(with_action(reply), action_path(".reply"));

    reply.parts.assign(max_reply_parts, reply_part{});
    for (auto& part : reply.parts)
        part.bytes = buf({1});
    compile_ok(with_action(reply));
    reply.parts.push_back(reply.parts[0]);
    require_only(with_action(reply), action_path(".reply"));
}

TEST_CASE("rules: compile refuses an empty literal part")
{
    action reply = reply_of(buf({1}));
    reply_part empty;
    reply.parts.push_back(empty);
    require_only(with_action(reply), action_path(".reply[1]"));
}

TEST_CASE("rules: compile refuses a part from a slot that is not declared")
{
    action reply = of_kind(action_kind::reply);
    reply_part from_slot;
    from_slot.kind = part_kind::slot;
    from_slot.slot = "reading";
    reply.parts.push_back(from_slot);
    rule_set_source source = with_action(reply);
    require_only(source, action_path(".reply[0]"));
    source.slots.emplace_back("reading", byte_buffer{});
    compile_ok(source);
}

TEST_CASE("rules: compile refuses a part from outside the match")
{
    action reply = of_kind(action_kind::reply);
    reply_part from_match;
    from_match.kind = part_kind::match;
    from_match.first = 0;
    from_match.last = 2;
    reply.parts.push_back(from_match);
    compile_ok(with_action(reply));

    reply.parts[0].last = 3;
    require_only(with_action(reply), action_path(".reply[0]"));
    reply.parts[0].first = 2;
    reply.parts[0].last = 1;
    require_only(with_action(reply), action_path(".reply[0]"));

    // No bytes to take from.
    rule_set_source source = set_of(role::peer);
    rule by_port;
    by_port.name = "p";
    by_port.when.port = port_event::closed;
    step one;
    reply.parts[0].first = 0;
    reply.parts[0].last = 0;
    one.actions.push_back(reply);
    by_port.sequence.push_back(one);
    source.rules.push_back(by_port);
    require_only(source, action_path(".reply[0]"));
}

TEST_CASE("rules: compile refuses a check over a range that runs backwards")
{
    action reply = reply_of(buf({1, 2, 3}));
    reply_part check;
    check.kind = part_kind::check;
    check.whole = false;
    check.first = 0;
    check.last = 2;
    reply.parts.push_back(check);
    compile_ok(with_action(reply));
    // A range past the reply so far is fine: it is cut to what there is.
    reply.parts[1].last = 100;
    compile_ok(with_action(reply));
    reply.parts[1].first = 3;
    reply.parts[1].last = 2;
    require_only(with_action(reply), action_path(".reply[1]"));
}

TEST_CASE("rules: compile refuses a reply that must be over 4096 bytes")
{
    action reply = reply_of(byte_buffer(max_reply_bytes, byte{1}));
    compile_ok(with_action(reply));
    reply.parts[0].bytes.push_back(1);
    require_only(with_action(reply), action_path(".reply"));

    // Each check adds two bytes to what is counted.
    action checks = reply_of(byte_buffer(max_reply_bytes - 2, byte{1}));
    reply_part check;
    check.kind = part_kind::check;
    checks.parts.push_back(check);
    compile_ok(with_action(checks));
    checks.parts.push_back(check);
    require_only(with_action(checks), action_path(".reply"));
}

TEST_CASE("rules: compile refuses a corrupt fault with no bytes or too many")
{
    action reply = reply_of(buf({1, 2}));
    reply.fault.kind = fault_kind::corrupt;
    reply.fault.at = 1;
    require_only(with_action(reply), action_path(".fault.corrupt.xor"));
    reply.fault.xor_with.assign(max_pattern_bytes, byte{1});
    compile_ok(with_action(reply));
    reply.fault.xor_with.push_back(1);
    require_only(with_action(reply), action_path(".fault.corrupt.xor"));

    action pass = of_kind(action_kind::pass);
    pass.fault.kind = fault_kind::corrupt;
    require_only(with_action(pass, role::in_line),
                 action_path(".fault.corrupt.xor"));
}

TEST_CASE("rules: compile refuses a fixed delay outside 0 to 60 s")
{
    action wait = mark();
    wait.when.delay.fixed = microseconds(60'000'000);
    compile_ok(with_action(wait));
    wait.when.delay.fixed = microseconds(60'000'001);
    require_only(with_action(wait), action_path(".delay.fixed_us"));
    wait.when.delay.fixed = microseconds(-1);
    require_only(with_action(wait), action_path(".delay.fixed_us"));
}

TEST_CASE("rules: compile allows 24 hours on the edge of the run start only")
{
    action wait = mark();
    wait.when.after = edge::run_start;
    wait.when.delay.fixed = microseconds(86'400'000'000);
    compile_ok(with_action(wait));
    wait.when.delay.fixed = microseconds(86'400'000'001);
    require_only(with_action(wait), action_path(".delay.fixed_us"));

    action other = mark();
    other.when.after = edge::match_start;
    other.when.delay.fixed = microseconds(61'000'000);
    require_only(with_action(other), action_path(".delay.fixed_us"));
}

TEST_CASE("rules: compile refuses a bad uniform delay")
{
    action wait = mark();
    wait.when.delay.kind = delay_kind::uniform;
    wait.when.delay.min = microseconds(100);
    wait.when.delay.max = microseconds(100);
    compile_ok(with_action(wait));
    wait.when.delay.min = microseconds(101);
    require_only(with_action(wait), action_path(".delay.uniform"));
    wait.when.delay.min = microseconds(-1);
    require_only(with_action(wait), action_path(".delay.uniform"));
    wait.when.delay.min = microseconds(1);
    wait.when.delay.max = microseconds(60'000'001);
    require_only(with_action(wait), action_path(".delay.uniform"));
}

TEST_CASE("rules: compile refuses a bad normal delay")
{
    action wait = mark();
    wait.when.delay.kind = delay_kind::normal;
    wait.when.delay.mean = microseconds(500);
    wait.when.delay.sd = microseconds(50);
    wait.when.delay.min = microseconds(100);
    wait.when.delay.max = microseconds(900);
    compile_ok(with_action(wait));

    action backwards = wait;
    backwards.when.delay.min = microseconds(901);
    require_only(with_action(backwards), action_path(".delay.normal"));

    action beyond = wait;
    beyond.when.delay.max = microseconds(60'000'001);
    require_only(with_action(beyond), action_path(".delay.normal"));

    action mean_low = wait;
    mean_low.when.delay.mean = microseconds(99);
    require_only(with_action(mean_low), action_path(".delay.normal.mean_us"));
    action mean_high = wait;
    mean_high.when.delay.mean = microseconds(901);
    require_only(with_action(mean_high), action_path(".delay.normal.mean_us"));

    action spread = wait;
    spread.when.delay.sd = microseconds(60'000'001);
    require_only(with_action(spread), action_path(".delay.normal.sd_us"));
}

TEST_CASE("rules: compile refuses a bad exponential delay")
{
    action wait = mark();
    wait.when.delay.kind = delay_kind::exponential;
    wait.when.delay.mean = microseconds(100);
    wait.when.delay.max = microseconds(1000);
    compile_ok(with_action(wait));

    action no_max = wait;
    no_max.when.delay.max = microseconds(0);
    require_problem(with_action(no_max), action_path(".delay.exponential"));

    action backwards = wait;
    backwards.when.delay.min = microseconds(1001);
    require_only(with_action(backwards), action_path(".delay.exponential"));

    action no_mean = wait;
    no_mean.when.delay.mean = microseconds(0);
    require_only(with_action(no_mean),
                 action_path(".delay.exponential.mean_us"));
    action far_mean = wait;
    far_mean.when.delay.mean = microseconds(60'000'001);
    require_only(with_action(far_mean),
                 action_path(".delay.exponential.mean_us"));
}

TEST_CASE("rules: compile refuses a bad table delay")
{
    action wait = mark();
    wait.when.delay.kind = delay_kind::table;
    wait.when.delay.values = {microseconds(0), microseconds(60'000'000)};
    compile_ok(with_action(wait));

    action empty = wait;
    empty.when.delay.values.clear();
    require_only(with_action(empty), action_path(".delay.table"));

    action many = wait;
    many.when.delay.values.assign(max_delay_table, microseconds(5));
    compile_ok(with_action(many));
    many.when.delay.values.push_back(microseconds(5));
    require_only(with_action(many), action_path(".delay.table"));

    action far = wait;
    far.when.delay.values.push_back(microseconds(60'000'001));
    require_only(with_action(far), action_path(".delay.table"));
    action negative = wait;
    negative.when.delay.values.push_back(microseconds(-1));
    require_only(with_action(negative), action_path(".delay.table"));
}

TEST_CASE("rules: compile refuses an entry of the timeline out of range")
{
    rule_set_source source = set_of(role::peer);
    timeline_entry entry;
    entry.at = max_run_time;
    entry.actions.push_back(mark());
    source.timeline.push_back(entry);
    compile_ok(source);
    source.timeline[0].at = microseconds(max_run_time.count() + 1);
    require_only(source, "timeline[0].at_us");
    source.timeline[0].at = microseconds(-1);
    require_only(source, "timeline[0].at_us");
}

TEST_CASE("rules: compile refuses an entry of the timeline with no action")
{
    rule_set_source source = set_of(role::peer);
    source.timeline.push_back(timeline_entry{});
    require_only(source, "timeline[0].do");

    timeline_entry many;
    many.actions.assign(max_actions + 1, mark());
    source.timeline[0] = many;
    require_only(source, "timeline[0].do");
}

TEST_CASE("rules: compile refuses an action of the timeline that needs a match")
{
    rule_set_source source = set_of(role::peer);
    timeline_entry entry;
    action slot = of_kind(action_kind::set_slot);
    slot.slot = "s";
    entry.actions.push_back(slot);
    source.slots.emplace_back("s", byte_buffer{});
    source.timeline.push_back(entry);
    require_only(source, "timeline[0].do[0].set_slot");

    action from_match = of_kind(action_kind::reply);
    reply_part part;
    part.kind = part_kind::match;
    from_match.parts.push_back(part);
    source.timeline[0].actions[0] = from_match;
    require_only(source, "timeline[0].do[0].reply[0]");
}

TEST_CASE("rules: compile refuses a set that would be over 1 MiB")
{
    // 256 patterns of 256 bytes: a table of 8 KiB for each.
    rule_set_source source = set_of(role::peer);
    for (std::size_t i = 0; i < max_rules; ++i)
        source.rules.push_back(rule_of("r" + std::to_string(i),
                                       byte_buffer(max_pattern_bytes, byte{1}),
                                       {mark()}));
    require_only(source, "");

    // Half as many fit.
    source.rules.resize(100);
    const auto set = compile_ok(source);
    REQUIRE(set->size_bytes > max_compiled_bytes / 4);
    REQUIRE(set->size_bytes <= max_compiled_bytes);
}

TEST_CASE("rules: compile reports every problem, not the first only")
{
    rule_set_source source = set_of(role::peer);
    source.name = std::string(max_name_bytes + 1, 'n');
    source.late_after = microseconds(-5);
    source.states = {"a", "a"};
    source.rules.push_back(
        rule_of("x", buf({1}), {mark(std::string(300, 't'))}));
    source.rules.push_back(
        rule_of("x", buf({2}), {of_kind(action_kind::pass)}));
    action wait = mark();
    wait.when.delay.fixed = microseconds(-1);
    source.rules.push_back(rule_of("z", buf({3}), {wait}));
    source.rules[2].when.state = "nowhere";

    const auto problems = refuse(source);
    REQUIRE_EQ(count_at(problems, "name"), 1);
    REQUIRE_EQ(count_at(problems, "late_after_us"), 1);
    REQUIRE_EQ(count_at(problems, "states[1]"), 1);
    REQUIRE_EQ(count_at(problems, "rules[0].sequence[0].do[0].mark"), 1);
    REQUIRE_EQ(count_at(problems, "rules[1].name"), 1);
    REQUIRE_EQ(count_at(problems, "rules[1].sequence[0].do[0]"), 1);
    REQUIRE_EQ(count_at(problems, "rules[2].when.state"), 1);
    REQUIRE_EQ(count_at(problems, "rules[2].sequence[0].do[0].delay.fixed_us"),
               1);
    REQUIRE_EQ(problems.size(), std::size_t{8});
}

TEST_CASE("rules: compile works with no list to fill")
{
    REQUIRE(compile(with_action(mark()), nullptr));
    const auto refused = compile(set_of(role::peer), nullptr);
    REQUIRE(!refused);
    REQUIRE(refused.error() == make_error_code(errc::config_error));
}

TEST_CASE("rules: compile clears an old list when it succeeds")
{
    std::vector<problem> problems;
    problems.push_back(problem{"old", "old"});
    REQUIRE(compile(with_action(mark()), &problems));
    REQUIRE(problems.empty());
}
