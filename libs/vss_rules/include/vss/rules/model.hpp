//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rule set as a harness writes it: plain data, read from JSON by
// parse.hpp and checked by compile.hpp.  docs/response-timing.md describes
// what each part means.
//

#pragma once

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vss::rules {

using microseconds = std::chrono::microseconds;

// -- limits ------------------------------------------------------------------

inline constexpr std::size_t max_rules = 256;
inline constexpr std::size_t max_pattern_bytes = 256;
inline constexpr std::size_t max_reply_bytes = 4096;
inline constexpr std::size_t max_states = 64;
inline constexpr std::size_t max_slots = 64;
inline constexpr std::size_t max_slot_bytes = 4096;
inline constexpr std::size_t max_sequence_steps = 64;
inline constexpr std::size_t max_actions = 16;
inline constexpr std::size_t max_reply_parts = 32;
inline constexpr std::size_t max_delay_table = 256;
inline constexpr std::size_t max_timeline = 1024;
inline constexpr std::size_t max_tight_actions = 16;
inline constexpr std::size_t max_name_bytes = 64;
inline constexpr std::size_t max_text_bytes = 256;
/// The compiled size of one set, in bytes.
inline constexpr std::size_t max_compiled_bytes = std::size_t{1} << 20;
/// The longest delay of an action after its edge: one minute, as a link's.
inline constexpr microseconds max_delay{60'000'000};
/// The latest time of an action on the `run_start` edge: 24 hours.
inline constexpr microseconds max_run_time{86'400'000'000};
/// The shortest time a rule may wait for a plugin's answer.
inline constexpr microseconds min_ask_timeout{2000};
inline constexpr std::uint32_t chance_always = 1'000'000;

// -- where a set sits --------------------------------------------------------

enum class role
{
    /// Sees what the program on a port wrote; sends by writing to the port.
    peer,
    /// Sees what its links deliver to a fixture; sends as the fixture.
    station,
    /// Sees what passes on one direction of a link; sends nothing.
    in_line
};

struct attachment
{
    role as = role::peer;
    /// `peer` and `station`: the port.
    std::string port;
    /// `in_line`: the link's source and target ports.
    std::string from;
    std::string to;
};

// -- when --------------------------------------------------------------------

enum class edge
{
    /// The instant the last byte of the match arrived.
    match_end,
    /// The instant the first byte of the match arrived.
    match_start,
    /// The due time of the action before it in the same list.
    previous,
    /// The start of the run.
    run_start
};

enum class delay_kind
{
    fixed,
    uniform,
    normal,
    exponential,
    table
};

/// How long after its edge an action is due.  Every kind has hard bounds.
struct delay_spec
{
    delay_kind kind = delay_kind::fixed;
    /// `fixed`.
    microseconds fixed{0};
    /// `uniform`, `normal` (both required) and the lower cut of the rest.
    microseconds min{0};
    /// `uniform`, `normal`, `exponential`: the upper cut (required).
    microseconds max{0};
    /// `normal`, `exponential`.
    microseconds mean{0};
    /// `normal`.
    microseconds sd{0};
    /// `table`: the values, and whether they are taken in order (wrapping)
    /// or picked by the seeded source.
    std::vector<microseconds> values;
    bool in_order = true;
};

enum class precision
{
    normal,
    tight
};

struct timing
{
    edge after = edge::match_end;
    delay_spec delay;
    precision how = precision::normal;
};

// -- what a rule matches -----------------------------------------------------

enum class gap_where
{
    before,
    after,
    both
};

/// A line and the value a rule waits for it to take, or gives it.
struct line_value
{
    control_line which = control_line::dtr;
    bool high = false;
};

enum class port_event
{
    opened,
    closed
};

/// What a rule waits for.  Exactly one of `bytes`, `lines` and `port` is set.
struct trigger
{
    /// The pattern, 1 to #max_pattern_bytes bytes.
    byte_buffer bytes;
    /// Which bits of `bytes` count: a 1 bit must be equal.  Empty means every
    /// bit; otherwise as long as `bytes`.
    byte_buffer mask;
    /// With `bytes`: the pattern counts only with this much silence around
    /// it.  Zero means no condition.
    microseconds gap{0};
    gap_where where = gap_where::after;
    /// Lines that took these values (an edge).  All must hold after the
    /// change and at least one must have changed.
    std::vector<line_value> lines;
    std::optional<port_event> port;
    /// The rule is considered only in this state.  Empty means any.
    std::string state;
};

// -- what a rule does --------------------------------------------------------

enum class check_kind
{
    crc16_modbus,
    crc16_ccitt,
    sum8,
    xor8,
    lrc
};

enum class part_kind
{
    literal,
    slot,
    match,
    check
};

/// One piece of a reply.
struct reply_part
{
    part_kind kind = part_kind::literal;
    /// `literal`.
    byte_buffer bytes;
    /// `slot`.
    std::string slot;
    /// `match`: bytes `first` to `last` of the match, inclusive.  `check`
    /// with `whole` false: the same of the reply built so far.
    std::size_t first = 0;
    std::size_t last = 0;
    /// `check`.
    check_kind check = check_kind::crc16_modbus;
    /// `check`: over the whole reply built so far.
    bool whole = true;
};

enum class fault_kind
{
    none,
    drop,
    corrupt,
    truncate,
    duplicate
};

/// What goes wrong with a reply, or with the matched bytes on a link.
struct fault_spec
{
    fault_kind kind = fault_kind::none;
    /// `corrupt`: these bytes are xored in from `at`.  Past the end nothing
    /// is changed.
    std::size_t at = 0;
    byte_buffer xor_with;
    /// `truncate`: the length kept.
    std::size_t keep = 0;
};

enum class line_error
{
    parity,
    framing,
    overrun,
    break_signal,
    hang_up,
    disconnect
};

enum class action_kind
{
    /// `peer`, `station`: send bytes, perhaps with a fault.
    reply,
    /// `in_line`: let the matched bytes through, held for the delay, perhaps
    /// with a fault (`drop`, `corrupt`, `truncate`, `duplicate`).
    pass,
    /// `peer`, `station`: set output lines.
    set_lines,
    /// `peer`, `station`: raise a line error on the port.
    raise_error,
    /// Move the set to a state.  At the match; takes no time.
    set_state,
    /// Copy bytes of the match into a slot.  At the match; takes no time.
    set_slot,
    /// Publish an event with a text.
    mark,
    /// Hand the decision to the plugin, with a time limit.
    ask
};

struct action
{
    action_kind kind = action_kind::mark;
    timing when;
    /// `reply`.
    std::vector<reply_part> parts;
    /// `reply`, `pass`.
    fault_spec fault;
    /// `set_lines`.
    std::vector<line_value> lines;
    /// `raise_error`.
    line_error error = line_error::parity;
    /// `set_state`.
    std::string state;
    /// `set_slot`: the slot, and bytes `first` to `last` of the match.
    std::string slot;
    std::size_t first = 0;
    std::size_t last = 0;
    /// `mark`.
    std::string text;
    /// `ask`: the limit, and what happens when no answer came (an index
    /// into the same list is not needed: these are whole actions).
    microseconds timeout{0};
    std::vector<action> on_timeout;
};

/// One step of a rule's sequence.
struct step
{
    std::vector<action> actions;
    /// The share of matches the step applies to, in parts per million.
    std::uint32_t chance_ppm = chance_always;
};

enum class after_sequence
{
    /// Start again from the first step.
    repeat,
    /// The last step from then on.
    hold,
    /// The rule matches no more.
    stop
};

struct rule
{
    std::string name;
    trigger when;
    /// At least one step.  A rule with one `do` list has one step.
    std::vector<step> sequence;
    /// The rule was written with a `do` list and no `sequence`, so a problem
    /// in it is reported at `rules[i].do[j]`, where the writer will find it.
    bool written_as_do = false;
    after_sequence then = after_sequence::repeat;
    /// The most matches of the rule; zero means no limit.
    std::uint64_t count = 0;
};

/// An action at a time of the run, with no match.
struct timeline_entry
{
    microseconds at{0};
    std::vector<action> actions;
};

enum class ordering
{
    /// An action is never due before one from an earlier match.
    fifo,
    independent
};

struct rule_set_source
{
    std::string name;
    attachment attach;
    std::optional<std::uint64_t> seed;
    /// An action later than this is counted and flagged.  Zero: never.
    microseconds late_after{0};
    ordering order = ordering::fifo;
    /// The first state is the one the set starts in.
    std::vector<std::string> states;
    std::vector<std::pair<std::string, byte_buffer>> slots;
    std::vector<rule> rules;
    std::vector<timeline_entry> timeline;
};

/// One thing wrong with a rule set, and where.
struct problem
{
    /// Like `rules[2].do[0].delay.uniform`.
    std::string path;
    std::string message;
};

} // namespace vss::rules
