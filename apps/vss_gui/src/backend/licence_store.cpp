//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/licence_store.hpp"

#include <QByteArray>

#include <string>
#include <utility>

namespace vss::gui
{

namespace
{

namespace lic = vss::license;

QString text_of(const std::string& utf8)
{
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

licence_import_outcome outcome_of(lic::import_outcome outcome)
{
    switch (outcome)
    {
    case lic::import_outcome::imported:
        return licence_import_outcome::imported;
    case lic::import_outcome::already_installed:
        return licence_import_outcome::already_installed;
    case lic::import_outcome::not_valid:
        return licence_import_outcome::not_valid;
    case lic::import_outcome::unreadable:
        break;
    }
    return licence_import_outcome::unreadable;
}

} // namespace

licence_view to_licence_view(const lic::store_view& view)
{
    licence_view result;
    result.status = view.status;
    result.computer_name = text_of(view.computer_name);
    result.files.reserve(view.files.size());
    for (const lic::found_licence& found : view.files)
    {
        licence_file_view file;
        file.status = found.status;
        file.file_name = text_of(found.file_name);
        file.machine_wide = found.machine_wide;
        file.in_use = found.in_use;
        result.files.push_back(std::move(file));
    }
    return result;
}

licence_import_result to_licence_import_result(const lic::import_result& result)
{
    licence_import_result converted;
    converted.outcome = outcome_of(result.outcome);
    converted.status = result.status;
    converted.detail = text_of(result.detail);
    return converted;
}

std::filesystem::path to_path(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}

licence_store::licence_store(lic::store_options options)
    : store_(std::move(options))
{
    view_ = to_licence_view(store_.view());
}

bool licence_store::refresh()
{
    const bool changed = store_.refresh();
    view_ = to_licence_view(store_.view());
    return changed;
}

licence_import_result licence_store::import_file(const QString& path)
{
    const lic::import_result result = store_.import_file(to_path(path));
    view_ = to_licence_view(store_.view());
    return to_licence_import_result(result);
}

bool licence_store::remove(const QString& file_name)
{
    const bool removed = store_.remove(file_name.toStdString());
    view_ = to_licence_view(store_.view());
    return removed;
}

} // namespace vss::gui
