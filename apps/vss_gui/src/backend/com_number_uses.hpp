//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/studio_backend.hpp"

#include <QString>

#include <map>

namespace vss::gui
{

/// Who holds a COM number.
enum class com_holder
{
    /// A port Studio holds (named by `name`).
    studio_port,
    /// A device this computer lists with that name, such as a USB adapter.
    device,
    /// A device of something else that is not plugged in (a USB adapter),
    /// whose number Windows keeps for when it returns (named by `name`).
    unplugged,
    /// A number Windows holds with no device to name: a name that resolves,
    /// or a device this computer does not list.
    reserved
};

struct com_use
{
    com_holder holder = com_holder::reserved;
    QString name;
};

/// The COM numbers that cannot be chosen, and by what, taken from the ports
/// and devices the computer lists and from `reserved_com_numbers()` where the
/// computer can say.  `except` is a port whose own number does not count (the
/// port being renumbered).  A number held by a Studio port wins over a device
/// of the same path, and both over a reserved entry.
std::map<unsigned, com_use> com_number_uses(const StudioBackend& backend,
                                            vss::endpoint_id except = 0);

/// "Studio port rx-sim", "Windows device USB Serial Device", "Unplugged
/// Windows device USB Serial Device", "Reserved by Windows".
QString com_use_text(const com_use& use);

/// The free numbers from `from` to 255 that `uses` does not hold, lowest
/// first, at most `limit`.
std::vector<unsigned> free_com_numbers(const std::map<unsigned, com_use>& uses,
                                       unsigned from, std::size_t limit);

} // namespace vss::gui
