//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz harness for the control API. Any bytes may arrive on the gateway's
// endpoints, so the frame reader and the message decoders must refuse bad
// input with an error and never crash. The input is read twice as the bytes
// of a connection, once whole and once in pieces whose sizes come from the
// input itself, and both must give the same frames and the same error. The
// reader starts with the small limit of a connection that has not said hello
// and takes the full limit after its first frame, as the gateway does. Every
// frame is decoded as a request and as a reply; what decodes must encode
// again, and the frame made from it must decode to the same value. The input
// is also split into a header and a blob and decoded directly, because a
// mutated length prefix would otherwise keep most inputs from reaching the
// JSON decoder.
//

#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/control/proto/error.hpp>
#include <vss/control/proto/message.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace
{

namespace agent = vss::agent;
namespace control = vss::control;

/// A property did not hold: stop, so the fuzzer keeps the input.
void require(bool holds)
{
    if (!holds)
    {
        std::abort();
    }
}

// -- the frames of a stream --------------------------------------------------

/// What a reader makes of a stream: its frames in order, and the error that
/// ended it, if one did.
struct reading
{
    std::vector<agent::frame> frames;
    vss::error_code ended;
};

/// The size of the next piece of a stream, from the byte it starts with.
std::size_t piece_size(std::uint8_t first)
{
    return std::size_t{1} + (first % 64u);
}

/// An error is final: the same one for every call, whatever is fed after.
void check_final(agent::frame_reader& reader, vss::bytes_view rest,
                 const vss::error_code& expected)
{
    for (int round = 0; round < 2; ++round)
    {
        const auto again = reader.next();
        require(!again && again.error() == expected);
    }
    std::size_t at = 0;
    while (at < rest.size())
    {
        const std::size_t count =
            rest.size() - at < 64 ? rest.size() - at : std::size_t{64};
        reader.feed(rest.subspan(at, count));
        at += count;
        const auto again = reader.next();
        require(!again && again.error() == expected);
    }
}

/// Reads the whole input, all at once or in pieces.
reading read_stream(vss::bytes_view input, bool in_pieces)
{
    reading out;
    agent::frame_reader reader(control::max_hello_frame_size);
    std::size_t consumed = 0;
    std::size_t at = 0;
    do
    {
        if (at < input.size())
        {
            std::size_t count = input.size() - at;
            if (in_pieces)
            {
                const std::size_t wanted = piece_size(input[at]);
                if (wanted < count)
                {
                    count = wanted;
                }
            }
            reader.feed(input.subspan(at, count));
            at += count;
        }
        for (;;)
        {
            auto next = reader.next();
            if (!next)
            {
                out.ended = next.error();
                check_final(reader, input.subspan(at), out.ended);
                return out;
            }
            std::optional<agent::frame> ready = std::move(*next);
            if (!ready)
            {
                break;
            }
            consumed += agent::frame_prefix_size * 2 + ready->header.size() +
                        ready->blob.size();
            out.frames.push_back(std::move(*ready));
            // The first frame was the hello; the rest may be larger.
            if (out.frames.size() == 1)
            {
                reader.set_limit(agent::max_frame_size);
            }
        }
    } while (at < input.size());

    // Without an error every byte is in a frame or still waiting for one.
    require(consumed + reader.buffered() == input.size());
    return out;
}

// -- the messages of a frame -------------------------------------------------

/// The frame as a connection carries it: encoded and read back.  Nothing when
/// it is too large to send, which is the one thing that may stop an encoder.
std::optional<agent::frame> carried(const agent::frame& value)
{
    const auto bytes = agent::encode_frame(value);
    if (!bytes)
    {
        require(bytes.error() ==
                agent::make_error_code(agent::protocol_errc::oversized_frame));
        return std::nullopt;
    }
    agent::frame_reader reader;
    reader.feed(*bytes);
    auto next = reader.next();
    require(next.has_value());
    std::optional<agent::frame> ready = std::move(*next);
    if (!ready || !(*ready == value))
    {
        std::abort();
    }
    require(reader.buffered() == 0);
    return ready;
}

bool is_decode_error(const vss::error_code& error,
                     bool operation_may_be_unknown)
{
    if (error == control::make_error_code(control::api_errc::invalid_argument))
    {
        return true;
    }
    return operation_may_be_unknown &&
           error == control::make_error_code(control::api_errc::unsupported);
}

void check_request(const agent::frame& value)
{
    const auto decoded = control::decode_request(value);
    if (!decoded)
    {
        require(is_decode_error(decoded.error(), true));
        return;
    }
    require(decoded->id != 0);
    require(control::peek_request_id(value) == decoded->id);

    const agent::frame again = control::encode(*decoded);
    const auto sent = carried(again);
    if (!sent)
    {
        return;
    }
    const auto second = control::decode_request(*sent);
    require(second && *second == *decoded);
}

void check_reply(const agent::frame& value)
{
    const auto decoded = control::decode_reply(value);
    if (!decoded)
    {
        require(is_decode_error(decoded.error(), false));
        return;
    }
    const agent::frame again = control::encode(*decoded);
    const auto sent = carried(again);
    if (!sent)
    {
        return;
    }
    const auto second = control::decode_reply(*sent);
    require(second && *second == *decoded);
}

void check_frame(const agent::frame& value)
{
    // Never throws, and the id is what the request decoder would find.
    static_cast<void>(control::peek_request_id(value));
    check_request(value);
    check_reply(value);
}

/// The input as one frame: the first byte says how much of the rest is the
/// header.
agent::frame direct_frame(vss::bytes_view input)
{
    agent::frame out;
    if (input.empty())
    {
        return out;
    }
    const vss::bytes_view rest = input.subspan(1);
    const std::size_t header_size = rest.size() * input[0] / 255;
    out.header.assign(reinterpret_cast<const char*>(rest.data()), header_size);
    out.blob.assign(rest.begin() + static_cast<std::ptrdiff_t>(header_size),
                    rest.end());
    return out;
}

} // namespace

// libFuzzer looks for exactly this name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    const vss::bytes_view input(data, size);

    const reading in_pieces = read_stream(input, true);
    const reading whole = read_stream(input, false);
    require(in_pieces.frames == whole.frames);
    require(in_pieces.ended == whole.ended);

    for (const agent::frame& value : in_pieces.frames)
    {
        check_frame(value);
    }
    check_frame(direct_frame(input));
    return 0;
}
