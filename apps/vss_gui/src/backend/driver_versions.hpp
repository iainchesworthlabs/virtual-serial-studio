//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The three versions Studio knows of the Windows driver, and what to say about
// them: the package in Windows' driver store, the interface version of the
// ports that are running, and the package carried by this copy of Studio. The
// first and the last compare the way Windows compares two packages, so "older"
// here is exactly the case where `pnputil` would take the bundled one in.
//

#pragma once

#include "backend/view_types.hpp"

#include <vss/ports/driver_package.hpp>

#include <QString>

#include <optional>

namespace vss::gui
{

/// The versions as text and how the installed one compares with the bundled
/// one. `ports` is the interface version the running ports speak, or empty.
driver_versions assess_driver_versions(
    const std::optional<vss::driver_package_version>& installed,
    const std::optional<vss::driver_package_version>& bundled,
    const QString& ports);

/// The .inf file of the driver package carried by this copy of Studio, or
/// empty when there is none (VSS_DRIVER_PACKAGE, else the `driver` folder
/// beside the program). The same place the guided set-up takes its files from.
QString bundled_driver_inf_path();

/// The version in that .inf file.
std::optional<vss::driver_package_version> bundled_driver_version();

/// True when the installed package is older than the bundled one, so that the
/// guided set-up has a newer driver to put in.
bool driver_is_behind(const driver_versions& versions) noexcept;

/// The versions in plain words for the setup check, the About box and the
/// set-up dialog: two sentences at most. Empty when nothing is known and
/// nothing needs saying (a PC with no driver, or a system that needs none).
QString driver_versions_sentence(const driver_versions& versions);

} // namespace vss::gui
