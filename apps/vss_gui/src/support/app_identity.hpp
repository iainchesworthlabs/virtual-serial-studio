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

/// The organisation's internet domain. macOS names the settings file after it
/// (`com.iainchesworthlabs.<application>.plist`), and it matches the bundle
/// identifier `com.iainchesworthlabs.virtualserialstudio`. It must be a real
/// name, never a reserved one such as `.example` or `.invalid`.
QString organisation_domain();

/// The application name, also the second level of those paths. It must differ
/// from the organisation name, or the key is nested twice.
QString application_name();

/// Where to write about a licence. The one place to change it, and a real
/// address that receives mail.
QString support_address();

/// Where the documents are published (`site_url` in `mkdocs.yml`). Studio
/// copies it to the clipboard and never opens it. The one place to change it.
QString documents_address();

/// Sets the organisation name, organisation domain and application name on
/// QCoreApplication. Called once at the start of `main`, before the first
/// QSettings is made.
void apply_application_identity();

/// The place QSettings use for the program's own settings, as Qt reports it
/// for a default-constructed QSettings: the registry key on Windows, a file
/// elsewhere. On macOS the file follows the organisation domain, on the other
/// platforms the organisation name. It is only right after
/// `apply_application_identity`.
QString settings_location();

} // namespace vss::gui::support
