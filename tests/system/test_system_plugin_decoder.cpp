//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The extension point `decoder.*` and `trigger.publish`
// (`make_decoder_point`) on its own.  A test makes the handler with a
// hand-made `extension_context`: the link records what is sent and asked, the
// executor is an `io_context` that the test runs by hand, the instances are a
// real `plugin_instances` whose plugin answers from memory, and the hub is a
// real one with a mock port and a loopback port, so that the traffic tap sees
// real events.  Nothing here starts a process.
//

#include "support/test_support.hpp"

#include <vss/core/control_lines.hpp>
#include <vss/core/types.hpp>
#include <vss/plugin/api_decoder.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/decode_store.hpp>
#include <vss/plugin/host/decoder_point.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/instances.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/io_context.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using vss::control_line;
using vss::plugin::api_message;
using vss::plugin::decode_as;
using vss::plugin::decoder_ack_message;
using vss::plugin::decoder_create_message;
using vss::plugin::decoder_data_item;
using vss::plugin::decoder_data_message;
using vss::plugin::decoder_direction;
using vss::plugin::decoder_frame_message;
using vss::plugin::decoder_line;
using vss::plugin::decoder_line_item;
using vss::plugin::decoder_lost_item;
using vss::plugin::decoder_stop_message;
using vss::plugin::encode_as;
using vss::plugin::error_message;
using vss::plugin::extension_kind;
using vss::plugin::extension_point;
using vss::plugin::ok_message;
using vss::plugin::permission;
using vss::plugin::trigger_kind;
using vss::plugin::trigger_publish_message;
using vss::plugin::host::capture_place;
using vss::plugin::host::decode_store;
using vss::plugin::host::decode_trigger_kind;
using vss::plugin::host::decoder_point;
using vss::plugin::host::decoder_state;
using vss::plugin::host::decoder_target;
using vss::plugin::host::extension_context;
using vss::plugin::host::instance_spec;
using vss::plugin::host::make_decoder_point;
using vss::plugin::host::plugin_instances;
using vss::testkit::wait_until;
using vss_test_support::add_loopback;
using vss_test_support::add_mock;
using vss_test_support::mock_endpoint;
using vss_test_support::start_hub;
using namespace std::chrono_literals;

namespace
{

constexpr std::string_view port_name = "port-a";
constexpr std::string_view bus_name = "bus-a";
constexpr std::uint64_t first_instance = 1;
constexpr std::size_t window = std::size_t{256} * 1024;

/// What the handler said to the plugin: the request it answers, and the
/// answer.
struct answered
{
    std::uint64_t re = 0;
    api_message message{};
};

/// A request the handler made of the plugin, and what to call to answer it.
struct asked
{
    api_message message;
    vss::plugin::host::plugin_link::answer_handler handler;
};

api_message ok_reply()
{
    api_message reply;
    reply.body = ok_message{};
    reply.op = std::string(vss::plugin::op_of(reply.body));
    return reply;
}

api_message error_reply()
{
    error_message body;
    body.category = "vss.plugin.api";
    body.value = "refused";
    body.message = "no";
    api_message reply;
    reply.body = std::move(body);
    reply.op = std::string(vss::plugin::op_of(reply.body));
    return reply;
}

template <class Message>
api_message message_of(const Message& body, std::uint64_t id = 0)
{
    auto encoded = encode_as(body, id);
    if (encoded.has_error())
        throw vss_test::failure("a message would not encode: " +
                                std::string(Message::op()));
    return std::move(encoded).value();
}

/// What the plugin was sent, read back.
struct stream
{
    std::vector<decoder_data_message> messages;
    std::vector<decoder_data_item> data;
    std::vector<decoder_lost_item> lost;
    std::vector<decoder_line_item> lines;
    std::string blob;
    std::size_t bytes_in_last = 0;
};

/// The handler with everything around it.  The order of the members is the
/// order they are made in, and the handler goes first: it must not outlive the
/// hub it taps, the executor or the instances.
struct rig
{
    explicit rig(std::vector<permission> granted = {permission::traffic_tap})
        : hub(start_hub()), serial(add_mock(*hub, std::string(port_name))),
          wire(add_loopback(*hub, std::string(bus_name))),
          store(std::make_shared<decode_store>()),
          instances(std::make_shared<plugin_instances>(
              declared(), [this](api_message message,
                                 plugin_instances::answer_handler reply)
              { instance_requests.push_back({std::move(message),
                                             std::move(reply)}); }))
    {
        extension_context context;
        context.plugin_id = "com.example.decoder";
        context.plugin_name = "Decoder plugin";
        context.plugin_version = "1.0.0";
        context.granted = std::move(granted);
        context.instances = instances;
        context.executor = io.get_executor();
        context.link.answer = [this](std::uint64_t re, api_message message)
        { answers.push_back(answered{re, std::move(message)}); };
        context.link.notify = [this](api_message message)
        { notes.push_back(std::move(message)); };
        context.link.request =
            [this](api_message message,
                   vss::plugin::host::plugin_link::answer_handler reply,
                   std::chrono::milliseconds)
        { requests.push_back({std::move(message), std::move(reply)}); };
        handler = make_decoder_point(
            context, *hub,
            [this](std::string_view name) { return find(name); },
            [this](std::uint64_t instance)
            {
                return target(instance);
            });
    }

    rig(const rig&) = delete;
    rig& operator=(const rig&) = delete;

    static std::vector<extension_point> declared()
    {
        extension_point decoder;
        decoder.kind = extension_kind::decoder;
        decoder.id = "modbus";
        decoder.max_instances = 4;
        extension_point generator;
        generator.kind = extension_kind::generator;
        generator.id = "replay";
        generator.max_instances = 4;
        return {decoder, generator};
    }

    /// The places the plugin may read: a port, and a link taken for a bus.
    std::optional<capture_place> find(std::string_view name) const
    {
        capture_place place;
        place.settings = "115200 8N1";
        if (name == port_name)
        {
            place.tap.endpoints = {serial.endpoint->id()};
        }
        else if (name == bus_name)
        {
            place.tap.endpoints = {wire->id()};
            place.kind = vss::plugin::host::place_kind::link;
            place.lines = wire->lines();
            place.settings = "9600";
        }
        else
        {
            return std::nullopt;
        }
        place.tap.name = std::string(name);
        return place;
    }

    std::optional<decoder_target> target(std::uint64_t instance)
    {
        if (!bound)
            return std::nullopt;
        (void)instance;
        decoder_target out;
        out.places = places;
        out.store = store;
        return out;
    }

    /// The strand's work, run now.
    void pump()
    {
        io.poll();
        io.restart();
    }

    /// The plugin answers what the instances asked it.
    void settle()
    {
        auto waiting = std::move(instance_requests);
        instance_requests.clear();
        for (auto& request : waiting)
            request.handler({}, ok_reply());
    }

    /// An instance of the given kind is wanted and running.
    void run_instance(std::uint64_t number, extension_kind kind,
                      std::string_view type)
    {
        instance_spec spec;
        spec.instance = number;
        spec.kind = kind;
        spec.type = std::string(type);
        wanted.push_back(spec);
        instances->want(wanted);
        if (!instances_ready)
        {
            instances->plugin_ready();
            instances_ready = true;
        }
        settle();
    }

    /// A decoder instance runs and `decoder.create` was answered `ok`.
    void start_decoder(std::vector<std::string> names = {std::string(port_name)})
    {
        places = std::move(names);
        bound = true;
        run_instance(first_instance, extension_kind::decoder, "modbus");
        handler->refresh();
        REQUIRE_EQ(requests.size(), std::size_t{1});
        answer_request(0, ok_reply());
        pump();
    }

    void answer_request(std::size_t index, const api_message& reply)
    {
        REQUIRE(index < requests.size());
        requests[index].handler({}, reply);
    }

    /// Every `decoder.data` the plugin was sent, read back.
    stream sent() const
    {
        stream out;
        for (const api_message& note : notes)
        {
            if (note.op != decoder_data_message::op())
                continue;
            auto read = decode_as<decoder_data_message>(note);
            if (read.has_error())
                throw vss_test::failure("a decoder.data does not read back");
            const decoder_data_message& body = read.value();
            out.messages.push_back(body);
            out.blob.append(body.data.begin(), body.data.end());
            for (const auto& item : body.items)
            {
                if (const auto* data = std::get_if<decoder_data_item>(&item))
                    out.data.push_back(*data);
                else if (const auto* lost = std::get_if<decoder_lost_item>(&item))
                    out.lost.push_back(*lost);
                else if (const auto* line = std::get_if<decoder_line_item>(&item))
                    out.lines.push_back(*line);
            }
        }
        return out;
    }

    /// The bytes of `data` items plus the bytes said to be lost.
    std::uint64_t accounted() const
    {
        const stream got = sent();
        std::uint64_t total = 0;
        for (const auto& data : got.data)
            total += data.length;
        for (const auto& lost : got.lost)
            total += lost.bytes;
        return total;
    }

    /// The last `seq` the plugin was sent.
    std::uint64_t last_seq() const
    {
        const stream got = sent();
        return got.messages.empty() ? 0 : got.messages.back().seq;
    }

    /// Runs the strand until `bytes` bytes are accounted for, acknowledging
    /// what arrives when `acknowledge` is set.
    bool until_accounted(std::uint64_t bytes, bool acknowledge)
    {
        return wait_until(
            [&]
            {
                pump();
                if (acknowledge && last_seq() != 0)
                    ack(last_seq());
                return accounted() >= bytes;
            },
            30000ms);
    }

    void ack(std::uint64_t seq, std::uint64_t instance = first_instance)
    {
        decoder_ack_message body;
        body.instance = instance;
        body.seq = seq;
        REQUIRE(handler->take(message_of(body)));
        pump();
    }

    decoder_state state() const { return handler->state_of(first_instance); }

    std::size_t refused() const { return state().refused; }

    boost::asio::io_context io{};
    std::shared_ptr<vss::hub> hub;
    mock_endpoint serial;
    std::shared_ptr<vss::endpoint> wire;
    std::shared_ptr<decode_store> store;
    std::shared_ptr<plugin_instances> instances;
    std::vector<asked> instance_requests{};
    std::vector<instance_spec> wanted{};
    bool instances_ready = false;
    bool bound = false;
    std::vector<std::string> places{};
    std::vector<answered> answers{};
    std::vector<api_message> notes{};
    std::vector<asked> requests{};
    std::unique_ptr<decoder_point> handler{};
};

decoder_frame_message frame_of(std::string_view place = port_name)
{
    decoder_frame_message body;
    body.instance = first_instance;
    body.place = std::string(place);
    body.offset = 0;
    body.length = 8;
    body.t_start_ns = 10;
    body.t_end_ns = 20;
    body.message = "read";
    body.summary = "a read";
    return body;
}

trigger_publish_message trigger_of(std::string_view place = port_name)
{
    trigger_publish_message body;
    body.instance = first_instance;
    body.kind = trigger_kind::check;
    body.when_ns = 5;
    body.place = std::string(place);
    body.text = "the check value is wrong";
    return body;
}

std::string code_of(const answered& answer)
{
    const auto* const error = std::get_if<error_message>(&answer.message.body);
    if (error == nullptr)
        return "ok";
    return error->value;
}

} // namespace

// -- refusals -----------------------------------------------------------------

TEST_CASE("plugin decoder: without traffic.tap the decoder is not started")
{
    rig fixture({});
    fixture.places = {std::string(port_name)};
    fixture.bound = true;
    fixture.run_instance(first_instance, extension_kind::decoder, "modbus");
    fixture.handler->refresh();
    REQUIRE(fixture.requests.empty());
    const decoder_state state = fixture.state();
    REQUIRE(!state.known);
    REQUIRE(!state.reason.empty());
}

TEST_CASE("plugin decoder: without traffic.tap a frame and a trigger are "
          "refused")
{
    rig fixture({});
    REQUIRE(fixture.handler->take(message_of(frame_of())));
    REQUIRE(fixture.handler->take(message_of(trigger_of())));
    REQUIRE_EQ(fixture.store->snapshot()->frames->items.size(),
               std::size_t{0});
    REQUIRE_EQ(fixture.store->snapshot()->triggers->items.size(),
               std::size_t{0});
    REQUIRE_EQ(fixture.state().refused, std::size_t{2});
}

TEST_CASE("plugin decoder: a message for an instance that is not running is "
          "refused")
{
    rig fixture;
    // Nothing runs yet.
    REQUIRE(fixture.handler->take(message_of(frame_of())));
    REQUIRE_EQ(fixture.store->snapshot()->frames->items.size(),
               std::size_t{0});
    REQUIRE_EQ(fixture.state().refused, std::size_t{1});

    // An instance that is wanted but that the plugin has not made.
    fixture.bound = true;
    fixture.places = {std::string(port_name)};
    instance_spec spec;
    spec.instance = first_instance;
    spec.kind = extension_kind::decoder;
    spec.type = "modbus";
    fixture.instances->want({spec});
    REQUIRE(fixture.handler->take(message_of(trigger_of())));
    REQUIRE_EQ(fixture.state().refused, std::size_t{2});
}

TEST_CASE("plugin decoder: an instance of another kind is not a decoder")
{
    rig fixture;
    fixture.bound = true;
    fixture.places = {std::string(port_name)};
    fixture.run_instance(first_instance, extension_kind::generator, "replay");
    fixture.handler->refresh();
    REQUIRE(fixture.requests.empty());
    REQUIRE(fixture.handler->take(message_of(frame_of())));
    REQUIRE(fixture.handler->take(message_of(trigger_of())));
    REQUIRE_EQ(fixture.store->snapshot()->frames->items.size(),
               std::size_t{0});
    REQUIRE_EQ(fixture.state().refused, std::size_t{2});
}

TEST_CASE("plugin decoder: a place that is not the instance's is refused")
{
    rig fixture;
    fixture.start_decoder();
    REQUIRE(fixture.handler->take(message_of(frame_of("elsewhere"))));
    REQUIRE(fixture.handler->take(message_of(trigger_of(bus_name))));
    REQUIRE_EQ(fixture.store->snapshot()->frames->items.size(),
               std::size_t{0});
    REQUIRE_EQ(fixture.store->snapshot()->triggers->items.size(),
               std::size_t{0});
    REQUIRE_EQ(fixture.refused(), std::size_t{2});
}

TEST_CASE("plugin decoder: a request form of a notification is bad_request, "
          "never unknown_op")
{
    rig fixture;
    fixture.start_decoder();
    api_message frame = message_of(frame_of());
    frame.id = 41;
    api_message trigger = message_of(trigger_of());
    trigger.id = 42;
    decoder_ack_message body;
    body.instance = first_instance;
    body.seq = 1;
    api_message ack = message_of(body);
    ack.id = 43;
    REQUIRE(fixture.handler->take(frame));
    REQUIRE(fixture.handler->take(trigger));
    REQUIRE(fixture.handler->take(ack));
    REQUIRE_EQ(fixture.answers.size(), std::size_t{3});
    for (const answered& answer : fixture.answers)
        REQUIRE_EQ(code_of(answer), std::string("bad_request"));
    REQUIRE_EQ(fixture.store->snapshot()->frames->items.size(),
               std::size_t{0});
}

TEST_CASE("plugin decoder: the host's own messages sent by the plugin are "
          "bad_request")
{
    rig fixture;
    decoder_stop_message stop;
    stop.instance = first_instance;
    REQUIRE(fixture.handler->take(message_of(stop, 7)));
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    REQUIRE_EQ(code_of(fixture.answers.back()), std::string("bad_request"));
    api_message quiet = message_of(stop, 7);
    quiet.id = 0;
    REQUIRE(fixture.handler->take(quiet));
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
}

TEST_CASE("plugin decoder: a message of another op is not taken")
{
    rig fixture;
    api_message other;
    other.op = "capture.start";
    other.id = 4;
    other.body = vss::plugin::other_message{"{}"};
    REQUIRE(!fixture.handler->take(other));
    REQUIRE(fixture.answers.empty());
}

TEST_CASE("plugin decoder: a place that nobody knows stops the start")
{
    rig fixture;
    fixture.bound = true;
    fixture.places = {"nowhere"};
    fixture.run_instance(first_instance, extension_kind::decoder, "modbus");
    fixture.handler->refresh();
    REQUIRE(fixture.requests.empty());
    REQUIRE(!fixture.state().reason.empty());
    // It is not tried again until the instance is made again.
    fixture.handler->refresh();
    REQUIRE(fixture.requests.empty());
}

TEST_CASE("plugin decoder: no binding yet means no start, and a later one "
          "starts it")
{
    rig fixture;
    fixture.run_instance(first_instance, extension_kind::decoder, "modbus");
    fixture.handler->refresh();
    REQUIRE(fixture.requests.empty());
    fixture.bound = true;
    fixture.places = {std::string(port_name)};
    fixture.handler->refresh();
    REQUIRE_EQ(fixture.requests.size(), std::size_t{1});
}

// -- the start ----------------------------------------------------------------

TEST_CASE("plugin decoder: decoder.create names the places and their "
          "settings")
{
    rig fixture;
    fixture.bound = true;
    fixture.places = {std::string(port_name), std::string(bus_name)};
    fixture.run_instance(first_instance, extension_kind::decoder, "modbus");
    fixture.handler->refresh();
    REQUIRE_EQ(fixture.requests.size(), std::size_t{1});
    const auto create =
        decode_as<decoder_create_message>(fixture.requests[0].message);
    REQUIRE(create.has_value());
    REQUIRE_EQ(create.value().instance, first_instance);
    REQUIRE_EQ(create.value().places.size(), std::size_t{2});
    REQUIRE_EQ(create.value().places[0].place, std::string(port_name));
    REQUIRE_EQ(create.value().places[0].rate, std::uint64_t{115200});
    REQUIRE_EQ(create.value().places[0].format, std::string("8N1"));
    REQUIRE(!create.value().places[0].bus);
    REQUIRE_EQ(create.value().places[1].rate, std::uint64_t{9600});
    REQUIRE(create.value().places[1].format.empty());
    REQUIRE(create.value().places[1].bus);
    REQUIRE_EQ(create.value().params, std::string("{}"));
    // Asked for once, not on every refresh.
    fixture.handler->refresh();
    REQUIRE_EQ(fixture.requests.size(), std::size_t{1});
}

TEST_CASE("plugin decoder: nothing is sent until the plugin has taken the "
          "decoder")
{
    rig fixture;
    fixture.bound = true;
    fixture.places = {std::string(port_name)};
    fixture.run_instance(first_instance, extension_kind::decoder, "modbus");
    fixture.handler->refresh();
    fixture.serial.inject_read("early");
    wait_until([&] { fixture.pump(); return false; }, 300ms);
    REQUIRE(fixture.sent().messages.empty());
    REQUIRE(!fixture.state().running);
    // What the tap held is sent when the plugin has said ok.
    fixture.answer_request(0, ok_reply());
    REQUIRE(fixture.until_accounted(5, false));
    const stream got = fixture.sent();
    REQUIRE_EQ(got.blob, std::string("early"));
    REQUIRE(fixture.state().running);
}

TEST_CASE("plugin decoder: an error to decoder.create ends the instance and "
          "the store hears of it")
{
    rig fixture;
    fixture.bound = true;
    fixture.places = {std::string(port_name), std::string(bus_name)};
    fixture.run_instance(first_instance, extension_kind::decoder, "modbus");
    fixture.handler->refresh();
    fixture.answer_request(0, error_reply());
    REQUIRE(!fixture.state().known);
    REQUIRE(!fixture.state().reason.empty());
    const auto view = fixture.store->snapshot();
    REQUIRE_EQ(view->triggers->items.size(), std::size_t{2});
    REQUIRE(view->triggers->items[0]->value.kind ==
            decode_trigger_kind::decoder_lost);
    // It is not started again while it stays refused.
    fixture.handler->refresh();
    REQUIRE_EQ(fixture.requests.size(), std::size_t{1});
    // And the tap is let go: nothing is sent whatever arrives.
    fixture.serial.inject_read("x");
    wait_until([&] { fixture.pump(); return false; }, 200ms);
    REQUIRE(fixture.sent().messages.empty());
}

// -- the stream ---------------------------------------------------------------

TEST_CASE("plugin decoder: data is sent in order with offsets that add up")
{
    rig fixture;
    fixture.start_decoder();
    fixture.serial.inject_read("abc");
    REQUIRE(fixture.until_accounted(3, true));
    fixture.serial.inject_read("defgh");
    REQUIRE(fixture.until_accounted(8, true));
    const stream got = fixture.sent();
    REQUIRE_EQ(got.blob, std::string("abcdefgh"));
    std::uint64_t offset = 0;
    std::uint64_t previous_ns = 0;
    for (const decoder_data_item& item : got.data)
    {
        REQUIRE_EQ(item.place, std::string(port_name));
        REQUIRE(item.direction == decoder_direction::rx);
        REQUIRE_EQ(item.offset, offset);
        REQUIRE(item.length >= 1);
        REQUIRE(item.t_ns >= previous_ns);
        offset += item.length;
        previous_ns = item.t_ns;
    }
    REQUIRE_EQ(offset, std::uint64_t{8});
    for (std::size_t i = 0; i < got.messages.size(); ++i)
        REQUIRE_EQ(got.messages[i].seq, std::uint64_t{i + 1});
}

TEST_CASE("plugin decoder: a chunk over the limit of a chunk is split")
{
    rig fixture;
    fixture.start_decoder();
    fixture.serial.inject_read(std::string(100000, 'q'));
    REQUIRE(fixture.until_accounted(100000, true));
    const stream got = fixture.sent();
    REQUIRE_EQ(got.blob.size(), std::size_t{100000});
    std::uint64_t offset = 0;
    for (const decoder_data_item& item : got.data)
    {
        REQUIRE(item.length <= vss::plugin::max_decoder_chunk);
        REQUIRE_EQ(item.offset, offset);
        offset += item.length;
    }
    REQUIRE_EQ(offset, std::uint64_t{100000});
}

TEST_CASE("plugin decoder: line changes are in the stream with what changed")
{
    rig fixture;
    fixture.start_decoder({std::string(bus_name)});
    REQUIRE(!fixture.wire->set_line(control_line::dtr, true));
    REQUIRE(wait_until(
        [&]
        {
            fixture.pump();
            return !fixture.sent().lines.empty();
        },
        20000ms));
    const stream got = fixture.sent();
    const decoder_line_item& line = got.lines.front();
    REQUIRE_EQ(line.place, std::string(bus_name));
    REQUIRE_EQ(line.changed.size(), std::size_t{1});
    REQUIRE(line.changed.front() == decoder_line::dtr);
    const std::optional<bool> dtr = line.lines[0];
    REQUIRE(dtr.has_value());
    REQUIRE(dtr.value());
}

TEST_CASE("plugin decoder: the window is never exceeded under a slow plugin")
{
    rig fixture;
    fixture.start_decoder();
    // 400 chunks of 1000 bytes: more than the window holds.
    constexpr std::uint64_t total = std::uint64_t{400} * 1000;
    for (int i = 0; i < 400; ++i)
        fixture.serial.inject_read(std::string(1000, 'w'));
    // Nothing is acknowledged, so no more than the window is sent.
    REQUIRE(wait_until(
        [&]
        {
            fixture.pump();
            return fixture.state().unacknowledged_bytes >= window - 1000;
        },
        30000ms));
    wait_until([&] { fixture.pump(); return false; }, 300ms);
    REQUIRE(fixture.state().unacknowledged_bytes <= window);
    std::uint64_t held = 0;
    const stream already = fixture.sent();
    for (const auto& data : already.data)
        held += data.length;
    REQUIRE(held <= window);
    REQUIRE_EQ(fixture.state().acked_seq, std::uint64_t{0});

    // An acknowledgement lets more through, and everything arrives.
    REQUIRE(fixture.until_accounted(total, true));
    REQUIRE_EQ(fixture.sent().blob.size(), static_cast<std::size_t>(total));
    REQUIRE_EQ(fixture.state().lost_bytes, std::uint64_t{0});
}

TEST_CASE("plugin decoder: what a slow plugin makes the tap drop is told as "
          "lost, with the place and the offset")
{
    rig fixture;
    fixture.start_decoder();
    // 80 chunks of 60000 bytes are 4.8 MB: more than the window and the 4 MiB
    // of the tap.  Nothing is acknowledged while they come.
    constexpr std::uint64_t total = std::uint64_t{80} * 60000;
    for (int i = 0; i < 80; ++i)
        fixture.serial.inject_read(std::string(60000, 'z'));
    REQUIRE(wait_until(
        [&]
        {
            fixture.pump();
            return fixture.state().unacknowledged_bytes >= window - 60000;
        },
        30000ms));
    wait_until([&] { fixture.pump(); return false; }, 300ms);
    REQUIRE(fixture.state().unacknowledged_bytes <= window);

    // Now the plugin catches up.
    REQUIRE(fixture.until_accounted(total, true));
    const stream got = fixture.sent();
    REQUIRE(!got.lost.empty());
    REQUIRE(fixture.state().lost_bytes > 0);
    REQUIRE_EQ(fixture.state().lost_items, got.lost.size());
    // Data and losses cover every byte once, in order, and the offsets are
    // continuous across the gap.
    std::uint64_t offset = 0;
    std::uint64_t told_lost = 0;
    for (const auto& message : got.messages)
    {
        for (const auto& item : message.items)
        {
            if (const auto* data = std::get_if<decoder_data_item>(&item))
            {
                REQUIRE_EQ(data->offset, offset);
                offset += data->length;
            }
            else if (const auto* lost = std::get_if<decoder_lost_item>(&item))
            {
                REQUIRE_EQ(lost->place, std::string(port_name));
                REQUIRE(lost->direction == decoder_direction::rx);
                REQUIRE_EQ(lost->from_offset, offset);
                REQUIRE(lost->bytes >= 1);
                REQUIRE(lost->to_ns >= lost->from_ns);
                offset += lost->bytes;
                told_lost += lost->bytes;
            }
        }
    }
    REQUIRE_EQ(offset, total);
    REQUIRE_EQ(told_lost, fixture.state().lost_bytes);
}

TEST_CASE("plugin decoder: an acknowledgement that is not of something sent "
          "is refused")
{
    rig fixture;
    fixture.start_decoder();
    fixture.serial.inject_read("ab");
    REQUIRE(fixture.until_accounted(2, false));
    const std::uint64_t last = fixture.last_seq();
    fixture.ack(last + 5);
    REQUIRE_EQ(fixture.refused(), std::size_t{1});
    REQUIRE_EQ(fixture.state().acked_seq, std::uint64_t{0});
    fixture.ack(last);
    REQUIRE_EQ(fixture.state().acked_seq, last);
    REQUIRE_EQ(fixture.state().unacknowledged_bytes, std::size_t{0});
    // The same one again changes nothing.
    fixture.ack(last);
    REQUIRE_EQ(fixture.refused(), std::size_t{2});
}

// -- what the plugin finds ----------------------------------------------------

TEST_CASE("plugin decoder: frames and triggers go into the store")
{
    rig fixture;
    fixture.start_decoder();
    decoder_frame_message frame = frame_of();
    frame.fields.push_back({"unit", 0, 1, "17"});
    REQUIRE(fixture.handler->take(message_of(frame)));
    trigger_publish_message trigger = trigger_of();
    trigger.kind = trigger_kind::custom;
    trigger.code = "modbus.exception";
    REQUIRE(fixture.handler->take(message_of(trigger)));
    const auto view = fixture.store->snapshot();
    REQUIRE_EQ(view->frames->items.size(), std::size_t{1});
    const auto& stored = view->frames->items[0]->value;
    REQUIRE_EQ(stored.place, std::string(port_name));
    REQUIRE_EQ(stored.message, std::string("read"));
    REQUIRE_EQ(stored.fields.size(), std::size_t{1});
    REQUIRE_EQ(stored.fields[0].value, std::string("17"));
    REQUIRE_EQ(view->triggers->items.size(), std::size_t{1});
    REQUIRE(view->triggers->items[0]->value.kind ==
            decode_trigger_kind::custom);
    REQUIRE_EQ(view->triggers->items[0]->value.code,
               std::string("modbus.exception"));
    REQUIRE_EQ(fixture.refused(), std::size_t{0});
}

TEST_CASE("plugin decoder: markup is text, and a control character is "
          "refused before the store")
{
    rig fixture;
    fixture.start_decoder();
    decoder_frame_message frame = frame_of();
    frame.summary = "<b>bold</b> & %s {0} ${x}";
    REQUIRE(fixture.handler->take(message_of(frame)));
    const auto view = fixture.store->snapshot();
    REQUIRE_EQ(view->frames->items.size(), std::size_t{1});
    REQUIRE_EQ(view->frames->items[0]->value.summary,
               std::string("<b>bold</b> & %s {0} ${x}"));
    REQUIRE(!view->frames->items[0]->marks.control);
    REQUIRE_EQ(fixture.refused(), std::size_t{0});

    // The API refuses a control character in plain text; what it refuses
    // never reaches the store, and is counted.
    api_message hostile;
    hostile.op = "decoder.frame";
    hostile.body = vss::plugin::other_message{
        "{\"instance\":1,\"place\":\"port-a\",\"direction\":\"rx\","
        "\"offset\":0,\"length\":8,\"t_start_ns\":1,\"t_end_ns\":2,"
        "\"message\":\"read\",\"summary\":\"a\u001b[31m\"}"};
    REQUIRE(fixture.handler->take(hostile));
    REQUIRE_EQ(fixture.store->snapshot()->frames->items.size(),
               std::size_t{1});
}

TEST_CASE("plugin decoder: what the store's limits refuse is counted")
{
    rig fixture;
    fixture.start_decoder();
    decoder_frame_message frame = frame_of();
    frame.message = std::string(64, 'm');
    REQUIRE(fixture.handler->take(message_of(frame)));
    REQUIRE_EQ(fixture.refused(), std::size_t{0});
    // 200 triggers a second is the store's budget: the 201st in one second
    // is cut. The budget is counted in whole seconds of the real clock, so
    // a run that crosses a second boundary starts again at zero, and a slow
    // build may not reach 201 in a second at 205 sends. Keep sending until
    // one is refused; a bounded count and time stop it if none ever is.
    const auto give_up =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::size_t sent = 0;
    while (fixture.refused() == 0 && sent < 100000 &&
           std::chrono::steady_clock::now() < give_up)
    {
        REQUIRE(fixture.handler->take(message_of(trigger_of())));
        ++sent;
    }
    REQUIRE(fixture.refused() >= 1);
    REQUIRE(fixture.store->snapshot()->triggers->items.size() <= sent);
}

// -- the end ------------------------------------------------------------------

TEST_CASE("plugin decoder: an instance that stops is stopped and let go")
{
    rig fixture;
    fixture.start_decoder();
    fixture.serial.inject_read("x");
    REQUIRE(fixture.until_accounted(1, true));
    const std::size_t before = fixture.requests.size();
    // The owner no longer wants it.
    fixture.wanted.clear();
    fixture.instances->want({});
    fixture.settle();
    fixture.handler->refresh();
    REQUIRE_EQ(fixture.requests.size(), before + 1);
    REQUIRE(fixture.requests.back().message.op == decoder_stop_message::op());
    REQUIRE(!fixture.state().known);
    // The store says the decoding stopped.
    const auto view = fixture.store->snapshot();
    REQUIRE_EQ(view->triggers->items.size(), std::size_t{1});
    REQUIRE(view->triggers->items[0]->value.kind ==
            decode_trigger_kind::decoder_lost);
    // The tap is gone: more traffic is not sent.
    const std::size_t notes = fixture.notes.size();
    fixture.serial.inject_read("more");
    wait_until([&] { fixture.pump(); return false; }, 200ms);
    REQUIRE_EQ(fixture.notes.size(), notes);
}

TEST_CASE("plugin decoder: plugin_gone undoes everything")
{
    rig fixture;
    fixture.start_decoder({std::string(port_name), std::string(bus_name)});
    fixture.serial.inject_read("x");
    REQUIRE(fixture.until_accounted(1, true));
    const std::size_t requests = fixture.requests.size();
    fixture.handler->plugin_gone();
    // The plugin is gone, so it is not asked to stop.
    REQUIRE_EQ(fixture.requests.size(), requests);
    REQUIRE(!fixture.state().known);
    const auto view = fixture.store->snapshot();
    REQUIRE_EQ(view->triggers->items.size(), std::size_t{2});
    const std::size_t notes = fixture.notes.size();
    fixture.serial.inject_read("late");
    wait_until([&] { fixture.pump(); return false; }, 200ms);
    REQUIRE_EQ(fixture.notes.size(), notes);
    // What it sends after it is gone is refused.
    REQUIRE(fixture.handler->take(message_of(frame_of())));
    REQUIRE_EQ(fixture.store->snapshot()->frames->items.size(),
               std::size_t{0});
}

TEST_CASE("plugin decoder: a plugin that starts again starts from nothing")
{
    rig fixture;
    fixture.start_decoder();
    fixture.handler->plugin_gone();
    fixture.instances->plugin_gone();
    fixture.instances->plugin_ready();
    fixture.settle();
    fixture.handler->plugin_ready();
    REQUIRE_EQ(fixture.requests.size(), std::size_t{2});
    fixture.answer_request(1, ok_reply());
    fixture.serial.inject_read("again");
    REQUIRE(fixture.until_accounted(5, true));
    const stream got = fixture.sent();
    // Offsets begin again at 0 for the new run.
    REQUIRE_EQ(got.data.back().offset, std::uint64_t{0});
    REQUIRE_EQ(got.messages.back().seq, std::uint64_t{1});
}

TEST_CASE("plugin decoder: the handler may go while the plugin is reading")
{
    rig fixture;
    fixture.start_decoder();
    fixture.serial.inject_read("running");
    REQUIRE(fixture.until_accounted(7, false));
    // The answer of a request that is still out comes after the handler has
    // gone, and so does traffic.
    fixture.handler->refresh();
    fixture.handler.reset();
    fixture.serial.inject_read("after");
    wait_until([&] { fixture.pump(); return false; }, 200ms);
    // What was found stays in the store.
    REQUIRE_EQ(fixture.store->snapshot()->frames->items.size(),
               std::size_t{0});
}

TEST_CASE("plugin decoder: an answer that comes after the handler has gone "
          "does nothing")
{
    rig fixture;
    fixture.bound = true;
    fixture.places = {std::string(port_name)};
    fixture.run_instance(first_instance, extension_kind::decoder, "modbus");
    fixture.handler->refresh();
    REQUIRE_EQ(fixture.requests.size(), std::size_t{1});
    fixture.handler.reset();
    fixture.answer_request(0, ok_reply());
    fixture.pump();
    REQUIRE(fixture.sent().messages.empty());
}

TEST_CASE("plugin decoder: the store is read from other threads while the "
          "plugin writes")
{
    rig fixture;
    fixture.start_decoder();
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> read{0};
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int i = 0; i < 4; ++i)
    {
        readers.emplace_back(
            [&]
            {
                while (!done.load())
                {
                    const auto view = fixture.store->snapshot();
                    read.fetch_add(view->frames->items.size());
                }
            });
    }
    for (int i = 0; i < 200; ++i)
    {
        decoder_frame_message frame = frame_of();
        frame.offset = static_cast<std::uint64_t>(i);
        REQUIRE(fixture.handler->take(message_of(frame)));
    }
    done.store(true);
    for (auto& reader : readers)
        reader.join();
    REQUIRE_EQ(fixture.store->snapshot()->frames->items.size(),
               std::size_t{200});
}

namespace
{

/// A reference framer for a DLE protocol such as Jandy: a frame starts with
/// 0x10 0x02 and ends with 0x10 0x03, and 0x10 0x00 inside it stands for a
/// data 0x10. It is fed item by item, as a decoder reads the stream, and keeps
/// its state across items, so it holds whatever the chunking is.
struct dle_framer
{
    std::vector<std::string> frames;

    void feed(const std::string& chunk)
    {
        for (const char each : chunk)
            take(static_cast<unsigned char>(each));
    }

private:
    enum class mode
    {
        idle,
        idle_dle,
        body,
        body_dle
    };

    void take(unsigned char byte)
    {
        switch (mode_)
        {
        case mode::idle:
            mode_ = byte == 0x10 ? mode::idle_dle : mode::idle;
            break;
        case mode::idle_dle:
            if (byte == 0x02)
            {
                current_.clear();
                mode_ = mode::body;
            }
            else
                mode_ = byte == 0x10 ? mode::idle_dle : mode::idle;
            break;
        case mode::body:
            if (byte == 0x10)
                mode_ = mode::body_dle;
            else
                current_.push_back(static_cast<char>(byte));
            break;
        case mode::body_dle:
            if (byte == 0x03)
            {
                frames.push_back(current_);
                mode_ = mode::idle;
            }
            else
            {
                if (byte == 0x00)
                    current_.push_back(static_cast<char>(0x10));
                mode_ = mode::body;
            }
            break;
        }
    }

    mode mode_ = mode::idle;
    std::string current_;
};

/// Frames with noise between them and a data 0x10 inside one.
std::string jandy_like_traffic()
{
    std::string out;
    const auto frame = [&out](const std::string& body)
    {
        out += std::string("\x10\x02", 2);
        for (const char each : body)
        {
            out.push_back(each);
            if (each == '\x10')
                out.push_back('\0');
        }
        out += std::string("\x10\x03", 2);
    };
    out += "xx";
    frame(std::string("\x00\x01\x02", 3));
    frame(std::string("\x10\x20\x30", 3));
    out += std::string("\x10\x10", 2);
    frame("hello");
    frame("\x7f");
    return out;
}

/// The frames a reference framer finds in what the host sent for the bytes
/// written in these chunk sizes, and the bytes and offsets the host sent.
std::vector<std::string> frames_for(const std::string& traffic,
                                    const std::vector<std::size_t>& sizes)
{
    rig fixture;
    fixture.start_decoder();
    std::size_t at = 0;
    std::size_t step = 0;
    while (at < traffic.size())
    {
        const std::size_t size =
            std::min(sizes[step++ % sizes.size()], traffic.size() - at);
        fixture.serial.inject_read(traffic.substr(at, size));
        at += size;
        REQUIRE(fixture.until_accounted(at, true));
    }
    const stream got = fixture.sent();
    // The host never changes the bytes, and offsets add up whatever the
    // chunks were: a decoder can rely on both.
    REQUIRE_EQ(got.blob, traffic);
    std::uint64_t offset = 0;
    std::size_t blob_at = 0;
    dle_framer framer;
    for (const decoder_data_item& item : got.data)
    {
        REQUIRE_EQ(item.offset, offset);
        offset += item.length;
        framer.feed(got.blob.substr(blob_at, item.length));
        blob_at += item.length;
    }
    REQUIRE_EQ(offset, std::uint64_t{traffic.size()});
    return framer.frames;
}

} // namespace

TEST_CASE("plugin decoder: the same frames come out whether the bytes arrive "
          "one at a time, in random chunks or in one lump")
{
    const std::string traffic = jandy_like_traffic();
    const std::vector<std::string> expected = {
        std::string("\x00\x01\x02", 3), std::string("\x10\x20\x30", 3),
        std::string("hello"), std::string("\x7f")};

    const auto one_at_a_time = frames_for(traffic, {1});
    const auto lump = frames_for(traffic, {traffic.size()});
    REQUIRE_EQ(one_at_a_time, expected);
    REQUIRE_EQ(lump, expected);

    // Random sizes from 1 to 7, from a fixed seed, so a failure repeats.
    std::uint32_t state = 12345;
    for (int round = 0; round < 3; ++round)
    {
        std::vector<std::size_t> sizes;
        for (int index = 0; index < 16; ++index)
        {
            state = state * 1664525u + 1013904223u;
            sizes.push_back(1 + (state >> 16) % 7);
        }
        REQUIRE_EQ(frames_for(traffic, sizes), expected);
    }
}
