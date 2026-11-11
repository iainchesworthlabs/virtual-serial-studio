//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The serial rules of the Windows virtual COM port driver. The driver cannot
// be debugged cheaply, so the rules live in plain C (vsscom_core) and are
// tested here on every platform.
//

// The core is plain C and declares no linkage of its own.
extern "C"
{
#include "vsscom_core.h"
}

#include "test_framework.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

// The core defines its own types so the kernel build needs no C library; they
// must still be the sizes the rest of the code assumes.
static_assert(sizeof(vsscom_u8) == 1);
static_assert(sizeof(vsscom_u32) == 4);
static_assert(sizeof(vsscom_u64) == 8);

namespace
{

constexpr std::uint32_t max_dword = VSSCOM_MAXDWORD;

vsscom_timeouts timeouts(std::uint32_t interval, std::uint32_t read_mult,
                         std::uint32_t read_const, std::uint32_t write_mult = 0,
                         std::uint32_t write_const = 0)
{
    vsscom_timeouts t{};
    t.read_interval = interval;
    t.read_total_multiplier = read_mult;
    t.read_total_constant = read_const;
    t.write_total_multiplier = write_mult;
    t.write_total_constant = write_const;
    return t;
}

bool valid(vsscom_timeouts t)
{
    return vsscom_timeouts_valid(&t);
}

std::uint32_t write_ms(vsscom_timeouts t, std::uint32_t length)
{
    return vsscom_write_timeout_ms(&t, length);
}

std::string drain(vsscom_ring& ring, std::uint32_t length)
{
    std::string out(length, '\0');
    const auto got = vsscom_ring_read(
        &ring, reinterpret_cast<std::uint8_t*>(out.data()), length);
    out.resize(got);
    return out;
}

std::uint32_t put(vsscom_ring& ring, const std::string& text)
{
    return vsscom_ring_write(&ring,
                             reinterpret_cast<const std::uint8_t*>(text.data()),
                             static_cast<std::uint32_t>(text.size()));
}

} // namespace

TEST_CASE("vsscom ring: bytes come out in the order they went in")
{
    std::array<std::uint8_t, 8> storage{};
    vsscom_ring ring;
    vsscom_ring_init(&ring, storage.data(), 8);

    REQUIRE_EQ(vsscom_ring_count(&ring), 0u);
    REQUIRE_EQ(vsscom_ring_space(&ring), 8u);
    REQUIRE_EQ(drain(ring, 4), std::string());

    REQUIRE_EQ(put(ring, "abc"), 3u);
    REQUIRE_EQ(vsscom_ring_count(&ring), 3u);
    REQUIRE_EQ(drain(ring, 2), std::string("ab"));
    REQUIRE_EQ(drain(ring, 9), std::string("c"));
    REQUIRE_EQ(vsscom_ring_count(&ring), 0u);
}

TEST_CASE("vsscom ring: it wraps around the end of the storage")
{
    std::array<std::uint8_t, 8> storage{};
    vsscom_ring ring;
    vsscom_ring_init(&ring, storage.data(), 8);

    // Move the head to the middle, then write across the end.
    REQUIRE_EQ(put(ring, "123456"), 6u);
    REQUIRE_EQ(drain(ring, 5), std::string("12345"));
    REQUIRE_EQ(put(ring, "abcdefg"), 7u);
    REQUIRE_EQ(vsscom_ring_count(&ring), 8u);
    REQUIRE_EQ(vsscom_ring_space(&ring), 0u);
    REQUIRE_EQ(drain(ring, 8), std::string("6abcdefg"));
}

TEST_CASE("vsscom ring: a full ring takes what fits and no more")
{
    std::array<std::uint8_t, 4> storage{};
    vsscom_ring ring;
    vsscom_ring_init(&ring, storage.data(), 4);

    REQUIRE_EQ(put(ring, "abcdef"), 4u);
    REQUIRE_EQ(put(ring, "x"), 0u);
    REQUIRE_EQ(drain(ring, 8), std::string("abcd"));

    REQUIRE_EQ(put(ring, "xy"), 2u);
    vsscom_ring_clear(&ring);
    REQUIRE_EQ(vsscom_ring_count(&ring), 0u);
    REQUIRE_EQ(vsscom_ring_space(&ring), 4u);
    REQUIRE_EQ(put(ring, "ok"), 2u);
    REQUIRE_EQ(drain(ring, 8), std::string("ok"));
}

TEST_CASE("vsscom ring: the newest bytes can be taken back")
{
    std::array<std::uint8_t, 8> storage{};
    vsscom_ring ring;
    vsscom_ring_init(&ring, storage.data(), 8);

    REQUIRE_EQ(put(ring, "old"), 3u);
    REQUIRE_EQ(put(ring, "new"), 3u);
    REQUIRE_EQ(vsscom_ring_drop_newest(&ring, 3), 3u);
    REQUIRE_EQ(drain(ring, 8), std::string("old"));

    // The reader took some of what was asked back: only what is left goes.
    REQUIRE_EQ(put(ring, "abcd"), 4u);
    REQUIRE_EQ(drain(ring, 2), std::string("ab"));
    REQUIRE_EQ(vsscom_ring_drop_newest(&ring, 4), 2u);
    REQUIRE_EQ(vsscom_ring_count(&ring), 0u);
    REQUIRE_EQ(vsscom_ring_drop_newest(&ring, 1), 0u);

    // The ring is whole afterwards, across the end of the storage too.
    REQUIRE_EQ(put(ring, "123456"), 6u);
    REQUIRE_EQ(vsscom_ring_drop_newest(&ring, 2), 2u);
    REQUIRE_EQ(put(ring, "xyzw"), 4u);
    REQUIRE_EQ(drain(ring, 8), std::string("1234xyzw"));
}

TEST_CASE("vsscom timeouts: only all three MAXDWORD read values are refused")
{
    REQUIRE(!valid(timeouts(max_dword, max_dword, max_dword)));
    REQUIRE(valid(timeouts(0, 0, 0)));
    REQUIRE(valid(timeouts(max_dword, 0, 0)));
    REQUIRE(valid(timeouts(max_dword, max_dword, 100)));
    REQUIRE(valid(timeouts(max_dword, max_dword, 0)));
    REQUIRE(valid(timeouts(0, max_dword, max_dword)));
}

TEST_CASE("vsscom read plan: all zeros waits for every byte, with no timer")
{
    const auto t = timeouts(0, 0, 0);
    const auto plan = vsscom_plan_read(&t, 10);
    REQUIRE(!plan.return_now);
    REQUIRE(!plan.return_on_first_byte);
    REQUIRE(!plan.has_total_timer);
    REQUIRE_EQ(plan.interval_ms, 0u);

    REQUIRE(!vsscom_read_satisfied(&plan, 10, 0));
    REQUIRE(!vsscom_read_satisfied(&plan, 10, 9));
    REQUIRE(vsscom_read_satisfied(&plan, 10, 10));
}

TEST_CASE("vsscom read plan: MAXDWORD interval alone returns at once")
{
    const auto t = timeouts(max_dword, 0, 0);
    const auto plan = vsscom_plan_read(&t, 10);
    REQUIRE(plan.return_now);
    REQUIRE(!plan.has_total_timer);
}

TEST_CASE("vsscom read plan: MAXDWORD with a constant waits for one byte")
{
    const auto t = timeouts(max_dword, max_dword, 250);
    const auto plan = vsscom_plan_read(&t, 10);
    REQUIRE(!plan.return_now);
    REQUIRE(plan.return_on_first_byte);
    REQUIRE(plan.has_total_timer);
    REQUIRE_EQ(plan.total_ms, 250u);

    REQUIRE(!vsscom_read_satisfied(&plan, 10, 0));
    REQUIRE(vsscom_read_satisfied(&plan, 10, 1));
    REQUIRE(vsscom_read_satisfied(&plan, 10, 10));

    // Without a constant the same pair is an ordinary (very long) read.
    const auto odd = timeouts(max_dword, max_dword, 0);
    REQUIRE(!vsscom_plan_read(&odd, 10).return_on_first_byte);
}

TEST_CASE("vsscom read plan: the total timeout grows with the request")
{
    const auto t = timeouts(0, 10, 100);
    REQUIRE_EQ(vsscom_plan_read(&t, 1).total_ms, 110u);
    REQUIRE_EQ(vsscom_plan_read(&t, 50).total_ms, 600u);
    REQUIRE(vsscom_plan_read(&t, 50).has_total_timer);

    // A constant alone is a fixed limit.
    const auto fixed = timeouts(0, 0, 500);
    REQUIRE_EQ(vsscom_plan_read(&fixed, 4096).total_ms, 500u);

    // A huge product is a long wait, not a wrapped short one.
    const auto huge = timeouts(0, max_dword - 1, 5);
    const auto plan = vsscom_plan_read(&huge, 1000000);
    REQUIRE(plan.has_total_timer);
    REQUIRE_EQ(plan.total_ms, max_dword - 1u);
}

TEST_CASE("vsscom read plan: the interval timeout applies after the first byte")
{
    const auto t = timeouts(50, 0, 0);
    const auto plan = vsscom_plan_read(&t, 8);
    REQUIRE(!plan.has_total_timer);
    REQUIRE_EQ(plan.interval_ms, 50u);

    const auto both = timeouts(50, 2, 20);
    const auto combined = vsscom_plan_read(&both, 8);
    REQUIRE(combined.has_total_timer);
    REQUIRE_EQ(combined.total_ms, 36u);
    REQUIRE_EQ(combined.interval_ms, 50u);
}

TEST_CASE("vsscom write timeout: none when both values are zero")
{
    REQUIRE_EQ(write_ms(timeouts(0, 0, 0, 0, 0), 100), 0u);
    REQUIRE_EQ(write_ms(timeouts(0, 0, 0, 5, 20), 100), 520u);
    REQUIRE_EQ(write_ms(timeouts(0, 0, 0, 0, 750), 100), 750u);
    REQUIRE_EQ(write_ms(timeouts(0, 0, 0, max_dword, 1), 100000),
               max_dword - 1u);
}

TEST_CASE("vsscom events: a wait completes when a masked event happens")
{
    vsscom_events events;
    vsscom_events_init(&events);
    std::uint32_t fired = 0;

    REQUIRE(!vsscom_events_set_mask(&events, VSSCOM_EV_RXCHAR));
    REQUIRE(!vsscom_events_wait(&events, &fired));
    REQUIRE(events.waiting);

    // An event outside the mask neither completes the wait nor is kept.
    REQUIRE(!vsscom_events_raise(&events, VSSCOM_EV_TXEMPTY, &fired));
    REQUIRE(events.waiting);
    REQUIRE_EQ(events.history, 0u);

    REQUIRE(
        vsscom_events_raise(&events, VSSCOM_EV_RXCHAR | VSSCOM_EV_CTS, &fired));
    REQUIRE_EQ(fired, VSSCOM_EV_RXCHAR);
    REQUIRE(!events.waiting);
    REQUIRE_EQ(events.history, 0u);
}

TEST_CASE("vsscom events: events that happen before the wait are kept")
{
    vsscom_events events;
    vsscom_events_init(&events);
    std::uint32_t fired = 0;

    vsscom_events_set_mask(&events, VSSCOM_EV_RXCHAR | VSSCOM_EV_TXEMPTY);
    REQUIRE(!vsscom_events_raise(&events, VSSCOM_EV_RXCHAR, &fired));
    REQUIRE(!vsscom_events_raise(&events, VSSCOM_EV_TXEMPTY, &fired));

    // The wait finds both and empties the history.
    REQUIRE(vsscom_events_wait(&events, &fired));
    REQUIRE_EQ(fired, VSSCOM_EV_RXCHAR | VSSCOM_EV_TXEMPTY);
    REQUIRE(!events.waiting);

    // A second wait now has to wait.
    REQUIRE(!vsscom_events_wait(&events, &fired));
    REQUIRE(events.waiting);
}

TEST_CASE("vsscom events: events of a cancelled wait go back to the history")
{
    vsscom_events events;
    vsscom_events_init(&events);
    std::uint32_t fired = 0;

    vsscom_events_set_mask(&events, VSSCOM_EV_RXCHAR | VSSCOM_EV_BREAK);
    REQUIRE(!vsscom_events_wait(&events, &fired));
    REQUIRE(vsscom_events_raise(&events, VSSCOM_EV_BREAK, &fired));
    REQUIRE_EQ(fired, VSSCOM_EV_BREAK);

    // The request was cancelled before it could take them: the driver raises
    // them again. With no wait parked they are kept for the next one.
    REQUIRE(!vsscom_events_raise(&events, fired, &fired));
    REQUIRE(vsscom_events_wait(&events, &fired));
    REQUIRE_EQ(fired, VSSCOM_EV_BREAK);

    // With a wait parked meanwhile, they complete that one instead.
    REQUIRE(!vsscom_events_wait(&events, &fired));
    std::uint32_t again = 0;
    REQUIRE(vsscom_events_raise(&events, VSSCOM_EV_RXCHAR, &again));
    REQUIRE(!vsscom_events_wait(&events, &fired));
    REQUIRE(vsscom_events_raise(&events, again, &fired));
    REQUIRE_EQ(fired, VSSCOM_EV_RXCHAR);

    // A new mask drops them, as it drops any history.
    REQUIRE(!vsscom_events_raise(&events, VSSCOM_EV_BREAK, &fired));
    vsscom_events_set_mask(&events, VSSCOM_EV_RXCHAR);
    REQUIRE(!vsscom_events_wait(&events, &fired));
}

TEST_CASE("vsscom events: setting the mask releases a wait and drops history")
{
    vsscom_events events;
    vsscom_events_init(&events);
    std::uint32_t fired = 0;

    vsscom_events_set_mask(&events, VSSCOM_EV_RXCHAR);
    vsscom_events_raise(&events, VSSCOM_EV_RXCHAR, &fired);
    REQUIRE_EQ(events.history, VSSCOM_EV_RXCHAR);

    // A new mask starts from nothing, even with the same value.
    REQUIRE(!vsscom_events_set_mask(&events, VSSCOM_EV_RXCHAR));
    REQUIRE_EQ(events.history, 0u);

    REQUIRE(!vsscom_events_wait(&events, &fired));
    REQUIRE(vsscom_events_set_mask(&events, 0));
    REQUIRE(!events.waiting);

    // With no mask nothing is recorded.
    REQUIRE(!vsscom_events_raise(&events, VSSCOM_EV_RXCHAR, &fired));
    REQUIRE_EQ(events.history, 0u);
}

TEST_CASE("vsscom events: a second wait while one is pending is not started")
{
    vsscom_events events;
    vsscom_events_init(&events);
    std::uint32_t fired = 0;
    vsscom_events_set_mask(&events, VSSCOM_EV_BREAK);

    REQUIRE(!vsscom_events_wait(&events, &fired));
    REQUIRE(events.waiting);
    // The caller reads `waiting` and refuses the second request; the call
    // itself changes nothing.
    REQUIRE(!vsscom_events_wait(&events, &fired));
    REQUIRE(events.waiting);
    REQUIRE_EQ(fired, 0u);
}

TEST_CASE("vsscom modem lines: a loopback port wires RTS to CTS and DTR to DSR")
{
    REQUIRE_EQ(vsscom_loopback_lines(false, false), 0u);
    REQUIRE_EQ(vsscom_loopback_lines(false, true), VSSCOM_MSR_CTS);
    REQUIRE_EQ(vsscom_loopback_lines(true, false),
               VSSCOM_MSR_DSR | VSSCOM_MSR_DCD);
    REQUIRE_EQ(vsscom_loopback_lines(true, true),
               VSSCOM_MSR_CTS | VSSCOM_MSR_DSR | VSSCOM_MSR_DCD);
}

TEST_CASE("vsscom modem lines: a change gives deltas and events")
{
    const std::uint32_t none = 0;
    const std::uint32_t cts = VSSCOM_MSR_CTS;
    const std::uint32_t all = vsscom_loopback_lines(true, true);

    REQUIRE_EQ(vsscom_modem_deltas(none, none), 0u);
    REQUIRE_EQ(vsscom_modem_events(none, none), 0u);

    REQUIRE_EQ(vsscom_modem_deltas(none, cts), VSSCOM_MSR_DCTS);
    REQUIRE_EQ(vsscom_modem_events(none, cts), VSSCOM_EV_CTS);
    REQUIRE_EQ(vsscom_modem_deltas(cts, none), VSSCOM_MSR_DCTS);

    REQUIRE_EQ(vsscom_modem_deltas(none, all),
               VSSCOM_MSR_DCTS | VSSCOM_MSR_DDSR | VSSCOM_MSR_DDCD);
    REQUIRE_EQ(vsscom_modem_events(none, all),
               VSSCOM_EV_CTS | VSSCOM_EV_DSR | VSSCOM_EV_RLSD);

    // The ring indicator is not wired in a loopback, so it never raises.
    REQUIRE_EQ(vsscom_modem_events(none, VSSCOM_MSR_RI), 0u);
}

// -- Flow control ----------------------------------------------------------

namespace
{

constexpr std::uint32_t rx_size = 65536;

vsscom_flow flow(std::uint32_t control, std::uint32_t replace,
                 std::uint32_t xon = 0, std::uint32_t xoff = 0)
{
    vsscom_flow f{};
    f.control_handshake = control;
    f.flow_replace = replace;
    f.xon_limit = xon;
    f.xoff_limit = xoff;
    return f;
}

vsscom_flow_lines lines(bool dtr, bool rts)
{
    vsscom_flow_lines l{};
    l.dtr = dtr;
    l.rts = rts;
    return l;
}

bool flow_valid(vsscom_flow f)
{
    return vsscom_flow_valid(&f, rx_size) != 0;
}

bool dtr_manual(vsscom_flow f)
{
    return vsscom_flow_dtr_manual(&f) != 0;
}

bool rts_manual(vsscom_flow f)
{
    return vsscom_flow_rts_manual(&f) != 0;
}

vsscom_flow_lines update(vsscom_flow f, vsscom_flow_lines now,
                         std::uint32_t rx_count, bool sending, bool set)
{
    return vsscom_flow_update(&f, now, rx_count, rx_size, sending, set);
}

} // namespace

TEST_CASE("vsscom flow: enable drives the line high and disable drives it low")
{
    // The settings are given: enable and disable move the lines, whatever
    // they were.
    auto on = update(flow(VSSCOM_DTR_CONTROL, VSSCOM_RTS_CONTROL),
                     lines(false, false), 0, false, true);
    REQUIRE(on.dtr);
    REQUIRE(on.rts);

    auto down = update(flow(0, 0), lines(true, true), 0, false, true);
    REQUIRE(!down.dtr);
    REQUIRE(!down.rts);

    // One line each way.
    auto mixed = update(flow(VSSCOM_DTR_CONTROL, 0), lines(false, true), 0,
                        false, true);
    REQUIRE(mixed.dtr);
    REQUIRE(!mixed.rts);
}

TEST_CASE("vsscom flow: enable and disable leave the line to the application afterwards")
{
    // Not a new setting: a change in the buffers must not undo SET_DTR.
    auto same = update(flow(0, 0), lines(true, true), 100, true, false);
    REQUIRE(same.dtr);
    REQUIRE(same.rts);

    auto kept = update(flow(VSSCOM_DTR_CONTROL, VSSCOM_RTS_CONTROL),
                       lines(false, false), 100, true, false);
    REQUIRE(!kept.dtr);
    REQUIRE(!kept.rts);
}

TEST_CASE("vsscom flow: a handshake line drops when the buffer fills and rises when it empties")
{
    // Low at 4 KB or less free, high again at 16 KB or less in use.
    const vsscom_flow f =
        flow(VSSCOM_DTR_HANDSHAKE, VSSCOM_RTS_HANDSHAKE, 16384, 4096);

    // Given with an empty buffer: both high.
    auto l = update(f, lines(false, false), 0, false, true);
    REQUIRE(l.dtr);
    REQUIRE(l.rts);

    // Filling: nothing happens until the free space reaches the xoff limit.
    l = update(f, l, rx_size - 4097, false, false);
    REQUIRE(l.dtr);
    REQUIRE(l.rts);
    l = update(f, l, rx_size - 4096, false, false);
    REQUIRE(!l.dtr);
    REQUIRE(!l.rts);

    // Draining: still low between the limits, high at the xon limit.
    l = update(f, l, 20000, false, false);
    REQUIRE(!l.dtr);
    REQUIRE(!l.rts);
    l = update(f, l, 16385, false, false);
    REQUIRE(!l.rts);
    l = update(f, l, 16384, false, false);
    REQUIRE(l.dtr);
    REQUIRE(l.rts);

    // High stays high between the limits.
    l = update(f, l, 30000, false, false);
    REQUIRE(l.dtr);
    REQUIRE(l.rts);
}

TEST_CASE("vsscom flow: a handshake given while the buffer is full starts low")
{
    auto l = update(flow(0, VSSCOM_RTS_HANDSHAKE, 100, 100),
                    lines(false, true), rx_size, false, true);
    REQUIRE(!l.rts);
}

TEST_CASE("vsscom flow: toggle puts RTS high exactly while sending")
{
    const vsscom_flow f = flow(0, VSSCOM_RTS_TOGGLE);

    // Given while idle: low.
    auto l = update(f, lines(false, true), 0, false, true);
    REQUIRE(!l.rts);

    l = update(f, l, 0, true, false);
    REQUIRE(l.rts);
    l = update(f, l, 0, false, false);
    REQUIRE(!l.rts);

    // DTR is not touched by RTS toggle.
    l = update(flow(VSSCOM_DTR_CONTROL, VSSCOM_RTS_TOGGLE), lines(false, false),
               0, false, true);
    REQUIRE(l.dtr);
    REQUIRE(!l.rts);
}

TEST_CASE("vsscom flow: only a handshake line is closed to SET_DTR and SET_RTS")
{
    REQUIRE(dtr_manual(flow(0, 0)));
    REQUIRE(dtr_manual(flow(VSSCOM_DTR_CONTROL, 0)));
    REQUIRE(!dtr_manual(flow(VSSCOM_DTR_HANDSHAKE, 0)));

    REQUIRE(rts_manual(flow(0, 0)));
    REQUIRE(rts_manual(flow(0, VSSCOM_RTS_CONTROL)));
    REQUIRE(rts_manual(flow(0, VSSCOM_RTS_TOGGLE)));
    REQUIRE(!rts_manual(flow(0, VSSCOM_RTS_HANDSHAKE)));
}

TEST_CASE("vsscom flow: settings that mean nothing are refused")
{
    REQUIRE(flow_valid(flow(0, 0)));
    REQUIRE(flow_valid(flow(VSSCOM_DTR_CONTROL, VSSCOM_RTS_TOGGLE)));
    // CTS, DSR and carrier handshakes and XON/XOFF bits are accepted, even
    // though the port does not act on them.
    REQUIRE(flow_valid(flow(0x08u | 0x10u | 0x20u | 0x40u,
                            0x01u | 0x02u | 0x04u | 0x08u | 0x10u)));
    REQUIRE(flow_valid(flow(0x80000000u, 0x80000000u)));

    // Both DTR bits together.
    REQUIRE(!flow_valid(flow(VSSCOM_DTR_MASK, 0)));
    // An unknown bit in either field.
    REQUIRE(!flow_valid(flow(0x04u, 0)));
    REQUIRE(!flow_valid(flow(0, 0x20u)));
    REQUIRE(!flow_valid(flow(0x100u, 0)));

    // Limits matter only when a line follows the buffer.
    REQUIRE(flow_valid(flow(0, 0, rx_size + 1, 5)));
    REQUIRE(!flow_valid(flow(VSSCOM_DTR_HANDSHAKE, 0, rx_size + 1, 5)));
    REQUIRE(!flow_valid(flow(0, VSSCOM_RTS_HANDSHAKE, 5, rx_size + 1)));
    REQUIRE(flow_valid(flow(0, VSSCOM_RTS_HANDSHAKE, rx_size, rx_size)));
}

TEST_CASE("vsscom flow: how long bytes take to leave a UART")
{
    // 9600 8N1: ten bits a byte, about 1.0417 ms, in units of 100 ns.
    REQUIRE_EQ(vsscom_tx_time(9600, 8, 0, 0, 1), 10416ull);
    REQUIRE_EQ(vsscom_tx_time(9600, 8, 0, 0, 0), 0ull);
    // Eleven bits with parity, twelve with two stop bits.
    REQUIRE_EQ(vsscom_tx_time(1000, 8, 2, 0, 1), 110000ull);
    REQUIRE_EQ(vsscom_tx_time(1000, 8, 0, 2, 1), 110000ull);
    // One and a half stop bits: 10.5 bits.
    REQUIRE_EQ(vsscom_tx_time(1000, 8, 0, 1, 1), 105000ull);
    // Five data bits, one stop bit: seven bits.
    REQUIRE_EQ(vsscom_tx_time(1000, 5, 0, 0, 2), 140000ull);
    // No baud rate, no time.
    REQUIRE_EQ(vsscom_tx_time(0, 8, 0, 0, 100), 0ull);
}

TEST_CASE("vsscom flow: busy time queues behind earlier bytes")
{
    REQUIRE_EQ(vsscom_tx_busy_until(1000, 0, 500), 1500ull);
    // Still busy: the new bytes follow the old.
    REQUIRE_EQ(vsscom_tx_busy_until(1000, 1200, 500), 1700ull);
    // The earlier bytes are long gone.
    REQUIRE_EQ(vsscom_tx_busy_until(1000, 900, 500), 1500ull);

    REQUIRE(vsscom_tx_sending(1, 1000, 0));
    REQUIRE(vsscom_tx_sending(0, 1000, 1001));
    REQUIRE(!vsscom_tx_sending(0, 1000, 1000));
    REQUIRE(!vsscom_tx_sending(0, 1000, 0));
}

// -- Line errors -----------------------------------------------------------

namespace
{

// SERIAL_ERROR_ABORT, SERIAL_ERROR_CHAR and SERIAL_BREAK_CHAR of ntddser.h.
constexpr std::uint32_t error_abort = 0x80000000u;
constexpr std::uint32_t error_char = 0x04u;
constexpr std::uint32_t break_char = 0x10u;

constexpr std::uint32_t err_break = VSSCOM_ERROR_BREAK;
constexpr std::uint32_t err_framing = VSSCOM_ERROR_FRAMING;
constexpr std::uint32_t err_overrun = VSSCOM_ERROR_OVERRUN;
constexpr std::uint32_t err_queue = VSSCOM_ERROR_QUEUEOVERRUN;
constexpr std::uint32_t err_parity = VSSCOM_ERROR_PARITY;

// '?' replaces a bad character, '!' marks a break and '\r' is the event
// character.
constexpr vsscom_error_chars chars{'?', '!', '\r'};

vsscom_line_error line_error(std::uint32_t errors)
{
    vsscom_line_error e{};
    e.errors = errors;
    return e;
}

vsscom_line_error line_error(std::uint32_t errors, char byte)
{
    vsscom_line_error e{};
    e.errors = errors;
    e.has_byte = 1;
    e.byte = static_cast<std::uint8_t>(byte);
    return e;
}

vsscom_error_effect effect_of(vsscom_line_error e, std::uint32_t replace)
{
    const vsscom_flow f = flow(0, replace);
    return vsscom_line_error_effect(&e, &f, &chars);
}

std::string stream_of(const vsscom_error_effect& effect)
{
    return std::string(reinterpret_cast<const char*>(effect.bytes),
                       effect.count);
}

// What the driver does with an error at the head of the hub's writes: work
// out the effect, apply it, raise its events.
struct port
{
    std::array<std::uint8_t, 8> storage{};
    vsscom_ring rx{};
    vsscom_events events{};
    std::uint32_t pending = 0;
    vsscom_flow settings{};

    port()
    {
        vsscom_ring_init(&rx, storage.data(), 8);
        vsscom_events_init(&events);
    }

    // True when the error went in; `fired` and `completed` say what a
    // pending wait got.
    bool inject(vsscom_line_error e)
    {
        const auto effect = vsscom_line_error_effect(&e, &settings, &chars);
        if (!vsscom_line_error_apply(&pending, &rx, &e, &effect))
            return false;
        completed = vsscom_events_raise(&events, effect.events, &fired) != 0;
        return true;
    }

    bool blocked() const
    {
        return vsscom_errors_abort(&settings, pending) != 0;
    }

    std::uint32_t fired = 0;
    bool completed = false;
};

} // namespace

TEST_CASE("vsscom errors: only error bits make an error")
{
    auto valid_error = [](std::uint32_t errors)
    {
        const auto e = line_error(errors);
        return vsscom_line_error_valid(&e) != 0;
    };
    REQUIRE(valid_error(err_break));
    REQUIRE(valid_error(err_framing | err_parity));
    REQUIRE(valid_error(VSSCOM_ERROR_MASK));
    REQUIRE(!valid_error(0));
    REQUIRE(!valid_error(0x20u));
    REQUIRE(!valid_error(err_parity | 0x100u));
}

TEST_CASE("vsscom errors: each kind raises its event")
{
    REQUIRE_EQ(effect_of(line_error(err_framing), 0).events, VSSCOM_EV_ERR);
    REQUIRE_EQ(effect_of(line_error(err_parity), 0).events, VSSCOM_EV_ERR);
    REQUIRE_EQ(effect_of(line_error(err_overrun), 0).events, VSSCOM_EV_ERR);
    REQUIRE_EQ(effect_of(line_error(err_break), 0).events, VSSCOM_EV_BREAK);
    REQUIRE_EQ(effect_of(line_error(err_break | err_framing), 0).events,
               VSSCOM_EV_BREAK | VSSCOM_EV_ERR);
    // A queue overrun is not a line-status error: it is only latched.
    REQUIRE_EQ(effect_of(line_error(err_queue), 0).events, 0u);
    // Without a byte nothing enters the stream.
    REQUIRE_EQ(effect_of(line_error(err_parity), error_char | break_char).count,
               1u);
    REQUIRE_EQ(effect_of(line_error(err_queue), error_char).count, 0u);
}

TEST_CASE("vsscom errors: the bad byte is read unless the settings replace it")
{
    // No replacement: the application reads what the receiver made of it.
    auto plain = effect_of(line_error(err_parity, 'x'), 0);
    REQUIRE_EQ(stream_of(plain), std::string("x"));
    REQUIRE_EQ(plain.events, VSSCOM_EV_ERR | VSSCOM_EV_RXCHAR);

    // SERIAL_ERROR_CHAR: the error character takes its place.
    REQUIRE_EQ(stream_of(effect_of(line_error(err_parity, 'x'), error_char)),
               std::string("?"));
    REQUIRE_EQ(stream_of(effect_of(line_error(err_framing, 'x'), error_char)),
               std::string("?"));
    // And appears even when the hub named no byte.
    REQUIRE_EQ(stream_of(effect_of(line_error(err_framing), error_char)),
               std::string("?"));

    // The break character is a different setting.
    REQUIRE_EQ(stream_of(effect_of(line_error(err_break, '\0'), error_char)),
               std::string(1, '\0'));
    REQUIRE_EQ(stream_of(effect_of(line_error(err_break, '\0'), break_char)),
               std::string("!"));
    REQUIRE_EQ(stream_of(effect_of(line_error(err_break), break_char)),
               std::string("!"));
    REQUIRE_EQ(stream_of(effect_of(line_error(err_parity, 'x'), break_char)),
               std::string("x"));
    // A break hides the framing error of the same character time.
    REQUIRE_EQ(stream_of(effect_of(line_error(err_break | err_framing, 'x'),
                                   error_char)),
               std::string("x"));

    // An overrun lost an earlier character: the error character marks the
    // place and the byte that did arrive follows.
    REQUIRE_EQ(stream_of(effect_of(line_error(err_overrun, 'x'), 0)),
               std::string("x"));
    REQUIRE_EQ(stream_of(effect_of(line_error(err_overrun, 'x'), error_char)),
               std::string("?x"));
    REQUIRE_EQ(stream_of(effect_of(line_error(err_overrun | err_parity, 'x'),
                                   error_char)),
               std::string("??"));
}

TEST_CASE("vsscom errors: the event character raises RXFLAG only as itself")
{
    REQUIRE_EQ(effect_of(line_error(err_parity, '\r'), 0).events,
               VSSCOM_EV_ERR | VSSCOM_EV_RXCHAR | VSSCOM_EV_RXFLAG);
    // Replaced, it never arrived.
    REQUIRE_EQ(effect_of(line_error(err_parity, '\r'), error_char).events,
               VSSCOM_EV_ERR | VSSCOM_EV_RXCHAR);

    const std::uint8_t text[] = {'a', 'b', '\r', 'c'};
    REQUIRE(vsscom_bytes_have(text, 4, '\r'));
    REQUIRE(!vsscom_bytes_have(text, 2, '\r'));
    REQUIRE(!vsscom_bytes_have(text, 0, 'a'));
}

TEST_CASE("vsscom errors: an error with no wait pending is kept in the history")
{
    port p;
    vsscom_events_set_mask(&p.events, VSSCOM_EV_ERR | VSSCOM_EV_BREAK);

    REQUIRE(p.inject(line_error(err_parity)));
    REQUIRE(!p.completed);
    REQUIRE(p.inject(line_error(err_break)));
    REQUIRE(!p.completed);
    REQUIRE_EQ(p.pending, err_parity | err_break);

    // The next wait returns at once with both.
    std::uint32_t fired = 0;
    REQUIRE(vsscom_events_wait(&p.events, &fired));
    REQUIRE_EQ(fired, VSSCOM_EV_ERR | VSSCOM_EV_BREAK);
    // The history is spent, the latch is not.
    REQUIRE(!vsscom_events_wait(&p.events, &fired));
    REQUIRE_EQ(p.pending, err_parity | err_break);
}

TEST_CASE("vsscom errors: an error completes a pending wait that asked for it")
{
    port p;
    std::uint32_t fired = 0;
    vsscom_events_set_mask(&p.events, VSSCOM_EV_BREAK);
    REQUIRE(!vsscom_events_wait(&p.events, &fired));

    // Not in the mask: the wait stays, the error is still latched.
    REQUIRE(p.inject(line_error(err_framing)));
    REQUIRE(!p.completed);
    REQUIRE(p.events.waiting);
    REQUIRE_EQ(p.events.history, 0u);
    REQUIRE_EQ(p.pending, err_framing);

    REQUIRE(p.inject(line_error(err_break)));
    REQUIRE(p.completed);
    REQUIRE_EQ(p.fired, VSSCOM_EV_BREAK);
    REQUIRE(!p.events.waiting);
}

TEST_CASE("vsscom errors: a new mask drops the event and keeps the error")
{
    port p;
    std::uint32_t fired = 0;
    vsscom_events_set_mask(&p.events, VSSCOM_EV_ERR);
    REQUIRE(p.inject(line_error(err_overrun)));
    REQUIRE_EQ(p.events.history, VSSCOM_EV_ERR);

    // SetCommMask starts the history again, with the same mask or with none.
    vsscom_events_set_mask(&p.events, VSSCOM_EV_ERR);
    REQUIRE(!vsscom_events_wait(&p.events, &fired));
    REQUIRE(vsscom_events_set_mask(&p.events, 0));
    REQUIRE_EQ(p.pending, err_overrun);

    // With no mask an error raises nothing and is latched all the same.
    REQUIRE(p.inject(line_error(err_parity)));
    REQUIRE(!p.completed);
    REQUIRE_EQ(p.events.history, 0u);
    REQUIRE_EQ(p.pending, err_overrun | err_parity);
}

TEST_CASE("vsscom errors: ClearCommError reports every error once")
{
    port p;
    REQUIRE_EQ(vsscom_errors_take(&p.pending), 0u);

    REQUIRE(p.inject(line_error(err_parity)));
    REQUIRE(p.inject(line_error(err_parity)));
    REQUIRE(p.inject(line_error(err_queue)));
    REQUIRE_EQ(vsscom_errors_take(&p.pending), err_parity | err_queue);
    REQUIRE_EQ(vsscom_errors_take(&p.pending), 0u);
}

TEST_CASE("vsscom errors: abort on error holds until the error is read")
{
    port p;
    REQUIRE(!p.blocked());

    // Without the setting an error blocks nothing.
    REQUIRE(p.inject(line_error(err_framing)));
    REQUIRE(!p.blocked());

    // The setting given while an error is latched takes effect at once.
    p.settings = flow(error_abort, 0);
    REQUIRE(p.blocked());
    REQUIRE_EQ(vsscom_errors_take(&p.pending), err_framing);
    REQUIRE(!p.blocked());

    // Every kind blocks, the ones that raise no event included.
    REQUIRE(p.inject(line_error(err_queue)));
    REQUIRE(p.blocked());
    vsscom_errors_take(&p.pending);
    REQUIRE(p.inject(line_error(err_break)));
    REQUIRE(p.blocked());

    // Taking the setting away releases the port without reading the error.
    p.settings = flow(0, 0);
    REQUIRE(!p.blocked());
    REQUIRE_EQ(p.pending, err_break);
}

TEST_CASE("vsscom errors: an error keeps its place among the bytes")
{
    port p;
    p.settings = flow(0, error_char);

    REQUIRE_EQ(put(p.rx, "ab"), 2u);
    REQUIRE(p.inject(line_error(err_parity, 'x')));
    REQUIRE_EQ(put(p.rx, "cd"), 2u);
    REQUIRE_EQ(drain(p.rx, 8), std::string("ab?cd"));
}

TEST_CASE("vsscom errors: an error waits for room behind the bytes before it")
{
    port p;
    p.settings = flow(0, error_char);
    vsscom_events_set_mask(&p.events, VSSCOM_EV_ERR);

    // Seven of eight bytes: an overrun with a byte needs two.
    REQUIRE_EQ(put(p.rx, "1234567"), 7u);
    REQUIRE(!p.inject(line_error(err_overrun, 'x')));
    REQUIRE_EQ(p.pending, 0u);
    REQUIRE_EQ(p.events.history, 0u);
    REQUIRE_EQ(vsscom_ring_count(&p.rx), 7u);

    // The application reads one byte: now it fits, behind what was there.
    REQUIRE_EQ(drain(p.rx, 1), std::string("1"));
    REQUIRE(p.inject(line_error(err_overrun, 'x')));
    REQUIRE_EQ(p.pending, err_overrun);
    REQUIRE_EQ(drain(p.rx, 8), std::string("234567?x"));

    // An error that adds no byte never waits.
    REQUIRE_EQ(put(p.rx, "12345678"), 8u);
    REQUIRE(p.inject(line_error(err_queue)));
    REQUIRE(p.inject(line_error(err_break)));
}

TEST_CASE("vsscom errors: a purge empties the buffer and keeps the error")
{
    port p;
    vsscom_events_set_mask(&p.events, VSSCOM_EV_ERR);
    p.settings = flow(error_abort, 0);

    REQUIRE(p.inject(line_error(err_framing, 'x')));
    vsscom_ring_clear(&p.rx);

    // The byte is gone; the latch, the event and the hold are not.
    REQUIRE_EQ(vsscom_ring_count(&p.rx), 0u);
    REQUIRE(p.blocked());
    std::uint32_t fired = 0;
    REQUIRE(vsscom_events_wait(&p.events, &fired));
    REQUIRE_EQ(fired, VSSCOM_EV_ERR);
    REQUIRE_EQ(vsscom_errors_take(&p.pending), err_framing);
    REQUIRE(!p.blocked());
}
