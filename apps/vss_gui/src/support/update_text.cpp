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

QString update_status_text(const updates::snapshot& state,
                           const QString& running_version)
{
    using updates::check_failure;
    using updates::check_state;
    const QString offered =
        state.offer.has_value() ? state.offer->version_text : QString();
    switch (state.state)
    {
    case check_state::idle:
        return QCoreApplication::translate("About", "Not checked yet.");
    case check_state::checking:
        return QCoreApplication::translate("About", "Checking...");
    case check_state::current:
        if (state.developer_build && state.offer.has_value())
            return QCoreApplication::translate(
                       "About",
                       "This is a developer build. The newest release is %1.")
                .arg(offered);
        return QCoreApplication::translate("About", "%1 is the newest version.")
            .arg(running_version);
    case check_state::available:
        if (!state.offer.has_value())
            return QCoreApplication::translate("About",
                                               "%1 is the newest version.")
                .arg(running_version);
        if (!state.offer->file.has_value())
            return QCoreApplication::translate(
                       "About", "%1 is available. Download it from "
                                "virtual-serial-studio.com.")
                .arg(offered);
        return QCoreApplication::translate("About", "%1 is available.")
            .arg(offered);
    case check_state::failed:
        if (state.failure == check_failure::untrusted)
            return QCoreApplication::translate(
                "About", "The update information could not be trusted, so "
                         "nothing was changed.");
        return QCoreApplication::translate(
            "About", "Studio could not reach the update server. Check the "
                     "connection and try again.");
    case check_state::disabled_by_policy:
        return QCoreApplication::translate(
            "About", "Update checks are turned off by your administrator.");
    case check_state::no_keys:
        return QCoreApplication::translate(
            "About", "This build cannot check for updates.");
    }
    return {};
}

QString
update_this_version_text(const QString& running_version,
                         const std::optional<vss::update::file_kind>& kind)
{
    if (!kind.has_value())
        return running_version;
    //: "<version>, <kind of install>", for example "0.2.0, unpacked from an
    //: archive".
    return QCoreApplication::translate("About", "%1, %2")
        .arg(running_version, install_kind_text(*kind));
}

QString update_last_checked_text(const QDateTime& when)
{
    if (!when.isValid())
        return QCoreApplication::translate("About", "Never");
    return QLocale().toString(when.toLocalTime(), QLocale::ShortFormat);
}

QString update_question_text()
{
    return QCoreApplication::translate(
        "UpdateNotice",
        "Check for updates automatically? Once a day Studio would ask "
        "virtual-serial-studio.com for the newest version. Nothing about you "
        "or this computer is sent.");
}

QString update_notice_text(const QString& version)
{
    return QCoreApplication::translate("UpdateNotice",
                                       "Virtual Serial Studio %1 is available.")
        .arg(version);
}

QString update_answer_toast(bool turned_on)
{
    if (turned_on)
        return QCoreApplication::translate(
            "UpdateNotice", "Studio will check for updates once a day.");
    return QCoreApplication::translate(
        "UpdateNotice",
        "Studio will not check for updates. You can change this in Settings.");
}

QString update_chip_text()
{
    return QCoreApplication::translate("UpdateChip", "Update available");
}

QString update_chip_name(const QString& version)
{
    return QCoreApplication::translate(
               "UpdateChip", "Update available: version %1. Open the update.")
        .arg(version);
}

} // namespace vss::gui::support
