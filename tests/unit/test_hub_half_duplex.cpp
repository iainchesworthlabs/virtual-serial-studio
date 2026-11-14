//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A half-duplex bus in a running hub.  The hub is given a clock that the test
// moves by hand, so a chunk leaves the wire when the test says and never
// because the machine was slow or fast.  The line runs at 1000 bit/s, where
// one byte takes ten milliseconds.
//

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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

link_options
half_duplex(std::uint32_t turnaround_ms = 0,
            collision_action on_collision = collision_action::report,
            bool rts_gating = false)
{
    link_options options;
    options.half_duplex.enabled = true;
    options.half_duplex.bit_rate = 1000;
    options.half_duplex.turnaround_ms = turnaround_ms;
    options.half_duplex.on_collision = on_collision;
    options.half_duplex.rts_gating = rts_gating;
    return options;
}

/// What a receiver reads of bytes that collided on a garbling bus.
std::string inverted(std::string text)
{
    for (char& value : text)
        value = static_cast<char>(~static_cast<unsigned char>(value));
    return text;
}

/// Does `send`, which gives the bus `link` a chunk of `bytes` bytes from one
/// member, and waits until the hub has routed it to every other member,
/// carried or dropped, so that whatever the test does next comes after the
/// line has judged the chunk and after every count of it.  (An endpoint
/// counts the bytes it read before it routes them, so its own counter says
/// too little, and the link counts the chunk once for each member that is
/// to hear it, one after another.)
template <class Send>
void send_on(vss::hub& hub, link_id link, std::size_t bytes, Send&& send)
{
    std::uint64_t hearers = 0;
    const auto routed = [&]() -> std::uint64_t
    {
        const auto info = hub.find_link(link);
        if (!info)
            return 0;
        hearers = info->sources.empty() ? 0 : info->sources.size() - 1;
        return info->stats.bytes + info->stats.dropped;
    };
    const std::uint64_t before = routed();
    const std::uint64_t after = before + bytes * hearers;
    send();
    REQUIRE(wait_until([&] { return routed() == after; }, 5000ms));
}

struct line_fixture
{
    hand_clock clock;
    std::shared_ptr<vss::hub> hub = start_hub_on(clock);
    std::vector<mock_endpoint> ports;
    event_recorder events;
    std::optional<subscription_guard> guard;
    link_id link = 0;

    explicit line_fixture(std::size_t count)
    {
        for (std::size_t index = 0; index < count; ++index)
            ports.push_back(add_mock(*hub, "p" + std::to_string(index)));
        event_filter faults;
        faults.kind = event_kind::bus_fault;
        guard.emplace(*hub, faults,
                      [recorder = events](const endpoint_event& event)
                      { recorder.record(event); });
    }

    endpoint_id id(std::size_t index) const
    {
        return ports[index].endpoint->id();
    }

    std::vector<endpoint_id> ids() const
    {
        std::vector<endpoint_id> out;
        out.reserve(ports.size());
        for (const auto& port : ports)
            out.push_back(port.endpoint->id());
        return out;
    }

    /// Makes the bus the fixture sends on, of every port.
    link_id make_bus(const link_options& options)
    {
        const auto made = hub->bus(ids(), options);
        REQUIRE(made);
        link = *made;
        return link;
    }

    /// Sends from a member of that bus, and waits for the hub to route it.
    void send(std::size_t index, const std::string& text) const
    {
        send_on(*hub, link, text.size(),
                [&] { ports[index].inject_read(text); });
    }

    bool heard(std::size_t index, const std::string& text) const
    {
        return wait_until([&] { return ports[index].written() == text; },
                          5000ms);
    }

    /// The link `of`, which must exist.
    link_info info(link_id of) const
    {
        const auto found = hub->find_link(of);
        REQUIRE(found);
        return *found;
    }

    link_stats stats(link_id of) const
    {
        const auto info = hub->find_link(of);
        REQUIRE(info);
        return info->stats;
    }

    bool faults(std::size_t count) const
    {
        return wait_until(
            [&] { return events.count(event_kind::bus_fault) == count; },
            5000ms);
    }
};

/// A station whose RTS the hub follows: the test raises and drops it through
/// the endpoint, as a program does through its port.
class station_transport final : public vss::transport
{
public:
    explicit station_transport(const executor_type& executor)
        : executor_(executor)
    {
    }

    std::string describe() const override { return "station"; }
    bool is_open() const override { return open_.load(); }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override
    {
        std::lock_guard lock(mutex_);
        buffer_ = buffer;
        read_ = std::move(handler);
        complete_read();
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        const std::size_t size = buffer.size();
        {
            std::lock_guard lock(mutex_);
            written_.append(static_cast<const char*>(buffer.data()), size);
        }
        boost::asio::post(executor_, [handler, size] { handler({}, size); });
    }

    void close() override
    {
        open_.store(false);
        cancel();
    }

    void cancel() override
    {
        io_handler pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(read_);
        }
        if (pending)
            boost::asio::post(
                executor_, [pending]
                { pending(boost::asio::error::operation_aborted, 0); });
    }

    bool supports_control_lines() const noexcept override { return true; }
    bool reports_peer_rts() const noexcept override { return true; }

    void set_control_line(control_line line, bool state) override
    {
        std::lock_guard lock(mutex_);
        lines_.set(line, state);
    }

    control_lines lines() const override
    {
        std::lock_guard lock(mutex_);
        return lines_;
    }

    /// The station sends `text`.
    void inject(const std::string& text)
    {
        std::lock_guard lock(mutex_);
        incoming_ += text;
        complete_read();
    }

    std::string written() const
    {
        std::lock_guard lock(mutex_);
        return written_;
    }

private:
    /// Hands what the station sent to a waiting read.  The caller holds the
    /// mutex.
    void complete_read()
    {
        if (!read_ || incoming_.empty())
            return;
        const std::size_t count = std::min(buffer_.size(), incoming_.size());
        std::copy_n(incoming_.begin(), count,
                    static_cast<char*>(buffer_.data()));
        incoming_.erase(0, count);
        io_handler handler;
        handler.swap(read_);
        boost::asio::post(executor_, [handler, count] { handler({}, count); });
    }

    executor_type executor_;
    std::atomic<bool> open_{true};
    mutable std::mutex mutex_;
    boost::asio::mutable_buffer buffer_;
    io_handler read_;
    std::string incoming_;
    std::string written_;
    control_lines lines_;
};

struct station
{
    std::shared_ptr<vss::endpoint> endpoint;
    std::shared_ptr<station_transport> transport;

    void send(vss::hub& hub, link_id link, const std::string& text) const
    {
        send_on(hub, link, text.size(), [&] { transport->inject(text); });
    }

    bool heard(const std::string& text) const
    {
        return wait_until([&] { return transport->written() == text; }, 5000ms);
    }
};

station add_station(vss::hub& hub, const std::string& name)
{
    auto transport = std::make_shared<station_transport>(hub.executor());
    auto added =
        hub.add_endpoint(name, vss::endpoint_kind::virtual_port, transport);
    REQUIRE(added);
    return station{*added, std::move(transport)};
}

} // namespace

TEST_CASE("half-duplex: the option is reported and its values are checked")
{
    line_fixture fixture(3);

    link_options options = half_duplex(7, collision_action::garble, true);
    options.half_duplex.bit_rate = 19200;
    const link_id bus = fixture.make_bus(options);
    const auto info = fixture.hub->find_link(bus);
    REQUIRE(info);
    REQUIRE(info->options.half_duplex == options.half_duplex);
    REQUIRE(!info->options.emulate_modem_lines);
    REQUIRE(fixture.hub->disconnect(bus));

    const auto invalid = make_error_code(errc::invalid_argument);
    link_options slow = half_duplex();
    slow.half_duplex.bit_rate = min_bus_bit_rate - 1;
    REQUIRE_EQ(fixture.hub->bus(fixture.ids(), slow).error(), invalid);
    link_options fast = half_duplex();
    fast.half_duplex.bit_rate = max_bus_bit_rate + 1;
    REQUIRE_EQ(fixture.hub->bus(fixture.ids(), fast).error(), invalid);
    link_options patient = half_duplex();
    patient.half_duplex.turnaround_ms = max_link_delay_ms + 1;
    REQUIRE_EQ(fixture.hub->bus(fixture.ids(), patient).error(), invalid);
    REQUIRE(fixture.hub->links().empty());

    // The limits themselves are allowed, and values of an option that is off
    // are not looked at.
    link_options limits = half_duplex();
    limits.half_duplex.bit_rate = max_bus_bit_rate;
    limits.half_duplex.turnaround_ms = max_link_delay_ms;
    const auto widest = fixture.hub->bus(fixture.ids(), limits);
    REQUIRE(widest);
    REQUIRE(fixture.hub->disconnect(*widest));
    link_options off;
    off.half_duplex.bit_rate = 0;
    REQUIRE(fixture.hub->bus(fixture.ids(), off));
}

TEST_CASE("half-duplex: only a bus can be a half-duplex line")
{
    line_fixture fixture(3);
    const auto unsupported = make_error_code(errc::unsupported);
    const auto a = fixture.id(0);
    const auto b = fixture.id(1);
    const auto c = fixture.id(2);

    REQUIRE_EQ(fixture.hub->connect(a, b, half_duplex()).error(), unsupported);
    REQUIRE_EQ(fixture.hub->split(a, {b, c}, half_duplex()).error(),
               unsupported);
    REQUIRE_EQ(fixture.hub->join({a, b}, c, half_duplex()).error(),
               unsupported);
    REQUIRE_EQ(fixture.hub->redirect(a, b, half_duplex()).error(), unsupported);
    REQUIRE(fixture.hub->links().empty());
}

TEST_CASE("half-duplex: the owner and the wiring are still refused")
{
    line_fixture fixture(3);

    link_options owned = half_duplex();
    owned.policy = merge_policy::owner;
    REQUIRE_EQ(fixture.hub->bus(fixture.ids(), owned).error(),
               make_error_code(errc::unsupported));

    const link_id bus = fixture.make_bus(half_duplex());
    REQUIRE_EQ(fixture.hub->set_owner(bus, fixture.id(0)),
               make_error_code(errc::unsupported));
    REQUIRE_EQ(fixture.hub->set_link_wiring(bus, modem_wiring{}),
               make_error_code(errc::unsupported));
}

TEST_CASE("half-duplex: a chunk is heard when its last byte has left the wire")
{
    line_fixture fixture(3);
    const link_id bus = fixture.make_bus(half_duplex());

    // Four bytes take 40 ms.  The hub has read and counted them, and nobody
    // has heard them: the clock has not moved.
    fixture.send(0, "poll");
    REQUIRE_EQ(fixture.stats(bus).bytes, std::uint64_t{8});
    REQUIRE_EQ(fixture.stats(bus).messages, std::uint64_t{2});
    fixture.clock.advance(40ms - 1ns);
    // However often the wire looks, the chunk is not due yet.
    REQUIRE_EQ(fixture.ports[1].written(), std::string());
    REQUIRE_EQ(fixture.ports[2].written(), std::string());

    fixture.clock.advance(1ns);
    REQUIRE(fixture.heard(1, "poll"));
    REQUIRE(fixture.heard(2, "poll"));
    // The sender never hears itself.
    REQUIRE_EQ(fixture.ports[0].written(), std::string());

    // The answer starts when the poll has gone, so nothing collided.
    fixture.send(1, "ok");
    fixture.clock.advance(20ms);
    REQUIRE(fixture.heard(0, "ok"));
    REQUIRE(fixture.heard(2, "pollok"));

    const auto stats = fixture.stats(bus);
    REQUIRE_EQ(stats.bytes, std::uint64_t{12});
    REQUIRE_EQ(stats.dropped, std::uint64_t{0});
    REQUIRE_EQ(stats.collisions, std::uint64_t{0});
    REQUIRE_EQ(stats.garbled, std::uint64_t{0});
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{0});
}

TEST_CASE("half-duplex: two members at once are reported and both are heard")
{
    line_fixture fixture(3);
    const link_id bus = fixture.make_bus(half_duplex());

    fixture.send(0, "aaaa"); // on the wire for 40 ms
    fixture.clock.advance(10ms);
    fixture.send(1, "bb"); // 10 to 30 ms: inside it

    REQUIRE(fixture.faults(1));
    const auto fault = fixture.events.snapshot().front();
    REQUIRE(fault.fault == bus_fault::overlap);
    REQUIRE_EQ(fault.endpoint, fixture.id(1));
    REQUIRE_EQ(fault.name, std::string("p1"));
    REQUIRE_EQ(fault.link, bus);
    REQUIRE_EQ(fault.message, std::string("sent 2 bytes while p0 was sending"));

    // Reporting changes no byte.  The short chunk left the wire first.
    fixture.clock.advance(20ms);
    REQUIRE(fixture.heard(0, "bb"));
    REQUIRE(fixture.heard(2, "bb"));
    fixture.clock.advance(10ms);
    REQUIRE(fixture.heard(1, "aaaa"));
    REQUIRE(fixture.heard(2, "bbaaaa"));

    const auto stats = fixture.stats(bus);
    REQUIRE_EQ(stats.collisions, std::uint64_t{1});
    REQUIRE_EQ(stats.garbled, std::uint64_t{0});
    REQUIRE_EQ(stats.dropped, std::uint64_t{0});
    REQUIRE_EQ(stats.bytes, std::uint64_t{12});
}

TEST_CASE("half-duplex: with garble the chunks that collided arrive inverted")
{
    line_fixture fixture(3);
    const link_id bus =
        fixture.make_bus(half_duplex(0, collision_action::garble));

    fixture.send(0, "aaaa");
    fixture.clock.advance(10ms);
    fixture.send(1, "bb");
    REQUIRE(fixture.faults(1));
    REQUIRE_EQ(
        fixture.events.snapshot().front().message,
        std::string("sent 2 bytes while p0 was sending: both were garbled"));

    fixture.clock.advance(30ms);
    REQUIRE(fixture.heard(0, inverted("bb")));
    REQUIRE(fixture.heard(1, inverted("aaaa")));
    REQUIRE(fixture.heard(2, inverted("bb") + inverted("aaaa")));

    // Every byte is counted: delivered, and how many of them were spoilt.
    auto stats = fixture.stats(bus);
    REQUIRE_EQ(stats.bytes, std::uint64_t{12});
    REQUIRE_EQ(stats.garbled, std::uint64_t{12});
    REQUIRE_EQ(stats.collisions, std::uint64_t{1});
    REQUIRE_EQ(stats.dropped, std::uint64_t{0});

    // The next clean chunk is whole.
    fixture.send(2, "c");
    fixture.clock.advance(10ms);
    REQUIRE(fixture.heard(0, inverted("bb") + "c"));
    stats = fixture.stats(bus);
    REQUIRE_EQ(stats.garbled, std::uint64_t{12});
    REQUIRE_EQ(stats.collisions, std::uint64_t{1});
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{1});
}

TEST_CASE("half-duplex: back-to-back chunks of one member are clean")
{
    line_fixture fixture(3);
    const link_id bus =
        fixture.make_bus(half_duplex(5, collision_action::garble));

    // Three writes with no time between them leave one after another.
    fixture.send(0, "one");
    fixture.send(0, "two");
    fixture.send(0, "six");
    fixture.clock.advance(30ms);
    REQUIRE(fixture.heard(1, "one"));
    fixture.clock.advance(30ms);
    REQUIRE(fixture.heard(1, "onetwo"));
    fixture.clock.advance(30ms);
    REQUIRE(fixture.heard(1, "onetwosix"));
    REQUIRE(fixture.heard(2, "onetwosix"));

    REQUIRE_EQ(fixture.stats(bus).collisions, std::uint64_t{0});
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{0});
}

TEST_CASE("half-duplex: an answer inside the turn-around is reported")
{
    line_fixture fixture(3);
    const link_id bus =
        fixture.make_bus(half_duplex(5, collision_action::garble));

    fixture.send(0, "q"); // leaves the wire at 10 ms
    fixture.clock.advance(10ms);
    REQUIRE(fixture.heard(1, "q"));

    // 4 ms later the line has not been let go.
    fixture.clock.advance(4ms);
    fixture.send(1, "r");
    REQUIRE(fixture.faults(1));
    const auto fault = fixture.events.snapshot().front();
    REQUIRE(fault.fault == bus_fault::too_soon);
    REQUIRE_EQ(fault.endpoint, fixture.id(1));
    REQUIRE_EQ(
        fault.message,
        std::string(
            "sent 1 byte inside the turn-around time after p0: garbled"));

    // The poll had gone out whole; only the hasty answer is spoilt.
    fixture.clock.advance(10ms);
    REQUIRE(fixture.heard(0, inverted("r")));
    REQUIRE(fixture.heard(2, "q" + inverted("r")));
    REQUIRE_EQ(fixture.stats(bus).garbled, std::uint64_t{2});

    // Exactly the turn-around after that answer, the line is free.
    fixture.clock.advance(5ms);
    fixture.send(2, "s");
    fixture.clock.advance(10ms);
    REQUIRE(fixture.heard(0, inverted("r") + "s"));
    REQUIRE_EQ(fixture.stats(bus).collisions, std::uint64_t{1});
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{1});
}

TEST_CASE("half-duplex: RTS gates a member that has one and no other")
{
    hand_clock clock;
    auto hub = start_hub_on(clock);
    event_recorder events;
    event_filter faults;
    faults.kind = event_kind::bus_fault;
    const subscription_guard guard(*hub, faults,
                                   [events](const endpoint_event& event)
                                   { events.record(event); });

    const station gated = add_station(*hub, "gated");
    const station other = add_station(*hub, "other");
    const mock_endpoint plain = add_mock(*hub, "plain");
    const auto bus = hub->bus(
        {gated.endpoint->id(), other.endpoint->id(), plain.endpoint->id()},
        half_duplex(0, collision_action::report, true));
    REQUIRE(bus);

    // RTS is low: the transmitter is off and the bytes go nowhere.  They are
    // counted for each member that did not hear them, and it is said.
    gated.send(*hub, *bus, "lost");
    REQUIRE(wait_until([&] { return events.count(event_kind::bus_fault) == 1; },
                       5000ms));
    auto fault = events.snapshot().back();
    REQUIRE(fault.fault == bus_fault::transmitter_off);
    REQUIRE_EQ(fault.name, std::string("gated"));
    REQUIRE_EQ(
        fault.message,
        std::string("sent 4 bytes with RTS low: the bus did not carry them"));
    auto stats = vss_test::require_value(hub->find_link(*bus)).stats;
    REQUIRE_EQ(stats.dropped, std::uint64_t{8});
    REQUIRE_EQ(stats.bytes, std::uint64_t{0});
    REQUIRE_EQ(stats.collisions, std::uint64_t{0});

    // A member with no RTS is never muted, gating or not.
    send_on(*hub, *bus, 2, [&] { plain.inject_read("hi"); });
    clock.advance(20ms);
    REQUIRE(gated.heard("hi"));
    REQUIRE(other.heard("hi"));
    REQUIRE_EQ(events.count(event_kind::bus_fault), std::size_t{1});

    // With RTS raised the same member is heard.
    REQUIRE(!gated.endpoint->set_line(control_line::rts, true));
    gated.send(*hub, *bus, "sent");
    clock.advance(40ms);
    REQUIRE(other.heard("hisent"));
    REQUIRE(wait_until([&] { return plain.written() == "sent"; }, 5000ms));
    REQUIRE_EQ(events.count(event_kind::bus_fault), std::size_t{1});

    // While it keeps RTS high it holds the line against the others.
    send_on(*hub, *bus, 1, [&] { plain.inject_read("x"); });
    REQUIRE(wait_until([&] { return events.count(event_kind::bus_fault) == 2; },
                       5000ms));
    fault = events.snapshot().back();
    REQUIRE(fault.fault == bus_fault::line_held);
    REQUIRE_EQ(fault.name, std::string("plain"));
    REQUIRE_EQ(fault.message,
               std::string("sent 1 byte while gated held RTS high"));
    clock.advance(10ms);
    REQUIRE(other.heard("hisentx"));

    // Dropping RTS lets go of the line.
    REQUIRE(!gated.endpoint->set_line(control_line::rts, false));
    send_on(*hub, *bus, 1, [&] { plain.inject_read("y"); });
    clock.advance(10ms);
    REQUIRE(other.heard("hisentxy"));
    REQUIRE_EQ(events.count(event_kind::bus_fault), std::size_t{2});

    stats = vss_test::require_value(hub->find_link(*bus)).stats;
    REQUIRE_EQ(stats.dropped, std::uint64_t{8});
    REQUIRE_EQ(stats.collisions, std::uint64_t{1});
    REQUIRE_EQ(stats.garbled, std::uint64_t{0});
}

TEST_CASE("half-duplex: without RTS gating a low RTS mutes nobody")
{
    hand_clock clock;
    auto hub = start_hub_on(clock);
    const station first = add_station(*hub, "first");
    const station second = add_station(*hub, "second");
    const auto bus =
        hub->bus({first.endpoint->id(), second.endpoint->id()}, half_duplex());
    REQUIRE(bus);

    first.send(*hub, *bus, "free");
    clock.advance(40ms);
    REQUIRE(second.heard("free"));
    REQUIRE_EQ(vss_test::require_value(hub->find_link(*bus)).stats.dropped,
               std::uint64_t{0});
}

TEST_CASE("half-duplex: a sender removed in mid-chunk lets go of the line")
{
    line_fixture fixture(3);
    const link_id bus =
        fixture.make_bus(half_duplex(50, collision_action::garble));

    fixture.send(0, "unplugged"); // 90 ms on the wire
    fixture.clock.advance(10ms);
    REQUIRE(fixture.hub->remove_endpoint(fixture.id(0)));

    // The others send at once and cleanly: the line is free, turn-around and
    // all.  What the removed member had on the wire is never heard.
    fixture.send(1, "next");
    fixture.clock.advance(200ms);
    REQUIRE(fixture.heard(2, "next"));
    REQUIRE_EQ(fixture.ports[1].written(), std::string());
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{0});

    const auto info = fixture.hub->find_link(bus);
    REQUIRE(info);
    REQUIRE_EQ(info->sources.size(), std::size_t{2});
    REQUIRE_EQ(info->stats.collisions, std::uint64_t{0});
    REQUIRE_EQ(info->stats.garbled, std::uint64_t{0});
}

TEST_CASE("half-duplex: a receiver removed in mid-chunk does not stop the rest")
{
    line_fixture fixture(3);
    const link_id bus = fixture.make_bus(half_duplex());

    fixture.send(0, "still");
    REQUIRE(fixture.hub->remove_endpoint(fixture.id(2)));
    fixture.clock.advance(50ms);
    REQUIRE(fixture.heard(1, "still"));

    // The two that are left carry on as a half-duplex bus.
    fixture.send(1, "on");
    fixture.clock.advance(20ms);
    REQUIRE(fixture.heard(0, "on"));
    REQUIRE(fixture.hub->find_link(bus));
}

TEST_CASE("half-duplex: removing the bus drops what is on the wire")
{
    line_fixture fixture(3);
    const link_id bus = fixture.make_bus(half_duplex());

    fixture.send(0, "gone");
    REQUIRE(fixture.hub->disconnect(bus));
    fixture.clock.advance(1s);

    // A plain bus made of the same ports shows the hub is alive and that
    // nothing of the old wire arrives ahead of it.
    fixture.make_bus(link_options{});
    fixture.send(0, "new");
    REQUIRE(fixture.heard(1, "new"));
    REQUIRE(fixture.heard(2, "new"));
}

TEST_CASE("half-duplex: a member too far ahead of the line is dropped and said")
{
    line_fixture fixture(2);
    link_options options = half_duplex();
    options.half_duplex.bit_rate = 50; // 100 bytes take 20 s
    const link_id bus = fixture.make_bus(options);

    const std::string chunk(100, 'z');
    for (int sent = 0; sent < 4; ++sent)
        fixture.send(0, chunk);
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{0});

    // The fifth could not start for 80 s.
    fixture.send(0, chunk);
    REQUIRE(fixture.faults(1));
    const auto fault = fixture.events.snapshot().front();
    REQUIRE(fault.fault == bus_fault::overrun);
    REQUIRE_EQ(
        fault.message,
        std::string("sent 100 bytes too far ahead of the line rate: dropped"));
    auto stats = fixture.stats(bus);
    REQUIRE_EQ(stats.dropped, std::uint64_t{100});
    REQUIRE_EQ(stats.bytes, std::uint64_t{400});

    // What was carried arrives in full, in order.
    fixture.clock.advance(80s);
    REQUIRE(wait_until([&] { return fixture.ports[1].written().size() == 400; },
                       5000ms));
    REQUIRE_EQ(fixture.ports[1].written(), std::string(400, 'z'));
}

TEST_CASE("half-duplex: the link delay is added after the wire")
{
    line_fixture fixture(3);
    link_options options = half_duplex();
    options.delay_ms = 150;
    const link_id bus = fixture.make_bus(options);

    fixture.send(0, "late");
    // The delay is on the hub's clock, as the wire is, and starts when the
    // chunk leaves the wire.  With the chunk off the wire and the clock
    // still, no amount of real time delivers it.
    fixture.clock.advance(40ms);
    std::this_thread::sleep_for(50ms);
    REQUIRE_EQ(fixture.ports[1].written(), std::string());
    REQUIRE_EQ(fixture.ports[2].written(), std::string());

    // Moving the clock does.  It is moved a step at a time until the chunk
    // arrives, because when the chunk left the wire, and so when its delay
    // began, is the business of the wire's strand.
    REQUIRE(wait_until(
        [&]
        {
            fixture.clock.advance(10ms);
            return fixture.ports[1].written() == "late" &&
                   fixture.ports[2].written() == "late";
        },
        5000ms));
    REQUIRE_EQ(fixture.stats(bus).collisions, std::uint64_t{0});
}

TEST_CASE("half-duplex: on the steady clock a chunk is never heard early")
{
    auto hub = start_hub();
    const mock_endpoint a = add_mock(*hub, "a");
    const mock_endpoint b = add_mock(*hub, "b");
    link_options options = half_duplex();
    options.half_duplex.bit_rate = 50; // two bytes take 400 ms
    REQUIRE(hub->bus({a.endpoint->id(), b.endpoint->id()}, options));

    const auto sent = std::chrono::steady_clock::now();
    a.inject_read("ab");
    REQUIRE(wait_until([&] { return b.written() == "ab"; }, 10000ms));
    REQUIRE(std::chrono::steady_clock::now() - sent >= 400ms);
}

TEST_CASE("half-duplex: a plain bus beside it is untouched")
{
    line_fixture fixture(5);
    const auto line =
        fixture.hub->bus({fixture.id(0), fixture.id(1)},
                         half_duplex(0, collision_action::garble));
    const auto plain =
        fixture.hub->bus({fixture.id(2), fixture.id(3), fixture.id(4)});
    REQUIRE(line);
    REQUIRE(plain);

    // The clock never moves: the half-duplex bus holds its chunk, the plain
    // bus forwards at once, both senders together, unchanged.
    send_on(*fixture.hub, *line, 4,
            [&] { fixture.ports[0].inject_read("held"); });
    send_on(*fixture.hub, *plain, 3,
            [&] { fixture.ports[2].inject_read("now"); });
    REQUIRE(fixture.heard(4, "now"));
    send_on(*fixture.hub, *plain, 3,
            [&] { fixture.ports[3].inject_read("too"); });
    REQUIRE(fixture.heard(4, "nowtoo"));
    REQUIRE(fixture.heard(3, "now"));
    REQUIRE(fixture.heard(2, "too"));
    REQUIRE_EQ(fixture.ports[1].written(), std::string());

    REQUIRE_EQ(fixture.stats(*line).bytes, std::uint64_t{4});
    const auto stats = fixture.stats(*plain);
    REQUIRE_EQ(stats.bytes, std::uint64_t{12});
    REQUIRE_EQ(stats.collisions, std::uint64_t{0});
    REQUIRE_EQ(stats.garbled, std::uint64_t{0});
    REQUIRE_EQ(stats.dropped, std::uint64_t{0});
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{0});
}

TEST_CASE("half-duplex: senders in a storm lose no byte and count every one")
{
    // Several threads send at once on the steady clock, at the fastest line
    // rate, while a member is removed under them.  Whatever collides, each
    // byte reaches each remaining member once, whole or inverted, a chunk is
    // never part one and part the other, and the counters say how many of
    // each there were.  The sanitizer legs run this for the races.
    constexpr std::size_t members = 4;
    constexpr int messages = 200;

    auto hub = start_hub(4);
    std::vector<mock_endpoint> ports;
    std::vector<endpoint_id> ids;
    for (std::size_t index = 0; index < members; ++index)
    {
        ports.push_back(add_mock(*hub, "s" + std::to_string(index)));
        ids.push_back(ports.back().endpoint->id());
    }
    // A fifth member only listens, and is unplugged in the middle.
    const mock_endpoint leaver = add_mock(*hub, "leaver");
    ids.push_back(leaver.endpoint->id());

    link_options options = half_duplex(1, collision_action::garble);
    options.half_duplex.bit_rate = max_bus_bit_rate;
    const auto bus = hub->bus(ids, options);
    REQUIRE(bus);

    std::vector<std::thread> threads;
    threads.reserve(members + 1);
    for (std::size_t index = 0; index < members; ++index)
    {
        threads.emplace_back(
            [&ports, index]
            {
                const std::string chunk(3, static_cast<char>('a' + index));
                for (int count = 0; count < messages; ++count)
                    ports[index].inject_read(chunk);
            });
    }
    threads.emplace_back(
        [&hub, &leaver]
        { static_cast<void>(hub->remove_endpoint(leaver.endpoint->id())); });
    for (auto& thread : threads)
        thread.join();

    const std::size_t expected = (members - 1) * messages * 3;
    std::uint64_t spoilt = 0;
    for (std::size_t index = 0; index < members; ++index)
    {
        REQUIRE(wait_until(
            [&] { return ports[index].written().size() == expected; },
            20000ms));
        const std::string heard = ports[index].written();
        for (std::size_t other = 0; other < members; ++other)
        {
            const char whole = static_cast<char>('a' + other);
            const char broken =
                static_cast<char>(~static_cast<unsigned char>(whole));
            const auto count = static_cast<std::size_t>(
                std::count(heard.begin(), heard.end(), whole) +
                std::count(heard.begin(), heard.end(), broken));
            REQUIRE_EQ(count, other == index
                                  ? std::size_t{0}
                                  : static_cast<std::size_t>(messages) * 3);
            spoilt += static_cast<std::uint64_t>(
                std::count(heard.begin(), heard.end(), broken));
        }
        for (std::size_t at = 0; at < heard.size(); at += 3)
            REQUIRE(heard[at] == heard[at + 1] && heard[at] == heard[at + 2]);
    }

    // The leaver heard some of it before it went; the four who stayed
    // account for at most what the bus says it spoilt.
    const auto info = hub->find_link(*bus);
    REQUIRE(info);
    REQUIRE(info->stats.garbled >= spoilt);
    REQUIRE(info->stats.garbled <= spoilt + spoilt / (members - 1));
    REQUIRE_EQ(info->stats.dropped, std::uint64_t{0});
    if (spoilt != 0)
        REQUIRE(info->stats.collisions != 0);
}

TEST_CASE("half-duplex: a member added to the bus takes its turn on the line")
{
    line_fixture fixture(3);
    const link_id bus = fixture.make_bus(half_duplex());
    fixture.ports.push_back(add_mock(*fixture.hub, "late"));
    REQUIRE(fixture.hub->add_member(bus, fixture.id(3)));

    // It hears a chunk when the chunk has left the wire, like the others.
    fixture.send(0, "poll");
    REQUIRE_EQ(fixture.stats(bus).bytes, std::uint64_t{12});
    fixture.clock.advance(40ms - 1ns);
    REQUIRE_EQ(fixture.ports[3].written(), std::string());
    fixture.clock.advance(1ns);
    REQUIRE(fixture.heard(3, "poll"));

    // What it sends while another member is sending has collided.
    fixture.send(1, "aa");
    fixture.send(3, "bb");
    REQUIRE(fixture.faults(1));
    REQUIRE_EQ(fixture.stats(bus).collisions, std::uint64_t{1});
    fixture.clock.advance(1s);
    REQUIRE(fixture.heard(0, "aabb"));
    REQUIRE(fixture.heard(3, "pollaa"));
}

TEST_CASE("half-duplex: a member added in mid-chunk does not hear that chunk")
{
    line_fixture fixture(3);
    const link_id bus = fixture.make_bus(half_duplex());

    fixture.send(0, "early"); // 50 ms on the wire
    fixture.clock.advance(10ms);
    fixture.ports.push_back(add_mock(*fixture.hub, "late"));
    REQUIRE(fixture.hub->add_member(bus, fixture.id(3)));
    fixture.clock.advance(40ms);
    REQUIRE(fixture.heard(1, "early"));
    REQUIRE(fixture.heard(2, "early"));

    // The next chunk reaches it, and no part of the one before.
    fixture.send(1, "x");
    fixture.clock.advance(10ms);
    REQUIRE(fixture.heard(3, "x"));
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{0});
}

TEST_CASE("half-duplex: a member taken off in mid-chunk lets go of the line")
{
    line_fixture fixture(3);
    const link_id bus =
        fixture.make_bus(half_duplex(50, collision_action::garble));

    fixture.send(0, "unplugged"); // 90 ms on the wire
    fixture.clock.advance(10ms);
    REQUIRE(fixture.hub->remove_member(bus, fixture.id(0)));

    // The others send at once and cleanly: the line is free, turn-around and
    // all.  What the member had on the wire is never heard.
    fixture.send(1, "next");
    fixture.clock.advance(200ms);
    REQUIRE(fixture.heard(2, "next"));
    REQUIRE_EQ(fixture.ports[1].written(), std::string());
    REQUIRE_EQ(fixture.ports[0].written(), std::string());
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{0});

    // The endpoint is still in the hub, and off the bus.
    REQUIRE(fixture.hub->find_endpoint(fixture.id(0)));
    const auto info = fixture.hub->find_link(bus);
    REQUIRE(info);
    REQUIRE_EQ(info->sources.size(), std::size_t{2});
    REQUIRE_EQ(info->stats.collisions, std::uint64_t{0});
    REQUIRE_EQ(info->stats.garbled, std::uint64_t{0});
}

TEST_CASE("half-duplex: what a member sends as it is taken off is never heard")
{
    // The member is taken off while the hub may be anywhere in routing what
    // it just sent: not started, judged by the line, or on the wire.  In none
    // of them may the others hear it afterwards.
    for (int round = 0; round < 50; ++round)
    {
        line_fixture fixture(3);
        const link_id bus = fixture.make_bus(half_duplex());

        fixture.ports[0].inject_read("gone");
        if (round % 2 == 1)
            std::this_thread::yield();
        REQUIRE(fixture.hub->remove_member(bus, fixture.id(0)));

        // Long enough for anything left on the wire to leave it, and then a
        // chunk of another member behind it.
        fixture.clock.advance(1s);
        fixture.send(1, "k");
        fixture.clock.advance(1s);
        REQUIRE(fixture.heard(2, "k"));
        std::this_thread::sleep_for(2ms);
        REQUIRE_EQ(fixture.ports[2].written(), std::string("k"));
        REQUIRE_EQ(fixture.ports[1].written(), std::string());
        REQUIRE_EQ(fixture.stats(bus).collisions, std::uint64_t{0});
    }
}

TEST_CASE("half-duplex: the line of a bus is turned on and off in place")
{
    line_fixture fixture(3);
    const link_id bus = fixture.make_bus(link_options{});

    // A plain bus delivers at once.
    fixture.send(0, "a");
    REQUIRE(fixture.heard(1, "a"));
    REQUIRE(fixture.heard(2, "a"));

    // Turned on, a chunk waits for the wire.
    REQUIRE(fixture.hub->set_link_options(bus, half_duplex()));
    REQUIRE(fixture.info(bus).options.half_duplex.enabled);
    fixture.send(0, "poll");
    fixture.clock.advance(40ms - 1ns);
    REQUIRE_EQ(fixture.ports[1].written(), std::string("a"));
    fixture.clock.advance(1ns);
    REQUIRE(fixture.heard(1, "apoll"));
    REQUIRE(fixture.heard(2, "apoll"));

    // Turned off with a chunk on the wire: that chunk is discarded, and the
    // next one is delivered at once and is not passed by it later.
    fixture.send(1, "lost");
    REQUIRE(fixture.hub->set_link_options(bus, link_options{}));
    REQUIRE(!fixture.info(bus).options.half_duplex.enabled);
    fixture.send(0, "z");
    REQUIRE(fixture.heard(1, "apollz"));
    REQUIRE(fixture.heard(2, "apollz"));
    fixture.clock.advance(1s);
    REQUIRE_EQ(fixture.ports[0].written(), std::string());
    REQUIRE_EQ(fixture.ports[2].written(), std::string("apollz"));

    // The bus is the one that was made, with every byte it routed counted.
    const auto info = fixture.hub->find_link(bus);
    REQUIRE(info);
    REQUIRE_EQ(info->id, bus);
    REQUIRE_EQ(info->stats.bytes, std::uint64_t{20});
}

TEST_CASE("half-duplex: new values make a new line and bad ones are refused")
{
    line_fixture fixture(3);
    const link_id bus = fixture.make_bus(half_duplex());

    fixture.send(0, "aa");
    fixture.send(1, "bb");
    REQUIRE(fixture.faults(1));
    REQUIRE_EQ(fixture.stats(bus).collisions, std::uint64_t{1});
    const std::uint64_t routed = fixture.stats(bus).bytes;

    // Values outside the limits change nothing.
    link_options bad = half_duplex();
    bad.half_duplex.bit_rate = min_bus_bit_rate - 1;
    const auto refused = fixture.hub->set_link_options(bus, bad);
    REQUIRE(!refused);
    REQUIRE_EQ(refused.error(), make_error_code(errc::invalid_argument));
    REQUIRE_EQ(fixture.info(bus).options.half_duplex.bit_rate,
               std::uint32_t{1000});
    REQUIRE_EQ(fixture.stats(bus).collisions, std::uint64_t{1});

    // The same values keep the line and what it counted.
    REQUIRE(fixture.hub->set_link_options(bus, half_duplex()));
    REQUIRE_EQ(fixture.stats(bus).collisions, std::uint64_t{1});

    // A new rate is a new line: it starts free and counts from zero, and
    // what the old wire carried is never heard.
    link_options faster = half_duplex();
    faster.half_duplex.bit_rate = 2000;
    REQUIRE(fixture.hub->set_link_options(bus, faster));
    REQUIRE_EQ(fixture.info(bus).options.half_duplex.bit_rate,
               std::uint32_t{2000});
    REQUIRE_EQ(fixture.stats(bus).collisions, std::uint64_t{0});
    REQUIRE_EQ(fixture.stats(bus).bytes, routed);
    fixture.send(2, "ok"); // 10 ms at the new rate
    fixture.clock.advance(10ms);
    REQUIRE(fixture.heard(0, "ok"));
    REQUIRE(fixture.heard(1, "ok"));
    REQUIRE_EQ(fixture.events.count(event_kind::bus_fault), std::size_t{1});
    fixture.clock.advance(1s);
    REQUIRE_EQ(fixture.ports[2].written(), std::string());

    // Only a bus has a line to turn on.
    const auto left = add_mock(*fixture.hub, "left");
    const auto right = add_mock(*fixture.hub, "right");
    const auto pair =
        fixture.hub->connect(left.endpoint->id(), right.endpoint->id());
    REQUIRE(pair);
    const auto other = fixture.hub->set_link_options(*pair, half_duplex());
    REQUIRE(!other);
    REQUIRE_EQ(other.error(), make_error_code(errc::unsupported));
}
