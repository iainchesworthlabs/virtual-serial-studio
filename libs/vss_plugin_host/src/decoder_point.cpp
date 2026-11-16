//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/decoder_point.hpp>

#include <vss/core/control_lines.hpp>
#include <vss/core/hub.hpp>
#include <vss/core/types.hpp>
#include <vss/plugin/api_decoder.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/capture_point.hpp>
#include <vss/plugin/host/decode_store.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/instances.hpp>
#include <vss/plugin/host/traffic_tap.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vss::plugin::host
{

namespace
{

using namespace std::chrono_literals;

/// What the host leaves unacknowledged (section 20.4).
constexpr std::size_t window_bytes = std::size_t{256} * 1024;
/// How many `decoder.data` may wait for an acknowledgement.  The bytes are
/// the window; this bounds what a plugin that never acknowledges can make the
/// host hold when the messages carry no bytes.
constexpr std::size_t window_messages = 64;
/// What the tap holds for one decoder: 4 MiB, as section 20.4 says.
constexpr std::size_t tap_bytes = std::size_t{4} * 1024 * 1024;
constexpr std::size_t tap_items = 4096;
/// How much is taken from the tap at a time.
constexpr std::size_t take_items = 256;
/// How many messages one turn on the strand sends before it lets others
/// run.
constexpr int messages_a_turn = 16;
/// The number a request is encoded with; the runner gives it its own.
constexpr std::uint64_t placeholder_id = 1;
constexpr auto create_timeout = 10'000ms;
constexpr auto stop_timeout = 5'000ms;
/// The six lines of a serial port, as a mask.
constexpr unsigned six_lines = 0x3fU;

std::uint64_t nanoseconds_between(time_point from, time_point to) noexcept
{
    if (to <= from)
        return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(to - from)
            .count());
}

std::uint64_t steady_ns() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool all_digits(std::string_view text) noexcept
{
    return !text.empty() && std::ranges::all_of(text, [](char c)
                                                { return c >= '0' && c <= '9'; });
}

/// The rate and the format that the settings of a place say, "115200 8N1",
/// when they say them.
void read_settings(std::string_view settings, decoder_place& out)
{
    const std::size_t space = settings.find(' ');
    const std::string_view rate = settings.substr(0, space);
    if (all_digits(rate) && rate.size() <= 9)
    {
        std::uint64_t value = 0;
        for (const char c : rate)
            value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value >= 1 && value <= max_place_rate)
            out.rate = value;
    }
    if (space != std::string_view::npos)
    {
        const std::string_view format = settings.substr(space + 1);
        if (!format.empty() && format.size() <= max_place_format &&
            format.find(' ') == std::string_view::npos)
            out.format = std::string(format);
    }
}

decode_direction direction_of(decoder_direction value) noexcept
{
    return value == decoder_direction::tx ? decode_direction::tx
                                          : decode_direction::rx;
}

decoder_direction direction_of(data_direction value) noexcept
{
    return value == data_direction::tx ? decoder_direction::tx
                                       : decoder_direction::rx;
}

decode_fault fault_of(decoder_fault value) noexcept
{
    switch (value)
    {
    case decoder_fault::overlap:
        return decode_fault::overlap;
    case decoder_fault::too_soon:
        return decode_fault::too_soon;
    case decoder_fault::line_held:
        return decode_fault::line_held;
    case decoder_fault::transmitter_off:
        return decode_fault::transmitter_off;
    case decoder_fault::overrun:
        return decode_fault::overrun;
    }
    return decode_fault::overlap;
}

decode_trigger_kind kind_of(trigger_kind value) noexcept
{
    switch (value)
    {
    case trigger_kind::framing:
        return decode_trigger_kind::framing;
    case trigger_kind::unknown_type:
        return decode_trigger_kind::unknown_type;
    case trigger_kind::unknown_value:
        return decode_trigger_kind::unknown_value;
    case trigger_kind::check:
        return decode_trigger_kind::check;
    case trigger_kind::length:
        return decode_trigger_kind::length;
    case trigger_kind::timeout:
        return decode_trigger_kind::timeout;
    case trigger_kind::sequence:
        return decode_trigger_kind::sequence;
    case trigger_kind::contention:
        return decode_trigger_kind::contention;
    case trigger_kind::line_error:
        return decode_trigger_kind::line_error;
    case trigger_kind::decoder_lost:
        return decode_trigger_kind::decoder_lost;
    case trigger_kind::custom:
        return decode_trigger_kind::custom;
    }
    return decode_trigger_kind::framing;
}

decoded_frame to_frame(const decoder_frame_message& in)
{
    decoded_frame out;
    out.place = in.place;
    out.direction = direction_of(in.direction);
    out.offset = in.offset;
    out.length = in.length;
    out.t_start_ns = in.t_start_ns;
    out.t_end_ns = in.t_end_ns;
    out.message = in.message;
    out.summary = in.summary;
    out.fields.reserve(in.fields.size());
    for (const decoder_field& field : in.fields)
    {
        decode_field copy;
        copy.name = field.name;
        copy.offset = field.offset;
        copy.length = field.length;
        copy.value = field.value;
        out.fields.push_back(std::move(copy));
    }
    return out;
}

decoded_trigger to_trigger(const trigger_publish_message& in)
{
    decoded_trigger out;
    out.kind = kind_of(in.kind);
    out.code = in.code;
    out.when_ns = in.when_ns;
    out.place = in.place;
    out.direction = direction_of(in.direction);
    out.sender = in.sender;
    if (in.protocol)
        out.protocol = decode_protocol{in.protocol->id, in.protocol->version};
    if (in.frame)
    {
        decode_trigger_frame frame;
        frame.offset = in.frame->offset;
        frame.length = in.frame->length;
        frame.t_start_ns = in.frame->t_start_ns;
        frame.t_end_ns = in.frame->t_end_ns;
        frame.message = in.frame->message;
        out.frame = std::move(frame);
    }
    if (in.field)
    {
        decode_trigger_field field;
        field.name = in.field->name;
        field.offset = in.field->offset;
        field.length = in.field->length;
        out.field = std::move(field);
    }
    out.expected = in.expected;
    out.actual = in.actual;
    if (in.covers)
        out.covers = decode_covers{in.covers->first, in.covers->last};
    if (in.cause)
    {
        decode_cause cause;
        if (const auto* bus = std::get_if<trigger_bus_cause>(&*in.cause))
        {
            cause.which = decode_cause::kind::bus_fault;
            cause.fault = fault_of(bus->fault);
            cause.event = bus->event;
        }
        else if (const auto* own = std::get_if<trigger_own_action>(&*in.cause))
        {
            cause.which = decode_cause::kind::own_action;
            cause.own_action = own->action;
        }
        out.cause = cause;
    }
    if (in.request)
    {
        decode_request request;
        request.offset = in.request->offset;
        request.length = in.request->length;
        request.t_end_ns = in.request->t_end_ns;
        out.request = request;
    }
    out.text = in.text;
    return out;
}

class decoder_point_impl final : public decoder_point
{
public:
    decoder_point_impl(extension_context context, hub& source,
                       place_finder find, decoder_targets targets)
        : context_(std::move(context)), hub_(&source), find_(std::move(find)),
          targets_(std::move(targets)), alive_(std::make_shared<bool>(true))
    {
    }

    ~decoder_point_impl() override
    {
        *alive_ = false;
        // The taps go with the runs, before the hub they tap.
        runs_.clear();
    }

    bool take(const api_message& message) override
    {
        if (message.op == decoder_ack_message::op())
        {
            on_ack(message);
            return true;
        }
        if (message.op == decoder_frame_message::op())
        {
            on_frame(message);
            return true;
        }
        if (message.op == trigger_publish_message::op())
        {
            on_trigger(message);
            return true;
        }
        if (message.op == decoder_create_message::op() ||
            message.op == decoder_data_message::op() ||
            message.op == decoder_stop_message::op())
        {
            // The host's own messages, sent by the plugin.
            ++stray_unknown_;
            refuse(message.id, api_errc::bad_request,
                   "the host sends this message, and does not take it");
            return true;
        }
        return false;
    }

    void plugin_ready() override { refresh(); }

    void plugin_gone() override
    {
        for (auto& entry : runs_)
            finish(entry.second, "the plugin stopped", false);
        runs_.clear();
        failed_.clear();
    }

    void refresh() override
    {
        if (!context_.instances)
            return;
        std::set<std::uint64_t> wanted;
        for (const instance_view& view : context_.instances->views())
        {
            if (view.spec.kind != extension_kind::decoder)
                continue;
            const std::uint64_t number = view.spec.instance;
            if (context_.instances->running(number))
            {
                wanted.insert(number);
                if (!runs_.contains(number) && !failed_.contains(number))
                    start(view);
            }
        }
        for (auto it = runs_.begin(); it != runs_.end();)
        {
            if (wanted.contains(it->first))
            {
                ++it;
                continue;
            }
            finish(it->second, "the instance is not running", true);
            it = runs_.erase(it);
        }
        // A refusal is forgotten once the instance is gone, so that one made
        // again is tried again.
        for (auto it = failed_.begin(); it != failed_.end();)
            it = wanted.contains(it->first) ? std::next(it) : failed_.erase(it);
    }

    decoder_state state_of(std::uint64_t instance) const override
    {
        decoder_state out;
        if (const auto found = runs_.find(instance); found != runs_.end())
        {
            const run& r = found->second;
            out = r.counts;
            out.known = true;
            out.running = r.created;
            out.sent_seq = r.seq;
            out.acked_seq = r.acked;
            out.unacknowledged_bytes = r.unacked;
            return out;
        }
        if (const auto bad = failed_.find(instance); bad != failed_.end())
            out.reason = bad->second;
        if (const auto stray = stray_.find(instance); stray != stray_.end())
            out.refused = stray->second;
        return out;
    }

private:
    struct run
    {
        std::uint64_t instance = 0;
        std::uint64_t generation = 0;
        decoder_target target;
        std::unique_ptr<traffic_tap> tap;
        std::vector<std::string> names;
        /// The count of bytes sent for each place: receive, then send.
        std::vector<std::array<std::uint64_t, 2>> offsets;
        std::vector<control_lines> lines;
        std::vector<std::uint64_t> last_ns;
        time_point started{};
        /// `decoder.create` was answered `ok`.
        bool created = false;
        /// Taken from the tap, not sent yet; `front_used` bytes of the first
        /// are sent.
        std::deque<tap_item> backlog;
        std::size_t front_used = 0;
        std::uint64_t seq = 0;
        std::uint64_t acked = 0;
        /// Sent and not acknowledged: the `seq` and the bytes.
        std::deque<std::pair<std::uint64_t, std::size_t>> in_flight;
        std::size_t unacked = 0;
        decoder_state counts;
        bool pumping_posted = false;
    };

    void answer(std::uint64_t id, api_message message)
    {
        if (id != 0)
            context_.link.answer(id, std::move(message));
    }

    void refuse(std::uint64_t id, api_errc code, const std::string& text)
    {
        answer(id, error_answer(code, text));
    }

    void fail(std::uint64_t instance, const std::string& why)
    {
        failed_[instance] = why;
    }

    // --- Starting and ending an instance -------------------------------

    void start(const instance_view& view)
    {
        const std::uint64_t instance = view.spec.instance;
        if (!context_.may(permission::traffic_tap))
        {
            fail(instance, "the plugin may not see traffic");
            return;
        }
        std::optional<decoder_target> target =
            targets_ ? targets_(instance) : std::nullopt;
        if (!target)
            return;
        if (target->store == nullptr || target->places.empty() ||
            target->places.size() > max_decoder_places)
        {
            fail(instance, "the places or the store of the decoder are not "
                           "as they must be");
            return;
        }

        decoder_create_message create;
        create.instance = instance;
        create.params = view.spec.params;
        std::vector<tap_place> watched;
        std::vector<control_lines> lines;
        for (const std::string& name : target->places)
        {
            auto found = find_ ? find_(name) : std::nullopt;
            const bool twice = std::ranges::any_of(
                create.places, [&](const decoder_place& known)
                { return known.place == name; });
            if (!found || twice || name.empty() || name.size() > max_api_name)
            {
                fail(instance, "one of the places does not exist, or is "
                               "named twice");
                return;
            }
            found->tap.name = name;
            decoder_place place;
            place.place = name;
            place.bus = found->kind == place_kind::link;
            read_settings(found->settings, place);
            create.places.push_back(std::move(place));
            control_lines known;
            known.bits = found->lines.bits & six_lines;
            lines.push_back(known);
            watched.push_back(std::move(found->tap));
        }
        auto encoded = encode_as(create, placeholder_id);
        if (!encoded)
        {
            fail(instance, "the decoder could not be asked for");
            return;
        }

        run fresh;
        fresh.instance = instance;
        fresh.generation = ++generations_;
        fresh.target = std::move(*target);
        fresh.names = fresh.target.places;
        fresh.offsets.assign(fresh.names.size(), {0, 0});
        fresh.lines = std::move(lines);
        fresh.last_ns.assign(fresh.names.size(), 0);
        fresh.started = clock_type::now();
        tap_limits limits;
        limits.max_items = tap_items;
        limits.max_bytes = tap_bytes;
        limits.mark_losses = true;
        fresh.tap =
            std::make_unique<traffic_tap>(*hub_, std::move(watched), limits);
        const std::uint64_t generation = fresh.generation;
        runs_.insert_or_assign(instance, std::move(fresh));
        context_.link.request(
            std::move(*encoded),
            [alive = alive_, this, instance, generation](
                error_code failure, const api_message& reply)
            {
                if (*alive)
                    created(instance, generation, failure, reply);
            },
            create_timeout);
    }

    run* find_run(std::uint64_t instance, std::uint64_t generation)
    {
        const auto found = runs_.find(instance);
        if (found == runs_.end() || found->second.generation != generation)
            return nullptr;
        return &found->second;
    }

    void created(std::uint64_t instance, std::uint64_t generation,
                 error_code failure, const api_message& reply)
    {
        run* r = find_run(instance, generation);
        if (r == nullptr)
            return;
        if (failure || !std::holds_alternative<ok_message>(reply.body))
        {
            finish(*r, "the plugin did not take the decoder", false);
            fail(instance, "the plugin did not take the decoder");
            runs_.erase(instance);
            return;
        }
        r->created = true;
        r->tap->set_notify(
            [alive = alive_, this, instance, generation]()
            {
                if (*alive)
                    pump_later(instance, generation);
            },
            context_.executor);
    }

    /// Lets go of the decoder: the tap, the plugin's instance when it may
    /// still be told, and a `decoder_lost` trigger at each place.
    void finish(run& r, const std::string& why, bool tell_plugin)
    {
        r.tap.reset();
        r.backlog.clear();
        if (tell_plugin && r.created)
        {
            decoder_stop_message stop;
            stop.instance = r.instance;
            if (auto encoded = encode_as(stop, placeholder_id))
            {
                context_.link.request(
                    std::move(*encoded),
                    [](error_code, const api_message&) {},
                    stop_timeout);
            }
        }
        if (r.created || !why.empty())
            publish_lost(r, why);
        r.created = false;
    }

    /// The host's own trigger of section 20.6: the decoding stopped.
    void publish_lost(run& r, const std::string& why)
    {
        if (!r.target.store)
            return;
        const std::uint64_t now =
            nanoseconds_between(r.started, clock_type::now());
        std::vector<decoded_trigger> triggers;
        for (const std::string& name : r.names)
        {
            decoded_trigger lost;
            lost.kind = decode_trigger_kind::decoder_lost;
            lost.when_ns = now;
            lost.place = name;
            lost.text = "the decoder stopped: " + why;
            triggers.push_back(std::move(lost));
        }
        r.target.store->add_triggers(triggers, steady_ns());
    }

    // --- What the plugin sends -----------------------------------------

    /// The run that a message of the plugin is for, or nothing and the
    /// refusal counted.  `place` is checked when it is not empty.
    run* usable(std::uint64_t instance, const std::string* place)
    {
        const auto count_stray = [&]
        {
            ++stray_[instance];
            return nullptr;
        };
        if (!context_.may(permission::traffic_tap) || !context_.instances ||
            !context_.instances->running(instance))
            return count_stray();
        const auto view = context_.instances->find(instance);
        if (!view || view->spec.kind != extension_kind::decoder)
            return count_stray();
        const auto found = runs_.find(instance);
        if (found == runs_.end() || !found->second.created)
            return count_stray();
        run& r = found->second;
        if (place != nullptr && std::ranges::find(r.names, *place) ==
                                    r.names.end())
        {
            ++r.counts.refused;
            return nullptr;
        }
        return &r;
    }

    /// Reads a message that must be one of the plugin's notifications.
    template <class Body>
    std::optional<Body> read(const api_message& message)
    {
        auto body = decode_as<Body>(message);
        if (!body)
        {
            ++stray_unknown_;
            refuse(message.id, api_errc::bad_request,
                   "the message is not as the API describes it");
            return std::nullopt;
        }
        if (message.id != 0)
        {
            ++stray_unknown_;
            refuse(message.id, api_errc::bad_request,
                   "this message is a notification and is not asked as a "
                   "request");
            return std::nullopt;
        }
        return std::move(*body);
    }

    void on_ack(const api_message& message)
    {
        const auto body = read<decoder_ack_message>(message);
        if (!body)
            return;
        run* r = usable(body->instance, nullptr);
        if (r == nullptr)
            return;
        // An acknowledgement of something not sent, or of what an earlier
        // one covered, is the plugin's mistake: it changes nothing.
        if (body->seq <= r->acked || body->seq > r->seq)
        {
            ++r->counts.refused;
            return;
        }
        while (!r->in_flight.empty() && r->in_flight.front().first <= body->seq)
        {
            r->unacked -= r->in_flight.front().second;
            r->in_flight.pop_front();
        }
        r->acked = body->seq;
        pump(*r);
    }

    void on_frame(const api_message& message)
    {
        const auto body = read<decoder_frame_message>(message);
        if (!body)
            return;
        run* r = usable(body->instance, &body->place);
        if (r == nullptr)
            return;
        const std::array<decoded_frame, 1> one{to_frame(*body)};
        const add_result result = r->target.store->add_records(one, steady_ns());
        if (result.accepted == 0)
            ++r->counts.refused;
    }

    void on_trigger(const api_message& message)
    {
        const auto body = read<trigger_publish_message>(message);
        if (!body)
            return;
        run* r = usable(body->instance, &body->place);
        if (r == nullptr)
            return;
        const std::array<decoded_trigger, 1> one{to_trigger(*body)};
        const add_result result =
            r->target.store->add_triggers(one, steady_ns());
        if (result.accepted == 0)
            ++r->counts.refused;
    }

    // --- What is sent to the plugin ------------------------------------

    void pump_later(std::uint64_t instance, std::uint64_t generation)
    {
        run* r = find_run(instance, generation);
        if (r != nullptr)
            pump(*r);
    }

    void repost(run& r)
    {
        if (r.pumping_posted)
            return;
        r.pumping_posted = true;
        boost::asio::post(
            context_.executor,
            [alive = alive_, this, instance = r.instance,
             generation = r.generation]()
            {
                if (!*alive)
                    return;
                run* again = find_run(instance, generation);
                if (again == nullptr)
                    return;
                again->pumping_posted = false;
                pump(*again);
            });
    }

    std::size_t window_left(const run& r) const noexcept
    {
        if (r.in_flight.size() >= window_messages || r.unacked >= window_bytes)
            return 0;
        return window_bytes - r.unacked;
    }

    void pump(run& r)
    {
        if (!r.created || !r.tap)
            return;
        for (int sent = 0; sent < messages_a_turn; ++sent)
        {
            if (r.backlog.empty())
            {
                const std::size_t room = window_left(r);
                if (room == 0)
                    return;
                tap_batch batch = r.tap->take(take_items, room);
                for (tap_item& item : batch.items)
                    r.backlog.push_back(std::move(item));
                r.front_used = 0;
                if (r.backlog.empty())
                    return;
            }
            if (!send_some(r))
                return;
        }
        // More may be waiting; let other work on the strand go first.
        repost(r);
    }

    /// Builds and sends one message from the backlog.  False when nothing
    /// was sent and nothing more can be until the plugin acknowledges.
    bool send_some(run& r)
    {
        std::vector<decoder_item> items;
        byte_buffer blob;
        std::size_t room = window_left(r);
        while (!r.backlog.empty() && items.size() < max_decoder_items)
        {
            tap_item& front = r.backlog.front();
            if (!add_item(r, front, items, blob, room))
                break;
        }
        if (items.empty())
            return r.backlog.empty();
        decoder_data_message message;
        message.instance = r.instance;
        message.seq = r.seq + 1;
        message.items = std::move(items);
        message.data = std::move(blob);
        auto encoded = encode_as(message, 0);
        if (!encoded)
        {
            finish(r, "the stream could not be written", true);
            fail(r.instance, "the stream could not be written");
            r.tap.reset();
            return false;
        }
        r.seq = message.seq;
        r.in_flight.emplace_back(r.seq, message.data.size());
        r.unacked += message.data.size();
        ++r.counts.messages;
        r.counts.items += message.items.size();
        r.counts.bytes += message.data.size();
        context_.link.notify(std::move(*encoded));
        return true;
    }

    std::size_t index_of(const run& r, const std::string& place) const
    {
        return static_cast<std::size_t>(
            std::ranges::find(r.names, place) - r.names.begin());
    }

    /// The hub's time as the stream's: since the decoder started, and never
    /// back within a place.
    std::uint64_t time_of(run& r, std::size_t index, time_point when)
    {
        const std::uint64_t t =
            std::max(nanoseconds_between(r.started, when), r.last_ns[index]);
        r.last_ns[index] = t;
        return t;
    }

    /// Adds the first item of the backlog, or as much of it as the room
    /// takes, to the message.  False when no more can be added.
    bool add_item(run& r, tap_item& front, std::vector<decoder_item>& items,
                  byte_buffer& blob, std::size_t& room)
    {
        const std::size_t index = index_of(r, front.place);
        if (index >= r.names.size() && front.kind != tap_item_kind::loss)
        {
            r.backlog.pop_front();
            r.front_used = 0;
            return true;
        }
        switch (front.kind)
        {
        case tap_item_kind::data:
        {
            if (front.bytes == nullptr || front.bytes->empty())
            {
                r.backlog.pop_front();
                r.front_used = 0;
                return true;
            }
            const std::size_t left = front.bytes->size() - r.front_used;
            const std::size_t count = std::min<std::size_t>(
                {left, room, static_cast<std::size_t>(max_decoder_chunk)});
            if (count == 0)
                return false;
            const std::size_t way =
                front.direction == data_direction::tx ? 1U : 0U;
            decoder_data_item item;
            item.place = front.place;
            item.direction = direction_of(front.direction);
            item.offset = r.offsets[index][way];
            item.t_ns = time_of(r, index, front.when);
            item.length = count;
            items.push_back(std::move(item));
            const auto from = front.bytes->begin() +
                              static_cast<std::ptrdiff_t>(r.front_used);
            blob.insert(blob.end(), from,
                        from + static_cast<std::ptrdiff_t>(count));
            r.offsets[index][way] += count;
            room -= count;
            r.front_used += count;
            if (r.front_used == front.bytes->size())
            {
                r.backlog.pop_front();
                r.front_used = 0;
            }
            return true;
        }
        case tap_item_kind::lines:
        {
            control_lines now;
            now.bits = front.lines.bits & six_lines;
            const unsigned changed = now.bits ^ r.lines[index].bits;
            r.lines[index] = now;
            if (changed != 0)
            {
                decoder_line_item item;
                item.place = front.place;
                item.t_ns = time_of(r, index, front.when);
                constexpr std::array<decoder_line, max_decoder_lines> order{
                    decoder_line::dtr, decoder_line::rts, decoder_line::cts,
                    decoder_line::dsr, decoder_line::dcd, decoder_line::ri};
                for (std::size_t line = 0; line < order.size(); ++line)
                {
                    const unsigned mask = 1U << line;
                    item.lines[line] = (now.bits & mask) != 0;
                    if ((changed & mask) != 0)
                        item.changed.push_back(order[line]);
                }
                items.push_back(std::move(item));
            }
            r.backlog.pop_front();
            r.front_used = 0;
            return true;
        }
        case tap_item_kind::loss:
        {
            for (const tap_loss& loss : front.losses)
            {
                const std::size_t where = index_of(r, loss.place);
                if (where >= r.names.size() || loss.bytes == 0)
                    continue;
                const std::size_t way =
                    loss.direction == data_direction::tx ? 1U : 0U;
                decoder_lost_item item;
                item.place = loss.place;
                item.direction = direction_of(loss.direction);
                item.from_offset = r.offsets[where][way];
                item.bytes = loss.bytes;
                item.from_ns = time_of(r, where, loss.first);
                item.to_ns = time_of(r, where, loss.last);
                r.offsets[where][way] += loss.bytes;
                r.counts.lost_bytes += loss.bytes;
                ++r.counts.lost_items;
                items.push_back(std::move(item));
            }
            r.backlog.pop_front();
            r.front_used = 0;
            return true;
        }
        case tap_item_kind::error:
        case tap_item_kind::bus_fault:
            // Not in the stream: see the header.
            r.backlog.pop_front();
            r.front_used = 0;
            return true;
        }
        return false;
    }

    extension_context context_;
    hub* hub_;
    place_finder find_;
    decoder_targets targets_;
    /// False once this object has gone, for what was posted and runs
    /// later.  Read and written on the service's strand only.
    std::shared_ptr<bool> alive_;
    std::map<std::uint64_t, run> runs_;
    /// Instances that cannot start, and why.
    std::map<std::uint64_t, std::string> failed_;
    /// What the plugin sent for an instance that does not run.
    std::map<std::uint64_t, std::uint64_t> stray_;
    std::uint64_t stray_unknown_ = 0;
    std::uint64_t generations_ = 0;
};

} // namespace

std::unique_ptr<decoder_point>
make_decoder_point(const extension_context& context, hub& source,
                   place_finder find, decoder_targets targets)
{
    return std::make_unique<decoder_point_impl>(context, source,
                                                std::move(find),
                                                std::move(targets));
}

} // namespace vss::plugin::host
