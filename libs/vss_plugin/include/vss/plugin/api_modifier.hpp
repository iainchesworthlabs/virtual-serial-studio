//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages of a behaviour modifier: a plugin that sits in the path of one
// direction of a link and changes the bytes.  docs/plugins.md, sections 7.6
// and 7.9.
//
// The host collects the chunks the route stage hands it into a batch
// (`modifier.batch`: the bytes of all chunks are one blob) and the plugin
// answers with one action for each chunk (`modifier.result`).  At most one
// batch is outstanding for an instance.  Both are notifications, matched by
// `seq`; both carry `instance`, since a plugin may have many.
//
// The actions are a closed set, so that the host can bound what comes back
// (`modifier_result_within_bounds`).
//

#pragma once

#include <vss/plugin/api_fixture.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/manifest.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin
{

/// The most a `budget_us` or a `max_batch_us` may be: one second.
inline constexpr std::uint64_t max_modifier_budget_us = 1'000'000;
/// The most bytes a batch may have, and so the most `max_batch_bytes` is.
inline constexpr std::size_t max_modifier_batch = std::size_t{64} * 1024;
/// The most `max_batch_us`.
inline constexpr std::uint64_t max_modifier_batch_us = 1'000'000;
/// The most chunks in a batch, and so the most actions in a result.
inline constexpr std::size_t max_modifier_chunks = 256;
/// The most parts a `split` has.
inline constexpr std::uint64_t max_modifier_parts = 16;
/// How much more than four times its batch a result may hold (section 7.6).
inline constexpr std::size_t modifier_result_extra = 1024;
/// The largest `flush_on`: one byte value.
inline constexpr std::uint64_t max_flush_on = 255;
/// The most bytes of the blob of one `modifier.result`: four times the largest
/// batch and `modifier_result_extra` (section 7.6), 263,168 bytes.  It is more
/// than `max_api_blob`, which is the limit of every other message.
inline constexpr std::size_t max_modifier_result_blob =
    4 * max_modifier_batch + modifier_result_extra;

/// Which way of a link a modifier works on.
enum class modifier_direction
{
    a_to_b,
    b_to_a
};

/// "a_to_b" or "b_to_a".
std::string_view to_string(modifier_direction value) noexcept;
/// "fail_open" or "fail_closed".
std::string_view to_string(timeout_action value) noexcept;

/// Makes a modifier on one direction of a link.  The plugin answers "ok".
struct modifier_create_message
{
    static constexpr std::string_view op() noexcept
    {
        return "modifier.create";
    }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;
    /// 1 to `max_api_number`.
    std::string place;
    modifier_direction direction = modifier_direction::a_to_b;
    /// The parameters as one JSON object, already checked.  "{}" for none.
    std::string params = "{}";
    /// What the engine waits for one result: 1 to `max_modifier_budget_us`.
    std::uint64_t budget_us = 0;
    /// A batch is sent at this many bytes: 1 to `max_modifier_batch`.
    std::uint64_t max_batch_bytes = 0;
    /// A batch is sent this long after its first chunk: 1 to
    /// `max_modifier_batch_us`.
    std::uint64_t max_batch_us = 0;
    /// What the bytes of a batch do when the budget passes.
    timeout_action on_timeout = timeout_action::fail_open;
    /// A byte that sends the batch at once, 0 to 255.  Absent is none.
    std::optional<std::uint8_t> flush_on;

    friend bool operator==(const modifier_create_message&,
                           const modifier_create_message&) = default;
};

/// One chunk of a batch: a range of the blob and the time it was read.
struct modifier_chunk
{
    /// 0 to `max_modifier_batch`.
    std::uint64_t offset = 0;
    /// 1 to `max_modifier_batch`.
    std::uint64_t length = 0;
    /// 0 to `max_api_number`.
    std::uint64_t ts_ns = 0;

    friend bool operator==(const modifier_chunk&,
                           const modifier_chunk&) = default;
};

/// The chunks of one direction that the stage handed over.  The bytes of all
/// of them are the blob.
struct modifier_batch_message
{
    static constexpr std::string_view op() noexcept { return "modifier.batch"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    /// The number the result carries back: 1 to `max_api_number`.
    std::uint64_t seq = 0;
    /// The time of the first chunk, which is `chunks[0].ts_ns`.
    std::uint64_t ts_ns = 0;
    /// 1 to `max_modifier_chunks` entries.  Every chunk lies inside `data`,
    /// they are in order and do not overlap, and their times do not go back.
    std::vector<modifier_chunk> chunks;
    /// 1 to `max_modifier_batch` bytes, in the blob.
    byte_buffer data;

    friend bool operator==(const modifier_batch_message&,
                           const modifier_batch_message&) = default;
};

/// What the plugin does with one chunk.
enum class modifier_action_kind
{
    /// Let the chunk through as it is.
    pass,
    /// Throw the chunk away.
    drop,
    /// Send the range `offset`, `length` of the result blob in its place.
    replace,
    /// The engine holds the chunk for `us` microseconds.
    delay_us,
    /// The engine sends the chunk in `parts` parts, `gap_us` apart.
    split,
    /// Send the range of the result blob before the chunk.
    insert_before,
    /// Send the range of the result blob after the chunk.
    insert_after,
    /// Raise `fault` at the time the chunk passes.
    fault
};

/// "pass", "drop", "replace", "delay_us", "split", "insert_before",
/// "insert_after" or "fault".
std::string_view to_string(modifier_action_kind value) noexcept;

/// One action.  Only the members of its kind are used and written; the rest
/// stay 0 (and `fault` at `parity`).
struct modifier_action
{
    modifier_action_kind kind = modifier_action_kind::pass;
    /// `replace`, `insert_before`, `insert_after`: a range of the result
    /// blob.  `length` is 0 or more for `replace` and 1 or more for the
    /// others.  Both are at most `max_modifier_result_blob`.
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    /// `delay_us`: 1 to `max_api_delay_us`.
    std::uint64_t us = 0;
    /// `split`: 2 to `max_modifier_parts`.
    std::uint64_t parts = 0;
    /// `split`: 0 to `max_api_delay_us`.
    std::uint64_t gap_us = 0;
    /// `fault`.
    line_fault fault = line_fault::parity;

    friend bool operator==(const modifier_action&,
                           const modifier_action&) = default;
};

/// The plugin's answer to a batch.
struct modifier_result_message
{
    static constexpr std::string_view op() noexcept
    {
        return "modifier.result";
    }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    /// The `seq` of the batch this answers.
    std::uint64_t seq = 0;
    /// 1 to `max_modifier_chunks` entries: one for each chunk, in order.
    std::vector<modifier_action> actions;
    /// 0 to `max_modifier_result_blob` bytes, in the blob: what the actions
    /// refer to.
    byte_buffer data;

    friend bool operator==(const modifier_result_message&,
                           const modifier_result_message&) = default;
};

/// Stops a modifier.  The plugin answers "ok".
struct modifier_stop_message
{
    static constexpr std::string_view op() noexcept { return "modifier.stop"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;

    friend bool operator==(const modifier_stop_message&,
                           const modifier_stop_message&) = default;
};

/// The host's bounds of section 7.6, for a result to a batch of
/// `batch_bytes`: what the actions can send on (the batch itself and the
/// length of every `replace` and insert, whether or not their ranges of the
/// result blob are the same) and the result blob are each at most four times
/// the batch plus `modifier_result_extra` bytes, and the delays (`delay_us`,
/// and the gaps of a `split`, which has `parts` less one of them) add up to at
/// most `max_api_delay_us`.  A result that is not within them fails the whole
/// batch.
bool modifier_result_within_bounds(
    std::size_t batch_bytes, const modifier_result_message& result) noexcept;

template <>
result<modifier_create_message>
decode_as<modifier_create_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<modifier_create_message>(const modifier_create_message&,
                                   std::uint64_t);
template <>
result<modifier_batch_message>
decode_as<modifier_batch_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<modifier_batch_message>(const modifier_batch_message&, std::uint64_t);
template <>
result<modifier_result_message>
decode_as<modifier_result_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<modifier_result_message>(const modifier_result_message&,
                                   std::uint64_t);
template <>
result<modifier_stop_message>
decode_as<modifier_stop_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<modifier_stop_message>(const modifier_stop_message&, std::uint64_t);

} // namespace vss::plugin
