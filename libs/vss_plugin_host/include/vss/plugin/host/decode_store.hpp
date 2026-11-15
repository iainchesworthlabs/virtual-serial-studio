//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a decoder plugin sent, kept for Studio to read: the decoded frames
// (`decoder.frame`) and the triggers (`trigger.publish`) of one plugin
// instance.  docs/plugins.md, sections 7.9, 10, 11 and 20, are the design.
//
// The host's thread adds items.  Each is checked, given a sequence number and
// kept in arrival order.  The oldest are dropped when the store is over its
// limits, and what was dropped is counted.  Any thread reads an immutable
// snapshot (`shared_ptr<const>`), so a reader waits for a pointer copy and a
// writer waits for a pointer swap, never for more.  Nothing here blocks on a
// plugin, and nothing here touches the GUI: a reader that lives on the GUI
// thread asks to be told, by a callback posted to an executor it names, and
// then reads the snapshot there.
//
// The record types carry the members of the messages named beside each, and
// do not include those headers.  Every text is valid UTF-8 within its limit.
// A control character or a mark that changes the direction of text is kept as
// it came and flagged (`text_marks`), so a view can show it escaped.  Text is
// never changed, and never markup: a view shows it as plain text, quoted.
//

#pragma once

#include <boost/asio/any_io_executor.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin::host
{

/// The largest whole number a plugin message carries: 2^53 - 1.
inline constexpr std::uint64_t max_decode_number =
    (std::uint64_t{1} << 53) - 1;

/// The most items one call of `add_records` or `add_triggers` takes.  The
/// rest of a larger batch is counted as rejected.
inline constexpr std::size_t max_decode_batch = 1024;

/// What the limits of the store are when the caller does not choose.  The
/// trigger numbers are those of section 20.7 (200 a second, the last 10 000).
/// The others are this code's choice: 2000 frames a second is the rate of
/// frames in of section 7.9.
inline constexpr std::size_t default_max_frames = 10'000;
inline constexpr std::size_t default_max_frame_bytes = 8U * 1024 * 1024;
inline constexpr std::size_t default_max_triggers = 10'000;
inline constexpr std::size_t default_max_trigger_bytes = 4U * 1024 * 1024;
inline constexpr std::size_t default_frames_per_second = 2000;
inline constexpr std::size_t default_triggers_per_second = 200;

/// What a view needs to know about a text before it shows it.
struct text_marks
{
    /// A control character: below U+0020, U+007F, U+0080 to U+009F, U+2028 or
    /// U+2029.  A line feed counts.
    bool control = false;
    /// A mark that changes the direction of text: U+061C, U+200E, U+200F,
    /// U+202A to U+202E and U+2066 to U+2069.
    bool bidi = false;

    text_marks& operator|=(const text_marks& other) noexcept
    {
        control = control || other.control;
        bidi = bidi || other.bidi;
        return *this;
    }

    friend bool operator==(const text_marks&, const text_marks&) = default;
};

/// What `check_text` found.
struct text_check
{
    /// Strict UTF-8: no overlong form, no surrogate, nothing past U+10FFFF.
    bool valid_utf8 = true;
    text_marks marks;
};

/// Looks at one text.  Never changes it.
text_check check_text(std::string_view text) noexcept;

/// Why an item was refused.
enum class reject_reason
{
    none,
    /// The batch had more than `max_decode_batch` items; this is one of the
    /// rest.
    batch_too_large,
    /// A text that must not be empty is.
    text_empty,
    /// A text is longer than its limit, in bytes.
    text_too_long,
    /// A text is not valid UTF-8.
    invalid_utf8,
    /// A whole number is over `max_decode_number`, or a length is 0 or over
    /// 65536.
    number_out_of_range,
    /// A list is longer than its limit, or members that go together do not:
    /// a field outside its frame, an end before a start, `expected` without
    /// `actual`, `code` that does not match `kind`.
    bad_combination,
    /// A `code` that is not a token, or a value of an enumeration that is
    /// not one.
    bad_value
};

/// The name of a reason, for a log.
std::string_view to_string(reject_reason reason) noexcept;

enum class decode_direction
{
    rx,
    tx
};

enum class decode_fault
{
    overlap,
    too_soon,
    line_held,
    transmitter_off,
    overrun
};

enum class decode_trigger_kind
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

/// One member of a frame: `decoder.frame`, `fields`.
struct decode_field
{
    std::string name;            // 1 to 64 bytes
    std::uint64_t offset = 0;    // from the start of the frame
    std::uint64_t length = 0;    // 0 to 65536; offset + length <= frame length
    std::string value;           // 0 to 200 bytes

    friend bool operator==(const decode_field&, const decode_field&) = default;
};

/// A decoded frame: the members of `decoder.frame`, less `instance`.
struct decoded_frame
{
    std::string place;                                // 1 to 128 bytes
    decode_direction direction = decode_direction::rx;
    std::uint64_t offset = 0;                         // bytes before it
    std::uint64_t length = 0;                         // 1 to 65536
    std::uint64_t t_start_ns = 0;
    std::uint64_t t_end_ns = 0;                       // not before t_start_ns
    std::string message;                              // 1 to 64 bytes
    std::string summary;                              // 0 to 200 bytes
    std::vector<decode_field> fields;                 // 0 to 64

    friend bool operator==(const decoded_frame&, const decoded_frame&) =
        default;
};

/// `trigger.publish`, `protocol`.
struct decode_protocol
{
    std::string id;       // 1 to 128 bytes
    std::string version;  // 1 to 64 bytes

    friend bool operator==(const decode_protocol&, const decode_protocol&) =
        default;
};

/// `trigger.publish`, `frame`.
struct decode_trigger_frame
{
    std::uint64_t offset = 0;
    std::uint64_t length = 0;      // 1 to 65536
    std::uint64_t t_start_ns = 0;
    std::uint64_t t_end_ns = 0;    // not before t_start_ns
    std::string message;           // 0 to 64 bytes

    friend bool operator==(const decode_trigger_frame&,
                           const decode_trigger_frame&) = default;
};

/// `trigger.publish`, `field`.
struct decode_trigger_field
{
    std::string name;              // 1 to 64 bytes
    std::uint64_t offset = 0;
    std::uint64_t length = 0;      // 1 to 65536

    friend bool operator==(const decode_trigger_field&,
                           const decode_trigger_field&) = default;
};

/// `trigger.publish`, `covers`: the first and last byte, `last >= first`.
struct decode_covers
{
    std::uint64_t first = 0;
    std::uint64_t last = 0;

    friend bool operator==(const decode_covers&, const decode_covers&) =
        default;
};

/// `trigger.publish`, `cause`: exactly one of the two.
struct decode_cause
{
    enum class kind
    {
        bus_fault,
        own_action
    };

    kind which = kind::bus_fault;
    decode_fault fault = decode_fault::overlap;  // when `bus_fault`
    std::uint64_t event = 0;                     // when `bus_fault`
    std::uint64_t own_action = 0;                // when `own_action`

    friend bool operator==(const decode_cause&, const decode_cause&) = default;
};

/// `trigger.publish`, `request`.
struct decode_request
{
    std::uint64_t offset = 0;
    std::uint64_t length = 0;      // 1 to 65536
    std::uint64_t t_end_ns = 0;

    friend bool operator==(const decode_request&, const decode_request&) =
        default;
};

/// A trigger: the members of `trigger.publish`, less `instance`.  The host's
/// own stamp (its count for the run, the plugin it is from, the time it was
/// published) is the store's sequence number and `received_ns`.
struct decoded_trigger
{
    decode_trigger_kind kind = decode_trigger_kind::framing;
    std::string code;              // a token of 1 to 64 bytes, if and only if
                                   // `kind` is `custom`
    std::uint64_t when_ns = 0;
    std::string place;             // 1 to 128 bytes
    decode_direction direction = decode_direction::rx;
    std::string sender;            // 0 to 128 bytes
    std::optional<decode_protocol> protocol;
    std::optional<decode_trigger_frame> frame;
    std::optional<decode_trigger_field> field;
    std::string expected;          // 0 to 128 bytes; both or neither
    std::string actual;
    std::optional<decode_covers> covers;
    std::optional<decode_cause> cause;
    std::optional<decode_request> request;
    std::string text;              // 1 to 200 bytes

    friend bool operator==(const decoded_trigger&, const decoded_trigger&) =
        default;
};

/// An item as the store keeps it.
template <class T>
struct stored_item
{
    /// From 1, in arrival order, and never used twice for one store, even
    /// after `clear`.
    std::uint64_t sequence = 0;
    /// The host's time when it was added, as the caller gave it.
    std::uint64_t received_ns = 0;
    /// Which kinds of mark any of its texts have.
    text_marks marks;
    /// What it costs against the byte limit.
    std::size_t cost = 0;
    T value;
};

using stored_frame = stored_item<decoded_frame>;
using stored_trigger = stored_item<decoded_trigger>;

/// The items the store holds now, oldest first, and the counts that go with
/// them.  Immutable: the store makes a new one for each change.
template <class T>
struct decode_window
{
    std::vector<std::shared_ptr<const stored_item<T>>> items;
    /// The sequence number of `items.front()`, or `next_sequence` when there
    /// are none.
    std::uint64_t first_sequence = 1;
    /// The sequence number the next item will get.
    std::uint64_t next_sequence = 1;
    /// The bytes the items cost together.
    std::size_t bytes = 0;
    /// Since the store was made or last cleared: the oldest items that were
    /// dropped to stay within the limits, the items refused, and the items
    /// that were over the per-second budget and cut.
    std::uint64_t dropped = 0;
    std::uint64_t rejected = 0;
    std::uint64_t over_budget = 0;
    /// The reason for the latest refusal.
    reject_reason last_reject = reject_reason::none;
};

/// Both windows, taken together.
struct decode_snapshot
{
    std::shared_ptr<const decode_window<decoded_frame>> frames;
    std::shared_ptr<const decode_window<decoded_trigger>> triggers;
};

/// What `records_since` and `triggers_since` give back.
template <class T>
struct decode_since
{
    /// The items after the sequence number that are still held.
    std::vector<std::shared_ptr<const stored_item<T>>> items;
    /// True when items after the sequence number were dropped (or cleared),
    /// so what is in `items` does not follow what the reader had.
    bool gap = false;
    /// The sequence number of the newest item ever added, 0 when none.
    std::uint64_t last_sequence = 0;
};

struct decode_limits
{
    std::size_t max_frames = default_max_frames;
    std::size_t max_frame_bytes = default_max_frame_bytes;
    std::size_t max_triggers = default_max_triggers;
    std::size_t max_trigger_bytes = default_max_trigger_bytes;
    /// Items taken in one second; the rest of a batch is cut and counted.
    std::size_t frames_per_second = default_frames_per_second;
    std::size_t triggers_per_second = default_triggers_per_second;
};

/// What one call of `add_records` or `add_triggers` did.
struct add_result
{
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t over_budget = 0;
    /// Older items dropped by this call, to make room.
    std::size_t dropped = 0;
    /// The first refusal of the call, with its place in the batch.
    reject_reason first_reject = reject_reason::none;
    std::size_t first_reject_index = 0;
};

/// The store of one plugin instance.  Any thread may call any function, one
/// writer at a time is the intended use (the host's strand for the instance);
/// two writers are safe and take turns.
class decode_store
{
public:
    explicit decode_store(decode_limits limits = {});
    ~decode_store();
    decode_store(const decode_store&) = delete;
    decode_store& operator=(const decode_store&) = delete;

    /// Adds frames.  `now_ns` is the host's time, and is what the per-second
    /// budget counts by: the budget is for the whole second `now_ns` is in.
    /// Items over it are cut off the end of the batch and counted.
    add_result add_records(std::span<const decoded_frame> batch,
                           std::uint64_t now_ns);
    add_result add_triggers(std::span<const decoded_trigger> batch,
                            std::uint64_t now_ns);

    /// The current windows.  Never null.  Never changes once given out.
    std::shared_ptr<const decode_snapshot> snapshot() const;

    /// What arrived after `sequence` (0 for everything held).
    decode_since<decoded_frame> records_since(std::uint64_t sequence) const;
    decode_since<decoded_trigger>
    triggers_since(std::uint64_t sequence) const;

    /// Empties both windows and their counts.  Sequence numbers go on from
    /// where they were, so a reader's `since` shows a gap.
    void clear();

    /// Asks to be told when something changed.  `notify` is posted to
    /// `executor` and runs there.  At most one is pending: changes that come
    /// before it runs are told by that one call, so the reader reads the
    /// snapshot and finds all of them.  Replaces an earlier request; an
    /// empty `notify` stops them.  `notify` must stay valid for as long as
    /// the executor may run it, which can be after the store is gone: it
    /// then is not called for changes made later, but one already running is
    /// not waited for.
    void set_notify(boost::asio::any_io_executor executor,
                    std::function<void()> notify);

private:
    struct state;
    struct notice;

    void publish(std::shared_ptr<const decode_snapshot> next);
    void tell();

    const decode_limits limits_;
    std::shared_ptr<notice> notice_;
    mutable std::mutex write_mutex_;
    mutable std::mutex publish_mutex_;
    std::shared_ptr<const decode_snapshot> current_;
    std::unique_ptr<state> state_;
};

} // namespace vss::plugin::host
