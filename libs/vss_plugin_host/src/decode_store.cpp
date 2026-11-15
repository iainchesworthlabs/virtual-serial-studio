//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/decode_store.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <utility>

namespace vss::plugin::host
{

namespace
{

constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
constexpr std::uint64_t max_chunk = 65'536;
constexpr std::size_t max_place = 128;
constexpr std::size_t max_label = 64;
constexpr std::size_t max_value = 200;
constexpr std::size_t max_check = 128;
constexpr std::size_t max_protocol_id_bytes = 128;
constexpr std::size_t max_protocol_version_bytes = 64;
constexpr std::size_t max_frame_fields = 64;

/// The code point at `at`, and how many bytes it takes.  Zero bytes for a
/// sequence that is not strict UTF-8.
std::size_t decode_one(std::string_view text, std::size_t at,
                       char32_t& code) noexcept
{
    const auto byte = [&](std::size_t index) -> unsigned {
        return static_cast<unsigned char>(text[index]);
    };
    const unsigned lead = byte(at);
    if (lead < 0x80U)
    {
        code = lead;
        return 1;
    }

    std::size_t length = 0;
    char32_t value = 0;
    char32_t least = 0;
    if (lead >= 0xC2U && lead <= 0xDFU)
    {
        length = 2;
        value = lead & 0x1FU;
        least = 0x80;
    }
    else if (lead >= 0xE0U && lead <= 0xEFU)
    {
        length = 3;
        value = lead & 0x0FU;
        least = 0x800;
    }
    else if (lead >= 0xF0U && lead <= 0xF4U)
    {
        length = 4;
        value = lead & 0x07U;
        least = 0x10000;
    }
    else
    {
        return 0;
    }

    if (text.size() - at < length)
    {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i)
    {
        const unsigned next = byte(at + i);
        if ((next & 0xC0U) != 0x80U)
        {
            return 0;
        }
        value = (value << 6) | (next & 0x3FU);
    }
    if (value < least || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF))
    {
        return 0;
    }
    code = value;
    return length;
}

bool is_control(char32_t code) noexcept
{
    return code < 0x20 || code == 0x7F || (code >= 0x80 && code <= 0x9F) ||
           code == 0x2028 || code == 0x2029;
}

bool is_bidi(char32_t code) noexcept
{
    return code == 0x061C || code == 0x200E || code == 0x200F ||
           (code >= 0x202A && code <= 0x202E) ||
           (code >= 0x2066 && code <= 0x2069);
}

/// A text of `least` to `most` bytes, valid UTF-8.  Adds its marks to
/// `marks`.
reject_reason text_ok(std::string_view text, std::size_t least,
                      std::size_t most, text_marks& marks) noexcept
{
    if (text.size() < least)
    {
        return reject_reason::text_empty;
    }
    if (text.size() > most)
    {
        return reject_reason::text_too_long;
    }
    const text_check found = check_text(text);
    if (!found.valid_utf8)
    {
        return reject_reason::invalid_utf8;
    }
    marks |= found.marks;
    return reject_reason::none;
}

bool number_ok(std::uint64_t value) noexcept
{
    return value <= max_decode_number;
}

bool length_ok(std::uint64_t length) noexcept
{
    return length >= 1 && length <= max_chunk;
}

/// A start and a length that are whole numbers within the limit, and whose
/// end is too.
bool span_ok(std::uint64_t offset, std::uint64_t length) noexcept
{
    return number_ok(offset) && number_ok(length) &&
           length <= max_decode_number - offset;
}

bool is_token(std::string_view text) noexcept
{
    if (text.empty() || text.size() > max_label)
    {
        return false;
    }
    return std::ranges::all_of(text, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '-' || c == '_';
    });
}

/// Keeps the first reason given to it, so a run of checks reads in order.
struct first_failure
{
    reject_reason reason = reject_reason::none;

    void operator()(reject_reason next) noexcept
    {
        if (reason == reject_reason::none)
        {
            reason = next;
        }
    }
};

reject_reason validate(const decoded_frame& frame, text_marks& marks)
{
    first_failure fail;
    fail(text_ok(frame.place, 1, max_place, marks));
    fail(text_ok(frame.message, 1, max_label, marks));
    fail(text_ok(frame.summary, 0, max_value, marks));
    if (fail.reason != reject_reason::none)
    {
        return fail.reason;
    }
    if (frame.direction != decode_direction::rx &&
        frame.direction != decode_direction::tx)
    {
        return reject_reason::bad_value;
    }
    if (!length_ok(frame.length) || !span_ok(frame.offset, frame.length) ||
        !number_ok(frame.t_start_ns) || !number_ok(frame.t_end_ns))
    {
        return reject_reason::number_out_of_range;
    }
    if (frame.t_end_ns < frame.t_start_ns ||
        frame.fields.size() > max_frame_fields)
    {
        return reject_reason::bad_combination;
    }
    for (const decode_field& field : frame.fields)
    {
        fail(text_ok(field.name, 1, max_label, marks));
        fail(text_ok(field.value, 0, max_value, marks));
        if (fail.reason != reject_reason::none)
        {
            return fail.reason;
        }
        if (field.length > max_chunk || field.offset > max_chunk)
        {
            return reject_reason::number_out_of_range;
        }
        if (field.offset + field.length > frame.length)
        {
            return reject_reason::bad_combination;
        }
    }
    return reject_reason::none;
}

reject_reason validate(const decoded_trigger& trigger, text_marks& marks)
{
    if (trigger.kind < decode_trigger_kind::framing ||
        trigger.kind > decode_trigger_kind::custom ||
        (trigger.direction != decode_direction::rx &&
         trigger.direction != decode_direction::tx))
    {
        return reject_reason::bad_value;
    }
    const bool custom = trigger.kind == decode_trigger_kind::custom;
    if (custom != !trigger.code.empty())
    {
        return reject_reason::bad_combination;
    }
    if (custom && !is_token(trigger.code))
    {
        return reject_reason::bad_value;
    }
    if (!number_ok(trigger.when_ns))
    {
        return reject_reason::number_out_of_range;
    }
    first_failure fail;
    fail(text_ok(trigger.place, 1, max_place, marks));
    fail(text_ok(trigger.sender, 0, max_place, marks));
    fail(text_ok(trigger.text, 1, max_value, marks));
    fail(text_ok(trigger.expected, 0, max_check, marks));
    fail(text_ok(trigger.actual, 0, max_check, marks));
    if (trigger.protocol)
    {
        fail(text_ok(trigger.protocol->id, 1, max_protocol_id_bytes, marks));
        fail(text_ok(trigger.protocol->version, 1,
                     max_protocol_version_bytes, marks));
    }
    if (trigger.frame)
    {
        fail(text_ok(trigger.frame->message, 0, max_label, marks));
    }
    if (trigger.field)
    {
        fail(text_ok(trigger.field->name, 1, max_label, marks));
    }
    if (fail.reason != reject_reason::none)
    {
        return fail.reason;
    }
    if (trigger.expected.empty() != trigger.actual.empty())
    {
        return reject_reason::bad_combination;
    }
    if (trigger.frame)
    {
        const decode_trigger_frame& frame = *trigger.frame;
        if (!length_ok(frame.length) || !span_ok(frame.offset, frame.length) ||
            !number_ok(frame.t_start_ns) || !number_ok(frame.t_end_ns))
        {
            return reject_reason::number_out_of_range;
        }
        if (frame.t_end_ns < frame.t_start_ns)
        {
            return reject_reason::bad_combination;
        }
    }
    if (trigger.field)
    {
        const decode_trigger_field& field = *trigger.field;
        if (!length_ok(field.length) || !span_ok(field.offset, field.length))
        {
            return reject_reason::number_out_of_range;
        }
    }
    if (trigger.covers)
    {
        if (!number_ok(trigger.covers->first) ||
            !number_ok(trigger.covers->last))
        {
            return reject_reason::number_out_of_range;
        }
        if (trigger.covers->last < trigger.covers->first)
        {
            return reject_reason::bad_combination;
        }
    }
    if (trigger.cause)
    {
        const decode_cause& cause = *trigger.cause;
        if (cause.which == decode_cause::kind::bus_fault)
        {
            if (cause.fault < decode_fault::overlap ||
                cause.fault > decode_fault::overrun)
            {
                return reject_reason::bad_value;
            }
            if (!number_ok(cause.event))
            {
                return reject_reason::number_out_of_range;
            }
        }
        else if (cause.which == decode_cause::kind::own_action)
        {
            if (!number_ok(cause.own_action))
            {
                return reject_reason::number_out_of_range;
            }
        }
        else
        {
            return reject_reason::bad_value;
        }
    }
    if (trigger.request)
    {
        const decode_request& request = *trigger.request;
        if (!length_ok(request.length) ||
            !span_ok(request.offset, request.length) ||
            !number_ok(request.t_end_ns))
        {
            return reject_reason::number_out_of_range;
        }
    }
    return reject_reason::none;
}

std::size_t cost_of(const decoded_frame& frame) noexcept
{
    std::size_t cost = sizeof(stored_frame) + frame.place.size() +
                       frame.message.size() + frame.summary.size();
    for (const decode_field& field : frame.fields)
    {
        cost += sizeof(decode_field) + field.name.size() + field.value.size();
    }
    return cost;
}

std::size_t cost_of(const decoded_trigger& trigger) noexcept
{
    std::size_t cost = sizeof(stored_trigger) + trigger.code.size() +
                       trigger.place.size() + trigger.sender.size() +
                       trigger.expected.size() + trigger.actual.size() +
                       trigger.text.size();
    if (trigger.protocol)
    {
        cost += trigger.protocol->id.size() + trigger.protocol->version.size();
    }
    if (trigger.frame)
    {
        cost += trigger.frame->message.size();
    }
    if (trigger.field)
    {
        cost += trigger.field->name.size();
    }
    return cost;
}

} // namespace

text_check check_text(std::string_view text) noexcept
{
    text_check found;
    std::size_t at = 0;
    while (at < text.size())
    {
        char32_t code = 0;
        const std::size_t taken = decode_one(text, at, code);
        if (taken == 0)
        {
            found.valid_utf8 = false;
            found.marks = {};
            return found;
        }
        found.marks.control = found.marks.control || is_control(code);
        found.marks.bidi = found.marks.bidi || is_bidi(code);
        at += taken;
    }
    return found;
}

std::string_view to_string(reject_reason reason) noexcept
{
    switch (reason)
    {
    case reject_reason::none:
        return "none";
    case reject_reason::batch_too_large:
        return "batch_too_large";
    case reject_reason::text_empty:
        return "text_empty";
    case reject_reason::text_too_long:
        return "text_too_long";
    case reject_reason::invalid_utf8:
        return "invalid_utf8";
    case reject_reason::number_out_of_range:
        return "number_out_of_range";
    case reject_reason::bad_combination:
        return "bad_combination";
    case reject_reason::bad_value:
        return "bad_value";
    }
    return "unknown";
}

namespace
{

/// The count of items taken in the second `second`.
struct budget
{
    bool started = false;
    std::uint64_t second = 0;
    std::size_t used = 0;
};

template <class T>
struct lane_result
{
    std::shared_ptr<const decode_window<T>> window;
    add_result result;
    bool changed = false;
};

/// Checks `batch`, appends what passes, drops the oldest over the limits and
/// makes the new window.  `current` is not changed.
template <class T>
lane_result<T> add_batch(const std::shared_ptr<const decode_window<T>>& current,
                         std::span<const T> batch, std::uint64_t now_ns,
                         budget& spent, std::size_t per_second,
                         std::size_t max_items, std::size_t max_bytes)
{
    lane_result<T> out;
    add_result& result = out.result;

    const std::uint64_t second = now_ns / nanoseconds_per_second;
    if (!spent.started || second > spent.second)
    {
        spent.started = true;
        spent.second = second;
        spent.used = 0;
    }

    std::vector<std::shared_ptr<const stored_item<T>>> added;
    std::uint64_t next = current->next_sequence;
    reject_reason last = current->last_reject;
    for (std::size_t i = 0; i < batch.size(); ++i)
    {
        text_marks marks;
        const reject_reason why = i >= max_decode_batch
                                      ? reject_reason::batch_too_large
                                      : validate(batch[i], marks);
        if (why != reject_reason::none)
        {
            if (result.rejected == 0)
            {
                result.first_reject = why;
                result.first_reject_index = i;
            }
            ++result.rejected;
            last = why;
            continue;
        }
        if (spent.used >= per_second)
        {
            ++result.over_budget;
            continue;
        }
        ++spent.used;
        auto item = std::make_shared<stored_item<T>>();
        item->sequence = next++;
        item->received_ns = now_ns;
        item->marks = marks;
        item->cost = cost_of(batch[i]);
        item->value = batch[i];
        added.push_back(std::move(item));
    }
    result.accepted = added.size();

    out.changed = result.accepted != 0 || result.rejected != 0 ||
                  result.over_budget != 0;
    if (!out.changed)
    {
        out.window = current;
        return out;
    }

    // The oldest go first, until what is left is within both limits.
    const std::size_t old_count = current->items.size();
    const std::size_t total = old_count + added.size();
    std::size_t bytes = current->bytes;
    for (const auto& item : added)
    {
        bytes += item->cost;
    }
    const auto at = [&](std::size_t index) -> const stored_item<T>& {
        return index < old_count ? *current->items[index]
                                 : *added[index - old_count];
    };
    std::size_t drop = 0;
    while (drop < total && (total - drop > max_items || bytes > max_bytes))
    {
        bytes -= at(drop).cost;
        ++drop;
    }
    result.dropped = drop;

    auto next_window = std::make_shared<decode_window<T>>();
    next_window->items.reserve(total - drop);
    for (std::size_t i = drop; i < total; ++i)
    {
        next_window->items.push_back(i < old_count
                                         ? current->items[i]
                                         : added[i - old_count]);
    }
    next_window->first_sequence = current->first_sequence + drop;
    next_window->next_sequence = next;
    next_window->bytes = bytes;
    next_window->dropped = current->dropped + drop;
    next_window->rejected = current->rejected + result.rejected;
    next_window->over_budget = current->over_budget + result.over_budget;
    next_window->last_reject = last;
    out.window = std::move(next_window);
    return out;
}

template <class T>
decode_since<T> since_of(const decode_window<T>& window,
                         std::uint64_t sequence)
{
    decode_since<T> out;
    out.last_sequence = window.next_sequence - 1;
    if (sequence >= out.last_sequence)
    {
        return out;
    }
    out.gap = window.first_sequence > sequence + 1;
    const std::size_t start =
        sequence + 1 > window.first_sequence
            ? static_cast<std::size_t>(sequence + 1 - window.first_sequence)
            : 0;
    out.items.assign(window.items.begin() +
                         static_cast<std::ptrdiff_t>(start),
                     window.items.end());
    return out;
}

template <class T>
std::shared_ptr<const decode_window<T>>
emptied(const std::shared_ptr<const decode_window<T>>& window)
{
    auto fresh = std::make_shared<decode_window<T>>();
    fresh->first_sequence = window->next_sequence;
    fresh->next_sequence = window->next_sequence;
    return fresh;
}

} // namespace

struct decode_store::state
{
    budget frames;
    budget triggers;
};

struct decode_store::notice
{
    std::mutex mutex;
    std::optional<boost::asio::any_io_executor> executor;
    std::function<void()> function;
    std::atomic<bool> pending{false};
    bool closed = false;
};

decode_store::decode_store(decode_limits limits)
    : limits_(limits)
    , notice_(std::make_shared<notice>())
    , state_(std::make_unique<state>())
{
    auto first = std::make_shared<decode_snapshot>();
    first->frames = std::make_shared<const decode_window<decoded_frame>>();
    first->triggers = std::make_shared<const decode_window<decoded_trigger>>();
    current_ = std::move(first);
}

decode_store::~decode_store()
{
    const std::lock_guard lock(notice_->mutex);
    notice_->closed = true;
    notice_->function = nullptr;
    notice_->executor.reset();
}

std::shared_ptr<const decode_snapshot> decode_store::snapshot() const
{
    const std::lock_guard lock(publish_mutex_);
    return current_;
}

void decode_store::publish(std::shared_ptr<const decode_snapshot> next)
{
    std::shared_ptr<const decode_snapshot> old;
    {
        const std::lock_guard lock(publish_mutex_);
        old = std::move(current_);
        current_ = std::move(next);
    }
    // `old` ends here, outside the lock.
}

void decode_store::tell()
{
    std::optional<boost::asio::any_io_executor> executor;
    {
        const std::lock_guard lock(notice_->mutex);
        if (notice_->closed || !notice_->function || !notice_->executor)
        {
            return;
        }
        if (notice_->pending.exchange(true))
        {
            return;
        }
        executor = notice_->executor;
    }
    boost::asio::post(*executor, [shared = notice_] {
        std::function<void()> call;
        {
            const std::lock_guard lock(shared->mutex);
            shared->pending.store(false);
            if (shared->closed)
            {
                return;
            }
            call = shared->function;
        }
        if (call)
        {
            call();
        }
    });
}

add_result decode_store::add_records(std::span<const decoded_frame> batch,
                                     std::uint64_t now_ns)
{
    add_result result;
    {
        const std::lock_guard lock(write_mutex_);
        const auto before = snapshot();
        auto done = add_batch<decoded_frame>(
            before->frames, batch, now_ns, state_->frames,
            limits_.frames_per_second, limits_.max_frames,
            limits_.max_frame_bytes);
        result = done.result;
        if (!done.changed)
        {
            return result;
        }
        auto next = std::make_shared<decode_snapshot>();
        next->frames = std::move(done.window);
        next->triggers = before->triggers;
        publish(std::move(next));
    }
    tell();
    return result;
}

add_result decode_store::add_triggers(std::span<const decoded_trigger> batch,
                                      std::uint64_t now_ns)
{
    add_result result;
    {
        const std::lock_guard lock(write_mutex_);
        const auto before = snapshot();
        auto done = add_batch<decoded_trigger>(
            before->triggers, batch, now_ns, state_->triggers,
            limits_.triggers_per_second, limits_.max_triggers,
            limits_.max_trigger_bytes);
        result = done.result;
        if (!done.changed)
        {
            return result;
        }
        auto next = std::make_shared<decode_snapshot>();
        next->frames = before->frames;
        next->triggers = std::move(done.window);
        publish(std::move(next));
    }
    tell();
    return result;
}

decode_since<decoded_frame>
decode_store::records_since(std::uint64_t sequence) const
{
    return since_of(*snapshot()->frames, sequence);
}

decode_since<decoded_trigger>
decode_store::triggers_since(std::uint64_t sequence) const
{
    return since_of(*snapshot()->triggers, sequence);
}

void decode_store::clear()
{
    {
        const std::lock_guard lock(write_mutex_);
        const auto before = snapshot();
        auto next = std::make_shared<decode_snapshot>();
        next->frames = emptied(before->frames);
        next->triggers = emptied(before->triggers);
        *state_ = state{};
        publish(std::move(next));
    }
    tell();
}

void decode_store::set_notify(boost::asio::any_io_executor executor,
                              std::function<void()> notify)
{
    const std::lock_guard lock(notice_->mutex);
    if (notify)
    {
        notice_->executor = std::move(executor);
        notice_->function = std::move(notify);
    }
    else
    {
        notice_->executor.reset();
        notice_->function = nullptr;
    }
}

} // namespace vss::plugin::host
