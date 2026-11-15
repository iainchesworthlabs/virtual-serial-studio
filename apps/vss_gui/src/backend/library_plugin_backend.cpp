//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/library_plugin_backend.hpp"

#include <vss/plugin/hash.hpp>
#include <vss/plugin/locations.hpp>
#include <vss/plugin/manifest.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace vss::gui
{
namespace
{

namespace plug = vss::plugin;
namespace fs = std::filesystem;

fs::path to_path(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    const std::u8string wide(bytes.begin(), bytes.end());
    return fs::path(wide);
}

std::string text_of(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

QString path_text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                             static_cast<qsizetype>(text.size()));
}

std::chrono::year_month_day today_date()
{
    return std::chrono::year_month_day(std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now()));
}

} // namespace

plugin_gather_options default_plugin_gather_options()
{
    plugin_gather_options options;
    const fs::path programs = to_path(QCoreApplication::applicationDirPath());
    options.program_directory = programs;
    const plug::plugin_locations places = plug::plugin_locations_for(programs);
    options.roots = places.roots;
    options.policy_file = places.policy_file;
    for (const plug::plugin_root& root : places.roots)
    {
        if (root.scope == plug::plugin_scope::for_everyone)
            options.machine_folder_text = text_of(root.directory);
        if (root.scope == plug::plugin_scope::for_user)
            options.user_folder_text = text_of(root.directory);
    }
    const QString data =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!data.isEmpty())
    {
        const fs::path folder = to_path(data) / "plugins";
        options.trust_store_file = folder / "trust.json";
        options.revocations_file = folder / "revocations.json";
    }
    // The signed key list of the official keys travels with Studio's own
    // plugin folder.
    for (const plug::plugin_root& root : places.roots)
    {
        if (root.scope == plug::plugin_scope::with_studio)
            options.official_keys_file = root.directory / "official-keys.json";
    }
    options.studio_version =
        QCoreApplication::applicationVersion().toStdString();
    return options;
}

// -- the worker ---------------------------------------------------------------

class LibraryPluginBackend::Worker final : public QObject
{
public:
    Worker(LibraryPluginBackend* owner, plugin_gather_options options)
        : owner_(owner), options_(std::move(options))
    {
        // Nothing is read here: this runs on the GUI thread. The first
        // `rescan` reads the trust store on the worker thread.
    }

    void set_licence(const vss::license::status& status)
    {
        options_.licence = status;
        rescan();
    }

    void rescan()
    {
        reload_store();
        store_.note_date(
            options_.today.value_or(std::chrono::floor<std::chrono::days>(
                std::chrono::system_clock::now())));
        gathered_ = gather_plugins(options_, store_);
        owner_->publish(
            std::make_shared<const plugin_snapshot>(gathered_.snapshot));
    }

    void rescan_and_report()
    {
        rescan();
        plugin_request_result result;
        result.kind = plugin_request_kind::rescan;
        result.count = static_cast<int>(gathered_.snapshot.plugins.size());
        owner_->notify_request_finished(result);
    }

    void decide(const plugin_decision_request& request)
    {
        const plugin_record* record = find(request.plugin_id, request.hash);
        if (record != nullptr)
        {
            plug::plugin_decision decision;
            decision.plugin_id = request.plugin_id.toStdString();
            decision.hash = record->plugin_hash;
            decision.key_id = record->key_id;
            decision.level = record->level;
            decision.enabled = request.switch_on;
            decision.decided_on = today_date();
            for (const QString& id : request.granted)
            {
                const auto parsed = plug::parse_permission(id.toStdString());
                if (parsed.has_value() &&
                    std::find(decision.granted.begin(), decision.granted.end(),
                              parsed.value()) == decision.granted.end())
                    decision.granted.push_back(parsed.value());
            }
            if (const plug::plugin_decision* before = store_.find(
                    decision.plugin_id, decision.hash, decision.key_id))
                decision.run_without_sandbox = before->run_without_sandbox;
            if (store_.decide(std::move(decision)).has_value())
                save();
        }
        rescan();
        finished(plugin_request_kind::decide, request.plugin_id);
    }

    void set_enabled(const QString& plugin_id, const QString& hash, bool on)
    {
        if (const plugin_record* record = find(plugin_id, hash))
        {
            const plug::plugin_decision* before = store_.find(
                plugin_id.toStdString(), record->plugin_hash, record->key_id);
            if (before != nullptr)
            {
                plug::plugin_decision changed = *before;
                changed.enabled = on;
                if (store_.decide(std::move(changed)).has_value())
                    save();
            }
        }
        rescan();
        finished(plugin_request_kind::set_enabled, plugin_id);
    }

    void set_no_isolation(const QString& plugin_id, const QString& hash,
                          bool on)
    {
        if (const plugin_record* record = find(plugin_id, hash))
        {
            const plug::plugin_decision* before = store_.find(
                plugin_id.toStdString(), record->plugin_hash, record->key_id);
            if (before != nullptr)
            {
                plug::plugin_decision changed = *before;
                changed.run_without_sandbox = on;
                if (store_.decide(std::move(changed)).has_value())
                    save();
            }
        }
        rescan();
        finished(plugin_request_kind::set_no_isolation, plugin_id);
    }

    void read_key(quint64 request, const QString& path)
    {
        const plugin_key_file_view view =
            read_key_file_view(to_path(path), gathered_, store_, nullptr);
        owner_->notify_key_file_read(request, view);
    }

    void approve_key(const QString& path, const QString& fingerprint)
    {
        plug::approved_key key;
        const plugin_key_file_view view =
            read_key_file_view(to_path(path), gathered_, store_, &key);
        plugin_request_result result;
        result.kind = plugin_request_kind::approve_key;
        // A file that is no longer a key file, or is not the one whose
        // fingerprint was compared, approves nothing.
        if (view.failure != plugin_key_file_view::problem::none ||
            view.fingerprint != fingerprint)
        {
            result.error = plugin_request_error::fingerprint_changed;
        }
        else
        {
            key.approved_on = today_date();
            if (store_.approve_key(key).has_value())
            {
                save();
                result.author = view.author;
            }
            else
            {
                result.error = plugin_request_error::fingerprint_changed;
            }
        }
        rescan();
        owner_->notify_request_finished(result);
    }

    void remove_key(const QString& key_id)
    {
        if (store_.forget_key(key_id.toStdString()))
            save();
        rescan();
        finished(plugin_request_kind::remove_key, {});
    }

    void run_expired(const QString& plugin_id, const QString& hash)
    {
        options_.run_expired.insert(plugin_id.toStdString() + "|" +
                                    hash.toStdString());
        rescan();
        finished(plugin_request_kind::run_expired, plugin_id);
    }

    void inspect(quint64 request, const QString& folder)
    {
        plugin_inspection_view out;
        out.folder = folder;
        const fs::path source = to_path(folder);
        std::error_code ignored;
        std::ifstream stream(source / "plugin.json", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        const auto manifest =
            stream ? plug::parse_manifest(text)
                   : vss::result<plug::manifest>(
                         vss::make_error_code(vss::errc::io_error));
        if (!manifest.has_value())
        {
            out.failure = plugin_inspection_view::problem::not_a_plugin;
            owner_->notify_plugin_inspected(request, out);
            return;
        }
        out.plugin.id = QString::fromStdString(manifest->id);
        out.plugin.name = QString::fromStdString(manifest->name);
        if (gathered_.user_root.empty() ||
            !gathered_.snapshot.user_plugins_allowed)
        {
            out.failure = plugin_inspection_view::problem::cannot_write;
            owner_->notify_plugin_inspected(request, out);
            return;
        }
        for (const plugin_record& record : gathered_.records)
        {
            if (record.scope == plug::plugin_scope::for_user &&
                record.id == out.plugin.id)
            {
                out.failure =
                    plugin_inspection_view::problem::already_installed;
                owner_->notify_plugin_inspected(request, out);
                return;
            }
        }
        // The verdict the plugin would get as one of the user's own: the folder
        // is looked at in place, as the only plugin of a root of its own.
        plugin_gather_options look = options_;
        look.roots.clear();
        plug::plugin_root root;
        root.directory = source.parent_path();
        root.scope = plug::plugin_scope::for_user;
        look.roots.push_back(root);
        const plugin_gathered found = gather_plugins(look, store_);
        const QString where = path_text(source);
        for (const plugin_view& each : found.snapshot.plugins)
        {
            if (each.folder == where)
            {
                out.plugin = each;
                out.failure = plugin_inspection_view::problem::none;
                out.can_install = each.state != plugin_state::cannot_be_used;
            }
        }
        if (out.failure != plugin_inspection_view::problem::none)
            out.failure = plugin_inspection_view::problem::not_a_plugin;
        owner_->notify_plugin_inspected(request, out);
    }

    void add_plugin(const QString& folder)
    {
        plugin_request_result result;
        result.kind = plugin_request_kind::add_plugin;
        const fs::path source = to_path(folder);
        std::error_code ignored;
        const fs::path manifest_file = source / "plugin.json";
        if (!fs::is_regular_file(manifest_file, ignored))
        {
            result.error = plugin_request_error::not_a_plugin;
            owner_->notify_request_finished(result);
            return;
        }
        std::ifstream stream(manifest_file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        const auto manifest = plug::parse_manifest(text);
        if (!manifest.has_value())
        {
            result.error = plugin_request_error::not_a_plugin;
            owner_->notify_request_finished(result);
            return;
        }
        const QString name = QString::fromStdString(manifest->name);
        result.plugin_name = name;
        if (gathered_.user_root.empty() ||
            !gathered_.snapshot.user_plugins_allowed)
        {
            result.error = plugin_request_error::cannot_write;
            owner_->notify_request_finished(result);
            return;
        }
        for (const plugin_record& record : gathered_.records)
        {
            if (record.scope == plug::plugin_scope::for_user &&
                record.id == QString::fromStdString(manifest->id))
            {
                result.error = plugin_request_error::already_installed;
                owner_->notify_request_finished(result);
                return;
            }
        }
        fs::path target = gathered_.user_root / source.filename();
        if (fs::exists(target, ignored))
        {
            result.error = plugin_request_error::already_installed;
            owner_->notify_request_finished(result);
            return;
        }
        fs::create_directories(gathered_.user_root, ignored);
        std::error_code failure;
        fs::copy(source, target,
                 fs::copy_options::recursive | fs::copy_options::skip_symlinks,
                 failure);
        if (failure)
        {
            fs::remove_all(target, ignored);
            result.error = plugin_request_error::cannot_write;
            owner_->notify_request_finished(result);
            return;
        }
        rescan();
        owner_->notify_request_finished(result);
    }

    void remove_plugin(const QString& plugin_id, const QString& hash)
    {
        if (const plugin_record* record = find(plugin_id, hash))
        {
            if (record->scope == plug::plugin_scope::for_user)
            {
                // To the bin, not deleted.
                QFile::moveToTrash(record->folder);
            }
        }
        rescan();
        finished(plugin_request_kind::remove_plugin, plugin_id);
    }

private:
    const plugin_record* find(const QString& plugin_id,
                              const QString& hash) const
    {
        for (const plugin_record& record : gathered_.records)
        {
            if (record.id == plugin_id && record.hash == hash)
                return &record;
        }
        return nullptr;
    }

    void reload_store()
    {
        if (options_.trust_store_file.empty())
            return;
        std::error_code ignored;
        if (!fs::exists(options_.trust_store_file, ignored))
            return;
        auto loaded = plug::load_trust_store(options_.trust_store_file);
        if (loaded.has_value())
            store_ = std::move(loaded.value());
    }

    void save()
    {
        if (options_.trust_store_file.empty())
            return;
        std::error_code ignored;
        fs::create_directories(options_.trust_store_file.parent_path(),
                               ignored);
        static_cast<void>(
            plug::save_trust_store(options_.trust_store_file, store_));
    }

    void finished(plugin_request_kind kind, const QString& plugin_id)
    {
        plugin_request_result result;
        result.kind = kind;
        result.plugin_id = plugin_id;
        owner_->notify_request_finished(result);
    }

    LibraryPluginBackend* owner_;
    plugin_gather_options options_;
    plug::trust_store store_;
    plugin_gathered gathered_;
};

// -- the backend --------------------------------------------------------------

LibraryPluginBackend::LibraryPluginBackend(plugin_gather_options options,
                                           QObject* parent)
    : PluginBackend(parent),
      snapshot_(std::make_shared<const plugin_snapshot>())
{
    worker_ = new Worker(this, std::move(options));
    worker_->moveToThread(&thread_);
    thread_.setObjectName(QStringLiteral("vss-plugins"));
    thread_.start();
    rescan();
}

LibraryPluginBackend::~LibraryPluginBackend()
{
    thread_.quit();
    thread_.wait();
    delete worker_;
}

plugin_snapshot_ptr LibraryPluginBackend::snapshot() const
{
    const std::scoped_lock lock(mutex_);
    return snapshot_;
}

void LibraryPluginBackend::publish(plugin_snapshot_ptr snapshot)
{
    {
        const std::scoped_lock lock(mutex_);
        snapshot_ = std::move(snapshot);
    }
    notify_snapshot_changed();
}

void LibraryPluginBackend::set_licence(const vss::license::status& status)
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, status] { worker->set_licence(status); },
        Qt::QueuedConnection);
}

void LibraryPluginBackend::decide(const plugin_decision_request& request)
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, request] { worker->decide(request); },
        Qt::QueuedConnection);
}

void LibraryPluginBackend::set_enabled(const QString& plugin_id,
                                       const QString& hash, bool on)
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, plugin_id, hash, on]
        { worker->set_enabled(plugin_id, hash, on); }, Qt::QueuedConnection);
}

quint64 LibraryPluginBackend::read_key_file(const QString& path)
{
    const quint64 number = next_request_.fetch_add(1);
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, number, path]
        { worker->read_key(number, path); }, Qt::QueuedConnection);
    return number;
}

void LibraryPluginBackend::approve_key(const QString& path,
                                       const QString& fingerprint)
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, path, fingerprint]
        { worker->approve_key(path, fingerprint); }, Qt::QueuedConnection);
}

void LibraryPluginBackend::remove_key(const QString& key_id)
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, key_id] { worker->remove_key(key_id); },
        Qt::QueuedConnection);
}

void LibraryPluginBackend::set_no_isolation(const QString& plugin_id,
                                            const QString& hash, bool on)
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, plugin_id, hash, on]
        { worker->set_no_isolation(plugin_id, hash, on); },
        Qt::QueuedConnection);
}

void LibraryPluginBackend::start_again(const QString& /*plugin_id*/)
{
    // Nothing can be started in this build. The page shows the plugin as it
    // is, which is the truth.
    rescan();
}

void LibraryPluginBackend::run_expired(const QString& plugin_id,
                                       const QString& hash)
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, plugin_id, hash]
        { worker->run_expired(plugin_id, hash); }, Qt::QueuedConnection);
}

void LibraryPluginBackend::rescan()
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_] { worker->rescan_and_report(); },
        Qt::QueuedConnection);
}

quint64 LibraryPluginBackend::inspect_plugin(const QString& folder)
{
    const quint64 number = next_request_.fetch_add(1);
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, number, folder]
        { worker->inspect(number, folder); }, Qt::QueuedConnection);
    return number;
}

void LibraryPluginBackend::add_plugin(const QString& folder)
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, folder] { worker->add_plugin(folder); },
        Qt::QueuedConnection);
}

void LibraryPluginBackend::remove_plugin(const QString& plugin_id,
                                         const QString& hash,
                                         bool /*delete_saved_data*/)
{
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, plugin_id, hash]
        { worker->remove_plugin(plugin_id, hash); }, Qt::QueuedConnection);
}

} // namespace vss::gui
