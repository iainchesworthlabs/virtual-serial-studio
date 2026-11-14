//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rule sets on a studio's ports: what install_rules accepts and refuses,
// what an installed set does to the port, and how it is taken off again.  The
// studio runs on a clock the case moves by hand wherever a time is asserted,
// and a mock fixture is the device: what it "says" (studio::inject_data) is
// what the program on the port wrote, and what the set sends is what the
// hub writes to the fixture, which shows as a `tx` data event.
//

#include "support/test_support.hpp"

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/error.hpp>
#include <vss/core/event.hpp>
#include <vss/rules/parse.hpp>
#include <vss/studio/studio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

/// The hub's clock, moved by the test.  Copies share the one time.
class hand_clock
{
public:
    time_point now() const
    {
        return time_point{} + 1h + std::chrono::nanoseconds(ticks_->load());
    }

    void advance(std::chrono::nanoseconds by) const
    {
        ticks_->fetch_add(by.count());
    }

private:
    std::shared_ptr<std::atomic<std::int64_t>> ticks_ =
        std::make_shared<std::atomic<std::int64_t>>(0);
};

studio_options options_on(const hand_clock& clock)
{
    studio_options options;
    options.hub.clock = [clock] { return clock.now(); };
    return options;
}

std::shared_ptr<const rules::compiled_rule_set> make_set(std::string_view text)
{
    std::vector<rules::problem> problems;
    const auto set = rules::compile_rule_set(text, &problems);
    if (!set)
    {
        std::string why = "rejected:";
        for (const auto& each : problems)
            why += "\n  " + each.path + ": " + each.message;
        throw vss_test::failure(why);
    }
    return *set;
}

/// A peer set on `port`.  `extra` is more members of the document, each
/// followed by a comma; `rules_json` is the array of rules.
std::string peer_set(std::string_view rules_json, std::string_view extra = "",
                     std::string_view port = "device")
{
    std::string text = R"j({"version": 1, "name": "device", )j";
    text += R"j("attach": {"role": "peer", "port": ")j";
    text += port;
    text += R"j("}, )j";
    text += extra;
    text += R"j("rules": )j";
    text += rules_json;
    text += '}';
    return text;
}

std::string raw(std::string_view hex_text)
{
    const auto parsed = from_hex(hex_text);
    REQUIRE(parsed);
    return to_string(bytes_view(*parsed));
}

const char* const reply_after_5ms = R"j([
  {"name": "answer", "when": {"bytes": "01 02"},
   "do": [{"reply": ["aa bb"], "delay": {"fixed_us": 5000}}]}
])j";

/// A set has to have a rule or a timeline; this one never matches.
const char* const idle_rules = R"j([
  {"name": "idle", "when": {"bytes": "ff"}, "do": [{"mark": "idle"}]}
])j";

/// One write the hub made to a port, as its `tx` data event says.
struct sent_chunk
{
    time_point when;
    std::string data;
};

std::vector<sent_chunk> sent_to(const event_recorder& events,
                                const std::string& port)
{
    std::vector<sent_chunk> out;
    for (const auto& event : events.snapshot())
    {
        if (event.kind == event_kind::data && event.name == port &&
            event.direction == data_direction::tx && event.data)
            out.push_back({event.when, to_string(bytes_view(*event.data))});
    }
    return out;
}

std::string sent_text(const event_recorder& events, const std::string& port)
{
    std::string joined;
    for (const auto& chunk : sent_to(events, port))
        joined += chunk.data;
    return joined;
}

/// Events of `kind` on `port` whose text is `message`.
std::size_t messages(const event_recorder& events, const std::string& port,
                     const std::string& message)
{
    std::size_t found = 0;
    for (const auto& event : events.snapshot())
    {
        if (event.kind == event_kind::log_message && event.name == port &&
            event.message == message)
            ++found;
    }
    return found;
}

/// A studio on a clock moved by hand, with a mock fixture called "device" and
/// a recorder of every event.
struct studio_bench
{
    hand_clock clock;
    studio app{options_on(clock)};
    event_recorder events;
    std::shared_ptr<endpoint> device;
    endpoint_id id = 0;

    studio_bench()
    {
        const auto created = app.create_mock_port("device");
        REQUIRE(created);
        device = *created;
        id = device->id();
        app.engine().subscribe({}, [recorder = events](const endpoint_event& e)
                               { recorder.record(e); });
    }

    result<rules_installation>
    install(std::string_view text, const rules_install_options& options = {},
            std::vector<rules::problem>* problems = nullptr)
    {
        return app.install_rules(make_set(text), options, problems);
    }

    /// The fixture says `hex_text`, and this returns once the installed set
    /// has been shown the bytes, so that the clock may be moved.
    void speak(std::string_view hex_text)
    {
        const std::string data = raw(hex_text);
        const auto before = app.rule_stats(id);
        REQUIRE(before);
        const std::uint64_t want = before->machine.bytes_seen + data.size();
        REQUIRE(app.inject_data(id, as_bytes(data)));
        REQUIRE(wait_until(
            [&]
            {
                const auto stats = app.rule_stats(id);
                return stats && stats->machine.bytes_seen >= want;
            },
            5000ms));
    }

    std::string sent() const { return sent_text(events, "device"); }

    void wait_sent(std::string_view hex_text) const
    {
        const std::string want = raw(hex_text);
        REQUIRE(wait_until([&] { return sent() == want; }, 5000ms));
    }

    /// Nothing was written to the port beyond `hex_text`.
    void require_quiet(std::string_view hex_text) const
    {
        std::this_thread::sleep_for(30ms);
        REQUIRE_EQ(sent(), raw(hex_text));
    }
};

/// Makes the fixture "device" and returns its id, holding nothing of it.
endpoint_id make_fixture(studio& app)
{
    const auto created = app.create_mock_port("device");
    REQUIRE(created);
    return (*created)->id();
}

const auto not_found = make_error_code(errc::not_found);
const auto already_exists = make_error_code(errc::already_exists);
const auto unsupported = make_error_code(errc::unsupported);
const auto invalid_argument = make_error_code(errc::invalid_argument);

} // namespace

// -- installing and what comes of it ----------------------------------------

TEST_CASE("studio rules: a set answers what the fixture says, by a write to "
          "the port at the due time")
{
    studio_bench b;
    std::vector<rules::problem> problems;
    const auto installed = b.install(peer_set(reply_after_5ms),
                                     rules_install_options{}, &problems);
    REQUIRE(installed);
    REQUIRE(problems.empty());
    REQUIRE_EQ(installed->place, b.id);

    const time_point read_at = b.clock.now();
    b.speak("01 02");
    // Real time passing does nothing, nor does a move short of the delay.
    b.require_quiet("");
    b.clock.advance(4999us);
    b.require_quiet("");

    b.clock.advance(1us);
    b.wait_sent("aa bb");
    const auto writes = sent_to(b.events, "device");
    REQUIRE_EQ(writes.size(), std::size_t{1});
    REQUIRE(writes[0].when == read_at + 5ms);

    // A byte that is not part of a match is not answered.
    b.speak("01 03");
    b.clock.advance(1s);
    b.require_quiet("aa bb");

    const auto stats = b.app.rule_stats(b.id);
    REQUIRE(stats);
    REQUIRE_EQ(stats->done, std::uint64_t{1});
    REQUIRE_EQ(stats->machine.bytes_seen, std::uint64_t{4});
    REQUIRE_EQ(stats->machine.firings, std::uint64_t{1});
}

TEST_CASE("studio rules: an installation says the place, the generation and "
          "the seed")
{
    studio app;
    for (const char* name : {"one", "two", "three"})
        REQUIRE(app.create_mock_port(name));
    const auto one = app.find("one")->id();
    const auto two = app.find("two")->id();
    const auto three = app.find("three")->id();

    // The set's own seed.
    rules_install_options options;
    options.run_seed = 99;
    const auto first = app.install_rules(
        make_set(peer_set(idle_rules, R"j("seed": 7, )j", "one")), options);
    REQUIRE(first);
    REQUIRE_EQ(first->place, one);
    REQUIRE_EQ(first->generation, std::uint64_t{1});
    REQUIRE_EQ(first->seed, std::uint64_t{7});

    // None of its own: the run's.
    const auto second =
        app.install_rules(make_set(peer_set(idle_rules, "", "two")), options);
    REQUIRE(second);
    REQUIRE_EQ(second->place, two);
    REQUIRE_EQ(second->seed, std::uint64_t{99});
    REQUIRE(second->generation > first->generation);

    // And without options, the run's seed is zero.
    const auto third =
        app.install_rules(make_set(peer_set(idle_rules, "", "three")));
    REQUIRE(third);
    REQUIRE_EQ(third->place, three);
    REQUIRE_EQ(third->seed, std::uint64_t{0});
    REQUIRE(third->generation > second->generation);

    REQUIRE(app.rules_generation(one) == first->generation);
    REQUIRE(app.rules_generation(two) == second->generation);
    REQUIRE(app.rules_generation(three) == third->generation);
    REQUIRE(!app.rules_generation(endpoint_id{4242}).has_value());

    // Each generation is new, also for a set that replaces one that was
    // removed.
    REQUIRE(app.remove_rules(one));
    REQUIRE(!app.rules_generation(one).has_value());
    const auto again = app.install_rules(
        make_set(peer_set(idle_rules, R"j("seed": 7, )j", "one")));
    REQUIRE(again);
    REQUIRE(again->generation > third->generation);
    REQUIRE(app.rules_generation(one) == again->generation);
}

TEST_CASE("studio rules: a port has one set, and a port that is not there has "
          "none")
{
    studio_bench b;
    const auto first = b.install(peer_set(reply_after_5ms));
    REQUIRE(first);

    std::vector<rules::problem> problems;
    const auto second = b.install(peer_set(idle_rules), {}, &problems);
    REQUIRE(!second);
    REQUIRE_EQ(second.error(), already_exists);
    REQUIRE_EQ(problems.size(), std::size_t{1});
    REQUIRE_EQ(problems[0].path, std::string("attach.port"));
    // The set that was there is as it was.
    REQUIRE(b.app.rules_generation(b.id) == first->generation);

    problems.clear();
    const auto none =
        b.install(peer_set(idle_rules, "", "nowhere"), {}, &problems);
    REQUIRE(!none);
    REQUIRE_EQ(none.error(), not_found);
    REQUIRE_EQ(problems.size(), std::size_t{1});
    REQUIRE_EQ(problems[0].path, std::string("attach.port"));

    // A refusal leaves `problems` empty when the set is installed.
    studio_bench other;
    problems.push_back(rules::problem{"left", "over"});
    REQUIRE(other.install(peer_set(idle_rules), {}, &problems));
    REQUIRE(problems.empty());

    REQUIRE_EQ(b.app.install_rules(nullptr).error(), invalid_argument);
}

// -- what this build refuses -------------------------------------------------

namespace
{

/// `install_rules` must refuse `text` as `unsupported`, install nothing on
/// `port`, and name `expected` as the paths of the problems, in that order.
void require_refused(studio& app, std::string_view port, std::string_view text,
                     const std::vector<std::string>& expected)
{
    std::vector<rules::problem> problems;
    const auto installed =
        app.install_rules(make_set(text), rules_install_options{}, &problems);
    REQUIRE(!installed);
    REQUIRE_EQ(installed.error(), unsupported);

    std::string found;
    for (const auto& problem : problems)
    {
        REQUIRE(!problem.message.empty());
        found += problem.path + '\n';
    }
    std::string wanted;
    for (const auto& path : expected)
        wanted += path + '\n';
    REQUIRE_EQ(found, wanted);

    const auto place = app.find(port);
    REQUIRE(place != nullptr);
    REQUIRE(!app.rules_generation(place->id()).has_value());
    REQUIRE_EQ(app.rule_stats(place->id()).error(), not_found);
}

/// A rule of one step on `when`, doing `action`.
std::string one_step(std::string_view when, std::string_view action)
{
    std::string text = R"j([{"name": "r", "when": )j";
    text += when;
    text += R"j(, "sequence": [{"do": [)j";
    text += action;
    text += "]}]}]";
    return text;
}

const char* const on_bytes = R"j({"bytes": "01"})j";

} // namespace

TEST_CASE("studio rules: a set for another role is refused")
{
    studio app;
    REQUIRE(app.create_mock_port("device"));
    REQUIRE(app.create_mock_port("other"));

    // The role is the first thing looked at: the port may be there or not.
    require_refused(app, "device",
                    R"j({"attach": {"role": "station", "port": "device"},
                         "rules": [{"name": "r", "when": {"bytes": "01"},
                                    "do": [{"reply": ["aa"]}]}]})j",
                    {"attach.role"});
    require_refused(app, "device",
                    R"j({"attach": {"role": "station", "port": "nowhere"},
                         "rules": [{"name": "r", "when": {"bytes": "01"},
                                    "do": [{"mark": "x"}]}]})j",
                    {"attach.role"});
    require_refused(app, "device",
                    R"j({"attach": {"role": "inline", "from": "device",
                                    "to": "other"},
                         "rules": [{"name": "r", "when": {"bytes": "01"},
                                    "do": [{"mark": "x"}]}]})j",
                    {"attach.role"});
}

TEST_CASE("studio rules: a rule on lines or the port is refused")
{
    studio app;
    REQUIRE(app.create_mock_port("device"));
    const std::string mark = R"j({"mark": "x"})j";

    require_refused(app, "device",
                    peer_set(one_step(R"j({"lines": {"dtr": true}})j", mark)),
                    {"rules[0].when.lines"});
    require_refused(app, "device",
                    peer_set(one_step(R"j({"port": "opened"})j", mark)),
                    {"rules[0].when.port"});

    // The third rule of the set is the one named, and the rule on silence
    // before it is not refused.
    const std::string three_rules = R"j([
      {"name": "fine", "when": {"bytes": "01"}, "do": [{"mark": "x"}]},
      {"name": "quiet", "when": {"quiet_us": 5000}, "do": [{"mark": "y"}]},
      {"name": "edge", "when": {"lines": {"rts": true}},
       "do": [{"mark": "z"}]}
    ])j";
    require_refused(app, "device", peer_set(three_rules),
                    {"rules[2].when.lines"});
}

TEST_CASE("studio rules: an ask and a line a device does not drive are "
          "refused")
{
    studio app;
    REQUIRE(app.create_mock_port("device"));
    const std::string ask = R"j({"ask": {"timeout_us": 5000}})j";

    require_refused(app, "device", peer_set(one_step(on_bytes, ask)),
                    {"rules[0].sequence[0].do[0]"});
    // The second action of the list.
    require_refused(app, "device",
                    peer_set(one_step(on_bytes, R"j({"mark": "x"}, )j" + ask)),
                    {"rules[0].sequence[0].do[1]"});
    // A rule written with a plain list is named as its writer wrote it.
    require_refused(app, "device",
                    peer_set(R"j([{"name": "r", "when": {"bytes": "01"},
                                   "do": [{"mark": "x"},
                                          {"ask": {"timeout_us": 5000}}]}])j"),
                    {"rules[0].do[1]"});

    // A device drives CTS, DSR, DCD and RI.  DTR, RTS and BREAK are the
    // program's.
    for (const char* line : {"dtr", "rts", "break"})
    {
        const std::string action =
            std::string(R"j({"lines": {")j") + line + R"j(": true}})j";
        require_refused(app, "device", peer_set(one_step(on_bytes, action)),
                        {"rules[0].sequence[0].do[0].lines"});
    }
    // Each line that is not the device's is named, and the others are not.
    require_refused(
        app, "device",
        peer_set(one_step(
            on_bytes,
            R"j({"lines": {"dcd": true, "dtr": true, "rts": false}})j")),
        {"rules[0].sequence[0].do[0].lines",
         "rules[0].sequence[0].do[0].lines"});
}

TEST_CASE("studio rules: the timeline is checked as the rules are, and every "
          "problem is reported")
{
    studio app;
    REQUIRE(app.create_mock_port("device"));

    require_refused(app, "device", peer_set("[]", R"j("timeline": [
                      {"at_us": 1000, "do": [{"mark": "x"}]},
                      {"at_us": 2000, "do": [{"lines": {"dtr": true}}]}
                    ], )j"),
                    {"timeline[1].do[0].lines"});

    // Rules first, in the order listed, and then the timeline.
    const std::string rules_text = R"j([
      {"name": "edge", "when": {"lines": {"dtr": true}},
       "do": [{"mark": "x"}]},
      {"name": "ask", "when": {"bytes": "01"},
       "sequence": [{"do": [{"mark": "y"}]},
                    {"do": [{"ask": {"timeout_us": 5000}}]}]}
    ])j";
    require_refused(app, "device", peer_set(rules_text, R"j("timeline": [
                      {"at_us": 1000, "do": [{"lines": {"rts": true}}]}
                    ], )j"),
                    {"rules[0].when.lines", "rules[1].sequence[1].do[0]",
                     "timeline[0].do[0].lines"});
}

TEST_CASE("studio rules: a line error is refused on a port that takes none")
{
    studio app;
    // Neither a fixture nor a driver port: no fault applies to it.
    REQUIRE(app.create_virtual_port("loop", virtual_port_kind::loopback));
    REQUIRE(app.create_mock_port("device"));

    for (const char* error :
         {"parity", "framing", "overrun", "break", "hang_up", "disconnect"})
    {
        const std::string action =
            std::string(R"j({"line_error": ")j") + error + R"j("})j";
        require_refused(app, "loop",
                        peer_set(one_step(on_bytes, action), "", "loop"),
                        {"rules[0].sequence[0].do[0].line_error"});
    }
    require_refused(app, "loop",
                    peer_set("[]",
                             R"j("timeline": [
                   {"at_us": 1000, "do": [{"line_error": "break"}]}
                 ], )j",
                             "loop"),
                    {"timeline[0].do[0].line_error"});

    // The same set is fine on the fixture.
    const auto accepted = app.install_rules(make_set(peer_set(
        one_step(on_bytes, R"j({"line_error": "framing"})j"), "", "device")));
    REQUIRE(accepted);

    // A loopback port takes the rest: bytes, silence, replies, lines the
    // device drives, marks.
    const auto loop = app.install_rules(make_set(peer_set(
        R"j([
          {"name": "a", "when": {"bytes": "01"},
           "do": [{"reply": ["aa"]}, {"lines": {"dcd": true, "ri": false}},
                  {"mark": "m"}]},
          {"name": "q", "when": {"quiet_us": 5000}, "do": [{"mark": "q"}]}
        ])j",
        "", "loop")));
    REQUIRE(loop);
    REQUIRE(loop->generation > accepted->generation);
}

// -- what an installed set does ---------------------------------------------

TEST_CASE("studio rules: a set changes the lines the device drives and "
          "publishes the change")
{
    studio_bench b;
    REQUIRE(b.install(peer_set(R"j([
      {"name": "up", "when": {"bytes": "01"},
       "do": [{"lines": {"dcd": true}, "delay": {"fixed_us": 1000}}]},
      {"name": "swap", "when": {"bytes": "02"},
       "do": [{"lines": {"dcd": false, "cts": true},
               "delay": {"fixed_us": 1000}}]}
    ])j")));
    REQUIRE(!b.device->lines().dcd());
    const std::size_t before = b.events.count(event_kind::lines);

    b.speak("01");
    b.clock.advance(999us);
    std::this_thread::sleep_for(30ms);
    REQUIRE(!b.device->lines().dcd());
    b.clock.advance(1us);
    REQUIRE(wait_until([&] { return b.device->lines().dcd(); }, 5000ms));
    REQUIRE(wait_until(
        [&] { return b.events.count(event_kind::lines) > before; }, 5000ms));

    bool published = false;
    for (const auto& event : b.events.snapshot())
    {
        if (event.kind == event_kind::lines && event.name == "device" &&
            event.lines.dcd())
            published = true;
    }
    REQUIRE(published);
    REQUIRE(!b.device->lines().cts());

    // An action that sets two lines changes both, and leaves the rest.
    b.speak("02");
    b.clock.advance(1ms);
    REQUIRE(wait_until([&] { return b.device->lines().cts(); }, 5000ms));
    REQUIRE(!b.device->lines().dcd());
    REQUIRE(!b.device->lines().dsr());
    REQUIRE(!b.device->lines().ri());
    // Lines are not bytes: nothing was written to the port.
    b.require_quiet("");
}

TEST_CASE("studio rules: a mark publishes a log message with its text, from a "
          "rule and from the timeline")
{
    studio_bench b;
    const time_point started = b.clock.now();
    REQUIRE(b.install(peer_set(R"j([
      {"name": "m", "when": {"bytes": "01"},
       "do": [{"mark": "from the rule", "delay": {"fixed_us": 2000}}]}
    ])j",
                               R"j("timeline": [
                                 {"at_us": 1000,
                                  "do": [{"mark": "from the timeline"}]}
                               ], )j")));

    // The timeline counts from the installation.
    b.clock.advance(999us);
    b.require_quiet("");
    REQUIRE_EQ(messages(b.events, "device", "from the timeline"),
               std::size_t{0});
    b.clock.advance(1us);
    REQUIRE(wait_until(
        [&] { return messages(b.events, "device", "from the timeline") == 1; },
        5000ms));
    for (const auto& event : b.events.snapshot())
    {
        if (event.kind == event_kind::log_message &&
            event.message == "from the timeline")
        {
            REQUIRE_EQ(event.endpoint, b.id);
            REQUIRE(event.when == started + 1000us);
        }
    }

    b.speak("01");
    b.clock.advance(1999us);
    b.require_quiet("");
    REQUIRE_EQ(messages(b.events, "device", "from the rule"), std::size_t{0});
    b.clock.advance(1us);
    REQUIRE(wait_until(
        [&] { return messages(b.events, "device", "from the rule") == 1; },
        5000ms));
    // A mark does nothing else.
    b.require_quiet("");
    REQUIRE_EQ(b.app.rule_stats(b.id)->done, std::uint64_t{2});
}

TEST_CASE("studio rules: a line error reaches the fixture, as inject_fault "
          "does")
{
    for (const char* error :
         {"parity", "framing", "overrun", "break", "hang_up", "disconnect"})
    {
        studio_bench b;
        REQUIRE(b.install(peer_set(
            one_step(on_bytes, std::string(R"j({"line_error": ")j") + error +
                                   R"j(", "delay": {"fixed_us": 3000}})j"))));
        REQUIRE_EQ(b.device->state(), endpoint_state::open);

        b.speak("01");
        b.clock.advance(2999us);
        std::this_thread::sleep_for(30ms);
        REQUIRE_EQ(b.device->state(), endpoint_state::open);

        b.clock.advance(1us);
        REQUIRE(wait_until(
            [&] { return b.device->state() != endpoint_state::open; }, 5000ms));
    }
}

TEST_CASE("studio rules: the counters of a set follow its traffic")
{
    studio_bench b;
    REQUIRE_EQ(b.app.rule_stats(b.id).error(), not_found);
    REQUIRE_EQ(b.app.rule_stats(endpoint_id{4242}).error(), not_found);

    REQUIRE(b.install(peer_set(R"j([
      {"name": "reply", "when": {"bytes": "01"},
       "do": [{"reply": ["aa bb cc"], "delay": {"fixed_us": 1000}}]},
      {"name": "lost", "when": {"bytes": "02"},
       "do": [{"reply": ["dd"], "fault": "drop"}]},
      {"name": "note", "when": {"bytes": "03"},
       "do": [{"mark": "n", "delay": {"fixed_us": 1000}}]}
    ])j")));
    auto stats = b.app.rule_stats(b.id);
    REQUIRE(stats);
    REQUIRE_EQ(stats->scheduled, std::uint64_t{0});

    b.speak("01");
    b.speak("02");
    b.speak("03");
    b.speak("04 01");
    stats = b.app.rule_stats(b.id);
    REQUIRE_EQ(stats->machine.bytes_seen, std::uint64_t{5});
    REQUIRE_EQ(stats->machine.firings, std::uint64_t{4});
    REQUIRE_EQ(stats->scheduled, std::uint64_t{4});
    // The clock has not moved, and an action is not due before the reply
    // to an earlier match.
    REQUIRE_EQ(stats->done, std::uint64_t{0});

    b.clock.advance(1ms);
    REQUIRE(
        wait_until([&] { return b.app.rule_stats(b.id)->done == 4; }, 5000ms));
    stats = b.app.rule_stats(b.id);
    REQUIRE_EQ(stats->machine.replies, std::uint64_t{3});
    REQUIRE_EQ(stats->machine.reply_bytes, std::uint64_t{6});
    REQUIRE_EQ(stats->machine.faults, std::uint64_t{1});
    REQUIRE_EQ(stats->late, std::uint64_t{0});
    REQUIRE_EQ(stats->overrun, std::uint64_t{0});
    REQUIRE_EQ(stats->cancelled, std::uint64_t{0});
    b.wait_sent("aa bb cc aa bb cc");
}

// -- taking a set off --------------------------------------------------------

TEST_CASE("studio rules: removing a set discards what it holds and a new set "
          "can take its place")
{
    studio_bench b;
    REQUIRE_EQ(b.app.remove_rules(b.id), not_found);
    REQUIRE_EQ(b.app.remove_rules(endpoint_id{4242}), not_found);

    const auto first = b.install(peer_set(reply_after_5ms));
    REQUIRE(first);
    b.speak("01 02");
    REQUIRE_EQ(b.app.rule_stats(b.id)->scheduled, std::uint64_t{1});

    REQUIRE(b.app.remove_rules(b.id));
    REQUIRE(!b.app.rules_generation(b.id).has_value());
    REQUIRE_EQ(b.app.rule_stats(b.id).error(), not_found);
    REQUIRE_EQ(b.app.remove_rules(b.id), not_found);

    // The reply that was promised is not written, however far the clock
    // moves, and the port no longer answers.
    b.clock.advance(1s);
    b.require_quiet("");
    REQUIRE(b.app.inject_data(b.id, as_bytes(raw("01 02"))));
    REQUIRE(
        wait_until([&] { return b.device->stats().bytes_rx == 4; }, 5000ms));
    b.clock.advance(1s);
    b.require_quiet("");

    // A second set on the same port is a new generation, and answers once.
    const auto second = b.install(peer_set(reply_after_5ms));
    REQUIRE(second);
    REQUIRE(second->generation > first->generation);
    b.speak("01 02");
    b.clock.advance(5ms);
    b.wait_sent("aa bb");
    b.require_quiet("aa bb");
    REQUIRE_EQ(b.app.rule_stats(b.id)->done, std::uint64_t{1});
}

TEST_CASE("studio rules: removing a port removes its set")
{
    studio_bench b;
    REQUIRE(b.install(peer_set(reply_after_5ms)));
    b.speak("01 02");
    const endpoint_id old_id = b.id;

    REQUIRE(b.app.remove_port(old_id));
    REQUIRE(!b.app.rules_generation(old_id).has_value());
    REQUIRE_EQ(b.app.rule_stats(old_id).error(), not_found);
    REQUIRE_EQ(b.app.remove_rules(old_id), not_found);
    b.clock.advance(1s);
    b.require_quiet("");

    // A port made again with the name takes a set again.
    const auto again = b.app.create_mock_port("device");
    REQUIRE(again);
    REQUIRE_EQ((*again)->name(), std::string("device"));
    b.id = (*again)->id();
    REQUIRE_NE(b.id, old_id);
    const auto installed = b.install(peer_set(reply_after_5ms));
    REQUIRE(installed);
    REQUIRE_EQ(installed->place, b.id);

    b.speak("01 02");
    b.clock.advance(5ms);
    b.wait_sent("aa bb");
}

TEST_CASE("studio rules: a studio that goes with replies due far off "
          "neither crashes nor hangs")
{
    auto app = std::make_unique<studio>();
    // Only the id is kept: a port held past the studio would outlive its hub.
    const endpoint_id id = make_fixture(*app);
    REQUIRE(app->install_rules(make_set(peer_set(R"j([
      {"name": "far", "when": {"bytes": "01"},
       "do": [{"reply": ["aa"], "delay": {"fixed_us": 30000000}}]}
    ])j"))));
    for (int round = 0; round < 50; ++round)
        REQUIRE(app->inject_data(id, as_bytes(raw("01"))));
    REQUIRE(wait_until(
        [&]
        {
            const auto stats = app->rule_stats(id);
            return stats && stats->scheduled == 50;
        },
        5000ms));
    app.reset();
}

TEST_CASE("studio rules: a studio that goes while replies are being written "
          "neither crashes nor hangs")
{
    auto app = std::make_unique<studio>();
    // Only the id is kept: a port held past the studio would outlive its hub.
    const endpoint_id id = make_fixture(*app);
    REQUIRE(app->install_rules(make_set(peer_set(R"j([
      {"name": "now", "when": {"bytes": "01"}, "do": [{"reply": ["aa"]}]}
    ])j"))));
    for (int round = 0; round < 500; ++round)
        REQUIRE(app->inject_data(id, as_bytes(raw("01"))));
    REQUIRE(wait_until(
        [&]
        {
            const auto stats = app->rule_stats(id);
            return stats && stats->done >= 5;
        },
        5000ms));
    app.reset();
}

TEST_CASE("studio rules: a studio on a clock moved by hand goes with a reply "
          "pending, without a crash or a hang")
{
    studio_bench b;
    REQUIRE(b.install(peer_set(reply_after_5ms)));
    b.speak("01 02");
}

// -- signals ----------------------------------------------------------------

namespace
{

const char* const ping_head = R"j("signals": ["ping"], )j";

const char* const reply_to_ping = R"j([
  {"name": "on ping", "when": {"signal": "ping"},
   "do": [{"reply": ["aa", {"match": [0, 1]}]}]}
])j";

const char* const mark_on_ping = R"j([
  {"name": "on ping", "when": {"signal": "ping"}, "do": [{"mark": "pinged"}]}
])j";

const char* const answer_at_once = R"j([
  {"name": "answer", "when": {"bytes": "01 02"},
   "do": [{"reply": ["aa bb"]}]}
])j";

const char* const answer_one_byte = R"j([
  {"name": "answer", "when": {"bytes": "01"}, "do": [{"reply": ["aa"]}]}
])j";

const auto queue_full = make_error_code(errc::queue_full);

/// The fixture says `hex_text` to a set that is suspended, which is not shown
/// it, and this returns once the port has read it.
void speak_to_suspended(studio_bench& b, std::string_view hex_text)
{
    const std::string data = raw(hex_text);
    const std::uint64_t want = b.device->stats().bytes_rx + data.size();
    REQUIRE(b.app.inject_data(b.id, as_bytes(data)));
    REQUIRE(
        wait_until([&] { return b.device->stats().bytes_rx >= want; }, 5000ms));
}

} // namespace

TEST_CASE("studio rules: a rule on a signal installs, and raising the signal "
          "answers with what it carried")
{
    studio_bench b;
    std::vector<rules::problem> problems;
    REQUIRE(b.install(peer_set(reply_to_ping, ping_head),
                      rules_install_options{}, &problems));
    REQUIRE(problems.empty());

    const std::string carried = raw("11 22");
    REQUIRE(b.app.raise_signal(b.id, "ping", as_bytes(carried), b.clock.now()));
    b.wait_sent("aa 11 22");
    b.require_quiet("aa 11 22");

    // A signal that carries nothing is answered with the rest of the reply.
    REQUIRE(b.app.raise_signal(b.id, "ping", bytes_view{}, b.clock.now()));
    b.wait_sent("aa 11 22 aa");

    REQUIRE(
        wait_until([&] { return b.app.rule_stats(b.id)->done == 2; }, 5000ms));
    const auto stats = b.app.rule_stats(b.id);
    REQUIRE(stats);
    REQUIRE_EQ(stats->machine.firings, std::uint64_t{2});
    REQUIRE_EQ(stats->machine.replies, std::uint64_t{2});
    REQUIRE_EQ(stats->machine.reply_bytes, std::uint64_t{4});
    // A signal is not traffic.
    REQUIRE_EQ(stats->machine.bytes_seen, std::uint64_t{0});
    REQUIRE(stats->suspended == rules::suspend_reason::none);
}

TEST_CASE("studio rules: raise_signal refuses a port with no set, a signal the "
          "set does not declare, too many bytes and a cause out of range")
{
    studio_bench b;
    const time_point now = b.clock.now();
    REQUIRE_EQ(b.app.raise_signal(b.id, "ping", bytes_view{}, now).error(),
               not_found);
    REQUIRE_EQ(b.app.raise_signal(endpoint_id{4242}, "ping", bytes_view{}, now)
                   .error(),
               not_found);

    REQUIRE(b.install(peer_set(mark_on_ping, ping_head)));
    REQUIRE_EQ(b.app.raise_signal(b.id, "pong", bytes_view{}, now).error(),
               not_found);
    REQUIRE_EQ(b.app.raise_signal(b.id, "", bytes_view{}, now).error(),
               not_found);

    const std::string too_many(rules::max_signal_bytes + 1, 'x');
    REQUIRE_EQ(
        b.app.raise_signal(b.id, "ping", as_bytes(too_many), now).error(),
        invalid_argument);
    // A cause that has not happened yet, and one that is too old.
    REQUIRE_EQ(
        b.app.raise_signal(b.id, "ping", bytes_view{}, now + 1us).error(),
        invalid_argument);
    REQUIRE_EQ(b.app.raise_signal(b.id, "ping", bytes_view{}, now + 1s).error(),
               invalid_argument);
    REQUIRE_EQ(
        b.app.raise_signal(b.id, "ping", bytes_view{}, now - 61s).error(),
        invalid_argument);
    REQUIRE_EQ(
        b.app.raise_signal(b.id, "ping", bytes_view{}, now - 60s - 1us).error(),
        invalid_argument);

    // None of them reached the set.
    b.require_quiet("");
    REQUIRE_EQ(b.app.rule_stats(b.id)->machine.firings, std::uint64_t{0});
    REQUIRE_EQ(messages(b.events, "device", "pinged"), std::size_t{0});

    // The limits themselves are allowed: 256 bytes, a cause that is now, and
    // one that is a minute old.
    const std::string most(rules::max_signal_bytes, 'x');
    REQUIRE(b.app.raise_signal(b.id, "ping", as_bytes(most), now));
    REQUIRE(b.app.raise_signal(b.id, "ping", bytes_view{}, now));
    REQUIRE(b.app.raise_signal(b.id, "ping", bytes_view{}, now - 60s));
    REQUIRE(wait_until(
        [&] { return messages(b.events, "device", "pinged") == 3; }, 5000ms));
    REQUIRE_EQ(b.app.rule_stats(b.id)->machine.firings, std::uint64_t{3});
}

TEST_CASE("studio rules: a suspended set answers nothing until it is resumed")
{
    studio_bench b;
    REQUIRE_EQ(b.app.suspend_rules(b.id), not_found);
    REQUIRE_EQ(b.app.resume_rules(b.id), not_found);
    REQUIRE_EQ(b.app.suspend_rules(endpoint_id{4242}), not_found);
    REQUIRE_EQ(b.app.resume_rules(endpoint_id{4242}), not_found);

    REQUIRE(b.install(peer_set(answer_at_once)));
    b.speak("01 02");
    b.wait_sent("aa bb");
    REQUIRE(b.app.rule_stats(b.id)->suspended == rules::suspend_reason::none);

    REQUIRE(b.app.suspend_rules(b.id));
    auto stats = b.app.rule_stats(b.id);
    REQUIRE(stats);
    REQUIRE(stats->suspended == rules::suspend_reason::asked);
    REQUIRE_EQ(stats->suspensions, std::uint64_t{1});

    // What the program writes now is not answered, and the set is not shown
    // it.
    speak_to_suspended(b, "01 02");
    b.clock.advance(1s);
    b.require_quiet("aa bb");
    stats = b.app.rule_stats(b.id);
    REQUIRE_EQ(stats->machine.bytes_seen, std::uint64_t{2});
    REQUIRE_EQ(stats->machine.firings, std::uint64_t{1});

    // Suspending a suspended set is not an error and not another suspension.
    REQUIRE(b.app.suspend_rules(b.id));
    stats = b.app.rule_stats(b.id);
    REQUIRE(stats->suspended == rules::suspend_reason::asked);
    REQUIRE_EQ(stats->suspensions, std::uint64_t{1});

    REQUIRE(b.app.resume_rules(b.id));
    REQUIRE(b.app.rule_stats(b.id)->suspended == rules::suspend_reason::none);
    // And neither is resuming one that is running.
    REQUIRE(b.app.resume_rules(b.id));
    REQUIRE(b.app.rule_stats(b.id)->suspended == rules::suspend_reason::none);

    b.speak("01 02");
    b.wait_sent("aa bb aa bb");
    b.require_quiet("aa bb aa bb");
    stats = b.app.rule_stats(b.id);
    REQUIRE_EQ(stats->suspensions, std::uint64_t{1});
    REQUIRE_EQ(stats->machine.firings, std::uint64_t{2});
}

TEST_CASE("studio rules: a set that fires faster than its budget is "
          "suspended, and resume_rules brings it back")
{
    studio_bench b;
    rules_install_options options;
    options.limits.firings_per_second = 2;
    REQUIRE(b.install(peer_set(answer_one_byte), options));

    b.speak("01");
    b.wait_sent("aa");
    b.speak("01");
    b.wait_sent("aa aa");
    REQUIRE(
        wait_until([&] { return b.app.rule_stats(b.id)->done == 2; }, 5000ms));
    REQUIRE(b.app.rule_stats(b.id)->suspended == rules::suspend_reason::none);

    // The third at the same instant finds the budget spent.
    b.speak("01");
    b.require_quiet("aa aa");
    auto stats = b.app.rule_stats(b.id);
    REQUIRE(stats);
    REQUIRE(stats->suspended == rules::suspend_reason::budget);
    REQUIRE_EQ(stats->suspensions, std::uint64_t{1});
    REQUIRE_EQ(stats->suspended_firings, std::uint64_t{1});
    REQUIRE_EQ(stats->machine.firings, std::uint64_t{3});
    REQUIRE_EQ(stats->scheduled, std::uint64_t{2});
    REQUIRE_EQ(stats->done, std::uint64_t{2});
    REQUIRE_EQ(stats->cancelled, std::uint64_t{0});

    REQUIRE(b.app.resume_rules(b.id));
    stats = b.app.rule_stats(b.id);
    REQUIRE(stats->suspended == rules::suspend_reason::none);
    b.speak("01");
    b.wait_sent("aa aa aa");
    stats = b.app.rule_stats(b.id);
    REQUIRE_EQ(stats->suspensions, std::uint64_t{1});
    REQUIRE_EQ(stats->suspended_firings, std::uint64_t{1});
}

// -- what a studio takes -----------------------------------------------------

TEST_CASE("studio rules: a studio takes no more than rules::max_hub_sets sets")
{
    studio app;
    std::vector<endpoint_id> ids;
    std::vector<std::string> names;
    for (std::size_t i = 0; i <= rules::max_hub_sets; ++i)
    {
        names.push_back("port" + std::to_string(i));
        const auto created = app.create_mock_port(names.back());
        REQUIRE(created);
        ids.push_back((*created)->id());
    }
    const auto tiny = [&](std::size_t i)
    { return make_set(peer_set(idle_rules, "", names[i])); };

    for (std::size_t i = 0; i < rules::max_hub_sets; ++i)
        REQUIRE(app.install_rules(tiny(i)));

    // One more is refused, and says why.
    std::vector<rules::problem> problems;
    const auto refused =
        app.install_rules(tiny(rules::max_hub_sets), {}, &problems);
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), queue_full);
    REQUIRE_EQ(problems.size(), std::size_t{1});
    REQUIRE(!problems[0].message.empty());
    REQUIRE(!app.rules_generation(ids[rules::max_hub_sets]).has_value());
    REQUIRE_EQ(app.rule_stats(ids[rules::max_hub_sets]).error(), not_found);

    // Taking one off makes room for one, and for no more than one.
    REQUIRE(app.remove_rules(ids[0]));
    REQUIRE(app.install_rules(tiny(rules::max_hub_sets)));
    REQUIRE_EQ(app.install_rules(tiny(0)).error(), queue_full);
    REQUIRE(!app.rules_generation(ids[0]).has_value());
}

namespace
{

/// A set on the port `port` of `rule_count` rules, each of which answers with
/// the longest reply there is.  The document may be 1 MiB, so a set compiles
/// to about half of that.
std::string heavy_set(std::string_view port, std::size_t rule_count)
{
    std::string reply;
    for (std::size_t i = 0; i < rules::max_reply_bytes; ++i)
        reply += "ab";
    std::string rules_json = "[";
    for (std::size_t i = 0; i < rule_count; ++i)
    {
        if (i != 0)
            rules_json += ", ";
        rules_json += R"j({"name": "r)j" + std::to_string(i) +
                      R"j(", "when": {"bytes": "01"}, "do": [{"reply": [")j" +
                      reply + R"j("]}]})j";
    }
    rules_json += "]";
    return peer_set(rules_json, "", port);
}

/// The names of ports whose sets are all of the same size.
std::string heavy_name(std::size_t number)
{
    return std::string("heavy") + static_cast<char>('0' + number / 10) +
           static_cast<char>('0' + number % 10);
}

} // namespace

TEST_CASE("studio rules: a studio takes no more than rules::max_hub_bytes of "
          "compiled sets")
{
    constexpr std::size_t rule_count = 120;
    const auto sample = make_set(heavy_set(heavy_name(0), rule_count));
    const std::size_t size = sample->size_bytes;
    REQUIRE(size > rules::max_compiled_bytes / 3);
    REQUIRE(size <= rules::max_compiled_bytes);
    // How many of them the studio takes, worked out from what a set is.
    const std::size_t fit = rules::max_hub_bytes / size;
    REQUIRE(fit >= 2);
    REQUIRE(fit + 2 < 100);

    studio app;
    std::vector<endpoint_id> ids;
    for (std::size_t i = 0; i <= fit; ++i)
    {
        const auto created = app.create_mock_port(heavy_name(i));
        REQUIRE(created);
        ids.push_back((*created)->id());
    }

    for (std::size_t i = 0; i < fit; ++i)
    {
        const auto set = make_set(heavy_set(heavy_name(i), rule_count));
        REQUIRE_EQ(set->size_bytes, size);
        REQUIRE(app.install_rules(set));
    }

    // The next would take the sets past the limit.
    const auto over = make_set(heavy_set(heavy_name(fit), rule_count));
    REQUIRE(over->size_bytes > rules::max_hub_bytes - fit * size);
    std::vector<rules::problem> problems;
    const auto refused = app.install_rules(over, {}, &problems);
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), queue_full);
    REQUIRE_EQ(problems.size(), std::size_t{1});
    REQUIRE(!problems[0].message.empty());
    REQUIRE(!app.rules_generation(ids[fit]).has_value());

    // One taken off leaves room for one.
    REQUIRE(app.remove_rules(ids[0]));
    REQUIRE(app.install_rules(over));
    REQUIRE(app.rules_generation(ids[fit]).has_value());
    const auto again = make_set(heavy_set(heavy_name(0), rule_count));
    problems.clear();
    REQUIRE_EQ(app.install_rules(again, {}, &problems).error(), queue_full);
    REQUIRE_EQ(problems.size(), std::size_t{1});
}
