//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Checks a rule set and turns it into the form the engine runs: names
// resolved to indices, a match table for each pattern, and the bounds the
// engine needs before it installs the set.  A pure function: no hub, no
// clock, no threads.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/rules/model.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vss::rules {

/// "No state", "no slot".
inline constexpr std::size_t no_index = static_cast<std::size_t>(-1);

struct compiled_part
{
    reply_part part;
    /// The slot of a `slot` part.
    std::size_t slot = no_index;
};

struct compiled_action
{
    action_kind kind = action_kind::mark;
    timing when;
    std::vector<compiled_part> parts;
    fault_spec fault;
    std::vector<line_value> lines;
    line_error error = line_error::parity;
    /// `set_state`.
    std::size_t state = no_index;
    /// `set_slot`.
    std::size_t slot = no_index;
    std::size_t first = 0;
    std::size_t last = 0;
    std::string text;
    microseconds timeout{0};
    std::vector<compiled_action> on_timeout;
};

struct compiled_step
{
    std::vector<compiled_action> actions;
    std::uint32_t chance_ppm = chance_always;
};

enum class trigger_kind
{
    bytes,
    lines,
    port,
    quiet,
    signal
};

struct compiled_rule
{
    std::string name;
    trigger_kind kind = trigger_kind::bytes;

    /// The length of the pattern, and how many 64-bit words hold one bit for
    /// each of its bytes.
    std::size_t length = 0;
    std::size_t words = 0;
    /// For each byte value, the positions of the pattern that accept it:
    /// `accept[value * words + w]`, bit `i` of the row for position `i`.
    std::vector<std::uint64_t> accept;
    microseconds gap{0};
    gap_where where = gap_where::after;

    std::vector<line_value> lines;
    port_event port = port_event::opened;
    /// `quiet`: how long nothing must have arrived.
    microseconds quiet{0};
    /// `signal`: which of the set's signals.
    std::size_t signal = no_index;

    /// The state the rule needs, or #no_index for any.
    std::size_t state = no_index;
    std::vector<compiled_step> sequence;
    /// The rule was written with a `do` list and no `sequence`, so a problem
    /// found later is reported at `rules[i].do[j]`.
    bool written_as_do = false;
    after_sequence then = after_sequence::repeat;
    std::uint64_t count = 0;
};

struct compiled_timeline_entry
{
    microseconds at{0};
    std::vector<compiled_action> actions;
};

/// A rule set that passed every check.  Never changed once made.
struct compiled_rule_set
{
    std::string name;
    attachment attach;
    std::optional<std::uint64_t> seed;
    microseconds late_after{0};
    ordering order = ordering::fifo;
    std::vector<std::string> states;
    std::vector<std::string> slot_names;
    std::vector<byte_buffer> slot_values;
    std::vector<std::string> signals;
    std::vector<compiled_rule> rules;
    std::vector<compiled_timeline_entry> timeline;

    // -- bounds, known before the set runs ---------------------------------

    /// The longest pattern, in bytes.
    std::size_t longest_pattern = 0;
    /// The longest any action can be due after its edge (not counting the
    /// `run_start` edge, whose times are absolute).
    microseconds latest_action{0};
    /// The longest gap any rule waits for after its pattern.
    microseconds longest_gap{0};
    /// The actions that asked for `tight` precision.
    std::size_t tight_actions = 0;
    /// What the set costs in memory, in bytes, to within a small factor.
    std::size_t size_bytes = 0;
};

/// Checks `source` and compiles it.  On failure the error is
/// `errc::config_error`, and `problems`, when given, lists everything that
/// is wrong (not only the first), each with the path of the value.
result<std::shared_ptr<const compiled_rule_set>> compile(
    const rule_set_source& source, std::vector<problem>* problems = nullptr);

} // namespace vss::rules
