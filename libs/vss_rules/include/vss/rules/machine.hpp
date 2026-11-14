//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The running form of one rule set at one place: it is given bytes, line
// changes and times, and says which rule fired and what is to happen when.
// It has no clock, no timer and no thread: every call is given the time, so
// a test writes the times.  It does nothing itself; carrying out what it
// plans is the engine's work.
//
// Not thread safe.  The engine calls it from one strand.
//

#pragma once

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/error.hpp>
#include <vss/core/stats.hpp>
#include <vss/rules/compile.hpp>
#include <vss/rules/model.hpp>
#include <vss/rules/random.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace vss::rules {

/// A check value of a reply, appended to `to`: two bytes for the CRCs (low
/// byte first for `crc16_modbus`, high byte first for `crc16_ccitt`), one for
/// the rest.
void append_check(check_kind kind, bytes_view over, byte_buffer& to);

/// One action of a firing, with its time worked out.
struct planned_action
{
    action_kind kind = action_kind::mark;
    /// Its place in the step's list.
    std::size_t index = 0;
    /// When it is to happen.  For `set_state` and `set_slot` of a rule this
    /// is the time of the match, and the change is already made.
    time_point due{};
    /// The delay that was drawn for it, before any `fifo` adjustment.
    microseconds drawn{0};
    precision how = precision::normal;
    /// `reply`: the bytes to send, after the fault.  `pass`: the matched
    /// bytes, after the fault.
    byte_buffer bytes;
    /// How many times `bytes` is sent: 0 after `drop`, 2 after `duplicate`.
    unsigned copies = 1;
    fault_kind fault = fault_kind::none;
    /// The reply was longer than #max_reply_bytes and was cut.
    bool cut = false;
    /// The action as compiled, for the fields a kind does not change
    /// (`lines`, `error`, `text`, `timeout`, `on_timeout`).  It points into
    /// the compiled set, which the machine keeps alive.
    const compiled_action* source = nullptr;
};

/// One match of one rule, or one entry of the timeline.
struct firing
{
    /// The rule, or #no_index for the timeline.
    std::size_t rule = no_index;
    /// The step of the rule's sequence, or the entry of the timeline.
    std::size_t step = 0;
    /// Which match of the rule this is, from 1.
    std::uint64_t ordinal = 0;
    /// The stream offset of the first matched byte, and how many there are.
    /// A rule that matches lines or the port has no bytes.
    std::uint64_t offset = 0;
    std::size_t length = 0;
    /// When the first and the last matched byte arrived.  For a rule on
    /// silence both are the instant the silence was complete, and for a rule
    /// on a signal the instant the signal arrived.
    time_point start{};
    time_point end{};
    /// A rule on a signal: which signal, the time of the event the plugin
    /// decoded, and how many bytes the signal carried.
    std::size_t signal = no_index;
    time_point cause{};
    std::size_t signal_bytes = 0;
    /// The step's `chance_ppm` left it out this time.  No action is planned.
    bool skipped = false;
    std::vector<planned_action> actions;
};

struct machine_stats
{
    std::uint64_t bytes_seen = 0;
    /// Bytes that were part of a match.
    std::uint64_t bytes_matched = 0;
    std::uint64_t firings = 0;
    std::uint64_t skipped = 0;
    std::uint64_t replies = 0;
    std::uint64_t reply_bytes = 0;
    /// Replies a fault dropped, changed, cut or doubled.
    std::uint64_t faults = 0;
};

class machine
{
public:
    /// `run_seed` is used when the set has no seed of its own.
    machine(std::shared_ptr<const compiled_rule_set> set,
            std::uint64_t run_seed);

    const compiled_rule_set& set() const noexcept { return *set_; }
    /// The seed in use, for the log.
    std::uint64_t seed() const noexcept { return seed_; }

    /// Fixes the start of the run and returns the timeline, one firing for
    /// each entry in time order.  Nothing of it has happened yet: a
    /// `set_state` among its actions is the engine's to apply when due.
    std::vector<firing> start(time_point origin);

    /// Bytes that arrived at `when`.  Times must not go backwards.  Firings
    /// are appended to `out` in the order they were decided.
    ///
    /// A match is decided when its last byte arrives: of the rules that
    /// complete on the same byte, the longest pattern wins (it started
    /// first), then the rule listed first.  A match uses up the bytes: every
    /// partial match is dropped.  So what matches does not depend on how the
    /// bytes were split into chunks.  A rule that needs silence after its
    /// pattern is decided when the silence has passed (see #tick), and loses
    /// to any rule that completes on the same byte without that need.
    void feed(bytes_view data, time_point when, std::vector<firing>& out);

    /// The lines of the place changed.  The first rule listed that matches
    /// the change fires; it uses up no bytes.
    void lines_changed(control_lines before, control_lines after,
                       time_point when, std::vector<firing>& out);

    /// A program opened or closed the port.
    void port_changed(port_event what, time_point when,
                      std::vector<firing>& out);

    /// The owner of the set raised signal `index` of the set's `signals`,
    /// which arrived at `when`.  `cause` is the time of the event the plugin
    /// decoded, and `carried` what it sent with the signal, cut to
    /// #max_signal_bytes; a rule's actions use those bytes as they use the
    /// bytes of a match.  The first rule listed that matches the signal
    /// fires; it uses up no bytes of the stream.  Whether the signal may be
    /// raised at all (its owner, the age of `cause`) is the engine's to
    /// decide before it calls this.
    void signal(std::size_t index, bytes_view carried, time_point cause,
                time_point when, std::vector<firing>& out);

    /// Time has passed with no bytes.  Fires, in the order of their times,
    /// the rule that was waiting for silence after its pattern and every
    /// rule on `quiet` whose silence is complete at `now`.
    ///
    /// A rule on `quiet` fires once for each silence, at the instant the
    /// silence was complete (not at `now`), and only if the set is in the
    /// rule's state at that instant; one that was not is not tried again
    /// until bytes have arrived.  Silence is counted from the last byte, so
    /// nothing fires before the first byte has arrived.
    void tick(time_point now, std::vector<firing>& out);

    /// When #tick has to be called next, if a rule is waiting for silence.
    std::optional<time_point> next_deadline() const;

    std::size_t state() const noexcept { return state_; }
    /// Moves the set to a state: `errc::not_found` for one it does not have.
    /// The engine calls it for a `set_state` of the timeline when that entry
    /// is due, and to carry a state over to a set that replaces this one.
    result<void> set_state(std::size_t index);
    bytes_view slot(std::size_t index) const;
    /// `errc::not_found` for a slot that does not exist and
    /// `errc::invalid_argument` for more than #max_slot_bytes.
    result<void> set_slot(std::size_t index, bytes_view value);

    /// The bytes that arrived since the last match and could still be the
    /// start of one: at most the longest pattern less one.  A set that
    /// replaces this one is fed them first.
    byte_buffer unmatched_tail() const;

    const machine_stats& stats() const noexcept { return stats_; }

private:
    struct rule_state
    {
        /// Where the rule's words are in `shift_`.
        std::size_t first_word = 0;
        std::uint64_t matches = 0;
        bool finished = false;
        /// A rule on `quiet`: it has had its turn in this silence.
        bool quiet_spent = false;
        generator source;
        /// How often each step has run, for a delay table taken in order.
        std::vector<std::uint64_t> step_runs;
    };

    /// A chunk boundary: the offset of the chunk's first byte, when it
    /// arrived, and how long nothing had arrived before it.
    struct boundary
    {
        std::uint64_t offset = 0;
        time_point when{};
        clock_type::duration quiet{};
    };

    /// A rule whose pattern is complete and that waits for silence.
    struct waiting
    {
        std::size_t rule = 0;
        std::uint64_t offset = 0;
        time_point start{};
        time_point end{};
        time_point deadline{};
        byte_buffer bytes;
    };

    bool advance(std::size_t rule, byte value);
    bool gap_before_holds(const compiled_rule& rule,
                          std::uint64_t first_offset) const;
    time_point time_of(std::uint64_t offset) const;
    /// `cause` is given for a rule on a signal, whose `matched` are the
    /// bytes the signal carried and not bytes of the stream.
    void fire(std::size_t rule, std::uint64_t offset, bytes_view matched,
              time_point start, time_point end, std::vector<firing>& out,
              const time_point* cause = nullptr);
    void fire_waiting(const waiting& ready, std::vector<firing>& out);
    time_point quiet_deadline(std::size_t rule) const;
    void plan(const std::vector<compiled_action>& actions, rule_state* runtime,
              std::uint64_t position, bytes_view matched, firing& into);
    byte_buffer render(const compiled_action& action, bytes_view matched,
                       bool& cut) const;
    void consume();
    void fire_event_rule(std::size_t rule, time_point when,
                         std::vector<firing>& out);

    std::shared_ptr<const compiled_rule_set> set_;
    std::uint64_t seed_ = 0;
    time_point origin_{};
    std::size_t state_ = 0;
    std::vector<byte_buffer> slots_;
    std::vector<rule_state> rules_;
    /// The rules on `quiet`, the shortest silence first, then as listed.
    std::vector<std::size_t> quiet_order_;
    std::vector<std::uint64_t> shift_;

    std::uint64_t seen_ = 0;
    bool any_seen_ = false;
    time_point last_arrival_{};
    /// The bytes since the last match, cut to the longest pattern.
    byte_buffer recent_;
    std::vector<boundary> boundaries_;
    std::optional<waiting> waiting_;
    /// The latest due time planned, for `fifo`.
    time_point last_due_{};
    machine_stats stats_;
};

} // namespace vss::rules
