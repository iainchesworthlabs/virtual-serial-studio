//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "devices_json.hpp"

#include <boost/json.hpp>

#include <utility>

namespace vss_cli
{

std::string devices_json(const std::vector<vss::serial_device_info>& devices,
                         const vss::com0com_installation& installation)
{
    namespace json = boost::json;

    json::array list;
    for (const auto& device : devices)
    {
        json::object entry;
        entry["path"] = device.path;
        entry["description"] = device.description;
        entry["hardware_id"] = device.hardware_id;
        entry["virtual"] = device.virtual_device;
        entry["com0com"] = device.com0com;
        entry["pair"] = device.com0com_pair;
        entry["side"] = device.com0com_side;
        list.push_back(std::move(entry));
    }

    json::object com0com;
    com0com["found"] = installation.found;
    com0com["pairs"] = installation.pairs.size();

    json::object document;
    document["devices"] = std::move(list);
    document["com0com"] = std::move(com0com);

    std::string text = json::serialize(document);
    text += '\n';
    return text;
}

} // namespace vss_cli
