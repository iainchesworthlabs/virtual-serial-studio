//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "tx_order.hpp"

#include <algorithm>

namespace vss::detail
{

void tx_order::add_marks(std::span<const tx_break_mark> breaks,
                         std::span<const tx_write_mark> writes)
{
    for (const auto& mark : breaks)
    {
        if (held_ && mark.on_time == held_on_time_)
        {
            held_seen_ = true;
            if (!mark.open)
                ended_ = mark;
            continue;
        }
        const auto same = std::find_if(
            breaks_.begin(), breaks_.end(),
            [&](const tx_break_mark& known)
            { return known.on_time == mark.on_time; });
        if (same != breaks_.end())
        {
            // The same break given again: it may have ended since.
            if (!mark.open)
                *same = mark;
            continue;
        }
        breaks_.push_back(mark);
    }
    for (const auto& mark : writes)
        writes_.push_back(mark);
}

void tx_order::add_bytes(std::span<const std::uint8_t> bytes)
{
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void tx_order::emit_bytes(std::vector<tx_piece>& out, std::uint64_t limit)
{
    while (start_ < limit && !buffer_.empty())
    {
        while (!writes_.empty() && writes_.front().offset <= start_)
        {
            stamp_ = writes_.front().time;
            writes_.pop_front();
        }
        std::uint64_t end = std::min<std::uint64_t>(limit, start_ + buffer_.size());
        if (!writes_.empty())
            end = std::min(end, writes_.front().offset);

        tx_piece piece;
        piece.what = tx_piece::kind::bytes;
        piece.stamp = stamp_;
        const auto count = static_cast<std::size_t>(end - start_);
        piece.bytes.assign(buffer_.begin(),
                           buffer_.begin() + static_cast<std::ptrdiff_t>(count));
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(count));
        start_ = end;
        out.push_back(std::move(piece));
    }
}

std::vector<tx_piece> tx_order::take()
{
    std::vector<tx_piece> out;
    for (;;)
    {
        if (held_)
        {
            if (!ended_)
                break;
            tx_piece piece;
            piece.what = tx_piece::kind::break_end;
            piece.on_time = held_on_time_;
            piece.off_time = ended_->off_time;
            out.push_back(std::move(piece));
            held_ = false;
            ended_.reset();
            continue;
        }

        const std::uint64_t end = start_ + buffer_.size();
        if (breaks_.empty())
        {
            emit_bytes(out, end);
            break;
        }

        const tx_break_mark mark = breaks_.front();
        if (mark.on_offset > end)
        {
            // The bytes before the break have not all been read.
            emit_bytes(out, end);
            break;
        }
        emit_bytes(out, mark.on_offset);
        breaks_.pop_front();

        tx_piece piece;
        piece.on_time = mark.on_time;
        piece.off_time = mark.off_time;
        if (mark.open)
        {
            piece.what = tx_piece::kind::break_begin;
            held_ = true;
            held_on_time_ = mark.on_time;
            held_seen_ = true;
            ended_.reset();
        }
        else
        {
            piece.what = tx_piece::kind::break_complete;
        }
        out.push_back(std::move(piece));
    }
    return out;
}

void tx_order::restart()
{
    buffer_.clear();
    start_ = 0;
    breaks_.clear();
    writes_.clear();
    stamp_.reset();
    ended_.reset();
    held_seen_ = false;
}

void tx_order::settle(std::uint64_t now)
{
    if (held_ && !held_seen_ && !ended_)
    {
        tx_break_mark mark;
        mark.on_time = held_on_time_;
        mark.off_time = now;
        ended_ = mark;
    }
}

} // namespace vss::detail
