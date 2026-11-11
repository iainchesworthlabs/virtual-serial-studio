//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/agent/proto/message.hpp>

#include "test_framework.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace vss;
using namespace vss::agent;

namespace
{

constexpr std::uint64_t max_u64 = std::numeric_limits<std::uint64_t>::max();

// -- frames ------------------------------------------------------------------

frame hand_frame(std::string header, byte_buffer blob = {})
{
    frame out;
    out.header = std::move(header);
    out.blob = std::move(blob);
    return out;
}

byte_buffer bytes_of(const frame& value)
{
    const auto out = encode_frame(value);
    REQUIRE(out);
    return *out;
}

byte_buffer le32(std::uint32_t value)
{
    return {static_cast<byte>(value & 0xffu),
            static_cast<byte>((value >> 8) & 0xffu),
            static_cast<byte>((value >> 16) & 0xffu),
            static_cast<byte>((value >> 24) & 0xffu)};
}

void append(byte_buffer& out, const byte_buffer& more)
{
    out.insert(out.end(), more.begin(), more.end());
}

/// A length prefix, a header size and `body` filler bytes: what the reader
/// sees of a frame whose two lengths are whatever a test needs.
byte_buffer raw_frame(std::uint32_t size, std::uint32_t header_size,
                      std::size_t body)
{
    byte_buffer out = le32(size);
    append(out, le32(header_size));
    out.resize(out.size() + body, static_cast<byte>('h'));
    return out;
}

/// The frame the reader returns next.
frame take(frame_reader& reader)
{
    auto next = reader.next();
    REQUIRE(next);
    std::optional<frame> ready = std::move(*next);
    REQUIRE(ready.has_value());
    return std::move(*ready);
}

/// True when the reader has no whole frame yet, and no error.
bool wants_more(frame_reader& reader)
{
    const auto next = reader.next();
    REQUIRE(next);
    return !next->has_value();
}

error_code failure_of(frame_reader& reader)
{
    const auto next = reader.next();
    REQUIRE(!next);
    return next.error();
}

/// A frame as big as the protocol allows.
frame largest_frame()
{
    frame out;
    out.header = std::string(100, 'h');
    out.blob.assign(max_frame_size - frame_prefix_size - out.header.size(),
                    static_cast<byte>(0x5a));
    return out;
}

// -- text and bytes ----------------------------------------------------------

byte_buffer all_bytes()
{
    byte_buffer out;
    for (unsigned value = 0; value < 256; ++value)
        out.push_back(static_cast<byte>(value));
    return out;
}

/// Text made of exactly these bytes, whatever they are.
std::string raw(std::initializer_list<unsigned char> values)
{
    std::string out;
    for (const unsigned char value : values)
        out.push_back(static_cast<char>(value));
    return out;
}

std::string repeat(std::string_view unit, std::size_t count)
{
    std::string out;
    for (std::size_t index = 0; index < count; ++index)
        out.append(unit);
    return out;
}

std::string replace_once(std::string text, std::string_view from,
                         std::string_view to)
{
    const std::size_t at = text.find(from);
    REQUIRE_NE(at, std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

// -- messages ----------------------------------------------------------------

request make_request(std::uint64_t id, request_body body)
{
    return request{id, std::move(body)};
}

reply make_reply(std::uint64_t id, reply_body body)
{
    return reply{id, std::move(body)};
}

/// The frame goes through bytes and a reader and comes out the same.
void check_wire_roundtrip(const frame& value)
{
    const byte_buffer wire = bytes_of(value);
    frame_reader reader;
    reader.feed(bytes_view(wire));
    REQUIRE_EQ(take(reader), value);
    REQUIRE_EQ(reader.buffered(), std::size_t{0});
}

void check_request(const request& value)
{
    const frame wire = encode(value);
    check_wire_roundtrip(wire);
    const auto decoded = decode_request(wire);
    REQUIRE(decoded);
    REQUIRE_EQ(*decoded, value);
    REQUIRE_EQ(peek_request_id(wire), value.id);
    // A request is not something the agent sends.
    REQUIRE(!decode_server_message(wire));
}

void check_reply(const reply& value)
{
    const frame wire = encode(value);
    check_wire_roundtrip(wire);
    const auto decoded = decode_server_message(wire);
    REQUIRE(decoded);
    const server_message expected{value};
    REQUIRE_EQ(*decoded, expected);
    // A reply is not something a client sends.
    REQUIRE(!decode_request(wire));
}

void check_batch(const event_batch& value)
{
    const frame wire = encode(value);
    check_wire_roundtrip(wire);
    const auto decoded = decode_server_message(wire);
    REQUIRE(decoded);
    const server_message expected{value};
    REQUIRE_EQ(*decoded, expected);
    REQUIRE(!decode_request(wire));
}

std::vector<request> sample_requests()
{
    std::vector<request> out;
    out.push_back(
        make_request(1, hello_request{protocol_version, "vss_cli 0.1.0"}));
    out.push_back(make_request(
        2, hello_request{std::numeric_limits<std::uint32_t>::max(), ""}));
    out.push_back(make_request(3, list_request{}));
    out.push_back(make_request(4, create_port_request{"left", "virtual"}));
    out.push_back(make_request(5, create_port_request{"", ""}));
    out.push_back(make_request(6, remove_port_request{7}));
    out.push_back(make_request(7, connect_request{1, 2, false}));
    out.push_back(make_request(8, connect_request{2, 1, true}));
    out.push_back(
        make_request(9, write_request{3, byte_buffer{0x41, 0x54, 0x0d}}));
    out.push_back(make_request(10, write_request{3, byte_buffer{}}));
    out.push_back(make_request(11, write_request{4, all_bytes()}));
    out.push_back(make_request(12, subscribe_request{true, 10}));
    out.push_back(
        make_request(13, subscribe_request{false, default_event_interval_ms}));
    out.push_back(make_request(14, shutdown_request{}));
    out.push_back(
        make_request(max_u64, connect_request{max_u64, max_u64 - 1, true}));
    out.push_back(make_request(max_u64, remove_port_request{max_u64}));
    out.push_back(
        make_request(max_u64 - 1, write_request{max_u64, byte_buffer{0}}));
    return out;
}

list_reply sample_list()
{
    constexpr std::array kinds{
        endpoint_kind::virtual_port, endpoint_kind::physical,
        endpoint_kind::tcp,          endpoint_kind::udp,
        endpoint_kind::rfc2217,      endpoint_kind::mock};
    constexpr std::array states{endpoint_state::created,
                                endpoint_state::opening, endpoint_state::open,
                                endpoint_state::closed, endpoint_state::failed};
    list_reply body;
    body.revision = 77;
    for (std::size_t index = 0; index < kinds.size(); ++index)
    {
        port_summary port;
        port.id = index + 1;
        port.name = "port-" + std::to_string(index);
        port.path = "/dev/pts/" + std::to_string(index);
        port.kind = kinds[index];
        port.state = states[index % states.size()];
        port.open = index % 2 == 0;
        port.bytes_rx = max_u64 - index;
        port.bytes_tx = index * 1000;
        body.ports.push_back(port);
    }
    link_summary duplex;
    duplex.id = 1;
    duplex.mode = link_mode::duplex;
    duplex.sources = {1};
    duplex.targets = {2};
    duplex.bytes = 5;
    link_summary one_way;
    one_way.id = 2;
    one_way.mode = link_mode::one_way;
    one_way.sources = {1, 2};
    one_way.targets = {3, 4, 5};
    link_summary bus;
    bus.id = 3;
    bus.mode = link_mode::bus;
    bus.sources = {1, 2, 3};
    bus.bytes = max_u64;
    body.links = {duplex, one_way, bus};
    return body;
}

std::vector<reply> sample_replies()
{
    std::vector<reply> out;
    out.push_back(make_reply(1, done_reply{}));
    out.push_back(make_reply(
        2, hello_reply{protocol_version, "0.1.0", 4242,
                       std::vector<std::string>{"virtual", "tcp", "mock"}}));
    out.push_back(
        make_reply(3, hello_reply{std::numeric_limits<std::uint32_t>::max(), "",
                                  max_u64, std::vector<std::string>{}}));
    out.push_back(make_reply(4, list_reply{}));
    out.push_back(make_reply(5, sample_list()));
    out.push_back(make_reply(6, create_port_reply{max_u64}));
    out.push_back(make_reply(7, connect_reply{max_u64}));
    out.push_back(
        make_reply(8, error_reply{wire_error{"vss", 1, "no such port"}}));
    out.push_back(make_reply(9, error_reply{wire_error{"system", -1, ""}}));
    out.push_back(make_reply(
        10, error_reply{wire_error{"vss.agent", std::numeric_limits<int>::max(),
                                   "m"}}));
    out.push_back(
        make_reply(11, error_reply{wire_error{
                           "generic", std::numeric_limits<int>::min(), "m"}}));
    // The id of an error about a frame that had none.
    out.push_back(make_reply(
        0, error_reply{wire_error{"vss.agent", 1, "malformed frame"}}));
    out.push_back(make_reply(max_u64, done_reply{}));
    return out;
}

event_record make_event(event_kind kind, endpoint_id endpoint)
{
    event_record out;
    out.kind = kind;
    out.endpoint = endpoint;
    return out;
}

event_batch sample_batch()
{
    event_batch batch;
    batch.revision = 12;
    batch.dropped = 3;

    event_record started = make_event(event_kind::started, 1);
    started.name = "COM7";
    batch.events.push_back(started);

    event_record received = make_event(event_kind::data, 1);
    received.direction = data_direction::rx;
    received.size = 4;
    received.data = {1, 2, 3, 4};
    batch.events.push_back(received);

    // Traffic whose bytes the client did not ask for.
    event_record counted = make_event(event_kind::data, 1);
    counted.direction = data_direction::tx;
    counted.size = 100000;
    batch.events.push_back(counted);

    event_record everything = make_event(event_kind::data, 2);
    everything.direction = data_direction::tx;
    everything.size = 256;
    everything.data = all_bytes();
    batch.events.push_back(everything);

    event_record lines = make_event(event_kind::lines, 2);
    lines.lines = 0x2d;
    batch.events.push_back(lines);

    event_record failed = make_event(event_kind::error, 3);
    failed.name = "tcp";
    failed.message = "connection refused";
    failed.error = wire_error{"vss", 6, "not open"};
    batch.events.push_back(failed);

    event_record logged = make_event(event_kind::log_message, 0);
    logged.message = "d\xc3\xa9j\xc3\xa0 vu";
    batch.events.push_back(logged);

    batch.events.push_back(make_event(event_kind::stats, 3));
    batch.events.push_back(make_event(event_kind::stopped, 1));
    return batch;
}

/// A decoder test builds the header text by hand.
std::string event_json(std::uint64_t stored)
{
    return R"({"kind":"data","endpoint":1,"name":"","direction":"rx",)"
           R"("size":4,"stored":)" +
           std::to_string(stored) + R"(,"lines":0,"message":""})";
}

std::string batch_json(const std::vector<std::string>& events)
{
    std::string out = R"({"event":"batch","revision":0,"dropped":0,"events":[)";
    for (std::size_t index = 0; index < events.size(); ++index)
    {
        if (index != 0)
            out += ',';
        out += events[index];
    }
    out += "]}";
    return out;
}

void require_request_error(const frame& value, protocol_errc expected)
{
    const auto decoded = decode_request(value);
    REQUIRE(!decoded);
    REQUIRE_EQ(decoded.error(), make_error_code(expected));
}

void require_message_error(const frame& value, protocol_errc expected)
{
    const auto decoded = decode_server_message(value);
    REQUIRE(!decoded);
    REQUIRE_EQ(decoded.error(), make_error_code(expected));
}

void require_malformed_request(std::string header, byte_buffer blob = {})
{
    require_request_error(hand_frame(std::move(header), std::move(blob)),
                          protocol_errc::malformed_message);
}

void require_malformed_message(std::string header, byte_buffer blob = {})
{
    require_message_error(hand_frame(std::move(header), std::move(blob)),
                          protocol_errc::malformed_message);
}

/// `{"id":1,"op":"list", "x": [[...]]}` with `depth` arrays inside.
std::string nested_header(std::size_t depth)
{
    return R"({"id":1,"op":"list","x":)" + repeat("[", depth) + "0" +
           repeat("]", depth) + "}";
}

/// The name of a create_port request after it crossed the wire.
std::string name_after_wire(const std::string& name)
{
    const request sent = make_request(1, create_port_request{name, "mock"});
    const auto decoded = decode_request(encode(sent));
    REQUIRE(decoded);
    const auto* const body = std::get_if<create_port_request>(&decoded->body);
    REQUIRE(body != nullptr);
    return body->name;
}

} // namespace

// -- frames ------------------------------------------------------------------

TEST_CASE("agent proto: a frame is two lengths, a header and a blob")
{
    frame value;
    value.header = R"({"id":1,"op":"list"})";
    value.blob = {9, 8, 7};
    const auto header_size = static_cast<std::uint32_t>(value.header.size());

    byte_buffer expected = le32(4 + header_size + 3);
    append(expected, le32(header_size));
    append(expected, byte_buffer(value.header.begin(), value.header.end()));
    append(expected, value.blob);

    REQUIRE_EQ(bytes_of(value), expected);
}

TEST_CASE("agent proto: a whole frame fed at once comes out the same")
{
    frame value;
    value.header = R"({"id":1,"op":"list"})";
    value.blob = {0, 1, 0xfe, 0xff};
    check_wire_roundtrip(value);

    // The smallest frame: a header of `{}` and no blob.
    check_wire_roundtrip(hand_frame("{}"));
}

TEST_CASE("agent proto: one byte at a time needs more until the last")
{
    frame value;
    value.header = R"({"id":1,"op":"list"})";
    value.blob = {0, 1, 0xfe, 0xff};
    const byte_buffer wire = bytes_of(value);

    frame_reader reader;
    for (std::size_t index = 0; index < wire.size(); ++index)
    {
        reader.feed(bytes_view(wire).subspan(index, 1));
        if (index + 1 < wire.size())
        {
            REQUIRE(wants_more(reader));
            REQUIRE_EQ(reader.buffered(), index + 1);
        }
    }
    REQUIRE_EQ(take(reader), value);
    REQUIRE_EQ(reader.buffered(), std::size_t{0});
    REQUIRE(wants_more(reader));
}

TEST_CASE("agent proto: two frames in one feed come out in order")
{
    const frame first = hand_frame(R"({"a":1})", {1, 2, 3});
    const frame second = hand_frame(R"({"b":2})");
    byte_buffer wire = bytes_of(first);
    append(wire, bytes_of(second));

    frame_reader reader;
    reader.feed(bytes_view(wire));
    REQUIRE_EQ(take(reader), first);
    REQUIRE_EQ(take(reader), second);
    REQUIRE(wants_more(reader));
    REQUIRE_EQ(reader.buffered(), std::size_t{0});
}

TEST_CASE("agent proto: a frame split across feeds is joined")
{
    // The second feed holds the end of one frame and the start of the next,
    // which also moves the reader past the bytes it has used.
    for (std::size_t round = 0; round < 40; ++round)
    {
        const frame first =
            hand_frame(R"({"n":)" + std::to_string(round) + "}",
                       byte_buffer(round, static_cast<byte>(round)));
        const frame second = hand_frame(R"({"second":true})", {7});
        const byte_buffer one = bytes_of(first);
        const byte_buffer two = bytes_of(second);

        frame_reader reader;
        byte_buffer head = one;
        head.insert(head.end(), two.begin(), two.begin() + 3);
        reader.feed(bytes_view(head));
        REQUIRE_EQ(take(reader), first);
        REQUIRE(wants_more(reader));
        REQUIRE_EQ(reader.buffered(), std::size_t{3});

        reader.feed(bytes_view(two).subspan(3));
        REQUIRE_EQ(take(reader), second);
        REQUIRE_EQ(reader.buffered(), std::size_t{0});
    }
}

TEST_CASE("agent proto: a long stream of frames is read in order")
{
    frame_reader reader;
    for (std::size_t index = 0; index < 500; ++index)
    {
        const frame value =
            hand_frame(R"({"i":)" + std::to_string(index) + "}",
                       byte_buffer(index % 7, static_cast<byte>(index)));
        const byte_buffer wire = bytes_of(value);
        // Feed all but the last byte, take nothing, then feed the last.
        reader.feed(bytes_view(wire).first(wire.size() - 1));
        REQUIRE(wants_more(reader));
        reader.feed(bytes_view(wire).last(1));
        REQUIRE_EQ(take(reader), value);
    }
    REQUIRE_EQ(reader.buffered(), std::size_t{0});
}

TEST_CASE("agent proto: buffered counts the bytes of a frame not yet taken")
{
    const frame value = hand_frame(R"({"id":1})", {1, 2});
    const byte_buffer wire = bytes_of(value);

    frame_reader reader;
    REQUIRE_EQ(reader.buffered(), std::size_t{0});
    reader.feed(bytes_view(wire).first(5));
    REQUIRE_EQ(reader.buffered(), std::size_t{5});
    reader.feed(bytes_view(wire).subspan(5));
    REQUIRE_EQ(reader.buffered(), wire.size());
    static_cast<void>(take(reader));
    REQUIRE_EQ(reader.buffered(), std::size_t{0});

    // Feeding nothing changes nothing.
    reader.feed(bytes_view{});
    REQUIRE_EQ(reader.buffered(), std::size_t{0});
    REQUIRE(wants_more(reader));
}

TEST_CASE("agent proto: encode_frame refuses an empty or one byte header")
{
    for (const char* header : {"", "x"})
    {
        const auto out = encode_frame(hand_frame(header, {1, 2}));
        REQUIRE(!out);
        REQUIRE_EQ(out.error(),
                   make_error_code(protocol_errc::malformed_frame));
    }
}

TEST_CASE("agent proto: a frame of max_frame_size round-trips")
{
    const frame value = largest_frame();
    const byte_buffer wire = bytes_of(value);
    REQUIRE_EQ(wire.size(), frame_prefix_size + max_frame_size);

    frame_reader whole;
    whole.feed(bytes_view(wire));
    REQUIRE_EQ(take(whole), value);
    REQUIRE_EQ(whole.buffered(), std::size_t{0});

    // In pieces too.
    frame_reader pieces;
    constexpr std::size_t piece = 4093;
    for (std::size_t at = 0; at < wire.size(); at += piece)
    {
        const std::size_t left = wire.size() - at;
        const std::size_t count = left < piece ? left : piece;
        pieces.feed(bytes_view(wire).subspan(at, count));
        if (at + count < wire.size())
            REQUIRE(wants_more(pieces));
    }
    REQUIRE_EQ(take(pieces), value);

    // A header alone may take all of it.
    frame header_only;
    header_only.header = std::string(max_frame_size - frame_prefix_size, 'h');
    check_wire_roundtrip(header_only);
}

TEST_CASE("agent proto: encode_frame refuses a frame over max_frame_size")
{
    frame value = largest_frame();
    value.blob.push_back(0);
    const auto out = encode_frame(value);
    REQUIRE(!out);
    REQUIRE_EQ(out.error(), make_error_code(protocol_errc::oversized_frame));

    frame header_only;
    header_only.header =
        std::string(max_frame_size - frame_prefix_size + 1, 'h');
    const auto header_out = encode_frame(header_only);
    REQUIRE(!header_out);
    REQUIRE_EQ(header_out.error(),
               make_error_code(protocol_errc::oversized_frame));

    frame blob_only = hand_frame("{}");
    blob_only.blob.assign(max_frame_size, 0);
    const auto blob_out = encode_frame(blob_only);
    REQUIRE(!blob_out);
    REQUIRE_EQ(blob_out.error(),
               make_error_code(protocol_errc::oversized_frame));

    // A write whose bytes fill the frame is the same mistake.
    const frame big_write = encode(
        make_request(1, write_request{1, byte_buffer(max_frame_size, 0)}));
    const auto write_out = encode_frame(big_write);
    REQUIRE(!write_out);
    REQUIRE_EQ(write_out.error(),
               make_error_code(protocol_errc::oversized_frame));
}

TEST_CASE("agent proto: a length over max_frame_size fails at the prefix")
{
    const byte_buffer prefix =
        le32(static_cast<std::uint32_t>(max_frame_size + 1));
    frame_reader reader;
    // Three bytes are not a length yet.
    reader.feed(bytes_view(prefix).first(3));
    REQUIRE(wants_more(reader));
    // The fourth is, and no body is needed to refuse it.
    reader.feed(bytes_view(prefix).last(1));
    REQUIRE_EQ(failure_of(reader),
               make_error_code(protocol_errc::oversized_frame));
    REQUIRE(reader.buffered() <= prefix.size());

    const byte_buffer largest = le32(0xffffffffu);
    frame_reader other;
    other.feed(bytes_view(largest));
    REQUIRE_EQ(failure_of(other),
               make_error_code(protocol_errc::oversized_frame));
}

TEST_CASE("agent proto: a frame reader error is final")
{
    frame_reader reader;
    const byte_buffer prefix = le32(0xffffffffu);
    reader.feed(bytes_view(prefix));
    REQUIRE_EQ(failure_of(reader),
               make_error_code(protocol_errc::oversized_frame));

    // Good frames after it are not read, and the error stays.
    const byte_buffer good = bytes_of(hand_frame("{}"));
    for (int round = 0; round < 3; ++round)
    {
        reader.feed(bytes_view(good));
        REQUIRE_EQ(failure_of(reader),
                   make_error_code(protocol_errc::oversized_frame));
    }

    frame_reader malformed;
    const byte_buffer short_prefix = le32(3);
    malformed.feed(bytes_view(short_prefix));
    REQUIRE_EQ(failure_of(malformed),
               make_error_code(protocol_errc::malformed_frame));
    malformed.feed(bytes_view(good));
    REQUIRE_EQ(failure_of(malformed),
               make_error_code(protocol_errc::malformed_frame));
}

TEST_CASE("agent proto: a length of exactly max_frame_size is accepted")
{
    const byte_buffer prefix = le32(static_cast<std::uint32_t>(max_frame_size));
    frame_reader reader;
    reader.feed(bytes_view(prefix));
    REQUIRE(wants_more(reader));
}

TEST_CASE("agent proto: a length under 6 is a malformed frame")
{
    for (std::uint32_t size = 0; size < 6; ++size)
    {
        frame_reader reader;
        const byte_buffer prefix = le32(size);
        reader.feed(bytes_view(prefix));
        REQUIRE_EQ(failure_of(reader),
                   make_error_code(protocol_errc::malformed_frame));
    }
}

TEST_CASE("agent proto: a bad header size is a malformed frame")
{
    // The frame is 10 bytes after the prefix: the header size, 6 more.  The
    // header may take 2 to 6 of them.
    for (const std::uint32_t header_size : {0u, 1u, 7u, 100u, 0xffffffffu})
    {
        frame_reader reader;
        const byte_buffer wire = raw_frame(10, header_size, 6);
        reader.feed(bytes_view(wire));
        REQUIRE_EQ(failure_of(reader),
                   make_error_code(protocol_errc::malformed_frame));
    }

    // The reader says so before the body is here.
    frame_reader early;
    const byte_buffer head = raw_frame(1000, 0, 0);
    early.feed(bytes_view(head));
    REQUIRE_EQ(failure_of(early),
               make_error_code(protocol_errc::malformed_frame));

    frame_reader other;
    const byte_buffer beyond = raw_frame(1000, 997, 0);
    other.feed(bytes_view(beyond));
    REQUIRE_EQ(failure_of(other),
               make_error_code(protocol_errc::malformed_frame));
}

TEST_CASE("agent proto: the header size bounds are accepted")
{
    // The smallest header.
    frame_reader smallest;
    const byte_buffer two = raw_frame(10, 2, 6);
    smallest.feed(bytes_view(two));
    const frame tiny = take(smallest);
    REQUIRE_EQ(tiny.header.size(), std::size_t{2});
    REQUIRE_EQ(tiny.blob.size(), std::size_t{4});

    // A header that takes the whole body, and no blob.
    frame_reader largest;
    const byte_buffer six = raw_frame(10, 6, 6);
    largest.feed(bytes_view(six));
    const frame whole = take(largest);
    REQUIRE_EQ(whole.header.size(), std::size_t{6});
    REQUIRE_EQ(whole.blob.size(), std::size_t{0});
}

// -- messages ----------------------------------------------------------------

TEST_CASE("agent proto: every request round-trips")
{
    for (const request& value : sample_requests())
        check_request(value);

    // Every alternative is in the samples.
    std::array<bool, std::variant_size_v<request_body>> seen{};
    for (const request& value : sample_requests())
        seen[value.body.index()] = true;
    for (const bool present : seen)
        REQUIRE(present);
}

TEST_CASE("agent proto: every reply round-trips")
{
    for (const reply& value : sample_replies())
        check_reply(value);

    std::array<bool, std::variant_size_v<reply_body>> seen{};
    for (const reply& value : sample_replies())
        seen[value.body.index()] = true;
    for (const bool present : seen)
        REQUIRE(present);
}

TEST_CASE("agent proto: a list reply keeps every kind, state and mode")
{
    const list_reply body = sample_list();
    REQUIRE_EQ(body.ports.size(), std::size_t{6});
    check_reply(make_reply(1, body));

    const auto decoded = decode_server_message(encode(make_reply(1, body)));
    REQUIRE(decoded);
    const auto* const answer = std::get_if<reply>(&*decoded);
    REQUIRE(answer != nullptr);
    const auto* const list = std::get_if<list_reply>(&answer->body);
    REQUIRE(list != nullptr);
    REQUIRE_EQ(list->ports.size(), body.ports.size());
    for (std::size_t index = 0; index < body.ports.size(); ++index)
    {
        REQUIRE(list->ports[index].kind == body.ports[index].kind);
        REQUIRE(list->ports[index].state == body.ports[index].state);
    }
    REQUIRE_EQ(list->links.size(), std::size_t{3});
    REQUIRE(list->links[2].mode == link_mode::bus);
    REQUIRE(list->links[2].targets.empty());
}

TEST_CASE("agent proto: ids up to the largest 64 bit value round-trip")
{
    check_request(make_request(max_u64, list_request{}));
    check_request(make_request(max_u64, remove_port_request{max_u64}));
    check_reply(make_reply(max_u64, create_port_reply{max_u64}));
    check_reply(make_reply(max_u64, connect_reply{max_u64}));

    list_reply body;
    body.revision = max_u64;
    port_summary port;
    port.id = max_u64;
    port.bytes_rx = max_u64;
    port.bytes_tx = max_u64;
    body.ports.push_back(port);
    link_summary member;
    member.id = max_u64;
    member.sources = {max_u64, max_u64 - 1};
    member.targets = {max_u64};
    member.bytes = max_u64;
    body.links.push_back(member);
    check_reply(make_reply(max_u64, body));

    event_batch batch;
    batch.revision = max_u64;
    batch.dropped = max_u64;
    event_record record = make_event(event_kind::data, max_u64);
    record.size = max_u64;
    batch.events.push_back(record);
    check_batch(batch);
}

TEST_CASE("agent proto: a write request carries its bytes in the blob")
{
    const byte_buffer data = all_bytes();
    const request sent = make_request(5, write_request{9, data});
    const frame wire = encode(sent);
    REQUIRE_EQ(wire.blob, data);

    // The header says nothing of the bytes: it is the same for any payload.
    const frame other =
        encode(make_request(5, write_request{9, byte_buffer(256, 0x7f)}));
    REQUIRE_EQ(wire.header, other.header);
    const frame none = encode(make_request(5, write_request{9, {}}));
    REQUIRE_EQ(wire.header, none.header);
    REQUIRE(wire.header.size() < 64);
    REQUIRE(none.blob.empty());

    const auto decoded = decode_request(wire);
    REQUIRE(decoded);
    const auto* const body = std::get_if<write_request>(&decoded->body);
    REQUIRE(body != nullptr);
    REQUIRE_EQ(body->data, data);
    REQUIRE_EQ(body->endpoint, std::uint64_t{9});

    // Through bytes, one at a time, with all 256 values.
    const byte_buffer bytes = bytes_of(wire);
    frame_reader reader;
    for (const byte value : bytes)
        reader.feed(bytes_view(&value, 1));
    REQUIRE_EQ(take(reader), wire);
}

TEST_CASE("agent proto: an event batch keeps its events and their bytes")
{
    const event_batch batch = sample_batch();
    check_batch(batch);

    // The bytes of the events sit in the blob in order, with no gaps.
    const frame wire = encode(batch);
    byte_buffer expected;
    for (const event_record& record : batch.events)
        append(expected, record.data);
    REQUIRE_EQ(wire.blob, expected);
    REQUIRE_EQ(wire.blob.size(), std::size_t{260});

    const auto decoded = decode_server_message(wire);
    REQUIRE(decoded);
    const auto* const out = std::get_if<event_batch>(&*decoded);
    REQUIRE(out != nullptr);
    REQUIRE_EQ(out->events.size(), batch.events.size());
    REQUIRE_EQ(out->events[1].data, (byte_buffer{1, 2, 3, 4}));
    REQUIRE(out->events[2].data.empty());
    REQUIRE_EQ(out->events[2].size, std::uint64_t{100000});
    REQUIRE_EQ(out->events[3].data, all_bytes());
    REQUIRE_EQ(out->events[4].lines, std::uint32_t{0x2d});
    REQUIRE(out->events[5].error == (wire_error{"vss", 6, "not open"}));
    REQUIRE_EQ(out->events[6].message, batch.events[6].message);
}

TEST_CASE("agent proto: an empty batch and a lone error event round-trip")
{
    check_batch(event_batch{});

    event_batch batch;
    event_record record = make_event(event_kind::error, 1);
    record.error = wire_error{"system", 5, "access denied"};
    batch.events.push_back(record);
    check_batch(batch);

    // A negative value and an error that is the default one but for a
    // category are not lost.
    batch.events[0].error = wire_error{"", -7, ""};
    check_batch(batch);
    batch.events[0].error = wire_error{"x", 0, ""};
    check_batch(batch);
}

TEST_CASE("agent proto: a request is not decoded as a reply or a batch")
{
    for (const reply& value : sample_replies())
        REQUIRE(!decode_request(encode(value)));
    REQUIRE(!decode_request(encode(sample_batch())));
    for (const request& value : sample_requests())
        REQUIRE(!decode_server_message(encode(value)));
}

TEST_CASE("agent proto: hand written headers in the right form decode")
{
    // Guards the strictness tests below: the form they bend is a good one.
    const auto listed = decode_request(hand_frame(R"({"id":1,"op":"list"})"));
    REQUIRE(listed);
    REQUIRE_EQ(listed->id, std::uint64_t{1});

    const auto error = decode_server_message(
        hand_frame(R"({"id":1,"reply":"error",)"
                   R"("error":{"category":"vss","value":1,"message":"m"}})"));
    REQUIRE(error);

    const auto batch = decode_server_message(
        hand_frame(batch_json({event_json(4), event_json(0)}), {1, 2, 3, 4}));
    REQUIRE(batch);

    // The largest id.
    const auto both = decode_request(hand_frame(
        R"({"id":18446744073709551615,"op":"remove_port","endpoint":0})"));
    REQUIRE(both);
    REQUIRE_EQ(both->id, max_u64);

    const auto limits = decode_server_message(
        hand_frame(R"({"id":1,"reply":"error","error":{"category":"c",)"
                   R"("value":-2147483648,"message":""}})"));
    REQUIRE(limits);
    const auto top = decode_server_message(
        hand_frame(R"({"id":1,"reply":"error","error":{"category":"c",)"
                   R"("value":2147483647,"message":""}})"));
    REQUIRE(top);
}

TEST_CASE("agent proto: a request header that is not a request is malformed")
{
    const std::vector<std::string> headers{
        // Not JSON.
        "",
        "not json",
        "{",
        "}",
        R"({"id":1,"op":"list"} trailing)",
        R"({"id":1,"op":"list",})",
        // JSON that is not an object.
        "[]",
        "42",
        R"("list")",
        "null",
        "true",
        R"([{"id":1,"op":"list"}])",
        // The id.
        R"({"op":"list"})",
        R"({"id":0,"op":"list"})",
        R"({"id":-1,"op":"list"})",
        R"({"id":1.5,"op":"list"})",
        R"({"id":1.0,"op":"list"})",
        R"({"id":"1","op":"list"})",
        R"({"id":true,"op":"list"})",
        R"({"id":null,"op":"list"})",
        R"({"id":[1],"op":"list"})",
        R"({"id":18446744073709551616,"op":"list"})",
        // The op.
        R"({"id":1})",
        R"({"id":1,"op":7})",
        R"({"id":1,"op":null})",
        R"({"id":1,"op":["list"]})",
        R"({"id":1,"reply":"done"})",
        // Fields of the wrong type or missing.
        R"({"id":1,"op":"hello","protocol":"1","client":"c"})",
        R"({"id":1,"op":"hello","protocol":-1,"client":"c"})",
        R"({"id":1,"op":"hello","protocol":4294967296,"client":"c"})",
        R"({"id":1,"op":"hello","protocol":1.5,"client":"c"})",
        R"({"id":1,"op":"hello","protocol":1,"client":5})",
        R"({"id":1,"op":"hello","protocol":1})",
        R"({"id":1,"op":"hello","client":"c"})",
        R"({"id":1,"op":"create_port","name":1,"kind":"mock"})",
        R"({"id":1,"op":"create_port","name":"a","kind":null})",
        R"({"id":1,"op":"create_port","name":"a"})",
        R"({"id":1,"op":"create_port","kind":"mock"})",
        R"({"id":1,"op":"remove_port"})",
        R"({"id":1,"op":"remove_port","endpoint":-1})",
        R"({"id":1,"op":"remove_port","endpoint":"2"})",
        R"({"id":1,"op":"remove_port","endpoint":2.5})",
        R"({"id":1,"op":"connect","first":1,"second":2})",
        R"({"id":1,"op":"connect","first":1,"second":2,"one_way":"no"})",
        R"({"id":1,"op":"connect","first":1,"second":2,"one_way":0})",
        R"({"id":1,"op":"connect","first":1,"one_way":true})",
        R"({"id":1,"op":"connect","second":1,"one_way":true})",
        R"({"id":1,"op":"connect","first":-1,"second":2,"one_way":true})",
        R"({"id":1,"op":"write"})",
        R"({"id":1,"op":"write","endpoint":[1]})",
        R"({"id":1,"op":"write","endpoint":-2})",
        R"({"id":1,"op":"subscribe","data":1,"interval_ms":250})",
        R"({"id":1,"op":"subscribe","data":true,"interval_ms":-1})",
        R"({"id":1,"op":"subscribe","data":true,"interval_ms":4294967296})",
        R"({"id":1,"op":"subscribe","data":true,"interval_ms":"250"})",
        R"({"id":1,"op":"subscribe","data":true,"interval_ms":2.5})",
    };
    for (const std::string& header : headers)
        require_malformed_request(header);
}

TEST_CASE("agent proto: an unknown operation is not a malformed message")
{
    for (const char* header :
         {R"({"id":1,"op":"explode"})", R"({"id":1,"op":""})",
          R"({"id":1,"op":"LIST"})", R"({"id":1,"op":"list "})",
          R"({"id":1,"op":"hello_reply"})"})
    {
        require_request_error(hand_frame(header),
                              protocol_errc::unknown_operation);
        // The caller can still answer the right request.
        REQUIRE_EQ(peek_request_id(hand_frame(header)), std::uint64_t{1});
    }
}

TEST_CASE("agent proto: only a write request carries a blob")
{
    for (const request& value : sample_requests())
    {
        frame wire = encode(value);
        wire.blob = {1};
        if (std::holds_alternative<write_request>(value.body))
        {
            REQUIRE(decode_request(wire));
        }
        else
        {
            require_request_error(wire, protocol_errc::malformed_message);
        }
    }
    // A blob where there is none is wrong even with a hand written header.
    require_malformed_request(R"({"id":1,"op":"list"})", {0});
    require_malformed_request(R"({"id":1,"op":"shutdown"})", {0});
}

TEST_CASE("agent proto: a reply with a blob is malformed")
{
    for (const reply& value : sample_replies())
    {
        frame wire = encode(value);
        wire.blob = {1};
        require_message_error(wire, protocol_errc::malformed_message);
    }
}

TEST_CASE("agent proto: a reply header that is not a reply is malformed")
{
    const char* const headers[] = {
        "",
        "not json",
        "[]",
        "7",
        R"({"reply":"done"})",
        R"({"id":-1,"reply":"done"})",
        R"({"id":"1","reply":"done"})",
        R"({"id":1.5,"reply":"done"})",
        R"({"id":1})",
        R"({"id":1,"reply":5})",
        R"({"id":1,"reply":"nope"})",
        R"({"id":1,"op":"list"})",
        R"({"id":1,"event":"other"})",
        R"({"id":1,"event":5})",
        R"({"id":1,"reply":"port"})",
        R"({"id":1,"reply":"port","endpoint":"1"})",
        R"({"id":1,"reply":"port","endpoint":-1})",
        R"({"id":1,"reply":"link"})",
        R"({"id":1,"reply":"link","link":true})",
        R"({"id":1,"reply":"hello","protocol":1,"agent_version":"1",)"
        R"("process_id":2})",
        R"({"id":1,"reply":"hello","protocol":1,"agent_version":"1",)"
        R"("process_id":2,"port_kinds":"mock"})",
        R"({"id":1,"reply":"hello","protocol":1,"agent_version":"1",)"
        R"("process_id":2,"port_kinds":[1]})",
        R"({"id":1,"reply":"hello","protocol":"1","agent_version":"1",)"
        R"("process_id":2,"port_kinds":[]})",
        R"({"id":1,"reply":"hello","protocol":1,"agent_version":1,)"
        R"("process_id":2,"port_kinds":[]})",
        R"({"id":1,"reply":"hello","protocol":1,"agent_version":"1",)"
        R"("process_id":-2,"port_kinds":[]})",
        R"({"id":1,"reply":"list","revision":1,"ports":[]})",
        R"({"id":1,"reply":"list","revision":1,"links":[]})",
        R"({"id":1,"reply":"list","ports":[],"links":[]})",
        R"({"id":1,"reply":"list","revision":1,"ports":{},"links":[]})",
        R"({"id":1,"reply":"list","revision":1,"ports":[7],"links":[]})",
        R"({"id":1,"reply":"list","revision":1,"ports":[],"links":[[]]})",
        R"({"id":1,"reply":"error"})",
        R"({"id":1,"reply":"error","error":"x"})",
        R"({"id":1,"reply":"error","error":{"category":"vss","value":1}})",
        R"({"id":1,"reply":"error","error":{"value":1,"message":""}})",
        R"({"id":1,"reply":"error","error":{"category":"vss",)"
        R"("message":""}})",
        R"({"id":1,"reply":"error","error":{"category":"vss",)"
        R"("value":1.5,"message":""}})",
        R"({"id":1,"reply":"error","error":{"category":"vss",)"
        R"("value":"1","message":""}})",
        R"({"id":1,"reply":"error","error":{"category":"vss",)"
        R"("value":2147483648,"message":""}})",
        R"({"id":1,"reply":"error","error":{"category":"vss",)"
        R"("value":-2147483649,"message":""}})",
        R"({"id":1,"reply":"error","error":{"category":5,"value":1,)"
        R"("message":""}})",
    };
    for (const char* header : headers)
        require_malformed_message(header);
}

TEST_CASE("agent proto: a batch header that is not a batch is malformed")
{
    const std::vector<std::string> headers{
        R"({"event":"batch","dropped":0,"events":[]})",
        R"({"event":"batch","revision":0,"events":[]})",
        R"({"event":"batch","revision":0,"dropped":0})",
        R"({"event":"batch","revision":0,"dropped":0,"events":{}})",
        R"({"event":"batch","revision":0,"dropped":0,"events":"none"})",
        R"({"event":"batch","revision":-1,"dropped":0,"events":[]})",
        R"({"event":"batch","revision":0,"dropped":"0","events":[]})",
        R"({"event":"batch","revision":0,"dropped":0,"events":[7]})",
        R"({"event":"batch","revision":0,"dropped":0,"events":[{}]})",
        R"({"event":"batch","revision":0,"dropped":0,"events":[[]]})",
        R"({"event":"update","revision":0,"dropped":0,"events":[]})",
        R"({"event":7,"revision":0,"dropped":0,"events":[]})",
    };
    for (const std::string& header : headers)
        require_malformed_message(header);

    // One field of an event at a time.
    const char* const events[] = {
        R"({"endpoint":1,"name":"","direction":"rx","size":0,"stored":0,)"
        R"("lines":0,"message":""})",
        R"({"kind":"data","name":"","direction":"rx","size":0,"stored":0,)"
        R"("lines":0,"message":""})",
        R"({"kind":"data","endpoint":1,"direction":"rx","size":0,)"
        R"("stored":0,"lines":0,"message":""})",
        R"({"kind":"data","endpoint":1,"name":"","size":0,"stored":0,)"
        R"("lines":0,"message":""})",
        R"({"kind":"data","endpoint":1,"name":"","direction":"rx",)"
        R"("stored":0,"lines":0,"message":""})",
        R"({"kind":"data","endpoint":1,"name":"","direction":"rx","size":0,)"
        R"("lines":0,"message":""})",
        R"({"kind":"data","endpoint":1,"name":"","direction":"rx","size":0,)"
        R"("stored":0,"message":""})",
        R"({"kind":"data","endpoint":1,"name":"","direction":"rx","size":0,)"
        R"("stored":0,"lines":0})",
        R"({"kind":"data","endpoint":1,"name":"","direction":"rx","size":0,)"
        R"("stored":0,"lines":4294967296,"message":""})",
        R"({"kind":"data","endpoint":1,"name":"","direction":"rx","size":0,)"
        R"("stored":-1,"lines":0,"message":""})",
        R"({"kind":"data","endpoint":1,"name":"","direction":"rx","size":0,)"
        R"("stored":0,"lines":0,"message":"","error":5})",
        R"({"kind":"data","endpoint":1,"name":"","direction":"rx","size":0,)"
        R"("stored":0,"lines":0,"message":"","error":{"value":1}})",
    };
    for (const char* event : events)
        require_malformed_message(batch_json({std::string(event)}));
}

TEST_CASE("agent proto: a name over max_text_size is malformed")
{
    const std::string longest = std::string(max_text_size, 'n');
    const std::string too_long = std::string(max_text_size + 1, 'n');

    const auto fits =
        decode_request(hand_frame(R"({"id":1,"op":"create_port","name":")" +
                                  longest + R"(","kind":"mock"})"));
    REQUIRE(fits);

    require_malformed_request(R"({"id":1,"op":"create_port","name":")" +
                              too_long + R"(","kind":"mock"})");
    require_malformed_request(R"({"id":1,"op":"create_port","name":"a",)"
                              R"("kind":")" +
                              too_long + R"("})");
    require_malformed_request(R"({"id":1,"op":"hello","protocol":1,)"
                              R"("client":")" +
                              too_long + R"("})");

    require_malformed_message(
        R"({"id":1,"reply":"error","error":{"category":"vss","value":1,)"
        R"("message":")" +
        too_long + R"("}})");
    require_malformed_message(
        R"({"id":1,"reply":"error","error":{"category":")" + too_long +
        R"(","value":1,"message":""}})");
    require_malformed_message(
        R"({"id":1,"reply":"hello","protocol":1,"agent_version":")" + too_long +
        R"(","process_id":2,"port_kinds":[]})");
    require_malformed_message(
        R"({"id":1,"reply":"hello","protocol":1,"agent_version":"1",)"
        R"("process_id":2,"port_kinds":[")" +
        too_long + R"("]})");

    std::string event = event_json(0);
    event =
        replace_once(event, R"("name":"")", R"("name":")" + too_long + R"(")");
    require_malformed_message(batch_json({event}));
    event = event_json(0);
    event = replace_once(event, R"("message":"")",
                         R"("message":")" + too_long + R"(")");
    require_malformed_message(batch_json({event}));

    // A port's name and path.
    const std::string port_head =
        R"({"id":1,"reply":"list","revision":1,"links":[],"ports":[)";
    const std::string port_tail =
        R"("kind":"mock","state":"open","open":true,"bytes_rx":0,)"
        R"("bytes_tx":0}]})";
    const auto port_fits = decode_server_message(
        hand_frame(port_head + R"({"id":1,"name":")" + longest +
                   R"(","path":"",)" + port_tail));
    REQUIRE(port_fits);
    require_malformed_message(port_head + R"({"id":1,"name":")" + too_long +
                              R"(","path":"",)" + port_tail);
    require_malformed_message(port_head + R"({"id":1,"name":"","path":")" +
                              too_long + R"(",)" + port_tail);
}

TEST_CASE("agent proto: JSON nested deeper than max_json_depth is refused")
{
    // The object is one level, so the arrays may have max_json_depth - 1.
    const auto shallow =
        decode_request(hand_frame(nested_header(max_json_depth - 1)));
    REQUIRE(shallow);

    require_malformed_request(nested_header(max_json_depth));
    require_malformed_request(nested_header(max_json_depth + 1));
    require_malformed_request(nested_header(1000));
    require_malformed_message(R"({"id":1,"reply":"done","x":)" +
                              repeat("[", 100) + repeat("]", 100) + "}");
    // The depth is not a limit on the text length.
    require_malformed_request(nested_header(100000));
    REQUIRE_EQ(peek_request_id(hand_frame(nested_header(max_json_depth))),
               std::uint64_t{0});
}

TEST_CASE("agent proto: an unknown enumeration name is malformed")
{
    const reply listed = make_reply(1, sample_list());
    const std::string header = encode(listed).header;
    REQUIRE(decode_server_message(hand_frame(header)));

    for (const std::string_view from :
         {R"("kind":"tcp")", R"("kind":"mock")", R"("state":"failed")",
          R"("mode":"bus")", R"("mode":"duplex")"})
    {
        const std::size_t colon = from.find(':');
        const std::string key(from.substr(0, colon + 1));
        for (const char* bad :
             {"usb", "", "Virtual", "TCP", "one_way", "unknown", "0"})
        {
            const std::string text =
                replace_once(header, from, key + '"' + bad + '"');
            require_malformed_message(text);
        }
    }

    // The same names of an event.
    const std::string event = event_json(0);
    REQUIRE(decode_server_message(hand_frame(batch_json({event}))));
    for (const char* bad : {"boom", "", "Data", "log_message", "0"})
    {
        require_malformed_message(
            batch_json({replace_once(event, R"("kind":"data")",
                                     std::string(R"("kind":")") + bad + '"')}));
        require_malformed_message(batch_json(
            {replace_once(event, R"("direction":"rx")",
                          std::string(R"("direction":")") + bad + '"')}));
    }
    // The number of a name is not the name.
    require_malformed_message(
        batch_json({replace_once(event, R"("kind":"data")", R"("kind":2)")}));
    require_malformed_message(batch_json(
        {replace_once(event, R"("direction":"rx")", R"("direction":0)")}));
}

TEST_CASE("agent proto: stored sizes must add up to the blob")
{
    const std::string two = batch_json({event_json(4), event_json(4)});

    REQUIRE(decode_server_message(hand_frame(two, byte_buffer(8, 1))));
    // More bytes than the events name.
    require_malformed_message(two, byte_buffer(9, 1));
    require_malformed_message(two, byte_buffer(100, 1));
    // Fewer.
    require_malformed_message(two, byte_buffer(7, 1));
    require_malformed_message(two, byte_buffer(4, 1));
    require_malformed_message(two, {});

    // Bytes left over after events that stored none.
    require_malformed_message(batch_json({event_json(0)}), {1});
    require_malformed_message(batch_json({}), {1});
    REQUIRE(decode_server_message(hand_frame(batch_json({}))));

    // A single event.
    require_malformed_message(batch_json({event_json(4)}), byte_buffer(3, 1));
    require_malformed_message(batch_json({event_json(4)}), byte_buffer(5, 1));
}

TEST_CASE("agent proto: a stored size over the blob is refused")
{
    // None of these may allocate what the number says.
    require_malformed_message(batch_json({event_json(5)}), byte_buffer(4, 1));
    require_malformed_message(batch_json({event_json(1u << 30)}),
                              byte_buffer(4, 1));
    require_malformed_message(batch_json({event_json(max_u64)}),
                              byte_buffer(4, 1));
    require_malformed_message(batch_json({event_json(max_u64)}));
    require_malformed_message(
        R"({"event":"batch","revision":0,"dropped":0,"events":[)"
        R"({"kind":"data","endpoint":1,"name":"","direction":"rx",)"
        R"("size":4,"stored":99999999999999999999,"lines":0,"message":""}]})",
        byte_buffer(4, 1));
    // The second event asks for what the first already used.
    require_malformed_message(batch_json({event_json(4), event_json(1)}),
                              byte_buffer(4, 1));
    // Many events that each name the whole blob.
    std::vector<std::string> many(100, event_json(4));
    require_malformed_message(batch_json(many), byte_buffer(4, 1));
}

TEST_CASE("agent proto: a list over max_list_size is refused")
{
    // Strings in the port kinds of a hello reply.
    const auto hello = [](std::size_t count)
    {
        return hand_frame(
            R"({"id":1,"reply":"hello","protocol":1,"agent_version":"1",)"
            R"("process_id":2,"port_kinds":[)" +
            repeat(R"("",)", count == 0 ? 0 : count - 1) +
            (count == 0 ? "" : R"("")") + "]}");
    };
    REQUIRE(decode_server_message(hello(max_list_size)));
    require_message_error(hello(max_list_size + 1),
                          protocol_errc::malformed_message);

    // Ids of a link.
    const auto link = [](std::size_t count)
    {
        return hand_frame(
            R"({"id":1,"reply":"list","revision":1,"ports":[],"links":[)"
            R"({"id":1,"mode":"duplex","targets":[],"bytes":0,)"
            R"("sources":[)" +
            repeat("1,", count == 0 ? 0 : count - 1) + (count == 0 ? "" : "1") +
            "]}]}");
    };
    REQUIRE(decode_server_message(link(max_list_size)));
    require_message_error(link(max_list_size + 1),
                          protocol_errc::malformed_message);

    // Events.
    const auto events = [](std::size_t count)
    {
        return hand_frame(
            batch_json(std::vector<std::string>(count, event_json(0))));
    };
    REQUIRE(decode_server_message(events(max_list_size)));
    require_message_error(events(max_list_size + 1),
                          protocol_errc::malformed_message);

    // Ports and links themselves.
    const auto ports = [](std::size_t count)
    {
        const std::string port =
            R"({"id":1,"name":"","path":"","kind":"mock","state":"open",)"
            R"("open":true,"bytes_rx":0,"bytes_tx":0})";
        std::string items;
        for (std::size_t index = 0; index < count; ++index)
            items += (index == 0 ? "" : ",") + port;
        return hand_frame(R"({"id":1,"reply":"list","revision":1,)"
                          R"("links":[],"ports":[)" +
                          items + "]}");
    };
    REQUIRE(decode_server_message(ports(max_list_size)));
    require_message_error(ports(max_list_size + 1),
                          protocol_errc::malformed_message);
}

// -- text --------------------------------------------------------------------

TEST_CASE("agent proto: invalid UTF-8 in a name is replaced byte by byte")
{
    REQUIRE_EQ(name_after_wire(raw({0xff, 0xfe})), std::string("??"));
    // A multibyte sequence cut short.
    REQUIRE_EQ(name_after_wire(raw({'o', 'k', 0xe2, 0x82})),
               std::string("ok??"));
    REQUIRE_EQ(name_after_wire(raw({0xf0, 0x9f, 0x98})), std::string("???"));
    REQUIRE_EQ(name_after_wire(raw({0xc3})), std::string("?"));
    // A sequence cut short by a plain character.
    REQUIRE_EQ(name_after_wire(raw({0xe2, 0x82, 'A'})), std::string("??A"));
    // Overlong encodings.
    REQUIRE_EQ(name_after_wire(raw({0xc0, 0xaf})), std::string("??"));
    REQUIRE_EQ(name_after_wire(raw({0xc1, 0xbf})), std::string("??"));
    REQUIRE_EQ(name_after_wire(raw({0xe0, 0x80, 0x80})), std::string("???"));
    REQUIRE_EQ(name_after_wire(raw({0xf0, 0x80, 0x80, 0x80})),
               std::string("????"));
    // A UTF-16 surrogate.
    REQUIRE_EQ(name_after_wire(raw({0xed, 0xa0, 0x80})), std::string("???"));
    REQUIRE_EQ(name_after_wire(raw({0xed, 0xbf, 0xbf})), std::string("???"));
    // Beyond U+10FFFF, and lead bytes that are no lead.
    REQUIRE_EQ(name_after_wire(raw({0xf4, 0x90, 0x80, 0x80})),
               std::string("????"));
    REQUIRE_EQ(name_after_wire(raw({0xf8, 0x88, 0x80, 0x80, 0x80})),
               std::string("?????"));
    REQUIRE_EQ(name_after_wire(raw({0x80})), std::string("?"));
    REQUIRE_EQ(name_after_wire(raw({0xbf, 0xbf})), std::string("??"));
    // Good text around the bad bytes stays.
    REQUIRE_EQ(name_after_wire(raw({'a', 0xff, 'z'})), std::string("a?z"));
    REQUIRE_EQ(name_after_wire(raw({0xc3, 0xa9, 0xff, 0xc3, 0xa9})),
               raw({0xc3, 0xa9, '?', 0xc3, 0xa9}));
}

TEST_CASE("agent proto: every kind of text is cleaned, not only names")
{
    const std::string bad = raw({'b', 'a', 'd', 0xff});
    const std::string clean = "bad?";

    const auto hello = decode_request(
        encode(make_request(1, hello_request{protocol_version, bad})));
    REQUIRE(hello);
    REQUIRE_EQ(std::get<hello_request>(hello->body).client, clean);

    const auto kind =
        decode_request(encode(make_request(1, create_port_request{"n", bad})));
    REQUIRE(kind);
    REQUIRE_EQ(std::get<create_port_request>(kind->body).kind, clean);

    const auto greeting = decode_server_message(
        encode(make_reply(1, hello_reply{protocol_version, bad, 1,
                                         std::vector<std::string>{bad}})));
    REQUIRE(greeting);
    const auto& hello_out =
        std::get<hello_reply>(std::get<reply>(*greeting).body);
    REQUIRE_EQ(hello_out.agent_version, clean);
    REQUIRE_EQ(hello_out.port_kinds.size(), std::size_t{1});
    REQUIRE_EQ(hello_out.port_kinds[0], clean);

    list_reply body;
    port_summary port;
    port.id = 1;
    port.name = bad;
    port.path = bad;
    body.ports.push_back(port);
    const auto listed = decode_server_message(encode(make_reply(1, body)));
    REQUIRE(listed);
    const auto& list_out = std::get<list_reply>(std::get<reply>(*listed).body);
    REQUIRE_EQ(list_out.ports[0].name, clean);
    REQUIRE_EQ(list_out.ports[0].path, clean);

    const auto failed = decode_server_message(
        encode(make_reply(1, error_reply{wire_error{bad, 3, bad}})));
    REQUIRE(failed);
    const auto& error_out =
        std::get<error_reply>(std::get<reply>(*failed).body);
    REQUIRE_EQ(error_out.error.category, clean);
    REQUIRE_EQ(error_out.error.message, clean);
    REQUIRE_EQ(error_out.error.value, 3);

    event_batch batch;
    event_record record = make_event(event_kind::error, 1);
    record.name = bad;
    record.message = bad;
    record.error = wire_error{bad, 4, bad};
    batch.events.push_back(record);
    const auto events = decode_server_message(encode(batch));
    REQUIRE(events);
    const auto& batch_out = std::get<event_batch>(*events);
    REQUIRE_EQ(batch_out.events[0].name, clean);
    REQUIRE_EQ(batch_out.events[0].message, clean);
    REQUIRE_EQ(batch_out.events[0].error.category, clean);
    REQUIRE_EQ(batch_out.events[0].error.message, clean);
}

TEST_CASE("agent proto: valid UTF-8 and control characters are unchanged")
{
    const std::vector<std::string> texts{
        "plain",
        "",
        raw({0xc3, 0xa9}),             // U+00E9, 2 bytes
        raw({0xe2, 0x82, 0xac}),       // U+20AC, 3 bytes
        raw({0xf0, 0x9f, 0x98, 0x80}), // U+1F600, 4 bytes
        raw({0xc2, 0x80}),             // U+0080, the first 2 byte
        raw({0xdf, 0xbf}),             // U+07FF, the last 2 byte
        raw({0xe0, 0xa0, 0x80}),       // U+0800, the first 3 byte
        raw({0xed, 0x9f, 0xbf}),       // U+D7FF, before surrogates
        raw({0xee, 0x80, 0x80}),       // U+E000, after surrogates
        raw({0xef, 0xbf, 0xbd}),       // U+FFFD
        raw({0xf0, 0x90, 0x80, 0x80}), // U+10000, the first 4 byte
        raw({0xf4, 0x8f, 0xbf, 0xbf}), // U+10FFFF, the last
        "a \"quoted\" \\ name\n\twith\r controls",
        std::string("nul\0inside", 10),
        "\x01\x02\x1f\x7f",
    };
    for (const std::string& text : texts)
        REQUIRE_EQ(name_after_wire(text), text);
}

TEST_CASE("agent proto: a header with invalid UTF-8 or a lone surrogate is "
          "refused")
{
    // What the encoder never writes is not accepted either, so a decoded
    // text always encodes back to itself.
    const std::string head = R"({"id":1,"op":"create_port","name":")";
    const std::string tail = R"(","kind":"mock"})";
    for (const std::string& name :
         {raw({0xff, 0xfe}), raw({0xc0, 0xaf}), raw({0xed, 0xa0, 0x80}),
          raw({'o', 'k', 0xe2, 0x82}), raw({0xf4, 0x90, 0x80, 0x80}),
          std::string(R"(\ud800)"), std::string(R"(\udc00)"),
          std::string(R"(\ud800\ud800)")})
    {
        std::string header = head;
        header += name;
        header += tail;
        require_malformed_request(header);
    }
    // A pair of surrogate escapes is one character.
    const std::string backslash_u = R"(\u)";
    const std::string escaped = backslash_u + "d83d" + backslash_u + "de00";
    const auto pair = decode_request(hand_frame(head + escaped + tail));
    REQUIRE(pair);
    REQUIRE_EQ(std::get<create_port_request>(pair->body).name,
               raw({0xf0, 0x9f, 0x98, 0x80}));
}

TEST_CASE("agent proto: text over max_text_size is cut between characters")
{
    const std::string e_acute = raw({0xc3, 0xa9});
    const std::string euro = raw({0xe2, 0x82, 0xac});
    const std::string smile = raw({0xf0, 0x9f, 0x98, 0x80});

    const std::string full(max_text_size, 'a');
    REQUIRE_EQ(name_after_wire(full), full);
    REQUIRE_EQ(name_after_wire(std::string(max_text_size + 500, 'a')), full);

    // A character that does not fit whole is left out.
    REQUIRE_EQ(name_after_wire(std::string(max_text_size - 1, 'a') + e_acute),
               std::string(max_text_size - 1, 'a'));
    // One that fits exactly stays.
    REQUIRE_EQ(name_after_wire(std::string(max_text_size - 2, 'a') + e_acute),
               std::string(max_text_size - 2, 'a') + e_acute);

    // 400 euro signs are 1200 bytes; 341 of them are 1023.
    const std::string euros = name_after_wire(repeat(euro, 400));
    REQUIRE_EQ(euros, repeat(euro, 341));
    REQUIRE(euros.size() <= max_text_size);

    const std::string smiles = name_after_wire(repeat(smile, 300));
    REQUIRE_EQ(smiles, repeat(smile, 256));

    // Bad bytes count as one each, and are cut like the rest.
    const std::string bad(2000, '\xff');
    REQUIRE_EQ(name_after_wire(bad), std::string(max_text_size, '?'));
}

TEST_CASE("agent proto: long text in a reply and a batch still decodes")
{
    const std::string long_text(5000, 'x');
    const std::string cut(max_text_size, 'x');

    const auto failed = decode_server_message(encode(
        make_reply(1, error_reply{wire_error{long_text, 1, long_text}})));
    REQUIRE(failed);
    const auto& error_out =
        std::get<error_reply>(std::get<reply>(*failed).body);
    REQUIRE_EQ(error_out.error.category, cut);
    REQUIRE_EQ(error_out.error.message, cut);

    event_batch batch;
    event_record record = make_event(event_kind::log_message, 1);
    record.name = long_text;
    record.message = long_text;
    batch.events.push_back(record);
    const auto events = decode_server_message(encode(batch));
    REQUIRE(events);
    const auto& batch_out = std::get<event_batch>(*events);
    REQUIRE_EQ(batch_out.events[0].name, cut);
    REQUIRE_EQ(batch_out.events[0].message, cut);

    // The text of 100 ports, each cut, makes a frame the reader accepts.
    list_reply body;
    for (std::size_t index = 0; index < 100; ++index)
    {
        port_summary port;
        port.id = index + 1;
        port.name = long_text;
        port.path = long_text;
        body.ports.push_back(port);
    }
    const frame wire = encode(make_reply(1, body));
    REQUIRE(encode_frame(wire));
    const auto listed = decode_server_message(wire);
    REQUIRE(listed);
    const auto& list_out = std::get<list_reply>(std::get<reply>(*listed).body);
    REQUIRE_EQ(list_out.ports.size(), std::size_t{100});
    REQUIRE_EQ(list_out.ports[99].name, cut);
}

// -- errors ------------------------------------------------------------------

TEST_CASE("agent proto: every engine error crosses the wire unchanged")
{
    for (int value = 0; value <= std::to_underlying(errc::internal); ++value)
    {
        const error_code original(value, vss::error_category());
        const wire_error wire = to_wire(original);
        REQUIRE_EQ(wire.category, std::string("vss"));
        REQUIRE_EQ(wire.value, value);
        REQUIRE_EQ(wire.message, original.message());

        const error_code back = from_wire(wire);
        REQUIRE_EQ(back.value(), value);
        REQUIRE(back.category() == vss::error_category());
        REQUIRE_EQ(back.message(), original.message());
    }
}

TEST_CASE("agent proto: every protocol error crosses the wire unchanged")
{
    for (int value = 0; value <= std::to_underlying(protocol_errc::not_running);
         ++value)
    {
        const error_code original(value, protocol_category());
        const wire_error wire = to_wire(original);
        REQUIRE_EQ(wire.category, std::string("vss.agent"));
        REQUIRE_EQ(wire.value, value);
        REQUIRE_EQ(wire.message, original.message());

        const error_code back = from_wire(wire);
        REQUIRE_EQ(back.value(), value);
        REQUIRE(back.category() == protocol_category());
    }
}

TEST_CASE("agent proto: operating system errors keep value and category")
{
    const error_code os_error(5, boost::system::system_category());
    const error_code os_back = from_wire(to_wire(os_error));
    REQUIRE_EQ(os_back.value(), 5);
    REQUIRE(os_back.category() == boost::system::system_category());
    REQUIRE_EQ(os_back.message(), os_error.message());

    const error_code generic(2, boost::system::generic_category());
    const error_code generic_back = from_wire(to_wire(generic));
    REQUIRE_EQ(generic_back.value(), 2);
    REQUIRE(generic_back.category() == boost::system::generic_category());

    // Not a value this side knows, but the other side may.
    const error_code odd(-123456, boost::system::system_category());
    const error_code odd_back = from_wire(to_wire(odd));
    REQUIRE_EQ(odd_back.value(), -123456);
    REQUIRE(odd_back.category() == boost::system::system_category());
}

TEST_CASE("agent proto: an out of range engine or protocol value is internal")
{
    const error_code internal = vss::make_error_code(errc::internal);
    for (const int value :
         {std::to_underlying(errc::internal) + 1, -1, 1000,
          std::numeric_limits<int>::max(), std::numeric_limits<int>::min()})
    {
        REQUIRE_EQ(from_wire(wire_error{"vss", value, "agent text"}), internal);
    }
    for (const int value :
         {std::to_underlying(protocol_errc::not_running) + 1, -1, 1000,
          std::numeric_limits<int>::max(), std::numeric_limits<int>::min()})
    {
        REQUIRE_EQ(from_wire(wire_error{"vss.agent", value, "agent text"}),
                   internal);
    }
}

TEST_CASE("agent proto: an unknown error category is an I/O error")
{
    const error_code io = vss::make_error_code(errc::io_error);
    for (const char* category :
         {"mystery", "", "VSS", "vss ", "vss.agent.x", "asio.misc", "iostream"})
    {
        REQUIRE_EQ(from_wire(wire_error{category, 1, "text"}), io);
        REQUIRE_EQ(from_wire(wire_error{category, 0, ""}), io);
    }
}

TEST_CASE("agent proto: an error code survives a reply on the wire")
{
    const std::vector<error_code> codes{
        vss::make_error_code(errc::timeout),
        vss::make_error_code(errc::port_exhausted),
        make_error_code(protocol_errc::busy),
        make_error_code(protocol_errc::not_running),
        error_code(13, boost::system::system_category()),
        error_code(2, boost::system::generic_category()),
    };
    for (const error_code& code : codes)
    {
        const reply sent = make_reply(4, error_reply{to_wire(code)});
        const auto decoded = decode_server_message(encode(sent));
        REQUIRE(decoded);
        const auto& answer = std::get<reply>(*decoded);
        const auto& error = std::get<error_reply>(answer.body);
        REQUIRE_EQ(from_wire(error.error), code);
        REQUIRE_EQ(error.error.message, code.message());
    }
}

TEST_CASE("agent proto: the protocol category is named vss.agent")
{
    REQUIRE_EQ(std::string(protocol_category().name()),
               std::string("vss.agent"));
    const error_code code = make_error_code(protocol_errc::busy);
    REQUIRE(code.category() == protocol_category());
    REQUIRE_EQ(code.value(), std::to_underlying(protocol_errc::busy));
    REQUIRE(code);
    REQUIRE(!make_error_code(protocol_errc::success));

    // The enumeration converts to an error code by itself.
    const error_code implicit = protocol_errc::version_mismatch;
    REQUIRE_EQ(implicit.value(),
               std::to_underlying(protocol_errc::version_mismatch));
    REQUIRE(implicit == protocol_errc::version_mismatch);
}

TEST_CASE("agent proto: every protocol error has its own message")
{
    const std::string unknown = protocol_category().message(9999);
    REQUIRE(!unknown.empty());
    REQUIRE_NE(protocol_category().message(-1), std::string());

    std::vector<std::string> seen;
    for (int value = 0; value <= std::to_underlying(protocol_errc::not_running);
         ++value)
    {
        const std::string text = protocol_category().message(value);
        REQUIRE(!text.empty());
        REQUIRE_NE(text, unknown);
        for (const std::string& earlier : seen)
            REQUIRE_NE(text, earlier);
        seen.push_back(text);
        // The code says the same through its own message.
        REQUIRE_EQ(error_code(value, protocol_category()).message(), text);
    }
}

TEST_CASE("agent proto: peek_request_id finds the id of a bad request")
{
    // The body is wrong, the id is there.
    const frame bad_body = hand_frame(R"({"id":7,"op":"create_port"})");
    require_request_error(bad_body, protocol_errc::malformed_message);
    REQUIRE_EQ(peek_request_id(bad_body), std::uint64_t{7});

    const frame unknown = hand_frame(R"({"id":9,"op":"explode"})");
    require_request_error(unknown, protocol_errc::unknown_operation);
    REQUIRE_EQ(peek_request_id(unknown), std::uint64_t{9});

    REQUIRE_EQ(
        peek_request_id(hand_frame(R"({"id":18446744073709551615,"op":7})")),
        max_u64);
    REQUIRE_EQ(peek_request_id(hand_frame(R"({"id":3})", {1, 2})),
               std::uint64_t{3});

    for (const request& value : sample_requests())
        REQUIRE_EQ(peek_request_id(encode(value)), value.id);
}

TEST_CASE("agent proto: peek_request_id is 0 when there is no id")
{
    for (const char* header :
         {"garbage", "", "{", "[]", "[7]", "42", "null", R"("id")",
          R"({"op":"list"})", R"({"id":"7"})", R"({"id":-3})", R"({"id":7.5})",
          R"({"id":null})", R"({"id":0,"op":"list"})", R"({"ID":7})",
          R"({"id":18446744073709551616})"})
    {
        REQUIRE_EQ(peek_request_id(hand_frame(header)), std::uint64_t{0});
    }
    REQUIRE_EQ(peek_request_id(hand_frame(raw({0xff, 0xfe, 0x00}))),
               std::uint64_t{0});
    REQUIRE_EQ(peek_request_id(frame{}), std::uint64_t{0});
    REQUIRE_EQ(peek_request_id(hand_frame(std::string(100000, '['))),
               std::uint64_t{0});
}
