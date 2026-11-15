//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "dialogs/recent_files.hpp"

#include <vss/core/profile.hpp>

#include <QCryptographicHash>
#include <QSettings>
#include <QStringList>

namespace vss::gui
{
namespace
{

constexpr const char* kRecentFiles = "ui/recentFiles";
constexpr const char* kRecentSummary = "ui/recentSummary";
constexpr const char* kEntryPrefix = "ui/recentEntries/";
constexpr int kMaximumRecent = 10;

QString entry_key(const QString& path)
{
    const QByteArray digest =
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(kEntryPrefix) +
           QString::fromLatin1(digest.toHex());
}

} // namespace

QVector<recent_entry> recent_entries()
{
    VSS_PROFILE_ZONE("gui.dialogs.recent_files.read");
    QSettings settings;
    const QStringList files =
        settings.value(QLatin1String(kRecentFiles)).toStringList();
    QVector<recent_entry> entries;
    entries.reserve(files.size());
    for (const QString& path : files)
    {
        recent_entry entry;
        entry.path = path;
        const QString prefix = entry_key(path);
        entry.summary =
            settings.value(prefix + QStringLiteral("/summary")).toString();
        entry.last_opened = QDateTime::fromString(
            settings.value(prefix + QStringLiteral("/lastOpened")).toString(),
            Qt::ISODate);
        entries.push_back(entry);
    }
    return entries;
}

void record_recent(const QString& path, const QString& summary)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.dialogs.recent_files.record");
    VSS_PROFILE_ZONE_TEXT(zone, path.toStdString());
    if (path.isEmpty())
        return;

    QSettings settings;
    QStringList files =
        settings.value(QLatin1String(kRecentFiles)).toStringList();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > kMaximumRecent)
    {
        const QString dropped = files.takeLast();
        const QString prefix = entry_key(dropped);
        settings.remove(prefix);
    }
    settings.setValue(QLatin1String(kRecentFiles), files);
    settings.setValue(QLatin1String(kRecentSummary), summary);

    const QString prefix = entry_key(path);
    settings.setValue(prefix + QStringLiteral("/summary"), summary);
    settings.setValue(prefix + QStringLiteral("/lastOpened"),
                      QDateTime::currentDateTime().toString(Qt::ISODate));
}

QString recent_summary(const QString& path)
{
    if (path.isEmpty())
        return {};
    const QString prefix = entry_key(path);
    return QSettings().value(prefix + QStringLiteral("/summary")).toString();
}

} // namespace vss::gui
