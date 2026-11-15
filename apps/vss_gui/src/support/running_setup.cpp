//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/running_setup.hpp"

#include <vss/core/profile.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <string>
#include <string_view>

namespace vss::gui::support
{
namespace
{

constexpr int kVersion = 1;

QString bad_path(const QString& path)
{
    QString bad = path;
    if (bad.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        bad.chop(5);
    return bad + QStringLiteral(".bad.json");
}

void move_aside(const QString& path)
{
    const QString bad = bad_path(path);
    QFile::remove(bad);
    if (!QFile::rename(path, bad))
        QFile::remove(path);
}

/// The file's top object, or none when it is missing or not an object.
std::optional<QJsonObject> read_object(const QString& path, bool* exists)
{
    QFile file(path);
    *exists = file.exists();
    if (!*exists || !file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    if (!json.isObject())
        return std::nullopt;
    return json.object();
}

running_state state_of(const QJsonObject& object)
{
    running_state state;
    state.document_path =
        object.value(QLatin1String("documentPath")).toString();
    state.dirty = object.value(QLatin1String("dirty")).toBool(false);
    state.stopped = object.value(QLatin1String("stopped")).toBool(false);
    return state;
}

bool write_object(const QString& path, const QJsonObject& object)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray bytes = QJsonDocument(object).toJson();
    if (file.write(bytes) != bytes.size())
    {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace

QString running_setup_path()
{
    const QString folder =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(folder).filePath(QStringLiteral("running-setup.json"));
}

bool save_running_setup(const QString& path,
                        const vss::config::document& document,
                        const running_state& state)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.support.running_setup.save");
    VSS_PROFILE_ZONE_TEXT(zone, path.toStdString());
    const auto text = vss::config::to_json(document, false);
    if (!text)
        return false;
    const QJsonDocument setup =
        QJsonDocument::fromJson(QByteArray::fromStdString(*text));
    if (!setup.isObject())
        return false;
    QJsonObject object;
    object.insert(QStringLiteral("version"), kVersion);
    object.insert(QStringLiteral("documentPath"), state.document_path);
    object.insert(QStringLiteral("dirty"), state.dirty);
    object.insert(QStringLiteral("stopped"), state.stopped);
    object.insert(QStringLiteral("setup"), setup.object());
    return write_object(path, object);
}

bool save_running_state(const QString& path, const running_state& state)
{
    VSS_PROFILE_ZONE_NAMED(zone,
                           "gui.support.running_setup.save_state");
    VSS_PROFILE_ZONE_TEXT(zone, path.toStdString());
    bool exists = false;
    auto object = read_object(path, &exists);
    if (!object)
        return false;
    object->insert(QStringLiteral("documentPath"), state.document_path);
    object->insert(QStringLiteral("dirty"), state.dirty);
    object->insert(QStringLiteral("stopped"), state.stopped);
    return write_object(path, *object);
}

running_state load_running_state(const QString& path)
{
    VSS_PROFILE_ZONE_NAMED(zone,
                           "gui.support.running_setup.load_state");
    VSS_PROFILE_ZONE_TEXT(zone, path.toStdString());
    bool exists = false;
    const auto object = read_object(path, &exists);
    return object ? state_of(*object) : running_state{};
}

std::optional<running_setup> load_running_setup(const QString& path,
                                                bool* damaged)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.support.running_setup.load");
    VSS_PROFILE_ZONE_TEXT(zone, path.toStdString());
    if (damaged != nullptr)
        *damaged = false;
    bool exists = false;
    const auto object = read_object(path, &exists);
    if (!exists)
        return std::nullopt;

    std::optional<vss::config::document> document;
    if (object && object->value(QLatin1String("version")).toInt() == kVersion &&
        object->value(QLatin1String("setup")).isObject())
    {
        const QByteArray text =
            QJsonDocument(object->value(QLatin1String("setup")).toObject())
                .toJson(QJsonDocument::Compact);
        auto parsed = vss::config::from_json(std::string_view(
            text.constData(), static_cast<std::size_t>(text.size())));
        if (parsed && !vss::config::validate(*parsed))
            document = std::move(*parsed);
    }
    if (!document)
    {
        move_aside(path);
        if (damaged != nullptr)
            *damaged = true;
        return std::nullopt;
    }
    if (document->ports.empty())
        return std::nullopt;

    running_setup setup;
    setup.ports = static_cast<int>(document->ports.size());
    setup.links = static_cast<int>(document->links.size());
    setup.document = std::move(*document);
    setup.state = state_of(*object);
    return setup;
}

} // namespace vss::gui::support
