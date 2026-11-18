//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_PORTS_SERIAL_ENUMERATION_HPP
#define VSS_PORTS_SERIAL_ENUMERATION_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss {

/// One end of a com0com pair, parsed from a PnP instance id.
struct com0com_end
{
    int pair = 0;
    /// `"A"` or `"B"`.
    std::string side;
};

/// `"USB Serial Device (COM13)"` -> `"COM13"`; empty when the description
/// does not end in a COM port name.
std::string parse_com_port_name(std::string_view description);

/// `"COM0COM\\PORT\\CNCA1"` -> pair 1, side `"A"`; `nullopt` otherwise.
std::optional<com0com_end> parse_com0com_instance_id(
    std::string_view instance_id);

/// A serial device present on this machine.
struct serial_device_info
{
    /// `COM3` on Windows, `/dev/ttyUSB0` on POSIX.
    std::string path;
    /// Friendly name, e.g. `com0com - serial port emulator (COM3)`.
    std::string description;
    /// PnP hardware id, when the platform exposes one.
    std::string hardware_id;
    /// True for ports that do not correspond to a physical UART.
    bool virtual_device = false;
    /// True when the device is one end of a com0com pair.
    bool com0com = false;
    /// com0com pair index (CNCA1 / CNCB1 -> 1), or -1.
    int com0com_pair = -1;
    /// `"A"` or `"B"` for com0com ports.
    std::string com0com_side;
};

/// Enumerates serial devices, sorted by path.
std::vector<serial_device_info> enumerate_serial_devices();

/// Convenience view used by pickers and command completion.
std::vector<std::string> enumerate_serial_port_names();

/// The COM numbers (1 to 255) set in a COM name database bitmap: bit
/// `(n - 1) % 8` of byte `(n - 1) / 8` is number `n`.  Bytes beyond the 32 that
/// cover 1 to 255 are ignored.
std::vector<unsigned>
com_numbers_in_bitmap(const std::vector<unsigned char>& bitmap);

/// The COM numbers Windows will not give to a new port, which is more than the
/// ports it lists: the COM name database (`ComDB`) keeps the number of a port
/// that is unplugged or was removed, and a name can resolve with no device
/// behind it.  Sorted, without duplicates.  `nullopt` where the computer
/// cannot be asked (every platform but Windows, or a database that cannot be
/// read), so a caller never mistakes "unknown" for "all free".
std::optional<std::vector<unsigned>> reserved_com_numbers();

/// What holds a COM number.
enum class com_holder_kind
{
    /// A device that is present: a real port, com0com, a running Studio port.
    present_device,
    /// A Studio port that was removed. Windows keeps its entry and the number;
    /// a new Studio port may take the number (the helper removes the entry).
    studio_port_gone,
    /// A port of something else that is not plugged in. Not Studio's to take.
    other_port_gone,
    /// `\\.\COMn` resolves but no listed device has the name (com0com, a
    /// driver that made the name itself). Taken.
    name_only,
    /// Only the COM name database marks the number; no device has it. A new
    /// Studio port may take the number.
    database_only
};

/// A device of the Ports class, present or not, with its PortName.
struct com_port_entry
{
    unsigned number = 0;
    std::string instance_id;
    std::string description;
    bool present = false;
};

/// One COM number that is in use or kept, and by what.
struct com_number_holder
{
    unsigned number = 0;
    com_holder_kind kind = com_holder_kind::present_device;
    /// The device's PnP instance id; empty for `name_only` and `database_only`.
    std::string instance_id;
    /// Friendly name of the device; empty when there is none.
    std::string description;

    /// True when a new Studio port can be given this number.
    bool reusable_by_studio() const noexcept
    {
        return kind == com_holder_kind::studio_port_gone ||
               kind == com_holder_kind::database_only;
    }
};

/// True for the instance id of a Studio port (`SWD\VSSCOMUM\...` of the
/// user-mode driver, `VSSCOM\PORT\...` of the kernel-mode one). Not case
/// sensitive.
bool is_studio_port_instance_id(std::string_view instance_id);

/// Says what holds each COM number in `database` (the numbers the COM name
/// database marks), `resolving` (the names that resolve) or `entries`.  One
/// row per number, sorted by number.  A present device wins over everything,
/// then a name that resolves, then a port of something else that is gone, then
/// a Studio port that is gone, and last a number only the database marks.
std::vector<com_number_holder>
classify_com_numbers(const std::vector<unsigned>& database,
                     const std::vector<unsigned>& resolving,
                     const std::vector<com_port_entry>& entries);

/// The COM numbers a new Studio port cannot take: a device that is present
/// holds them, the name resolves, or an unplugged device of something else
/// keeps them for when it returns.  A number that only a Studio port that is
/// gone keeps, or that only the COM name database marks with no device, is
/// not here: the helper clears it when a port asks for it.  Sorted, without
/// duplicates.  `nullopt` where `com_number_holders` is.
std::optional<std::vector<unsigned>> unavailable_com_numbers();

/// Which COM numbers are in use or kept, and by what. Use `reusable_by_studio`
/// to tell the numbers a new Studio port may take from the others. `nullopt`
/// where the computer cannot be asked (every platform but Windows, or a
/// database that cannot be read).
std::optional<std::vector<com_number_holder>> com_number_holders();

/// Removes duplicate and empty entries, keeping the first occurrence.
std::vector<serial_device_info> normalise(std::vector<serial_device_info> devices);

} // namespace vss

#endif // VSS_PORTS_SERIAL_ENUMERATION_HPP
