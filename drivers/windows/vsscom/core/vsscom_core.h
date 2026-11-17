//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The serial rules of the virtual COM port, with no dependency on the kernel.
//
// The driver (driver.c) turns Windows requests into calls to these functions
// and carries out what they decide: complete a request, keep it waiting, arm a
// timer. The same code is built for the host and unit-tested there, because a
// driver cannot be debugged cheaply and these rules are where mistakes hide:
// read timeouts, wait-mask events, the byte ring and the modem lines.
//
// Plain C, no allocation, no locking. The caller serialises access.
//

#ifndef VSSCOM_CORE_H
#define VSSCOM_CORE_H

// No C library headers: the kernel build of this file must not pull in the
// user-mode runtime, so the few types it needs are defined here. Every system
// the studio supports has a 32-bit unsigned int and a 64-bit unsigned long long.
typedef unsigned char vsscom_u8;
typedef unsigned int vsscom_u32;
typedef unsigned long long vsscom_u64;
// 0 or 1.
typedef unsigned char vsscom_bool;

/// The value Windows uses for "no limit" in a timeout field.
#define VSSCOM_MAXDWORD 0xFFFFFFFFu

// -- The byte ring
// ---------------------------------------------------------

typedef struct vsscom_ring
{
    vsscom_u8* data;
    vsscom_u32 size;
    vsscom_u32 head; // index of the oldest byte
    vsscom_u32 count;
} vsscom_ring;

/// `storage` is owned by the caller and must hold `size` bytes.
void vsscom_ring_init(vsscom_ring* ring, vsscom_u8* storage, vsscom_u32 size);
vsscom_u32 vsscom_ring_count(const vsscom_ring* ring);
vsscom_u32 vsscom_ring_space(const vsscom_ring* ring);
/// Copies as many bytes as fit and returns how many.
vsscom_u32 vsscom_ring_write(vsscom_ring* ring, const vsscom_u8* bytes,
                           vsscom_u32 length);
/// Removes up to `length` of the oldest bytes and returns how many.
vsscom_u32 vsscom_ring_read(vsscom_ring* ring, vsscom_u8* bytes,
                          vsscom_u32 length);
void vsscom_ring_clear(vsscom_ring* ring);

/// Takes back the `length` bytes written last (fewer when the ring holds
/// fewer, because the reader already took the rest). Returns how many it
/// dropped. The bytes before them stay.
vsscom_u32 vsscom_ring_drop_newest(vsscom_ring* ring, vsscom_u32 length);

// -- Timeouts
// --------------------------------------------------------------

/// The five numbers of COMMTIMEOUTS, in milliseconds.
typedef struct vsscom_timeouts
{
    vsscom_u32 read_interval;
    vsscom_u32 read_total_multiplier;
    vsscom_u32 read_total_constant;
    vsscom_u32 write_total_multiplier;
    vsscom_u32 write_total_constant;
} vsscom_timeouts;

/// False for the one combination Windows refuses: all three read values set
/// to MAXDWORD.
vsscom_bool vsscom_timeouts_valid(const vsscom_timeouts* timeouts);

/// What a read of `requested` bytes does under the current timeouts.
typedef struct vsscom_read_plan
{
    /// Complete at once with whatever is in the ring, possibly nothing.
    vsscom_bool return_now;
    /// Complete as soon as one byte is available, with all that is there.
    vsscom_bool return_on_first_byte;
    /// A total timer applies; `total_ms` is its length.
    vsscom_bool has_total_timer;
    vsscom_u32 total_ms;
    /// The gap allowed between bytes once the first has arrived, or 0 for
    /// none.
    vsscom_u32 interval_ms;
} vsscom_read_plan;

vsscom_read_plan vsscom_plan_read(const vsscom_timeouts* timeouts,
                                  vsscom_u32 requested);

/// True when a read has everything it asked for, or the plan says that what
/// it has is enough. Timers are the caller's: an expired timer completes
/// the read with what it holds.
vsscom_bool vsscom_read_satisfied(const vsscom_read_plan* plan, vsscom_u32 requested,
                           vsscom_u32 received);

/// The total write timeout for `length` bytes in milliseconds, or 0 for
/// none.
vsscom_u32 vsscom_write_timeout_ms(const vsscom_timeouts* timeouts,
                                 vsscom_u32 length);

// -- Events (SetCommMask and WaitCommEvent) --------------------------------

// The same values as SERIAL_EV_* in ntddser.h and EV_* in winbase.h.
#define VSSCOM_EV_RXCHAR 0x0001u
#define VSSCOM_EV_RXFLAG 0x0002u
#define VSSCOM_EV_TXEMPTY 0x0004u
#define VSSCOM_EV_CTS 0x0008u
#define VSSCOM_EV_DSR 0x0010u
#define VSSCOM_EV_RLSD 0x0020u
#define VSSCOM_EV_BREAK 0x0040u
#define VSSCOM_EV_ERR 0x0080u
#define VSSCOM_EV_RING 0x0100u

typedef struct vsscom_events
{
    vsscom_u32 mask; // the events the application asked about
    vsscom_u32
        history;  // masked events that happened while nobody was waiting
    vsscom_bool waiting; // a wait request is pending
} vsscom_events;

void vsscom_events_init(vsscom_events* events);

/// Sets the mask. History is dropped. Returns true when a pending wait must
/// be completed now with no events.
vsscom_bool vsscom_events_set_mask(vsscom_events* events, vsscom_u32 mask);

/// Starts a wait. Returns true and sets `*fired` when events are already
/// waiting in the history; the history is then empty. Returns false when
/// the request must stay pending (and marks it pending). A second wait
/// while one is pending also returns false and leaves `*fired` untouched;
/// the caller checks `waiting` first and refuses it.
vsscom_bool vsscom_events_wait(vsscom_events* events, vsscom_u32* fired);

/// Records events that happened. Returns true and sets `*fired` when a
/// pending wait completes because of them.
vsscom_bool vsscom_events_raise(vsscom_events* events, vsscom_u32 happened,
                         vsscom_u32* fired);

// -- Modem lines -----------------------------------------------------------

// The same values as SERIAL_MSR_* in ntddser.h.
#define VSSCOM_MSR_DCTS 0x01u
#define VSSCOM_MSR_DDSR 0x02u
#define VSSCOM_MSR_TERI 0x04u
#define VSSCOM_MSR_DDCD 0x08u
#define VSSCOM_MSR_CTS 0x10u
#define VSSCOM_MSR_DSR 0x20u
#define VSSCOM_MSR_RI 0x40u
#define VSSCOM_MSR_DCD 0x80u

/// The input lines a loopback port reports: RTS comes back as CTS, and DTR
/// as DSR and carrier detect, the way a null-modem cable to itself would.
vsscom_u32 vsscom_loopback_lines(vsscom_bool dtr, vsscom_bool rts);

/// The change bits (DCTS, DDSR, DDCD) between two readings of the level
/// bits.
vsscom_u32 vsscom_modem_deltas(vsscom_u32 before, vsscom_u32 after);

/// The events (CTS, DSR, RLSD) that a change between two readings raises.
vsscom_u32 vsscom_modem_events(vsscom_u32 before, vsscom_u32 after);

// -- Flow control (IOCTL_SERIAL_SET_HANDFLOW) ------------------------------
//
// The bits are the ones of SERIAL_HANDFLOW in ntddser.h, so that the driver
// passes its structure through. Only the lines the driver itself drives are
// here: DTR and RTS. Output flow control (stop sending while CTS or DSR is
// low, XON/XOFF) is not applied.

// Enumerators and not macros, because the preprocessor policy counts directives.
enum
{
    VSSCOM_DTR_CONTROL = 0x01,   // SERIAL_DTR_CONTROL: DTR high
    VSSCOM_DTR_HANDSHAKE = 0x02, // SERIAL_DTR_HANDSHAKE: follows the receive buffer
    VSSCOM_DTR_MASK = 0x03,
    VSSCOM_RTS_CONTROL = 0x40,   // SERIAL_RTS_CONTROL: RTS high
    VSSCOM_RTS_HANDSHAKE = 0x80, // SERIAL_RTS_HANDSHAKE: follows the receive buffer
    VSSCOM_RTS_TOGGLE = 0xC0,    // SERIAL_TRANSMIT_TOGGLE: high while sending
    VSSCOM_RTS_MASK = 0xC0
};

/// The four fields of SERIAL_HANDFLOW that decide what DTR and RTS do.
/// `control_handshake` and `flow_replace` carry every bit of the structure,
/// including those that this file does not interpret, so that they validate.
typedef struct vsscom_flow
{
    vsscom_u32 control_handshake;
    vsscom_u32 flow_replace;
    /// A handshake line goes high again when this many bytes or fewer are
    /// in the receive buffer.
    vsscom_u32 xon_limit;
    /// A handshake line goes low when this many bytes or fewer are free.
    vsscom_u32 xoff_limit;
} vsscom_flow;

typedef struct vsscom_flow_lines
{
    vsscom_bool dtr;
    vsscom_bool rts;
} vsscom_flow_lines;

/// False for a combination Windows refuses with STATUS_INVALID_PARAMETER: a bit
/// that means nothing, DTR_CONTROL and DTR_HANDSHAKE together, or a limit larger
/// than the receive buffer while a handshake is in use.
vsscom_bool vsscom_flow_valid(const vsscom_flow* flow, vsscom_u32 rx_size);

/// True when the application may set or clear DTR (or RTS) itself. A line that
/// follows the receive buffer is the driver's to move, so setting it is refused.
vsscom_bool vsscom_flow_dtr_manual(const vsscom_flow* flow);
vsscom_bool vsscom_flow_rts_manual(const vsscom_flow* flow);

/// The levels of DTR and RTS after a change.
///
/// `set` is true when the application has just given the settings: enable
/// drives the line high and disable drives it low. When it is false, only the
/// lines the settings drive on their own change: a handshake line follows the
/// receive buffer (low when `xoff_limit` bytes or fewer are free, high again
/// at `xon_limit` bytes or fewer in use, unchanged in between), and a toggle
/// RTS is high exactly while `sending` is true.
vsscom_flow_lines vsscom_flow_update(const vsscom_flow* flow,
                                     vsscom_flow_lines now, vsscom_u32 rx_count,
                                     vsscom_u32 rx_size, vsscom_bool sending,
                                     vsscom_bool set);

/// True when `wanted` is among the `length` bytes. The driver uses it for
/// EV_RXFLAG: the event character arrived.
vsscom_bool vsscom_bytes_have(const vsscom_u8* bytes, vsscom_u32 length,
                              vsscom_u8 wanted);

// -- Line errors (ClearCommError, EV_ERR, EV_BREAK) --------------------------
//
// The hub end raises a line error as a wire would: a character time that
// arrived bad. The error is latched until the application reads it with
// IOCTL_SERIAL_GET_COMMSTATUS (ClearCommError), raises EV_ERR or EV_BREAK, and
// may put a replacement character in the receive stream. The rules follow the
// serial driver sample that Microsoft publishes (SerialProcessLSR in isr.c).

// The same values as SERIAL_ERROR_* in ntddser.h.
enum
{
    VSSCOM_ERROR_BREAK = 0x01,
    VSSCOM_ERROR_FRAMING = 0x02,
    VSSCOM_ERROR_OVERRUN = 0x04,
    VSSCOM_ERROR_QUEUEOVERRUN = 0x08,
    VSSCOM_ERROR_PARITY = 0x10,
    VSSCOM_ERROR_MASK = 0x1F
};

/// One bad character time. `errors` is one or more VSSCOM_ERROR_* bits. With
/// `has_byte`, `byte` is the character the receiver made of it, which the
/// application reads unless its settings replace it.
typedef struct vsscom_line_error
{
    vsscom_u32 errors;
    vsscom_bool has_byte;
    vsscom_u8 byte;
} vsscom_line_error;

/// The three characters of SERIAL_CHARS that the receive path looks at.
typedef struct vsscom_error_chars
{
    vsscom_u8 error_char;
    vsscom_u8 break_char;
    vsscom_u8 event_char;
} vsscom_error_chars;

/// What a line error does: the events it raises and the bytes it adds to the
/// receive stream, at most two.
typedef struct vsscom_error_effect
{
    vsscom_u32 events;
    vsscom_u32 count;
    vsscom_u8 bytes[2];
} vsscom_error_effect;

/// False when `errors` is empty or has a bit that is not an error.
vsscom_bool vsscom_line_error_valid(const vsscom_line_error* error);

/// The effect of `error` under the application's settings.
///
/// Framing, overrun and parity raise EV_ERR; a break raises EV_BREAK; a queue
/// overrun raises nothing and is only latched. The stream gets, in this order:
/// the error character for an overrun when SERIAL_ERROR_CHAR is set (the
/// overrun lost an earlier character; the one that arrived is still good);
/// then, for a break, the break character when SERIAL_BREAK_CHAR is set, or,
/// for a parity or framing error, the error character when SERIAL_ERROR_CHAR
/// is set, either of which replaces the byte; otherwise the byte itself, if
/// there is one. Any byte raises EV_RXCHAR, and the byte itself raises
/// EV_RXFLAG when it is the event character.
vsscom_error_effect vsscom_line_error_effect(const vsscom_line_error* error,
                                             const vsscom_flow* flow,
                                             const vsscom_error_chars* chars);

/// Latches the error in `*pending` and puts the bytes of `effect` in `rx`.
/// Returns false, and changes nothing, when the ring has no room for them: the
/// error waits its turn behind the bytes that came before it, as on a wire.
vsscom_bool vsscom_line_error_apply(vsscom_u32* pending, vsscom_ring* rx,
                                    const vsscom_line_error* error,
                                    const vsscom_error_effect* effect);

/// ClearCommError: returns the latched errors and clears them.
vsscom_u32 vsscom_errors_take(vsscom_u32* pending);

/// True while SERIAL_ERROR_ABORT is set and an error is latched: every request
/// of the application but IOCTL_SERIAL_GET_COMMSTATUS then ends with
/// STATUS_CANCELLED, and so do its waiting reads and writes.
vsscom_bool vsscom_errors_abort(const vsscom_flow* flow, vsscom_u32 pending);

/// How long `bytes` bytes take to leave a UART, in units of 100 ns. The frame
/// is one start bit, `data_bits` data bits, a parity bit unless `parity` is 0,
/// and the stop bits (`stop_bits` is 0 for one, 1 for one and a half, 2 for
/// two, as in SERIAL_LINE_CONTROL). A baud rate of 0 gives 0.
vsscom_u64 vsscom_tx_time(vsscom_u32 baud, vsscom_u32 data_bits,
                          vsscom_u32 parity, vsscom_u32 stop_bits,
                          vsscom_u32 bytes);

/// When the last byte of a write that starts at `now` will have left, given
/// that earlier bytes leave until `busy_until` (0 for none). Times are in 100
/// ns units on one clock.
vsscom_u64 vsscom_tx_busy_until(vsscom_u64 now, vsscom_u64 busy_until,
                                vsscom_u64 duration);

/// True while bytes are queued or the last one is still leaving.
vsscom_bool vsscom_tx_sending(vsscom_u32 queued, vsscom_u64 now,
                              vsscom_u64 busy_until);

// -- Break and write marks (the hub reads where the program's calls fell) ----
//
// The hub reads the bytes the program wrote as one stream. Two things about the
// program's calls are lost in that stream: where it called SetCommBreak and
// ClearCommBreak among the bytes, and where each WriteFile began. The marks keep
// them. Every mark is a time (the raw count of the performance counter, the
// driver does not convert it) and an offset: how many bytes the program had put
// into Tx before the call, counted from the start of the hub's handle.
//
// The offsets are exact because the caller takes the same lock for the ring and
// for the marks. `written` is the bytes the ring has taken in and not given back;
// `read` is the bytes the hub has read. Written minus read is what Tx holds.

enum
{
    VSSCOM_MARKS_MAX_BREAKS = 64,
    VSSCOM_MARKS_MAX_WRITES = 256,
    // vsscom_break_record.flags
    VSSCOM_BREAK_OPEN = 1 // still held: off_time and off_offset mean nothing
};

typedef struct vsscom_break_record
{
    vsscom_u64 on_time;
    vsscom_u64 off_time;
    vsscom_u64 on_offset;
    vsscom_u64 off_offset;
    vsscom_u32 flags;
    vsscom_u32 reserved;
} vsscom_break_record;

typedef struct vsscom_write_record
{
    vsscom_u64 time;   // when the program called WriteFile
    vsscom_u64 offset; // Tx offset of the first byte this call put in
} vsscom_write_record;

typedef struct vsscom_marks
{
    vsscom_u64 written;
    vsscom_u64 read;
    // Oldest first. A break that is still held is the last one, with
    // VSSCOM_BREAK_OPEN set.
    vsscom_break_record breaks[VSSCOM_MARKS_MAX_BREAKS];
    vsscom_u32 break_head;
    vsscom_u32 break_count;
    vsscom_write_record writes[VSSCOM_MARKS_MAX_WRITES];
    vsscom_u32 write_head;
    vsscom_u32 write_count;
    // Since the last take.
    vsscom_u32 breaks_dropped;
    vsscom_u32 writes_merged;
    // Breaks begun since the port was made; wraps.
    vsscom_u32 break_total;
} vsscom_marks;

void vsscom_marks_init(vsscom_marks* marks);

/// The hub handle opened or closed: Tx is empty and the offsets start again at 0.
/// Closed breaks and the writes are forgotten, since nobody read them; a break
/// that is held stays, at offset 0, and its time is kept.
void vsscom_marks_reset(vsscom_marks* marks);

/// `count` bytes went into Tx. `now` is the time of the call that brought them;
/// `first` is true for the first bytes of that call, which makes a write mark.
/// A full set of write marks drops the oldest and counts it as merged.
void vsscom_marks_stored(vsscom_marks* marks, vsscom_u32 count, vsscom_u64 now,
                         vsscom_bool first);

/// The hub read `count` bytes.
void vsscom_marks_read(vsscom_marks* marks, vsscom_u32 count);

/// `count` of the newest bytes left Tx without being read (a purge, an abort).
/// Marks that pointed past the new end are removed or moved back to it.
void vsscom_marks_taken_back(vsscom_marks* marks, vsscom_u32 count);

/// Tx was emptied.
void vsscom_marks_cleared(vsscom_marks* marks);

/// SetCommBreak. Returns false when a break is already held.
vsscom_bool vsscom_marks_break_on(vsscom_marks* marks, vsscom_u64 now);

/// ClearCommBreak. Returns false when none is held.
vsscom_bool vsscom_marks_break_off(vsscom_marks* marks, vsscom_u64 now);

vsscom_bool vsscom_marks_break_held(const vsscom_marks* marks);

/// Copies what the hub has not been given into the arrays, which hold
/// VSSCOM_MARKS_MAX_BREAKS and VSSCOM_MARKS_MAX_WRITES records, and takes it out
/// of the marks: every break that ended, and every write mark that points at a
/// byte the hub has read. A break that is held is copied and stays. The drop and
/// merge counts are returned and start again from 0.
void vsscom_marks_take(vsscom_marks* marks, vsscom_break_record* breaks,
                       vsscom_u32* break_count, vsscom_write_record* writes,
                       vsscom_u32* write_count, vsscom_u32* breaks_dropped,
                       vsscom_u32* writes_merged);

#endif // VSSCOM_CORE_H
