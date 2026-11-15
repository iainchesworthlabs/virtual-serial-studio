//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installing a checked deb or rpm (docs/updates.md, section 7.3). The
// window shows the command for a person to run, and, where polkit's pkexec
// and the package tool are installed, a button that runs it. Studio never
// raises its own rights: pkexec asks for the administrator.
//
// What is checked is what is installed. A file in the person's own folders
// can be changed by any program of theirs between Studio's check and the
// administrator's install, and Linux has no way to hold a file still. So
// the step that runs as the administrator does not trust the file it is
// given: it copies it into a folder only the administrator can reach,
// compares the copy's SHA-512 with the one from the signed feed, and
// installs the copy.
//

#pragma once

#include <vss/update/feed.hpp>

#include <QObject>
#include <QString>

#include <functional>

namespace vss::gui::updates
{

enum class package_install_result
{
    /// The package tool finished and reported success.
    done,
    /// The person dismissed the administrator prompt. Nothing changed.
    declined,
    /// The copy did not match, or the package tool failed.
    failed,
    /// This system has no pkexec, no package tool for this kind, or the
    /// kind is not a package.
    not_available
};

/// The command a person can run themselves, for `linux_deb` and
/// `linux_rpm`: "sudo apt install <file>" or "sudo dnf install <file>".
/// Empty for any other kind.
QString package_install_command(vss::update::file_kind kind,
                                const QString& path);

/// Whether `install_package` can do anything here for this kind.
bool package_install_available(vss::update::file_kind kind);

/// Installs the package at `path` through pkexec, as described above, and
/// calls `done` once on the thread of `context`, later and never from
/// inside this call. Nothing is called when `context` is destroyed first;
/// the install then carries on by itself.
void install_package(vss::update::file_kind kind, const QString& path,
                     const vss::update::sha512_digest& sha512, QObject* context,
                     std::function<void(package_install_result)> done);

} // namespace vss::gui::updates
