//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// macOS: the checked disk image is opened by the system, which shows its
// window. The person drags the app onto Applications; Studio does not
// replace its own bundle.

#include "updates/checked_file.hpp"
#include "updates/plain_checked_file.hpp"

#include <QDesktopServices>
#include <QUrl>

namespace vss::gui::updates
{

std::unique_ptr<checked_file> open_checked_file(const QString& path)
{
    return open_plain_checked_file(path);
}

install_start start_install(vss::update::file_kind kind,
                            const checked_file& file)
{
    if (kind != vss::update::file_kind::macos_dmg)
        return install_start::not_supported;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(file.path()))
               ? install_start::started
               : install_start::failed;
}

} // namespace vss::gui::updates
