//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/zip_writer.hpp"

#include <vss/core/profile.hpp>

#include <algorithm>
#include <array>

namespace vss::gui::support
{
namespace
{

constexpr std::uint32_t kLocalHeader = 0x04034b50U;
constexpr std::uint32_t kCentralHeader = 0x02014b50U;
constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50U;
constexpr std::uint16_t kVersion = 20;
/// Bit 11: the file name is UTF-8.
constexpr std::uint16_t kUtf8Flag = 0x0800;
constexpr int kMaxEntries = 65535;

const std::array<std::uint32_t, 256>& crc_table()
{
    static const std::array<std::uint32_t, 256> table = []
    {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < 256; ++index)
        {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit)
            {
                value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U
                                           : value >> 1U;
            }
            values.at(index) = value;
        }
        return values;
    }();
    return table;
}

void put_u16(QByteArray& out, std::uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFU));
    out.append(static_cast<char>((value >> 8U) & 0xFFU));
}

void put_u32(QByteArray& out, std::uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFU));
    out.append(static_cast<char>((value >> 8U) & 0xFFU));
    out.append(static_cast<char>((value >> 16U) & 0xFFU));
    out.append(static_cast<char>((value >> 24U) & 0xFFU));
}

} // namespace

std::uint32_t StoredZipWriter::crc32(const QByteArray& data)
{
    std::uint32_t value = 0xFFFFFFFFU;
    for (const char byte : data)
    {
        const auto index = static_cast<std::uint8_t>(
            (value ^ static_cast<std::uint8_t>(byte)) & 0xFFU);
        value = (value >> 8U) ^ crc_table().at(index);
    }
    return value ^ 0xFFFFFFFFU;
}

bool StoredZipWriter::add_file(const QString& name, const QByteArray& data,
                               const QDateTime& modified)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.support.zip.add_file");
    VSS_PROFILE_ZONE_TEXT(zone, name.toStdString());
    if (entries_.size() >= kMaxEntries)
        return false;

    const QDateTime stamp =
        modified.isValid() ? modified : QDateTime::currentDateTime();
    const QDate date = stamp.date();
    const QTime time = stamp.time();

    entry item;
    item.name = name.toUtf8();
    item.data = data;
    const int year = std::clamp(date.year(), 1980, 2107);
    item.dos_date = static_cast<std::uint16_t>(
        ((year - 1980) << 9) | (date.month() << 5) | date.day());
    item.dos_time = static_cast<std::uint16_t>(
        (time.hour() << 11) | (time.minute() << 5) | (time.second() / 2));
    entries_.push_back(std::move(item));
    return true;
}

QByteArray StoredZipWriter::data() const
{
    VSS_PROFILE_ZONE("gui.support.zip.finish");
    QByteArray out;
    QVector<std::uint32_t> offsets;
    offsets.reserve(entries_.size());

    for (const entry& item : entries_)
    {
        const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
        offsets.push_back(offset);
        const std::uint32_t checksum = crc32(item.data);
        const auto size = static_cast<std::uint32_t>(item.data.size());

        put_u32(out, kLocalHeader);
        put_u16(out, kVersion);
        put_u16(out, kUtf8Flag);
        put_u16(out, 0); // stored
        put_u16(out, item.dos_time);
        put_u16(out, item.dos_date);
        put_u32(out, checksum);
        put_u32(out, size);
        put_u32(out, size);
        put_u16(out, static_cast<std::uint16_t>(item.name.size()));
        put_u16(out, 0); // extra field length
        out.append(item.name);
        out.append(item.data);
    }

    const std::uint32_t central_start = static_cast<std::uint32_t>(out.size());
    for (int index = 0; index < entries_.size(); ++index)
    {
        const entry& item = entries_.at(index);
        const std::uint32_t checksum = crc32(item.data);
        const auto size = static_cast<std::uint32_t>(item.data.size());

        put_u32(out, kCentralHeader);
        put_u16(out, kVersion); // version made by
        put_u16(out, kVersion); // version needed
        put_u16(out, kUtf8Flag);
        put_u16(out, 0); // stored
        put_u16(out, item.dos_time);
        put_u16(out, item.dos_date);
        put_u32(out, checksum);
        put_u32(out, size);
        put_u32(out, size);
        put_u16(out, static_cast<std::uint16_t>(item.name.size()));
        put_u16(out, 0); // extra
        put_u16(out, 0); // comment
        put_u16(out, 0); // disk number
        put_u16(out, 0); // internal attributes
        put_u32(out, 0); // external attributes
        put_u32(out, offsets.at(index));
        out.append(item.name);
    }

    const std::uint32_t central_size =
        static_cast<std::uint32_t>(out.size()) - central_start;
    const auto count = static_cast<std::uint16_t>(entries_.size());
    put_u32(out, kEndOfCentralDirectory);
    put_u16(out, 0); // this disk
    put_u16(out, 0); // disk with the central directory
    put_u16(out, count);
    put_u16(out, count);
    put_u32(out, central_size);
    put_u32(out, central_start);
    put_u16(out, 0); // comment length
    return out;
}

} // namespace vss::gui::support
