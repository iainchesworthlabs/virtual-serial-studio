//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A downloaded file, opened once, so that what is checked is what runs
// (docs/updates.md, section 7.2; the rule of docs/plugins.md, section 5.4).
//
// The file is opened for reading with writing and deleting denied to
// everyone else where the system can do that (Windows), hashed through that
// one handle, and the handle is kept until the install step has started. A
// program running as the same user therefore cannot swap the file between
// the check and the moment the system asks for an administrator. The
// opening and the hand-over are one source file per system under
// `platform/`.
//

#pragma once

#include <vss/update/feed.hpp>

#include <QByteArray>
#include <QString>

#include <memory>
#include <optional>

namespace vss::gui::updates
{

class checked_file
{
public:
    checked_file() = default;
    checked_file(const checked_file&) = delete;
    checked_file& operator=(const checked_file&) = delete;
    /// Lets go of the file. It is not deleted.
    virtual ~checked_file() = default;

    /// The path the file was opened by.
    virtual QString path() const = 0;

    /// The next bytes of the file, at most `max`, read through the handle
    /// this object holds. Empty at the end of the file. Nothing when the
    /// read failed.
    virtual std::optional<QByteArray> read(qint64 max) = 0;
};

/// Opens `path` as described above. Null when it cannot be opened, is not a
/// plain file, or is a link.
std::unique_ptr<checked_file> open_checked_file(const QString& path);

enum class install_start
{
    /// The system took the file: the installer is running, or the disk
    /// image is open.
    started,
    /// The person said no to the system's own question (the administrator
    /// prompt of Windows). Nothing is wrong and nothing changed.
    declined,
    /// The system could not start it.
    failed,
    /// This kind of install has no hand-over on this system: the window
    /// shows the file or the command instead.
    not_supported
};

/// Hands a checked file to the system: starts a `windows_installer`, which
/// asks for an administrator itself and shows its own pages, or opens a
/// `macos_dmg`. Studio never raises its own rights. The caller keeps `file`
/// alive until this has returned.
install_start start_install(vss::update::file_kind kind,
                            const checked_file& file);

} // namespace vss::gui::updates
