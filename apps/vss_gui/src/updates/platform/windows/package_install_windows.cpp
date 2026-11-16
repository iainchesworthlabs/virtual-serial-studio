//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Windows: there is no package tool to hand a file to. An update is the
// installer or the zip.

#include "updates/package_install.hpp"

#include <memory>
#include <utility>

namespace vss::gui::updates
{

bool package_install_available(vss::update::file_kind)
{
    return false;
}

void install_package(vss::update::file_kind, const QString&,
                     const vss::update::sha512_digest&, QObject* context,
                     std::function<void(package_install_result)> done)
{
    const auto report =
        std::make_shared<std::function<void(package_install_result)>>(
            std::move(done));
    QMetaObject::invokeMethod(
        context, [report] { (*report)(package_install_result::not_available); },
        Qt::QueuedConnection);
}

} // namespace vss::gui::updates
