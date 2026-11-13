//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QString>

namespace vss::gui::support
{

/// The organisation name Qt builds its storage paths from. It is the one the
/// ARINC Data Loader uses, so both programs sit under the same top-level key:
/// `HKCU\Software\iainchesworthlabs\<application>` on Windows and
/// `<config>/iainchesworthlabs/<application>` elsewhere.
QString organisation_name();

/// The application name, also the second level of those paths. It must differ
/// from the organisation name, or the key is nested twice.
QString application_name();

/// Sets the organisation and application names on QCoreApplication. Called
/// once at the start of `main`, before the first QSettings is made.
void apply_application_identity();

/// The path QSettings use for the program's own settings in the native format:
/// the registry key on Windows, a file elsewhere.
QString settings_location();

} // namespace vss::gui::support
