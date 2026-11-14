//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reader of the rule set document: each key lands in its field, every
// refusal names the path of the value, and several problems come back
// together.  Whether a set makes sense is compile()'s business and is tested
// in test_rules_compile.cpp.
//

#include "test_framework.hpp"

#include <vss/rules/compile.hpp>
#include <vss/rules/model.hpp>
#include <vss/rules/parse.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace vss;
using namespace vss::rules;

namespace
{

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

rule_set_source parse_ok(std::string_view text)
{
    std::vector<problem> problems;
    auto parsed = parse_rule_set(text, &problems);
    if (!parsed)
        throw vss_test::failure("rejected:" + describe(problems));
    REQUIRE(problems.empty());
    return *parsed;
}

std::vector<problem> parse_bad(std::string_view text)
{
    std::vector<problem> problems;
    const auto parsed = parse_rule_set(text, &problems);
    REQUIRE(!parsed);
    REQUIRE(parsed.error() == make_error_code(errc::config_error));
    REQUIRE(!problems.empty());
    return problems;
}

bool has_path(const std::vector<problem>& problems, std::string_view path)
{
    for (const auto& each : problems)
    {
        if (each.path == path)
            return true;
    }
    return false;
}

/// A refusal that names exactly `path`, once.
void require_refused(std::string_view text, std::string_view path)
{
    const auto problems = parse_bad(text);
    int found = 0;
    for (const auto& each : problems)
    {
        if (each.path == path)
            ++found;
    }
    if (found != 1)
        throw vss_test::failure("expected one problem at " + std::string(path) +
                                ", got:" + describe(problems));
    REQUIRE_EQ(problems.size(), std::size_t{1});
}

const char* const head = R"j({"attach": {"port": "p", "role": "peer"}, )j";

/// A document of one rule given as JSON.
std::string with_rule(std::string_view rule_json)
{
    return std::string(head) + "\"rules\": [" + std::string(rule_json) + "]}";
}

/// A document of one rule with a pattern and the given `do` list.
std::string with_actions(std::string_view actions_json)
{
    return with_rule(R"j({"name": "r", "when": {"bytes": "01"}, "do": [)j" +
                     std::string(actions_json) + "]}");
}

/// The first action of the first rule of a document of one action.
action action_of(std::string_view action_json)
{
    const rule_set_source source = parse_ok(with_actions(action_json));
    REQUIRE_EQ(source.rules.size(), std::size_t{1});
    REQUIRE_EQ(source.rules[0].sequence.size(), std::size_t{1});
    REQUIRE_EQ(source.rules[0].sequence[0].actions.size(), std::size_t{1});
    return source.rules[0].sequence[0].actions[0];
}

trigger trigger_of(std::string_view when_json)
{
    const rule_set_source source =
        parse_ok(with_rule(R"j({"name": "r", "when": )j" +
                           std::string(when_json) + R"j(, "do": []})j"));
    return source.rules[0].when;
}

/// Refusal of one action: the path under the first action of the first rule.
void require_action_refused(std::string_view action_json, std::string_view path)
{
    require_refused(with_actions(action_json),
                    "rules[0].do[0]" + std::string(path));
}

std::string repeat_items(std::string_view item, std::size_t count)
{
    std::string out;
    for (std::size_t i = 0; i < count; ++i)
        out += (i == 0 ? "" : ",") + std::string(item);
    return out;
}

// The example of docs/response-timing.md, as it is written there.
const char* const example = R"j({
  "version": 1,
  "name": "meter 17",
  "attach": { "port": "meter-17", "role": "station" },
  "seed": 7,
  "late_after_us": 500,
  "slots": { "reading": "00 00 12 34" },
  "states": ["normal", "busy"],
  "rules": [
    {
      "name": "read holding registers",
      "when": { "bytes": "11 03 ?? ?? 00 02 ?? ??", "state": "normal" },
      "sequence": [
        { "do": [
            { "reply": ["11 03 04", { "slot": "reading" },
                        { "crc16_modbus": "reply" }],
              "after": "match_end",
              "delay": { "normal": { "mean_us": 3500, "sd_us": 400,
                                     "min_us": 1750, "max_us": 8000 } } } ] },
        { "do": [
            { "reply": ["11 83 06", { "crc16_modbus": "reply" }],
              "delay": { "fixed_us": 2000 },
              "fault": { "corrupt": { "at": 1, "xor": "01" } } },
            { "set_state": "busy" } ] }
      ],
      "then": "repeat",
      "count": 0
    },
    {
      "name": "busy until DTR drops",
      "when": { "lines": { "dtr": false }, "state": "busy" },
      "do": [ { "set_state": "normal" } ]
    }
  ],
  "timeline": [
    { "at_us": 5000000, "do": [ { "lines": { "dcd": false } } ] }
  ]
}
)j";

} // namespace

TEST_CASE("rules: parse reads the example of the design document")
{
    const rule_set_source source = parse_ok(example);
    REQUIRE_EQ(source.name, std::string("meter 17"));
    REQUIRE(source.attach.as == role::station);
    REQUIRE_EQ(source.attach.port, std::string("meter-17"));
    REQUIRE(source.seed.has_value());
    REQUIRE_EQ(*source.seed, std::uint64_t{7});
    REQUIRE(source.late_after == microseconds(500));
    REQUIRE(source.order == ordering::fifo);
    REQUIRE_EQ(source.slots.size(), std::size_t{1});
    REQUIRE_EQ(source.slots[0].first, std::string("reading"));
    REQUIRE(source.slots[0].second == buf({0x00, 0x00, 0x12, 0x34}));
    REQUIRE_EQ(source.states.size(), std::size_t{2});
    REQUIRE_EQ(source.states[0], std::string("normal"));
    REQUIRE_EQ(source.states[1], std::string("busy"));

    REQUIRE_EQ(source.rules.size(), std::size_t{2});
    const rule& read = source.rules[0];
    REQUIRE_EQ(read.name, std::string("read holding registers"));
    REQUIRE(read.when.bytes == buf({0x11, 0x03, 0, 0, 0x00, 0x02, 0, 0}));
    REQUIRE(read.when.mask ==
            buf({0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00}));
    REQUIRE_EQ(read.when.state, std::string("normal"));
    REQUIRE(read.then == after_sequence::repeat);
    REQUIRE_EQ(read.count, std::uint64_t{0});
    REQUIRE_EQ(read.sequence.size(), std::size_t{2});

    const auto& first = read.sequence[0].actions;
    REQUIRE_EQ(first.size(), std::size_t{1});
    REQUIRE(first[0].kind == action_kind::reply);
    REQUIRE_EQ(first[0].parts.size(), std::size_t{3});
    REQUIRE(first[0].parts[0].kind == part_kind::literal);
    REQUIRE(first[0].parts[0].bytes == buf({0x11, 0x03, 0x04}));
    REQUIRE(first[0].parts[1].kind == part_kind::slot);
    REQUIRE_EQ(first[0].parts[1].slot, std::string("reading"));
    REQUIRE(first[0].parts[2].kind == part_kind::check);
    REQUIRE(first[0].parts[2].check == check_kind::crc16_modbus);
    REQUIRE(first[0].parts[2].whole);
    REQUIRE(first[0].when.after == edge::match_end);
    REQUIRE(first[0].when.delay.kind == delay_kind::normal);
    REQUIRE(first[0].when.delay.mean == microseconds(3500));
    REQUIRE(first[0].when.delay.sd == microseconds(400));
    REQUIRE(first[0].when.delay.min == microseconds(1750));
    REQUIRE(first[0].when.delay.max == microseconds(8000));

    const auto& second = read.sequence[1].actions;
    REQUIRE_EQ(second.size(), std::size_t{2});
    REQUIRE(second[0].kind == action_kind::reply);
    REQUIRE(second[0].when.delay.kind == delay_kind::fixed);
    REQUIRE(second[0].when.delay.fixed == microseconds(2000));
    REQUIRE(second[0].fault.kind == fault_kind::corrupt);
    REQUIRE_EQ(second[0].fault.at, std::size_t{1});
    REQUIRE(second[0].fault.xor_with == buf({0x01}));
    REQUIRE(second[1].kind == action_kind::set_state);
    REQUIRE_EQ(second[1].state, std::string("busy"));

    const rule& busy = source.rules[1];
    REQUIRE_EQ(busy.when.lines.size(), std::size_t{1});
    REQUIRE(busy.when.lines[0].which == control_line::dtr);
    REQUIRE(!busy.when.lines[0].high);
    REQUIRE_EQ(busy.when.state, std::string("busy"));
    REQUIRE(busy.when.bytes.empty());
    REQUIRE_EQ(busy.sequence.size(), std::size_t{1});
    REQUIRE_EQ(busy.sequence[0].actions.size(), std::size_t{1});
    REQUIRE(busy.sequence[0].actions[0].kind == action_kind::set_state);
    REQUIRE_EQ(busy.sequence[0].actions[0].state, std::string("normal"));

    REQUIRE_EQ(source.timeline.size(), std::size_t{1});
    REQUIRE(source.timeline[0].at == microseconds(5'000'000));
    REQUIRE_EQ(source.timeline[0].actions.size(), std::size_t{1});
    REQUIRE(source.timeline[0].actions[0].kind == action_kind::set_lines);
    REQUIRE_EQ(source.timeline[0].actions[0].lines.size(), std::size_t{1});
    REQUIRE(source.timeline[0].actions[0].lines[0].which == control_line::dcd);
    REQUIRE(!source.timeline[0].actions[0].lines[0].high);
}

TEST_CASE("rules: the example of the design document compiles")
{
    std::vector<problem> problems;
    const auto set = compile_rule_set(example, &problems);
    if (!set)
        throw vss_test::failure("rejected:" + describe(problems));
    REQUIRE_EQ((*set)->rules.size(), std::size_t{2});
    REQUIRE_EQ((*set)->longest_pattern, std::size_t{8});
    REQUIRE(problems.empty());
}

TEST_CASE("rules: parse reads the header")
{
    const rule_set_source minimal =
        parse_ok(R"j({"attach": {"port": "p", "role": "peer"}})j");
    REQUIRE(minimal.attach.as == role::peer);
    REQUIRE_EQ(minimal.attach.port, std::string("p"));
    REQUIRE(minimal.name.empty());
    REQUIRE(!minimal.seed.has_value());
    REQUIRE(minimal.late_after == microseconds(0));
    REQUIRE(minimal.order == ordering::fifo);
    REQUIRE(minimal.states.empty());
    REQUIRE(minimal.slots.empty());
    REQUIRE(minimal.rules.empty());
    REQUIRE(minimal.timeline.empty());

    const rule_set_source inline_set = parse_ok(
        R"j({"version": 1, "attach": {"role": "inline", "from": "a", "to": "b"},
             "order": "independent", "late_after_us": 86400000000})j");
    REQUIRE(inline_set.attach.as == role::in_line);
    REQUIRE_EQ(inline_set.attach.from, std::string("a"));
    REQUIRE_EQ(inline_set.attach.to, std::string("b"));
    REQUIRE(inline_set.attach.port.empty());
    REQUIRE(inline_set.order == ordering::independent);
    REQUIRE(inline_set.late_after == microseconds(86'400'000'000));

    const rule_set_source peer = parse_ok(
        R"j({"attach": {"role": "peer", "port": "COM9"}, "seed": 0})j");
    REQUIRE(peer.seed.has_value());
    REQUIRE_EQ(*peer.seed, std::uint64_t{0});
}

TEST_CASE("rules: parse takes a seed signed or unsigned")
{
    const auto seed_of = [](std::string_view number)
    {
        const rule_set_source source =
            parse_ok(R"j({"attach": {"port": "p", "role": "peer"}, "seed": )j" +
                     std::string(number) + "}");
        return source.seed;
    };
    REQUIRE(seed_of("42") == std::uint64_t{42});
    REQUIRE(seed_of("-1") == std::uint64_t{0xffffffffffffffffULL});
    REQUIRE(seed_of("18446744073709551615") ==
            std::uint64_t{0xffffffffffffffffULL});
    REQUIRE(seed_of("9223372036854775808") ==
            std::uint64_t{0x8000000000000000ULL});
    REQUIRE(seed_of("-9223372036854775808") ==
            std::uint64_t{0x8000000000000000ULL});
}

TEST_CASE("rules: parse keeps the order of states and slots, and empty slots")
{
    const rule_set_source source = parse_ok(
        R"j({"attach": {"port": "p", "role": "peer"},
             "states": ["zeta", "alpha", "mid"],
             "slots": {"z": "01", "a": "", "m": "0a 0b 0c"}})j");
    REQUIRE_EQ(source.states.size(), std::size_t{3});
    REQUIRE_EQ(source.states[0], std::string("zeta"));
    REQUIRE_EQ(source.states[1], std::string("alpha"));
    REQUIRE_EQ(source.states[2], std::string("mid"));
    REQUIRE_EQ(source.slots.size(), std::size_t{3});
    REQUIRE_EQ(source.slots[0].first, std::string("z"));
    REQUIRE(source.slots[0].second == buf({0x01}));
    REQUIRE_EQ(source.slots[1].first, std::string("a"));
    REQUIRE(source.slots[1].second.empty());
    REQUIRE_EQ(source.slots[2].first, std::string("m"));
    REQUIRE(source.slots[2].second == buf({0x0a, 0x0b, 0x0c}));
}

TEST_CASE("rules: parse reads patterns, wildcards and masks")
{
    const trigger plain = trigger_of(R"j({"bytes": "11 03 ff"})j");
    REQUIRE(plain.bytes == buf({0x11, 0x03, 0xff}));
    REQUIRE(plain.mask.empty());

    // "??" is any byte, one "?" a nibble that is ignored.
    const trigger wild = trigger_of(R"j({"bytes": "11 03 ?? 1? ?3"})j");
    REQUIRE(wild.bytes == buf({0x11, 0x03, 0x00, 0x10, 0x03}));
    REQUIRE(wild.mask == buf({0xff, 0xff, 0x00, 0xf0, 0x0f}));

    const trigger lower = trigger_of(R"j({"bytes": "aB ?c D?"})j");
    REQUIRE(lower.bytes == buf({0xab, 0x0c, 0xd0}));
    REQUIRE(lower.mask == buf({0xff, 0x0f, 0xf0}));

    const trigger masked =
        trigger_of(R"j({"bytes": "12 34", "mask": "ff f0"})j");
    REQUIRE(masked.bytes == buf({0x12, 0x34}));
    REQUIRE(masked.mask == buf({0xff, 0xf0}));

    // Plain hex goes through the common reader: other separators work too.
    const trigger joined = trigger_of(R"j({"bytes": "0x1103"})j");
    REQUIRE(joined.bytes == buf({0x11, 0x03}));

    const trigger single = trigger_of(R"j({"bytes": "??"})j");
    REQUIRE(single.bytes == buf({0x00}));
    REQUIRE(single.mask == buf({0x00}));
}

TEST_CASE("rules: parse reads gaps, lines, port and state of a trigger")
{
    const trigger after = trigger_of(R"j({"bytes": "01", "gap_us": 3500})j");
    REQUIRE(after.gap == microseconds(3500));
    REQUIRE(after.where == gap_where::after);

    const trigger before =
        trigger_of(R"j({"bytes": "01", "gap_us": 1, "gap": "before"})j");
    REQUIRE(before.where == gap_where::before);
    const trigger both = trigger_of(
        R"j({"bytes": "01", "gap_us": 1, "gap": "both", "state": "s"})j");
    REQUIRE(both.where == gap_where::both);
    REQUIRE_EQ(both.state, std::string("s"));

    const trigger lines = trigger_of(
        R"j({"lines": {"dtr": true, "rts": false, "break": true, "cts": false,
                       "dsr": true, "dcd": false, "ri": true}})j");
    REQUIRE_EQ(lines.lines.size(), std::size_t{7});
    const control_line wanted[] = {
        control_line::dtr, control_line::rts, control_line::break_signal,
        control_line::cts, control_line::dsr, control_line::dcd,
        control_line::ri};
    for (std::size_t i = 0; i < 7; ++i)
    {
        REQUIRE(lines.lines[i].which == wanted[i]);
        REQUIRE_EQ(lines.lines[i].high, i % 2 == 0);
    }

    const trigger opened = trigger_of(R"j({"port": "opened"})j");
    REQUIRE(opened.port.has_value());
    REQUIRE(opened.port == port_event::opened);
    const trigger closed = trigger_of(R"j({"port": "closed"})j");
    REQUIRE(closed.port == port_event::closed);
    REQUIRE(!trigger_of(R"j({"bytes": "01"})j").port.has_value());
}

TEST_CASE("rules: parse reads a do list, a sequence, then and count")
{
    const rule_set_source shorthand = parse_ok(with_rule(
        R"j({"name": "a", "when": {"bytes": "01"}, "do": [{"mark": "x"}],
             "chance_ppm": 250000})j"));
    const rule& one = shorthand.rules[0];
    REQUIRE_EQ(one.sequence.size(), std::size_t{1});
    REQUIRE_EQ(one.sequence[0].chance_ppm, std::uint32_t{250000});
    REQUIRE_EQ(one.sequence[0].actions.size(), std::size_t{1});
    REQUIRE(one.then == after_sequence::repeat);
    REQUIRE_EQ(one.count, std::uint64_t{0});

    const rule_set_source sequence = parse_ok(with_rule(
        R"j({"name": "b", "when": {"bytes": "01"},
             "sequence": [{"do": [{"mark": "1"}], "chance_ppm": 0},
                          {"do": []},
                          {"do": [{"mark": "3"}, {"mark": "4"}]}],
             "then": "stop", "count": 18446744073709551615})j"));
    const rule& many = sequence.rules[0];
    REQUIRE_EQ(many.sequence.size(), std::size_t{3});
    REQUIRE_EQ(many.sequence[0].chance_ppm, std::uint32_t{0});
    REQUIRE_EQ(many.sequence[1].chance_ppm, chance_always);
    REQUIRE(many.sequence[1].actions.empty());
    REQUIRE_EQ(many.sequence[2].actions.size(), std::size_t{2});
    REQUIRE(many.then == after_sequence::stop);
    REQUIRE_EQ(many.count, std::uint64_t{0xffffffffffffffffULL});

    const rule_set_source held = parse_ok(with_rule(
        R"j({"name": "c", "when": {"port": "closed"}, "do": [],
             "then": "hold", "count": 5})j"));
    REQUIRE(held.rules[0].then == after_sequence::hold);
    REQUIRE_EQ(held.rules[0].count, std::uint64_t{5});
}

TEST_CASE("rules: parse reads each kind of reply part")
{
    const action reply = action_of(
        R"j({"reply": ["01 02", {"slot": "s"}, {"match": [1, 3]},
                       {"crc16_modbus": "reply"}, {"crc16_ccitt": "reply"},
                       {"sum8": "reply"}, {"xor8": "reply"}, {"lrc": "reply"},
                       {"crc16_modbus": [2, 5]}, {"lrc": [0, 0]}, ""]})j");
    REQUIRE(reply.kind == action_kind::reply);
    REQUIRE_EQ(reply.parts.size(), std::size_t{11});
    REQUIRE(reply.parts[0].kind == part_kind::literal);
    REQUIRE(reply.parts[0].bytes == buf({0x01, 0x02}));
    REQUIRE(reply.parts[1].kind == part_kind::slot);
    REQUIRE_EQ(reply.parts[1].slot, std::string("s"));
    REQUIRE(reply.parts[2].kind == part_kind::match);
    REQUIRE_EQ(reply.parts[2].first, std::size_t{1});
    REQUIRE_EQ(reply.parts[2].last, std::size_t{3});
    const check_kind kinds[] = {check_kind::crc16_modbus,
                                check_kind::crc16_ccitt, check_kind::sum8,
                                check_kind::xor8, check_kind::lrc};
    for (std::size_t i = 0; i < 5; ++i)
    {
        REQUIRE(reply.parts[3 + i].kind == part_kind::check);
        REQUIRE(reply.parts[3 + i].check == kinds[i]);
        REQUIRE(reply.parts[3 + i].whole);
    }
    REQUIRE(reply.parts[8].kind == part_kind::check);
    REQUIRE(reply.parts[8].check == check_kind::crc16_modbus);
    REQUIRE(!reply.parts[8].whole);
    REQUIRE_EQ(reply.parts[8].first, std::size_t{2});
    REQUIRE_EQ(reply.parts[8].last, std::size_t{5});
    REQUIRE(reply.parts[9].check == check_kind::lrc);
    REQUIRE(!reply.parts[9].whole);
    // An empty literal is read; compile() refuses it.
    REQUIRE(reply.parts[10].kind == part_kind::literal);
    REQUIRE(reply.parts[10].bytes.empty());
}

TEST_CASE("rules: parse reads the faults of a reply")
{
    REQUIRE(action_of(R"j({"reply": ["01"]})j").fault.kind == fault_kind::none);
    REQUIRE(action_of(R"j({"reply": ["01"], "fault": "drop"})j").fault.kind ==
            fault_kind::drop);
    REQUIRE(
        action_of(R"j({"reply": ["01"], "fault": "duplicate"})j").fault.kind ==
        fault_kind::duplicate);

    const action corrupt = action_of(
        R"j({"reply": ["01"], "fault": {"corrupt": {"at": 2, "xor": "ff 0f"}}})j");
    REQUIRE(corrupt.fault.kind == fault_kind::corrupt);
    REQUIRE_EQ(corrupt.fault.at, std::size_t{2});
    REQUIRE(corrupt.fault.xor_with == buf({0xff, 0x0f}));

    const action truncate =
        action_of(R"j({"reply": ["01"], "fault": {"truncate": 3}})j");
    REQUIRE(truncate.fault.kind == fault_kind::truncate);
    REQUIRE_EQ(truncate.fault.keep, std::size_t{3});
}

TEST_CASE("rules: parse reads the actions of an inline set")
{
    const action pass = action_of(R"j({"pass": true})j");
    REQUIRE(pass.kind == action_kind::pass);
    REQUIRE(pass.fault.kind == fault_kind::none);

    const action hold = action_of(R"j({"hold": true})j");
    REQUIRE(hold.kind == action_kind::pass);
    REQUIRE(hold.fault.kind == fault_kind::none);

    const action drop = action_of(R"j({"drop": true})j");
    REQUIRE(drop.kind == action_kind::pass);
    REQUIRE(drop.fault.kind == fault_kind::drop);

    const action duplicate = action_of(R"j({"duplicate": true})j");
    REQUIRE(duplicate.kind == action_kind::pass);
    REQUIRE(duplicate.fault.kind == fault_kind::duplicate);

    const action corrupt =
        action_of(R"j({"corrupt": {"at": 0, "xor": "80"}})j");
    REQUIRE(corrupt.kind == action_kind::pass);
    REQUIRE(corrupt.fault.kind == fault_kind::corrupt);
    REQUIRE_EQ(corrupt.fault.at, std::size_t{0});
    REQUIRE(corrupt.fault.xor_with == buf({0x80}));

    const action truncate = action_of(R"j({"truncate": 2})j");
    REQUIRE(truncate.kind == action_kind::pass);
    REQUIRE(truncate.fault.kind == fault_kind::truncate);
    REQUIRE_EQ(truncate.fault.keep, std::size_t{2});

    // A delay holds: "hold" is the same as "pass" with a delay.
    const action held =
        action_of(R"j({"hold": true, "delay": {"fixed_us": 900}})j");
    REQUIRE(held.when.delay.fixed == microseconds(900));
}

TEST_CASE("rules: parse reads lines, line errors, state, slot and mark")
{
    const action lines =
        action_of(R"j({"lines": {"dcd": false, "cts": true, "ri": true}})j");
    REQUIRE(lines.kind == action_kind::set_lines);
    REQUIRE_EQ(lines.lines.size(), std::size_t{3});
    REQUIRE(lines.lines[0].which == control_line::dcd);
    REQUIRE(!lines.lines[0].high);
    REQUIRE(lines.lines[1].which == control_line::cts);
    REQUIRE(lines.lines[1].high);
    REQUIRE(lines.lines[2].which == control_line::ri);

    const std::pair<const char*, line_error> errors[] = {
        {"parity", line_error::parity},
        {"framing", line_error::framing},
        {"overrun", line_error::overrun},
        {"break", line_error::break_signal},
        {"hang_up", line_error::hang_up},
        {"disconnect", line_error::disconnect}};
    for (const auto& [name, kind] : errors)
    {
        const action raised =
            action_of(std::string(R"j({"line_error": ")j") + name + "\"}");
        REQUIRE(raised.kind == action_kind::raise_error);
        REQUIRE(raised.error == kind);
    }

    const action state = action_of(R"j({"set_state": "busy"})j");
    REQUIRE(state.kind == action_kind::set_state);
    REQUIRE_EQ(state.state, std::string("busy"));

    const action slot =
        action_of(R"j({"set_slot": {"slot": "reading", "match": [2, 3]}})j");
    REQUIRE(slot.kind == action_kind::set_slot);
    REQUIRE_EQ(slot.slot, std::string("reading"));
    REQUIRE_EQ(slot.first, std::size_t{2});
    REQUIRE_EQ(slot.last, std::size_t{3});

    const action mark = action_of(R"j({"mark": "hello"})j");
    REQUIRE(mark.kind == action_kind::mark);
    REQUIRE_EQ(mark.text, std::string("hello"));
}

TEST_CASE("rules: parse reads ask and its on_timeout list")
{
    const action plain = action_of(R"j({"ask": {"timeout_us": 5000}})j");
    REQUIRE(plain.kind == action_kind::ask);
    REQUIRE(plain.timeout == microseconds(5000));
    REQUIRE(plain.on_timeout.empty());

    const action full = action_of(
        R"j({"ask": {"timeout_us": 8000,
                     "on_timeout": [{"reply": ["ee"]},
                                    {"mark": "late", "after": "previous"}]}})j");
    REQUIRE(full.timeout == microseconds(8000));
    REQUIRE_EQ(full.on_timeout.size(), std::size_t{2});
    REQUIRE(full.on_timeout[0].kind == action_kind::reply);
    REQUIRE(full.on_timeout[1].kind == action_kind::mark);
    REQUIRE(full.on_timeout[1].when.after == edge::previous);
}

TEST_CASE("rules: parse reads after, precision and each kind of delay")
{
    const std::pair<const char*, edge> edges[] = {
        {"match_end", edge::match_end},
        {"match_start", edge::match_start},
        {"previous", edge::previous},
        {"run_start", edge::run_start},
        {"cause", edge::cause}};
    for (const auto& [name, kind] : edges)
    {
        const action each = action_of(
            std::string(R"j({"mark": "m", "after": ")j") + name + "\"}");
        REQUIRE(each.when.after == kind);
    }
    REQUIRE(action_of(R"j({"mark": "m"})j").when.after == edge::match_end);
    REQUIRE(action_of(R"j({"mark": "m"})j").when.how == precision::normal);
    REQUIRE(action_of(R"j({"mark": "m", "precision": "normal"})j").when.how ==
            precision::normal);
    REQUIRE(action_of(R"j({"mark": "m", "precision": "tight"})j").when.how ==
            precision::tight);

    const auto delay_of = [](std::string_view delay_json)
    {
        return action_of(R"j({"mark": "m", "delay": )j" +
                         std::string(delay_json) + "}")
            .when.delay;
    };

    REQUIRE(delay_of(R"j({"fixed_us": 1500})j").kind == delay_kind::fixed);
    REQUIRE(delay_of(R"j({"fixed_us": 1500})j").fixed == microseconds(1500));

    const delay_spec uniform =
        delay_of(R"j({"uniform": {"min_us": 100, "max_us": 900}})j");
    REQUIRE(uniform.kind == delay_kind::uniform);
    REQUIRE(uniform.min == microseconds(100));
    REQUIRE(uniform.max == microseconds(900));

    const delay_spec normal = delay_of(
        R"j({"normal": {"mean_us": 500, "sd_us": 50, "min_us": 300,
                        "max_us": 700}})j");
    REQUIRE(normal.kind == delay_kind::normal);
    REQUIRE(normal.mean == microseconds(500));
    REQUIRE(normal.sd == microseconds(50));
    REQUIRE(normal.min == microseconds(300));
    REQUIRE(normal.max == microseconds(700));

    const delay_spec exponential =
        delay_of(R"j({"exponential": {"mean_us": 800, "max_us": 6000}})j");
    REQUIRE(exponential.kind == delay_kind::exponential);
    REQUIRE(exponential.mean == microseconds(800));
    REQUIRE(exponential.max == microseconds(6000));
    REQUIRE(exponential.min == microseconds(0));
    const delay_spec cut = delay_of(
        R"j({"exponential": {"mean_us": 800, "max_us": 6000, "min_us": 200}})j");
    REQUIRE(cut.min == microseconds(200));

    const delay_spec table =
        delay_of(R"j({"table": {"values_us": [10, 20, 30]}})j");
    REQUIRE(table.kind == delay_kind::table);
    REQUIRE_EQ(table.values.size(), std::size_t{3});
    REQUIRE(table.values[2] == microseconds(30));
    REQUIRE(table.in_order);
    const delay_spec picked =
        delay_of(R"j({"table": {"values_us": [5], "pick": "random"}})j");
    REQUIRE(!picked.in_order);
    REQUIRE(delay_of(R"j({"table": {"values_us": [5], "pick": "in_order"}})j")
                .in_order);
}

TEST_CASE("rules: parse reads the timeline")
{
    const rule_set_source source = parse_ok(
        R"j({"attach": {"port": "p", "role": "peer"},
             "timeline": [
               {"at_us": 3000, "do": [{"mark": "a"}, {"mark": "b"}]},
               {"at_us": 1000, "do": [{"set_state": "x"}]},
               {"do": [{"mark": "c"}]}]})j");
    REQUIRE_EQ(source.timeline.size(), std::size_t{3});
    REQUIRE(source.timeline[0].at == microseconds(3000));
    REQUIRE_EQ(source.timeline[0].actions.size(), std::size_t{2});
    REQUIRE(source.timeline[1].at == microseconds(1000));
    REQUIRE(source.timeline[2].at == microseconds(0));
}

// -- refusals
// ------------------------------------------------------------------

TEST_CASE("rules: parse refuses what is not JSON, and says so")
{
    const auto problems = parse_bad("{\"attach\": ");
    REQUIRE_EQ(problems.size(), std::size_t{1});
    REQUIRE_EQ(problems[0].path, std::string());
    REQUIRE(problems[0].message.find("not JSON") != std::string::npos);

    REQUIRE(has_path(parse_bad(""), ""));
    REQUIRE(has_path(parse_bad("{} {}"), ""));
    REQUIRE(has_path(parse_bad("{'a': 1}"), ""));
    REQUIRE(has_path(parse_bad("{\"a\": 1,}"), ""));
    REQUIRE(has_path(parse_bad("{\"a\": NaN}"), ""));
}

TEST_CASE("rules: parse refuses a root that is not an object")
{
    for (const char* text : {"[]", "5", "null", "\"text\"", "true"})
    {
        const auto problems = parse_bad(text);
        REQUIRE_EQ(problems.size(), std::size_t{1});
        REQUIRE_EQ(problems[0].path, std::string());
    }
}

TEST_CASE("rules: parse refuses a document over the size limit")
{
    std::string text =
        R"j({"attach": {"port": "p", "role": "peer"}, "name": ")j";
    text.append(max_document_bytes, 'x');
    text += "\"}";
    const auto problems = parse_bad(text);
    REQUIRE_EQ(problems.size(), std::size_t{1});
    REQUIRE_EQ(problems[0].path, std::string());
    REQUIRE(problems[0].message.find("over") != std::string::npos);

    // Exactly at the limit is read (and is then refused for what it holds).
    std::string at_limit = R"j({"name": ")j";
    at_limit.append(max_document_bytes - at_limit.size() - 2, 'x');
    at_limit += "\"}";
    REQUIRE_EQ(at_limit.size(), max_document_bytes);
    const auto problems_at = parse_bad(at_limit);
    REQUIRE(has_path(problems_at, "attach"));
    REQUIRE(!has_path(problems_at, ""));
}

TEST_CASE("rules: parse refuses nesting beyond the depth limit")
{
    // "{" + "\"x\":" + n levels of "[" ... "]" + "}": n + 1 deep.
    const auto nested = [](std::size_t levels)
    {
        return "{\"x\":" + std::string(levels, '[') + std::string(levels, ']') +
               "}";
    };
    const auto too_deep = parse_bad(nested(max_document_depth));
    REQUIRE_EQ(too_deep.size(), std::size_t{1});
    REQUIRE_EQ(too_deep[0].path, std::string());
    REQUIRE(too_deep[0].message.find("nested") != std::string::npos);

    // One level less is read: it is then an unknown key, not a depth error.
    const auto allowed = parse_bad(nested(max_document_depth - 1));
    REQUIRE(has_path(allowed, "x"));
    REQUIRE(has_path(allowed, "attach"));

    // A hostile depth costs nothing: it is refused by the parser.
    const auto hostile = parse_bad(nested(100'000));
    REQUIRE_EQ(hostile.size(), std::size_t{1});
}

TEST_CASE("rules: parse refuses a delay whose only key is misspelt")
{
    // The delay has one key, so it passed the check for exactly one, and none
    // of the kinds is in it. Found by the fuzzer: the reader read the key of
    // the last kind anyway.
    require_refused(
        with_actions(
            R"j({"mark": "m", "delay": {"nomlra": {"mean_us": 5}}})j"),
        "rules[0].do[0].delay.nomlra");
    require_refused(
        with_actions(R"j({"mark": "m", "delay": {"expnential": {}}})j"),
        "rules[0].do[0].delay.expnential");
}

TEST_CASE("rules: parse refuses an unknown key at every level")
{
    const std::string base = R"j({"attach": {"port": "p", "role": "peer"})j";
    require_refused(base + R"j(, "colour": 1})j", "colour");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer", "speed": 1}})j",
        "attach.speed");
    require_refused(
        R"j({"attach": {"role": "inline", "from": "a", "to": "b", "port": "p"}})j",
        "attach.port");
    require_refused(
        R"j({"attach": {"role": "peer", "port": "p", "from": "a"}})j",
        "attach.from");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01"}, "do": [], "dely": 1})j"),
        "rules[0].dely");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01", "mask_": "ff"}, "do": []})j"),
        "rules[0].when.mask_");
    require_refused(with_rule(R"j({"name": "r", "when": {"bytes": "01"},
                       "sequence": [{"do": [], "chance": 5}]})j"),
                    "rules[0].sequence[0].chance");
    require_refused(with_actions(R"j({"mark": "m", "dalay": {}})j"),
                    "rules[0].do[0].dalay");
    require_action_refused(
        R"j({"mark": "m", "delay": {"fixed_us": 1, "x": 2}})j", ".delay.x");
    require_action_refused(
        R"j({"mark": "m", "delay": {"uniform": {"min_us": 1, "max_us": 2, "x": 2}}})j",
        ".delay.uniform.x");
    require_action_refused(
        R"j({"mark": "m", "delay": {"table": {"values_us": [1], "x": 2}}})j",
        ".delay.table.x");
    require_action_refused(
        R"j({"reply": ["01"], "fault": {"corrupt": {"at": 0, "xor": "01", "x": 1}}})j",
        ".fault.corrupt.x");
    require_action_refused(R"j({"reply": ["01"], "fault": {"drop": true}})j",
                           ".fault.drop");
    require_action_refused(R"j({"reply": [{"slot": "s", "slat": "s"}]})j",
                           ".reply[0].slat");
    require_action_refused(R"j({"reply": [{"crc32": "reply"}]})j",
                           ".reply[0].crc32");
    require_action_refused(
        R"j({"set_slot": {"slot": "s", "match": [0, 0], "x": 1}})j",
        ".set_slot.x");
    require_action_refused(R"j({"ask": {"timeout_us": 5000, "x": 1}})j",
                           ".ask.x");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"},
             "timeline": [{"at_us": 0, "do": [], "when": 1}]})j",
        "timeline[0].when");
}

TEST_CASE("rules: parse refuses a value of the wrong type")
{
    require_refused(R"j({"attach": {"port": "p", "role": "peer"}, "name": 5})j",
                    "name");
    require_refused(R"j({"attach": [], "name": "n"})j", "attach");
    require_refused(R"j({"attach": {"port": 5, "role": "peer"}})j",
                    "attach.port");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "rules": {}})j", "rules");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "rules": [5]})j",
        "rules[0]");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "states": ["a", 2]})j",
        "states[1]");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "slots": []})j", "slots");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "slots": {"a": 1}})j",
        "slots.a");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "order": 1})j", "order");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "timeline": [3]})j",
        "timeline[0]");
    require_refused(
        with_rule(R"j({"name": 3, "when": {"bytes": "01"}, "do": []})j"),
        "rules[0].name");
    require_refused(with_rule(R"j({"name": "r", "when": [], "do": []})j"),
                    "rules[0].when");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"bytes": 1}, "do": []})j"),
        "rules[0].when.bytes");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"state": 1, "bytes": "01"}, "do": []})j"),
        "rules[0].when.state");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"lines": {"dtr": 1}}, "do": []})j"),
        "rules[0].when.lines.dtr");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"lines": {"cd": true}}, "do": []})j"),
        "rules[0].when.lines.cd");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"lines": []}, "do": []})j"),
        "rules[0].when.lines");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"bytes": "01"}, "do": {}})j"),
        "rules[0].do");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01"}, "sequence": [3]})j"),
        "rules[0].sequence[0]");
    require_action_refused("7", "");
    require_action_refused(R"j({"mark": 7})j", ".mark");
    require_action_refused(R"j({"set_state": []})j", ".set_state");
    require_action_refused(R"j({"set_slot": "s"})j", ".set_slot");
    require_action_refused(R"j({"reply": "01"})j", ".reply");
    require_action_refused(R"j({"reply": [7]})j", ".reply[0]");
    require_action_refused(R"j({"mark": "m", "after": 1})j", ".after");
    require_action_refused(R"j({"mark": "m", "delay": 5})j", ".delay");
    require_action_refused(R"j({"mark": "m", "precision": true})j",
                           ".precision");
    require_action_refused(R"j({"ask": 5000})j", ".ask");
    require_action_refused(
        R"j({"ask": {"timeout_us": 5000, "on_timeout": 1}})j",
        ".ask.on_timeout");
}

TEST_CASE("rules: parse refuses a word that is not one of the choices")
{
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "order": "random"})j",
        "order");
    require_refused(R"j({"attach": {"port": "p", "role": "master"}})j",
                    "attach.role");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01", "gap": "during"}, "do": []})j"),
        "rules[0].when.gap");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"port": "reset"}, "do": []})j"),
        "rules[0].when.port");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01"}, "do": [], "then": "loop"})j"),
        "rules[0].then");
    require_action_refused(R"j({"mark": "m", "after": "match_middle"})j",
                           ".after");
    require_action_refused(R"j({"mark": "m", "precision": "loose"})j",
                           ".precision");
    require_action_refused(R"j({"line_error": "overflow"})j", ".line_error");
    require_action_refused(R"j({"reply": ["01"], "fault": "mangle"})j",
                           ".fault");
    require_action_refused(
        R"j({"mark": "m", "delay": {"table": {"values_us": [1], "pick": "first"}}})j",
        ".delay.table.pick");
    require_action_refused(R"j({"reply": [{"crc16_modbus": "request"}]})j",
                           ".reply[0].crc16_modbus");
    require_action_refused(R"j({"reply": [{"sum8": 3}]})j", ".reply[0].sum8");
}

TEST_CASE(
    "rules: parse refuses numbers that are negative, fractional or too big")
{
    const std::string base = std::string(head);
    for (const char* number :
         {"-1", "1.5", "86400000001", "\"5\"", "1e3", "true", "null"})
    {
        require_refused(base + "\"late_after_us\": " + number + "}",
                        "late_after_us");
    }
    // The largest time is accepted.
    parse_ok(base + "\"late_after_us\": 86400000000}");
    parse_ok(base + "\"late_after_us\": 0}");

    for (const char* number :
         {"-1", "0.5", "86400000001", "18446744073709551616"})
    {
        require_action_refused(
            std::string(R"j({"mark": "m", "delay": {"fixed_us": )j") + number +
                "}}",
            ".delay.fixed_us");
        require_action_refused(
            std::string(R"j({"mark": "m", "delay": {"uniform": {"min_us": )j") +
                number + R"j(, "max_us": 5}}})j",
            ".delay.uniform.min_us");
        require_action_refused(
            std::string(
                R"j({"mark": "m", "delay": {"table": {"values_us": [1, )j") +
                number + "]}}}",
            ".delay.table.values_us[1]");
        require_action_refused(std::string(R"j({"ask": {"timeout_us": )j") +
                                   number + "}}",
                               ".ask.timeout_us");
    }
    require_refused(base + R"j("timeline": [{"at_us": -5, "do": []}]})j",
                    "timeline[0].at_us");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01", "gap_us": -1}, "do": []})j"),
        "rules[0].when.gap_us");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01", "gap_us": 86400000001}, "do": []})j"),
        "rules[0].when.gap_us");
}

TEST_CASE(
    "rules: parse refuses counts and indices that are negative or not whole")
{
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01"}, "do": [], "count": -1})j"),
        "rules[0].count");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01"}, "do": [], "count": 1.5})j"),
        "rules[0].count");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01"}, "do": [], "chance_ppm": -1})j"),
        "rules[0].chance_ppm");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01"}, "do": [], "chance_ppm": 4294967296})j"),
        "rules[0].chance_ppm");
    require_refused(with_rule(R"j({"name": "r", "when": {"bytes": "01"},
                       "sequence": [{"do": [], "chance_ppm": 0.5}]})j"),
                    "rules[0].sequence[0].chance_ppm");
    require_action_refused(
        R"j({"reply": ["01"], "fault": {"corrupt": {"at": -1, "xor": "01"}}})j",
        ".fault.corrupt.at");
    require_action_refused(R"j({"truncate": -3})j", ".truncate");
    require_action_refused(R"j({"truncate": 2.5})j", ".truncate");
    require_action_refused(R"j({"reply": [{"match": [0, -1]}]})j",
                           ".reply[0].match[1]");
    require_action_refused(R"j({"reply": [{"match": [1.5, 2]}]})j",
                           ".reply[0].match[0]");
    require_action_refused(R"j({"reply": [{"match": [1]}]})j",
                           ".reply[0].match");
    require_action_refused(R"j({"reply": [{"match": [1, 2, 3]}]})j",
                           ".reply[0].match");
    require_action_refused(R"j({"reply": [{"lrc": [1]}]})j", ".reply[0].lrc");
    require_action_refused(R"j({"set_slot": {"slot": "s", "match": [-1, 1]}})j",
                           ".set_slot.match[0]");
    require_action_refused(R"j({"set_slot": {"slot": "s", "match": 1}})j",
                           ".set_slot.match");
}

TEST_CASE("rules: parse refuses a list longer than its limit, unread")
{
    // The items are wrong on purpose: if they were read, there would be more
    // than the one problem about the length.
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "states": [)j" +
            repeat_items("5", max_states + 1) + "]}",
        "states");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "rules": [)j" +
            repeat_items("5", max_rules + 1) + "]}",
        "rules");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "timeline": [)j" +
            repeat_items("5", max_timeline + 1) + "]}",
        "timeline");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"bytes": "01"}, "sequence": [)j" +
                  repeat_items("5", max_sequence_steps + 1) + "]}"),
        "rules[0].sequence");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"bytes": "01"}, "do": [)j" +
                  repeat_items("5", max_actions + 1) + "]}"),
        "rules[0].do");
    require_refused(with_rule(R"j({"name": "r", "when": {"bytes": "01"},
                       "sequence": [{"do": [)j" +
                              repeat_items("5", max_actions + 1) + "]}]}"),
                    "rules[0].sequence[0].do");
    require_action_refused(R"j({"reply": [)j" +
                               repeat_items("5", max_reply_parts + 1) + "]}",
                           ".reply");
    require_action_refused(
        R"j({"mark": "m", "delay": {"table": {"values_us": [)j" +
            repeat_items("-5", max_delay_table + 1) + "]}}}",
        ".delay.table.values_us");
    require_action_refused(R"j({"ask": {"timeout_us": 5000, "on_timeout": [)j" +
                               repeat_items("5", max_actions + 1) + "]}}",
                           ".ask.on_timeout");
    require_refused(R"j({"attach": {"port": "p", "role": "peer"}, "timeline":
                         [{"at_us": 0, "do": [)j" +
                        repeat_items("5", max_actions + 1) + "]}]}",
                    "timeline[0].do");

    // The slots are an object; the limit is the same kind of thing.
    std::string slots;
    for (std::size_t i = 0; i < max_slots + 1; ++i)
        slots += (i == 0 ? "" : ",") + std::string("\"s") + std::to_string(i) +
                 "\": 5";
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "slots": {)j" + slots +
            "}}",
        "slots");

    // At the limit itself the list is read.
    parse_ok(R"j({"attach": {"port": "p", "role": "peer"}, "states": [)j" +
             repeat_items("\"a\"", max_states) + "]}");
    parse_ok(with_rule(R"j({"name": "r", "when": {"bytes": "01"}, "do": [)j" +
                       repeat_items(R"j({"mark": "m"})j", max_actions) + "]}"));
}

TEST_CASE("rules: parse refuses hex that is not hex")
{
    require_action_refused(R"j({"reply": ["0g"]})j", ".reply[0]");
    require_action_refused(R"j({"reply": ["123"]})j", ".reply[0]");
    require_action_refused(
        R"j({"reply": ["01"], "fault": {"corrupt": {"at": 0, "xor": "zz"}}})j",
        ".fault.corrupt.xor");
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "slots": {"s": "xyz"}})j",
        "slots.s");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"bytes": "zz"}, "do": []})j"),
        "rules[0].when.bytes");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"bytes": ""}, "do": []})j"),
        "rules[0].when.bytes");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01", "mask": "zz"}, "do": []})j"),
        "rules[0].when.mask");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01", "mask": ""}, "do": []})j"),
        "rules[0].when.mask");
    // A pattern byte is two characters.
    require_refused(
        with_rule(R"j({"name": "r", "when": {"bytes": "1 ??"}, "do": []})j"),
        "rules[0].when.bytes");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"bytes": "11?? 22"}, "do": []})j"),
        "rules[0].when.bytes");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"bytes": "1g ??"}, "do": []})j"),
        "rules[0].when.bytes");
    // A mask cannot be given beside wildcards.
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "11 ??", "mask": "ff ff"}, "do": []})j"),
        "rules[0].when.mask");
}

TEST_CASE("rules: parse refuses an action with no kind or two kinds")
{
    require_action_refused("{}", "");
    require_action_refused(R"j({"after": "match_end"})j", "");
    require_action_refused(R"j({"mark": "m", "set_state": "s"})j", "");
    require_action_refused(R"j({"pass": true, "drop": true})j", "");
    require_action_refused(R"j({"reply": ["01"], "lines": {"dcd": true}})j",
                           "");
}

TEST_CASE("rules: parse refuses a fault beside anything but a reply")
{
    require_action_refused(R"j({"mark": "m", "fault": "drop"})j", ".fault");
    require_action_refused(R"j({"pass": true, "fault": "drop"})j", ".fault");
    require_action_refused(R"j({"set_state": "s", "fault": "drop"})j",
                           ".fault");
    require_action_refused(R"j({"lines": {"dtr": true}, "fault": "drop"})j",
                           ".fault");
}

TEST_CASE("rules: parse accepts only true for pass, hold, drop and duplicate")
{
    for (const char* key : {"pass", "hold", "drop", "duplicate"})
    {
        for (const char* value : {"false", "1", "\"yes\"", "null", "[]"})
        {
            require_action_refused(std::string("{\"") + key + "\": " + value +
                                       "}",
                                   std::string(".") + key);
        }
    }
}

TEST_CASE(
    "rules: parse refuses a delay with no kind, two kinds or a missing key")
{
    require_action_refused(R"j({"mark": "m", "delay": {}})j", ".delay");
    require_action_refused(
        R"j({"mark": "m", "delay": {"fixed_us": 1, "uniform": {"min_us": 1, "max_us": 2}}})j",
        ".delay");
    require_action_refused(
        R"j({"mark": "m", "delay": {"uniform": {"min_us": 1}}})j",
        ".delay.uniform.max_us");
    require_action_refused(
        R"j({"mark": "m", "delay": {"uniform": {"max_us": 1}}})j",
        ".delay.uniform.min_us");
    require_action_refused(
        R"j({"mark": "m", "delay": {"normal": {"mean_us": 5, "sd_us": 1, "min_us": 1}}})j",
        ".delay.normal.max_us");
    require_action_refused(
        R"j({"mark": "m", "delay": {"normal": {"mean_us": 5, "sd_us": 1, "max_us": 9}}})j",
        ".delay.normal.min_us");
    require_action_refused(
        R"j({"mark": "m", "delay": {"normal": {"sd_us": 1, "min_us": 1, "max_us": 9}}})j",
        ".delay.normal.mean_us");
    require_action_refused(
        R"j({"mark": "m", "delay": {"normal": {"mean_us": 5, "min_us": 1, "max_us": 9}}})j",
        ".delay.normal.sd_us");
    require_action_refused(
        R"j({"mark": "m", "delay": {"exponential": {"mean_us": 5}}})j",
        ".delay.exponential.max_us");
    require_action_refused(
        R"j({"mark": "m", "delay": {"exponential": {"max_us": 5}}})j",
        ".delay.exponential.mean_us");
    require_action_refused(R"j({"mark": "m", "delay": {"table": {}}})j",
                           ".delay.table.values_us");
    require_action_refused(R"j({"mark": "m", "delay": {"uniform": 5}})j",
                           ".delay.uniform");
    require_action_refused(R"j({"mark": "m", "delay": {"table": []}})j",
                           ".delay.table");
}

TEST_CASE("rules: parse refuses what a rule, a part or an action lacks")
{
    require_refused(R"j({"name": "n"})j", "attach");
    require_refused(R"j({"attach": {"role": "peer"}})j", "attach.port");
    require_refused(R"j({"attach": {"port": "p"}})j", "attach.role");
    require_refused(R"j({"attach": {"role": "inline", "to": "b"}})j",
                    "attach.from");
    require_refused(R"j({"attach": {"role": "inline", "from": "a"}})j",
                    "attach.to");
    require_refused(with_rule(R"j({"when": {"bytes": "01"}, "do": []})j"),
                    "rules[0].name");
    require_refused(with_rule(R"j({"name": "r", "do": []})j"), "rules[0].when");
    require_refused(with_rule(R"j({"name": "r", "when": {"bytes": "01"}})j"),
                    "rules[0]");
    require_action_refused(R"j({"set_slot": {"match": [0, 0]}})j",
                           ".set_slot.slot");
    require_action_refused(R"j({"set_slot": {"slot": "s"}})j",
                           ".set_slot.match");
    require_action_refused(R"j({"ask": {}})j", ".ask.timeout_us");
    require_action_refused(
        R"j({"reply": ["01"], "fault": {"corrupt": {"xor": "01"}}})j",
        ".fault.corrupt.at");
    require_action_refused(
        R"j({"reply": ["01"], "fault": {"corrupt": {"at": 1}}})j",
        ".fault.corrupt.xor");
    require_action_refused(R"j({"reply": [{}]})j", ".reply[0]");
    require_action_refused(R"j({"reply": [{"slot": "a", "match": [0, 0]}]})j",
                           ".reply[0]");
    require_action_refused(R"j({"reply": ["01"], "fault": {}})j", ".fault");
    require_action_refused(
        R"j({"reply": ["01"], "fault": {"drop": 1, "truncate": 1}})j",
        ".fault.drop");
}

TEST_CASE("rules: parse refuses a rule with both do and sequence, or neither")
{
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01"}, "do": [], "sequence": [{"do": []}]})j"),
        "rules[0]");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01"}, "then": "stop"})j"),
        "rules[0]");
    // The share of matches goes with the shorthand only.
    require_refused(with_rule(R"j({"name": "r", "when": {"bytes": "01"},
                       "sequence": [{"do": []}], "chance_ppm": 5})j"),
                    "rules[0].chance_ppm");
}

TEST_CASE("rules: parse refuses a version other than 1")
{
    const std::string base = R"j("attach": {"port": "p", "role": "peer"})j";
    require_refused("{\"version\": 2, " + base + "}", "version");
    require_refused("{\"version\": \"1\", " + base + "}", "version");
    require_refused("{\"version\": 1.5, " + base + "}", "version");
    require_refused("{\"version\": null, " + base + "}", "version");
    parse_ok("{\"version\": 1, " + base + "}");
}

TEST_CASE("rules: parse reports several problems together")
{
    const auto problems = parse_bad(R"j({
      "version": 3,
      "attach": {"role": "peer"},
      "colour": "red",
      "order": "sideways",
      "rules": [
        {"name": "a", "when": {"bytes": "zz"}, "do": [{"mark": 5}]},
        {"when": {"bytes": "01"}, "do": [{"mark": "m", "delay": {"fixed_us": -1}}]},
        {"name": "c", "when": {"bytes": "01"}, "do": [{"reply": ["01"]}],
         "count": -1}
      ],
      "timeline": [{"at_us": "soon", "do": []}]
    })j");
    REQUIRE(has_path(problems, "version"));
    REQUIRE(has_path(problems, "attach.port"));
    REQUIRE(has_path(problems, "colour"));
    REQUIRE(has_path(problems, "order"));
    REQUIRE(has_path(problems, "rules[0].when.bytes"));
    REQUIRE(has_path(problems, "rules[0].do[0].mark"));
    REQUIRE(has_path(problems, "rules[1].name"));
    REQUIRE(has_path(problems, "rules[1].do[0].delay.fixed_us"));
    REQUIRE(has_path(problems, "rules[2].count"));
    REQUIRE(has_path(problems, "timeline[0].at_us"));
    REQUIRE_EQ(problems.size(), std::size_t{10});
    for (const auto& each : problems)
        REQUIRE(!each.message.empty());
}

TEST_CASE("rules: parse names a rule written with sequence by its step")
{
    const auto problems = parse_bad(with_rule(
        R"j({"name": "r", "when": {"bytes": "01"},
             "sequence": [{"do": [{"mark": "ok"}]},
                          {"do": [{"mark": "m"},
                                  {"mark": "n", "delay": {"uniform":
                                     {"min_us": 1, "max_us": -2}}}]}]})j"));
    REQUIRE_EQ(problems.size(), std::size_t{1});
    REQUIRE_EQ(problems[0].path,
               std::string("rules[0].sequence[1].do[1].delay.uniform.max_us"));
}

TEST_CASE("rules: parse stops listing after a great many problems")
{
    std::string text = "{\"attach\": {\"port\": \"p\", \"role\": \"peer\"}";
    for (int i = 0; i < 2000; ++i)
        text += ", \"k" + std::to_string(i) + "\": 0";
    text += '}';
    const auto problems = parse_bad(text);
    REQUIRE(problems.size() < 300);
    REQUIRE(problems.size() > 100);
    REQUIRE(problems.back().message.find("too many") != std::string::npos);
}

TEST_CASE("rules: parse quotes a very long key in short")
{
    const std::string key(5000, 'k');
    const auto problems = parse_bad(
        R"j({"attach": {"port": "p", "role": "peer"}, ")j" + key + "\": 1}");
    REQUIRE_EQ(problems.size(), std::size_t{1});
    REQUIRE(problems[0].path.size() < 100);
}

TEST_CASE("rules: parse leaves the list empty when it succeeds")
{
    std::vector<problem> problems;
    problems.push_back(problem{"old", "old"});
    const auto parsed = parse_rule_set(
        R"j({"attach": {"port": "p", "role": "peer"}})j", &problems);
    REQUIRE(parsed);
    REQUIRE(problems.empty());
    // A caller that asks for no list gets the error alone.
    const auto refused = parse_rule_set("{}", nullptr);
    REQUIRE(!refused);
    REQUIRE(refused.error() == make_error_code(errc::config_error));
}

TEST_CASE(
    "rules: compile_rule_set returns the problems of the stage that failed")
{
    // A reader problem.
    std::vector<problem> problems;
    const auto unreadable = compile_rule_set(
        R"j({"attach": {"port": "p", "role": "peer"}, "x": 1})j", &problems);
    REQUIRE(!unreadable);
    REQUIRE(unreadable.error() == make_error_code(errc::config_error));
    REQUIRE(has_path(problems, "x"));

    // A compiler problem: the document reads, the set makes no sense.
    const auto wrong = compile_rule_set(
        with_actions(
            R"j({"mark": "m", "delay": {"uniform": {"min_us": 9, "max_us": 1}}})j"),
        &problems);
    REQUIRE(!wrong);
    REQUIRE(wrong.error() == make_error_code(errc::config_error));
    REQUIRE_EQ(problems.size(), std::size_t{1});
    REQUIRE_EQ(problems[0].path,
               std::string("rules[0].do[0].delay.uniform"));

    // A good one compiles, and there is nothing to report.
    const auto good =
        compile_rule_set(with_actions(R"j({"mark": "m"})j"), &problems);
    REQUIRE(good);
    REQUIRE(problems.empty());
    REQUIRE_EQ((*good)->rules.size(), std::size_t{1});
}

TEST_CASE("rules: compile reports a do list where it was written")
{
    std::vector<problem> problems;
    const auto set = compile_rule_set(
        R"j({"attach": {"port": "p", "role": "peer"},
             "rules": [
               {"name": "a", "when": {"bytes": "01"},
                "do": [{"set_state": "missing"}]},
               {"name": "b", "when": {"bytes": "02"},
                "sequence": [{"do": [{"set_state": "missing"}]}]}]})j",
        &problems);
    REQUIRE(!set);
    REQUIRE_EQ(problems.size(), std::size_t{2});
    REQUIRE_EQ(problems[0].path, std::string("rules[0].do[0].set_state"));
    REQUIRE_EQ(problems[1].path,
               std::string("rules[1].sequence[0].do[0].set_state"));
}

TEST_CASE("rules: parse reads quiet_us and signal of a trigger")
{
    const trigger quiet = trigger_of(R"j({"quiet_us": 4500})j");
    REQUIRE(quiet.quiet == microseconds(4500));
    REQUIRE(quiet.signal.empty());
    REQUIRE(quiet.bytes.empty());
    REQUIRE(quiet.state.empty());

    const trigger in_state =
        trigger_of(R"j({"quiet_us": 1, "state": "waiting"})j");
    REQUIRE(in_state.quiet == microseconds(1));
    REQUIRE_EQ(in_state.state, std::string("waiting"));

    // The reader takes anything up to the longest time; the compiler holds
    // a rule on silence to 60 seconds.
    REQUIRE(trigger_of(R"j({"quiet_us": 86400000000})j").quiet ==
            microseconds(86'400'000'000));

    const trigger signal = trigger_of(R"j({"signal": "bad_crc"})j");
    REQUIRE_EQ(signal.signal, std::string("bad_crc"));
    REQUIRE(signal.quiet == microseconds(0));
    REQUIRE(signal.bytes.empty());

    const trigger armed =
        trigger_of(R"j({"signal": "bad_crc", "state": "armed"})j");
    REQUIRE_EQ(armed.signal, std::string("bad_crc"));
    REQUIRE_EQ(armed.state, std::string("armed"));

    // A trigger of the older kinds has neither.
    const trigger bytes = trigger_of(R"j({"bytes": "01"})j");
    REQUIRE(bytes.quiet == microseconds(0));
    REQUIRE(bytes.signal.empty());

    // Whether a rule has exactly one trigger is the compiler's to say.
    const trigger both = trigger_of(R"j({"bytes": "01", "quiet_us": 7})j");
    REQUIRE(!both.bytes.empty());
    REQUIRE(both.quiet == microseconds(7));
}

TEST_CASE("rules: parse reads the signals of a set")
{
    REQUIRE(parse_ok(R"j({"attach": {"port": "p", "role": "peer"}})j")
                .signals.empty());
    REQUIRE(parse_ok(R"j({"attach": {"port": "p", "role": "peer"},
                          "signals": []})j")
                .signals.empty());

    const rule_set_source source = parse_ok(
        R"j({"attach": {"port": "p", "role": "peer"},
             "signals": ["zeta", "alpha", "mid"]})j");
    REQUIRE_EQ(source.signals.size(), std::size_t{3});
    REQUIRE_EQ(source.signals[0], std::string("zeta"));
    REQUIRE_EQ(source.signals[1], std::string("alpha"));
    REQUIRE_EQ(source.signals[2], std::string("mid"));

    // At the limit itself the list is read.
    REQUIRE_EQ(
        parse_ok(R"j({"attach": {"port": "p", "role": "peer"}, "signals": [)j" +
                 repeat_items("\"a\"", max_signals) + "]}")
            .signals.size(),
        max_signals);
}

TEST_CASE("rules: parse reads a document with signals, a signal rule and "
          "a rule on silence")
{
    const rule_set_source source = parse_ok(
        R"j({
  "attach": { "port": "meter", "role": "station" },
  "states": ["idle", "waiting"],
  "signals": ["checksum_failed", "address_seen"],
  "rules": [
    {
      "name": "request",
      "when": { "bytes": "11 03" },
      "do": [ { "set_state": "waiting" } ]
    },
    {
      "name": "no reply came",
      "when": { "quiet_us": 250000, "state": "waiting" },
      "do": [ { "mark": "timeout" }, { "set_state": "idle" } ]
    },
    {
      "name": "break on a bad checksum",
      "when": { "signal": "checksum_failed", "state": "idle" },
      "do": [ { "line_error": "break", "after": "cause",
                "delay": { "fixed_us": 2000 } },
              { "reply": [ { "match": [0, 1] } ] } ]
    }
  ]
})j");
    REQUIRE_EQ(source.states.size(), std::size_t{2});
    REQUIRE_EQ(source.signals.size(), std::size_t{2});
    REQUIRE_EQ(source.signals[0], std::string("checksum_failed"));
    REQUIRE_EQ(source.signals[1], std::string("address_seen"));
    REQUIRE_EQ(source.rules.size(), std::size_t{3});

    const rule& silent = source.rules[1];
    REQUIRE(silent.when.quiet == microseconds(250'000));
    REQUIRE_EQ(silent.when.state, std::string("waiting"));
    REQUIRE(silent.when.bytes.empty());
    REQUIRE(silent.when.signal.empty());
    REQUIRE_EQ(silent.sequence.size(), std::size_t{1});
    REQUIRE_EQ(silent.sequence[0].actions.size(), std::size_t{2});
    REQUIRE(silent.sequence[0].actions[1].kind == action_kind::set_state);

    const rule& signalled = source.rules[2];
    REQUIRE_EQ(signalled.when.signal, std::string("checksum_failed"));
    REQUIRE_EQ(signalled.when.state, std::string("idle"));
    REQUIRE(signalled.when.quiet == microseconds(0));
    REQUIRE(signalled.when.bytes.empty());
    REQUIRE_EQ(signalled.sequence.size(), std::size_t{1});
    REQUIRE_EQ(signalled.sequence[0].actions.size(), std::size_t{2});
    const action& error = signalled.sequence[0].actions[0];
    REQUIRE(error.kind == action_kind::raise_error);
    REQUIRE(error.when.after == edge::cause);
    REQUIRE(error.when.delay.kind == delay_kind::fixed);
    REQUIRE(error.when.delay.fixed == microseconds(2000));

    // The set means something, too.
    std::vector<problem> problems;
    const auto compiled = compile(source, &problems);
    if (!compiled)
        throw vss_test::failure("rejected:" + describe(problems));
    REQUIRE_EQ((*compiled)->signals.size(), std::size_t{2});
    REQUIRE(problems.empty());
}

TEST_CASE("rules: parse refuses quiet_us, signal and signals of the wrong type")
{
    const std::string base = std::string(head);
    for (const char* number :
         {"-1", "0.5", "1e3", "86400000001", "\"5\"", "true", "null", "[]"})
    {
        require_refused(
            with_rule(std::string(R"j({"name": "r", "when": {"quiet_us": )j") +
                      number + R"j(}, "do": []})j"),
            "rules[0].when.quiet_us");
    }
    for (const char* value : {"5", "true", "null", "[]", "{}"})
    {
        require_refused(
            with_rule(std::string(R"j({"name": "r", "when": {"signal": )j") +
                      value + R"j(}, "do": []})j"),
            "rules[0].when.signal");
    }
    require_refused(base + R"j("signals": {}})j", "signals");
    require_refused(base + R"j("signals": "a"})j", "signals");
    require_refused(base + R"j("signals": ["a", 2]})j", "signals[1]");
    require_refused(base + R"j("signals": [null, "b"]})j", "signals[0]");
    // The key of the document is "signals", and of a trigger "signal".
    require_refused(base + R"j("signal": ["a"]})j", "signal");

    // The unknown-key check still sees a misspelling of the new keys.
    require_refused(
        with_rule(R"j({"name": "r", "when": {"quiet": 5}, "do": []})j"),
        "rules[0].when.quiet");
    require_refused(
        with_rule(R"j({"name": "r", "when": {"signals": "s"}, "do": []})j"),
        "rules[0].when.signals");
}

TEST_CASE("rules: parse refuses a quiet_us of zero, which is no silence")
{
    // Zero is what the model holds for "not set", so the reader refuses it
    // by name, and a rule that also has another trigger does not slip by.
    require_refused(
        with_rule(R"j({"name": "r", "when": {"quiet_us": 0}, "do": []})j"),
        "rules[0].when.quiet_us");
    require_refused(
        with_rule(
            R"j({"name": "r", "when": {"bytes": "01", "quiet_us": 0}, "do": []})j"),
        "rules[0].when.quiet_us");
    // The shortest silence is accepted.
    parse_ok(
        with_rule(R"j({"name": "r", "when": {"quiet_us": 1}, "do": []})j"));
}

TEST_CASE(
    "rules: parse refuses a list of signals longer than its limit, unread")
{
    require_refused(
        R"j({"attach": {"port": "p", "role": "peer"}, "signals": [)j" +
            repeat_items("5", max_signals + 1) + "]}",
        "signals");
}
