//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/com_number_uses.hpp"

#include "backend/com_numbers.hpp"

#include <QCoreApplication>

#include <vss/core/profile.hpp>

namespace vss::gui
{
namespace
{

std::vector<unsigned> numbers_of(const QString& path)
{
    return com_numbers_in_use({path.toStdString()});
}

} // namespace

std::map<unsigned, com_use> com_number_uses(const StudioBackend& backend,
                                            vss::endpoint_id except)
{
    VSS_PROFILE_ZONE("gui.backend.com_number_uses");
    std::map<unsigned, com_use> uses;
    if (backend.caps().com_numbers)
    {
        if (const auto reserved = backend.reserved_com_numbers())
        {
            for (const unsigned number : *reserved)
                uses[number] = com_use{com_holder::reserved, {}};
        }
    }
    for (const auto& device : backend.devices())
    {
        for (const unsigned number : numbers_of(device.path))
        {
            uses[number] = com_use{com_holder::device,
                                   device.description.isEmpty()
                                       ? device.path
                                       : device.description};
        }
    }
    for (const auto& port : backend.ports())
    {
        if (port.id == except)
            continue;
        for (const unsigned number : numbers_of(port.path))
            uses[number] = com_use{com_holder::studio_port, port.name};
    }
    return uses;
}

QString com_use_text(const com_use& use)
{
    switch (use.holder)
    {
    case com_holder::studio_port:
        return QCoreApplication::translate("com_use", "Studio port %1")
            .arg(use.name);
    case com_holder::device:
        return QCoreApplication::translate("com_use", "Windows device %1")
            .arg(use.name);
    case com_holder::reserved:
        break;
    }
    return QCoreApplication::translate("com_use", "Reserved by Windows");
}

std::vector<unsigned> free_com_numbers(const std::map<unsigned, com_use>& uses,
                                       unsigned from, std::size_t limit)
{
    std::vector<unsigned> free;
    for (unsigned number = from; number <= kLastGuiComNumber && free.size() < limit;
         ++number)
    {
        if (!uses.contains(number))
            free.push_back(number);
    }
    return free;
}

} // namespace vss::gui
