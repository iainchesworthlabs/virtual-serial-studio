//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Puts the bytes a program wrote to a Windows driver port back in order with the
// break calls and the write calls it made around them.
//
// The driver gives the hub one stream of bytes, and, apart from it, marks: where
// each SetCommBreak and ClearCommBreak fell among the bytes, and where each
// WriteFile began (drivers/windows/vsscom/public.h, IOCTL_VSSCOM_HUB_READ_BREAKS).
// This class takes both and gives back pieces in the order a wire would show them:
// bytes up to a break, the break, and the bytes after it. Bytes the program wrote
// while a break was held follow its end, as a UART holds its output while the line
// is in break. Each run of bytes carries the time of the write that made it.
//
// It has no operating system calls, so it is tested on every platform. Times are
// the driver's raw counts; the caller converts them.
//

#ifndef VSS_PORTS_TX_ORDER_HPP
#define VSS_PORTS_TX_ORDER_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace vss::detail
{

struct tx_break_mark
{
    std::uint64_t on_time = 0;
    std::uint64_t off_time = 0;
    std::uint64_t on_offset = 0;
    std::uint64_t off_offset = 0;
    /// Still held: `off_time` and `off_offset` mean nothing.
    bool open = false;
};

struct tx_write_mark
{
    std::uint64_t time = 0;
    std::uint64_t offset = 0;
};

struct tx_piece
{
    enum class kind
    {
        bytes,
        break_begin,
        break_end,
        break_complete
    };

    kind what = kind::bytes;
    /// For bytes.
    std::vector<std::uint8_t> bytes;
    /// For bytes: when the program called WriteFile, when the driver told us.
    std::optional<std::uint64_t> stamp;
    /// For the three break kinds. `off_time` means nothing for a begin.
    std::uint64_t on_time = 0;
    std::uint64_t off_time = 0;
};

class tx_order
{
public:
    /// Marks the driver gave, oldest first. A break that is held is given again
    /// until it ends; the repeat changes nothing.
    void add_marks(std::span<const tx_break_mark> breaks,
                   std::span<const tx_write_mark> writes);

    /// Bytes the hub just read from the driver, after the marks that go with them
    /// were added.
    void add_bytes(std::span<const std::uint8_t> bytes);

    /// What is ready now, in order. Call after add_marks or add_bytes.
    std::vector<tx_piece> take();

    /// The hub handle is new: the driver counts from 0, and whatever was waiting
    /// here is gone. A break that is held stays held.
    void restart();

    /// After a restart and the first read of the marks: a break that was held
    /// before the restart and that the driver no longer reports ended while the
    /// hub had no handle. It ends at `now`, the best time there is.
    void settle(std::uint64_t now);

    /// A break has begun and not ended: bytes behind it wait.
    bool holding() const noexcept { return held_; }

    /// Bytes waiting behind a break or for a mark.
    std::size_t waiting() const noexcept { return buffer_.size(); }

private:
    void emit_bytes(std::vector<tx_piece>& out, std::uint64_t limit);

    // Offsets of the bytes in `buffer_`: start_, start_ + 1, ...
    std::deque<std::uint8_t> buffer_;
    std::uint64_t start_ = 0;
    std::deque<tx_break_mark> breaks_;
    std::deque<tx_write_mark> writes_;
    std::optional<std::uint64_t> stamp_;
    bool held_ = false;
    std::uint64_t held_on_time_ = 0;
    /// The end of the break that is held, once the driver has given it.
    std::optional<tx_break_mark> ended_;
    /// The driver has reported the break that is held since the last restart.
    bool held_seen_ = false;
};

} // namespace vss::detail

#endif // VSS_PORTS_TX_ORDER_HPP
