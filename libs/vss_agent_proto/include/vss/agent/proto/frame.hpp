//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/agent/proto/error.hpp>
#include <vss/core/bytes.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace vss::agent
{

/// The version of the wire protocol.  A client and an agent talk only when
/// both have the same number: there is no negotiation and no older form is
/// read.  Any change to a frame or a message changes it.
inline constexpr std::uint32_t protocol_version = 1;

/// The most bytes a frame may have after its length prefix.  A reader refuses
/// a longer one as soon as it has read the prefix, before it buffers anything.
inline constexpr std::size_t max_frame_size = std::size_t{1} << 20;

/// The bytes of the length prefix.
inline constexpr std::size_t frame_prefix_size = 4;

/// One message on the wire.
///
///     u32le  size of everything after this field   (6 .. max_frame_size)
///     u32le  size of the header                    (2 .. size - 4)
///     header a JSON object, UTF-8
///     blob   the rest: bytes the header describes (written data, a batch of
///            traffic), so no payload is ever escaped into JSON
struct frame
{
    std::string header;
    byte_buffer blob;

    friend bool operator==(const frame&, const frame&) = default;
};

/// The bytes of a frame.  `protocol_errc::oversized_frame` when it would be
/// longer than `max_frame_size`, and `malformed_frame` for an empty header.
result<byte_buffer> encode_frame(const frame& value);

/// Cuts a byte stream into frames.
///
/// Feed it what was read, in pieces of any size, and take frames until it
/// says it needs more.  An error is final: the stream cannot be read past a
/// bad length, so the connection is closed.
class frame_reader
{
public:
    frame_reader() = default;

    /// A reader that refuses a frame with more than `limit` bytes after its
    /// length prefix.  Never more than `max_frame_size`.
    explicit frame_reader(std::size_t limit) noexcept { set_limit(limit); }

    /// Changes the most bytes the next frames may have, for a connection
    /// that is allowed little before it has said who it is and more after.
    void set_limit(std::size_t limit) noexcept
    {
        limit_ = limit < max_frame_size ? limit : max_frame_size;
    }

    /// Adds bytes that were read from the connection.
    void feed(bytes_view data);

    /// The next whole frame, nothing when more bytes are needed, or the error
    /// that ended the stream.
    result<std::optional<frame>> next();

    /// Bytes held that are not yet part of a returned frame.
    std::size_t buffered() const noexcept { return buffer_.size() - start_; }

private:
    byte_buffer buffer_;
    std::size_t start_ = 0;
    std::size_t limit_ = max_frame_size;
    error_code failed_;
};

} // namespace vss::agent
