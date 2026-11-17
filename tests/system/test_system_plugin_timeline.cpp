//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The extension point `timeline.load` (`make_timeline_point`) on its own.  A
// test makes the handler with a hand-made `extension_context`, as the
// generator test does, writes a recording into a scratch folder with
// `recording_writer`, and reads the replayed bytes back at a mock port of a
// real hub.  Nothing here starts a process.
//

#include "support/test_support.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/api_capture.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/generator_point.hpp>
#include <vss/plugin/host/instances.hpp>
#include <vss/plugin/host/recording.hpp>
#include <vss/plugin/host/timeline_point.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using vss::plugin::api_errc;
using vss::plugin::api_message;
using vss::plugin::encode_as;
using vss::plugin::error_message;
using vss::plugin::extension_kind;
using vss::plugin::extension_point;
using vss::plugin::ok_message;
using vss::plugin::permission;
using vss::plugin::timeline_load_message;
using vss::plugin::host::extension_context;
using vss::plugin::host::instance_spec;
using vss::plugin::host::make_timeline_point;
using vss::plugin::host::plugin_instances;
using vss::plugin::host::recording_options;
using vss::plugin::host::recording_writer;
using vss::plugin::host::timeline_point;
using vss::testkit::wait_until;
using vss_test_support::add_mock;
using vss_test_support::mock_endpoint;
using vss_test_support::scratch_directory;
using vss_test_support::start_hub;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;

constexpr std::string_view port_name = "port-a";
constexpr std::uint64_t instance_number = 3;

api_message ok_reply()
{
    api_message reply;
    reply.body = ok_message{};
    reply.op = std::string(vss::plugin::op_of(reply.body));
    return reply;
}

extension_point point_for(extension_kind kind, std::string_view id)
{
    extension_point point;
    point.kind = kind;
    point.id = std::string(id);
    point.max_instances = 4;
    return point;
}

/// Writes a recording of one place named `place`: `rx` chunks at the given
/// times, and one `tx` chunk that a replay must not play.
fs::path write_recording(const fs::path& folder, std::string place,
                         const std::vector<std::uint64_t>& times,
                         const std::vector<std::string>& chunks)
{
    recording_options options;
    options.origin_utc = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    vss::plugin::host::place_spec spec;
    spec.name = std::move(place);
    options.places.push_back(std::move(spec));
    options.made_by.plugin = "com.example.replay";
    options.made_by.version = "1.0.0";
    std::string detail;
    auto writer = recording_writer::open(folder, std::move(options), &detail);
    REQUIRE(writer.has_value());
    for (std::size_t i = 0; i < times.size(); ++i)
    {
        const vss::byte_buffer bytes(chunks[i].begin(), chunks[i].end());
        REQUIRE(writer->add_data(0, times[i], vss::data_direction::rx, bytes));
    }
    const std::string sent = "not played";
    const vss::byte_buffer out(sent.begin(), sent.end());
    REQUIRE(writer->add_data(0, times.empty() ? 0 : times.back(),
                             vss::data_direction::tx, out));
    REQUIRE(writer->finish().has_value());
    return folder;
}

api_message load_of(std::string recording, std::string place, double scale,
                    std::uint64_t instance = instance_number,
                    std::uint64_t id = 1)
{
    timeline_load_message body;
    body.instance = instance;
    body.recording = std::move(recording);
    body.place = std::move(place);
    body.time_scale = scale;
    auto encoded = encode_as(body, id);
    REQUIRE(encoded.has_value());
    return std::move(encoded).value();
}

struct answered
{
    std::uint64_t re = 0;
    api_message message{};
};

struct rig
{
    explicit rig(std::vector<permission> granted = {permission::ports_write})
        : hub(start_hub()), first(add_mock(*hub, std::string(port_name))),
          data(scratch.path() / "data"),
          instances(std::make_shared<plugin_instances>(
              std::vector<extension_point>{
                  point_for(extension_kind::generator, "replay"),
                  point_for(extension_kind::adapter, "other")},
              [this](api_message message,
                     plugin_instances::answer_handler on_answer)
              {
                  creates.push_back({std::move(message), std::move(on_answer)});
              }))
    {
        fs::create_directories(data);
        extension_context context;
        context.plugin_id = "com.example.replay";
        context.plugin_name = "Replay";
        context.plugin_version = "1.0.0";
        context.granted = std::move(granted);
        context.data_directory = data;
        context.instances = instances;
        context.executor = io.get_executor();
        context.link.answer = [this](std::uint64_t re, api_message message)
        { answers.push_back(answered{re, std::move(message)}); };
        context.link.notify = [](api_message) {};
        context.link.request = [](api_message,
                                  vss::plugin::host::plugin_link::answer_handler,
                                  std::chrono::milliseconds) {};
        handler = make_timeline_point(
            context,
            [this](std::string_view name) -> std::shared_ptr<vss::endpoint>
            { return name == port_name ? first.endpoint : nullptr; },
            slow.get_executor());
    }

    rig(const rig&) = delete;
    rig& operator=(const rig&) = delete;

    ~rig()
    {
        handler.reset();
        slow.join();
    }

    void pump()
    {
        io.poll();
        io.restart();
    }

    void have(instance_spec spec)
    {
        instances->want({std::move(spec)});
        instances->plugin_ready();
        for (auto& item : creates)
        {
            if (item.handler)
            {
                auto copy = std::move(item.handler);
                item.handler = nullptr;
                copy(vss::error_code{}, ok_reply());
            }
        }
    }

    static instance_spec replay_instance()
    {
        instance_spec spec;
        spec.instance = instance_number;
        spec.kind = extension_kind::generator;
        spec.type = "replay";
        spec.params = "{}";
        return spec;
    }

    /// Offers the request and runs the strand until it is answered.
    answered ask(const api_message& message)
    {
        const std::size_t before = answers.size();
        REQUIRE(handler->take(message));
        REQUIRE(wait_until(
            [&]
            {
                pump();
                return answers.size() > before;
            },
            20000ms));
        return answers.back();
    }

    static bool is_ok(const answered& reply)
    {
        return std::holds_alternative<ok_message>(reply.message.body);
    }

    static bool is_error(const answered& reply, api_errc code)
    {
        const auto* body = std::get_if<error_message>(&reply.message.body);
        return body != nullptr && body->value == to_string(code);
    }

    bool arrived(std::size_t size)
    {
        return wait_until(
            [&]
            {
                pump();
                return first.written().size() >= size;
            },
            20000ms);
    }

    boost::asio::io_context io{};
    scratch_directory scratch{};
    std::shared_ptr<vss::hub> hub;
    mock_endpoint first;
    fs::path data;
    struct created
    {
        api_message message{};
        plugin_instances::answer_handler handler;
    };
    std::vector<created> creates{};
    std::shared_ptr<plugin_instances> instances;
    std::vector<answered> answers{};
    boost::asio::thread_pool slow{1};
    std::unique_ptr<timeline_point> handler;
};

} // namespace

TEST_CASE("timeline.load plays the received bytes of a recording onto a port")
{
    rig r;
    write_recording(r.data / "recordings" / "one", std::string(port_name),
                    {0, 1'000'000, 2'000'000}, {"abc", "de", "f"});
    r.have(rig::replay_instance());
    const auto reply = r.ask(load_of("recordings/one", std::string(port_name),
                                     1.0));
    REQUIRE(rig::is_ok(reply));
    REQUIRE(r.arrived(6));
    const auto got = r.first.written();
    REQUIRE_EQ(std::string(got.begin(), got.end()), std::string("abcdef"));
    REQUIRE(wait_until(
        [&]
        {
            r.pump();
            return r.handler->counts().finished == 1;
        },
        20000ms));
    REQUIRE_EQ(r.handler->counts().chunks, std::uint64_t{3});
    REQUIRE_EQ(r.handler->counts().bytes, std::uint64_t{6});
    REQUIRE_EQ(r.handler->playing(), std::size_t{0});
}

TEST_CASE("timeline.load never writes a chunk before its time, and scales it")
{
    rig r;
    write_recording(r.data / "recordings" / "slow", std::string(port_name),
                    {0, 100'000'000}, {"a", "b"});
    r.have(rig::replay_instance());
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(rig::is_ok(
        r.ask(load_of("recordings/slow", std::string(port_name), 2.0))));
    REQUIRE(r.arrived(2));
    const auto took = std::chrono::steady_clock::now() - start;
    // 100 ms recorded, at scale 2: 200 ms.
    REQUIRE(took >= 190ms);
}

TEST_CASE("timeline.load refuses what it cannot play")
{
    rig r;
    write_recording(r.data / "recordings" / "one", std::string(port_name), {0},
                    {"a"});
    r.have(rig::replay_instance());

    // A recording that is not there.
    REQUIRE(rig::is_error(
        r.ask(load_of("recordings/none", std::string(port_name), 1.0)),
        api_errc::refused));
    // A port the owner does not have.
    REQUIRE(rig::is_error(r.ask(load_of("recordings/one", "nothing", 1.0)),
                          api_errc::refused));
    // An instance the plugin does not hold.
    REQUIRE(rig::is_error(
        r.ask(load_of("recordings/one", std::string(port_name), 1.0, 99)),
        api_errc::refused));
    REQUIRE_EQ(r.handler->counts().started, std::uint64_t{0});
}

TEST_CASE("timeline.load needs the permission to write to ports")
{
    rig r{std::vector<permission>{}};
    write_recording(r.data / "recordings" / "one", std::string(port_name), {0},
                    {"a"});
    r.have(rig::replay_instance());
    REQUIRE(rig::is_error(
        r.ask(load_of("recordings/one", std::string(port_name), 1.0)),
        api_errc::not_granted));
}

TEST_CASE("timeline.load answers busy for an instance that is playing")
{
    rig r;
    write_recording(r.data / "recordings" / "long", std::string(port_name),
                    {0, 5'000'000'000}, {"a", "b"});
    r.have(rig::replay_instance());
    REQUIRE(rig::is_ok(
        r.ask(load_of("recordings/long", std::string(port_name), 1.0))));
    REQUIRE_EQ(r.handler->playing(), std::size_t{1});
    REQUIRE(rig::is_error(
        r.ask(load_of("recordings/long", std::string(port_name), 1.0, 3, 2)),
        api_errc::busy));
    r.handler->plugin_gone();
    REQUIRE_EQ(r.handler->playing(), std::size_t{0});
}

TEST_CASE("a replay keeps the chunk boundaries and times of the recording")
{
    // Whole frames in one chunk (as a network adapter delivers them: one
    // frame, then two frames together) followed by one frame that a serial
    // port delivered a byte at a time, 10 ms apart.
    const std::vector<std::string> chunks{
        std::string("\x01\x03\x00\x02\xAA\xBB", 6),
        std::string("\x01\x03\x00\x02\xAA\xBB\x01\x03\x00\x02\xCC\xDD", 12),
        "\x11", "\x03", std::string(1, '\0'), "\x02", "\xEE", "\xFF"};
    const std::vector<std::uint64_t> times{0,          5'000'000,  20'000'000,
                                           30'000'000, 40'000'000, 50'000'000,
                                           60'000'000, 70'000'000};
    rig r;
    const fs::path folder = write_recording(
        r.data / "recordings" / "mixed", std::string(port_name), times, chunks);

    // The recording itself holds one line for each chunk, with its time.
    std::string detail;
    const auto read = vss::plugin::host::read_recording(folder, &detail);
    REQUIRE(read.has_value());
    auto place = read->open_place(0);
    REQUIRE(place.has_value());
    std::size_t index = 0;
    for (;;)
    {
        auto next = place->next();
        REQUIRE(next.has_value());
        if (!next->has_value())
            break;
        const auto& event = **next;
        if (event.direction != vss::data_direction::rx)
            continue;
        REQUIRE(index < chunks.size());
        REQUIRE_EQ(event.t_ns, times[index]);
        REQUIRE_EQ(event.bytes.size(), chunks[index].size());
        ++index;
    }
    REQUIRE_EQ(index, chunks.size());

    // The replay hands the port one write for each chunk, none merged or
    // split, and the bytes arrive in order.
    r.have(rig::replay_instance());
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(rig::is_ok(
        r.ask(load_of("recordings/mixed", std::string(port_name), 1.0))));
    std::string all;
    for (const std::string& chunk : chunks)
        all += chunk;
    REQUIRE(r.arrived(all.size()));
    const auto took = std::chrono::steady_clock::now() - start;
    REQUIRE(wait_until(
        [&]
        {
            r.pump();
            return r.handler->counts().finished == 1;
        },
        20000ms));
    REQUIRE_EQ(r.handler->counts().chunks, std::uint64_t{chunks.size()});
    REQUIRE_EQ(r.handler->counts().bytes, std::uint64_t{all.size()});
    REQUIRE_EQ(r.first.written(), all);
    // Never early: the last chunk was recorded at 70 ms.
    REQUIRE(took >= 65ms);
}

TEST_CASE("a replay is read as it plays, so a recording larger than the read "
          "ahead is played whole")
{
    rig r;
    const fs::path folder = r.data / "recordings" / "big";
    recording_options options;
    options.origin_utc = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    vss::plugin::host::place_spec spec;
    spec.name = std::string(port_name);
    options.places.push_back(std::move(spec));
    options.made_by.plugin = "com.example.replay";
    options.made_by.version = "1.0.0";
    auto writer = recording_writer::open(folder, std::move(options));
    REQUIRE(writer.has_value());
    // 3000 chunks of 1000 bytes: nearly 3 MiB, more than two reads of the
    // read ahead, the bytes of each chunk telling which chunk it is.
    std::string all;
    for (std::size_t i = 0; i < 3000; ++i)
    {
        const std::string chunk(1000, static_cast<char>('a' + i % 26));
        all += chunk;
        const vss::byte_buffer bytes(chunk.begin(), chunk.end());
        REQUIRE(writer->add_data(0, i * 1000, vss::data_direction::rx, bytes));
    }
    REQUIRE(writer->finish().has_value());
    r.have(rig::replay_instance());
    REQUIRE(rig::is_ok(
        r.ask(load_of("recordings/big", std::string(port_name), 1.0))));
    REQUIRE(r.arrived(all.size()));
    REQUIRE_EQ(r.first.written(), all);
    REQUIRE(wait_until(
        [&]
        {
            r.pump();
            return r.handler->counts().finished == 1;
        },
        20000ms));
    REQUIRE_EQ(r.handler->counts().chunks, std::uint64_t{3000});
    REQUIRE_EQ(r.handler->counts().read_errors, std::uint64_t{0});
}

TEST_CASE("a replay counts what it does not play")
{
    rig r;
    const fs::path folder = r.data / "recordings" / "mixed-kinds";
    recording_options options;
    options.origin_utc = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    vss::plugin::host::place_spec spec;
    spec.name = std::string(port_name);
    options.places.push_back(std::move(spec));
    options.made_by.plugin = "com.example.replay";
    options.made_by.version = "1.0.0";
    auto writer = recording_writer::open(folder, std::move(options));
    REQUIRE(writer.has_value());
    const std::string in = "in";
    const std::string out = "out";
    REQUIRE(writer->add_data(0, 1'000, vss::data_direction::rx,
                             vss::byte_buffer(in.begin(), in.end())));
    REQUIRE(writer->add_data(0, 2'000, vss::data_direction::tx,
                             vss::byte_buffer(out.begin(), out.end())));
    REQUIRE(writer->add_fault(0, 3'000, "framing"));
    vss::control_lines lines;
    lines.set(vss::control_line::cts, true);
    REQUIRE(writer->add_line(0, 4'000, lines, lines));
    REQUIRE(writer->finish().has_value());
    r.have(rig::replay_instance());
    REQUIRE(rig::is_ok(
        r.ask(load_of("recordings/mixed-kinds", std::string(port_name), 1.0))));
    REQUIRE(wait_until(
        [&]
        {
            r.pump();
            return r.handler->counts().finished == 1;
        },
        20000ms));
    const auto counts = r.handler->counts();
    REQUIRE_EQ(r.first.written(), in);
    REQUIRE_EQ(counts.sent_skipped, std::uint64_t{1});
    REQUIRE_EQ(counts.faults_skipped, std::uint64_t{1});
    // The line change is played where the port follows lines and counted as
    // skipped where it does not: either way it is counted.
    REQUIRE_EQ(counts.line_changes + counts.lines_skipped, std::uint64_t{1});
}
