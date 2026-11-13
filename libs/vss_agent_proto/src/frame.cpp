//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/agent/proto/frame.hpp>

#include <utility>

namespace vss::agent
{
namespace
{

/// The smallest header is `{}`.
constexpr std::size_t min_header_size = 2;

void put_u32(byte_buffer& out, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<byte>((value >> shift) & 0xffu));
}

std::uint32_t get_u32(const byte* data) noexcept
{
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(data[index]) << (index * 8);
    return value;
}

} // namespace

result<byte_buffer> encode_frame(const frame& value)
{
    if (value.header.size() < min_header_size)
        return make_error_code(protocol_errc::malformed_frame);
    if (value.header.size() > max_frame_size ||
        value.blob.size() > max_frame_size)
        return make_error_code(protocol_errc::oversized_frame);
    const std::size_t size =
        frame_prefix_size + value.header.size() + value.blob.size();
    if (size > max_frame_size)
        return make_error_code(protocol_errc::oversized_frame);

    byte_buffer out;
    out.reserve(frame_prefix_size + size);
    put_u32(out, static_cast<std::uint32_t>(size));
    put_u32(out, static_cast<std::uint32_t>(value.header.size()));
    const bytes_view header = as_bytes(value.header);
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), value.blob.begin(), value.blob.end());
    return out;
}

void frame_reader::feed(bytes_view data)
{
    if (failed_)
        return;
    // Drop what earlier frames used once it is most of the buffer, so a long
    // connection does not grow it.
    if (start_ != 0 && start_ >= buffer_.size() / 2)
    {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(start_));
        start_ = 0;
    }
    buffer_.insert(buffer_.end(), data.begin(), data.end());
}

result<std::optional<frame>> frame_reader::next()
{
    if (failed_)
        return failed_;
    const std::size_t available = buffer_.size() - start_;
    if (available < frame_prefix_size)
        return std::optional<frame>{};

    const byte* const base = buffer_.data() + start_;
    const std::size_t size = get_u32(base);
    if (size > limit_)
    {
        failed_ = make_error_code(protocol_errc::oversized_frame);
        return failed_;
    }
    if (size < frame_prefix_size + min_header_size)
    {
        failed_ = make_error_code(protocol_errc::malformed_frame);
        return failed_;
    }
    // The header size is checked as soon as it is here, not when the whole
    // frame is, so a bad one is refused before its body is buffered.
    if (available < 2 * frame_prefix_size)
        return std::optional<frame>{};
    const std::size_t header_size = get_u32(base + frame_prefix_size);
    if (header_size < min_header_size || header_size > size - frame_prefix_size)
    {
        failed_ = make_error_code(protocol_errc::malformed_frame);
        return failed_;
    }
    if (available < frame_prefix_size + size)
        return std::optional<frame>{};

    frame out;
    const byte* const header = base + 2 * frame_prefix_size;
    out.header.assign(reinterpret_cast<const char*>(header), header_size);
    const byte* const blob = header + header_size;
    out.blob.assign(blob, base + frame_prefix_size + size);
    start_ += frame_prefix_size + size;
    if (start_ == buffer_.size())
    {
        buffer_.clear();
        start_ = 0;
    }
    return std::optional<frame>{std::move(out)};
}

} // namespace vss::agent
