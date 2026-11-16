//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Linux: nothing is started from here. A package is installed by the
// system's package tool, which the window shows as a command and can run
// through pkexec; an archive is shown in its folder.

#include "updates/checked_file.hpp"
#include "updates/plain_checked_file.hpp"

namespace vss::gui::updates
{

std::unique_ptr<checked_file> open_checked_file(const QString& path)
{
    return open_plain_checked_file(path);
}

install_start start_install(vss::update::file_kind, const checked_file&)
{
    return install_start::not_supported;
}

} // namespace vss::gui::updates
