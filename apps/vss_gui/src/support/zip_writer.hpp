//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_ZIP_WRITER_HPP
#define VSS_GUI_SUPPORT_ZIP_WRITER_HPP

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

#include <cstdint>

namespace vss::gui::support
{

/// A minimal stored (uncompressed) ZIP writer.
///
/// The support bundle needs one standard ZIP file and the project forbids
/// a new third-party dependency, so this writes the format directly: local
/// headers, a central directory and the end-of-central-directory record, with
/// the UTF-8 file-name flag set.  Nothing is compressed; every reader accepts
/// stored entries.
class StoredZipWriter final
{
public:
    /// Adds one file.  `name` is stored with forward slashes.  Returns false
    /// only when the format's 65,535-entry limit is reached.
    bool add_file(const QString& name, const QByteArray& data,
                  const QDateTime& modified = {});

    int file_count() const { return static_cast<int>(entries_.size()); }

    /// Renders the complete archive.  Safe to call more than once.
    QByteArray data() const;

    /// The CRC-32 (IEEE 802.3) of `data`, as used by ZIP.
    static std::uint32_t crc32(const QByteArray& data);

private:
    struct entry
    {
        QByteArray name;
        QByteArray data;
        std::uint16_t dos_time = 0;
        std::uint16_t dos_date = 0;
    };

    QVector<entry> entries_;
};

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_ZIP_WRITER_HPP
