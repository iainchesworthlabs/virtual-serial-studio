//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "vsscom_core.h"

// -- The byte ring ---------------------------------------------------------

static void copy_bytes(vsscom_u8* to, const vsscom_u8* from, vsscom_u32 count)
{
    while (count--)
        *to++ = *from++;
}

void vsscom_ring_init(vsscom_ring* ring, vsscom_u8* storage, vsscom_u32 size)
{
    ring->data = storage;
    ring->size = size;
    ring->head = 0;
    ring->count = 0;
}

vsscom_u32 vsscom_ring_count(const vsscom_ring* ring)
{
    return ring->count;
}

vsscom_u32 vsscom_ring_space(const vsscom_ring* ring)
{
    return ring->size - ring->count;
}

vsscom_u32 vsscom_ring_write(vsscom_ring* ring, const vsscom_u8* bytes,
                           vsscom_u32 length)
{
    const vsscom_u32 room = vsscom_ring_space(ring);
    const vsscom_u32 take = length < room ? length : room;
    if (take == 0)
        return 0;

    // The first free slot, then at most two runs: up to the end, then the
    // start.
    const vsscom_u32 tail = (ring->head + ring->count) % ring->size;
    const vsscom_u32 first = take < ring->size - tail ? take : ring->size - tail;
    copy_bytes(ring->data + tail, bytes, first);
    if (take > first)
        copy_bytes(ring->data, bytes + first, take - first);
    ring->count += take;
    return take;
}

vsscom_u32 vsscom_ring_read(vsscom_ring* ring, vsscom_u8* bytes, vsscom_u32 length)
{
    const vsscom_u32 take = length < ring->count ? length : ring->count;
    if (take == 0)
        return 0;

    const vsscom_u32 first =
        take < ring->size - ring->head ? take : ring->size - ring->head;
    copy_bytes(bytes, ring->data + ring->head, first);
    if (take > first)
        copy_bytes(bytes + first, ring->data, take - first);
    ring->head = (ring->head + take) % ring->size;
    ring->count -= take;
    return take;
}

void vsscom_ring_clear(vsscom_ring* ring)
{
    ring->head = 0;
    ring->count = 0;
}

// -- Timeouts --------------------------------------------------------------

vsscom_bool vsscom_timeouts_valid(const vsscom_timeouts* timeouts)
{
    return !(timeouts->read_interval == VSSCOM_MAXDWORD &&
             timeouts->read_total_multiplier == VSSCOM_MAXDWORD &&
             timeouts->read_total_constant == VSSCOM_MAXDWORD);
}

// multiplier * length + constant, held at the largest value a timer can take
// below MAXDWORD so that a huge product still counts as "a very long time"
// and never wraps to a short one.
static vsscom_u32 total_ms(vsscom_u32 multiplier, vsscom_u32 constant,
                         vsscom_u32 length)
{
    const vsscom_u64 total = (vsscom_u64)multiplier * length + constant;
    return total >= VSSCOM_MAXDWORD ? VSSCOM_MAXDWORD - 1u : (vsscom_u32)total;
}

vsscom_read_plan vsscom_plan_read(const vsscom_timeouts* timeouts,
                                  vsscom_u32 requested)
{
    vsscom_read_plan plan;
    plan.return_now = 0;
    plan.return_on_first_byte = 0;
    plan.has_total_timer = 0;
    plan.total_ms = 0;
    plan.interval_ms = 0;

    // MAXDWORD interval with no total timeout: return what is there, now.
    if (timeouts->read_interval == VSSCOM_MAXDWORD &&
        timeouts->read_total_multiplier == 0 &&
        timeouts->read_total_constant == 0)
    {
        plan.return_now = 1;
        return plan;
    }

    // MAXDWORD interval and multiplier with a constant: wait up to the constant
    // for the first byte, then return with what is there.
    if (timeouts->read_interval == VSSCOM_MAXDWORD &&
        timeouts->read_total_multiplier == VSSCOM_MAXDWORD &&
        timeouts->read_total_constant > 0 &&
        timeouts->read_total_constant < VSSCOM_MAXDWORD)
    {
        plan.return_on_first_byte = 1;
        plan.has_total_timer = 1;
        plan.total_ms = timeouts->read_total_constant;
        return plan;
    }

    if (timeouts->read_total_multiplier != 0 ||
        timeouts->read_total_constant != 0)
    {
        plan.has_total_timer = 1;
        plan.total_ms = total_ms(timeouts->read_total_multiplier,
                                 timeouts->read_total_constant, requested);
    }
    plan.interval_ms = timeouts->read_interval;
    return plan;
}

vsscom_bool vsscom_read_satisfied(const vsscom_read_plan* plan, vsscom_u32 requested,
                           vsscom_u32 received)
{
    if (received >= requested)
        return 1;
    return plan->return_on_first_byte && received > 0;
}

vsscom_u32 vsscom_write_timeout_ms(const vsscom_timeouts* timeouts,
                                 vsscom_u32 length)
{
    if (timeouts->write_total_multiplier == 0 &&
        timeouts->write_total_constant == 0)
        return 0;
    return total_ms(timeouts->write_total_multiplier,
                    timeouts->write_total_constant, length);
}

// -- Events ----------------------------------------------------------------

void vsscom_events_init(vsscom_events* events)
{
    events->mask = 0;
    events->history = 0;
    events->waiting = 0;
}

vsscom_bool vsscom_events_set_mask(vsscom_events* events, vsscom_u32 mask)
{
    const vsscom_bool release = events->waiting;
    events->mask = mask;
    events->history = 0;
    events->waiting = 0;
    return release;
}

vsscom_bool vsscom_events_wait(vsscom_events* events, vsscom_u32* fired)
{
    if (events->waiting)
        return 0;
    if (events->history != 0)
    {
        *fired = events->history;
        events->history = 0;
        return 1;
    }
    events->waiting = 1;
    return 0;
}

vsscom_bool vsscom_events_raise(vsscom_events* events, vsscom_u32 happened,
                         vsscom_u32* fired)
{
    const vsscom_u32 wanted = happened & events->mask;
    if (wanted == 0)
        return 0;

    events->history |= wanted;
    if (!events->waiting)
        return 0;

    events->waiting = 0;
    *fired = events->history;
    events->history = 0;
    return 1;
}

// -- Modem lines -----------------------------------------------------------

vsscom_u32 vsscom_loopback_lines(vsscom_bool dtr, vsscom_bool rts)
{
    vsscom_u32 lines = 0;
    if (rts)
        lines |= VSSCOM_MSR_CTS;
    if (dtr)
        lines |= VSSCOM_MSR_DSR | VSSCOM_MSR_DCD;
    return lines;
}

vsscom_u32 vsscom_modem_deltas(vsscom_u32 before, vsscom_u32 after)
{
    const vsscom_u32 changed = before ^ after;
    vsscom_u32 deltas = 0;
    if (changed & VSSCOM_MSR_CTS)
        deltas |= VSSCOM_MSR_DCTS;
    if (changed & VSSCOM_MSR_DSR)
        deltas |= VSSCOM_MSR_DDSR;
    if (changed & VSSCOM_MSR_DCD)
        deltas |= VSSCOM_MSR_DDCD;
    return deltas;
}

vsscom_u32 vsscom_modem_events(vsscom_u32 before, vsscom_u32 after)
{
    const vsscom_u32 changed = before ^ after;
    vsscom_u32 events = 0;
    if (changed & VSSCOM_MSR_CTS)
        events |= VSSCOM_EV_CTS;
    if (changed & VSSCOM_MSR_DSR)
        events |= VSSCOM_EV_DSR;
    if (changed & VSSCOM_MSR_DCD)
        events |= VSSCOM_EV_RLSD;
    return events;
}

// -- Flow control ----------------------------------------------------------

// The bits of ControlHandShake and FlowReplace that mean something, from
// ntddser.h. Anything else is refused.
static const vsscom_u32 control_known =
    VSSCOM_DTR_MASK | 0x08u | 0x10u | 0x20u | 0x40u | 0x80000000u;
static const vsscom_u32 flow_known =
    0x01u | 0x02u | 0x04u | 0x08u | 0x10u | VSSCOM_RTS_MASK | 0x80000000u;

vsscom_bool vsscom_flow_valid(const vsscom_flow* flow, vsscom_u32 rx_size)
{
    vsscom_bool uses_limits;

    if ((flow->control_handshake & ~control_known) != 0)
        return 0;
    if ((flow->flow_replace & ~flow_known) != 0)
        return 0;
    // Both DTR bits together mean nothing.
    if ((flow->control_handshake & VSSCOM_DTR_MASK) == VSSCOM_DTR_MASK)
        return 0;

    uses_limits =
        (flow->control_handshake & VSSCOM_DTR_MASK) == VSSCOM_DTR_HANDSHAKE ||
        (flow->flow_replace & VSSCOM_RTS_MASK) == VSSCOM_RTS_HANDSHAKE;
    if (uses_limits &&
        (flow->xon_limit > rx_size || flow->xoff_limit > rx_size))
        return 0;
    return 1;
}

vsscom_bool vsscom_flow_dtr_manual(const vsscom_flow* flow)
{
    return (flow->control_handshake & VSSCOM_DTR_MASK) != VSSCOM_DTR_HANDSHAKE;
}

vsscom_bool vsscom_flow_rts_manual(const vsscom_flow* flow)
{
    return (flow->flow_replace & VSSCOM_RTS_MASK) != VSSCOM_RTS_HANDSHAKE;
}

// A handshake line with hysteresis: low when the free space has run down to
// the xoff limit, high again once the buffer has emptied to the xon limit.
static vsscom_bool handshake_level(const vsscom_flow* flow, vsscom_bool now,
                                   vsscom_u32 rx_count, vsscom_u32 rx_size,
                                   vsscom_bool set)
{
    const vsscom_u32 free_bytes = rx_count >= rx_size ? 0 : rx_size - rx_count;

    if (free_bytes <= flow->xoff_limit)
        return 0;
    if (set || rx_count <= flow->xon_limit)
        return 1;
    return now;
}

vsscom_flow_lines vsscom_flow_update(const vsscom_flow* flow,
                                     vsscom_flow_lines now, vsscom_u32 rx_count,
                                     vsscom_u32 rx_size, vsscom_bool sending,
                                     vsscom_bool set)
{
    vsscom_flow_lines next = now;
    const vsscom_u32 dtr = flow->control_handshake & VSSCOM_DTR_MASK;
    const vsscom_u32 rts = flow->flow_replace & VSSCOM_RTS_MASK;

    if (dtr == VSSCOM_DTR_HANDSHAKE)
        next.dtr = handshake_level(flow, now.dtr, rx_count, rx_size, set);
    else if (set)
        next.dtr = dtr == VSSCOM_DTR_CONTROL;

    if (rts == VSSCOM_RTS_HANDSHAKE)
        next.rts = handshake_level(flow, now.rts, rx_count, rx_size, set);
    else if (rts == VSSCOM_RTS_TOGGLE)
        next.rts = sending ? 1 : 0;
    else if (set)
        next.rts = rts == VSSCOM_RTS_CONTROL;

    return next;
}

// -- Line errors -------------------------------------------------------------

// SERIAL_ERROR_ABORT of ControlHandShake, and SERIAL_ERROR_CHAR and
// SERIAL_BREAK_CHAR of FlowReplace, from ntddser.h.
static const vsscom_u32 control_error_abort = 0x80000000u;
static const vsscom_u32 flow_error_char = 0x04u;
static const vsscom_u32 flow_break_char = 0x10u;

vsscom_bool vsscom_bytes_have(const vsscom_u8* bytes, vsscom_u32 length,
                              vsscom_u8 wanted)
{
    vsscom_u32 index;
    for (index = 0; index < length; ++index)
    {
        if (bytes[index] == wanted)
            return 1;
    }
    return 0;
}

vsscom_bool vsscom_line_error_valid(const vsscom_line_error* error)
{
    return error->errors != 0 &&
           (error->errors & ~(vsscom_u32)VSSCOM_ERROR_MASK) == 0;
}

vsscom_error_effect vsscom_line_error_effect(const vsscom_line_error* error,
                                             const vsscom_flow* flow,
                                             const vsscom_error_chars* chars)
{
    const vsscom_bool error_char = (flow->flow_replace & flow_error_char) != 0;
    const vsscom_bool break_char = (flow->flow_replace & flow_break_char) != 0;
    const vsscom_u32 line = (vsscom_u32)VSSCOM_ERROR_FRAMING |
                            (vsscom_u32)VSSCOM_ERROR_PARITY;
    vsscom_bool keep_byte = error->has_byte;
    vsscom_error_effect effect;

    effect.events = 0;
    effect.count = 0;
    effect.bytes[0] = 0;
    effect.bytes[1] = 0;

    if ((error->errors & (line | (vsscom_u32)VSSCOM_ERROR_OVERRUN)) != 0)
        effect.events |= VSSCOM_EV_ERR;
    if ((error->errors & (vsscom_u32)VSSCOM_ERROR_BREAK) != 0)
        effect.events |= VSSCOM_EV_BREAK;

    if ((error->errors & (vsscom_u32)VSSCOM_ERROR_OVERRUN) != 0 && error_char)
        effect.bytes[effect.count++] = chars->error_char;

    // A break hides a parity or framing error of the same character time.
    if ((error->errors & (vsscom_u32)VSSCOM_ERROR_BREAK) != 0)
    {
        if (break_char)
        {
            effect.bytes[effect.count++] = chars->break_char;
            keep_byte = 0;
        }
    }
    else if ((error->errors & line) != 0 && error_char)
    {
        effect.bytes[effect.count++] = chars->error_char;
        keep_byte = 0;
    }

    if (keep_byte)
    {
        effect.bytes[effect.count++] = error->byte;
        if (error->byte == chars->event_char)
            effect.events |= VSSCOM_EV_RXFLAG;
    }
    if (effect.count != 0)
        effect.events |= VSSCOM_EV_RXCHAR;
    return effect;
}

vsscom_bool vsscom_line_error_apply(vsscom_u32* pending, vsscom_ring* rx,
                                    const vsscom_line_error* error,
                                    const vsscom_error_effect* effect)
{
    if (vsscom_ring_space(rx) < effect->count)
        return 0;
    (void)vsscom_ring_write(rx, effect->bytes, effect->count);
    *pending |= error->errors & (vsscom_u32)VSSCOM_ERROR_MASK;
    return 1;
}

vsscom_u32 vsscom_errors_take(vsscom_u32* pending)
{
    const vsscom_u32 errors = *pending;
    *pending = 0;
    return errors;
}

vsscom_bool vsscom_errors_abort(const vsscom_flow* flow, vsscom_u32 pending)
{
    return pending != 0 && (flow->control_handshake & control_error_abort) != 0;
}

vsscom_u64 vsscom_tx_time(vsscom_u32 baud, vsscom_u32 data_bits,
                          vsscom_u32 parity, vsscom_u32 stop_bits,
                          vsscom_u32 bytes)
{
    // Counted in half bits, so that one and a half stop bits stay whole.
    vsscom_u64 half_bits;

    if (baud == 0)
        return 0;
    half_bits = 2u * (1u + data_bits + (parity != 0 ? 1u : 0u));
    half_bits += stop_bits == 0 ? 2u : (stop_bits == 1 ? 3u : 4u);
    // One half bit lasts 5,000,000 / baud units of 100 ns.
    return ((vsscom_u64)bytes * half_bits * 5000000ull) / baud;
}

vsscom_u64 vsscom_tx_busy_until(vsscom_u64 now, vsscom_u64 busy_until,
                                vsscom_u64 duration)
{
    const vsscom_u64 start = busy_until > now ? busy_until : now;
    return start + duration;
}

vsscom_bool vsscom_tx_sending(vsscom_u32 queued, vsscom_u64 now,
                              vsscom_u64 busy_until)
{
    return queued != 0 || now < busy_until;
}
