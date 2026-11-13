//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scheduler of a rule set, on a hub: put on a mock endpoint as its read
// stage, it matches what the endpoint reads and writes its replies to the
// same endpoint when they are due.  Most cases move the hub's clock by hand,
// so what is due and what is not is written in the case.
//

#include "support/test_support.hpp"

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/core/error.hpp>
#include <vss/core/hub.hpp>
#include <vss/rules/parse.hpp>
#include <vss/rules/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace vss;
using namespace vss::rules;
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

std::shared_ptr<vss::hub> start_hub_on(const hand_clock& clock)
{
    vss::hub_options options;
    options.io_threads = 2;
    options.traffic_capacity = 256;
    options.clock = [clock] { return clock.now(); };
    auto hub = std::make_shared<vss::hub>(options);
    hub->start();
    return hub;
}

std::shared_ptr<const compiled_rule_set> make_set(std::string_view text)
{
    std::vector<problem> problems;
    const auto set = compile_rule_set(text, &problems);
    if (!set)
    {
        std::string why = "rejected:";
        for (const auto& each : problems)
            why += "\n  " + each.path + ": " + each.message;
        throw vss_test::failure(why);
    }
    return *set;
}

/// A set attached to a peer.  `extra` is more members of the document, each
/// followed by a comma.
std::string peer_set(std::string_view rules_json, std::string_view extra = "")
{
    std::string text = R"({"version": 1, "name": "device", )";
    text += R"("attach": {"role": "peer", "port": "port"}, "seed": 7, )";
    text += extra;
    text += R"("rules": )";
    text += rules_json;
    text += "}";
    return text;
}

/// What the scheduler reported, kept for the case to read.
class report_log
{
public:
    void add(const action_report& report) const
    {
        std::lock_guard lock(state_->mutex);
        state_->reports.push_back(report);
    }

    std::vector<action_report> snapshot() const
    {
        std::lock_guard lock(state_->mutex);
        return state_->reports;
    }

    std::size_t size() const
    {
        std::lock_guard lock(state_->mutex);
        return state_->reports.size();
    }

private:
    struct state
    {
        std::mutex mutex;
        std::vector<action_report> reports;
    };
    std::shared_ptr<state> state_ = std::make_shared<state>();
};

/// A scheduler for `port`, not yet started and not yet shown its chunks:
/// replies are written to the port, which the mock records, and every action
/// is logged.
std::shared_ptr<scheduler> make_scheduler(vss::hub& hub,
                                          const mock_endpoint& port,
                                          std::string_view set_text,
                                          const report_log& log,
                                          bool poll_clock)
{
    scheduler_hooks hooks;
    hooks.now = [&hub] { return hub.now(); };
    hooks.poll_clock = poll_clock;
    hooks.act = [target = std::weak_ptr<vss::endpoint>(port.endpoint)](
                    const planned_action& action)
    {
        const auto endpoint = target.lock();
        if (!endpoint || action.kind != action_kind::reply)
            return;
        for (unsigned copy = 0; copy < action.copies; ++copy)
            endpoint->write(bytes_view(action.bytes));
    };
    hooks.done = [log](const action_report& report) { log.add(report); };

    return std::make_shared<scheduler>(hub.executor(), make_set(set_text), 1,
                                       std::move(hooks));
}

/// Puts `stage` on `port` (null takes the one there off) and returns once the
/// endpoint has made the change.
void put_stage(const mock_endpoint& port, std::shared_ptr<read_stage> stage)
{
    std::promise<void> done;
    auto finished = done.get_future();
    port.endpoint->set_read_stage(std::move(stage),
                                  [&done] { done.set_value(); });
    REQUIRE(finished.wait_for(10s) == std::future_status::ready);
}

/// A started scheduler that is the read stage of `port`.
std::shared_ptr<scheduler> put_scheduler(vss::hub& hub,
                                         const mock_endpoint& port,
                                         std::string_view set_text,
                                         const report_log& log, bool poll_clock)
{
    auto made = make_scheduler(hub, port, set_text, log, poll_clock);
    made->start(hub.now());
    put_stage(port, made);
    return made;
}

const char* const reply_after_5ms = R"([
  {"name": "answer", "when": {"bytes": "01 02"},
   "do": [{"reply": ["aa bb"], "delay": {"fixed_us": 5000}}]}
])";

/// The bytes a hex text stands for, as a string.
std::string raw(std::string_view hex_text)
{
    const auto parsed = from_hex(hex_text);
    REQUIRE(parsed);
    return to_string(bytes_view(*parsed));
}

control_lines lines_of(std::initializer_list<control_line> high)
{
    control_lines out;
    for (const control_line each : high)
        out.set(each, true);
    return out;
}

/// Joins when it goes out of scope, so that a failed requirement does not
/// leave a running thread behind.
class joined_thread
{
public:
    template <class Work>
    explicit joined_thread(Work&& work) : thread_(std::forward<Work>(work))
    {
    }

    ~joined_thread() { join(); }

    joined_thread(const joined_thread&) = delete;
    joined_thread& operator=(const joined_thread&) = delete;

    void join()
    {
        if (thread_.joinable())
            thread_.join();
    }

private:
    std::thread thread_;
};

/// A hub, a mock port with a scheduler on it, and the log of what the
/// scheduler did.  On a hand-moved clock `origin` is the time the run started.
struct bench
{
    enum class clock_mode
    {
        by_hand,
        steady
    };

    hand_clock clock;
    std::shared_ptr<vss::hub> hub;
    mock_endpoint port;
    report_log log;
    std::shared_ptr<scheduler> rules;
    time_point origin;

    explicit bench(std::string_view set_text,
                   clock_mode mode = clock_mode::by_hand, unsigned threads = 2,
                   bool started = true)
        : hub(mode == clock_mode::by_hand ? start_hub_on(clock)
                                          : start_hub(threads)),
          port(add_mock(*hub, "port")), origin(hub->now())
    {
        const bool poll = mode == clock_mode::by_hand;
        if (started)
        {
            rules = put_scheduler(*hub, port, set_text, log, poll);
        }
        else
        {
            rules = make_scheduler(*hub, port, set_text, log, poll);
            put_stage(port, rules);
        }
    }

    /// Writes `data` as if the device sent it, and returns once the scheduler
    /// has been shown it: the clock has not moved since the chunk was read.
    void send_raw(const std::string& data) const
    {
        const std::uint64_t want =
            rules->stats().machine.bytes_seen + data.size();
        port.inject_read(data);
        REQUIRE(wait_until(
            [&] { return rules->stats().machine.bytes_seen >= want; }, 5000ms));
    }

    void send(std::string_view hex_text) const { send_raw(raw(hex_text)); }

    void wait_reports(std::size_t count) const
    {
        REQUIRE(wait_until([&] { return log.size() >= count; }, 5000ms));
    }

    /// Long enough for the scheduler to have looked at the clock many times.
    static void settle() { std::this_thread::sleep_for(30ms); }

    void wait_written(std::string_view hex_text) const
    {
        const std::string want = raw(hex_text);
        REQUIRE(wait_until([&] { return port.written() == want; }, 5000ms));
    }

    /// Nothing more was reported and nothing was written beyond `hex_text`.
    void require_still(std::size_t reports, std::string_view hex_text) const
    {
        settle();
        REQUIRE_EQ(log.size(), reports);
        REQUIRE_EQ(port.written(), raw(hex_text));
    }

    time_point at(microseconds from_origin) const
    {
        return origin + from_origin;
    }
};

} // namespace

TEST_CASE("rules scheduler: a reply is written when the hub's clock reaches "
          "its time, not before")
{
    const hand_clock clock;
    const auto hub = start_hub_on(clock);
    const mock_endpoint port = add_mock(*hub, "port");
    const report_log log;
    const auto rules =
        put_scheduler(*hub, port, peer_set(reply_after_5ms), log, true);

    const time_point read_at = clock.now();
    port.inject_read("\x01\x02");
    REQUIRE(wait_until([&] { return rules->waiting() == 1; }, 5000ms));

    // Real time passing does nothing, nor does a move short of the delay.
    std::this_thread::sleep_for(30ms);
    REQUIRE_EQ(port.written(), std::string());
    clock.advance(4999us);
    std::this_thread::sleep_for(20ms);
    REQUIRE_EQ(port.written(), std::string());

    clock.advance(1us);
    REQUIRE(wait_until([&] { return port.written() == "\xaa\xbb"; }, 5000ms));
    REQUIRE(wait_until([&] { return log.size() == 1; }, 5000ms));

    const auto reports = log.snapshot();
    REQUIRE_EQ(reports[0].rule, std::size_t{0});
    REQUIRE(reports[0].kind == action_kind::reply);
    REQUIRE_EQ(reports[0].bytes, std::size_t{2});
    REQUIRE(reports[0].due == read_at + 5ms);
    REQUIRE(reports[0].done == read_at + 5ms);
    REQUIRE(!reports[0].late);

    const scheduler_stats stats = rules->stats();
    REQUIRE_EQ(stats.scheduled, std::uint64_t{1});
    REQUIRE_EQ(stats.done, std::uint64_t{1});
    REQUIRE_EQ(stats.machine.firings, std::uint64_t{1});
    REQUIRE_EQ(rules->waiting(), std::size_t{0});
}

TEST_CASE("rules scheduler: on the steady clock a reply is never early")
{
    const auto hub = start_hub();
    const mock_endpoint port = add_mock(*hub, "port");
    const report_log log;
    const auto rules =
        put_scheduler(*hub, port, peer_set(reply_after_5ms), log, false);

    for (int round = 0; round < 20; ++round)
        port.inject_read("\x01\x02");
    REQUIRE(wait_until([&] { return log.size() == 20; }, 10000ms));

    for (const auto& report : log.snapshot())
        REQUIRE(report.done >= report.due);
    REQUIRE(wait_until([&] { return port.written().size() == 40; }, 5000ms));
}

TEST_CASE("rules scheduler: close discards what is waiting and nothing is "
          "written after it")
{
    const hand_clock clock;
    const auto hub = start_hub_on(clock);
    const mock_endpoint port = add_mock(*hub, "port");
    const report_log log;
    const auto rules =
        put_scheduler(*hub, port, peer_set(reply_after_5ms), log, true);

    port.inject_read("\x01\x02");
    REQUIRE(wait_until([&] { return rules->waiting() == 1; }, 5000ms));
    rules->close();
    REQUIRE_EQ(rules->waiting(), std::size_t{0});
    REQUIRE_EQ(rules->stats().cancelled, std::uint64_t{1});

    clock.advance(1s);
    port.inject_read("\x01\x02");
    REQUIRE(wait_until([&] { return port.endpoint->stats().bytes_rx == 4; },
                       5000ms));
    std::this_thread::sleep_for(30ms);
    REQUIRE_EQ(port.written(), std::string());
    REQUIRE_EQ(log.size(), std::size_t{0});
}

// -- order -------------------------------------------------------------------

TEST_CASE("rules scheduler: replies are written in the order they are due, "
          "not the order they were planned")
{
    // Planned in the order 01, 02; due in the order 02, 01.
    const char* const rules_text = R"j([
      {"name": "slow", "when": {"bytes": "01"},
       "do": [{"reply": ["aa"], "delay": {"fixed_us": 3000}}]},
      {"name": "quick", "when": {"bytes": "02"},
       "do": [{"reply": ["bb"], "delay": {"fixed_us": 1000}}]}
    ])j";

    for (const bool in_steps : {false, true})
    {
        bench b(peer_set(rules_text, R"j("order": "independent", )j"));
        b.send("01");
        b.send("02");
        REQUIRE_EQ(b.rules->waiting(), std::size_t{2});

        if (in_steps)
        {
            b.clock.advance(999us);
            b.require_still(0, "");
            b.clock.advance(1us);
            b.wait_reports(1);
            b.wait_written("bb");
            REQUIRE_EQ(b.rules->waiting(), std::size_t{1});
            b.clock.advance(1999us);
            b.require_still(1, "bb");
            b.clock.advance(1us);
        }
        else
        {
            b.clock.advance(3000us);
        }
        b.wait_reports(2);
        b.wait_written("bb aa");

        const auto reports = b.log.snapshot();
        REQUIRE_EQ(reports.size(), std::size_t{2});
        REQUIRE_EQ(reports[0].rule, std::size_t{1});
        REQUIRE(reports[0].due == b.at(1000us));
        REQUIRE(reports[0].done == (in_steps ? b.at(1000us) : b.at(3000us)));
        REQUIRE_EQ(reports[1].rule, std::size_t{0});
        REQUIRE(reports[1].due == b.at(3000us));
        REQUIRE(reports[1].done == b.at(3000us));
        REQUIRE(!reports[0].late);
        REQUIRE(!reports[1].late);
    }
}

TEST_CASE("rules scheduler: replies due at the same instant are written in "
          "the order they were planned")
{
    // The rules are listed in the opposite order to the bytes, so the order
    // of the writes is neither the order of the rules nor of the patterns.
    bench b(peer_set(R"j([
      {"name": "second", "when": {"bytes": "02"},
       "do": [{"reply": ["bb"], "delay": {"fixed_us": 2000}}]},
      {"name": "first", "when": {"bytes": "01"},
       "do": [{"reply": ["aa"], "delay": {"fixed_us": 2000}}]},
      {"name": "pair", "when": {"bytes": "03"},
       "do": [{"reply": ["c1"], "delay": {"fixed_us": 2000}},
              {"reply": ["c2"], "delay": {"fixed_us": 2000}}]}
    ])j",
                     R"j("order": "independent", )j"));
    b.send("01");
    b.send("02");
    b.send("03");
    REQUIRE_EQ(b.rules->waiting(), std::size_t{4});

    b.clock.advance(1999us);
    b.require_still(0, "");
    b.clock.advance(1us);
    b.wait_reports(4);
    b.wait_written("aa bb c1 c2");

    const auto reports = b.log.snapshot();
    const std::size_t rules_in_order[] = {1, 0, 2, 2};
    const std::size_t actions_in_order[] = {0, 0, 0, 1};
    for (std::size_t i = 0; i < 4; ++i)
    {
        REQUIRE_EQ(reports[i].rule, rules_in_order[i]);
        REQUIRE_EQ(reports[i].index, actions_in_order[i]);
        REQUIRE(reports[i].due == b.at(2000us));
        REQUIRE(reports[i].done == b.at(2000us));
    }
}

TEST_CASE("rules scheduler: with fifo a short delay does not overtake a long "
          "one from an earlier match")
{
    // "order" is not given: fifo is the default.
    bench b(peer_set(R"j([
      {"name": "long", "when": {"bytes": "01"},
       "do": [{"reply": ["aa"], "delay": {"fixed_us": 4000}}]},
      {"name": "short", "when": {"bytes": "02"},
       "do": [{"reply": ["bb"], "delay": {"fixed_us": 1000}}]}
    ])j"));
    b.send("01");
    b.clock.advance(500us);
    // On its own this one would be due at 1500 us.
    b.send("02");
    REQUIRE_EQ(b.rules->waiting(), std::size_t{2});

    b.clock.advance(1500us);
    b.require_still(0, "");
    b.clock.advance(1999us);
    b.require_still(0, "");
    b.clock.advance(1us);
    b.wait_reports(2);
    b.wait_written("aa bb");

    const auto reports = b.log.snapshot();
    REQUIRE_EQ(reports[0].rule, std::size_t{0});
    REQUIRE_EQ(reports[1].rule, std::size_t{1});
    REQUIRE(reports[0].due == b.at(4000us));
    REQUIRE(reports[1].due == b.at(4000us));
    REQUIRE(reports[1].done == b.at(4000us));
}

// -- faults ------------------------------------------------------------------

TEST_CASE("rules scheduler: a duplicated reply is written twice and a dropped "
          "one is reported with no bytes")
{
    bench b(peer_set(R"j([
      {"name": "twice", "when": {"bytes": "01"},
       "do": [{"reply": ["aa bb"], "fault": "duplicate"}]},
      {"name": "lost", "when": {"bytes": "02"},
       "do": [{"reply": ["cc"], "fault": "drop"}]},
      {"name": "plain", "when": {"bytes": "03"},
       "do": [{"reply": ["dd"]}]}
    ])j"));
    b.send("01");
    b.wait_reports(1);
    b.send("02");
    b.wait_reports(2);
    b.send("03");
    b.wait_reports(3);
    b.wait_written("aa bb aa bb dd");
    b.require_still(3, "aa bb aa bb dd");

    const auto reports = b.log.snapshot();
    REQUIRE(reports[0].kind == action_kind::reply);
    REQUIRE_EQ(reports[0].bytes, std::size_t{4});
    REQUIRE(reports[1].kind == action_kind::reply);
    REQUIRE_EQ(reports[1].rule, std::size_t{1});
    REQUIRE_EQ(reports[1].bytes, std::size_t{0});
    REQUIRE_EQ(reports[2].bytes, std::size_t{1});

    const scheduler_stats stats = b.rules->stats();
    REQUIRE_EQ(stats.done, std::uint64_t{3});
    REQUIRE_EQ(stats.machine.replies, std::uint64_t{3});
    REQUIRE_EQ(stats.machine.faults, std::uint64_t{2});
    REQUIRE_EQ(stats.machine.reply_bytes, std::uint64_t{5});
}

// -- state -------------------------------------------------------------------

TEST_CASE("rules scheduler: a rule's set_state takes effect at the match and "
          "is not reported")
{
    bench b(peer_set(R"j([
      {"name": "go", "when": {"bytes": "01", "state": "a"},
       "do": [{"set_state": "b"}, {"mark": "went"}]},
      {"name": "back", "when": {"bytes": "03", "state": "b"},
       "do": [{"set_state": "a"}]},
      {"name": "in a", "when": {"bytes": "02", "state": "a"},
       "do": [{"reply": ["aa"]}]},
      {"name": "in b", "when": {"bytes": "02", "state": "b"},
       "do": [{"reply": ["bb"]}]}
    ])j",
                     R"j("states": ["a", "b"], )j"));

    b.send("02");
    b.wait_reports(1);
    b.wait_written("aa");

    b.send("01");
    b.wait_reports(2);
    b.send("02");
    b.wait_reports(3);
    b.wait_written("aa bb");
    // The change was not an action: only the mark and the replies were given
    // a time.
    REQUIRE_EQ(b.rules->stats().scheduled, std::uint64_t{3});

    b.send("03");
    b.require_still(3, "aa bb");
    REQUIRE_EQ(b.rules->stats().scheduled, std::uint64_t{3});

    // The state is "a" again, and the byte after the match in one chunk is
    // already matched in the new state.
    b.send("01 02");
    b.wait_reports(5);
    b.wait_written("aa bb bb");

    const auto reports = b.log.snapshot();
    REQUIRE(reports[0].kind == action_kind::reply);
    REQUIRE_EQ(reports[0].rule, std::size_t{2});
    REQUIRE(reports[1].kind == action_kind::mark);
    REQUIRE_EQ(reports[1].rule, std::size_t{0});
    REQUIRE_EQ(reports[1].index, std::size_t{1});
    REQUIRE_EQ(reports[2].rule, std::size_t{3});
    REQUIRE(reports[3].kind == action_kind::mark);
    REQUIRE_EQ(reports[4].rule, std::size_t{3});
    for (const auto& report : reports)
        REQUIRE(report.kind != action_kind::set_state);
    REQUIRE_EQ(b.rules->stats().scheduled, std::uint64_t{5});
}

TEST_CASE("rules scheduler: a state change on the timeline takes effect when "
          "its time is reached on the hub's clock")
{
    bench b(peer_set(R"j([
      {"name": "in a", "when": {"bytes": "02", "state": "a"},
       "do": [{"reply": ["aa"]}]},
      {"name": "in b", "when": {"bytes": "02", "state": "b"},
       "do": [{"reply": ["bb"]}]}
    ])j",
                     R"j("states": ["a", "b"],
                         "timeline": [{"at_us": 2000,
                                       "do": [{"set_state": "b"}]}], )j"));
    REQUIRE_EQ(b.rules->waiting(), std::size_t{1});

    b.send("02");
    b.wait_reports(1);
    b.wait_written("aa");

    // Just before its time the set is still in "a".
    b.clock.advance(1999us);
    b.send("02");
    b.wait_reports(2);
    b.wait_written("aa aa");
    REQUIRE_EQ(b.rules->waiting(), std::size_t{1});

    b.clock.advance(1us);
    b.wait_reports(3);
    const auto change = b.log.snapshot()[2];
    REQUIRE_EQ(change.rule, no_index);
    REQUIRE_EQ(change.step, std::size_t{0});
    REQUIRE(change.kind == action_kind::set_state);
    REQUIRE_EQ(change.bytes, std::size_t{0});
    REQUIRE(change.due == b.at(2000us));
    REQUIRE(change.done == b.at(2000us));

    b.send("02");
    b.wait_reports(4);
    b.wait_written("aa aa bb");
    REQUIRE_EQ(b.rules->stats().scheduled, std::uint64_t{4});
}

// -- timeline ----------------------------------------------------------------

TEST_CASE("rules scheduler: timeline entries are done at their times on the "
          "hub's clock")
{
    bench b(peer_set("[]", R"j("timeline": [
      {"at_us": 1000, "do": [{"mark": "one"}]},
      {"at_us": 3000, "do": [{"mark": "three"}, {"reply": ["cc dd"]}]}
    ], )j"));
    REQUIRE_EQ(b.rules->waiting(), std::size_t{3});
    REQUIRE_EQ(b.rules->stats().scheduled, std::uint64_t{3});

    b.clock.advance(999us);
    b.require_still(0, "");
    b.clock.advance(1us);
    b.wait_reports(1);
    b.clock.advance(1999us);
    b.require_still(1, "");
    b.clock.advance(1us);
    b.wait_reports(3);
    b.wait_written("cc dd");

    const auto reports = b.log.snapshot();
    REQUIRE_EQ(reports.size(), std::size_t{3});
    for (const auto& report : reports)
        REQUIRE_EQ(report.rule, no_index);

    REQUIRE_EQ(reports[0].step, std::size_t{0});
    REQUIRE(reports[0].kind == action_kind::mark);
    REQUIRE(reports[0].due == b.at(1000us));
    REQUIRE(reports[0].done == b.at(1000us));

    REQUIRE_EQ(reports[1].step, std::size_t{1});
    REQUIRE_EQ(reports[1].index, std::size_t{0});
    REQUIRE(reports[1].kind == action_kind::mark);
    REQUIRE(reports[1].due == b.at(3000us));
    REQUIRE(reports[1].done == b.at(3000us));

    REQUIRE_EQ(reports[2].step, std::size_t{1});
    REQUIRE_EQ(reports[2].index, std::size_t{1});
    REQUIRE(reports[2].kind == action_kind::reply);
    REQUIRE_EQ(reports[2].bytes, std::size_t{2});
    REQUIRE(reports[2].due == b.at(3000us));
    REQUIRE(reports[2].done == b.at(3000us));
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});
}

// -- silence -----------------------------------------------------------------

TEST_CASE("rules scheduler: a rule on silence is done when the silence has "
          "lasted, and more bytes push it back")
{
    bench b(peer_set(R"j([
      {"name": "quiet", "when": {"quiet_us": 5000},
       "do": [{"reply": ["ee"], "delay": {"fixed_us": 2000}}]}
    ])j"));

    // The first byte, and the second 3 ms after it: the silence is counted
    // from the second, so it is complete at 8000 us and not at 5000 us.
    b.send("01");
    b.clock.advance(3000us);
    b.send("02");
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});

    b.clock.advance(2000us);
    b.require_still(0, "");
    REQUIRE_EQ(b.rules->stats().machine.firings, std::uint64_t{0});
    b.clock.advance(2999us);
    b.require_still(0, "");
    REQUIRE_EQ(b.rules->stats().machine.firings, std::uint64_t{0});

    // The scheduler woke for the deadline of the matcher with nothing queued.
    // The rule fires, and its reply is due 2 ms after the end of the silence.
    b.clock.advance(1us);
    REQUIRE(wait_until([&] { return b.rules->waiting() == 1; }, 5000ms));
    REQUIRE_EQ(b.rules->stats().machine.firings, std::uint64_t{1});
    b.require_still(0, "");

    b.clock.advance(1999us);
    b.require_still(0, "");
    b.clock.advance(1us);
    b.wait_reports(1);
    b.wait_written("ee");
    const auto report = b.log.snapshot()[0];
    REQUIRE_EQ(report.rule, std::size_t{0});
    REQUIRE(report.due == b.at(10000us));
    REQUIRE(report.done == b.at(10000us));

    // Once for each silence: time passing is not another one.
    b.clock.advance(10s);
    b.require_still(1, "ee");
    REQUIRE_EQ(b.rules->stats().machine.firings, std::uint64_t{1});

    // New bytes, and the next silence fires it again.
    b.send("03");
    b.clock.advance(7000us);
    b.wait_reports(2);
    b.wait_written("ee ee");
    REQUIRE_EQ(b.rules->stats().machine.firings, std::uint64_t{2});
}

TEST_CASE("rules scheduler: a pattern that needs silence after it fires when "
          "the silence has lasted, and not when another byte comes first")
{
    bench b(peer_set(R"j([
      {"name": "frame", "when": {"bytes": "01 02", "gap_us": 5000,
                                 "gap": "after"},
       "do": [{"reply": ["aa"]}]}
    ])j"));

    b.send("01 02");
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});
    b.clock.advance(4999us);
    b.require_still(0, "");
    b.clock.advance(1us);
    b.wait_reports(1);
    b.wait_written("aa");
    // The firing is as if it had been at the end of the pattern.
    const auto report = b.log.snapshot()[0];
    REQUIRE(report.due == b.at(0us));
    REQUIRE(report.done == b.at(5000us));

    // The same bytes, and a byte 2 ms later: no silence, no match.
    b.send("01 02");
    b.clock.advance(2000us);
    b.send("03");
    b.clock.advance(10s);
    b.require_still(1, "aa");
    REQUIRE_EQ(b.rules->stats().machine.firings, std::uint64_t{1});
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});
}

// -- lateness ----------------------------------------------------------------

TEST_CASE("rules scheduler: an action done long after it was due is reported "
          "late")
{
    bench b(peer_set(reply_after_5ms, R"j("late_after_us": 1000, )j"));

    // Nothing looks at the clock while it moves 50 ms.
    b.send("01 02");
    b.clock.advance(50ms);
    b.wait_reports(1);
    const auto first = b.log.snapshot()[0];
    REQUIRE(first.due == b.at(5000us));
    REQUIRE(first.done == b.at(50ms));
    REQUIRE(first.done > first.due);
    REQUIRE(first.late);
    scheduler_stats stats = b.rules->stats();
    REQUIRE_EQ(stats.late, std::uint64_t{1});
    REQUIRE(stats.worst_lateness >= 45ms);

    // Exactly as late as `late_after_us` is not late; a microsecond more is.
    b.send("01 02");
    b.clock.advance(6000us);
    b.wait_reports(2);
    REQUIRE(!b.log.snapshot()[1].late);
    REQUIRE_EQ(b.rules->stats().late, std::uint64_t{1});

    b.send("01 02");
    b.clock.advance(6001us);
    b.wait_reports(3);
    REQUIRE(b.log.snapshot()[2].late);
    stats = b.rules->stats();
    REQUIRE_EQ(stats.late, std::uint64_t{2});
    REQUIRE_EQ(stats.done, std::uint64_t{3});
    REQUIRE(stats.worst_lateness >= 45ms);
}

TEST_CASE("rules scheduler: with no late_after_us nothing is flagged late, "
          "but the lateness is still measured")
{
    bench b(peer_set(reply_after_5ms));
    b.send("01 02");
    b.clock.advance(50ms);
    b.wait_reports(1);

    const auto report = b.log.snapshot()[0];
    REQUIRE(report.done > report.due);
    REQUIRE(!report.late);
    const scheduler_stats stats = b.rules->stats();
    REQUIRE_EQ(stats.late, std::uint64_t{0});
    REQUIRE(stats.worst_lateness >= 45ms);
}

// -- the queue's limit -------------------------------------------------------

namespace
{

/// A rule that plans `actions` marks for each match, a minute from the match.
std::string many_marks(std::size_t actions)
{
    std::string text = R"j([{"name": "many", "when": {"bytes": "01"},
                             "do": [)j";
    for (std::size_t i = 0; i < actions; ++i)
    {
        if (i != 0)
            text += ", ";
        text += R"j({"mark": "m", "delay": {"fixed_us": 60000000}})j";
    }
    text += "]}]";
    return text;
}

} // namespace

TEST_CASE("rules scheduler: actions past the limit are counted and not done")
{
    constexpr std::size_t per_match = 16;
    constexpr std::size_t matches = 65;
    bench b(peer_set(many_marks(per_match)));

    // One chunk of 65 matches: 1040 actions, 16 more than can wait.
    b.send_raw(std::string(matches, '\x01'));
    REQUIRE_EQ(b.rules->waiting(), max_waiting_actions);
    scheduler_stats stats = b.rules->stats();
    REQUIRE_EQ(stats.machine.firings, std::uint64_t{matches});
    REQUIRE_EQ(stats.scheduled, std::uint64_t{max_waiting_actions});
    REQUIRE_EQ(stats.overrun,
               std::uint64_t{matches * per_match - max_waiting_actions});
    REQUIRE_EQ(stats.overrun, std::uint64_t{16});
    b.require_still(0, "");

    b.clock.advance(60s);
    b.wait_reports(max_waiting_actions);
    b.require_still(max_waiting_actions, "");

    stats = b.rules->stats();
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});
    REQUIRE_EQ(stats.done, std::uint64_t{max_waiting_actions});
    REQUIRE_EQ(stats.overrun, std::uint64_t{16});
    REQUIRE_EQ(stats.cancelled, std::uint64_t{0});

    // What was kept is the first 1024 planned, in the order they were planned.
    const auto reports = b.log.snapshot();
    for (std::size_t i = 0; i < reports.size(); ++i)
    {
        REQUIRE_EQ(reports[i].ordinal, std::uint64_t{i / per_match + 1});
        REQUIRE_EQ(reports[i].index, i % per_match);
        REQUIRE(reports[i].kind == action_kind::mark);
    }
}

// -- signals, lines and the port ---------------------------------------------

TEST_CASE("rules scheduler: a declared signal fires its rule and an undeclared "
          "one is refused")
{
    bench b(peer_set(R"j([
      {"name": "alpha", "when": {"signal": "alpha"},
       "do": [{"reply": ["aa"], "delay": {"fixed_us": 1000}}]},
      {"name": "beta", "when": {"signal": "beta"},
       "do": [{"reply": ["bb"], "after": "cause",
               "delay": {"fixed_us": 2000}}]}
    ])j",
                     R"j("signals": ["alpha", "beta"],
                         "order": "independent", )j"));
    const time_point now = b.clock.now();

    for (const std::size_t index : {std::size_t{2}, std::size_t{1000}})
    {
        const result<void> missing =
            b.rules->signal(index, bytes_view{}, now, now);
        REQUIRE(!missing);
        REQUIRE_EQ(missing.error(), make_error_code(errc::not_found));
    }
    REQUIRE_EQ(b.rules->stats().machine.firings, std::uint64_t{0});
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});

    REQUIRE(b.rules->signal(0, bytes_view{}, now, now));
    REQUIRE_EQ(b.rules->waiting(), std::size_t{1});
    b.clock.advance(999us);
    b.require_still(0, "");
    b.clock.advance(1us);
    b.wait_reports(1);
    b.wait_written("aa");
    REQUIRE_EQ(b.log.snapshot()[0].rule, std::size_t{0});

    // A cause in the past: the action is due at cause + delay, which has
    // passed, so it is done at the next look at the clock, and not earlier
    // than that time.
    const time_point cause = b.clock.now() - 10ms;
    REQUIRE(b.rules->signal(1, bytes_view{}, cause, b.clock.now()));
    b.wait_reports(2);
    b.wait_written("aa bb");
    const auto late_cause = b.log.snapshot()[1];
    REQUIRE_EQ(late_cause.rule, std::size_t{1});
    REQUIRE(late_cause.due == cause + 2000us);
    REQUIRE(late_cause.done == b.at(1000us));
    REQUIRE(late_cause.done >= late_cause.due);

    // A cause that is now is due 2 ms from now, whatever the signal's age.
    REQUIRE(b.rules->signal(1, bytes_view{}, b.clock.now(), b.clock.now()));
    b.clock.advance(1999us);
    b.require_still(2, "aa bb");
    b.clock.advance(1us);
    b.wait_reports(3);
    b.wait_written("aa bb bb");
    REQUIRE(b.log.snapshot()[2].due == b.at(3000us));
    REQUIRE(b.log.snapshot()[2].done == b.at(3000us));
}

TEST_CASE("rules scheduler: a change of the lines and an open of the port fire "
          "their rules")
{
    bench b(peer_set(R"j([
      {"name": "dtr up", "when": {"lines": {"dtr": true}},
       "do": [{"reply": ["11"]}]},
      {"name": "opened", "when": {"port": "opened"},
       "do": [{"reply": ["22"], "delay": {"fixed_us": 2000}}]}
    ])j"));

    const auto now = [&] { return b.clock.now(); };
    b.rules->lines_changed(lines_of({}), lines_of({control_line::dtr}), now());
    b.wait_reports(1);
    b.wait_written("11");
    REQUIRE_EQ(b.log.snapshot()[0].rule, std::size_t{0});

    // Not the edge the rule waits for: no change, another line, DTR falling.
    b.rules->lines_changed(lines_of({control_line::dtr}),
                           lines_of({control_line::dtr}), now());
    b.rules->lines_changed(lines_of({control_line::dtr}),
                           lines_of({control_line::dtr, control_line::rts}),
                           now());
    b.rules->lines_changed(lines_of({control_line::dtr}), lines_of({}), now());
    b.rules->port_changed(port_event::closed, now());
    b.require_still(1, "11");
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});

    b.rules->port_changed(port_event::opened, now());
    REQUIRE_EQ(b.rules->waiting(), std::size_t{1});
    b.clock.advance(1999us);
    b.require_still(1, "11");
    b.clock.advance(1us);
    b.wait_reports(2);
    b.wait_written("11 22");
    REQUIRE_EQ(b.log.snapshot()[1].rule, std::size_t{1});
    REQUIRE_EQ(b.rules->stats().machine.firings, std::uint64_t{2});
}

// -- start and close ---------------------------------------------------------

TEST_CASE("rules scheduler: nothing is matched before start, and a second "
          "start schedules nothing more")
{
    bench b(peer_set(R"j([
      {"name": "answer", "when": {"bytes": "01"},
       "do": [{"reply": ["aa"]}]}
    ])j",
                     R"j("timeline": [{"at_us": 1000,
                                       "do": [{"mark": "t"}]}], )j"),
            bench::clock_mode::by_hand, 2, false);

    b.port.inject_read(raw("01"));
    REQUIRE(wait_until([&] { return b.port.endpoint->stats().bytes_rx == 1; },
                       5000ms));
    b.rules->lines_changed(lines_of({}), lines_of({control_line::dtr}),
                           b.clock.now());
    b.require_still(0, "");
    REQUIRE_EQ(b.rules->stats().machine.bytes_seen, std::uint64_t{0});
    REQUIRE_EQ(b.rules->stats().scheduled, std::uint64_t{0});
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});

    b.rules->start(b.origin);
    b.rules->start(b.origin + 5ms);
    REQUIRE_EQ(b.rules->waiting(), std::size_t{1});
    REQUIRE_EQ(b.rules->stats().scheduled, std::uint64_t{1});

    b.clock.advance(1000us);
    b.wait_reports(1);
    b.clock.advance(10s);
    b.require_still(1, "");
    const auto report = b.log.snapshot()[0];
    REQUIRE_EQ(report.rule, no_index);
    REQUIRE(report.due == b.at(1000us));

    // Started, it matches.
    b.send("01");
    b.wait_reports(2);
    b.wait_written("aa");
    REQUIRE_EQ(b.rules->stats().scheduled, std::uint64_t{2});
}

TEST_CASE("rules scheduler: close under load stops the replies, and what was "
          "reported was written")
{
    bench b(peer_set(R"j([
      {"name": "answer", "when": {"bytes": "01"},
       "do": [{"reply": ["aa"]}]}
    ])j"),
            bench::clock_mode::steady);

    constexpr int chunks = 2000;
    std::atomic<bool> failed{false};
    std::atomic<int> sent{0};
    joined_thread injector(
        [&]
        {
            try
            {
                for (int number = 0; number < chunks; ++number)
                {
                    b.port.inject_read(std::string(1, '\x01'));
                    ++sent;
                }
            }
            catch (...)
            {
                failed.store(true);
            }
        });

    REQUIRE(wait_until([&] { return b.log.size() >= 20; }, 10000ms));
    b.rules->close();

    // When close returns the scheduler has done its last action: the number
    // of reports never grows again.
    const std::size_t reported = b.log.size();
    const scheduler_stats at_close = b.rules->stats();
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});
    REQUIRE_EQ(at_close.done, std::uint64_t{reported});

    std::this_thread::sleep_for(50ms);
    REQUIRE_EQ(b.log.size(), reported);
    std::this_thread::sleep_for(50ms);
    REQUIRE_EQ(b.log.size(), reported);

    injector.join();
    REQUIRE(!failed.load());
    REQUIRE_EQ(sent.load(), chunks);
    REQUIRE_EQ(b.log.size(), reported);

    // Nothing was shown to the matcher after the close.
    const scheduler_stats quiet = b.rules->stats();
    REQUIRE_EQ(quiet.machine.bytes_seen, at_close.machine.bytes_seen);
    REQUIRE_EQ(quiet.scheduled, at_close.scheduled);
    REQUIRE_EQ(quiet.done + quiet.cancelled, quiet.scheduled);

    // Each reply that was reported reached the port, and no other did.  A
    // reply is one byte, and the endpoint's own queue may still be draining.
    REQUIRE(wait_until([&] { return b.port.written().size() == reported; },
                       5000ms));
    std::this_thread::sleep_for(50ms);
    REQUIRE_EQ(b.port.written().size(), reported);
}

TEST_CASE("rules scheduler: a scheduler whose stage is removed and that is "
          "dropped does nothing more")
{
    // On a clock moved by hand.
    {
        bench b(peer_set(reply_after_5ms));
        b.send("01 02");
        b.send("01 02");
        b.send("01 02");
        REQUIRE_EQ(b.rules->waiting(), std::size_t{3});

        put_stage(b.port, nullptr);
        std::weak_ptr<scheduler> weak = b.rules;
        b.rules.reset();
        REQUIRE(wait_until([&] { return weak.expired(); }, 5000ms));

        b.clock.advance(1s);
        b.require_still(0, "");
    }

    // On the steady clock, with a timer waiting for a time far off.
    {
        bench b(peer_set(R"j([
          {"name": "far", "when": {"bytes": "01"},
           "do": [{"reply": ["aa"], "delay": {"fixed_us": 30000000}}]}
        ])j"),
                bench::clock_mode::steady);
        for (int round = 0; round < 5; ++round)
            b.send("01");
        REQUIRE_EQ(b.rules->waiting(), std::size_t{5});

        put_stage(b.port, nullptr);
        std::weak_ptr<scheduler> weak = b.rules;
        b.rules.reset();
        REQUIRE(wait_until([&] { return weak.expired(); }, 5000ms));
        b.require_still(0, "");
    }

    // On the steady clock, with replies being carried out as it goes.
    {
        bench b(peer_set(R"j([
          {"name": "soon", "when": {"bytes": "01"},
           "do": [{"reply": ["aa"],
                   "delay": {"uniform": {"min_us": 0, "max_us": 5000}}}]}
        ])j",
                         R"j("order": "independent", )j"),
                bench::clock_mode::steady, 4);
        for (int round = 0; round < 200; ++round)
            b.send("01");

        put_stage(b.port, nullptr);
        std::weak_ptr<scheduler> weak = b.rules;
        b.rules.reset();
        REQUIRE(wait_until([&] { return weak.expired(); }, 5000ms));

        // Whatever was handed to the port before the scheduler went is
        // written; then the port is quiet.
        std::this_thread::sleep_for(100ms);
        const std::string written = b.port.written();
        REQUIRE(written.size() <= 200);
        std::this_thread::sleep_for(100ms);
        REQUIRE_EQ(b.port.written(), written);
        REQUIRE(b.log.size() <= 200);
    }
}

// -- stress ------------------------------------------------------------------

TEST_CASE("rules scheduler: many replies with random delays on the steady "
          "clock are each done once and never early")
{
    bench b(peer_set(R"j([
      {"name": "answer", "when": {"bytes": "01"},
       "do": [{"reply": ["aa bb"],
               "delay": {"uniform": {"min_us": 0, "max_us": 2000}}}]}
    ])j",
                     R"j("order": "independent", )j"),
            bench::clock_mode::steady, 4);

    constexpr std::size_t chunks = 2000;
    for (std::size_t number = 0; number < chunks; ++number)
        b.port.inject_read(std::string(1, '\x01'));
    REQUIRE(wait_until([&] { return b.log.size() == chunks; }, 30000ms));
    REQUIRE(wait_until([&] { return b.port.written().size() == chunks * 2; },
                       30000ms));
    std::this_thread::sleep_for(50ms);
    REQUIRE_EQ(b.log.size(), chunks);

    for (const auto& report : b.log.snapshot())
    {
        REQUIRE(report.done >= report.due);
        REQUIRE(report.kind == action_kind::reply);
        REQUIRE_EQ(report.bytes, std::size_t{2});
    }
    const scheduler_stats stats = b.rules->stats();
    REQUIRE_EQ(stats.machine.firings, std::uint64_t{chunks});
    REQUIRE_EQ(stats.scheduled, std::uint64_t{chunks});
    REQUIRE_EQ(stats.done, std::uint64_t{chunks});
    REQUIRE_EQ(stats.overrun, std::uint64_t{0});
    REQUIRE_EQ(b.rules->waiting(), std::size_t{0});
}
