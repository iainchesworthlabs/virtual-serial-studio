//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/licence_store.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vss::gui
{

namespace
{

namespace lic = vss::license;

/// A file's bytes, or nothing when it cannot be read or is larger than a
/// signed file may be. The size is checked before the file is read.
std::optional<QByteArray> read_signed_file(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const auto limit = static_cast<qint64>(lic::signed_file::max_file_size);
    if (file.size() > limit)
        return std::nullopt;
    QByteArray bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFileDevice::NoError)
        return std::nullopt;
    return bytes;
}

std::string_view view_of(const QByteArray& bytes)
{
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

/// The files with `suffix` in `directory`, by name, at most `kMaxFiles`.
QStringList files_in(const QString& directory, const char* suffix)
{
    if (directory.isEmpty())
        return {};
    const QDir folder(directory);
    QStringList names =
        folder.entryList({QStringLiteral("*.") + QLatin1String(suffix)},
                         QDir::Files | QDir::Readable, QDir::Name);
    while (names.size() > licence_store::kMaxFiles)
        names.removeLast();
    QStringList paths;
    for (const QString& name : names)
        paths << folder.filePath(name);
    return paths;
}

/// A file name for a licence id: its letters, digits and hyphens. A licence
/// id is text from a file, so nothing else of it reaches the file system.
QString safe_name(const std::string& licence_id)
{
    QString name;
    for (const char c : licence_id)
    {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-';
        if (keep && name.size() < 64)
            name += QLatin1Char(c);
    }
    return name.isEmpty() ? QStringLiteral("licence") : name;
}

bool same(const lic::status& left, const lic::status& right)
{
    return left.state == right.state && left.reason == right.reason &&
           left.value == right.value && left.key_id == right.key_id &&
           left.days_left == right.days_left;
}

bool same(const licence_view& left, const licence_view& right)
{
    if (!same(left.status, right.status) ||
        left.computer_name != right.computer_name ||
        left.files.size() != right.files.size())
        return false;
    for (std::size_t index = 0; index < left.files.size(); ++index)
    {
        const licence_file_view& a = left.files[index];
        const licence_file_view& b = right.files[index];
        if (!same(a.status, b.status) || a.file_name != b.file_name ||
            a.machine_wide != b.machine_wide || a.in_use != b.in_use)
            return false;
    }
    return true;
}

lic::time_point system_now()
{
    return std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
}

} // namespace

licence_store_options default_licence_store_options()
{
    licence_store_options options;
    const QString data =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!data.isEmpty())
    {
        options.user_directory =
            QDir(data).filePath(QStringLiteral("licences"));
        options.clock_file =
            QDir(data).filePath(QStringLiteral("licence-clock.json"));
    }
    options.machine_directory = machine_licence_directory();
    options.keys = lic::signed_file::product_trust_list();
    options.computer_name = QSysInfo::machineHostName();
    return options;
}

licence_store::licence_store(licence_store_options options)
    : options_(std::move(options))
{
    if (!options_.clock)
        options_.clock = system_now;
    view_.computer_name = options_.computer_name;
    load_clock();
}

lic::context licence_store::context(lic::time_point now) const
{
    lic::context where;
    where.now = now;
    where.latest_seen = clock_.latest_seen;
    where.build_time = options_.build_time;
    where.computer_name = options_.computer_name.toStdString();
    where.keys = options_.keys;
    where.revocations = &revocations_;
    return where;
}

void licence_store::load_clock()
{
    if (options_.clock_file.isEmpty())
        return;
    const auto bytes = read_signed_file(options_.clock_file);
    if (!bytes)
        return;
    const QJsonObject object = QJsonDocument::fromJson(*bytes).object();
    const auto seen = lic::parse_time(
        object.value(QLatin1String("latest_seen")).toString().toStdString());
    if (seen)
        clock_.latest_seen = *seen;
    for (const auto& token :
         object.value(QLatin1String("applied_resets")).toArray())
        clock_.applied_resets.push_back(token.toString().toStdString());
}

void licence_store::save_clock() const
{
    if (options_.clock_file.isEmpty() ||
        clock_.latest_seen == lic::time_point::min())
        return;
    QJsonArray resets;
    for (const std::string& token : clock_.applied_resets)
        resets.append(QString::fromStdString(token));
    QJsonObject object;
    object.insert(QLatin1String("latest_seen"),
                  QString::fromStdString(lic::format_time(clock_.latest_seen)));
    object.insert(QLatin1String("applied_resets"), resets);

    QDir().mkpath(QFileInfo(options_.clock_file).absolutePath());
    QSaveFile file(options_.clock_file);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.commit();
}

void licence_store::load_revocations()
{
    // The list with the highest sequence number wins, wherever it is. The
    // one built into this release is the starting point; a signed list in a
    // licence folder replaces it only when its number is higher.
    revocations_ = lic::builtin_revocations();
    for (const QString& directory :
         {options_.machine_directory, options_.user_directory})
    {
        for (const QString& path : files_in(directory, kRevocationsSuffix))
        {
            const auto bytes = read_signed_file(path);
            if (!bytes)
                continue;
            const auto list =
                lic::read_revocations(view_of(*bytes), options_.keys);
            if (!list)
                continue;
            revocations_ = lic::newer(revocations_, *list);
        }
    }
}

bool licence_store::refresh()
{
    const lic::time_point now = options_.clock();
    load_revocations();
    const lic::context where = context(now);

    licence_view found;
    found.computer_name = options_.computer_name;
    std::vector<lic::status> statuses;
    const auto read_folder = [&](const QString& directory, bool machine_wide)
    {
        for (const QString& path : files_in(directory, kLicenceSuffix))
        {
            const auto bytes = read_signed_file(path);
            licence_file_view file;
            file.file_name = QFileInfo(path).fileName();
            file.machine_wide = machine_wide;
            if (bytes)
            {
                file.status = lic::evaluate(view_of(*bytes), where);
            }
            else
            {
                file.status.state = lic::state::invalid;
                file.status.reason = lic::reason::malformed;
            }
            statuses.push_back(file.status);
            found.files.push_back(std::move(file));
        }
    };
    read_folder(options_.machine_directory, true);
    read_folder(options_.user_directory, false);

    found.status = lic::best(statuses);
    // Mark the file in use and put it first. `best` returns a copy, so the
    // first file with the same licence and state is the one.
    const auto used =
        std::ranges::find_if(found.files, [&](const licence_file_view& file)
                             { return same(file.status, found.status); });
    if (used != found.files.end())
    {
        used->in_use = true;
        std::rotate(found.files.begin(), used, used + 1);
    }

    // The time only moves forward here, and only a clock that is not behind
    // is written down.
    const lic::clock_state seen = lic::observe(clock_, now);
    if (seen != clock_)
    {
        clock_ = seen;
        save_clock();
    }

    const bool changed = !same(found, view_);
    view_ = std::move(found);
    return changed;
}

licence_import_result licence_store::import_file(const QString& path)
{
    licence_import_result result;
    // A file too large to be a licence is refused as not valid, unread.
    if (const QFileInfo info(path);
        info.isFile() &&
        info.size() > static_cast<qint64>(lic::signed_file::max_file_size))
    {
        result.outcome = licence_import_outcome::not_valid;
        result.status.state = lic::state::invalid;
        result.status.reason = lic::reason::malformed;
        result.detail = QString::fromStdString(
            lic::make_error_code(lic::errc::too_large).message());
        return result;
    }
    const auto bytes = read_signed_file(path);
    if (!bytes)
    {
        result.outcome = licence_import_outcome::unreadable;
        return result;
    }

    load_revocations();
    const lic::time_point now = options_.clock();
    result.status = lic::evaluate(view_of(*bytes), context(now));

    // Only a file whose signature verified has a value, and of those a
    // withdrawn licence or one for other computers is refused as well.
    if (!result.status.value || result.status.state == lic::state::invalid)
    {
        result.outcome = licence_import_outcome::not_valid;
        const auto envelope = lic::signed_file::parse(view_of(*bytes));
        if (!envelope)
        {
            result.detail = QString::fromStdString(envelope.error().message());
        }
        else
        {
            const auto key = lic::signed_file::verify(
                *envelope, lic::licence_purpose, options_.keys);
            if (!key)
                result.detail = QString::fromStdString(key.error().message());
        }
        return result;
    }

    // The same bytes in either folder: nothing to do.
    for (const QString& directory :
         {options_.machine_directory, options_.user_directory})
    {
        for (const QString& installed : files_in(directory, kLicenceSuffix))
        {
            const auto other = read_signed_file(installed);
            if (other && *other == *bytes)
            {
                result.outcome = licence_import_outcome::already_installed;
                return result;
            }
        }
    }

    if (options_.user_directory.isEmpty() ||
        !QDir().mkpath(options_.user_directory))
    {
        result.outcome = licence_import_outcome::unreadable;
        return result;
    }

    // A new name each time: importing never replaces an older file.
    const QDir folder(options_.user_directory);
    const QString base = safe_name(result.status.value->licence_id);
    QString target;
    for (int number = 1; number <= kMaxFiles; ++number)
    {
        const QString name =
            (number == 1 ? base
                         : base + QLatin1Char('-') + QString::number(number)) +
            QLatin1Char('.') + QLatin1String(kLicenceSuffix);
        if (!folder.exists(name))
        {
            target = folder.filePath(name);
            break;
        }
    }
    if (target.isEmpty())
    {
        result.outcome = licence_import_outcome::unreadable;
        return result;
    }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(*bytes) != bytes->size() || !file.commit())
    {
        result.outcome = licence_import_outcome::unreadable;
        return result;
    }

    // A verified licence may carry a token that sets the stored time back,
    // once, for a computer whose clock ran ahead.
    const lic::clock_state reset =
        lic::apply_reset(clock_, *result.status.value);
    if (reset != clock_)
    {
        clock_ = reset;
        save_clock();
        result.status = lic::evaluate(view_of(*bytes), context(now));
    }

    result.outcome = licence_import_outcome::imported;
    refresh();
    return result;
}

bool licence_store::remove(const QString& file_name)
{
    // Only a file this store listed in the user's folder: the name is
    // matched against the folder's own listing and never used as a path.
    bool removed = false;
    for (const QString& path :
         files_in(options_.user_directory, kLicenceSuffix))
    {
        if (QFileInfo(path).fileName() == file_name)
            removed = QFile::remove(path) || removed;
    }
    if (removed)
        refresh();
    return removed;
}

} // namespace vss::gui
