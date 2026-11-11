//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_PORTS_VCOM_DRIVER_HPP
#define VSS_PORTS_VCOM_DRIVER_HPP

#include <vss/core/error.hpp>
#include <vss/ports/virtual_port.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace vss {

/// The numbers a driver port may be given: `COM1` to `COM255`.
constexpr unsigned min_driver_com_number = 1;
constexpr unsigned max_driver_com_number = 255;

/// True when the Virtual Serial Studio COM port driver is installed: its
/// control device is there, or at least one port is.
///
/// A Windows `COMx` is a kernel device: no user-mode process can create one,
/// so a real drop-in replacement for com0com needs this driver.  Detection
/// lists the driver's device interfaces without opening any of them, so it is
/// safe to call whenever a dialog refreshes.
bool driver_available();

/// The driver's port interface version, or an empty string.  With the control
/// device installed it asks that; otherwise it opens a free port for a moment,
/// so it is empty when every port is taken.
std::string driver_version();

/// Makes a port and returns the transport that carries bytes between the hub
/// and that port.  `path` is the COM name an application opens.
///
/// With the control device installed this asks it for a new port and waits,
/// usually a second or two, while Windows builds it.  The port is the caller's
/// alone and is removed when the transport closes, or when the process ends.
/// Only the user who made it can open its hub end.  Without the control device
/// it takes a free port of the ones that exist, which only an administrator can
/// open.
///
/// `com_number` is the COM number the port should have, 1 to 255, as Device
/// Manager's Advanced > COM Port Number gives one; 0 leaves it to Windows,
/// which takes the lowest free number.  A port keeps the number it was given
/// for as long as it lives, and the same slot keeps its number from one run to
/// the next, but a port without a requested number may land on a different one
/// if the free numbers have changed, so a program that opens a fixed name
/// should ask for it.  A number needs the helper service
/// (`drivers/windows/vsscom_helper`), which does what Device Manager does: it
/// claims the number in the COM name database and restarts the port, so
/// creating the port takes a second or two longer.  Not available for a port
/// that already existed.
///
/// Returns `errc::unsupported` when the driver is absent (or a number was asked
/// for and the helper service is not installed or too old),
/// `errc::invalid_argument` for a number outside 1 to 255,
/// `errc::already_exists` when another device has the number, a real port,
/// com0com or anything else (the port is not made),
/// `errc::device_busy` when there is no control device and every port is in use,
/// `errc::port_exhausted` when the control device has no slot left or the user
/// already holds 16 ports,
/// `errc::permission_denied` when the caller may not open it and
/// `errc::timeout` when the port did not appear.  See
/// [drivers/windows/README.md](../../../../drivers/windows/README.md) for the
/// two ends of a port.
result<virtual_port_result> create_driver_port(
    const transport::executor_type& executor, const std::string& name,
    unsigned com_number = 0);

/// Gives a driver port a different COM number, as Device Manager's Advanced >
/// COM Port Number does for any port.  `port` is the transport that
/// `create_driver_port` returned (an endpoint's transport); the result is the
/// new COM name, `COM` followed by the number, and the transport's
/// `public_path()` is that name from then on.
///
/// The port has to be restarted for the driver to take the number, so no
/// program may have it open, and for a moment the hub end is closed: the port
/// loops back, and reads and writes the studio has waiting resume on the new
/// handle.  The call takes a second or two, so do not make it on a thread that
/// serves the user interface.  A failure leaves the old number in place.
///
/// `errc::invalid_argument` for a number outside 1 to 255 or a transport that is
/// not a driver port, `errc::unsupported` for a port that already existed (not
/// made through the control device or the helper) or when the helper service
/// is missing or too old, `errc::already_exists` when another device has the
/// number, `errc::device_busy` when a program has the port open, and
/// `errc::not_open` when the transport is closed.
result<std::string> set_driver_com_number(transport& port, unsigned com_number);

/// Takes a driver port back from the program that has it open, as a driver
/// revoke request does: the program's waiting reads, writes and event waits
/// end, every later request on its handle fails, and another program can open
/// the port at once.  The program keeps running and keeps its handle, which no
/// longer does anything.  `port` is the transport that `create_driver_port`
/// returned.  The answer is the process that had the port open, or 0 when none
/// did (the call then changes nothing).
///
/// Only the user who made the port holds its hub end, so only that user can
/// revoke it; another user's request cannot get that far.  `errc::unsupported`
/// where there is no driver, `errc::invalid_argument` for a transport that is
/// not a driver port, `errc::not_open` when the transport is closed and
/// `errc::io_error` when the driver refused (a driver older than version 3
/// does not know the request).
result<std::uint32_t> revoke_driver_port(transport& port);

/// A line error a driver port can raise at the program on its COM end, as a
/// bad character time on a wire does.
enum class driver_line_error
{
    /// A break: `CE_BREAK`, and `EV_BREAK` for `WaitCommEvent`.
    break_condition,
    /// `CE_FRAME` and `EV_ERR`.
    framing,
    /// `CE_RXPARITY` and `EV_ERR`.
    parity,
    /// A character was lost in the receiver: `CE_OVERRUN` and `EV_ERR`.
    overrun,
    /// The receive queue was full: `CE_RXOVER`, and no event.
    queue_overrun,
};

/// Raises `error` at the program that has the driver port open.  `port` is the
/// transport that `create_driver_port` returned.
///
/// The program sees what it would see on hardware: `ClearCommError` reports the
/// error until it is called, `WaitCommEvent` reports `EV_ERR` or `EV_BREAK` when
/// the program's mask has it, and, when the program set `fAbortOnError`, its
/// reads and writes end with `ERROR_OPERATION_ABORTED` until it calls
/// `ClearCommError`.  `byte` is the character the receiver made of the bad
/// character time; the program reads it in its place in the stream unless its
/// settings replace it (`fErrorChar` for a parity or framing error).  Without a
/// byte only such a replacement character enters the stream.
///
/// The error keeps its place among the bytes: it takes effect after every byte
/// this transport has already handed to the driver, and before any written
/// after the call.  The call does not wait for that; it returns once the driver
/// has the request.  An error is dropped when no program has the port open,
/// and when the port is renumbered or closed while the error still waits behind
/// bytes the program has not read.
///
/// `errc::unsupported` where there is no driver, `errc::invalid_argument` for a
/// transport that is not a driver port, `errc::not_open` when the transport is
/// closed and `errc::io_error` when the driver refused.
result<void> raise_driver_line_error(transport& port, driver_line_error error,
                                     std::optional<std::uint8_t> byte = {});

/// What the driver says about the program that has the application end of a
/// port open, and the line settings that program has chosen.  The numbers are
/// those of `SERIAL_LINE_CONTROL`.
struct driver_peer_info
{
    /// A program has the COM port open.
    bool open = false;
    /// The process that has it open, or 0 when none does.
    std::uint32_t process_id = 0;
    std::uint32_t baud_rate = 0;
    /// 5 to 8.
    std::uint32_t data_bits = 0;
    /// 0 none, 1 odd, 2 even, 3 mark, 4 space.
    std::uint32_t parity = 0;
    /// 0 one, 1 one and a half, 2 two.
    std::uint32_t stop_bits = 0;

    friend bool operator==(const driver_peer_info&,
                           const driver_peer_info&) = default;
};

/// What the driver last told this process about the program on the COM port
/// `com_name` (such as `COM7`), for a port this process holds the hub end of.
/// The driver announces every change, so the answer is current to within a
/// few milliseconds and the call does not touch the device.  Empty for any
/// other port, which includes ports of other processes, com0com and hardware,
/// and everywhere but Windows.
std::optional<driver_peer_info> driver_peer(const std::string& com_name);

} // namespace vss

#endif // VSS_PORTS_VCOM_DRIVER_HPP
