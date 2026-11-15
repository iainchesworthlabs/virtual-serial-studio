//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/scripted_transport.hpp"
#include "support/test_support.hpp"

#include <vss/core/hub.hpp>
#include <vss/core/transport.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace {
using namespace std::chrono_literals;
}

TEST_CASE("endpoint: starts open and reports its identity")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    endpoint& port = *mock.endpoint;
    REQUIRE(port.is_open());
    REQUIRE_EQ(port.state(), endpoint_state::open);
    REQUIRE_EQ(port.name(), std::string("device"));
    REQUIRE_EQ(port.kind(), endpoint_kind::mock);
    REQUIRE(port.id() != 0);

    const endpoint_info info = port.info();
    REQUIRE_EQ(info.id, port.id());
    REQUIRE_EQ(info.name, std::string("device"));
    REQUIRE_EQ(info.kind, endpoint_kind::mock);
    REQUIRE(info.open);
    REQUIRE(!info.supports_control_lines);
    REQUIRE_EQ(info.stats.bytes_rx, std::uint64_t{0});
    REQUIRE_EQ(port.public_path(), std::string("device"));
    REQUIRE(port.description().find("mock") != std::string::npos);
}

TEST_CASE("endpoint: writes reach the transport and count bytes")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    mock.endpoint->write(as_bytes("PING"));
    REQUIRE(wait_until(
        [&] { return mock.written() == "PING"; }, 2000ms));
    REQUIRE(wait_until(
        [&] { return mock.endpoint->stats().bytes_tx == 4; }, 2000ms));
    REQUIRE_EQ(mock.endpoint->stats().write_ops, std::uint64_t{1});
    REQUIRE(mock.endpoint->stats().last_activity != time_point{});
}

TEST_CASE("endpoint: injected data is received and routed")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    mock.inject_read("PONG");
    REQUIRE(wait_for_rx(*mock.endpoint, 4, 2000ms));
    REQUIRE_EQ(mock.endpoint->stats().bytes_rx, std::uint64_t{4});
    REQUIRE_EQ(mock.endpoint->stats().read_ops, std::uint64_t{1});
}

TEST_CASE("endpoint: data events carry payload in both directions")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    event_recorder recorder;
    hub->subscribe({}, [recorder](const endpoint_event& event) { recorder.record(event); });

    mock.inject_read("in");
    mock.endpoint->write(as_bytes("out"));

    REQUIRE(wait_until(
        [&] { return recorder.data_ascii(data_direction::rx) == "in"; }, 2000ms));
    REQUIRE(wait_until(
        [&] { return recorder.data_ascii(data_direction::tx) == "out"; }, 2000ms));

    const auto events = recorder.snapshot();
    REQUIRE(!events.empty());
    REQUIRE_EQ(events.front().name, std::string("device"));
}

TEST_CASE("endpoint: capture can be disabled")
{
    auto hub = start_hub();
    endpoint_options options = test_options();
    options.capture_traffic = false;

    boost::system::error_code ec;
    auto transport = serial_transport::make_mock(hub->executor(), serial_settings{},
                                                 "quiet", ec);
    REQUIRE(!ec);
    auto added = hub->add_endpoint("quiet", endpoint_kind::mock, transport, options);
    REQUIRE(added);

    event_recorder recorder;
    hub->subscribe({}, [recorder](const endpoint_event& event) { recorder.record(event); });

    mock_endpoint{*added, transport}.inject_read("data");
    REQUIRE(wait_for_rx(**added, 4, 2000ms));
    REQUIRE_EQ(recorder.count(event_kind::data), std::size_t{0});
    REQUIRE_EQ(hub->traffic().size(), std::size_t{0});
}

TEST_CASE("endpoint: write_sync completes with the byte count")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    const auto written = mock.endpoint->write_sync(as_bytes("hello"), 2000ms);
    REQUIRE(written);
    REQUIRE_EQ(*written, std::size_t{5});
    REQUIRE_EQ(mock.written(), std::string("hello"));
}

TEST_CASE("endpoint: write_sync reports timeouts")
{
    auto hub = start_hub();
    auto transport = std::make_shared<stalled_transport>(hub->executor());
    auto added = hub->add_endpoint("stalled", endpoint_kind::mock, transport,
                                   test_options());
    REQUIRE(added);

    const auto written = (*added)->write_sync(as_bytes("data"), 50ms);
    REQUIRE(!written);
    REQUIRE_EQ(written.error(), make_error_code(errc::timeout));
}

TEST_CASE("endpoint: a full write queue rejects new data")
{
    auto hub = start_hub();

    endpoint_options options = test_options();
    options.max_write_queue_bytes = 4;
    options.capture_traffic = false;

    auto transport = std::make_shared<stalled_transport>(hub->executor());
    auto added = hub->add_endpoint("stalled", endpoint_kind::mock, transport, options);
    REQUIRE(added);
    endpoint& port = **added;

    port.write(byte_buffer{1, 2, 3, 4});
    REQUIRE(wait_until([&] { return port.stats().bytes_tx == 0; }, 500ms));

    // The handler runs on a hub thread, so the result travels through a promise.
    std::promise<boost::system::error_code> result;
    auto outcome = result.get_future();
    port.write(byte_buffer{5, 6, 7, 8},
               [&result](boost::system::error_code ec, std::size_t) {
                   result.set_value(ec);
               });

    REQUIRE(outcome.wait_for(2000ms) == std::future_status::ready);
    REQUIRE_EQ(outcome.get(), make_error_code(errc::queue_full));
    REQUIRE_EQ(port.stats().dropped, std::uint64_t{4});
}

TEST_CASE("endpoint: empty writes without a handler are no-ops")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    mock.endpoint->write(bytes_view{});
    mock.endpoint->write(as_bytes(""));
    mock.endpoint->start();
    REQUIRE_EQ(mock.endpoint->state(), endpoint_state::open);
    REQUIRE_EQ(mock.endpoint->stats().bytes_tx, std::uint64_t{0});
}

TEST_CASE("endpoint: write_sync on an empty buffer completes immediately")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");
    const auto written = mock.endpoint->write_sync(bytes_view{}, 100ms);
    REQUIRE(written);
    REQUIRE_EQ(*written, std::size_t{0});
}

TEST_CASE("endpoint: a read error moves the endpoint to failed")
{
    auto hub = start_hub();
    auto transport = std::make_shared<failing_transport>(
        hub->executor(), make_error_code(errc::io_error));
    auto added = hub->add_endpoint("failing", endpoint_kind::mock,
                                   std::move(transport), test_options());
    REQUIRE(added);

    REQUIRE(wait_until(
        [&] { return (*added)->state() == endpoint_state::failed; }, 2000ms));
    REQUIRE(!(*added)->is_open());
    REQUIRE_EQ((*added)->stats().errors, std::uint64_t{1});

    // The base transport defaults: no OS path, no control lines.
    REQUIRE_EQ((*added)->public_path(), std::string());
    REQUIRE(!(*added)->info().supports_control_lines);
    (*added)->refresh_lines();
    REQUIRE_EQ((*added)->lines().bits, 0u);
    REQUIRE_EQ((*added)->set_line(control_line::dtr, true),
               make_error_code(errc::unsupported));
}

TEST_CASE("endpoint: monitor links do not propagate control lines")
{
    auto hub = start_hub();
    auto first = add_loopback(*hub, "source");
    auto second = add_loopback(*hub, "tap");

    REQUIRE(hub->monitor(first->id(), second->id()));

    const control_lines before = second->lines();
    REQUIRE(!first->set_line(control_line::dtr, true));
    REQUIRE(wait_until([&] { return first->lines().dtr(); }, 2000ms));

    // The tap observes the data but its modem inputs are untouched.
    REQUIRE_EQ(second->lines().bits, before.bits);
}

TEST_CASE("endpoint: a link wiring decides which lines the peer sees")
{
    auto hub = start_hub();
    auto first = add_loopback(*hub, "first");
    auto second = add_loopback(*hub, "second");

    // The default is the null-modem cable.
    const auto linked = hub->connect(first->id(), second->id());
    REQUIRE(linked);
    REQUIRE(!first->set_line(control_line::dtr, true));
    REQUIRE(wait_until(
        [&] { return second->lines().dsr() && second->lines().dcd(); },
        2000ms));
    REQUIRE(!second->lines().ri());

    // Rewiring DTR to RI takes effect at once, without another change on the
    // sending side, and drops what the old wires carried.
    modem_wiring wiring;
    wiring.dtr_to_dsr = false;
    wiring.dtr_to_dcd = false;
    wiring.dtr_to_ri = true;
    REQUIRE(hub->set_link_wiring(*linked, wiring));
    REQUIRE(wait_until(
        [&] {
            return second->lines().ri() && !second->lines().dsr() &&
                   !second->lines().dcd();
        },
        2000ms));
    REQUIRE(vss_test::require_value(hub->find_link(*linked)).options.wiring == wiring);

    // The same wiring applies in the other direction of a duplex link.
    REQUIRE(!second->set_line(control_line::dtr, true));
    REQUIRE(wait_until([&] { return first->lines().ri(); }, 2000ms));

    REQUIRE_EQ(hub->set_link_wiring(link_id{9999}, wiring),
               make_error_code(errc::not_found));
}

TEST_CASE("endpoint: peer close is graceful")
{
    auto hub = start_hub();

    // The transport reports end of file as soon as the endpoint starts, so
    // subscribe first or the stopped event can be published before anyone is
    // listening.
    event_recorder recorder;
    hub->subscribe({}, [recorder](const endpoint_event& event) { recorder.record(event); });

    auto transport = std::make_shared<failing_transport>(
        hub->executor(), boost::asio::error::eof);
    auto added = hub->add_endpoint("closing", endpoint_kind::mock,
                                   std::move(transport), test_options());
    REQUIRE(added);

    REQUIRE(wait_until(
        [&] { return (*added)->state() == endpoint_state::closed; }, 2000ms));
    REQUIRE(wait_until(
        [&] { return recorder.count(event_kind::stopped) >= 1; }, 2000ms));
}

TEST_CASE("endpoint: stop closes the transport and publishes stopped")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    event_recorder recorder;
    hub->subscribe({}, [recorder](const endpoint_event& event) { recorder.record(event); });

    mock.endpoint->stop();
    REQUIRE(wait_until(
        [&] { return mock.endpoint->state() == endpoint_state::closed; }, 2000ms));
    REQUIRE(wait_until(
        [&] { return recorder.count(event_kind::stopped) >= 1; }, 2000ms));
}

TEST_CASE("endpoint: control lines are unsupported on the mock backend")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    REQUIRE(!mock.endpoint->info().supports_control_lines);
    const auto result = mock.endpoint->set_line(control_line::dtr, true);
    REQUIRE_EQ(result, make_error_code(errc::unsupported));
}

TEST_CASE("endpoint: loopback control lines publish events")
{
    auto hub = start_hub();
    auto port = add_loopback(*hub, "virtual");

    REQUIRE(port->info().supports_control_lines);

    event_recorder recorder;
    event_filter filter;
    filter.endpoint = port->id();
    hub->subscribe(filter, [recorder](const endpoint_event& event) {
        recorder.record(event);
    });

    REQUIRE(!port->set_line(control_line::dtr, true));
    REQUIRE(port->lines().dtr());
    REQUIRE(wait_until(
        [&] { return recorder.count(event_kind::lines) >= 1; }, 2000ms));

    port->refresh_lines();
    REQUIRE(port->lines().dtr());
}

TEST_CASE("endpoint: peer inputs merge into the line state")
{
    auto hub = start_hub();
    auto port = add_loopback(*hub, "virtual");

    control_lines inputs;
    inputs.set(control_line::cts, false);
    inputs.set(control_line::dsr, false);
    inputs.set(control_line::dcd, false);

    port->set_peer_inputs(inputs);
    REQUIRE(!port->lines().cts());
    REQUIRE(!port->lines().dsr());
    REQUIRE(!port->lines().dcd());
    REQUIRE(!port->lines().ri());

    port->set_peer_inputs(control_lines{});
    REQUIRE(!port->lines().cts());
}

TEST_CASE("endpoint: reset_stats keeps the start time")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    mock.endpoint->write(as_bytes("abc"));
    REQUIRE(wait_until(
        [&] { return mock.endpoint->stats().bytes_tx == 3; }, 2000ms));

    const time_point started = mock.endpoint->stats().started;
    mock.endpoint->reset_stats();

    REQUIRE_EQ(mock.endpoint->stats().bytes_tx, std::uint64_t{0});
    REQUIRE_EQ(mock.endpoint->stats().started, started);
}

TEST_CASE("endpoint: partial writes continue until complete")
{
    auto hub = start_hub();

    endpoint_options options = test_options();
    options.capture_traffic = true;
    auto transport = std::make_shared<partial_transport>(hub->executor());
    auto added = hub->add_endpoint("partial", endpoint_kind::mock, transport, options);
    REQUIRE(added);

    const auto written = (*added)->write_sync(as_bytes("abcdefgh"), 2000ms);
    REQUIRE(written);
    REQUIRE_EQ(*written, std::size_t{8});
    REQUIRE(wait_until(
        [&] { return (*added)->stats().bytes_tx == 8; }, 2000ms));
    REQUIRE((*added)->stats().write_ops >= 2);
}

TEST_CASE("endpoint: close and repeated stop are safe")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    mock.endpoint->write(as_bytes("data"));
    REQUIRE(wait_until(
        [&] { return mock.endpoint->stats().bytes_tx == 4; }, 2000ms));

    mock.endpoint->close();
    REQUIRE_EQ(mock.endpoint->state(), endpoint_state::closed);
    REQUIRE(!mock.endpoint->is_open());

    // stop() after a close has nothing left to do.
    mock.endpoint->stop();
    mock.endpoint->stop();
    REQUIRE_EQ(mock.endpoint->state(), endpoint_state::closed);
}

TEST_CASE("endpoint: write_sync without a timeout waits for completion")
{
    auto hub = start_hub();
    auto mock = add_mock(*hub, "device");

    const auto written = mock.endpoint->write_sync(as_bytes("blocking"), 0ms);
    REQUIRE(written);
    REQUIRE_EQ(*written, std::size_t{8});
}

TEST_CASE("endpoint: a reset connection is graceful")
{
    auto hub = start_hub();

    // Subscribe before the endpoint starts; see the peer close test.
    event_recorder recorder;
    hub->subscribe({}, [recorder](const endpoint_event& event) { recorder.record(event); });

    auto transport = std::make_shared<failing_transport>(
        hub->executor(), boost::asio::error::connection_reset);
    auto added = hub->add_endpoint("reset", endpoint_kind::mock,
                                   std::move(transport), test_options());
    REQUIRE(added);

    REQUIRE(wait_until(
        [&] { return (*added)->state() == endpoint_state::closed; }, 2000ms));
    REQUIRE(wait_until(
        [&] { return recorder.count(event_kind::stopped) >= 1; }, 2000ms));
}

TEST_CASE("endpoint: a broken pipe is graceful")
{
    auto hub = start_hub();
    auto transport = std::make_shared<failing_transport>(
        hub->executor(), boost::asio::error::broken_pipe);
    auto added = hub->add_endpoint("broken", endpoint_kind::mock,
                                   std::move(transport), test_options());
    REQUIRE(added);

    REQUIRE(wait_until(
        [&] { return (*added)->state() == endpoint_state::closed; }, 2000ms));
}

TEST_CASE("endpoint: starting on a closed transport fails immediately")
{
    auto hub = start_hub();
    auto transport = loopback_transport::create(hub->executor(), "closed");
    transport->close();

    auto added = hub->add_endpoint("closed", endpoint_kind::virtual_port,
                                   std::move(transport), test_options());
    REQUIRE(added);
    REQUIRE_EQ((*added)->state(), endpoint_state::failed);
    REQUIRE(!(*added)->is_open());
}

TEST_CASE("endpoint: peer inputs are idempotent")
{
    auto hub = start_hub();
    auto port = add_loopback(*hub, "virtual");

    control_lines inputs;
    inputs.set(control_line::cts, false);
    port->set_peer_inputs(inputs);
    REQUIRE(!port->lines().cts());

    // Applying the same inputs again must be a no-op.
    port->set_peer_inputs(inputs);
    REQUIRE(!port->lines().cts());

    inputs.set(control_line::cts, true);
    port->set_peer_inputs(inputs);
    REQUIRE(port->lines().cts());
}

TEST_CASE("hub: destroying the hub releases an idle endpoint")
{
    auto hub = start_hub();
    std::weak_ptr<endpoint> weak;
    {
        auto port = add_loopback(*hub, "idle");
        weak = port;

        // The first read is armed on the strand. Once this marker has run, the
        // loopback transport holds the pending read.
        std::promise<void> armed;
        auto ready = armed.get_future();
        boost::asio::post(port->get_executor(), [&armed] { armed.set_value(); });
        REQUIRE(ready.wait_for(2000ms) == std::future_status::ready);
    }

    // Stopping cancels the pending read; without that the endpoint and its
    // transport keep each other alive after the hub is gone.
    hub.reset();
    REQUIRE(weak.expired());
}

TEST_CASE("hub: destroying the hub releases an endpoint removed just before")
{
    auto hub = start_hub();
    std::weak_ptr<endpoint> weak;
    {
        auto port = add_loopback(*hub, "removed");
        weak = port;

        std::promise<void> armed;
        auto ready = armed.get_future();
        boost::asio::post(port->get_executor(), [&armed] { armed.set_value(); });
        REQUIRE(ready.wait_for(2000ms) == std::future_status::ready);

        REQUIRE(hub->remove_endpoint(port->id()));
    }

    // The removal queued a shutdown; stopping must not discard it.
    hub.reset();
    REQUIRE(weak.expired());
}

TEST_CASE("endpoint: removal closes it and drops its links")
{
    auto hub = start_hub();
    auto first = add_mock(*hub, "first");
    auto second = add_mock(*hub, "second");

    const auto link = hub->connect(first.endpoint->id(), second.endpoint->id());
    REQUIRE(link);

    REQUIRE(hub->remove_endpoint(first.endpoint->id()));
    REQUIRE(wait_until(
        [&] { return first.endpoint->state() == endpoint_state::closed; }, 2000ms));
    REQUIRE_EQ(hub->links().size(), std::size_t{0});
    REQUIRE(!hub->find_endpoint("first"));

    REQUIRE_EQ(hub->remove_endpoint(first.endpoint->id()),
               make_error_code(errc::not_found));
}

namespace {

/// A transport that tells its endpoint something, as a driver port does when a
/// program opens it. It never has any bytes.
class chatty_transport final : public transport
{
public:
    explicit chatty_transport(executor_type executor)
        : executor_(std::move(executor))
    {
    }

    std::string describe() const override { return "chatty"; }
    bool is_open() const override { return open_.load(); }

    void async_read_some(boost::asio::mutable_buffer, io_handler handler) override
    {
        const std::lock_guard lock(mutex_);
        read_ = std::move(handler);
    }

    void async_write_some(boost::asio::const_buffer buffer, io_handler handler) override
    {
        boost::asio::post(executor_, [handler = std::move(handler), size = buffer.size()]
                          { handler({}, size); });
    }

    void close() override
    {
        io_handler read;
        {
            const std::lock_guard lock(mutex_);
            read.swap(read_);
        }
        open_ = false;
        if (read)
            boost::asio::post(executor_, [read = std::move(read)]
                              { read(boost::asio::error::operation_aborted, 0); });
    }

    void set_notice_sink(notice_sink sink) override
    {
        const std::lock_guard lock(mutex_);
        sink_ = std::move(sink);
    }

    void say(std::string text)
    {
        notice_sink sink;
        {
            const std::lock_guard lock(mutex_);
            sink = sink_;
        }
        if (sink)
            sink(std::move(text));
    }

private:
    executor_type executor_;
    std::atomic<bool> open_{true};
    std::mutex mutex_;
    io_handler read_;
    notice_sink sink_;
};

} // namespace

TEST_CASE("endpoint: a notice from the transport is logged against the endpoint")
{
    auto hub = start_hub();
    event_recorder recorder;
    hub->subscribe({}, [recorder](const endpoint_event& event) { recorder.record(event); });

    auto transport = std::make_shared<chatty_transport>(hub->executor());
    auto added = hub->add_endpoint("talker", endpoint_kind::mock, transport);
    REQUIRE(added.has_value());
    const auto id = (*added)->id();

    transport->say("a program opened the port");

    REQUIRE(wait_until(
        [&]
        {
            for (const auto& event : recorder.snapshot())
            {
                if (event.kind == event_kind::log_message && event.endpoint == id
                    && event.message == "a program opened the port"
                    && event.name == "talker" && event.when != time_point{})
                    return true;
            }
            return false;
        },
        2000ms));

    // Once the endpoint has gone, the transport can still talk without harm.
    REQUIRE(hub->remove_endpoint(id).has_value());
    transport->say("nobody listens");
}

namespace
{

/// An endpoint on a scripted transport, with its first read already armed.
struct scripted_endpoint
{
    std::shared_ptr<vss::hub> hub = start_hub();
    std::shared_ptr<scripted_transport> transport =
        std::make_shared<scripted_transport>(hub->executor());
    std::shared_ptr<vss::endpoint> endpoint;

    scripted_endpoint()
    {
        const auto added =
            hub->add_endpoint("scripted", endpoint_kind::tcp, transport);
        REQUIRE(added);
        endpoint = *added;
        REQUIRE(
            wait_until([&] { return transport->reads_armed() == 1; }, 5000ms));
    }
};

std::shared_ptr<scripted_transport> make_scripted(vss::hub& hub)
{
    return std::make_shared<scripted_transport>(hub.executor());
}

/// Returns once everything posted to the endpoint's strand before the call has
/// run.
void drain_strand(const vss::endpoint& endpoint)
{
    std::promise<void> reached;
    auto done = reached.get_future();
    boost::asio::post(endpoint.get_executor(),
                      [&reached] { reached.set_value(); });
    REQUIRE(done.wait_for(5000ms) == std::future_status::ready);
}

/// What a write handler was called with, for every call.
class handler_log
{
public:
    vss::write_handler handler() const
    {
        const auto calls = calls_;
        return [calls](boost::system::error_code ec, std::size_t count)
        {
            std::lock_guard lock(calls->mutex);
            calls->entries.emplace_back(ec, count);
        };
    }

    std::size_t calls() const
    {
        std::lock_guard lock(calls_->mutex);
        return calls_->entries.size();
    }

    boost::system::error_code error(std::size_t index) const
    {
        std::lock_guard lock(calls_->mutex);
        return calls_->entries.at(index).first;
    }

private:
    struct shared_calls
    {
        std::mutex mutex;
        std::vector<std::pair<boost::system::error_code, std::size_t>> entries;
    };

    std::shared_ptr<shared_calls> calls_ = std::make_shared<shared_calls>();
};

/// How a `write_sync` ended: it returned a value or an error, or it threw.
struct sync_outcome
{
    bool threw = false;
    bool ok = false;
    boost::system::error_code error;
};

sync_outcome sync_write(vss::endpoint& endpoint, const std::string& text)
{
    sync_outcome outcome;
    try
    {
        const auto written = endpoint.write_sync(as_bytes(text), 5000ms);
        outcome.ok = written.has_value();
        if (!written)
            outcome.error = written.error();
    }
    catch (...)
    {
        outcome.threw = true;
    }
    return outcome;
}

/// Holds the strand's read stage until released, and says when it has a chunk.
class blocking_stage final : public vss::read_stage
{
public:
    blocking_stage() { gate_ = release_.get_future().share(); }

    ~blocking_stage() override { release(); }

    void on_read(vss::endpoint_id, vss::bytes_view,
                 vss::time_point) noexcept override
    {
        entered_.fetch_add(1);
        gate_.wait();
    }

    int entered() const { return entered_.load(); }

    void release()
    {
        std::lock_guard lock(release_mutex_);
        if (!released_)
        {
            released_ = true;
            release_.set_value();
        }
    }

private:
    std::atomic<int> entered_{0};
    std::mutex release_mutex_;
    bool released_ = false;
    std::promise<void> release_;
    std::shared_future<void> gate_;
};

} // namespace

TEST_CASE("endpoint: a reopen during a read leaves one read armed on the new "
          "transport")
{
    scripted_endpoint fixture;

    const auto stage = std::make_shared<blocking_stage>();
    // A failed check must not leave the strand held: the hub waits for it.
    struct release_on_exit
    {
        std::shared_ptr<blocking_stage> held;
        ~release_on_exit() { held->release(); }
    } const guard{stage};
    std::promise<void> installed;
    fixture.endpoint->set_read_stage(stage, [&] { installed.set_value(); });
    REQUIRE(installed.get_future().wait_for(5000ms) ==
            std::future_status::ready);

    // A chunk is being handled on the strand, held in the read stage.
    REQUIRE(fixture.transport->complete_read("ab"));
    REQUIRE(wait_until([&] { return stage->entered() == 1; }, 5000ms));

    // The reopen comes from another thread and posts the first read of the
    // new transport behind the chunk.
    const auto fresh = make_scripted(*fixture.hub);
    REQUIRE(fixture.endpoint->reopen(fresh));

    // The chunk is done: it re-arms a read, and so does the posted one.
    stage->release();
    drain_strand(*fixture.endpoint);
    REQUIRE(wait_until([&] { return fresh->reads_armed() >= 1; }, 5000ms));
    drain_strand(*fixture.endpoint);
    REQUIRE_EQ(fresh->reads_armed(), std::size_t{1});
    REQUIRE_EQ(fresh->reads_pending(), std::size_t{1});

    // And it carries on: what the new transport reads arrives once, and the
    // next read is the only one outstanding.
    REQUIRE(fresh->complete_read("xyz"));
    REQUIRE(wait_for_rx(*fixture.endpoint, 5, 5000ms));
    REQUIRE(wait_until([&] { return fresh->reads_armed() == 2; }, 5000ms));
    drain_strand(*fixture.endpoint);
    REQUIRE_EQ(fresh->reads_armed(), std::size_t{2});
    REQUIRE_EQ(fixture.endpoint->stats().bytes_rx, std::uint64_t{5});
}

TEST_CASE(
    "endpoint: write_sync on a transport that has closed returns an error")
{
    scripted_endpoint fixture;
    fixture.transport->close();

    const sync_outcome outcome = sync_write(*fixture.endpoint, "lost");
    REQUIRE(!outcome.threw);
    REQUIRE(!outcome.ok);
    REQUIRE(outcome.error == boost::asio::error::operation_aborted);
}

TEST_CASE("endpoint: a reopen completes every queued write with an error")
{
    scripted_endpoint fixture;

    // The first write is with the transport and never completes; the others
    // wait behind it.
    auto first = std::async(std::launch::async, [&]
                            { return sync_write(*fixture.endpoint, "one"); });
    REQUIRE(wait_until([&] { return fixture.transport->writes_pending() == 1; },
                       5000ms));
    handler_log second;
    handler_log third;
    fixture.endpoint->write(as_bytes("two"), second.handler());
    fixture.endpoint->write(as_bytes("three"), third.handler());

    const auto fresh = make_scripted(*fixture.hub);
    REQUIRE(fixture.endpoint->reopen(fresh));

    REQUIRE(first.wait_for(5000ms) == std::future_status::ready);
    const sync_outcome outcome = first.get();
    REQUIRE(!outcome.threw);
    REQUIRE(!outcome.ok);
    REQUIRE(outcome.error == boost::asio::error::operation_aborted);

    REQUIRE(wait_until(
        [&] { return second.calls() == 1 && third.calls() == 1; }, 5000ms));
    drain_strand(*fixture.endpoint);
    REQUIRE_EQ(second.calls(), std::size_t{1});
    REQUIRE_EQ(third.calls(), std::size_t{1});
    REQUIRE(second.error(0) == boost::asio::error::operation_aborted);
    REQUIRE(third.error(0) == boost::asio::error::operation_aborted);

    // Nothing queued for the old transport reached the new one.
    REQUIRE_EQ(fresh->written().size(), std::size_t{0});
}

TEST_CASE("endpoint: a stop completes every queued write with an error")
{
    scripted_endpoint fixture;

    auto first = std::async(std::launch::async, [&]
                            { return sync_write(*fixture.endpoint, "one"); });
    REQUIRE(wait_until([&] { return fixture.transport->writes_pending() == 1; },
                       5000ms));
    handler_log second;
    handler_log third;
    fixture.endpoint->write(as_bytes("two"), second.handler());
    fixture.endpoint->write(as_bytes("three"), third.handler());

    fixture.endpoint->stop();

    REQUIRE(first.wait_for(5000ms) == std::future_status::ready);
    const sync_outcome outcome = first.get();
    REQUIRE(!outcome.threw);
    REQUIRE(!outcome.ok);
    REQUIRE(outcome.error == boost::asio::error::operation_aborted);

    REQUIRE(wait_until(
        [&] { return second.calls() == 1 && third.calls() == 1; }, 5000ms));
    drain_strand(*fixture.endpoint);
    REQUIRE_EQ(second.calls(), std::size_t{1});
    REQUIRE_EQ(third.calls(), std::size_t{1});
    REQUIRE(second.error(0) == boost::asio::error::operation_aborted);
    REQUIRE(third.error(0) == boost::asio::error::operation_aborted);
}

TEST_CASE("endpoint: a failed write completes the writes queued behind it")
{
    scripted_endpoint fixture;

    // The write_sync is queued behind a write that is with the transport.
    // Nothing says when it has been queued, so wait a while; if it was late,
    // the transport has a second write pending: finish that one and go round
    // again.
    bool queued_behind = false;
    for (int attempt = 0; attempt < 20 && !queued_behind; ++attempt)
    {
        handler_log head;
        fixture.endpoint->write(as_bytes("one"), head.handler());
        REQUIRE(wait_until(
            [&] { return fixture.transport->writes_pending() == 1; }, 5000ms));
        auto behind =
            std::async(std::launch::async,
                       [&] { return sync_write(*fixture.endpoint, "two"); });
        std::this_thread::sleep_for(100ms);

        REQUIRE(fixture.transport->complete_write(
            boost::asio::error::connection_refused));
        REQUIRE(wait_until([&] { return head.calls() == 1; }, 5000ms));
        drain_strand(*fixture.endpoint);
        REQUIRE(head.error(0) == boost::asio::error::connection_refused);

        if (fixture.transport->writes_pending() == 1)
        {
            // Late: it went to the transport as a write of its own.
            REQUIRE(fixture.transport->complete_write());
            REQUIRE(behind.wait_for(6000ms) == std::future_status::ready);
            REQUIRE(!behind.get().threw);
            continue;
        }

        REQUIRE(behind.wait_for(6000ms) == std::future_status::ready);
        const sync_outcome outcome = behind.get();
        REQUIRE(!outcome.threw);
        REQUIRE(!outcome.ok);
        REQUIRE(outcome.error == boost::asio::error::operation_aborted);
        queued_behind = true;
    }
    REQUIRE(queued_behind);
}

TEST_CASE(
    "endpoint: every queued handler runs once when the queue is discarded")
{
    scripted_endpoint fixture;

    handler_log head;
    fixture.endpoint->write(as_bytes("head"), head.handler());
    REQUIRE(wait_until([&] { return fixture.transport->writes_pending() == 1; },
                       5000ms));
    std::vector<handler_log> behind(5);
    for (const auto& log : behind)
        fixture.endpoint->write(as_bytes("tail"), log.handler());

    // The head fails, and the writes behind it are abandoned.
    REQUIRE(fixture.transport->complete_write(
        boost::asio::error::connection_reset));
    REQUIRE(wait_until([&] { return head.calls() == 1; }, 5000ms));
    for (const auto& log : behind)
        REQUIRE(wait_until([&] { return log.calls() == 1; }, 5000ms));

    // A reopen and a stop after it find nothing left to call.
    REQUIRE(fixture.endpoint->reopen(make_scripted(*fixture.hub)));
    fixture.endpoint->stop();
    drain_strand(*fixture.endpoint);

    REQUIRE_EQ(head.calls(), std::size_t{1});
    REQUIRE(head.error(0) == boost::asio::error::connection_reset);
    for (const auto& log : behind)
    {
        REQUIRE_EQ(log.calls(), std::size_t{1});
        REQUIRE(log.error(0) == boost::asio::error::operation_aborted);
    }
}

TEST_CASE("endpoint: info reads the transport that reopen swaps in")
{
    scripted_endpoint fixture;

    std::atomic<bool> stop{false};
    std::atomic<int> seen{0};
    std::thread reader(
        [&]
        {
            while (!stop.load())
            {
                const endpoint_info info = fixture.endpoint->info();
                if (info.clients)
                    seen.fetch_add(1);
            }
        });

    std::vector<std::shared_ptr<scripted_transport>> swapped;
    for (int round = 0; round < 200; ++round)
    {
        swapped.push_back(make_scripted(*fixture.hub));
        REQUIRE(fixture.endpoint->reopen(swapped.back()));
    }
    stop = true;
    reader.join();

    // The reader asked the transports it found, one way or another.
    REQUIRE(seen.load() > 0);
    REQUIRE(fixture.endpoint->info().clients.has_value());
}

TEST_CASE("endpoint: writes racing reopen are never sent twice")
{
    scripted_endpoint fixture;
    fixture.transport->auto_complete_writes(true);

    std::atomic<bool> stop{false};
    std::atomic<int> counter{0};
    std::vector<std::thread> writers;
    writers.reserve(3);
    for (int index = 0; index < 3; ++index)
    {
        writers.emplace_back(
            [&]
            {
                while (!stop.load())
                {
                    // A text that is not sent twice on the same transport.
                    const std::string text =
                        "w" + std::to_string(counter.fetch_add(1)) + ";";
                    fixture.endpoint->write(as_bytes(text));
                    std::this_thread::sleep_for(100us);
                }
            });
    }

    std::vector<std::shared_ptr<scripted_transport>> all{fixture.transport};
    for (int round = 0; round < 150; ++round)
    {
        auto fresh = make_scripted(*fixture.hub);
        fresh->auto_complete_writes(true);
        all.push_back(fresh);
        REQUIRE(fixture.endpoint->reopen(fresh));
        std::this_thread::sleep_for(1ms);
    }
    stop = true;
    for (auto& writer : writers)
        writer.join();
    drain_strand(*fixture.endpoint);

    for (const auto& transport : all)
    {
        auto texts = transport->written();
        std::sort(texts.begin(), texts.end());
        REQUIRE(std::adjacent_find(texts.begin(), texts.end()) == texts.end());
    }
}

TEST_CASE("endpoint: a failed read of the replaced transport leaves the new one alone")
{
    scripted_endpoint fixture;

    event_recorder recorder;
    const subscription_guard subscription(
        *fixture.hub, event_filter{fixture.endpoint->id(), std::nullopt},
        [recorder](const endpoint_event& event) { recorder.record(event); });

    // The strand is busy, so the reopen is made while the read of the first
    // transport is still waiting to be handled.
    std::promise<void> release;
    const std::shared_future<void> gate = release.get_future().share();
    struct release_on_exit
    {
        std::promise<void>& held;
        bool released = false;
        void now()
        {
            if (!released)
                held.set_value();
            released = true;
        }
        ~release_on_exit() { now(); }
    } guard{release};
    boost::asio::post(fixture.endpoint->get_executor(), [gate] { gate.wait(); });

    const auto fresh = make_scripted(*fixture.hub);
    REQUIRE(fixture.endpoint->reopen(fresh));
    REQUIRE(fixture.transport->fail_read(boost::asio::error::network_down));
    guard.now();
    drain_strand(*fixture.endpoint);

    REQUIRE(fresh->is_open());
    REQUIRE_EQ(fixture.endpoint->state(), endpoint_state::open);
    REQUIRE_EQ(fresh->reads_armed(), std::size_t{1});
    REQUIRE_EQ(recorder.count(event_kind::error), std::size_t{0});
}

TEST_CASE("endpoint: a read that fails as the endpoint is reopened never closes the new transport")
{
    // The failure of the read of the first transport and the reopen are on
    // two threads and meet at a different point each round.  Whichever comes
    // first, the new transport is not closed and is read.  (The state label
    // can be wrong in one order, and is not asked about.)
    auto hub = start_hub();
    for (int round = 0; round < 1500; ++round)
    {
        const auto first = make_scripted(*hub);
        const auto added = hub->add_endpoint("race" + std::to_string(round),
                                             endpoint_kind::tcp, first);
        REQUIRE(added);
        const auto& endpoint = *added;
        REQUIRE(wait_until([&] { return first->reads_armed() == 1; }, 5000ms));

        const auto fresh = make_scripted(*hub);
        std::atomic<int> ready{0};
        std::thread failing([&] {
            ready.fetch_add(1);
            while (ready.load() < 2)
                std::this_thread::yield();
            first->fail_read(boost::asio::error::network_down);
        });
        ready.fetch_add(1);
        while (ready.load() < 2)
            std::this_thread::yield();
        static_cast<void>(endpoint->reopen(fresh));
        failing.join();

        drain_strand(*endpoint);
        drain_strand(*endpoint);
        REQUIRE(fresh->is_open());
        REQUIRE_EQ(fresh->reads_armed(), std::size_t{1});
        REQUIRE(hub->remove_endpoint(endpoint->id()).has_value());
    }
}
