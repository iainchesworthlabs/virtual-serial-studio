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

namespace
{

/// `text` with every byte that is not part of a well-formed UTF-8 sequence
/// replaced by U+FFFD.  JSON text is Unicode, and a device name on Linux or
/// macOS may be any bytes; a parser that checks would refuse the document.
std::string valid_utf8(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        unsigned long point = 0;
        unsigned long least = 0;
        if (lead < 0x80)
        {
            length = 1;
            point = lead;
        }
        else if (lead >= 0xC2 && lead <= 0xDF)
        {
            length = 2;
            point = lead & 0x1Fu;
            least = 0x80;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            length = 3;
            point = lead & 0x0Fu;
            least = 0x800;
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            length = 4;
            point = lead & 0x07u;
            least = 0x10000;
        }

        bool good = length != 0 && index + length <= text.size();
        for (std::size_t next = 1; good && next < length; ++next)
        {
            const auto byte = static_cast<unsigned char>(text[index + next]);
            good = (byte & 0xC0u) == 0x80u;
            point = (point << 6) | (byte & 0x3Fu);
        }
        // Not an overlong form, not a surrogate, not beyond U+10FFFF.
        good = good && point >= least && point <= 0x10FFFF &&
               !(point >= 0xD800 && point <= 0xDFFF);

        if (good)
        {
            out.append(text, index, length);
            index += length;
        }
        else
        {
            // U+FFFD as its UTF-8 bytes.
            out += "\xEF\xBF\xBD";
            ++index;
        }
    }
    return out;
}

} // namespace

std::string devices_json(const std::vector<vss::serial_device_info>& devices,
                         const vss::com0com_installation& installation)
{
    namespace json = boost::json;

    json::array list;
    for (const auto& device : devices)
    {
        json::object entry;
        entry["path"] = valid_utf8(device.path);
        entry["description"] = valid_utf8(device.description);
        entry["hardware_id"] = valid_utf8(device.hardware_id);
        entry["virtual"] = device.virtual_device;
        entry["com0com"] = device.com0com;
        entry["pair"] = device.com0com_pair;
        entry["side"] = valid_utf8(device.com0com_side);
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
