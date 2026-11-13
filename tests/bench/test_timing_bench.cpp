//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How accurate the engine's timers are and what a round trip to another
// process costs.  This is a measurement, not a test: it is built with the other
// tests, is not registered with CTest and asserts no upper bound on a time.  A
// case requires only what is guaranteed (the setup worked, the events came).
//
// Every case prints one line in a fixed form:
//
//   BENCH <name> n=<N> unit=us min=<..> p50=<..> p95=<..> p99=<..> max=<..>
//
// Arrival is observed with the `when` of the hub's data events, which the
// endpoint reads from the steady clock as the write to the mock device
// completes.  It is not the time a subscriber ran, which a later line of the
// case reports when that matters.
//

#include <vss/agent/client.hpp>
#include <vss/agent/proto/message.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/event.hpp>
#include <vss/core/hub.hpp>
#include <vss/core/link.hpp>
#include <vss/core/types.hpp>
#include <vss/studio/responder.hpp>

#include "child_process.hpp"
#include "precise_wait.hpp"
#include "support/test_support.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{

using namespace std::chrono_literals;

using bench_clock = std::chrono::steady_clock;
using bench_time = bench_clock::time_point;

/// A measured time in microseconds; negative when something came early.
using micros = std::int64_t;

/// Rounds that run before the counted ones, so that the first use of a path
/// does not stand in the distribution.
constexpr std::size_t warmup_rounds = 5;

/// The longest any wait may take.  Generous: it only ends a broken run.
constexpr std::chrono::milliseconds long_limit{20000};

micros micros_between(bench_time from, bench_time to)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(to - from)
        .count();
}

/// The value at percent `percent` by the nearest-rank rule, from sorted
/// samples.
micros percentile(const std::vector<micros>& sorted, std::size_t percent)
{
    const std::size_t rank = (sorted.size() * percent + 99) / 100;
    return sorted[rank == 0 ? 0 : rank - 1];
}

/// Prints the line of one case.  `extra` is appended as it stands.
void report(const std::string& name, std::vector<micros> samples,
            const std::string& extra = std::string())
{
    REQUIRE(!samples.empty());
    std::sort(samples.begin(), samples.end());
    std::ostringstream line;
    line << "BENCH " << name << " n=" << samples.size() << " unit=us"
         << " min=" << samples.front() << " p50=" << percentile(samples, 50)
         << " p95=" << percentile(samples, 95)
         << " p99=" << percentile(samples, 99) << " max=" << samples.back()
         << extra;
    std::cout << line.str() << '\n';
}

// -- 1: a bare timer ---------------------------------------------------------

/// A steady timer on a running io_context thread, armed again from its own
/// handler.  A sample is when the handler ran minus when the timer was asked
/// to expire.
class timer_probe
{
public:
    timer_probe(std::chrono::milliseconds delay, std::size_t count)
        : timer_(context_), delay_(delay), count_(count)
    {
        samples_.reserve(count);
    }

    std::vector<micros> run()
    {
        boost::asio::post(context_, [this] { arm(); });
        std::thread worker([this] { context_.run(); });
        worker.join();
        return std::move(samples_);
    }

private:
    void arm()
    {
        expiry_ = bench_clock::now() + delay_;
        timer_.expires_at(expiry_);
        timer_.async_wait(
            [this](const boost::system::error_code& error)
            {
                const bench_time ran = bench_clock::now();
                if (error)
                    return;
                samples_.push_back(micros_between(expiry_, ran));
                if (samples_.size() < count_)
                    arm();
            });
    }

    boost::asio::io_context context_;
    boost::asio::steady_timer timer_;
    std::chrono::milliseconds delay_;
    std::size_t count_;
    bench_time expiry_{};
    std::vector<micros> samples_;
};

// -- 2 to 5: the hub ---------------------------------------------------------

/// When a data event came, on the endpoint's clock, and when the subscriber
/// that saw it ran.
struct arrival
{
    bench_time when{};
    bench_time seen{};
};

/// The arrivals one subscriber collected.  The sink may run after the test's
/// locals are gone, so the log is shared.
class arrival_log
{
public:
    void add(const arrival& item)
    {
        {
            std::lock_guard lock(mutex_);
            items_.push_back(item);
        }
        changed_.notify_all();
    }

    /// The `count`th arrival (the first is 1), waiting for it up to `limit`.
    std::optional<arrival> wait_for(std::size_t count,
                                    std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex_);
        const bool ready = changed_.wait_for(
            lock, limit, [&] { return items_.size() >= count; });
        if (!ready)
            return std::nullopt;
        return items_[count - 1];
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<arrival> items_;
};

/// A subscription to the data events of one direction on one endpoint.
class data_watch
{
public:
    data_watch(hub& engine, endpoint_id target, data_direction direction)
        : log_(std::make_shared<arrival_log>()),
          guard_(engine, filter_for(target),
                 [log = log_, direction](const endpoint_event& event)
                 {
                     const bench_time seen = bench_clock::now();
                     if (event.kind != event_kind::data ||
                         event.direction != direction)
                         return;
                     log->add({event.when, seen});
                 })
    {
    }

    std::optional<arrival> wait_for(std::size_t count) const
    {
        return log_->wait_for(count, long_limit);
    }

private:
    static event_filter filter_for(endpoint_id target)
    {
        event_filter filter;
        filter.endpoint = target;
        filter.kind = event_kind::data;
        return filter;
    }

    std::shared_ptr<arrival_log> log_;
    subscription_guard guard_;
};

/// Delivers `data` on `port` as if the device had sent it and returns the time
/// just before it was handed to the mock.  The clock is read on the port's own
/// strand, so the hop from the calling thread to that strand is not in it.
bench_time inject_timed(const mock_endpoint& port, const std::string& data)
{
    bench_time sent{};
    port.on_strand(
        [&]
        {
            sent = bench_clock::now();
            port.transport->mock()->inject_read(data);
        });
    return sent;
}

/// What one chunk took from the injection on a to its tx data event on b.
struct hop
{
    micros reached = 0; ///< injection until the event's `when`
    micros seen = 0;    ///< injection until the subscriber ran
};

link_options options_with(std::uint32_t delay_ms, std::uint32_t jitter_ms)
{
    link_options options;
    options.delay_ms = delay_ms;
    options.jitter_ms = jitter_ms;
    return options;
}

/// Sends `count` chunks from a to b over a link with the given delay and
/// jitter, one at a time, each after the one before has arrived.
std::vector<hop> measure_link(std::uint32_t delay_ms, std::uint32_t jitter_ms,
                              std::size_t count)
{
    auto engine = start_hub();
    auto a = add_mock(*engine, "a");
    auto b = add_mock(*engine, "b");
    REQUIRE(engine->connect(a.endpoint->id(), b.endpoint->id(),
                            options_with(delay_ms, jitter_ms)));
    const data_watch at_b(*engine, b.endpoint->id(), data_direction::tx);

    std::vector<hop> hops;
    hops.reserve(count);
    for (std::size_t round = 0; round < warmup_rounds + count; ++round)
    {
        const bench_time sent = inject_timed(a, "x");
        const auto arrived = at_b.wait_for(round + 1);
        REQUIRE(arrived.has_value());
        if (round >= warmup_rounds)
        {
            hops.push_back({micros_between(sent, arrived->when),
                            micros_between(sent, arrived->seen)});
        }
    }
    return hops;
}

std::vector<micros> reached_times(const std::vector<hop>& hops, micros minus)
{
    std::vector<micros> times;
    times.reserve(hops.size());
    for (const auto& item : hops)
        times.push_back(item.reached - minus);
    return times;
}

std::vector<micros> seen_times(const std::vector<hop>& hops)
{
    std::vector<micros> times;
    times.reserve(hops.size());
    for (const auto& item : hops)
        times.push_back(item.seen);
    return times;
}

std::string with_delay(const std::string& prefix, std::uint32_t delay_ms)
{
    return prefix + std::to_string(delay_ms) + "ms";
}

/// The script text of one rule: bytes `aa` are answered with `bb` after
/// `delay_ms`.
std::string one_rule_script(std::uint32_t delay_ms)
{
    return std::string(R"({"rules":[{"match":"aa","reply":"bb","delay_ms":)") +
           std::to_string(delay_ms) + "}]}";
}

/// Triggers a responder `count` times, one after the reply to the one before,
/// and returns the trigger-to-reply times minus the delay the rule asks for,
/// from the injection and from the engine's own event of the trigger.
struct reply_samples
{
    std::vector<micros> from_injection;
    std::vector<micros> from_trigger_event;
};

reply_samples measure_replies(std::uint32_t delay_ms, std::size_t count)
{
    auto engine = start_hub();
    auto device = add_mock(*engine, "device");
    const data_watch triggers(*engine, device.endpoint->id(),
                              data_direction::rx);
    const data_watch replies(*engine, device.endpoint->id(),
                             data_direction::tx);

    std::string detail;
    const auto script = parse_responder_script(one_rule_script(delay_ms),
                                               &detail);
    REQUIRE(script.has_value());
    auto running = responder::start(*engine, device.endpoint, *script);
    REQUIRE(running.has_value());

    const micros nominal = static_cast<micros>(delay_ms) * 1000;
    const std::string trigger("\xaa", 1);

    reply_samples samples;
    samples.from_injection.reserve(count);
    samples.from_trigger_event.reserve(count);
    for (std::size_t round = 0; round < warmup_rounds + count; ++round)
    {
        const bench_time sent = inject_timed(device, trigger);
        const auto replied = replies.wait_for(round + 1);
        REQUIRE(replied.has_value());
        const auto triggered = triggers.wait_for(round + 1);
        REQUIRE(triggered.has_value());
        if (round >= warmup_rounds)
        {
            samples.from_injection.push_back(
                micros_between(sent, replied->when) - nominal);
            samples.from_trigger_event.push_back(
                micros_between(triggered->when, replied->when) - nominal);
        }
    }
    (*running)->stop();
    return samples;
}

// -- 6: another process ------------------------------------------------------

std::string unique_instance()
{
    static const std::string prefix = []
    {
        std::random_device device;
        std::ostringstream text;
        text << "vssb-" << std::hex << std::setw(8) << std::setfill('0')
             << device();
        return text.str();
    }();
    static unsigned counter = 0;
    return prefix + "-" + std::to_string(counter++);
}

std::string describe(const vss::error_code& error)
{
    return std::string(error.category().name()) + ":" +
           std::to_string(error.value()) + " (" + error.message() + ")";
}

template <class Outcome>
void require_ok(const Outcome& outcome, const char* what)
{
    if (!outcome)
    {
        throw vss_test::failure(std::string(what) +
                                " failed: " + describe(outcome.error()));
    }
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

/// The real `vss_agent` as a child process under its own instance name, with
/// ports `a` and `b` linked one way, a to b.
class agent_fixture
{
public:
    agent_fixture()
        : instance_(unique_instance()),
          child_(VSS_AGENT_PROGRAM, {"--instance=" + instance_})
    {
        wait_until_listening();
        writer_ = connect();
        const auto first = writer_->create_port("a");
        require_ok(first, "creating port a");
        const auto second = writer_->create_port("b");
        require_ok(second, "creating port b");
        const auto linked = writer_->connect_ports(*first, *second, true);
        require_ok(linked, "linking a to b");
        a_ = *first;
        b_ = *second;
    }

    /// A new connection to this agent.
    std::unique_ptr<vss::agent::client> connect() const
    {
        vss::agent::client_options options;
        options.instance = instance_;
        options.name = "timing-bench";
        options.timeout = 10000ms;
        auto connected = vss::agent::client::connect(options);
        require_ok(connected, "connecting to the agent");
        return std::move(*connected);
    }

    /// Asks the agent to stop and waits for it to exit.
    void finish()
    {
        const auto stopped = writer_->shutdown();
        require_ok(stopped, "shutting the agent down");
        REQUIRE(child_.wait(long_limit).has_value());
    }

    vss::agent::client& writer() { return *writer_; }
    endpoint_id a() const noexcept { return a_; }
    endpoint_id b() const noexcept { return b_; }

private:
    void wait_until_listening()
    {
        std::string seen;
        const auto deadline = bench_clock::now() + long_limit;
        while (bench_clock::now() < deadline)
        {
            const auto line = child_.read_line(long_limit);
            if (!line)
                break;
            seen += *line + "\n";
            if (contains(*line, "listening on"))
                return;
        }
        throw vss_test::failure("the agent did not say it listens; it said: " +
                                seen);
    }

    std::string instance_;
    child_process child_;
    std::unique_ptr<vss::agent::client> writer_;
    endpoint_id a_ = 0;
    endpoint_id b_ = 0;
};

/// Eight distinguishable bytes: a letter and a seven digit counter.
std::string numbered(char letter, std::size_t number)
{
    std::ostringstream text;
    text << letter << std::setw(7) << std::setfill('0') << number;
    return text.str();
}

/// Reads batches from the subscribed `tap` until an rx data event on `port`
/// holds `payload`.  False when the deadline passed first.
bool wait_for_payload(vss::agent::client& tap, endpoint_id port,
                      const std::string& payload)
{
    const auto deadline = bench_clock::now() + long_limit;
    while (bench_clock::now() < deadline)
    {
        const auto batch = tap.next_events(200ms);
        if (!batch)
        {
            if (batch.error() == vss::errc::timeout)
                continue;
            throw vss_test::failure("reading events failed: " +
                                    describe(batch.error()));
        }
        for (const auto& event : batch->events)
        {
            if (event.kind != event_kind::data ||
                event.direction != data_direction::rx || event.endpoint != port)
                continue;
            if (contains(vss::to_string(event.data), payload))
                return true;
        }
    }
    return false;
}

/// Prints the system timer resolution, in units of 100 ns, as it is now.
void print_timer_resolution(const char* name)
{
    std::cout << "INFO " << name << '='
              << vss_bench::current_timer_resolution_100ns() << '\n';
}

/// The bare timer cases; `suffix` is added to their names.
void report_timer_overshoots(const std::string& suffix)
{
    struct setting
    {
        std::chrono::milliseconds delay;
        std::size_t count;
    };
    const setting settings[] = {{1ms, 300}, {5ms, 300}, {20ms, 200}};
    for (const auto& item : settings)
    {
        timer_probe probe(item.delay, item.count);
        const auto samples = probe.run();
        REQUIRE_EQ(samples.size(), item.count);
        report("timer_overshoot_" + std::to_string(item.delay.count()) + "ms" +
                   suffix,
               samples);
    }
}

/// One link delay case: `count` chunks over a link of `delay_ms`, as the error
/// against the delay.
void report_link_delay(std::uint32_t delay_ms, std::size_t count,
                       const std::string& suffix)
{
    const auto hops = measure_link(delay_ms, 0, count);
    REQUIRE_EQ(hops.size(), count);
    report(with_delay("link_delay_", delay_ms) + suffix,
           reached_times(hops, static_cast<micros>(delay_ms) * 1000));
}

/// One responder case, as the error against the delay of its rule.
void report_reply(std::uint32_t delay_ms, const std::string& suffix)
{
    const auto samples = measure_replies(delay_ms, 300);
    REQUIRE_EQ(samples.from_injection.size(), std::size_t{300});
    const std::string name = with_delay("responder_reply_", delay_ms) + suffix;
    report(name, samples.from_injection);
    report(name + "_from_trigger_event", samples.from_trigger_event);
}

} // namespace

TEST_CASE("bench: a bare steady timer overshoots by")
{
    print_timer_resolution("timer_resolution_100ns");
    report_timer_overshoots(std::string());
}

TEST_CASE("bench: a route with no delay")
{
    const auto hops = measure_link(0, 0, 2000);
    REQUIRE_EQ(hops.size(), std::size_t{2000});
    report("route_latency_nodelay", reached_times(hops, 0));
    report("route_latency_nodelay_sink", seen_times(hops));
}

TEST_CASE("bench: a link delay against its nominal value")
{
    report_link_delay(1, 300, std::string());
    report_link_delay(5, 300, std::string());
    report_link_delay(20, 300, std::string());
    report_link_delay(100, 100, std::string());
}

TEST_CASE("bench: a link delay with jitter")
{
    const auto hops = measure_link(20, 10, 500);
    REQUIRE_EQ(hops.size(), std::size_t{500});
    const auto observed = reached_times(hops, 0);
    std::set<micros> buckets;
    for (const micros value : observed)
        buckets.insert(value / 1000);
    report("link_jitter_20ms_10ms", observed,
           " buckets=" + std::to_string(buckets.size()));
}

TEST_CASE("bench: the responder replies after its delay")
{
    for (const std::uint32_t delay_ms : {0U, 1U, 5U, 20U})
        report_reply(delay_ms, std::string());
}

TEST_CASE("bench: one request to the agent and its answer")
{
    agent_fixture agent;
    constexpr std::size_t count = 2000;
    std::vector<micros> samples;
    samples.reserve(count);
    for (std::size_t round = 0; round < warmup_rounds + count; ++round)
    {
        const bench_time sent = bench_clock::now();
        const auto listed = agent.writer().list();
        const bench_time answered = bench_clock::now();
        require_ok(listed, "listing the ports");
        if (round >= warmup_rounds)
            samples.push_back(micros_between(sent, answered));
    }
    agent.finish();
    report("agent_rpc_roundtrip", samples);
}

TEST_CASE("bench: one write through the agent")
{
    agent_fixture agent;
    constexpr std::size_t count = 2000;
    std::vector<micros> samples;
    samples.reserve(count);
    for (std::size_t round = 0; round < warmup_rounds + count; ++round)
    {
        const std::string payload = numbered('w', round);
        const bench_time sent = bench_clock::now();
        const auto written =
            agent.writer().write(agent.a(), vss::as_bytes(payload));
        const bench_time answered = bench_clock::now();
        require_ok(written, "writing");
        if (round >= warmup_rounds)
            samples.push_back(micros_between(sent, answered));
    }
    agent.finish();
    report("agent_write_roundtrip", samples);
}

TEST_CASE("bench: a plugin that answers bytes from outside the agent")
{
    agent_fixture agent;
    auto tap = agent.connect();
    const auto subscribed =
        tap->subscribe(true, vss::agent::min_event_interval_ms);
    require_ok(subscribed, "subscribing");

    constexpr std::size_t count = 300;
    std::vector<micros> seen;
    std::vector<micros> answered;
    seen.reserve(count);
    answered.reserve(count);
    for (std::size_t round = 0; round < warmup_rounds + count; ++round)
    {
        const std::string payload = numbered('h', round);
        const std::string reply = numbered('r', round);

        const bench_time sent = bench_clock::now();
        const auto written =
            agent.writer().write(agent.a(), vss::as_bytes(payload));
        require_ok(written, "writing the trigger");
        REQUIRE(wait_for_payload(*tap, agent.b(), payload));
        const bench_time noticed = bench_clock::now();
        const auto replied =
            agent.writer().write(agent.a(), vss::as_bytes(reply));
        const bench_time done = bench_clock::now();
        require_ok(replied, "writing the reply");

        if (round >= warmup_rounds)
        {
            seen.push_back(micros_between(sent, noticed));
            answered.push_back(micros_between(sent, done));
        }
    }
    agent.finish();
    report("agent_hook_detect", seen);
    report("agent_hook_roundtrip", answered);
}

// -- the wait of the operating system, with and without a raised period ------

namespace
{

/// On a thread of its own: `count` waits of `span` each, as the time the wait
/// returned after its due time.  With `spin` the operating system wait ends 300
/// microseconds early and a loop that only reads the clock does the rest.
std::vector<micros> measure_waits(std::chrono::microseconds span, bool spin,
                                  std::size_t count)
{
    constexpr std::chrono::microseconds margin{300};
    std::vector<micros> samples;
    samples.reserve(count);
    std::thread worker(
        [&]
        {
            for (std::size_t round = 0; round < warmup_rounds + count; ++round)
            {
                const bench_time due = bench_clock::now() + span;
                bench_time woke{};
                if (spin)
                {
                    if (span > margin)
                        vss_bench::precise_wait_until(due - margin);
                    woke = bench_clock::now();
                    while (woke < due)
                        woke = bench_clock::now();
                }
                else
                {
                    vss_bench::precise_wait_until(due);
                    woke = bench_clock::now();
                }
                if (round >= warmup_rounds)
                    samples.push_back(micros_between(due, woke));
            }
        });
    worker.join();
    return samples;
}

} // namespace

TEST_CASE("bench: the precise wait")
{
    struct setting
    {
        std::chrono::microseconds span;
        const char* label;
    };
    const setting settings[] = {{100us, "100us"},
                                {500us, "500us"},
                                {1ms, "1ms"},
                                {5ms, "5ms"},
                                {20ms, "20ms"}};
    for (const auto& item : settings)
    {
        const auto waited = measure_waits(item.span, false, 500);
        REQUIRE_EQ(waited.size(), std::size_t{500});
        report(std::string("precise_wait_") + item.label, waited);
        const auto spun = measure_waits(item.span, true, 500);
        REQUIRE_EQ(spun.size(), std::size_t{500});
        report(std::string("precise_wait_spin_") + item.label, spun);
    }
}

// These run last and each holds a timer period of 1 ms only while it works, so
// that no case above ran under it.

TEST_CASE("bench: a bare steady timer with the period at 1 ms")
{
    const vss_bench::timer_period_scope scope;
    print_timer_resolution("timer_resolution_100ns_in_scope");
    report_timer_overshoots("_period1");
}

TEST_CASE("bench: a link delay with the period at 1 ms")
{
    const vss_bench::timer_period_scope scope;
    report_link_delay(1, 300, "_period1");
    report_link_delay(5, 300, "_period1");
    report_link_delay(20, 300, "_period1");
}

TEST_CASE("bench: the responder with the period at 1 ms")
{
    const vss_bench::timer_period_scope scope;
    report_reply(1, "_period1");
    report_reply(5, "_period1");
}
