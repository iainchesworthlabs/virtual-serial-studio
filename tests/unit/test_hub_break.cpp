//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// A break is an item of the stream, in order with the bytes around it: it
// takes the same routes, delay lines, monitors and buses as bytes
// (docs/break-event-plan.md).

#include "support/test_support.hpp"

#include <vss/core/hub.hpp>
#include <vss/core/line_break.hpp>
#include <vss/ports/loopback_transport.hpp>
#include <vss/testkit/harness.hpp>

#include <boost/asio/post.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace
{
using namespace std::chrono_literals;

/// A port whose reads are in the test's hands: bytes and breaks injected on
/// it are read in the order they were injected.
struct source_port
{
    std::shared_ptr<endpoint> endpoint;
    std::shared_ptr<loopback_transport> transport;

    void bytes(const std::string& text) const
    {
        transport->inject(bytes_view(
            reinterpret_cast<const byte*>(text.data()), text.size()));
    }

    void make_break(std::chrono::nanoseconds length,
                    bool measured = true) const
    {
        line_break item;
        item.phase = break_phase::complete;
        item.start = clock_type::now();
        item.duration = length;
        item.measured = measured;
        transport->inject_break(item);
    }

    void begin() const
    {
        line_break item;
        item.phase = break_phase::begin;
        item.start = clock_type::now();
        item.measured = true;
        transport->inject_break(item);
    }

    void end(std::chrono::nanoseconds length) const
    {
        line_break item;
        item.phase = break_phase::end;
        item.start = clock_type::now() - length;
        item.duration = length;
        item.measured = true;
        transport->inject_break(item);
    }
};

source_port add_source(vss::hub& hub, const std::string& name)
{
    auto transport = loopback_transport::create(hub.executor(), name);
    auto added = hub.add_endpoint(name, endpoint_kind::virtual_port, transport);
    REQUIRE(added);
    return source_port{*added, std::move(transport)};
}

/// Everything the hub published, for the test to read back.
struct watch
{
    event_recorder events;
    subscription_guard guard;

    explicit watch(vss::hub& hub)
        : guard(hub, event_filter{},
                [recorder = events](const endpoint_event& event)
                { recorder.record(event); })
    {
    }

    /// What an endpoint wrote out, as one string: bytes as they are, and a
    /// break as `[break]` (`[begin]`, `[end]` for the halves).
    std::string written_by(endpoint_id id) const
    {
        std::string out;
        for (const auto& event : events.snapshot())
        {
            if (event.endpoint != id || event.direction != data_direction::tx)
                continue;
            if (event.kind == event_kind::data && event.data)
                out += to_string(bytes_view(*event.data));
            else if (event.kind == event_kind::line_break)
                out += std::string("[") +
                       (event.phase == break_phase::complete
                            ? "break"
                            : to_string(event.phase)) +
                       "]";
        }
        return out;
    }

    /// What an endpoint read, in the same form.
    std::string read_by(endpoint_id id) const
    {
        std::string out;
        for (const auto& event : events.snapshot())
        {
            if (event.endpoint != id || event.direction != data_direction::rx)
                continue;
            if (event.kind == event_kind::data && event.data)
                out += to_string(bytes_view(*event.data));
            else if (event.kind == event_kind::line_break)
                out += std::string("[") +
                       (event.phase == break_phase::complete
                            ? "break"
                            : to_string(event.phase)) +
                       "]";
        }
        return out;
    }

    std::size_t breaks_written_by(endpoint_id id) const
    {
        std::size_t count = 0;
        for (const auto& event : events.snapshot())
        {
            if (event.endpoint == id && event.kind == event_kind::line_break &&
                event.direction == data_direction::tx)
                ++count;
        }
        return count;
    }

    std::vector<endpoint_event> breaks() const
    {
        std::vector<endpoint_event> out;
        for (const auto& event : events.snapshot())
        {
            if (event.kind == event_kind::line_break)
                out.push_back(event);
        }
        return out;
    }
};

link_options delayed(std::uint32_t delay_ms)
{
    link_options options;
    options.delay_ms = delay_ms;
    return options;
}

} // namespace

TEST_CASE("break: it is a part of the stream between two chunks across a link with delay")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto to = add_mock(*hub, "to");
    REQUIRE(hub->redirect(from.endpoint->id(), to.endpoint->id(), delayed(80)));

    const auto sent = std::chrono::steady_clock::now();
    from.bytes("AB");
    from.make_break(2ms);
    from.bytes("CD");

    REQUIRE(wait_until([&] { return seen.written_by(to.endpoint->id()) ==
                                    "AB[break]CD"; },
                       5000ms));
    // The break waited for the delay, as the bytes before it did.
    REQUIRE(std::chrono::steady_clock::now() - sent >= 80ms);
    REQUIRE_EQ(to.written(), std::string("ABCD"));

    // What the receiving port read from its device is not touched, and the
    // source read the same as it was sent.
    REQUIRE_EQ(seen.read_by(from.endpoint->id()), std::string("AB[break]CD"));
}

TEST_CASE("break: the length and the stamps arrive with it")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto to = add_mock(*hub, "to");
    REQUIRE(hub->redirect(from.endpoint->id(), to.endpoint->id()));

    from.make_break(88us, false);
    REQUIRE(wait_until([&] { return seen.breaks_written_by(to.endpoint->id()) == 1; },
                       5000ms));

    for (const auto& event : seen.breaks())
    {
        REQUIRE_EQ(event.phase, break_phase::complete);
        REQUIRE_EQ(event.duration, std::chrono::nanoseconds(88us));
        REQUIRE(!event.measured);
        REQUIRE(event.start != time_point{});
        REQUIRE(event.when == event.start);
    }
}

TEST_CASE("break: a begin and an end are carried in order, and a break is counted once")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto to = add_mock(*hub, "to");
    const auto link =
        hub->redirect(from.endpoint->id(), to.endpoint->id(), delayed(20));
    REQUIRE(link);

    from.bytes("x");
    from.begin();
    from.bytes("y");
    from.end(10ms);
    from.bytes("z");

    REQUIRE(wait_until([&] { return seen.written_by(to.endpoint->id()) ==
                                    "x[begin]y[end]z"; },
                       5000ms));

    // The begin is not a break of its own: both ends count one.
    REQUIRE(wait_until([&] { return to.endpoint->stats().breaks_tx == 1; },
                       5000ms));
    REQUIRE_EQ(from.endpoint->stats().breaks_rx, std::uint64_t{1});
    REQUIRE_EQ(hub->find_link(*link)->stats.breaks, std::uint64_t{1});
    // A break is not bytes.
    REQUIRE_EQ(hub->find_link(*link)->stats.bytes, std::uint64_t{3});
    REQUIRE_EQ(to.endpoint->stats().bytes_tx, std::uint64_t{3});

    // The end says how long the break lasted.
    const auto events = seen.breaks();
    for (const auto& event : events)
    {
        if (event.phase == break_phase::end)
            REQUIRE_EQ(event.duration, std::chrono::nanoseconds(10ms));
    }
}

TEST_CASE("break: every listener of a split hears it once")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto one = add_mock(*hub, "one");
    const auto two = add_mock(*hub, "two");
    const auto three = add_mock(*hub, "three");
    REQUIRE(hub->split(from.endpoint->id(),
                       {one.endpoint->id(), two.endpoint->id(),
                        three.endpoint->id()}));

    from.bytes("a");
    from.make_break(1ms);
    from.bytes("b");

    for (const auto* target : {&one, &two, &three})
    {
        REQUIRE(wait_until([&] { return seen.written_by(target->endpoint->id()) ==
                                        "a[break]b"; },
                           5000ms));
    }
}

TEST_CASE("break: a chain of two-way links shows it once to each port and not to its maker")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto a = add_mock(*hub, "a");
    const auto b = add_mock(*hub, "b");
    const auto c = add_mock(*hub, "c");
    REQUIRE(hub->connect(a.endpoint->id(), b.endpoint->id(), delayed(10)));
    REQUIRE(hub->connect(b.endpoint->id(), c.endpoint->id()));

    make_break(*a.endpoint, 1ms);
    REQUIRE(wait_until([&] { return seen.breaks_written_by(b.endpoint->id()) == 1 &&
                                    seen.breaks_written_by(c.endpoint->id()) == 1; },
                       5000ms));
    // Nothing comes back to where it was made, and nothing arrives twice.
    std::this_thread::sleep_for(100ms);
    REQUIRE_EQ(seen.breaks_written_by(a.endpoint->id()), std::size_t{0});
    REQUIRE_EQ(seen.breaks_written_by(b.endpoint->id()), std::size_t{1});
    REQUIRE_EQ(seen.breaks_written_by(c.endpoint->id()), std::size_t{1});
}

TEST_CASE("break: where two paths meet a port sees the break once, with delay on one")
{
    // The ring: a to b, b to c and a to c.  c is reached by two paths, one of
    // them slower.
    auto hub = start_hub();
    watch seen(*hub);
    const auto a = add_mock(*hub, "a");
    const auto b = add_mock(*hub, "b");
    const auto c = add_mock(*hub, "c");
    REQUIRE(hub->redirect(a.endpoint->id(), b.endpoint->id()));
    REQUIRE(hub->redirect(b.endpoint->id(), c.endpoint->id(), delayed(30)));
    REQUIRE(hub->redirect(a.endpoint->id(), c.endpoint->id(), delayed(60)));

    make_break(*a.endpoint, 1ms);
    REQUIRE(wait_until([&] { return seen.breaks_written_by(b.endpoint->id()) == 1 &&
                                    seen.breaks_written_by(c.endpoint->id()) >= 1; },
                       5000ms));
    std::this_thread::sleep_for(200ms);
    REQUIRE_EQ(seen.breaks_written_by(a.endpoint->id()), std::size_t{0});
    REQUIRE_EQ(seen.breaks_written_by(b.endpoint->id()), std::size_t{1});
    REQUIRE_EQ(seen.breaks_written_by(c.endpoint->id()), std::size_t{1});
}

TEST_CASE("break: a monitor copies it by its direction")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto other = add_source(*hub, "other");
    const auto port = add_source(*hub, "port");
    const auto sink = add_mock(*hub, "sink");
    const auto all = add_mock(*hub, "all");
    const auto transmit = add_mock(*hub, "transmit");
    const auto receive = add_mock(*hub, "receive");
    // `other` sends to `port`; `port` sends to `sink`.
    REQUIRE(hub->redirect(other.endpoint->id(), port.endpoint->id()));
    REQUIRE(hub->redirect(port.endpoint->id(), sink.endpoint->id()));

    link_options options;
    options.direction = monitor_direction::both;
    REQUIRE(hub->monitor(port.endpoint->id(), all.endpoint->id(), options));
    options.direction = monitor_direction::transmit;
    REQUIRE(hub->monitor(port.endpoint->id(), transmit.endpoint->id(), options));
    options.direction = monitor_direction::receive;
    REQUIRE(hub->monitor(port.endpoint->id(), receive.endpoint->id(), options));

    // The program on `port` sends a break: what the port reads.
    port.bytes("1");
    port.make_break(1ms);
    REQUIRE(wait_until([&] { return seen.written_by(all.endpoint->id()) == "1[break]" &&
                                    seen.written_by(transmit.endpoint->id()) == "1[break]"; },
                       5000ms));
    REQUIRE_EQ(seen.written_by(sink.endpoint->id()), std::string("1[break]"));
    REQUIRE_EQ(seen.written_by(receive.endpoint->id()), std::string());

    // A break a link brings into the port.
    other.make_break(1ms);
    REQUIRE(wait_until([&] { return seen.written_by(receive.endpoint->id()) == "[break]" &&
                                    seen.written_by(all.endpoint->id()) == "1[break][break]"; },
                       5000ms));
    std::this_thread::sleep_for(100ms);
    REQUIRE_EQ(seen.written_by(transmit.endpoint->id()), std::string("1[break]"));
    // It is carried on along the links of `port`, as bytes are.
    REQUIRE(wait_until([&] { return seen.written_by(sink.endpoint->id()) ==
                                    "1[break][break]"; },
                       5000ms));
}

TEST_CASE("break: a monitor with no modem lines still gets it")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    const auto copy = add_mock(*hub, "copy");
    link_options options;
    options.emulate_modem_lines = false;
    REQUIRE(hub->monitor(from.endpoint->id(), copy.endpoint->id(), options));
    from.make_break(1ms);
    REQUIRE(wait_until([&] { return seen.breaks_written_by(copy.endpoint->id()) == 1; },
                       5000ms));
}

namespace
{

/// A port that cannot show a break to its program: the default of a
/// transport.
class deaf_transport final : public transport
{
public:
    explicit deaf_transport(const executor_type& executor)
        : executor_(executor)
    {
    }

    std::string describe() const override { return "deaf transport"; }
    bool is_open() const override { return open_; }

    void async_read_some(boost::asio::mutable_buffer, io_handler handler) override
    {
        parked_ = std::move(handler);
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        const std::size_t size = buffer.size();
        boost::asio::post(executor_,
                          [handler = std::move(handler), size]
                          { handler({}, size); });
    }

    void close() override { open_ = false; }
    void cancel() override {}

private:
    executor_type executor_;
    io_handler parked_;
    bool open_ = true;
};

} // namespace

TEST_CASE("break: a port that cannot show one says so once and counts it dropped")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto from = add_source(*hub, "from");
    auto added = hub->add_endpoint("deaf", endpoint_kind::virtual_port,
                                   std::make_shared<deaf_transport>(hub->executor()));
    REQUIRE(added);
    const auto deaf = *added;
    REQUIRE(hub->redirect(from.endpoint->id(), deaf->id()));

    from.bytes("a");
    from.make_break(1ms);
    from.make_break(1ms);
    from.bytes("b");

    // The bytes go on, in order, past the breaks that were not delivered.
    REQUIRE(wait_until([&] { return seen.written_by(deaf->id()) == "ab"; }, 5000ms));
    REQUIRE(wait_until([&] { return deaf->stats().dropped == 2; }, 5000ms));
    REQUIRE_EQ(deaf->stats().breaks_tx, std::uint64_t{0});

    std::size_t notices = 0;
    for (const auto& event : seen.events.snapshot())
    {
        if (event.endpoint == deaf->id() &&
            event.kind == event_kind::log_message &&
            event.message.find("break not delivered") != std::string::npos)
            ++notices;
    }
    REQUIRE_EQ(notices, std::size_t{1});
}

TEST_CASE("break: an owner link forwards a break only for its owner")
{
    auto hub = start_hub();
    watch seen(*hub);
    const auto first = add_source(*hub, "first");
    const auto second = add_source(*hub, "second");
    const auto to = add_mock(*hub, "to");
    link_options options;
    options.policy = merge_policy::owner;
    REQUIRE(hub->join({first.endpoint->id(), second.endpoint->id()},
                      to.endpoint->id(), options));

    first.make_break(1ms);
    REQUIRE(wait_until([&] { return seen.breaks_written_by(to.endpoint->id()) == 1; },
                       5000ms));
    second.make_break(1ms);
    std::this_thread::sleep_for(150ms);
    REQUIRE_EQ(seen.breaks_written_by(to.endpoint->id()), std::size_t{1});
}
