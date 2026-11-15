//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages of a decoder: a plugin that reads the traffic of the places it
// was given and says what the bytes mean.  docs/plugins.md, sections 20.4 (the
// decode stream), 20.6 (the messages) and 20.7 (the trigger).
//
// The host sends the decode stream in `decoder.data`: a list of items in the
// order the engine saw them, with the bytes of all `data` items as one blob.
// The plugin acknowledges with `decoder.ack`.  It reports what it found in
// `decoder.frame` (for the traffic view) and `trigger.publish` (for the
// agent).  A decoder never changes the traffic.
//
// A trigger carries no bytes and no id: the host stamps `id`, `from` and the
// time of publishing, and a member of those names in a `trigger.publish` is
// passed over as an unknown one.
//

#pragma once

#include <vss/plugin/api_message.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vss::plugin
{

/// The most places one decoder reads.
inline constexpr std::size_t max_decoder_places = 64;
/// The fastest line rate of a place, in bits per second.
inline constexpr std::uint64_t max_place_rate = 100'000'000;
/// The longest format of a place ("8N1"), in bytes.
inline constexpr std::size_t max_place_format = 16;
/// The most items in one `decoder.data`.
inline constexpr std::size_t max_decoder_items = 128;
/// The number of entries in the `covers` of a trigger: the first byte and the
/// last.
inline constexpr std::size_t trigger_covers_count = 2;
/// The most bytes in the blob of one `decoder.data`: what the host leaves
/// unacknowledged (section 20.4).
inline constexpr std::size_t max_decoder_blob = max_api_blob;
/// The most bytes of one chunk, of one frame, and of a field.
inline constexpr std::uint64_t max_decoder_chunk = 65'536;
/// The most fields of one `decoder.frame`.
inline constexpr std::size_t max_decoder_fields = 64;
/// The longest message name, rule, action, field name and custom code, in
/// bytes.
inline constexpr std::size_t max_decoder_name = 64;
/// The most lines a `line` item can name.
inline constexpr std::size_t max_decoder_lines = 6;
/// The longest id of a protocol, in bytes.
inline constexpr std::size_t max_protocol_id = 128;
/// The longest version of a protocol, in bytes.
inline constexpr std::size_t max_protocol_version = 64;

/// Which way a chunk went at a place.
enum class decoder_direction
{
    rx,
    tx
};

/// A control line.  The design does not list them (section 20.4, the `line`
/// item); these are the six of a serial port.
enum class decoder_line
{
    dtr,
    rts,
    cts,
    dsr,
    dcd,
    ri
};

/// What a line error is.
enum class decoder_line_error
{
    parity,
    framing,
    overrun,
    line_break
};

/// The verdict of a half-duplex bus.
enum class decoder_fault
{
    overlap,
    too_soon,
    line_held,
    transmitter_off,
    overrun
};

/// The kind of a trigger (section 20.7).
enum class trigger_kind
{
    framing,
    unknown_type,
    unknown_value,
    check,
    length,
    timeout,
    sequence,
    contention,
    line_error,
    decoder_lost,
    custom
};

/// A place the decoder reads.
struct decoder_place
{
    /// The name of the place.  1 to `max_api_name` bytes.
    std::string place;
    /// The line rate in bits per second, 1 to `max_place_rate`.  0 is not
    /// given.
    std::uint64_t rate = 0;
    /// The format, such as "8N1".  1 to `max_place_format` bytes.  Empty is
    /// not given.
    std::string format;
    /// Whether the place is a half-duplex bus.
    bool bus = false;

    friend bool operator==(const decoder_place&,
                           const decoder_place&) = default;
};

// The items of the decode stream (section 20.4).  Each has the member `item`
// in the header, which names its kind.

/// A chunk of bytes.  They are in the blob of the message, after those of the
/// items before it.
struct decoder_data_item
{
    std::string place;
    decoder_direction direction = decoder_direction::rx;
    /// The count of bytes at this place and direction before this chunk.
    std::uint64_t offset = 0;
    std::uint64_t t_ns = 0;
    /// On a half-duplex bus: when the last byte left the wire.  Not before
    /// `t_ns`.
    std::optional<std::uint64_t> end_ns;
    /// 1 to `max_decoder_chunk`.
    std::uint64_t length = 0;

    friend bool operator==(const decoder_data_item&,
                           const decoder_data_item&) = default;
};

/// What the control lines are, and which of them changed.
struct decoder_line_item
{
    std::string place;
    std::uint64_t t_ns = 0;
    /// The state of each line, in the order of `decoder_line`.  A line that
    /// has no value is not told.
    std::array<std::optional<bool>, max_decoder_lines> lines;
    /// The lines that changed, each with a value in `lines`, none twice.
    std::vector<decoder_line> changed;

    friend bool operator==(const decoder_line_item&,
                           const decoder_line_item&) = default;
};

struct decoder_line_error_item
{
    std::string place;
    std::uint64_t t_ns = 0;
    decoder_line_error kind = decoder_line_error::parity;

    friend bool operator==(const decoder_line_error_item&,
                           const decoder_line_error_item&) = default;
};

/// The verdict of a half-duplex bus, with the sender's chunk.
struct decoder_bus_fault_item
{
    /// The name the hub gives the link the bus is on.
    std::string place;
    std::string sender;
    std::uint64_t t_ns = 0;
    decoder_fault fault = decoder_fault::overlap;
    /// The id of the event that `trigger.publish` names in `cause.event`.
    std::uint64_t event = 0;
    std::uint64_t offset = 0;
    /// 1 to `max_decoder_chunk`.
    std::uint64_t length = 0;

    friend bool operator==(const decoder_bus_fault_item&,
                           const decoder_bus_fault_item&) = default;
};

/// A rule fired at the place.
struct decoder_rule_item
{
    std::string place;
    std::uint64_t t_ns = 0;
    /// 1 to `max_decoder_name` bytes.  Plain text.
    std::string rule;
    /// 1 to `max_decoder_name` bytes.  Plain text.
    std::string action;

    friend bool operator==(const decoder_rule_item&,
                           const decoder_rule_item&) = default;
};

/// The engine's time now.
struct decoder_time_item
{
    std::uint64_t t_ns = 0;

    friend bool operator==(const decoder_time_item&,
                           const decoder_time_item&) = default;
};

/// The decoder was too slow, and this much was not sent.
struct decoder_lost_item
{
    std::string place;
    decoder_direction direction = decoder_direction::rx;
    std::uint64_t from_offset = 0;
    /// 1 to `max_api_number`.
    std::uint64_t bytes = 0;
    std::uint64_t from_ns = 0;
    /// Not before `from_ns`.
    std::uint64_t to_ns = 0;

    friend bool operator==(const decoder_lost_item&,
                           const decoder_lost_item&) = default;
};

using decoder_item =
    std::variant<decoder_data_item, decoder_line_item, decoder_line_error_item,
                 decoder_bus_fault_item, decoder_rule_item, decoder_time_item,
                 decoder_lost_item>;

/// Creates a decoder on its places.  The plugin answers "ok".
struct decoder_create_message
{
    static constexpr std::string_view op() noexcept { return "decoder.create"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;
    /// 1 to `max_decoder_places` places, no place twice.
    std::vector<decoder_place> places;
    /// The parameters as one JSON object, already checked.  "{}" for none.
    std::string params = "{}";

    friend bool operator==(const decoder_create_message&,
                           const decoder_create_message&) = default;
};

/// Some of the decode stream.  The bytes of all `data` items are the blob, in
/// the order of the items, and the lengths of the `data` items add up to its
/// size.  The message names no instance (section 20.6 gives none).
struct decoder_data_message
{
    static constexpr std::string_view op() noexcept { return "decoder.data"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    /// 1 to `max_api_number`.
    std::uint64_t instance = 0;
    /// 1 to `max_api_number`.
    std::uint64_t seq = 0;
    /// 1 to `max_decoder_items` items.
    std::vector<decoder_item> items;
    /// 0 to `max_decoder_blob` bytes.  They travel in the blob.
    byte_buffer data;

    friend bool operator==(const decoder_data_message&,
                           const decoder_data_message&) = default;
};

/// The plugin has taken the stream up to this `seq`.
struct decoder_ack_message
{
    static constexpr std::string_view op() noexcept { return "decoder.ack"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    /// 1 to `max_api_number`.
    std::uint64_t instance = 0;
    /// 1 to `max_api_number`.
    std::uint64_t seq = 0;

    friend bool operator==(const decoder_ack_message&,
                           const decoder_ack_message&) = default;
};

/// A part of a frame.
struct decoder_field
{
    /// 1 to `max_decoder_name` bytes.  Plain text.
    std::string name;
    /// From the start of the frame.
    std::uint64_t offset = 0;
    /// 0 to `max_decoder_chunk`.  The field lies within the frame.
    std::uint64_t length = 0;
    /// The value as the plugin shows it.  At most `max_api_text` bytes.
    /// Plain text.
    std::string value;

    friend bool operator==(const decoder_field&,
                           const decoder_field&) = default;
};

/// A decoded frame, for the traffic view.
struct decoder_frame_message
{
    static constexpr std::string_view op() noexcept { return "decoder.frame"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    std::string place;
    decoder_direction direction = decoder_direction::rx;
    /// The count of bytes at this place and direction before the frame.
    std::uint64_t offset = 0;
    /// 1 to `max_decoder_chunk`.
    std::uint64_t length = 0;
    std::uint64_t t_start_ns = 0;
    /// Not before `t_start_ns`.
    std::uint64_t t_end_ns = 0;
    /// The name of the message.  1 to `max_decoder_name` bytes.  Plain text.
    std::string message;
    /// At most `max_api_text` bytes.  Plain text.  Empty is left out.
    std::string summary;
    /// 0 to `max_decoder_fields` fields.
    std::vector<decoder_field> fields;

    friend bool operator==(const decoder_frame_message&,
                           const decoder_frame_message&) = default;
};

/// The protocol that a trigger was found with.
struct trigger_protocol
{
    /// 1 to `max_protocol_id` bytes.  Plain text.
    std::string id;
    /// 1 to `max_protocol_version` bytes.  Plain text.
    std::string version;

    friend bool operator==(const trigger_protocol&,
                           const trigger_protocol&) = default;
};

/// The frame that a trigger is about.
struct trigger_frame
{
    std::uint64_t offset = 0;
    /// 1 to `max_decoder_chunk`.
    std::uint64_t length = 0;
    std::uint64_t t_start_ns = 0;
    /// Not before `t_start_ns`.
    std::uint64_t t_end_ns = 0;
    /// 0 to `max_decoder_name` bytes.  Plain text.
    std::string message;

    friend bool operator==(const trigger_frame&,
                           const trigger_frame&) = default;
};

/// The field that a trigger is about, from the start of the frame.
struct trigger_field
{
    /// 1 to `max_decoder_name` bytes.  Plain text.
    std::string name;
    /// From the start of the frame.
    std::uint64_t offset = 0;
    /// 0 to `max_decoder_chunk`, and `offset` plus `length` is not past the
    /// frame's length.
    std::uint64_t length = 0;

    friend bool operator==(const trigger_field&,
                           const trigger_field&) = default;
};

/// The bytes that a check value covers, first to last, from the start of the
/// frame.  The last is not before the first.
struct trigger_covers
{
    std::uint64_t first = 0;
    std::uint64_t last = 0;

    friend bool operator==(const trigger_covers&,
                           const trigger_covers&) = default;
};

/// The cause is a bus fault that covers the same bytes.
struct trigger_bus_cause
{
    decoder_fault fault = decoder_fault::overlap;
    /// The id of the event that reported the fault.
    std::uint64_t event = 0;

    friend bool operator==(const trigger_bus_cause&,
                           const trigger_bus_cause&) = default;
};

/// The cause is a fault that this instance asked for.
struct trigger_own_action
{
    /// The id of the action.
    std::uint64_t action = 0;

    friend bool operator==(const trigger_own_action&,
                           const trigger_own_action&) = default;
};

using trigger_cause = std::variant<trigger_bus_cause, trigger_own_action>;

/// The request that a reply or a timeout is about.
struct trigger_request
{
    std::uint64_t offset = 0;
    /// 1 to `max_decoder_chunk`.
    std::uint64_t length = 0;
    std::uint64_t t_end_ns = 0;

    friend bool operator==(const trigger_request&,
                           const trigger_request&) = default;
};

/// Publishes a trigger (section 20.7).  The host stamps its id, its sender and
/// its time.  No bytes are in it.
struct trigger_publish_message
{
    static constexpr std::string_view op() noexcept
    {
        return "trigger.publish";
    }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::notification; }

    std::uint64_t instance = 0;
    trigger_kind kind = trigger_kind::framing;
    /// The code that the manifest declared.  1 to `max_decoder_name` lower-case
    /// letters, digits, dots, hyphens and underscores.  Given when `kind` is
    /// `custom` and not otherwise.
    std::string code;
    std::uint64_t when_ns = 0;
    std::string place;
    decoder_direction direction = decoder_direction::rx;
    /// The name of the sender.  Empty is left out.
    std::string sender;
    std::optional<trigger_protocol> protocol;
    std::optional<trigger_frame> frame;
    std::optional<trigger_field> field;
    /// A check value as hex, 1 to `max_api_name` bytes.  Both or neither.
    std::string expected;
    std::string actual;
    std::optional<trigger_covers> covers;
    std::optional<trigger_cause> cause;
    std::optional<trigger_request> request;
    /// 1 to `max_api_text` bytes.  Plain text.
    std::string text;

    friend bool operator==(const trigger_publish_message&,
                           const trigger_publish_message&) = default;
};

/// Stops a decoder.  The plugin answers "ok".
struct decoder_stop_message
{
    static constexpr std::string_view op() noexcept { return "decoder.stop"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;

    friend bool operator==(const decoder_stop_message&,
                           const decoder_stop_message&) = default;
};

template <>
result<decoder_create_message>
decode_as<decoder_create_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<decoder_create_message>(const decoder_create_message&, std::uint64_t);
template <>
result<decoder_data_message> decode_as<decoder_data_message>(const api_message&,
                                                             std::string*);
template <>
result<api_message> encode_as<decoder_data_message>(const decoder_data_message&,
                                                    std::uint64_t);
template <>
result<decoder_ack_message> decode_as<decoder_ack_message>(const api_message&,
                                                           std::string*);
template <>
result<api_message> encode_as<decoder_ack_message>(const decoder_ack_message&,
                                                   std::uint64_t);
template <>
result<decoder_frame_message>
decode_as<decoder_frame_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<decoder_frame_message>(const decoder_frame_message&, std::uint64_t);
template <>
result<trigger_publish_message>
decode_as<trigger_publish_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<trigger_publish_message>(const trigger_publish_message&,
                                   std::uint64_t);
template <>
result<decoder_stop_message> decode_as<decoder_stop_message>(const api_message&,
                                                             std::string*);
template <>
result<api_message> encode_as<decoder_stop_message>(const decoder_stop_message&,
                                                    std::uint64_t);

} // namespace vss::plugin
