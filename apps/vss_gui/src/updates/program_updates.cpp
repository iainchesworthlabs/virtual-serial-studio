//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "updates/program_updates.hpp"

#include "updates/checked_file.hpp"
#include "updates/install_kind.hpp"

#include <vss/plugin/access.hpp>
#include <vss/plugin/locations.hpp>
#include <vss/update/feed.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QStandardPaths>

#include <utility>

namespace vss::gui::updates
{

bool update_check_disabled_by_policy(
    const std::filesystem::path& program_directory)
{
    const std::filesystem::path file =
        vss::plugin::plugin_locations_for(program_directory).policy_file;
    if (file.empty())
        return false;
    const vss::plugin::loaded_policy loaded = vss::plugin::load_policy(file);
    // `rules` of a file that could not be used is the fallback for plugins,
    // not what an administrator wrote, so it says nothing about updates.
    if (loaded.problem || !loaded.rules.has_value())
        return false;
    return !loaded.rules->update_check;
}

ProgramUpdates::ProgramUpdates(const QString& running_version, QObject* parent)
    : QObject(parent)
{
    UpdateService::config settings;
    settings.running_version = running_version;
    settings.keys = vss::update::product_feed_keys();
    settings.disabled_by_policy =
        update_check_disabled_by_policy(std::filesystem::path(
            QCoreApplication::applicationDirPath().toStdU16String()));
    settings.now = [] { return QDateTime::currentDateTimeUtc(); };
    settings.probe =
        make_install_kind_probe(QCoreApplication::applicationFilePath());
    settings.architecture = built_architecture();
    settings.cache_folder =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    settings.downloads_folder =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    settings.open_file = [](const QString& path)
    { return open_checked_file(path); };
    settings.start_install =
        [](vss::update::file_kind kind, const checked_file& file)
    { return start_install(kind, file); };
    service_ = std::make_unique<UpdateService>(std::move(settings), transport_);
}

ProgramUpdates::~ProgramUpdates() = default;

} // namespace vss::gui::updates
