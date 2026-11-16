//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Windows: a copy that the installer put in place is named by the
// installer's Apps & features entry. Anything else was unpacked from the
// zip.

#include "updates/install_kind.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include <utility>

namespace vss::gui::updates
{
namespace
{

/// The installer writes its entry in the 32-bit view of the registry
/// (docs/releasing.md, "The Windows installer").
constexpr const char* kUninstallKey =
    "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
    "\\Uninstall\\VirtualSerialStudio";

/// The folder with a trailing separator, in the form Windows compares:
/// without links, with one kind of separator and in one case.
QString comparable(const QString& folder)
{
    QString path = QDir(folder).canonicalPath();
    if (path.isEmpty())
        return {};
    if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    return path.toCaseFolded();
}

/// The folder the installer says it installed into, or nothing.
QString installed_folder()
{
    const QSettings entry(QString::fromLatin1(kUninstallKey),
                          QSettings::Registry32Format);
    QString folder =
        entry.value(QStringLiteral("InstallLocation")).toString().trimmed();
    if (folder.isEmpty())
    {
        // Older entries name only the uninstaller, which is in the folder.
        QString uninstaller =
            entry.value(QStringLiteral("UninstallString")).toString().trimmed();
        uninstaller.remove(QLatin1Char('"'));
        if (!uninstaller.isEmpty())
            folder = QFileInfo(uninstaller).absolutePath();
    }
    return folder;
}

class windows_probe final : public install_kind_probe
{
public:
    explicit windows_probe(QString program_file)
        : program_file_(std::move(program_file))
    {
    }

    void detect(std::function<void(vss::update::file_kind)> done) override
    {
        const QString installed = comparable(installed_folder());
        const QString program =
            comparable(QFileInfo(program_file_).absolutePath());
        const bool from_installer = !installed.isEmpty() &&
                                    !program.isEmpty() &&
                                    program.startsWith(installed);
        done(from_installer ? vss::update::file_kind::windows_installer
                            : vss::update::file_kind::windows_zip);
    }

private:
    QString program_file_;
};

} // namespace

std::unique_ptr<install_kind_probe>
make_install_kind_probe(const QString& program_file)
{
    return std::make_unique<windows_probe>(program_file);
}

vss::update::operating_system built_system()
{
    return vss::update::operating_system::windows;
}

} // namespace vss::gui::updates
