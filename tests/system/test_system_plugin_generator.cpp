//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The extension point `generator.*` (`make_generator_point`) on its own.  A
// test makes the handler with a hand-made `extension_context`: the link
// records what is answered, sent and asked, the executor is an `io_context`
// that the test runs by hand, the instances are a real `plugin_instances`
// whose plugin is played from memory, and the hub is a real one with mock
// ports, so that the bytes can be read back at the port.  Nothing here
// starts a process.
//

#include "support/test_support.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/api_generator.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/generator_feed.hpp>
#include <vss/plugin/host/generator_point.hpp>
#include <vss/plugin/host/instances.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/io_context.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using vss::plugin::api_errc;
using vss::plugin::api_message;
using vss::plugin::decode_as;
using vss::plugin::encode_as;
using vss::plugin::error_message;
using vss::plugin::extension_kind;
using vss::plugin::extension_point;
using vss::plugin::generator_credit_message;
using vss::plugin::generator_done_message;
using vss::plugin::generator_pattern_message;
using vss::plugin::generator_start_message;
using vss::plugin::generator_stop_message;
using vss::plugin::generator_write_message;
using vss::plugin::ok_message;
using vss::plugin::other_message;
using vss::plugin::permission;
using vss::plugin::host::extension_context;
using vss::plugin::host::feed_timing;
using vss::plugin::host::generator_owner;
using vss::plugin::host::generator_point;
using vss::plugin::host::instance_spec;
using vss::plugin::host::make_generator_point;
using vss::plugin::host::plugin_instances;
using vss::testkit::wait_until;
using vss_test_support::add_mock;
using vss_test_support::mock_endpoint;
using vss_test_support::start_hub;
using namespace std::chrono_literals;

namespace
{

constexpr std::string_view first_port = "port-a";
constexpr std::string_view second_port = "port-b";
constexpr std::uint64_t first_instance = 3;
constexpr std::uint64_t second_instance = 4;

/// What the handler answered.
struct answered
{
    std::uint64_t re = 0;
    api_message message{};
};

/// A request the handler sent to the plugin, and how to answer it.
struct asked
{
    api_message message{};
    vss::plugin::host::plugin_link::answer_handler handler;
    bool answered = false;
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

extension_point point_for(extension_kind kind, std::string_view id,
                          std::uint32_t most)
{
    extension_point point;
    point.kind = kind;
    point.id = std::string(id);
    point.max_instances = most;
    return point;
}

/// A generator `replay` that is "async", one called `pace` that is "rules",
/// and an adapter `recorder`.
std::vector<extension_point> declared_points()
{
    return {point_for(extension_kind::generator, "replay", 4),
            point_for(extension_kind::generator, "pace", 4),
            point_for(extension_kind::adapter, "recorder", 4)};
}

instance_spec spec_for(std::uint64_t number, extension_kind kind,
                       std::string_view type, std::string_view params)
{
    instance_spec spec;
    spec.instance = number;
    spec.kind = kind;
    spec.type = std::string(type);
    spec.params = std::string(params);
    return spec;
}

instance_spec replay_on(std::uint64_t number, std::string_view port)
{
    std::string params = "{\"port\":\"";
    params.append(port);
    params.append("\"}");
    return spec_for(number, extension_kind::generator, "replay", params);
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

api_message raw_message(std::string_view op, std::uint64_t id,
                        std::string_view header)
{
    api_message message;
    message.op = std::string(op);
    message.id = id;
    message.body = other_message{std::string(header)};
    return message;
}

generator_write_message write_of(std::uint64_t instance, std::size_t size,
                                 std::uint8_t fill = 0x61)
{
    generator_write_message body;
    body.instance = instance;
    body.data.assign(size, static_cast<vss::byte>(fill));
    return body;
}

/// The handler with everything around it.  The order of the members is the
/// order they are made in, and the handler goes last: it is destroyed first.
struct rig
{
    explicit rig(std::vector<permission> granted = {permission::ports_write})
        : hub(start_hub()),
          first(add_mock(*hub, std::string(first_port))),
          second(add_mock(*hub, std::string(second_port))),
          instances(std::make_shared<plugin_instances>(
              declared_points(),
              [this](api_message message,
                     plugin_instances::answer_handler on_answer)
              {
                  creates.push_back({std::move(message), std::move(on_answer)});
              }))
    {
        extension_context context;
        context.plugin_id = "com.example.generator";
        context.plugin_name = "Generator plugin";
        context.plugin_version = "1.0.0";
        context.granted = std::move(granted);
        context.instances = instances;
        context.executor = io.get_executor();
        context.link.answer = [this](std::uint64_t re, api_message message)
        { answers.push_back(answered{re, std::move(message)}); };
        context.link.notify = [this](api_message message)
        { notified.push_back(std::move(message)); };
        context.link.request =
            [this](api_message message,
                   vss::plugin::host::plugin_link::answer_handler on_answer,
                   std::chrono::milliseconds)
        { requests.push_back(asked{std::move(message), std::move(on_answer)}); };

        generator_owner owner;
        owner.find_port = [this](std::string_view name)
            -> std::shared_ptr<vss::endpoint>
        {
            if (name == first_port)
                return first.endpoint;
            if (name == second_port)
                return second.endpoint;
            return nullptr;
        };
        owner.timing_of = [](std::string_view type)
        { return type == "pace" ? feed_timing::rules : feed_timing::async; };
        handler = make_generator_point(context, std::move(owner));
    }

    rig(const rig&) = delete;
    rig& operator=(const rig&) = delete;

    void pump()
    {
        io.poll();
        io.restart();
    }

    /// The plugin has these instances, which it created as asked.
    void have(std::vector<instance_spec> wanted)
    {
        instances->want(std::move(wanted));
        instances->plugin_ready();
        for (std::size_t index = 0; index < creates.size(); ++index)
        {
            auto handler_copy = creates[index].handler;
            if (handler_copy)
            {
                creates[index].handler = nullptr;
                handler_copy(vss::error_code{}, ok_reply());
            }
        }
    }

    /// Offers a message the handler must take, and returns its answer when
    /// it is a request.
    answered ask(const api_message& message)
    {
        const std::size_t before = answers.size();
        REQUIRE(handler->take(message));
        REQUIRE_EQ(answers.size(), before + 1);
        return answers.back();
    }

    /// Whether the answer is the error with this code.
    static bool is_error(const answered& reply, api_errc code)
    {
        const auto* body = std::get_if<error_message>(&reply.message.body);
        return body != nullptr && body->value == to_string(code);
    }

    /// The requests of this op the handler sent.
    std::vector<std::size_t> sent(std::string_view op) const
    {
        std::vector<std::size_t> found;
        for (std::size_t index = 0; index < requests.size(); ++index)
        {
            if (requests[index].message.op == op)
                found.push_back(index);
        }
        return found;
    }

    /// Starts the generators and answers `generator.start` for each.
    void start_all()
    {
        handler->sync();
        for (asked& item : requests)
        {
            if (item.message.op == generator_start_message::op() &&
                !item.answered)
            {
                item.answered = true;
                item.handler(vss::error_code{}, ok_reply());
            }
        }
    }

    /// The credit given to an instance so far, in bytes.
    std::uint64_t credit_of(std::uint64_t instance) const
    {
        std::uint64_t total = 0;
        for (const api_message& message : notified)
        {
            const auto credit = decode_as<generator_credit_message>(message);
            if (credit && credit->instance == instance)
                total += credit->bytes;
        }
        return total;
    }

    /// Runs the strand until the port holds at least `size` bytes.
    bool arrived(const mock_endpoint& port, std::size_t size)
    {
        return wait_until(
            [&]
            {
                pump();
                return port.written().size() >= size;
            },
            20000ms);
    }

    /// Before the hub, so that it outlives what the hub's endpoints post.
    boost::asio::io_context io{};
    std::shared_ptr<vss::hub> hub;
    mock_endpoint first;
    mock_endpoint second;
    std::vector<asked> creates{};
    std::shared_ptr<plugin_instances> instances;
    std::vector<answered> answers{};
    std::vector<api_message> notified{};
    std::vector<asked> requests{};
    std::unique_ptr<generator_point> handler{};
};

} // namespace

TEST_CASE("generator point: a running instance is started on its port")
{
    rig test;
    test.have({replay_on(first_instance, first_port)});
    REQUIRE(test.requests.empty());
    test.handler->sync();
    const auto starts = test.sent(generator_start_message::op());
    REQUIRE_EQ(starts.size(), std::size_t{1});
    const auto body = decode_as<generator_start_message>(
        test.requests[starts[0]].message);
    REQUIRE(body.has_value());
    REQUIRE_EQ(body->instance, first_instance);
    REQUIRE_EQ(body->place, std::string(first_port));
    REQUIRE_EQ(body->params, std::string("{\"port\":\"port-a\"}"));
    // No credit before the plugin has answered.
    REQUIRE_EQ(test.credit_of(first_instance), std::uint64_t{0});
    // Syncing again starts nothing twice.
    test.handler->sync();
    REQUIRE_EQ(test.sent(generator_start_message::op()).size(),
               std::size_t{1});

    test.requests[starts[0]].handler(vss::error_code{}, ok_reply());
    REQUIRE_EQ(test.credit_of(first_instance), std::uint64_t{65536});
    REQUIRE_EQ(test.handler->counts().started, std::uint64_t{1});
}

TEST_CASE("generator point: bytes the plugin writes reach the port")
{
    rig test;
    test.have({replay_on(first_instance, first_port)});
    test.start_all();
    const std::uint64_t first_credit = test.credit_of(first_instance);

    REQUIRE(test.handler->take(message_of(write_of(first_instance, 10, 0x41))));
    REQUIRE(test.handler->take(message_of(write_of(first_instance, 5, 0x42))));
    REQUIRE(test.arrived(test.first, 15));
    REQUIRE_EQ(test.first.written(), std::string("AAAAAAAAAABBBBB"));
    REQUIRE_EQ(test.second.written().size(), std::size_t{0});
    // Credit goes back as the bytes are taken.
    REQUIRE_EQ(test.credit_of(first_instance), first_credit + 15);
    REQUIRE_EQ(test.handler->counts().bytes_written, std::uint64_t{15});
    REQUIRE(test.answers.empty());
}

TEST_CASE("generator point: a start that fails leaves nothing running")
{
    {
        // The plugin refuses `generator.start`.
        rig test;
        test.have({replay_on(first_instance, first_port)});
        test.handler->sync();
        const auto starts = test.sent(generator_start_message::op());
        REQUIRE_EQ(starts.size(), std::size_t{1});
        test.requests[starts[0]].handler(vss::error_code{}, error_reply());
        REQUIRE_EQ(test.handler->counts().start_failed, std::uint64_t{1});
        REQUIRE_EQ(test.credit_of(first_instance), std::uint64_t{0});
        test.handler->take(message_of(write_of(first_instance, 4)));
        REQUIRE_EQ(test.handler->counts().writes_dropped, std::uint64_t{1});
        test.handler->sync();
        REQUIRE_EQ(test.sent(generator_start_message::op()).size(),
                   std::size_t{1});
        test.pump();
        REQUIRE_EQ(test.first.written().size(), std::size_t{0});
    }
    {
        // A port that the owner does not have, and no port at all.
        rig test;
        test.have({replay_on(first_instance, "no-such-port"),
                   spec_for(second_instance, extension_kind::generator,
                            "replay", "{}")});
        test.handler->sync();
        test.handler->sync();
        REQUIRE(test.sent(generator_start_message::op()).empty());
        REQUIRE_EQ(test.handler->counts().start_failed, std::uint64_t{2});
    }
    {
        // The plugin does not answer in time.
        rig test;
        test.have({replay_on(first_instance, first_port)});
        test.handler->sync();
        const auto starts = test.sent(generator_start_message::op());
        REQUIRE_EQ(starts.size(), std::size_t{1});
        test.requests[starts[0]].handler(
            make_error_code(api_errc::timeout), api_message{});
        REQUIRE_EQ(test.handler->counts().start_failed, std::uint64_t{1});
    }
}

TEST_CASE("generator point: each refusal has its answer")
{
    rig test;
    // A generator that is running, one that is only wanted, an adapter that
    // is running, and a rules generator.
    test.have({replay_on(first_instance, first_port),
               spec_for(5, extension_kind::adapter, "recorder", "{}"),
               spec_for(6, extension_kind::generator, "pace",
                        "{\"port\":\"port-b\"}")});
    test.start_all();

    generator_pattern_message pattern;
    pattern.data = {vss::byte{1}, vss::byte{2}};

    // Not a running instance, and not a generator.
    pattern.instance = 99;
    REQUIRE(rig::is_error(test.ask(message_of(pattern, 7)), api_errc::refused));
    pattern.instance = 5;
    REQUIRE(rig::is_error(test.ask(message_of(pattern, 8)), api_errc::refused));

    // A running generator that did not declare "rules".
    pattern.instance = first_instance;
    const answered plain = test.ask(message_of(pattern, 9));
    REQUIRE(rig::is_error(plain, api_errc::not_declared));
    REQUIRE_EQ(plain.re, std::uint64_t{9});

    // One that did: this host cannot make patterns yet.
    pattern.instance = 6;
    REQUIRE(
        rig::is_error(test.ask(message_of(pattern, 10)), api_errc::unsupported));

    // A request that is not as the API says.
    REQUIRE(rig::is_error(
        test.ask(raw_message(generator_pattern_message::op(), 11,
                             "{\"op\":\"generator.pattern\",\"id\":11}")),
        api_errc::bad_request));

    // The host's own messages are not a plugin's to send.
    REQUIRE(rig::is_error(
        test.ask(raw_message(generator_stop_message::op(), 12,
                             "{\"op\":\"generator.stop\",\"id\":12,"
                             "\"instance\":3}")),
        api_errc::bad_request));
    REQUIRE(rig::is_error(
        test.ask(raw_message(generator_start_message::op(), 13,
                             "{\"op\":\"generator.start\",\"id\":13,"
                             "\"instance\":3,\"place\":\"port-a\"}")),
        api_errc::bad_request));
    REQUIRE_EQ(test.handler->counts().wrong_side, std::uint64_t{2});

    // An op that is not a generator's is not taken.
    REQUIRE(!test.handler->take(raw_message(
        "generator.nothing", 14, "{\"op\":\"generator.nothing\",\"id\":14}")));
    REQUIRE(!test.handler->take(
        raw_message("ping", 15, "{\"op\":\"ping\",\"id\":15}")));
}

TEST_CASE("generator point: without the permission every request is refused")
{
    rig test(std::vector<permission>{});
    test.have({replay_on(first_instance, first_port)});
    test.start_all();
    // A plugin without the permission has no generator started for it.
    REQUIRE(test.sent(generator_start_message::op()).empty());
    generator_pattern_message pattern;
    pattern.instance = first_instance;
    pattern.data = {vss::byte{1}};
    REQUIRE(rig::is_error(test.ask(message_of(pattern, 7)),
                          api_errc::not_granted));
    // A write is a notification: nothing is answered, nothing is written.
    test.handler->take(message_of(write_of(first_instance, 4)));
    test.handler->take(message_of(generator_done_message{first_instance, "x"}));
    test.pump();
    REQUIRE_EQ(test.first.written().size(), std::size_t{0});
    REQUIRE_EQ(test.handler->counts().writes_dropped, std::uint64_t{2});
    REQUIRE(test.answers.size() == 1);
}

TEST_CASE("generator point: a write for an instance that is not running")
{
    rig test;
    test.have({replay_on(first_instance, first_port),
               spec_for(5, extension_kind::adapter, "recorder", "{}")});
    test.start_all();
    test.handler->take(message_of(write_of(99, 4)));
    test.handler->take(message_of(write_of(5, 4)));
    // A write that is not as the API says.
    test.handler->take(raw_message(generator_write_message::op(), 0,
                                   "{\"op\":\"generator.write\"}"));
    REQUIRE_EQ(test.handler->counts().writes_dropped, std::uint64_t{3});
    test.pump();
    REQUIRE_EQ(test.first.written().size(), std::size_t{0});
    REQUIRE(test.answers.empty());
}

TEST_CASE("generator point: writes the feed refuses end the generator")
{
    rig test;
    // A rules generator takes no plain write: every one is refused.
    test.have({spec_for(6, extension_kind::generator, "pace",
                        "{\"port\":\"port-b\"}")});
    test.start_all();
    for (int index = 0; index < 2; ++index)
        test.handler->take(message_of(write_of(6, 4)));
    REQUIRE_EQ(test.handler->counts().writes_refused, std::uint64_t{2});
    REQUIRE(test.sent(generator_stop_message::op()).empty());
    test.handler->take(message_of(write_of(6, 4)));
    REQUIRE_EQ(test.handler->counts().writes_refused, std::uint64_t{3});
    REQUIRE_EQ(test.handler->counts().misbehaved, std::uint64_t{1});
    REQUIRE_EQ(test.sent(generator_stop_message::op()).size(), std::size_t{1});
    // It is over: more is dropped and never reaches the port.
    test.handler->take(message_of(write_of(6, 4)));
    REQUIRE_EQ(test.handler->counts().writes_dropped, std::uint64_t{1});
    test.pump();
    REQUIRE_EQ(test.second.written().size(), std::size_t{0});
}

TEST_CASE("generator point: a port that is full holds the credit back")
{
    rig test;
    test.have({replay_on(first_instance, first_port)});
    test.start_all();
    const std::uint64_t window = test.credit_of(first_instance);
    REQUIRE_EQ(window, std::uint64_t{65536});

    // The first write fills what the endpoint may hold.  The strand is not
    // run, so the endpoint's result is not seen yet.
    test.handler->take(message_of(write_of(first_instance, 65536, 0x31)));
    REQUIRE_EQ(test.credit_of(first_instance), window + 65536);
    // A second is taken by the feed but held: no credit comes back for it.
    test.handler->take(message_of(write_of(first_instance, 1000, 0x32)));
    REQUIRE_EQ(test.credit_of(first_instance), window + 65536);
    // When the endpoint has finished with bytes, the rest is written and the
    // credit follows.
    REQUIRE(test.arrived(test.first, 66536));
    REQUIRE(wait_until(
        [&]
        {
            test.pump();
            return test.credit_of(first_instance) == window + 66536;
        },
        20000ms));
    const std::string got = test.first.written();
    REQUIRE_EQ(got.size(), std::size_t{66536});
    REQUIRE_EQ(got.substr(65536), std::string(1000, '2'));
    REQUIRE_EQ(test.handler->counts().bytes_written, std::uint64_t{66536});
}

TEST_CASE("generator point: done writes what is held and ends")
{
    rig test;
    test.have({replay_on(first_instance, first_port)});
    test.start_all();
    test.handler->take(message_of(write_of(first_instance, 65536, 0x31)));
    test.handler->take(message_of(write_of(first_instance, 10, 0x32)));
    test.handler->take(
        message_of(generator_done_message{first_instance, "end"}));
    // After done nothing more is taken.
    test.handler->take(message_of(write_of(first_instance, 10, 0x33)));
    REQUIRE_EQ(test.handler->counts().writes_dropped + test.handler->counts()
                   .writes_refused,
               std::uint64_t{1});
    REQUIRE(test.arrived(test.first, 65546));
    test.pump();
    REQUIRE_EQ(test.first.written().size(), std::size_t{65546});
    // It ended because the plugin said so: the host tells it nothing.
    REQUIRE(test.sent(generator_stop_message::op()).empty());
}

TEST_CASE("generator point: an instance that is removed is stopped")
{
    rig test;
    test.have({replay_on(first_instance, first_port)});
    test.start_all();
    test.handler->take(message_of(write_of(first_instance, 4, 0x41)));
    REQUIRE(test.arrived(test.first, 4));

    // The owner no longer wants it.  The instance is stopped, and so is its
    // generator.
    test.instances->want({});
    test.handler->sync();
    const auto stops = test.sent(generator_stop_message::op());
    REQUIRE_EQ(stops.size(), std::size_t{1});
    const auto body =
        decode_as<generator_stop_message>(test.requests[stops[0]].message);
    REQUIRE(body.has_value());
    REQUIRE_EQ(body->instance, first_instance);
    test.handler->take(message_of(write_of(first_instance, 4, 0x42)));
    test.pump();
    REQUIRE_EQ(test.first.written(), std::string("AAAA"));
    REQUIRE_EQ(test.handler->counts().writes_dropped, std::uint64_t{1});
    // Syncing again does not stop it twice.
    test.handler->sync();
    REQUIRE_EQ(test.sent(generator_stop_message::op()).size(), std::size_t{1});
}

TEST_CASE("generator point: two instances write to their own ports")
{
    rig test;
    test.have({replay_on(first_instance, first_port),
               replay_on(second_instance, second_port)});
    test.start_all();
    REQUIRE_EQ(test.sent(generator_start_message::op()).size(), std::size_t{2});
    test.handler->take(message_of(write_of(first_instance, 3, 0x61)));
    test.handler->take(message_of(write_of(second_instance, 4, 0x62)));
    test.handler->take(message_of(write_of(first_instance, 2, 0x63)));
    REQUIRE(test.arrived(test.first, 5));
    REQUIRE(test.arrived(test.second, 4));
    REQUIRE_EQ(test.first.written(), std::string("aaacc"));
    REQUIRE_EQ(test.second.written(), std::string("bbbb"));
    // Each has its own credit.
    REQUIRE(test.credit_of(first_instance) >= 65536 + 5);
    REQUIRE(test.credit_of(second_instance) >= 65536 + 4);

    // One ends and the other goes on.
    test.handler->take(message_of(generator_done_message{first_instance, "x"}));
    test.handler->take(message_of(write_of(second_instance, 1, 0x62)));
    REQUIRE(test.arrived(test.second, 5));
}

TEST_CASE("generator point: when the plugin goes everything stops")
{
    rig test;
    test.have({replay_on(first_instance, first_port)});
    test.start_all();
    test.handler->take(message_of(write_of(first_instance, 65536, 0x31)));
    test.handler->take(message_of(write_of(first_instance, 100, 0x32)));
    const std::size_t sent_before = test.requests.size();
    const std::size_t notified_before = test.notified.size();

    test.handler->plugin_gone();
    test.instances->plugin_gone();
    // What the endpoint finishes after that is not seen, and nothing is sent
    // to a plugin that is not there.
    test.pump();
    test.handler->take(message_of(write_of(first_instance, 4, 0x33)));
    REQUIRE_EQ(test.requests.size(), sent_before);
    REQUIRE_EQ(test.notified.size(), notified_before);
    REQUIRE_EQ(test.handler->counts().writes_dropped, std::uint64_t{1});

    // The plugin starts again, from nothing: the generator is started again.
    test.creates.clear();
    test.instances->plugin_ready();
    for (asked& item : test.creates)
    {
        if (item.handler)
            item.handler(vss::error_code{}, ok_reply());
    }
    test.start_all();
    REQUIRE_EQ(test.sent(generator_start_message::op()).size(), std::size_t{2});
    const std::uint64_t before = test.credit_of(first_instance);
    test.handler->take(message_of(write_of(first_instance, 4, 0x34)));
    REQUIRE(test.credit_of(first_instance) == before + 4);
    REQUIRE(test.arrived(test.first, 4));
}

TEST_CASE("generator point: it can be destroyed while generators run")
{
    rig test;
    test.have({replay_on(first_instance, first_port),
               replay_on(second_instance, second_port)});
    test.start_all();
    test.handler->take(message_of(write_of(first_instance, 65536, 0x31)));
    test.handler->take(message_of(write_of(first_instance, 100, 0x32)));
    test.handler->take(message_of(write_of(second_instance, 100, 0x32)));
    const std::size_t notified_before = test.notified.size();
    test.handler.reset();
    // Work that was posted for it runs and finds nothing.
    test.pump();
    test.pump();
    REQUIRE_EQ(test.notified.size(), notified_before);
}
