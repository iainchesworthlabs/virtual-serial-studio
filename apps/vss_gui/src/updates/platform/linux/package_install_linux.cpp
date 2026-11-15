//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Linux: the administrator's step is one fixed shell script, run by
// pkexec. Its three arguments are the file, the SHA-512 it must have and
// the name to copy it under. Nothing of the script is built from them.

#include "updates/package_install.hpp"

#include <QFileInfo>
#include <QProcess>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <utility>

namespace vss::gui::updates
{
namespace
{

constexpr const char* kPkexec = "/usr/bin/pkexec";
constexpr const char* kShell = "/bin/sh";

/// pkexec's own exit status when the person dismissed its prompt.
constexpr int kPkexecDismissed = 126;

/// The copy is made in a folder that only the administrator can reach,
/// checked there, installed from there and removed. `set -e` stops at the
/// first step that fails, so a copy that does not match is never
/// installed.
constexpr const char* kDebScript =
    "set -eu\n"
    "umask 077\n"
    "dir=$(mktemp -d /tmp/vss-update.XXXXXX)\n"
    "trap 'rm -rf \"$dir\"' EXIT\n"
    "cp -- \"$1\" \"$dir/$3\"\n"
    "printf '%s  %s\\n' \"$2\" \"$dir/$3\" | sha512sum -c --status -\n"
    "DEBIAN_FRONTEND=noninteractive apt-get install -y \"$dir/$3\"\n";

constexpr const char* kRpmScript =
    "set -eu\n"
    "umask 077\n"
    "dir=$(mktemp -d /tmp/vss-update.XXXXXX)\n"
    "trap 'rm -rf \"$dir\"' EXIT\n"
    "cp -- \"$1\" \"$dir/$3\"\n"
    "printf '%s  %s\\n' \"$2\" \"$dir/$3\" | sha512sum -c --status -\n"
    "dnf install -y \"$dir/$3\"\n";

bool is_program(const char* path)
{
    const QFileInfo info(QString::fromLatin1(path));
    return info.isFile() && info.isExecutable();
}

struct plan
{
    const char* script = nullptr;
    const char* tool = nullptr;
    const char* copy_name = nullptr;
};

plan plan_for(vss::update::file_kind kind)
{
    if (kind == vss::update::file_kind::linux_deb)
        return {kDebScript, "/usr/bin/apt-get", "package.deb"};
    if (kind == vss::update::file_kind::linux_rpm)
        return {kRpmScript, "/usr/bin/dnf", "package.rpm"};
    return {};
}

QString hex_of(const vss::update::sha512_digest& digest)
{
    QByteArray bytes;
    for (const std::uint8_t byte : digest)
        bytes.append(static_cast<char>(byte));
    return QString::fromLatin1(bytes.toHex());
}

} // namespace

bool package_install_available(vss::update::file_kind kind)
{
    const plan steps = plan_for(kind);
    return steps.script != nullptr && is_program(kPkexec) &&
           is_program(kShell) && is_program(steps.tool);
}

void install_package(vss::update::file_kind kind, const QString& path,
                     const vss::update::sha512_digest& sha512, QObject* context,
                     std::function<void(package_install_result)> done)
{
    const auto report =
        std::make_shared<std::function<void(package_install_result)>>(
            std::move(done));
    if (!package_install_available(kind))
    {
        QMetaObject::invokeMethod(
            context,
            [report] { (*report)(package_install_result::not_available); },
            Qt::QueuedConnection);
        return;
    }
    const plan steps = plan_for(kind);
    // The process is not a child of `context`: closing the window must not
    // kill a package tool half way through.
    auto* const process = new QProcess;
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    const auto settled = std::make_shared<bool>(false);
    QObject::connect(process, &QProcess::finished, process,
                     &QObject::deleteLater);
    QObject::connect(process, &QProcess::finished, context,
                     [report, settled](int code, QProcess::ExitStatus status)
                     {
                         if (std::exchange(*settled, true))
                             return;
                         if (status == QProcess::NormalExit && code == 0)
                             (*report)(package_install_result::done);
                         else if (status == QProcess::NormalExit &&
                                  code == kPkexecDismissed)
                             (*report)(package_install_result::declined);
                         else
                             (*report)(package_install_result::failed);
                     });
    QObject::connect(process, &QProcess::errorOccurred, context,
                     [report, settled, process](QProcess::ProcessError error)
                     {
                         if (error != QProcess::FailedToStart ||
                             std::exchange(*settled, true))
                             return;
                         process->deleteLater();
                         (*report)(package_install_result::failed);
                     });
    process->start(
        QString::fromLatin1(kPkexec),
        QStringList{QString::fromLatin1(kShell), QStringLiteral("-c"),
                    QString::fromLatin1(steps.script),
                    QStringLiteral("vss-update"), path, hex_of(sha512),
                    QString::fromLatin1(steps.copy_name)},
        QIODevice::NotOpen);
}

} // namespace vss::gui::updates
