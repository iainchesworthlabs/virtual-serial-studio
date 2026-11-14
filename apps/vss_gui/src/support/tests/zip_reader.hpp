//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A tiny independent ZIP reader for the tests, so a test proves the archive
// structure the same way a ZIP tool walks the central directory.
//

#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>

namespace vss::gui::test_zip
{

inline std::uint16_t read_u16(const QByteArray& data, int offset)
{
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(data.at(offset)) |
        (static_cast<unsigned char>(data.at(offset + 1)) << 8));
}

inline std::uint32_t read_u32(const QByteArray& data, int offset)
{
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(data.at(offset)) |
        (static_cast<unsigned char>(data.at(offset + 1)) << 8) |
        (static_cast<unsigned char>(data.at(offset + 2)) << 16) |
        (static_cast<unsigned char>(data.at(offset + 3)) << 24));
}

struct zip_entry
{
    QString name;
    QByteArray data;
};

/// A tiny independent reader, so the test proves the archive structure the
/// same way a ZIP tool walks the central directory.
inline QVector<zip_entry> read_zip(const QByteArray& archive)
{
    QVector<zip_entry> entries;
    int eocd = -1;
    for (int offset = static_cast<int>(archive.size()) - 22; offset >= 0;
         --offset)
    {
        if (read_u32(archive, offset) == 0x06054b50U)
        {
            eocd = offset;
            break;
        }
    }
    if (eocd < 0)
        return entries;
    const int count = read_u16(archive, eocd + 10);
    int cursor = static_cast<int>(read_u32(archive, eocd + 16));
    for (int index = 0; index < count; ++index)
    {
        if (read_u32(archive, cursor) != 0x02014b50U)
            break;
        const int name_length = read_u16(archive, cursor + 28);
        const int extra_length = read_u16(archive, cursor + 30);
        const int comment_length = read_u16(archive, cursor + 32);
        const int local_offset =
            static_cast<int>(read_u32(archive, cursor + 42));
        const QString name =
            QString::fromUtf8(archive.mid(cursor + 46, name_length));

        const int local_name = read_u16(archive, local_offset + 26);
        const int local_extra = read_u16(archive, local_offset + 28);
        const int size = static_cast<int>(read_u32(archive, local_offset + 18));
        const int data_offset = local_offset + 30 + local_name + local_extra;
        entries.push_back({name, archive.mid(data_offset, size)});

        cursor += 46 + name_length + extra_length + comment_length;
    }
    return entries;
}

} // namespace vss::gui::test_zip
