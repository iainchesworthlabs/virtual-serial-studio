//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/update_text.hpp"

#include <QCoreApplication>
#include <QLocale>

namespace vss::gui::support
{

QString update_size_text(std::uint64_t bytes)
{
    constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;
    constexpr double kWholeFrom = 10.0;
    const double megabytes = static_cast<double>(bytes) / kBytesPerMegabyte;
    // 9.96 MB would be written "10.0 MB" with one decimal, so the choice is
    // made on the rounded number.
    const bool whole = megabytes >= kWholeFrom - 0.05;
    const QString number = QLocale().toString(megabytes, 'f', whole ? 0 : 1);
    //: The size of a file in megabytes, for example "48 MB". %1 is the number.
    return QCoreApplication::translate("UpdateDialog", "%1 MB").arg(number);
}

QString install_kind_text(vss::update::file_kind kind)
{
    using vss::update::file_kind;
    if (kind == file_kind::macos_dmg)
        return QCoreApplication::translate("About",
                                           "copied from the disk image");
    if (kind == file_kind::windows_installer || kind == file_kind::linux_deb ||
        kind == file_kind::linux_rpm)
        return QCoreApplication::translate(
            "About", "installed for everyone on this computer");
    return QCoreApplication::translate("About", "unpacked from an archive");
}

} // namespace vss::gui::support
