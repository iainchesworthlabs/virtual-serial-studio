//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hardware-in-the-loop tests.  They need a connected pair of serial ports:
// VSS_LOOPBACK_A / VSS_LOOPBACK_B on Linux and macOS CI, or any com0com
// pair on Windows.  Without one every case skips rather than fails.
//

#include "support/test_support.hpp"

#include <vss/ports/native_lines.hpp>
#include <vss/ports/serial_transport.hpp>
#include <vss/ports/vcom_driver.hpp>
#include <vss/studio/studio.hpp>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

using namespace vss;
using namespace vss::testkit;
using namespace vss_test_support;

namespace {
using namespace std::chrono_literals;

serial_pair require_pair()
{
    const auto pair = find_loopback_pair();
    if (!pair)
    {
        // CI sets VSS_REQUIRE_LOOPBACK so a missing pair fails the job instead
        // of quietly reducing what is tested.
        if (!vss::testkit::environment("VSS_REQUIRE_LOOPBACK").empty())
            throw vss_test::failure(
                "VSS_REQUIRE_LOOPBACK is set but no serial pair was found");
        VSS_SKIP("no connected serial pair on this machine");
    }
    return *pair;
}

serial_settings fast_settings()
{
    serial_settings settings;
    settings.baud_rate = 115200;
    settings.read_timeout = std::chrono::milliseconds(1000);
    return settings;
}
} // namespace

TEST_CASE("system: a real serial pair carries data in each direction")
{
    const auto pair = require_pair();
    studio app;

    const auto first = app.attach_serial(
        "a", "serial://" + pair.a + "?baud=115200", fast_settings());
    REQUIRE(first);
    const auto second = app.attach_serial(
        "b", "serial://" + pair.b + "?baud=115200", fast_settings());
    REQUIRE(second);

    REQUIRE((*first)->is_open());
    REQUIRE((*second)->is_open());
    REQUIRE_EQ((*first)->public_path(), pair.a);
    REQUIRE_EQ((*second)->public_path(), pair.b);

    // The endpoints are deliberately not linked to each other: linking two
    // ends of a null-modem cable through a forwarder is a physical loop.
    const auto sent = (*first)->write_sync(as_bytes("PING"), 3000ms);
    REQUIRE(sent);
    REQUIRE(wait_for_rx(**second, 4, 5000ms));

    const auto reply = (*second)->write_sync(as_bytes("PONG"), 3000ms);
    REQUIRE(reply);
    REQUIRE(wait_for_rx(**first, 4, 5000ms));

    REQUIRE_EQ((*first)->stats().bytes_tx, std::uint64_t{4});
    REQUIRE_EQ((*second)->stats().bytes_rx, std::uint64_t{4});
}

TEST_CASE("system: control lines can be driven on a real port")
{
    const auto pair = require_pair();
    studio app;

    const auto port = app.attach_serial(
        "ctl", "serial://" + pair.a + "?baud=115200", fast_settings());
    REQUIRE(port);

    // com0com and USB UARTs both accept DTR/RTS; a platform that does not is
    // allowed to report unsupported rather than fail.
    const error_code raised = (*port)->set_line(control_line::dtr, true);
    REQUIRE((!raised || raised == make_error_code(errc::unsupported)));
    if (!raised)
    {
        REQUIRE((*port)->lines().dtr());
        REQUIRE(!(*port)->set_line(control_line::dtr, false));
        REQUIRE(!(*port)->lines().dtr());
    }

    REQUIRE(!(*port)->set_line(control_line::rts, true));
    REQUIRE((*port)->lines().rts());

    (*port)->refresh_lines();
    REQUIRE((*port)->lines().rts());
}TEST_CASE("system: high speed transfer keeps every byte")
{
    const auto pair = require_pair();

    // The receiving state lives outside the studio's scope: the hub must be
    // destroyed (and its event queue drained) before the buffer dies.
    std::mutex mutex;
    byte_buffer received;

    const std::size_t total = pair.com0com ? 256 * 1024 : 8 * 1024;
    const byte_buffer payload = make_pattern(total, 7);

    {
        studio app;

        const auto sender = app.attach_serial(
            "sender", "serial://" + pair.a + "?baud=921600", fast_settings());
        REQUIRE(sender);
        const auto receiver = app.attach_serial(
            "receiver", "serial://" + pair.b + "?baud=921600", fast_settings());
        REQUIRE(receiver);
        REQUIRE((*sender)->is_open());
        REQUIRE((*receiver)->is_open());

        // Collect what the receiver sees so the payload can be compared byte
        // for byte, not just counted.
        event_filter filter;
        filter.endpoint = (*receiver)->id();
        filter.kind = event_kind::data;
        subscription_guard subscription(
            app.engine(), filter, [&](const endpoint_event& event) {
                if (event.direction != data_direction::rx || !event.data)
                    return;
                std::lock_guard lock(mutex);
                received.insert(received.end(), event.data->begin(),
                                event.data->end());
            });

        const std::size_t chunk = 4096;
        for (std::size_t offset = 0; offset < payload.size(); offset += chunk)
        {
            const std::size_t size = std::min(chunk, payload.size() - offset);
            const auto written = (*sender)->write_sync(
                bytes_view(payload.data() + offset, size), 5000ms);
            REQUIRE(written);
        }

        REQUIRE(wait_for_rx(**receiver, total, 30000ms));

        // Data events are dispatched on the hub's event strand slightly after
        // the counters move, so wait for the subscriber's copy as well.
        REQUIRE(wait_until(
            [&] {
                std::lock_guard lock(mutex);
                return received.size() >= payload.size();
            },
            10000ms));

        std::lock_guard lock(mutex);
        REQUIRE_EQ(received.size(), payload.size());
        REQUIRE(received == payload);
    }
}

TEST_CASE("system: sharing a real port with several listeners")
{
    // The listener is an automatic virtual port, which is a real COM port
    // where the driver is installed; a test does not make one on the machine
    // it runs on.
    if (vss::driver_available())
        VSS_SKIP("the Windows driver is installed; it would make a real port");

    const auto pair = require_pair();
    studio app;

    // Attach the observer on the far side of the cable first, so the
    // listener creation cannot take that end on Windows/com0com.
    const auto observer = app.attach_serial(
        "observer", "serial://" + pair.b + "?baud=115200", fast_settings());
    REQUIRE(observer);

    const auto shared = app.share_serial("serial://" + pair.a + "?baud=115200", 2);
    REQUIRE(shared);
    REQUIRE_EQ(shared->listeners.size(), std::size_t{2});

    std::vector<std::string> advertised;
    for (const endpoint_id id : shared->listeners)
    {
        const auto listener = app.engine().find_endpoint(id);
        REQUIRE(listener);
        REQUIRE(listener->is_open());

        const std::string path = app.advertised_path(id);
        REQUIRE(!path.empty());
        REQUIRE(std::find(advertised.begin(), advertised.end(), path)
                == advertised.end());
        advertised.push_back(path);
    }

    // Data arriving on the shared port is fanned out to every listener. The
    // observer writes into the far end of the cable, so this exercises the
    // split without depending on loopback echo semantics.
    const auto written = (*observer)->write_sync(as_bytes("fanout"), 3000ms);
    REQUIRE(written);

    for (const endpoint_id id : shared->listeners)
    {
        const auto listener = app.engine().find_endpoint(id);
        REQUIRE(listener);
        REQUIRE(wait_until(
            [&] { return listener->stats().bytes_tx >= 6; }, 5000ms));
    }
}

TEST_CASE("system: the native control line layer drives a real device")
{
    const auto pair = require_pair();
    boost::asio::io_context io;
    boost::system::error_code error;

    serial_transport transport(io.get_executor(),
                               "serial://" + pair.a + "?baud=115200", error);
    REQUIRE(!error);
    REQUIRE(transport.is_open());

    const auto handle = transport.native_handle();
    REQUIRE(handle != 0);

    // A PTY slave on Linux does not implement TIOCMGET, and some virtual
    // drivers do not report modem status either. Skip rather than assert a
    // capability the device does not claim.
    const auto status = native::read_modem_status(handle);
    if (!status)
        VSS_SKIP("this device does not report modem status");

    // Some virtual drivers refuse DTR/RTS requests; either outcome is a
    // reported error rather than a crash, and the success path is asserted
    // only where the driver supports it.
    const auto dtr = native::write_line(handle, control_line::dtr, true);
    const auto rts = native::write_line(handle, control_line::rts, true);

    if (dtr && rts)
    {
        transport.refresh_lines();
        REQUIRE(transport.lines().rts());
    }

    static_cast<void>(native::write_line(handle, control_line::dtr, false));
    static_cast<void>(native::write_line(handle, control_line::rts, false));

    // Outputs are not reported back by GetCommModemStatus/TIOCMGET, so the
    // inputs are what a refresh can confirm.
    const auto refreshed = native::read_modem_status(handle);
    REQUIRE(refreshed);

    // Break signalling is driver dependent; either answer is acceptable as
    // long as it is reported rather than thrown.
    static_cast<void>(native::set_break(handle, true));
    static_cast<void>(native::set_break(handle, false));

    REQUIRE_EQ(native::write_line(handle, control_line::cts, true),
               make_error_code(errc::unsupported));
}

TEST_CASE("system: a bridge rolls back when the second port fails")
{
    const auto pair = require_pair();
    studio app;

    const auto bridged =
        app.bridge("serial://" + pair.a, "serial://no-such-device");
    REQUIRE(!bridged);
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
    REQUIRE_EQ(app.link_infos().size(), std::size_t{0});
}

TEST_CASE("system: a missing device reports an error instead of crashing")
{
    studio app;
    const auto missing = app.attach_serial(
        "missing", "serial://com-does-not-exist-9876", fast_settings());
    REQUIRE(!missing);
    REQUIRE_EQ(app.endpoint_infos().size(), std::size_t{0});
}
