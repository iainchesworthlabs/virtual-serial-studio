//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The machine of a rule set, on its own.  It has no clock: every time is
// written here, so each case says exactly when bytes arrive and when an
// action is due.  Sets are written as the JSON a harness would write.
//

#include "test_framework.hpp"

#include <vss/rules/machine.hpp>
#include <vss/rules/parse.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace vss;
using namespace vss::rules;

namespace
{

using namespace std::chrono_literals;

/// A time so many milliseconds after the origin used by the cases.
time_point ms(std::int64_t count)
{
    return time_point{} + 1h + std::chrono::milliseconds(count);
}

/// A time so many microseconds after the origin used by the cases.
time_point us(std::int64_t count)
{
    return time_point{} + 1h + std::chrono::microseconds(count);
}

byte_buffer hex(std::string_view text)
{
    const auto parsed = from_hex(text);
    REQUIRE(parsed);
    return *parsed;
}

std::string describe(const std::vector<problem>& problems)
{
    std::string out;
    for (const auto& each : problems)
        out += "\n  " + each.path + ": " + each.message;
    return out;
}

std::shared_ptr<const compiled_rule_set> make_set(std::string_view text)
{
    std::vector<problem> problems;
    const auto set = compile_rule_set(text, &problems);
    if (!set)
        throw vss_test::failure("rejected:" + describe(problems));
    return *set;
}

/// A set attached to a peer, with no seed of its own unless `extra` names one.
/// `extra` is more members of the document, each followed by a comma.
std::string peer_set(std::string_view rules_json, std::string_view extra = "")
{
    return R"j({"attach": {"port": "p", "role": "peer"}, )j" +
           std::string(extra) + R"j("rules": [)j" + std::string(rules_json) +
           "]}";
}

std::string inline_set(std::string_view rules_json)
{
    return R"j({"attach": {"role": "inline", "from": "a", "to": "b"}, "rules": [)j" +
           std::string(rules_json) + "]}";
}

std::vector<firing> feed(machine& each, std::string_view data, time_point when)
{
    std::vector<firing> out;
    const byte_buffer bytes = hex(data);
    each.feed(bytes, when, out);
    return out;
}

std::vector<firing> tick(machine& each, time_point when)
{
    std::vector<firing> out;
    each.tick(when, out);
    return out;
}

bool bytes_equal(bytes_view left, const byte_buffer& right)
{
    return std::equal(left.begin(), left.end(), right.begin(), right.end());
}

control_lines lines_of(std::initializer_list<control_line> high)
{
    control_lines out;
    for (const control_line each : high)
        out.set(each, true);
    return out;
}

/// The text of the first action of a firing, which is a mark in most cases.
std::string text_of(const firing& fired, std::size_t action = 0)
{
    REQUIRE(fired.actions.size() > action);
    REQUIRE(fired.actions[action].source != nullptr);
    return fired.actions[action].source->text;
}

std::string show(const std::vector<firing>& list, bool with_times)
{
    std::ostringstream out;
    for (const auto& fired : list)
    {
        out << fired.rule << '/' << fired.step << '/' << fired.ordinal << '/'
            << fired.offset << '/' << fired.length
            << (fired.skipped ? "/skipped" : "");
        if (with_times)
            out << '@' << fired.start.time_since_epoch().count() << '-'
                << fired.end.time_since_epoch().count();
        for (const auto& planned : fired.actions)
        {
            out << " [" << static_cast<int>(planned.kind) << ':'
                << planned.index;
            if (with_times)
                out << '@' << planned.due.time_since_epoch().count();
            out << " drawn=" << planned.drawn.count() << ' '
                << to_hex(planned.bytes) << " x" << planned.copies << ']';
        }
        out << '\n';
    }
    return out.str();
}

void append(std::vector<firing>& to, std::vector<firing> from)
{
    for (auto& each : from)
        to.push_back(std::move(each));
}

} // namespace

// -- finding patterns
// ----------------------------------------------------------

TEST_CASE("rules: machine finds a pattern split across chunks once")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"bytes": "01 02 03"},
                          "do": [{"mark": "A"}]})j")),
                 1);
    std::vector<firing> got;
    append(got, feed(each, "01", ms(10)));
    append(got, feed(each, "02", ms(11)));
    REQUIRE(got.empty());
    append(got, feed(each, "03 04", ms(12)));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE_EQ(got[0].ordinal, std::uint64_t{1});
    REQUIRE_EQ(got[0].offset, std::uint64_t{0});
    REQUIRE_EQ(got[0].length, std::size_t{3});
    REQUIRE(got[0].start == ms(10));
    REQUIRE(got[0].end == ms(12));
    REQUIRE(!got[0].skipped);
    REQUIRE_EQ(text_of(got[0]), std::string("A"));

    // Again, after other bytes: the offset counts the whole stream.
    got.clear();
    append(got, feed(each, "05 01 02 03", ms(20)));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].offset, std::uint64_t{5});
    REQUIRE_EQ(got[0].ordinal, std::uint64_t{2});
    REQUIRE(got[0].start == ms(20));
    REQUIRE(got[0].end == ms(20));
}

TEST_CASE("rules: machine does not fire on a near miss")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"bytes": "01 02 03"},
                          "do": [{"mark": "A"}]})j")),
                 1);
    REQUIRE(feed(each, "01 02 04 01 03 02 03 01 02", ms(1)).empty());
    REQUIRE(feed(each, "01", ms(2)).empty());
    REQUIRE_EQ(feed(each, "02 03", ms(3)).size(), std::size_t{1});
}

namespace
{

/// Feeds `data` in chunks of odd sizes, a millisecond apart, and returns
/// every firing.
std::vector<firing> feed_in_pieces(machine& each, const byte_buffer& data)
{
    const std::size_t sizes[] = {1, 7, 63, 2, 30, 5, 22};
    std::vector<firing> got;
    std::size_t at = 0;
    std::size_t turn = 0;
    std::int64_t when = 1;
    while (at < data.size())
    {
        const std::size_t take = std::min(sizes[turn++ % 7], data.size() - at);
        each.feed(bytes_view(data).subspan(at, take), ms(when++), got);
        at += take;
    }
    return got;
}

} // namespace

TEST_CASE("rules: machine matches a pattern of more than 64 bytes")
{
    byte_buffer pattern;
    for (int i = 0; i < 130; ++i)
        pattern.push_back(static_cast<byte>(i));
    const std::string wanted = to_hex(pattern);
    // The last byte of the pattern is a wildcard on its low nibble.
    std::string with_wild = wanted.substr(0, wanted.size() - 2) + "8?";
    machine each(
        make_set(peer_set(R"j({"name": "long", "when": {"bytes": ")j" +
                          with_wild + R"j("}, "do": [{"mark": "L"}]})j")),
        1);

    // Odd-sized chunks, so the pattern crosses the words at 64 and 128.
    const std::vector<firing> got = feed_in_pieces(each, pattern);
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].length, std::size_t{130});
    REQUIRE_EQ(got[0].offset, std::uint64_t{0});

    // A pattern with one byte wrong in the middle word does not match.
    pattern[100] = static_cast<byte>(pattern[100] ^ 1U);
    std::vector<firing> out;
    each.feed(pattern, ms(500), out);
    REQUIRE(out.empty());
}

TEST_CASE("rules: machine gives the same firings however the bytes are cut")
{
    const std::string rules = R"j(
        {"name": "r0", "when": {"bytes": "01 02"}, "do": [{"mark": "r0"}]},
        {"name": "r1", "when": {"bytes": "02 03 04"}, "do": [{"mark": "r1"}]},
        {"name": "r2", "when": {"bytes": "03 ??"},
         "do": [{"mark": "r2", "delay": {"uniform": {"min_us": 0, "max_us": 1000}}}]},
        {"name": "r3", "when": {"bytes": "01 02 03 04 01"},
         "do": [{"mark": "r3"}]},
        {"name": "r4", "when": {"bytes": "04 04"}, "count": 3,
         "do": [{"mark": "r4"}]},
        {"name": "r5", "when": {"bytes": "04 01", "state": "x"},
         "do": [{"set_state": "y"}, {"mark": "r5"}]},
        {"name": "r6", "when": {"bytes": "02 02", "state": "y"},
         "do": [{"set_state": "x"}, {"mark": "r6"}]},
        {"name": "r7", "when": {"bytes": "?1 ?2 ?3 ?4"}, "chance_ppm": 500000,
         "do": [{"mark": "r7"}]}
    )j";
    const auto set = make_set(peer_set(rules, R"j("states": ["x", "y"], )j"));

    // A stream over four values, from a fixed generator of the test's own.
    byte_buffer stream;
    std::uint32_t state = 12345;
    for (int i = 0; i < 3000; ++i)
    {
        state = state * 1103515245U + 12345U;
        stream.push_back(static_cast<byte>(1 + ((state >> 16) % 4)));
    }

    const auto run = [&](std::size_t chunk, bool advancing)
    {
        machine each(set, 9);
        std::vector<firing> got;
        std::int64_t when = 5;
        for (std::size_t at = 0; at < stream.size(); at += chunk)
        {
            const std::size_t take = std::min(chunk, stream.size() - at);
            std::vector<firing> out;
            each.feed(bytes_view(stream).subspan(at, take),
                      advancing ? ms(when++) : ms(5), out);
            append(got, std::move(out));
        }
        return got;
    };

    // With every chunk at one time, the due times agree too.
    const std::string whole = show(run(stream.size(), false), true);
    REQUIRE(whole.size() > 1000);
    for (const std::size_t chunk :
         {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{5},
          std::size_t{7}, std::size_t{64}, std::size_t{1000}})
        REQUIRE_EQ(show(run(chunk, false), true), whole);

    // With the chunks apart in time, what matches and what is drawn agree.
    const std::string shape = show(run(stream.size(), true), false);
    for (const std::size_t chunk :
         {std::size_t{1}, std::size_t{4}, std::size_t{13}})
        REQUIRE_EQ(show(run(chunk, true), false), shape);
}

TEST_CASE(
    "rules: machine lets the longer pattern win, then the one listed first")
{
    // Both complete on the 03: the longer started first.
    machine longer(make_set(peer_set(
                       R"j({"name": "short", "when": {"bytes": "02 03"},
                            "do": [{"mark": "short"}]},
                           {"name": "long", "when": {"bytes": "01 02 03"},
                            "do": [{"mark": "long"}]})j")),
                   1);
    const auto got = feed(longer, "01 02 03", ms(1));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{1});
    REQUIRE_EQ(got[0].offset, std::uint64_t{0});
    REQUIRE_EQ(got[0].length, std::size_t{3});

    // The same length: the one listed first, whichever way round.
    for (const bool wild_first : {true, false})
    {
        const std::string wild =
            R"j({"name": "wild", "when": {"bytes": "?2 03"}, "do": [{"mark": "wild"}]})j";
        const std::string exact =
            R"j({"name": "exact", "when": {"bytes": "02 03"}, "do": [{"mark": "exact"}]})j";
        std::string both = wild_first ? wild : exact;
        both += ',';
        both += wild_first ? exact : wild;
        machine each(make_set(peer_set(both)), 1);
        const auto fired = feed(each, "02 03", ms(1));
        REQUIRE_EQ(fired.size(), std::size_t{1});
        REQUIRE_EQ(text_of(fired[0]),
                   std::string(wild_first ? "wild" : "exact"));
        REQUIRE_EQ(fired[0].rule, std::size_t{0});
    }
}

TEST_CASE("rules: machine uses up the partial matches of the other rules")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"bytes": "01 02"},
                          "do": [{"mark": "A"}]},
                         {"name": "b", "when": {"bytes": "02 03 04"},
                          "do": [{"mark": "B"}]})j")),
                 1);
    // The 02 is part of both, but A is decided first and uses it up.
    const auto first = feed(each, "01 02 03 04", ms(1));
    REQUIRE_EQ(first.size(), std::size_t{1});
    REQUIRE_EQ(text_of(first[0]), std::string("A"));
    // Without the 01, B has the whole of its pattern.
    const auto second = feed(each, "02 03 04", ms(2));
    REQUIRE_EQ(second.size(), std::size_t{1});
    REQUIRE_EQ(text_of(second[0]), std::string("B"));
    REQUIRE_EQ(second[0].offset, std::uint64_t{4});

    // The matches of one rule do not overlap.
    machine same(make_set(peer_set(
                     R"j({"name": "a", "when": {"bytes": "aa aa"},
                          "do": [{"mark": "A"}]})j")),
                 1);
    const auto pairs = feed(same, "aa aa aa aa aa", ms(1));
    REQUIRE_EQ(pairs.size(), std::size_t{2});
    REQUIRE_EQ(pairs[0].offset, std::uint64_t{0});
    REQUIRE_EQ(pairs[1].offset, std::uint64_t{2});
}

TEST_CASE("rules: machine matches masked patterns")
{
    machine each(make_set(peer_set(
                     R"j({"name": "wild", "when": {"bytes": "1? ?3 ??"},
                          "do": [{"mark": "W"}]},
                         {"name": "masked", "when": {"bytes": "77 80",
                                                      "mask": "ff 80"},
                          "do": [{"mark": "M"}]})j")),
                 1);
    REQUIRE_EQ(feed(each, "1f a3 00", ms(1)).size(), std::size_t{1});
    REQUIRE_EQ(feed(each, "10 03 ff", ms(2)).size(), std::size_t{1});
    REQUIRE(feed(each, "2f a3 00", ms(3)).empty());
    REQUIRE(feed(each, "1f a4 00", ms(4)).empty());

    // Only the top bit of the second byte counts.
    const auto high = feed(each, "77 ff", ms(5));
    REQUIRE_EQ(high.size(), std::size_t{1});
    REQUIRE_EQ(text_of(high[0]), std::string("M"));
    REQUIRE_EQ(feed(each, "77 81", ms(6)).size(), std::size_t{1});
    REQUIRE(feed(each, "77 7f", ms(7)).empty());
    REQUIRE(feed(each, "78 80", ms(8)).empty());
}

// -- rules that change and follow the state
// -------------------------------------

TEST_CASE("rules: machine changes state for the next byte of the same chunk")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "first", "when": {"bytes": "01", "state": "a"},
                          "do": [{"set_state": "b"}, {"mark": "one"}]},
                         {"name": "second", "when": {"bytes": "01", "state": "b"},
                          "do": [{"mark": "two"}]})j",
            R"j("states": ["a", "b"], )j")),
        1);
    REQUIRE_EQ(each.state(), std::size_t{0});
    const auto got = feed(each, "01 01 01", ms(1));
    REQUIRE_EQ(got.size(), std::size_t{3});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE_EQ(got[1].rule, std::size_t{1});
    REQUIRE_EQ(got[2].rule, std::size_t{1});
    REQUIRE_EQ(each.state(), std::size_t{1});

    // The change is already made at the match, and takes no time.
    REQUIRE(got[0].actions[0].kind == action_kind::set_state);
    REQUIRE(got[0].actions[0].due == ms(1));
    REQUIRE(got[0].actions[0].drawn == microseconds(0));
}

TEST_CASE("rules: machine ignores a rule outside its state")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "in_b", "when": {"bytes": "05 06", "state": "b"},
                          "do": [{"mark": "B"}]},
                         {"name": "any", "when": {"bytes": "05 06"},
                          "do": [{"mark": "any"}]})j",
            R"j("states": ["a", "b"], )j")),
        1);
    const auto got = feed(each, "05 06", ms(1));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(text_of(got[0]), std::string("any"));
}

TEST_CASE("rules: machine stops a rule at its count")
{
    machine each(make_set(peer_set(
                     R"j({"name": "twice", "when": {"bytes": "01"}, "count": 2,
                          "do": [{"mark": "T"}]},
                         {"name": "after", "when": {"bytes": "01"},
                          "do": [{"mark": "F"}]})j")),
                 1);
    const auto got = feed(each, "01 01 01 01", ms(1));
    REQUIRE_EQ(got.size(), std::size_t{4});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE_EQ(got[0].ordinal, std::uint64_t{1});
    REQUIRE_EQ(got[1].rule, std::size_t{0});
    REQUIRE_EQ(got[1].ordinal, std::uint64_t{2});
    // The bytes fall through to the rule after.
    REQUIRE_EQ(got[2].rule, std::size_t{1});
    REQUIRE_EQ(got[3].rule, std::size_t{1});
    REQUIRE_EQ(got[3].ordinal, std::uint64_t{2});
}

TEST_CASE("rules: machine runs the steps of a sequence")
{
    const std::string steps =
        R"j("sequence": [{"do": [{"mark": "s0"}]}, {"do": [{"mark": "s1"}]},
                         {"do": [{"mark": "s2"}]}])j";
    const auto run = [&](std::string_view then, int matches)
    {
        machine each(
            make_set(peer_set(R"j({"name": "q", "when": {"bytes": "01"}, )j" +
                              steps + R"j(, "then": ")j" + std::string(then) +
                              R"j("},
                    {"name": "later", "when": {"bytes": "01"},
                     "do": [{"mark": "later"}]})j")),
            1);
        std::string seen;
        for (int i = 0; i < matches; ++i)
        {
            const auto got = feed(each, "01", ms(i + 1));
            REQUIRE_EQ(got.size(), std::size_t{1});
            seen += text_of(got[0]) + " ";
        }
        return seen;
    };
    REQUIRE_EQ(run("repeat", 7), std::string("s0 s1 s2 s0 s1 s2 s0 "));
    REQUIRE_EQ(run("hold", 6), std::string("s0 s1 s2 s2 s2 s2 "));
    // After the last step the rule matches no more.
    REQUIRE_EQ(run("stop", 5), std::string("s0 s1 s2 later later "));
}

TEST_CASE("rules: machine reports the step and the ordinal of a firing")
{
    machine each(make_set(peer_set(
                     R"j({"name": "q", "when": {"bytes": "01"},
                          "sequence": [{"do": [{"mark": "a"}]},
                                       {"do": [{"mark": "b"}]}]})j")),
                 1);
    const auto got = feed(each, "01 01 01", ms(1));
    REQUIRE_EQ(got.size(), std::size_t{3});
    for (std::size_t i = 0; i < 3; ++i)
    {
        REQUIRE_EQ(got[i].step, i % 2);
        REQUIRE_EQ(got[i].ordinal, static_cast<std::uint64_t>(i + 1));
    }
}

TEST_CASE("rules: machine applies chance_ppm from the seeded source")
{
    const auto flags = [](std::string_view ppm, std::uint64_t seed, int matches)
    {
        machine each(make_set(peer_set(
                         R"j({"name": "c", "when": {"bytes": "01"},
                              "chance_ppm": )j" +
                             std::string(ppm) + R"j(, "do": [{"mark": "m"}]})j",
                         R"j("seed": )j" + std::to_string(seed) + ", ")),
                     99);
        std::string out;
        for (int i = 0; i < matches; ++i)
        {
            const auto got = feed(each, "01", ms(i + 1));
            REQUIRE_EQ(got.size(), std::size_t{1});
            out += got[0].skipped ? 's' : 'x';
            REQUIRE_EQ(got[0].actions.empty(), got[0].skipped);
        }
        return out;
    };

    // Never, and always.
    REQUIRE_EQ(flags("0", 3, 50), std::string(50, 's'));
    REQUIRE_EQ(flags("1000000", 3, 50), std::string(50, 'x'));

    // A share: the same pattern for the same seed, another for another.
    // (The set's own seed is used, so the run's seed is not in play.)
    const std::string one = flags("500000", 1, 200);
    REQUIRE_EQ(flags("500000", 1, 200), one);
    REQUIRE_NE(flags("500000", 2, 200), one);
    int applied = 0;
    for (const char each : one)
        applied += each == 'x' ? 1 : 0;
    REQUIRE(applied > 70);
    REQUIRE(applied < 130);

    // The counts say the same.
    machine each(make_set(peer_set(
                     R"j({"name": "c", "when": {"bytes": "01"},
                          "chance_ppm": 500000, "do": [{"mark": "m"}]})j")),
                 5);
    static_cast<void>(feed(each, "01 01 01 01 01 01 01 01 01 01", ms(1)));
    REQUIRE_EQ(each.stats().firings, std::uint64_t{10});
    REQUIRE(each.stats().skipped < 10);
    REQUIRE(each.stats().skipped > 0);
}

TEST_CASE("rules: machine takes the run's seed when the set has none")
{
    const std::string text = R"j({"attach": {"port": "p", "role": "peer"},
        "rules": [{"name": "c", "when": {"bytes": "01"},
                   "chance_ppm": 500000, "do": [{"mark": "m"}]}]})j";
    const auto set = make_set(text);
    const auto flags = [&](std::uint64_t seed)
    {
        machine each(set, seed);
        REQUIRE_EQ(each.seed(), seed);
        std::string out;
        for (int i = 0; i < 100; ++i)
            out += feed(each, "01", ms(i + 1))[0].skipped ? 's' : 'x';
        return out;
    };
    REQUIRE_EQ(flags(11), flags(11));
    REQUIRE_NE(flags(11), flags(12));

    // A seed in the set wins over the run's.
    const auto own = make_set(peer_set(
        R"j({"name": "c", "when": {"bytes": "01"},
             "chance_ppm": 500000, "do": [{"mark": "m"}]})j",
        R"j("seed": 77, )j"));
    machine a(own, 1);
    machine b(own, 2);
    REQUIRE_EQ(a.seed(), std::uint64_t{77});
    REQUIRE_EQ(b.seed(), std::uint64_t{77});
}

// -- when an action is due
// -----------------------------------------------------

TEST_CASE("rules: machine times an action from the end of the match")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"bytes": "01 02"},
                          "do": [{"mark": "end", "delay": {"fixed_us": 250}},
                                 {"mark": "none"}]})j")),
                 1);
    const auto got = feed(each, "01 02", ms(10));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].actions.size(), std::size_t{2});
    REQUIRE(got[0].actions[0].due == ms(10) + 250us);
    REQUIRE(got[0].actions[0].drawn == 250us);
    REQUIRE_EQ(got[0].actions[0].index, std::size_t{0});
    REQUIRE(got[0].actions[0].how == precision::normal);
    // The second asked for no delay, and is held to the first (fifo).
    REQUIRE(got[0].actions[1].drawn == microseconds(0));
    REQUIRE(got[0].actions[1].due == ms(10) + 250us);
    REQUIRE_EQ(got[0].actions[1].index, std::size_t{1});
}

TEST_CASE("rules: machine times an action from the edge it names")
{
    // A pattern that arrives in two chunks, with the actions timed from each
    // edge in turn.  The order is independent so that nothing is held back.
    machine each(make_set(peer_set(
                     R"j({"name": "e", "when": {"bytes": "01 02"},
                          "do": [{"mark": "end", "after": "match_end",
                                  "delay": {"fixed_us": 100}},
                                 {"mark": "start", "after": "match_start",
                                  "delay": {"fixed_us": 100}},
                                 {"mark": "prev", "after": "previous",
                                  "delay": {"fixed_us": 30}},
                                 {"mark": "run", "after": "run_start",
                                  "delay": {"fixed_us": 7000}}]})j",
                     R"j("order": "independent", )j")),
                 1);
    const auto timeline = each.start(ms(1000));
    REQUIRE(timeline.empty());
    REQUIRE(feed(each, "01", ms(10)).empty());
    const auto got = feed(each, "02", ms(14));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE(got[0].start == ms(10));
    REQUIRE(got[0].end == ms(14));
    REQUIRE_EQ(got[0].actions.size(), std::size_t{4});
    REQUIRE(got[0].actions[0].due == ms(14) + 100us);
    REQUIRE(got[0].actions[1].due == ms(10) + 100us);
    // "previous" is the action before it in the list: the one from the start.
    REQUIRE(got[0].actions[2].due == ms(10) + 100us + 30us);
    REQUIRE(got[0].actions[3].due == ms(1000) + 7000us);
}

TEST_CASE(
    "rules: machine does not let an action overtake an earlier one with fifo")
{
    const auto run = [](std::string_view order)
    {
        machine each(make_set(peer_set(
                         R"j({"name": "slow", "when": {"bytes": "01"},
                              "do": [{"mark": "slow", "delay": {"fixed_us": 1000}}]},
                             {"name": "quick", "when": {"bytes": "02"},
                              "do": [{"mark": "quick", "delay": {"fixed_us": 100}}]})j",
                         order)),
                     1);
        std::vector<firing> got;
        append(got, feed(each, "01", ms(10)));
        append(got, feed(each, "02", us(10'200)));
        REQUIRE_EQ(got.size(), std::size_t{2});
        return got;
    };

    const auto fifo = run("");
    REQUIRE(fifo[0].actions[0].due == ms(10) + 1000us);
    // The later match asked for 100 us, and would be due at 10.3 ms; the
    // earlier one is due at 11 ms, so it waits for it.
    REQUIRE(fifo[1].actions[0].due == ms(11));
    REQUIRE(fifo[1].actions[0].drawn == 100us);

    const auto fifo_named = run(R"j("order": "fifo", )j");
    REQUIRE(fifo_named[1].actions[0].due == ms(11));

    const auto free_order = run(R"j("order": "independent", )j");
    REQUIRE(free_order[0].actions[0].due == ms(10) + 1000us);
    REQUIRE(free_order[1].actions[0].due == us(10'300));
}

TEST_CASE("rules: machine takes a delay table in order across matches")
{
    machine each(make_set(peer_set(
                     R"j({"name": "t", "when": {"bytes": "01"},
                          "do": [{"mark": "t", "delay": {"table":
                                  {"values_us": [10, 20, 30]}}}]})j",
                     R"j("order": "independent", )j")),
                 1);
    std::vector<std::int64_t> drawn;
    drawn.reserve(7);
    for (int i = 0; i < 7; ++i)
        drawn.push_back(
            feed(each, "01", ms(i + 1))[0].actions[0].drawn.count());
    const std::vector<std::int64_t> want{10, 20, 30, 10, 20, 30, 10};
    REQUIRE(drawn == want);
}

TEST_CASE("rules: machine starts the timeline in time order")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "r", "when": {"bytes": "01"}, "do": [{"mark": "r"}]})j",
            R"j("timeline": [
                          {"at_us": 3000, "do": [{"mark": "third"}]},
                          {"at_us": 1000, "do": [{"mark": "first"}, {"lines": {"dcd": true}}]},
                          {"at_us": 2000, "do": [{"mark": "second"}]},
                          {"at_us": 2000, "do": [{"mark": "second too"}]}],
                         )j")),
        1);
    const auto timeline = each.start(ms(500));
    REQUIRE_EQ(timeline.size(), std::size_t{4});
    const char* const order[] = {"first", "second", "second too", "third"};
    const std::size_t entries[] = {1, 2, 3, 0};
    const std::int64_t at[] = {1000, 2000, 2000, 3000};
    for (std::size_t i = 0; i < 4; ++i)
    {
        REQUIRE_EQ(timeline[i].rule, no_index);
        REQUIRE_EQ(timeline[i].step, entries[i]);
        REQUIRE(timeline[i].start == ms(500) + microseconds(at[i]));
        REQUIRE(timeline[i].end == timeline[i].start);
        REQUIRE_EQ(text_of(timeline[i]), std::string(order[i]));
        REQUIRE(timeline[i].actions[0].due == ms(500) + microseconds(at[i]));
    }
    REQUIRE_EQ(timeline[0].actions.size(), std::size_t{2});
    REQUIRE(timeline[0].actions[1].kind == action_kind::set_lines);
    REQUIRE_EQ(timeline[0].actions[1].source->lines.size(), std::size_t{1});
}

TEST_CASE("rules: machine does not hold the run-start edge back with fifo")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "01"},
                          "do": [{"mark": "slow", "delay": {"fixed_us": 5000}},
                                 {"mark": "abs", "after": "run_start",
                                  "delay": {"fixed_us": 1000}}]})j")),
                 1);
    static_cast<void>(each.start(ms(100)));
    const auto got = feed(each, "01", ms(200));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE(got[0].actions[0].due == ms(200) + 5000us);
    REQUIRE(got[0].actions[1].due == ms(100) + 1000us);
}

// -- silence
// --------------------------------------------------------------------

TEST_CASE("rules: machine waits for the silence after a pattern")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "g", "when": {"bytes": "01 02", "gap_us": 5000},
                          "do": [{"mark": "G"}]})j")),
        1);
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE(feed(each, "01 02", ms(10)).empty());
    REQUIRE(each.next_deadline().has_value());
    REQUIRE(each.next_deadline() == ms(15));

    REQUIRE(tick(each, ms(12)).empty());
    REQUIRE(tick(each, ms(15) - 1us).empty());
    const auto got = tick(each, ms(15));
    REQUIRE_EQ(got.size(), std::size_t{1});
    // The firing is as if it had happened when the last byte arrived.
    REQUIRE(got[0].end == ms(10));
    REQUIRE(got[0].start == ms(10));
    REQUIRE_EQ(got[0].offset, std::uint64_t{0});
    REQUIRE_EQ(got[0].length, std::size_t{2});
    REQUIRE(got[0].actions[0].due == ms(10));
    REQUIRE(!each.next_deadline().has_value());
    // Once only.
    REQUIRE(tick(each, ms(100)).empty());
}

TEST_CASE(
    "rules: machine fires the waiting rule at the next byte, if the gap passed")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "g", "when": {"bytes": "01 02", "gap_us": 5000},
                          "do": [{"mark": "G"}]},
                         {"name": "n", "when": {"bytes": "09"},
                          "do": [{"mark": "N"}]})j")),
        1);
    REQUIRE(feed(each, "01 02", ms(10)).empty());
    // 20 ms later: the silence was long enough, so the rule fires first, and
    // then the new byte is matched.
    const auto got = feed(each, "09", ms(30));
    REQUIRE_EQ(got.size(), std::size_t{2});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE(got[0].end == ms(10));
    REQUIRE_EQ(got[1].rule, std::size_t{1});
    REQUIRE(got[1].end == ms(30));
}

TEST_CASE("rules: machine drops the waiting rule when a byte arrives sooner")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "g", "when": {"bytes": "01 02", "gap_us": 5000},
                          "do": [{"mark": "G"}]})j")),
        1);
    REQUIRE(feed(each, "01 02", ms(10)).empty());
    REQUIRE(feed(each, "03", ms(12)).empty());
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE(tick(each, ms(100)).empty());
    REQUIRE_EQ(each.stats().firings, std::uint64_t{0});

    // A byte at the same chunk also cancels it.
    REQUIRE(feed(each, "01 02 03", ms(200)).empty());
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE(tick(each, ms(300)).empty());

    // Then the pattern, alone again, is waited for as before.
    REQUIRE(feed(each, "01 02", ms(400)).empty());
    REQUIRE(each.next_deadline() == ms(405));
}

TEST_CASE("rules: machine lets a rule with no gap win over one that waits")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "waits", "when": {"bytes": "01 02", "gap_us": 5000},
                          "do": [{"mark": "waits"}]},
                         {"name": "now", "when": {"bytes": "02"},
                          "do": [{"mark": "now"}]})j")),
        1);
    // Both complete on the 02; the one with no need to wait is decided now,
    // and the other is used up.
    const auto got = feed(each, "01 02", ms(10));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{1});
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE(tick(each, ms(100)).empty());
}

TEST_CASE("rules: machine counts silence before a pattern from a chunk start")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "b", "when": {"bytes": "01 02", "gap_us": 5000,
                                                  "gap": "before"},
                          "do": [{"mark": "B"}]})j")),
        1);
    // The first byte ever follows silence, and nothing is waited for after.
    REQUIRE_EQ(feed(each, "01 02", ms(10)).size(), std::size_t{1});
    REQUIRE(!each.next_deadline().has_value());
    // 2 ms later: not enough silence.
    REQUIRE(feed(each, "01 02", ms(12)).empty());
    // 8 ms after that: enough.
    REQUIRE_EQ(feed(each, "01 02", ms(20)).size(), std::size_t{1});
    // In the middle of a chunk the silence is not known, so it does not count.
    REQUIRE(feed(each, "ff 01 02", ms(40)).empty());
    // A pattern that begins a chunk after silence counts when it ends later.
    REQUIRE(feed(each, "01", ms(60)).empty());
    const auto split = feed(each, "02", ms(61));
    REQUIRE_EQ(split.size(), std::size_t{1});
    REQUIRE(split[0].start == ms(60));
    REQUIRE(split[0].end == ms(61));
}

TEST_CASE("rules: machine counts the silence on both sides when asked")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "b", "when": {"bytes": "01 02", "gap_us": 5000,
                                                  "gap": "both"},
                          "do": [{"mark": "B"}]})j")),
        1);
    REQUIRE(feed(each, "01 02", ms(10)).empty());
    REQUIRE(each.next_deadline() == ms(15));
    REQUIRE_EQ(tick(each, ms(15)).size(), std::size_t{1});
    // Too soon after the last byte (2 ms): it is not waited for at all.
    REQUIRE(feed(each, "01 02", ms(12)).empty());
    REQUIRE(!each.next_deadline().has_value());
    // After enough silence, and then waited for.
    REQUIRE(feed(each, "01 02", ms(30)).empty());
    REQUIRE(each.next_deadline().has_value());
    REQUIRE_EQ(tick(each, ms(40)).size(), std::size_t{1});
}

// -- lines and the port
// ---------------------------------------------------------

TEST_CASE("rules: machine fires a lines rule on the edge only")
{
    machine each(make_set(peer_set(
                     R"j({"name": "l", "when": {"lines": {"dtr": true}},
                          "do": [{"mark": "L"}]})j")),
                 1);
    std::vector<firing> got;
    each.lines_changed(lines_of({}), lines_of({control_line::dtr}), ms(5), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE_EQ(got[0].length, std::size_t{0});
    REQUIRE(got[0].start == ms(5));
    REQUIRE(got[0].end == ms(5));
    REQUIRE(got[0].actions[0].due == ms(5));

    // No change of the line, or a change of another, is not the edge.
    got.clear();
    each.lines_changed(lines_of({control_line::dtr}),
                       lines_of({control_line::dtr}), ms(6), got);
    each.lines_changed(lines_of({control_line::dtr}),
                       lines_of({control_line::dtr, control_line::rts}), ms(7),
                       got);
    // The line going the other way is not it either.
    each.lines_changed(lines_of({control_line::dtr}), lines_of({}), ms(8), got);
    REQUIRE(got.empty());
}

TEST_CASE("rules: machine needs every listed line to hold after the change")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "l", "when": {"lines": {"dtr": true, "rts": true}},
                          "do": [{"mark": "L"}]})j")),
        1);
    std::vector<firing> got;
    each.lines_changed(lines_of({}), lines_of({control_line::dtr}), ms(1), got);
    REQUIRE(got.empty());
    each.lines_changed(lines_of({control_line::rts}),
                       lines_of({control_line::rts, control_line::dtr}), ms(2),
                       got);
    REQUIRE_EQ(got.size(), std::size_t{1});
    // And a line wanted low.
    machine low(
        make_set(peer_set(
            R"j({"name": "l", "when": {"lines": {"dtr": false, "dcd": true}},
                         "do": [{"mark": "L"}]})j")),
        1);
    got.clear();
    low.lines_changed(lines_of({control_line::dtr}),
                      lines_of({control_line::dcd}), ms(1), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
}

TEST_CASE("rules: machine fires only the first lines rule that matches")
{
    machine each(make_set(peer_set(
                     R"j({"name": "x", "when": {"lines": {"rts": true}},
                          "do": [{"mark": "X"}]},
                         {"name": "a", "when": {"lines": {"dtr": true}},
                          "do": [{"mark": "A"}]},
                         {"name": "b", "when": {"lines": {"dtr": true}},
                          "do": [{"mark": "B"}]})j")),
                 1);
    std::vector<firing> got;
    each.lines_changed(lines_of({}), lines_of({control_line::dtr}), ms(1), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{1});
}

TEST_CASE("rules: machine does not use up bytes when a lines rule fires")
{
    machine each(make_set(peer_set(
                     R"j({"name": "bytes", "when": {"bytes": "01 02"},
                          "do": [{"mark": "bytes"}]},
                         {"name": "l", "when": {"lines": {"dtr": true}},
                          "do": [{"mark": "L"}]})j")),
                 1);
    std::vector<firing> got;
    append(got, feed(each, "01", ms(1)));
    each.lines_changed(lines_of({}), lines_of({control_line::dtr}), ms(2), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].offset, std::uint64_t{1});
    append(got, feed(each, "02", ms(3)));
    REQUIRE_EQ(got.size(), std::size_t{2});
    REQUIRE_EQ(got[1].rule, std::size_t{0});
    REQUIRE_EQ(got[1].offset, std::uint64_t{0});
}

TEST_CASE("rules: machine honours state and count for a lines rule")
{
    machine each(make_set(peer_set(
                     R"j({"name": "go", "when": {"bytes": "01"},
                          "do": [{"set_state": "b"}]},
                         {"name": "l", "when": {"lines": {"dtr": true}, "state": "b"},
                          "count": 1, "do": [{"mark": "L"}]})j",
                     R"j("states": ["a", "b"], )j")),
                 1);
    std::vector<firing> got;
    // In state a the rule is not considered.
    each.lines_changed(lines_of({}), lines_of({control_line::dtr}), ms(1), got);
    REQUIRE(got.empty());
    static_cast<void>(feed(each, "01", ms(2)));
    REQUIRE_EQ(each.state(), std::size_t{1});
    each.lines_changed(lines_of({}), lines_of({control_line::dtr}), ms(3), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
    // It had its one match.
    each.lines_changed(lines_of({}), lines_of({control_line::dtr}), ms(4), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
}

TEST_CASE("rules: machine fires a port rule for the event it names")
{
    machine each(make_set(peer_set(
                     R"j({"name": "open", "when": {"port": "opened"},
                          "do": [{"mark": "open"}]},
                         {"name": "close", "when": {"port": "closed"},
                          "do": [{"mark": "close"}]},
                         {"name": "open2", "when": {"port": "opened"},
                          "do": [{"mark": "open2"}]})j")),
                 1);
    std::vector<firing> got;
    each.port_changed(port_event::closed, ms(1), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{1});
    REQUIRE(got[0].start == ms(1));
    got.clear();
    each.port_changed(port_event::opened, ms(2), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    // The first listed, once more: the second opened rule is never reached.
    got.clear();
    each.port_changed(port_event::opened, ms(3), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
}

// -- what a reply is
// -----------------------------------------------------------

TEST_CASE("rules: machine builds a reply of literals, slots and the match")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "10 20 30 40"},
                          "do": [{"reply": ["01", {"slot": "s"}, {"match": [1, 2]}, "ff"]}]})j",
                     R"j("slots": {"s": "aa bb"}, )j")),
                 1);
    const auto got = feed(each, "10 20 30 40", ms(1));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].actions.size(), std::size_t{1});
    REQUIRE(got[0].actions[0].kind == action_kind::reply);
    REQUIRE(got[0].actions[0].bytes == hex("01 aa bb 20 30 ff"));
    REQUIRE_EQ(got[0].actions[0].copies, 1U);
    REQUIRE(got[0].actions[0].fault == fault_kind::none);
    REQUIRE(!got[0].actions[0].cut);
}

TEST_CASE("rules: machine builds a reply from a slot as it is at the match")
{
    machine each(make_set(peer_set(
                     R"j({"name": "ask", "when": {"bytes": "10"},
                          "do": [{"reply": [{"slot": "s"}]}]},
                         {"name": "learn", "when": {"bytes": "20 ??"},
                          "do": [{"set_slot": {"slot": "s", "match": [1, 1]}},
                                 {"reply": ["99", {"slot": "s"}]}]})j",
                     R"j("slots": {"s": "aa"}, )j")),
                 1);
    REQUIRE(bytes_equal(each.slot(0), hex("aa")));

    const auto before = feed(each, "10", ms(1));
    REQUIRE(before[0].actions[0].bytes == hex("aa"));

    // The action sets the slot, and the reply after it in the list sees it.
    const auto learned = feed(each, "20 5c", ms(2));
    REQUIRE_EQ(learned.size(), std::size_t{1});
    REQUIRE(learned[0].actions[0].kind == action_kind::set_slot);
    REQUIRE(learned[0].actions[0].due == ms(2));
    REQUIRE(learned[0].actions[1].bytes == hex("99 5c"));
    REQUIRE(bytes_equal(each.slot(0), hex("5c")));
    const auto after_action = feed(each, "10", ms(3));
    REQUIRE(after_action[0].actions[0].bytes == hex("5c"));

    // The caller sets it too; a reply already planned does not change.
    const byte_buffer value = hex("01 02 03");
    REQUIRE(each.set_slot(0, value));
    REQUIRE(bytes_equal(each.slot(0), value));
    REQUIRE(after_action[0].actions[0].bytes == hex("5c"));
    const auto after_call = feed(each, "10", ms(4));
    REQUIRE(after_call[0].actions[0].bytes == hex("01 02 03"));
    // An empty slot is a reply of nothing from it.
    REQUIRE(each.set_slot(0, {}));
    REQUIRE(feed(each, "10", ms(5))[0].actions[0].bytes.empty());
}

TEST_CASE("rules: machine refuses to set a slot that is not there or too big")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "10"},
                          "do": [{"mark": "m"}]})j",
                     R"j("slots": {"s": "aa"}, )j")),
                 1);
    const byte_buffer value = hex("01");
    const auto missing = each.set_slot(1, value);
    REQUIRE(!missing);
    REQUIRE(missing.error() == make_error_code(errc::not_found));
    const byte_buffer big(max_slot_bytes + 1, byte{1});
    const auto too_big = each.set_slot(0, big);
    REQUIRE(!too_big);
    REQUIRE(too_big.error() == make_error_code(errc::invalid_argument));
    REQUIRE(bytes_equal(each.slot(0), hex("aa")));
    const byte_buffer fits(max_slot_bytes, byte{2});
    REQUIRE(each.set_slot(0, fits));
    REQUIRE_EQ(each.slot(0).size(), max_slot_bytes);
    // A slot that does not exist reads as nothing.
    REQUIRE(each.slot(7).empty());
}

TEST_CASE("rules: machine cuts a reply at the limit and says so")
{
    machine each(
        make_set(peer_set(
            R"j({"name": "r", "when": {"bytes": "10"},
                          "do": [{"reply": [{"slot": "s"}, {"slot": "s"}]}]})j",
            R"j("slots": {"s": ")j" + std::string(max_slot_bytes * 2 - 2, 'b') +
                R"j("}, )j")),
        1);
    const auto got = feed(each, "10", ms(1));
    REQUIRE_EQ(got[0].actions[0].bytes.size(), max_reply_bytes);
    REQUIRE(got[0].actions[0].cut);
}

TEST_CASE("rules: append_check gives the published values")
{
    // CRC-16/MODBUS of "123456789" is 0x4B37, low byte first.
    byte_buffer out;
    append_check(check_kind::crc16_modbus, as_bytes("123456789"), out);
    REQUIRE(out == hex("37 4b"));

    // CRC-16/CCITT-FALSE of the same is 0x29B1, high byte first.
    out.clear();
    append_check(check_kind::crc16_ccitt, as_bytes("123456789"), out);
    REQUIRE(out == hex("29 b1"));

    // By hand, over 01 02 03 04 05: the sum is 1 + 2 + 3 + 4 + 5 = 15 = 0x0f;
    // the xor is ((((1 ^ 2) ^ 3) ^ 4) ^ 5) = 3, 0, 4, 1 = 0x01; the LRC is the
    // two's complement of the sum, 256 - 15 = 241 = 0xf1.
    const byte_buffer five = hex("01 02 03 04 05");
    out.clear();
    append_check(check_kind::sum8, five, out);
    append_check(check_kind::xor8, five, out);
    append_check(check_kind::lrc, five, out);
    REQUIRE(out == hex("0f 01 f1"));

    // By hand, over ff 02: the sum is 0x101, which is 0x01 in a byte; the xor
    // is 0xfd; the LRC is 256 - 1 = 0xff.
    const byte_buffer two = hex("ff 02");
    out.clear();
    append_check(check_kind::sum8, two, out);
    append_check(check_kind::xor8, two, out);
    append_check(check_kind::lrc, two, out);
    REQUIRE(out == hex("01 fd ff"));

    // Over nothing: the CRCs start from ffff, the others from zero.
    out.clear();
    append_check(check_kind::crc16_modbus, {}, out);
    append_check(check_kind::crc16_ccitt, {}, out);
    append_check(check_kind::sum8, {}, out);
    append_check(check_kind::xor8, {}, out);
    append_check(check_kind::lrc, {}, out);
    REQUIRE(out == hex("ff ff ff ff 00 00 00"));

    // Appended to what is there.
    out = hex("aa");
    append_check(check_kind::sum8, five, out);
    REQUIRE(out == hex("aa 0f"));
}

TEST_CASE("rules: machine appends the check of the reply so far")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "10"},
                          "do": [{"reply": ["31 32 33 34 35 36 37 38 39",
                                            {"crc16_modbus": "reply"}]},
                                 {"reply": ["31 32 33 34 35 36 37 38 39",
                                            {"crc16_ccitt": "reply"}]},
                                 {"reply": ["01 02 03 04 05", {"sum8": "reply"},
                                            {"xor8": "reply"}, {"lrc": "reply"}]}]})j")),
                 1);
    const auto got = feed(each, "10", ms(1));
    REQUIRE_EQ(got[0].actions.size(), std::size_t{3});
    REQUIRE(got[0].actions[0].bytes == hex("31 32 33 34 35 36 37 38 39 37 4b"));
    REQUIRE(got[0].actions[1].bytes == hex("31 32 33 34 35 36 37 38 39 29 b1"));
    // The sum is 0x0f.  The xor is then taken over the five bytes and the
    // sum: 0x01 ^ 0x0f = 0x0e.  The LRC is over the five, the sum and the
    // xor: 0x0f + 0x0f + 0x0e = 0x2c, and 256 - 0x2c = 0xd4.
    REQUIRE(got[0].actions[2].bytes == hex("01 02 03 04 05 0f 0e d4"));
}

TEST_CASE("rules: machine appends the check of a range of the reply")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "10"},
                          "do": [{"reply": ["ff", "31 32 33 34 35 36 37 38 39",
                                            {"crc16_modbus": [1, 9]}]},
                                 {"reply": ["01 02 03", {"sum8": [1, 2]},
                                            {"xor8": [0, 0]}]},
                                 {"reply": ["01 02 03", {"sum8": [2, 100]}]},
                                 {"reply": ["01 02", {"sum8": [5, 6]}]}]})j")),
                 1);
    const auto got = feed(each, "10", ms(1));
    REQUIRE(got[0].actions[0].bytes ==
            hex("ff 31 32 33 34 35 36 37 38 39 37 4b"));
    // sum of 02 and 03 is 05; xor of the first byte alone is 01.
    REQUIRE(got[0].actions[1].bytes == hex("01 02 03 05 01"));
    // A range past the end is cut to what there is.
    REQUIRE(got[0].actions[2].bytes == hex("01 02 03 03"));
    // A range wholly past the end is a check over nothing.
    REQUIRE(got[0].actions[3].bytes == hex("01 02 00"));
}

TEST_CASE("rules: machine appends the check of a range that reaches any end")
{
    // A range up to the largest number reaches the end of the reply, as 100
    // does above.
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "10"},
                          "do": [{"reply": ["31 32 33 34 35 36 37 38 39",
                                            {"crc16_modbus": [0, 18446744073709551615]}]}]})j")),
                 1);
    const auto got = feed(each, "10", ms(1));
    REQUIRE_EQ(to_hex(got[0].actions[0].bytes),
               std::string("31 32 33 34 35 36 37 38 39 37 4b"));
}

TEST_CASE("rules: machine applies the faults of a reply")
{
    const auto run = [](std::string_view fault)
    {
        machine each(make_set(peer_set(
                         R"j({"name": "r", "when": {"bytes": "10"},
                              "do": [{"reply": ["01 02 03 04"], "fault": )j" +
                         std::string(fault) + "}]}")),
                     1);
        auto got = feed(each, "10", ms(1));
        REQUIRE_EQ(got.size(), std::size_t{1});
        return std::make_pair(std::move(got[0].actions[0]), each.stats());
    };

    const auto drop = run(R"j("drop")j");
    REQUIRE(drop.first.fault == fault_kind::drop);
    REQUIRE_EQ(drop.first.copies, 0U);
    REQUIRE(drop.first.bytes == hex("01 02 03 04"));
    REQUIRE_EQ(drop.second.replies, std::uint64_t{1});
    REQUIRE_EQ(drop.second.reply_bytes, std::uint64_t{0});
    REQUIRE_EQ(drop.second.faults, std::uint64_t{1});

    const auto twice = run(R"j("duplicate")j");
    REQUIRE(twice.first.fault == fault_kind::duplicate);
    REQUIRE_EQ(twice.first.copies, 2U);
    REQUIRE(twice.first.bytes == hex("01 02 03 04"));
    REQUIRE_EQ(twice.second.reply_bytes, std::uint64_t{8});
    REQUIRE_EQ(twice.second.faults, std::uint64_t{1});

    const auto corrupt = run(R"j({"corrupt": {"at": 1, "xor": "ff 0f"}})j");
    REQUIRE(corrupt.first.fault == fault_kind::corrupt);
    REQUIRE_EQ(corrupt.first.copies, 1U);
    REQUIRE(corrupt.first.bytes == hex("01 fd 0c 04"));
    REQUIRE_EQ(corrupt.second.reply_bytes, std::uint64_t{4});
    REQUIRE_EQ(corrupt.second.faults, std::uint64_t{1});

    // Past the end nothing is changed; a part of it is.
    REQUIRE(run(R"j({"corrupt": {"at": 4, "xor": "ff"}})j").first.bytes ==
            hex("01 02 03 04"));
    REQUIRE(run(R"j({"corrupt": {"at": 3, "xor": "ff ff"}})j").first.bytes ==
            hex("01 02 03 fb"));
    REQUIRE(run(R"j({"corrupt": {"at": 100, "xor": "ff"}})j").first.bytes ==
            hex("01 02 03 04"));
    REQUIRE(
        run(R"j({"corrupt": {"at": 18446744073709551615, "xor": "ff 01"}})j")
            .first.bytes == hex("01 02 03 04"));

    const auto cut = run(R"j({"truncate": 2})j");
    REQUIRE(cut.first.fault == fault_kind::truncate);
    REQUIRE(cut.first.bytes == hex("01 02"));
    REQUIRE_EQ(cut.second.reply_bytes, std::uint64_t{2});
    REQUIRE(run(R"j({"truncate": 4})j").first.bytes == hex("01 02 03 04"));
    REQUIRE(run(R"j({"truncate": 40})j").first.bytes == hex("01 02 03 04"));
    REQUIRE(run(R"j({"truncate": 0})j").first.bytes.empty());
}

TEST_CASE("rules: machine counts a reply with no fault and the bytes it sends")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "10"},
                          "do": [{"reply": ["01 02 03"]}, {"reply": ["04"]}]})j")),
                 1);
    static_cast<void>(feed(each, "10 10", ms(1)));
    const machine_stats& stats = each.stats();
    REQUIRE_EQ(stats.bytes_seen, std::uint64_t{2});
    REQUIRE_EQ(stats.bytes_matched, std::uint64_t{2});
    REQUIRE_EQ(stats.firings, std::uint64_t{2});
    REQUIRE_EQ(stats.skipped, std::uint64_t{0});
    REQUIRE_EQ(stats.replies, std::uint64_t{4});
    REQUIRE_EQ(stats.reply_bytes, std::uint64_t{8});
    REQUIRE_EQ(stats.faults, std::uint64_t{0});
}

TEST_CASE("rules: machine plans what an inline set does with the matched bytes")
{
    machine each(make_set(inline_set(
                     R"j({"name": "pass", "when": {"bytes": "aa bb cc"},
                          "do": [{"pass": true}]},
                         {"name": "drop", "when": {"bytes": "dd"},
                          "do": [{"drop": true}]},
                         {"name": "dup", "when": {"bytes": "ee ff"},
                          "do": [{"duplicate": true}]},
                         {"name": "cut", "when": {"bytes": "11 22 33 44"},
                          "do": [{"truncate": 2}]},
                         {"name": "bad", "when": {"bytes": "55 66"},
                          "do": [{"corrupt": {"at": 1, "xor": "01"}}]},
                         {"name": "hold", "when": {"bytes": "77"},
                          "do": [{"hold": true, "delay": {"fixed_us": 900}}]})j")),
                 1);

    const auto passed = feed(each, "aa bb cc", ms(1));
    REQUIRE(passed[0].actions[0].kind == action_kind::pass);
    REQUIRE(passed[0].actions[0].bytes == hex("aa bb cc"));
    REQUIRE_EQ(passed[0].actions[0].copies, 1U);
    REQUIRE(passed[0].actions[0].fault == fault_kind::none);

    const auto dropped = feed(each, "dd", ms(2));
    REQUIRE_EQ(dropped[0].actions[0].copies, 0U);
    REQUIRE(dropped[0].actions[0].fault == fault_kind::drop);

    const auto doubled = feed(each, "ee ff", ms(3));
    REQUIRE_EQ(doubled[0].actions[0].copies, 2U);
    REQUIRE(doubled[0].actions[0].bytes == hex("ee ff"));

    const auto cut = feed(each, "11 22 33 44", ms(4));
    REQUIRE(cut[0].actions[0].bytes == hex("11 22"));
    REQUIRE(cut[0].actions[0].fault == fault_kind::truncate);

    const auto changed = feed(each, "55 66", ms(5));
    REQUIRE(changed[0].actions[0].bytes == hex("55 67"));

    const auto held = feed(each, "77", ms(6));
    REQUIRE(held[0].actions[0].bytes == hex("77"));
    REQUIRE(held[0].actions[0].due == ms(6) + 900us);

    // Faults on passing bytes are counted, and nothing is a reply.
    REQUIRE_EQ(each.stats().faults, std::uint64_t{4});
    REQUIRE_EQ(each.stats().replies, std::uint64_t{0});
}

TEST_CASE("rules: machine hands on the other actions as compiled")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "01"},
                          "do": [{"lines": {"dcd": false, "cts": true}},
                                 {"line_error": "framing", "precision": "tight"},
                                 {"mark": "hello"},
                                 {"ask": {"timeout_us": 6000,
                                          "on_timeout": [{"mark": "late"}]}}]})j")),
                 1);
    const auto got = feed(each, "01", ms(1));
    REQUIRE_EQ(got[0].actions.size(), std::size_t{4});
    REQUIRE(got[0].actions[0].kind == action_kind::set_lines);
    REQUIRE_EQ(got[0].actions[0].source->lines.size(), std::size_t{2});
    REQUIRE(got[0].actions[1].kind == action_kind::raise_error);
    REQUIRE(got[0].actions[1].source->error == line_error::framing);
    REQUIRE(got[0].actions[1].how == precision::tight);
    REQUIRE(got[0].actions[2].kind == action_kind::mark);
    REQUIRE_EQ(got[0].actions[2].source->text, std::string("hello"));
    REQUIRE(got[0].actions[3].kind == action_kind::ask);
    REQUIRE(got[0].actions[3].source->timeout == 6000us);
    REQUIRE_EQ(got[0].actions[3].source->on_timeout.size(), std::size_t{1});
    // The sources point into the set, which the machine keeps.
    REQUIRE(got[0].actions[2].source ==
            &each.set().rules[0].sequence[0].actions[2]);
}

// -- the tail, the counts and the repeat --------------------------------------

TEST_CASE(
    "rules: machine keeps the unmatched tail, one short of the longest pattern")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "01 02 03 04"},
                          "do": [{"mark": "m"}]})j")),
                 1);
    REQUIRE(each.unmatched_tail().empty());
    static_cast<void>(feed(each, "aa bb", ms(1)));
    REQUIRE(each.unmatched_tail() == hex("aa bb"));
    static_cast<void>(feed(each, "cc dd ee", ms(2)));
    REQUIRE(each.unmatched_tail() == hex("cc dd ee"));
    static_cast<void>(feed(each, "01 02 03 04", ms(3)));
    REQUIRE(each.unmatched_tail().empty());
    static_cast<void>(feed(each, "01 02 03", ms(4)));
    REQUIRE(each.unmatched_tail() == hex("01 02 03"));
    // Enough bytes to trim the buffer inside: still the same bound.
    byte_buffer many(1000, byte{0x7a});
    std::vector<firing> out;
    each.feed(many, ms(5), out);
    REQUIRE(each.unmatched_tail() == hex("7a 7a 7a"));
}

TEST_CASE("rules: machine has no tail for a pattern of one byte or none")
{
    machine one(make_set(peer_set(
                    R"j({"name": "r", "when": {"bytes": "01"},
                         "do": [{"mark": "m"}]})j")),
                1);
    static_cast<void>(feed(one, "aa bb cc", ms(1)));
    REQUIRE(one.unmatched_tail().empty());

    machine none(make_set(peer_set(
                     R"j({"name": "r", "when": {"port": "opened"},
                          "do": [{"mark": "m"}]})j")),
                 1);
    static_cast<void>(feed(none, "aa bb cc", ms(1)));
    REQUIRE(none.unmatched_tail().empty());
    // The bytes are counted all the same.
    REQUIRE_EQ(none.stats().bytes_seen, std::uint64_t{3});
    REQUIRE_EQ(none.stats().bytes_matched, std::uint64_t{0});
}

TEST_CASE("rules: machine counts bytes, matches and firings")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "01 02"},
                          "do": [{"mark": "m"}]})j")),
                 1);
    static_cast<void>(feed(each, "09 01 02 09 01", ms(1)));
    static_cast<void>(feed(each, "02 09", ms(2)));
    const machine_stats& stats = each.stats();
    REQUIRE_EQ(stats.bytes_seen, std::uint64_t{7});
    REQUIRE_EQ(stats.bytes_matched, std::uint64_t{4});
    REQUIRE_EQ(stats.firings, std::uint64_t{2});
    REQUIRE_EQ(stats.skipped, std::uint64_t{0});
    REQUIRE_EQ(stats.replies, std::uint64_t{0});
    REQUIRE_EQ(stats.reply_bytes, std::uint64_t{0});
    REQUIRE_EQ(stats.faults, std::uint64_t{0});
}

// -- determinism
// ----------------------------------------------------------------

namespace
{

const char* const draw_rules = R"j(
    {"name": "u", "when": {"bytes": "01"},
     "do": [{"reply": ["aa"], "delay": {"uniform": {"min_us": 0, "max_us": 50000}}}]},
    {"name": "n", "when": {"bytes": "02"},
     "do": [{"reply": ["bb"], "delay": {"normal": {"mean_us": 20000, "sd_us": 3000,
                                                    "min_us": 1000, "max_us": 40000}}}]},
    {"name": "e", "when": {"bytes": "03"},
     "do": [{"reply": ["cc"], "delay": {"exponential": {"mean_us": 5000, "max_us": 40000}}}]},
    {"name": "t", "when": {"bytes": "04"},
     "do": [{"reply": ["dd"], "delay": {"table": {"values_us": [5, 10, 15, 20, 25],
                                                  "pick": "random"}}}]}
)j";

std::string run_draws(const std::shared_ptr<const compiled_rule_set>& set,
                      std::uint64_t seed)
{
    machine each(set, seed);
    std::string out;
    for (int i = 0; i < 60; ++i)
        out += show(feed(each,
                         i % 4 == 0   ? "01"
                         : i % 4 == 1 ? "02"
                         : i % 4 == 2 ? "03"
                                      : "04",
                         ms(i)),
                    true);
    return out;
}

} // namespace

TEST_CASE(
    "rules: machine gives the same firings for the same set, seed and input")
{
    const auto set = make_set(
        R"j({"attach": {"port": "p", "role": "peer"}, "name": "set", "order": "independent", "rules": [)j" +
        std::string(draw_rules) + "]}");
    const std::string first = run_draws(set, 7);
    REQUIRE(first.size() > 500);
    REQUIRE_EQ(run_draws(set, 7), first);
    // Another seed draws other delays.
    REQUIRE_NE(run_draws(set, 8), first);
    // Even a copy of the set made again from the text.
    const auto again = make_set(
        R"j({"attach": {"port": "p", "role": "peer"}, "name": "set", "order": "independent", "rules": [)j" +
        std::string(draw_rules) + "]}");
    REQUIRE_EQ(run_draws(again, 7), first);
}

TEST_CASE("rules: machine draws for a rule do not change when a rule is added "
          "before it")
{
    const std::string last =
        R"j({"name": "last", "when": {"bytes": "55"},
             "do": [{"reply": ["ee"], "delay": {"uniform": {"min_us": 0, "max_us": 50000}}}]})j";
    const std::string other =
        R"j({"name": "other", "when": {"bytes": "66"},
             "do": [{"reply": ["ff"], "delay": {"uniform": {"min_us": 0, "max_us": 50000}}}]})j";
    const auto alone = make_set(
        R"j({"attach": {"port": "p", "role": "peer"}, "name": "set", "seed": 5, "order": "independent", "rules": [)j" +
        last + "]}");
    const auto before = make_set(
        R"j({"attach": {"port": "p", "role": "peer"}, "name": "set", "seed": 5, "order": "independent", "rules": [)j" +
        other + "," + last + "]}");

    machine a(alone, 1);
    machine b(before, 1);
    for (int i = 0; i < 20; ++i)
    {
        const auto from_a = feed(a, "55", ms(i));
        // In the second set, the other rule fires in between.
        static_cast<void>(feed(b, "66 66", ms(i)));
        const auto from_b = feed(b, "55", ms(i));
        REQUIRE_EQ(from_a.size(), std::size_t{1});
        REQUIRE_EQ(from_b.size(), std::size_t{1});
        REQUIRE(from_a[0].actions[0].drawn == from_b[0].actions[0].drawn);
        REQUIRE(from_a[0].actions[0].due == from_b[0].actions[0].due);
        REQUIRE_EQ(from_a[0].rule, std::size_t{0});
        REQUIRE_EQ(from_b[0].rule, std::size_t{1});
    }
}

TEST_CASE("rules: machine draws differently for two rules of one set")
{
    const auto set = make_set(
        R"j({"attach": {"port": "p", "role": "peer"}, "name": "set", "seed": 5, "order": "independent", "rules": [
              {"name": "x", "when": {"bytes": "01"},
               "do": [{"mark": "x", "delay": {"uniform": {"min_us": 0, "max_us": 50000}}}]},
              {"name": "y", "when": {"bytes": "02"},
               "do": [{"mark": "y", "delay": {"uniform": {"min_us": 0, "max_us": 50000}}}]}]})j");
    machine each(set, 1);
    std::string xs;
    std::string ys;
    for (int i = 0; i < 10; ++i)
    {
        xs += std::to_string(
                  feed(each, "01", ms(i))[0].actions[0].drawn.count()) +
              " ";
        ys += std::to_string(
                  feed(each, "02", ms(i))[0].actions[0].drawn.count()) +
              " ";
    }
    REQUIRE_NE(xs, ys);
}

TEST_CASE("rules: machine times a fixed delay the same under any seed")
{
    // A fixed delay draws nothing: the same set with another seed times the
    // same.
    const auto set = make_set(peer_set(
        R"j({"name": "r", "when": {"bytes": "01"},
             "do": [{"reply": ["aa"], "delay": {"fixed_us": 321}}]})j"));
    machine a(set, 1);
    machine b(set, 2);
    REQUIRE(feed(a, "01", ms(1))[0].actions[0].due ==
            feed(b, "01", ms(1))[0].actions[0].due);
}

TEST_CASE("rules: machine tick and next_deadline do nothing when nothing waits")
{
    machine each(make_set(peer_set(
                     R"j({"name": "r", "when": {"bytes": "01"},
                          "do": [{"mark": "m"}]})j")),
                 1);
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE(tick(each, ms(1)).empty());
    static_cast<void>(feed(each, "01", ms(2)));
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE(tick(each, ms(3)).empty());
    // An empty chunk is nothing.
    std::vector<firing> out;
    each.feed({}, ms(4), out);
    REQUIRE(out.empty());
    REQUIRE_EQ(each.stats().bytes_seen, std::uint64_t{1});
}

TEST_CASE("rules: machine leaves a timeline state change to its due time")
{
    machine each(make_set(R"j({"attach": {"port": "p", "role": "peer"},
            "states": ["normal", "busy"],
            "timeline": [
              {"at_us": 5000, "do": [{"set_state": "busy"}]}]})j"),
                 1);
    const auto timeline = each.start(ms(0));
    REQUIRE_EQ(timeline.size(), std::size_t{1});
    REQUIRE_EQ(timeline[0].actions.size(), std::size_t{1});
    REQUIRE(timeline[0].actions[0].due == us(5000));
    // Nothing has happened yet: the engine applies it when it is due.
    REQUIRE_EQ(each.state(), std::size_t{0});
    REQUIRE(each.set_state(timeline[0].actions[0].source->state));
    REQUIRE_EQ(each.state(), std::size_t{1});
    REQUIRE(!each.set_state(2));
    REQUIRE_EQ(each.state(), std::size_t{1});
}

// -- rules on silence --------------------------------------------------------

namespace
{

std::vector<firing> raise(machine& each, std::size_t index,
                          std::string_view carried, time_point cause,
                          time_point when)
{
    std::vector<firing> out;
    const byte_buffer bytes = hex(carried);
    each.signal(index, bytes, cause, when, out);
    return out;
}

} // namespace

TEST_CASE("rules: machine fires a rule on silence once, at the end of the "
          "silence")
{
    machine each(make_set(peer_set(
                     R"j({"name": "q", "when": {"quiet_us": 5000},
                          "do": [{"mark": "Q"}]})j")),
                 1);
    // Nothing has arrived, so there is no silence to count.
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE(tick(each, ms(1)).empty());
    REQUIRE(tick(each, ms(100000)).empty());
    REQUIRE(!each.next_deadline().has_value());
    static_cast<void>(each.start(ms(0)));
    REQUIRE(tick(each, ms(100000)).empty());
    REQUIRE(!each.next_deadline().has_value());

    REQUIRE(feed(each, "01 02", ms(10)).empty());
    REQUIRE(each.next_deadline().has_value());
    REQUIRE(each.next_deadline() == ms(15));

    REQUIRE(tick(each, ms(12)).empty());
    REQUIRE(tick(each, ms(15) - 1us).empty());
    REQUIRE(each.next_deadline() == ms(15));

    // Called late, it is still the instant the silence was complete.
    const auto got = tick(each, ms(40));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE(got[0].start == ms(15));
    REQUIRE(got[0].end == ms(15));
    REQUIRE_EQ(got[0].length, std::size_t{0});
    REQUIRE_EQ(got[0].ordinal, std::uint64_t{1});
    REQUIRE_EQ(got[0].signal, no_index);
    REQUIRE_EQ(got[0].signal_bytes, std::size_t{0});
    REQUIRE_EQ(got[0].actions.size(), std::size_t{1});
    REQUIRE(got[0].actions[0].due == ms(15));
    REQUIRE_EQ(text_of(got[0]), std::string("Q"));
    REQUIRE_EQ(each.stats().firings, std::uint64_t{1});
    REQUIRE_EQ(each.stats().bytes_matched, std::uint64_t{0});
    REQUIRE_EQ(each.stats().bytes_seen, std::uint64_t{2});

    // Not again in the same silence.
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE(tick(each, ms(41)).empty());
    REQUIRE(tick(each, ms(100000)).empty());
    REQUIRE_EQ(each.stats().firings, std::uint64_t{1});

    // More bytes, and the next silence is counted from them.
    REQUIRE(feed(each, "03", ms(50)).empty());
    REQUIRE(each.next_deadline() == ms(55));
    REQUIRE(tick(each, ms(54)).empty());
    const auto again = tick(each, ms(55));
    REQUIRE_EQ(again.size(), std::size_t{1});
    REQUIRE(again[0].start == ms(55));
    REQUIRE(again[0].end == ms(55));
    REQUIRE_EQ(again[0].ordinal, std::uint64_t{2});
    REQUIRE(!each.next_deadline().has_value());
}

TEST_CASE("rules: machine counts silence from the last byte, not the first")
{
    machine each(make_set(peer_set(
                     R"j({"name": "q", "when": {"quiet_us": 5000},
                          "do": [{"mark": "Q"}]})j")),
                 1);
    // Bytes that keep coming each push the deadline on.
    REQUIRE(feed(each, "01", ms(10)).empty());
    REQUIRE(feed(each, "02", ms(14)).empty());
    REQUIRE(each.next_deadline() == ms(19));
    REQUIRE(feed(each, "03", ms(18)).empty());
    REQUIRE(each.next_deadline() == ms(23));
    REQUIRE(tick(each, ms(22)).empty());
    REQUIRE_EQ(tick(each, ms(23)).size(), std::size_t{1});

    // An empty chunk is not a byte.
    std::vector<firing> out;
    each.feed({}, ms(30), out);
    REQUIRE(out.empty());
    REQUIRE(!each.next_deadline().has_value());
}

TEST_CASE("rules: machine does not try a rule on silence again in the same "
          "silence")
{
    // The rule needs "b", and the set is in "a" when the silence completes.
    machine each(make_set(peer_set(
                     R"j({"name": "q", "when": {"quiet_us": 5000,
                                                "state": "b"},
                          "do": [{"mark": "Q"}]})j",
                     R"j("states": ["a", "b"], )j")),
                 1);
    REQUIRE(feed(each, "01", ms(10)).empty());
    REQUIRE(each.next_deadline() == ms(15));
    REQUIRE(tick(each, ms(15)).empty());
    // It has had its turn: nothing is due any more.
    REQUIRE(!each.next_deadline().has_value());

    // The state changes later in the same silence, and it still does not fire.
    REQUIRE(each.set_state(1));
    REQUIRE(tick(each, ms(20)).empty());
    REQUIRE(tick(each, ms(100000)).empty());
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE_EQ(each.stats().firings, std::uint64_t{0});

    // After the next bytes it is in "b" when the silence completes.
    REQUIRE(feed(each, "02", ms(30)).empty());
    REQUIRE(each.next_deadline() == ms(35));
    const auto got = tick(each, ms(35));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE(got[0].end == ms(35));
}

TEST_CASE("rules: machine fires rules on silence in the order of their times")
{
    // The longer silence is listed first.  Both are due at once.
    const auto set = [](std::string_view second_state)
    {
        return make_set(peer_set(
            R"j({"name": "long", "when": {"quiet_us": 8000,
                                          "state": ")j" +
                std::string(second_state) + R"j("},
                 "do": [{"mark": "long"}]},
                {"name": "short", "when": {"quiet_us": 5000, "state": "a"},
                 "do": [{"mark": "short"}, {"set_state": "b"}]})j",
            R"j("states": ["a", "b"], )j"));
    };

    // The shorter one's set_state decides whether the longer one fires.
    machine moves_on(set("b"), 1);
    REQUIRE(feed(moves_on, "01", ms(10)).empty());
    REQUIRE(moves_on.next_deadline() == ms(15));
    const auto got = tick(moves_on, ms(100));
    REQUIRE_EQ(got.size(), std::size_t{2});
    REQUIRE_EQ(got[0].rule, std::size_t{1});
    REQUIRE(got[0].end == ms(15));
    REQUIRE_EQ(text_of(got[0]), std::string("short"));
    REQUIRE_EQ(got[1].rule, std::size_t{0});
    REQUIRE(got[1].start == ms(18));
    REQUIRE(got[1].end == ms(18));
    REQUIRE_EQ(text_of(got[1]), std::string("long"));
    REQUIRE_EQ(moves_on.state(), std::size_t{1});
    REQUIRE(!moves_on.next_deadline().has_value());

    machine stays(set("a"), 1);
    REQUIRE(feed(stays, "01", ms(10)).empty());
    const auto only = tick(stays, ms(100));
    REQUIRE_EQ(only.size(), std::size_t{1});
    REQUIRE_EQ(only[0].rule, std::size_t{1});
    // The longer one's state was lost with the first firing's set_state.
    REQUIRE(!stays.next_deadline().has_value());
    REQUIRE(tick(stays, ms(200)).empty());

    // Between the two deadlines only the shorter one is due.
    machine between(set("b"), 1);
    REQUIRE(feed(between, "01", ms(10)).empty());
    const auto first = tick(between, ms(16));
    REQUIRE_EQ(first.size(), std::size_t{1});
    REQUIRE_EQ(first[0].rule, std::size_t{1});
    REQUIRE(between.next_deadline() == ms(18));
    const auto second = tick(between, ms(18));
    REQUIRE_EQ(second.size(), std::size_t{1});
    REQUIRE_EQ(second[0].rule, std::size_t{0});
}

TEST_CASE("rules: machine fires equal silences in the order they are listed")
{
    machine each(make_set(peer_set(
                     R"j({"name": "x", "when": {"quiet_us": 5000},
                          "do": [{"mark": "X"}]},
                         {"name": "y", "when": {"quiet_us": 5000},
                          "do": [{"mark": "Y"}]})j")),
                 1);
    REQUIRE(feed(each, "01", ms(10)).empty());
    REQUIRE(each.next_deadline() == ms(15));
    const auto got = tick(each, ms(15));
    REQUIRE_EQ(got.size(), std::size_t{2});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE_EQ(got[1].rule, std::size_t{1});
    REQUIRE(got[0].end == ms(15));
    REQUIRE(got[1].end == ms(15));
}

TEST_CASE("rules: machine puts the rule that matched bytes before a rule on "
          "silence due at the same instant")
{
    // The quiet rule is listed first, and is due when the gap rule is.
    machine each(make_set(peer_set(
                     R"j({"name": "q", "when": {"quiet_us": 5000},
                          "do": [{"mark": "Q"}]},
                         {"name": "g", "when": {"bytes": "01 02",
                                                "gap_us": 5000},
                          "do": [{"mark": "G"}]})j")),
                 1);
    REQUIRE(feed(each, "01 02", ms(10)).empty());
    REQUIRE(each.next_deadline() == ms(15));
    const auto got = tick(each, ms(15));
    REQUIRE_EQ(got.size(), std::size_t{2});
    REQUIRE_EQ(got[0].rule, std::size_t{1});
    REQUIRE_EQ(text_of(got[0]), std::string("G"));
    REQUIRE_EQ(got[0].length, std::size_t{2});
    REQUIRE_EQ(got[1].rule, std::size_t{0});
    REQUIRE_EQ(text_of(got[1]), std::string("Q"));
    REQUIRE_EQ(got[1].length, std::size_t{0});
    REQUIRE(!each.next_deadline().has_value());
}

TEST_CASE("rules: machine orders a gap rule and a rule on silence by their "
          "times")
{
    // The rule on silence is due first, at 14 ms, and the one that waited
    // for its gap is decided after it, at 19 ms.
    machine each(make_set(peer_set(
                     R"j({"name": "g", "when": {"bytes": "01 02",
                                                "gap_us": 9000},
                          "do": [{"mark": "G"}]},
                         {"name": "q", "when": {"quiet_us": 4000},
                          "do": [{"mark": "Q"}]})j")),
                 1);
    REQUIRE(feed(each, "01 02", ms(10)).empty());
    REQUIRE(each.next_deadline() == ms(14));
    const auto got = tick(each, ms(30));
    REQUIRE_EQ(got.size(), std::size_t{2});
    REQUIRE_EQ(got[0].rule, std::size_t{1});
    REQUIRE(got[0].end == ms(14));
    REQUIRE_EQ(got[1].rule, std::size_t{0});
    REQUIRE(got[1].end == ms(10));
}

TEST_CASE("rules: machine ends a rule on silence with count or then stop")
{
    machine counted(make_set(peer_set(
                        R"j({"name": "q", "when": {"quiet_us": 5000},
                             "do": [{"mark": "Q"}], "count": 2})j")),
                    1);
    REQUIRE(feed(counted, "01", ms(10)).empty());
    REQUIRE_EQ(tick(counted, ms(15)).size(), std::size_t{1});
    REQUIRE(feed(counted, "01", ms(20)).empty());
    REQUIRE_EQ(tick(counted, ms(25)).size(), std::size_t{1});
    // Used up: it is neither waited for nor fired.
    REQUIRE(feed(counted, "01", ms(30)).empty());
    REQUIRE(!counted.next_deadline().has_value());
    REQUIRE(tick(counted, ms(100)).empty());
    REQUIRE_EQ(counted.stats().firings, std::uint64_t{2});

    machine stopped(make_set(peer_set(
                        R"j({"name": "q", "when": {"quiet_us": 5000},
                             "sequence": [{"do": [{"mark": "one"}]},
                                          {"do": [{"mark": "two"}]}],
                             "then": "stop"})j")),
                    1);
    REQUIRE(feed(stopped, "01", ms(10)).empty());
    const auto one = tick(stopped, ms(15));
    REQUIRE_EQ(one.size(), std::size_t{1});
    REQUIRE_EQ(one[0].step, std::size_t{0});
    REQUIRE(feed(stopped, "01", ms(20)).empty());
    const auto two = tick(stopped, ms(25));
    REQUIRE_EQ(two.size(), std::size_t{1});
    REQUIRE_EQ(two[0].step, std::size_t{1});
    REQUIRE_EQ(text_of(two[0]), std::string("two"));
    REQUIRE(feed(stopped, "01", ms(30)).empty());
    REQUIRE(!stopped.next_deadline().has_value());
    REQUIRE(tick(stopped, ms(100)).empty());

    // A rule that is used up does not hold back the one after it.
    machine passed_on(make_set(peer_set(
                          R"j({"name": "q", "when": {"quiet_us": 5000},
                               "do": [{"mark": "Q"}], "count": 1},
                              {"name": "r", "when": {"quiet_us": 7000},
                               "do": [{"mark": "R"}]})j")),
                      1);
    REQUIRE(feed(passed_on, "01", ms(10)).empty());
    REQUIRE_EQ(tick(passed_on, ms(100)).size(), std::size_t{2});
    REQUIRE(feed(passed_on, "01", ms(200)).empty());
    REQUIRE(passed_on.next_deadline() == ms(207));
    const auto last = tick(passed_on, ms(300));
    REQUIRE_EQ(last.size(), std::size_t{1});
    REQUIRE_EQ(last[0].rule, std::size_t{1});
}

TEST_CASE("rules: machine fires a rule on silence before the bytes that end "
          "the silence")
{
    machine each(make_set(peer_set(
                     R"j({"name": "q", "when": {"quiet_us": 5000,
                                                "state": "a"},
                          "do": [{"mark": "Q"}]},
                         {"name": "n", "when": {"bytes": "09"},
                          "do": [{"mark": "N"}, {"set_state": "b"}]})j",
                     R"j("states": ["a", "b"], )j")),
                 1);
    REQUIRE(feed(each, "01", ms(10)).empty());
    // No tick between: the next feed finds the silence complete.
    const auto got = feed(each, "09", ms(30));
    REQUIRE_EQ(got.size(), std::size_t{2});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE(got[0].start == ms(15));
    REQUIRE(got[0].end == ms(15));
    REQUIRE_EQ(got[0].length, std::size_t{0});
    REQUIRE_EQ(got[1].rule, std::size_t{1});
    REQUIRE(got[1].start == ms(30));
    REQUIRE(got[1].end == ms(30));
    REQUIRE_EQ(got[1].length, std::size_t{1});
    REQUIRE_EQ(each.state(), std::size_t{1});
    // The new byte begins the next silence, and the state is now "b".
    REQUIRE(each.next_deadline() == ms(35));
    REQUIRE(tick(each, ms(35)).empty());
    REQUIRE(!each.next_deadline().has_value());
    REQUIRE_EQ(each.stats().bytes_matched, std::uint64_t{1});

    // A byte that arrives inside the silence fires nothing.
    machine soon(make_set(peer_set(
                     R"j({"name": "q", "when": {"quiet_us": 5000},
                          "do": [{"mark": "Q"}]})j")),
                 1);
    REQUIRE(feed(soon, "01", ms(10)).empty());
    REQUIRE(feed(soon, "01", ms(14)).empty());
    REQUIRE(soon.next_deadline() == ms(19));
    REQUIRE_EQ(soon.stats().firings, std::uint64_t{0});
}

TEST_CASE("rules: machine times the actions of a rule on silence from the end "
          "of the silence")
{
    machine each(make_set(peer_set(
                     R"j({"name": "q", "when": {"quiet_us": 5000},
                          "do": [{"reply": ["aa"], "delay": {"fixed_us": 300}},
                                 {"mark": "m"}]})j")),
                 1);
    REQUIRE(feed(each, "01", ms(10)).empty());
    const auto got = tick(each, ms(50));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].actions.size(), std::size_t{2});
    REQUIRE(got[0].actions[0].due == ms(15) + 300us);
    REQUIRE(got[0].actions[0].bytes == hex("aa"));
    // The second has no delay of its own and is held behind the first.
    REQUIRE(got[0].actions[1].due == ms(15) + 300us);
}

// -- rules on a signal -------------------------------------------------------

namespace
{

/// The signals alpha (0), beta (1) and gamma (2) of the cases, and the states
/// idle (0) and armed (1).
const char* const signal_head =
    R"j("signals": ["alpha", "beta", "gamma"], "states": ["idle", "armed"], )j";

} // namespace

TEST_CASE("rules: machine fires the first rule listed for the signal in the "
          "current state")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a armed", "when": {"signal": "alpha",
                                                      "state": "armed"},
                          "do": [{"mark": "A-armed"}]},
                         {"name": "a any", "when": {"signal": "alpha"},
                          "do": [{"mark": "A-any"}]},
                         {"name": "a later", "when": {"signal": "alpha"},
                          "do": [{"mark": "A-later"}]},
                         {"name": "b", "when": {"signal": "beta"},
                          "do": [{"mark": "B"}]})j",
                     signal_head)),
                 1);
    // In "idle" the first rule is not in its state: the second fires, and
    // the third, which also matches, does not.
    const auto idle = raise(each, 0, "", us(100), us(200));
    REQUIRE_EQ(idle.size(), std::size_t{1});
    REQUIRE_EQ(idle[0].rule, std::size_t{1});
    REQUIRE_EQ(text_of(idle[0]), std::string("A-any"));

    // In "armed" it is the first.
    REQUIRE(each.set_state(1));
    const auto armed = raise(each, 0, "", us(300), us(400));
    REQUIRE_EQ(armed.size(), std::size_t{1});
    REQUIRE_EQ(armed[0].rule, std::size_t{0});
    REQUIRE_EQ(text_of(armed[0]), std::string("A-armed"));

    // Another signal's rule is not touched by this one, and the other way.
    const auto beta = raise(each, 1, "", us(500), us(600));
    REQUIRE_EQ(beta.size(), std::size_t{1});
    REQUIRE_EQ(beta[0].rule, std::size_t{3});
    REQUIRE_EQ(beta[0].signal, std::size_t{1});
    REQUIRE_EQ(text_of(beta[0]), std::string("B"));

    // No rule for the signal, no firing.
    REQUIRE(raise(each, 2, "", us(700), us(800)).empty());
    REQUIRE_EQ(each.stats().firings, std::uint64_t{3});
}

TEST_CASE("rules: machine fires nothing for a signal no rule takes in this "
          "state")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a armed", "when": {"signal": "alpha",
                                                      "state": "armed"},
                          "do": [{"mark": "A"}]})j",
                     signal_head)),
                 1);
    REQUIRE(raise(each, 0, "01", us(1), us(2)).empty());
    REQUIRE(raise(each, 1, "01", us(1), us(2)).empty());
    REQUIRE_EQ(each.stats().firings, std::uint64_t{0});
    REQUIRE(each.set_state(1));
    REQUIRE_EQ(raise(each, 0, "01", us(1), us(2)).size(), std::size_t{1});

    // A rule on the stream is not a rule on a signal, whatever its index.
    machine stream(make_set(peer_set(
                       R"j({"name": "s", "when": {"bytes": "01"},
                            "do": [{"mark": "S"}]})j",
                       R"j("signals": ["alpha"], )j")),
                   1);
    REQUIRE(raise(stream, 0, "01", us(1), us(2)).empty());
}

TEST_CASE("rules: machine says what a signal firing is, and counts no bytes")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"signal": "beta"},
                          "do": [{"mark": "A"}]})j",
                     signal_head)),
                 1);
    // Some traffic first, so that the counters are not all zero.
    REQUIRE(feed(each, "01 02 03", ms(1)).empty());
    const machine_stats before = each.stats();

    const auto got = raise(each, 1, "de ad be ef", us(1000), us(5000));
    REQUIRE_EQ(got.size(), std::size_t{1});
    const firing& fired = got[0];
    REQUIRE_EQ(fired.rule, std::size_t{0});
    REQUIRE_EQ(fired.step, std::size_t{0});
    REQUIRE_EQ(fired.ordinal, std::uint64_t{1});
    REQUIRE_EQ(fired.signal, std::size_t{1});
    REQUIRE(fired.cause == us(1000));
    REQUIRE_EQ(fired.signal_bytes, std::size_t{4});
    REQUIRE_EQ(fired.length, std::size_t{0});
    REQUIRE(fired.start == us(5000));
    REQUIRE(fired.end == us(5000));
    REQUIRE(!fired.skipped);

    REQUIRE_EQ(each.stats().firings, before.firings + 1);
    REQUIRE_EQ(each.stats().bytes_matched, before.bytes_matched);
    REQUIRE_EQ(each.stats().bytes_seen, before.bytes_seen);
    REQUIRE_EQ(each.stats().bytes_seen, std::uint64_t{3});

    // A firing from the stream names no signal.
    machine bytes(make_set(peer_set(
                      R"j({"name": "b", "when": {"bytes": "01"},
                           "do": [{"mark": "B"}]})j",
                      signal_head)),
                  1);
    const auto from_stream = feed(bytes, "01", ms(1));
    REQUIRE_EQ(from_stream.size(), std::size_t{1});
    REQUIRE_EQ(from_stream[0].signal, no_index);
    REQUIRE_EQ(from_stream[0].signal_bytes, std::size_t{0});
}

TEST_CASE("rules: machine counts and ends a rule on a signal like any other")
{
    machine each(make_set(peer_set(
                     R"j({"name": "once", "when": {"signal": "alpha"},
                          "do": [{"mark": "once"}], "count": 1},
                         {"name": "then", "when": {"signal": "alpha"},
                          "sequence": [{"do": [{"mark": "s0"}]},
                                       {"do": [{"mark": "s1"}]}],
                          "then": "stop"})j",
                     signal_head)),
                 1);
    const auto one = raise(each, 0, "", us(1), us(2));
    REQUIRE_EQ(one.size(), std::size_t{1});
    REQUIRE_EQ(one[0].rule, std::size_t{0});
    // The first is used up, so the second is the first that is left.
    const auto two = raise(each, 0, "", us(3), us(4));
    REQUIRE_EQ(two.size(), std::size_t{1});
    REQUIRE_EQ(two[0].rule, std::size_t{1});
    REQUIRE_EQ(two[0].step, std::size_t{0});
    const auto three = raise(each, 0, "", us(5), us(6));
    REQUIRE_EQ(three.size(), std::size_t{1});
    REQUIRE_EQ(three[0].step, std::size_t{1});
    REQUIRE_EQ(three[0].ordinal, std::uint64_t{2});
    REQUIRE(raise(each, 0, "", us(7), us(8)).empty());
}

TEST_CASE("rules: machine takes the bytes of a reply from what the signal "
          "carried")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"signal": "alpha"},
                          "do": [{"reply": ["01", {"match": [1, 2]}, "02"]},
                                 {"reply": [{"match": [0, 3]},
                                            {"sum8": "reply"}]}]})j",
                     signal_head)),
                 1);
    const auto got = raise(each, 0, "aa bb cc dd", us(1), us(2));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].actions.size(), std::size_t{2});
    REQUIRE(got[0].actions[0].bytes == hex("01 bb cc 02"));
    // aa + bb + cc + dd = 0x30e, and the check is one byte.
    REQUIRE(got[0].actions[1].bytes == hex("aa bb cc dd 0e"));
    REQUIRE_EQ(got[0].signal_bytes, std::size_t{4});
    REQUIRE_EQ(each.stats().replies, std::uint64_t{2});
}

TEST_CASE("rules: machine adds nothing for a range past what the signal "
          "carried")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"signal": "alpha"},
                          "do": [{"reply": ["ee", {"match": [0, 5]}, "ff"]},
                                 {"reply": ["ee", {"match": [1, 1]}, "ff"]},
                                 {"set_slot": {"slot": "s", "match": [0, 5]}},
                                 {"set_slot": {"slot": "t", "match": [1, 1]}}]},
                         {"name": "b", "when": {"signal": "beta"},
                          "do": [{"reply": ["ee", {"match": [0, 0]}, "ff"]}]})j",
                     std::string(signal_head) +
                         R"j("slots": {"s": "99", "t": "77"}, )j")),
                 1);
    const auto got = raise(each, 0, "aa bb", us(1), us(2));
    REQUIRE_EQ(got.size(), std::size_t{1});
    // The whole range is not there, so none of it is taken.
    REQUIRE(got[0].actions[0].bytes == hex("ee ff"));
    // A range that is there is taken.
    REQUIRE(got[0].actions[1].bytes == hex("ee bb ff"));
    // The slot keeps what it had when the range is not all there.
    REQUIRE(bytes_equal(each.slot(0), hex("99")));
    REQUIRE(bytes_equal(each.slot(1), hex("bb")));

    // A signal that carried nothing.
    const auto empty = raise(each, 1, "", us(3), us(4));
    REQUIRE_EQ(empty.size(), std::size_t{1});
    REQUIRE(empty[0].actions[0].bytes == hex("ee ff"));
    REQUIRE_EQ(empty[0].signal_bytes, std::size_t{0});
}

TEST_CASE("rules: machine cuts what a signal carried at 256 bytes")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"signal": "alpha"},
                          "do": [{"reply": [{"match": [0, 255]}]},
                                 {"set_slot": {"slot": "s",
                                               "match": [250, 255]}}]})j",
                     std::string(signal_head) + R"j("slots": {"s": ""}, )j")),
                 1);
    byte_buffer carried;
    for (std::size_t i = 0; i < 300; ++i)
        carried.push_back(static_cast<byte>(i));

    std::vector<firing> got;
    each.signal(0, carried, us(1), us(2), got);
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].signal_bytes, max_signal_bytes);
    REQUIRE_EQ(got[0].actions[0].bytes.size(), max_signal_bytes);
    REQUIRE(std::equal(got[0].actions[0].bytes.begin(),
                       got[0].actions[0].bytes.end(), carried.begin()));
    REQUIRE(bytes_equal(each.slot(0), hex("fa fb fc fd fe ff")));

    // Exactly 256 is not cut, and 255 is as it is.
    machine again(make_set(peer_set(
                      R"j({"name": "a", "when": {"signal": "alpha"},
                           "do": [{"mark": "A"}]})j",
                      signal_head)),
                  1);
    got.clear();
    again.signal(0, bytes_view(carried).first(256), us(1), us(2), got);
    REQUIRE_EQ(got[0].signal_bytes, std::size_t{256});
    got.clear();
    again.signal(0, bytes_view(carried).first(255), us(1), us(2), got);
    REQUIRE_EQ(got[0].signal_bytes, std::size_t{255});
}

TEST_CASE("rules: machine times an action from the cause or from the arrival "
          "of a signal")
{
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"signal": "alpha"},
                          "do": [{"mark": "cause", "after": "cause",
                                  "delay": {"fixed_us": 2000}},
                                 {"mark": "plain",
                                  "delay": {"fixed_us": 500}},
                                 {"mark": "end", "after": "match_end"},
                                 {"mark": "start", "after": "match_start",
                                  "delay": {"fixed_us": 7}},
                                 {"mark": "no delay", "after": "cause"}]})j",
                     std::string(signal_head) +
                         R"j("order": "independent", )j")),
                 1);
    // The cause is long before the arrival: the action timed from it is due
    // already, and the engine finds that out and says so.
    const auto got = raise(each, 0, "", us(1000), us(9000));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].actions.size(), std::size_t{5});
    REQUIRE(got[0].actions[0].due == us(3000));
    REQUIRE(got[0].actions[0].drawn == 2000us);
    REQUIRE(got[0].actions[1].due == us(9500));
    REQUIRE(got[0].actions[2].due == us(9000));
    REQUIRE(got[0].actions[3].due == us(9007));
    REQUIRE(got[0].actions[4].due == us(1000));
    REQUIRE(got[0].cause == us(1000));
    REQUIRE(got[0].start == us(9000));
    REQUIRE(got[0].end == us(9000));

    // The action before it, for the edge "previous", is the one from the
    // cause.
    machine chained(make_set(peer_set(
                        R"j({"name": "a", "when": {"signal": "alpha"},
                             "do": [{"mark": "one", "after": "cause",
                                     "delay": {"fixed_us": 100}},
                                    {"mark": "two", "after": "previous",
                                     "delay": {"fixed_us": 50}}]})j",
                        signal_head)),
                    1);
    const auto both = raise(chained, 0, "", us(1000), us(9000));
    REQUIRE(both[0].actions[0].due == us(1100));
    REQUIRE(both[0].actions[1].due == us(1150));
}

TEST_CASE("rules: machine holds a reply to a signal behind an earlier one with "
          "fifo")
{
    // The cause is older than the arrival, but a later action is not due
    // before an earlier one.
    machine each(make_set(peer_set(
                     R"j({"name": "a", "when": {"signal": "alpha"},
                          "do": [{"mark": "late", "delay": {"fixed_us": 1000}},
                                 {"mark": "early", "after": "cause",
                                  "delay": {"fixed_us": 10}}]})j",
                     signal_head)),
                 1);
    const auto got = raise(each, 0, "", us(100), us(5000));
    REQUIRE(got[0].actions[0].due == us(6000));
    REQUIRE(got[0].actions[1].due == us(6000));
}

TEST_CASE("rules: machine uses up no bytes of the stream for a signal")
{
    machine each(make_set(peer_set(
                     R"j({"name": "p", "when": {"bytes": "01 02 03"},
                          "do": [{"mark": "P"}]},
                         {"name": "a", "when": {"signal": "alpha"},
                          "do": [{"mark": "A"}]})j",
                     signal_head)),
                 1);
    REQUIRE(feed(each, "01 02", ms(10)).empty());
    const auto signalled = raise(each, 0, "01 02 03", us(1), ms(11));
    REQUIRE_EQ(signalled.size(), std::size_t{1});
    REQUIRE_EQ(signalled[0].rule, std::size_t{1});
    // The pattern half-arrived before the signal still completes after it,
    // and the bytes the signal carried were no part of the stream.
    const auto got = feed(each, "03", ms(12));
    REQUIRE_EQ(got.size(), std::size_t{1});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE_EQ(got[0].offset, std::uint64_t{0});
    REQUIRE_EQ(got[0].length, std::size_t{3});
    REQUIRE(got[0].start == ms(10));
    REQUIRE(got[0].end == ms(12));
    REQUIRE_EQ(each.stats().bytes_seen, std::uint64_t{3});
    REQUIRE_EQ(each.stats().bytes_matched, std::uint64_t{3});
    REQUIRE_EQ(each.stats().firings, std::uint64_t{2});

    // The offset of the stream goes on from where it was.
    const auto next = feed(each, "01 02 03", ms(20));
    REQUIRE_EQ(next.size(), std::size_t{1});
    REQUIRE_EQ(next[0].offset, std::uint64_t{3});
}

TEST_CASE("rules: machine leaves a gap rule waiting and a silence running when "
          "a signal comes")
{
    machine each(make_set(peer_set(
                     R"j({"name": "g", "when": {"bytes": "01 02",
                                                "gap_us": 5000},
                          "do": [{"mark": "G"}]},
                         {"name": "q", "when": {"quiet_us": 8000},
                          "do": [{"mark": "Q"}]},
                         {"name": "a", "when": {"signal": "alpha"},
                          "do": [{"mark": "A"}]})j",
                     signal_head)),
                 1);
    // A signal is not a byte: before any, nothing is counted.
    REQUIRE_EQ(raise(each, 0, "", us(1), ms(1)).size(), std::size_t{1});
    REQUIRE(!each.next_deadline().has_value());

    REQUIRE(feed(each, "01 02", ms(10)).empty());
    REQUIRE(each.next_deadline() == ms(15));
    REQUIRE_EQ(raise(each, 0, "", ms(11), ms(12)).size(), std::size_t{1});
    REQUIRE(each.next_deadline() == ms(15));
    const auto got = tick(each, ms(100));
    REQUIRE_EQ(got.size(), std::size_t{2});
    REQUIRE_EQ(got[0].rule, std::size_t{0});
    REQUIRE_EQ(got[1].rule, std::size_t{1});
    REQUIRE(got[1].end == ms(18));
}

TEST_CASE("rules: machine lets a signal rule change the state, and the signal "
          "follows it")
{
    machine each(make_set(peer_set(
                     R"j({"name": "arm", "when": {"signal": "alpha",
                                                  "state": "idle"},
                          "do": [{"set_state": "armed"}]},
                         {"name": "fire", "when": {"signal": "alpha",
                                                   "state": "armed"},
                          "do": [{"mark": "fired"}, {"set_state": "idle"}]})j",
                     signal_head)),
                 1);
    const auto first = raise(each, 0, "", us(1), us(2));
    REQUIRE_EQ(first.size(), std::size_t{1});
    REQUIRE_EQ(first[0].rule, std::size_t{0});
    REQUIRE_EQ(each.state(), std::size_t{1});
    const auto second = raise(each, 0, "", us(3), us(4));
    REQUIRE_EQ(second.size(), std::size_t{1});
    REQUIRE_EQ(second[0].rule, std::size_t{1});
    REQUIRE_EQ(each.state(), std::size_t{0});
    REQUIRE_EQ(raise(each, 0, "", us(5), us(6))[0].rule, std::size_t{0});
}
