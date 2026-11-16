//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/plugin_mapping.hpp"

#include <vss/plugin/error.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/signature.hpp>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace vss::gui
{
namespace
{

namespace plug = vss::plugin;
namespace fs = std::filesystem;

QString path_text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                             static_cast<qsizetype>(text.size()));
}

QString from_std(const std::string& text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::optional<std::string> read_text_file(const fs::path& path,
                                          std::size_t limit)
{
    if (path.empty())
        return std::nullopt;
    std::error_code ignored;
    if (!fs::is_regular_file(path, ignored))
        return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
    if (text.size() > limit)
        return std::nullopt;
    return text;
}

} // namespace

QString hash_text(const plug::plugin_hash& hash)
{
    return from_std(plug::to_hex(hash.bytes));
}

std::optional<plug::plugin_hash> parse_hash_text(const QString& text)
{
    if (text.size() != 128)
        return std::nullopt;
    plug::plugin_hash hash;
    for (qsizetype index = 0; index < 64; ++index)
    {
        bool ok = false;
        const int value = text.mid(index * 2, 2).toInt(&ok, 16);
        if (!ok)
            return std::nullopt;
        hash.bytes[static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value);
    }
    return hash;
}

plugin_reason_state state_for_reason(plug::plugin_errc code,
                                     bool override_possible)
{
    using code_t = plug::plugin_errc;
    plugin_reason_state out;
    switch (code)
    {
    case code_t::success:
        out.state = plugin_state::off;
        out.note = plugin_note::none;
        break;
    case code_t::manifest_unsupported_version:
    case code_t::api_incompatible:
        out.state = plugin_state::needs_newer_studio;
        out.note = plugin_note::newer_studio_version;
        break;
    case code_t::capability_missing:
        out.state = plugin_state::needs_newer_studio;
        out.note = plugin_note::newer_studio_capability;
        break;
    case code_t::shadowed:
        out.state = plugin_state::not_used;
        out.note = plugin_note::not_used_same;
        break;
    case code_t::reserved_id:
        out.state = plugin_state::not_used;
        out.note = plugin_note::not_used_reserved;
        break;
    case code_t::policy:
        out.state = plugin_state::blocked;
        out.note = plugin_note::blocked_named;
        break;
    case code_t::sandbox_insufficient:
        out.state = override_possible ? plugin_state::needs_isolation
                                      : plugin_state::blocked;
        out.note = override_possible ? plugin_note::isolation_override
                                     : plugin_note::isolation_no_override;
        break;
    case code_t::not_licensed:
        out.state = plugin_state::needs_licence;
        out.note = plugin_note::needs_licence;
        break;
    case code_t::signature_missing:
    case code_t::key_unknown:
    case code_t::changed:
        out.state = plugin_state::needs_decision;
        out.note = plugin_note::decision_first;
        break;
    case code_t::quarantined:
    case code_t::start_failed:
    case code_t::no_hello:
    case code_t::bad_hello:
    case code_t::crashed:
    case code_t::exited:
        out.state = plugin_state::stopped;
        out.note = plugin_note::stopped_failures;
        break;
    case code_t::unresponsive:
        out.state = plugin_state::stopped;
        out.note = plugin_note::stopped_unresponsive;
        break;
    case code_t::misbehaving:
    case code_t::permission_denied:
        out.state = plugin_state::stopped;
        out.note = plugin_note::stopped_limit;
        break;
    case code_t::signature_invalid:
    case code_t::unlisted_file:
        out.state = plugin_state::cannot_be_used;
        out.note = plugin_note::invalid_signature;
        break;
    case code_t::revoked:
        out.state = plugin_state::cannot_be_used;
        out.note = plugin_note::withdrawn;
        break;
    case code_t::expired:
        out.state = plugin_state::cannot_be_used;
        out.note = plugin_note::expired;
        break;
    case code_t::manifest_invalid:
    case code_t::manifest_too_large:
    case code_t::too_many:
    case code_t::layout_invalid:
    case code_t::file_missing:
        out.state = plugin_state::cannot_be_used;
        out.note = plugin_note::invalid_manifest;
        break;
    }
    return out;
}

plugin_key_file_view read_key_file_view(const fs::path& path,
                                        const key_check_context& found,
                                        const plug::trust_store& store,
                                        plug::approved_key* key_out)
{
    plugin_key_file_view view;
    view.path = path_text(path);
    view.failure = plugin_key_file_view::problem::not_a_key_file;

    const auto text = read_text_file(path, std::size_t{16} * 1024);
    if (!text.has_value())
        return view;
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray::fromStdString(*text), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return view;
    const QJsonObject object = document.object();
    const QString key_id = object.value(QStringLiteral("key_id")).toString();
    const QString public_key =
        object.value(QStringLiteral("public_key")).toString();
    const QString name = object.value(QStringLiteral("name")).toString();
    if (key_id.isEmpty() || public_key.size() != 64 || name.isEmpty())
        return view;
    if (!plug::is_key_id(key_id.toStdString()) ||
        name.toUtf8().size() > static_cast<qsizetype>(plug::max_name_length))
        return view;
    // A name with a control character or a direction mark is not plain text.
    for (const QChar letter : name)
    {
        const char16_t unit = letter.unicode();
        if (unit < 0x20 || (unit >= 0x7f && unit <= 0x9f) ||
            (unit >= 0x202a && unit <= 0x202e) ||
            (unit >= 0x2066 && unit <= 0x2069) || unit == 0x200e ||
            unit == 0x200f || unit == 0x061c || unit == 0x2028 ||
            unit == 0x2029)
        {
            view.failure = plugin_key_file_view::problem::text_not_plain;
            return view;
        }
    }
    plug::approved_key approved;
    approved.key_id = key_id.toStdString();
    approved.name = name.toStdString();
    for (qsizetype index = 0; index < 32; ++index)
    {
        bool ok = false;
        const int value = public_key.mid(index * 2, 2).toInt(&ok, 16);
        if (!ok)
            return view;
        approved.public_key[static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>(value);
    }

    view.author = name;
    view.key_id = key_id;
    const plug::sha512_digest digest = plug::sha512_of(std::string_view(
        reinterpret_cast<const char*>(approved.public_key.data()),
        approved.public_key.size()));
    view.fingerprint = from_std(plug::to_hex(digest)).left(16);

    // An id that an official key or an administrator's key has is never taken.
    bool taken_by_other = false;
    if (found.official.has_value() &&
        found.official->find(approved.key_id, plug::plugin_purpose) != nullptr)
        taken_by_other = true;
    if (found.rules.has_value())
    {
        for (const plug::policy_key& each : found.rules->allow_keys)
        {
            if (each.key_id == approved.key_id)
                taken_by_other = true;
        }
    }
    if (taken_by_other)
    {
        view.failure =
            plugin_key_file_view::problem::id_is_official_or_administrators;
        return view;
    }
    if (const plug::approved_key* existing = store.find_key(approved.key_id);
        existing != nullptr && existing->public_key != approved.public_key)
    {
        view.failure = plugin_key_file_view::problem::id_in_use;
        return view;
    }

    // Is the file inside a plugin folder?
    std::error_code ignored;
    const fs::path where = fs::weakly_canonical(path, ignored);
    for (const fs::path& folder : found.plugin_folders)
    {
        const fs::path canonical = fs::weakly_canonical(folder, ignored);
        const auto [mismatch, unused] = std::mismatch(
            canonical.begin(), canonical.end(), where.begin(), where.end());
        static_cast<void>(unused);
        if (mismatch == canonical.end())
            view.inside_plugin_folder = true;
    }

    view.failure = plugin_key_file_view::problem::none;
    if (key_out != nullptr)
        *key_out = std::move(approved);
    return view;
}

} // namespace vss::gui
