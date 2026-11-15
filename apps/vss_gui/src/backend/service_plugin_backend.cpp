//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/service_plugin_backend.hpp"

#include "backend/plugin_mapping.hpp"

#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/locations.hpp>
#include <vss/plugin/trust_store.hpp>

#include <boost/asio/post.hpp>

#include <QCoreApplication>
#include <QStandardPaths>

#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <random>

namespace vss::gui
{
namespace
{

namespace plug = vss::plugin;
namespace host = vss::plugin::host;
namespace fs = std::filesystem;

fs::path to_path(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    const std::u8string wide(bytes.begin(), bytes.end());
    return fs::path(wide);
}

QString path_text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                             static_cast<qsizetype>(text.size()));
}

std::string text_of(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::chrono::year_month_day today_date()
{
    return std::chrono::year_month_day(std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now()));
}

/// The trust store as the file has it, or an empty one when there is none.
plug::trust_store load_store(const fs::path& file)
{
    if (file.empty())
        return {};
    std::error_code ignored;
    if (!fs::exists(file, ignored))
        return {};
    auto loaded = plug::load_trust_store(file);
    return loaded.has_value() ? std::move(loaded.value()) : plug::trust_store{};
}

const host::service_plugin* find_plugin(const host::service_snapshot& snapshot,
                                        const QString& id, const QString& hash)
{
    for (const host::service_plugin& each : snapshot.plugins)
    {
        if (QString::fromStdString(each.id) == id &&
            hash_text(each.hash) == hash)
            return &each;
    }
    return nullptr;
}

plug::plugin_decision decision_of(const host::service_plugin& plugin)
{
    plug::plugin_decision decision;
    decision.plugin_id = plugin.id;
    decision.hash = plugin.hash;
    decision.key_id = plugin.key_id;
    decision.level = plugin.level.value_or(plug::trust_level::unsigned_plugin);
    decision.enabled = plugin.enabled;
    decision.granted = plugin.granted;
    decision.decided_on = today_date();
    return decision;
}

} // namespace

service_plugin_options default_service_plugin_options()
{
    service_plugin_options options;
    const fs::path programs = to_path(QCoreApplication::applicationDirPath());
    options.program_directory = programs;
    const plug::plugin_locations places = plug::plugin_locations_for(programs);
    options.roots = places.roots;
    options.policy_file = places.policy_file;
    for (const plug::plugin_root& root : places.roots)
    {
        if (root.scope == plug::plugin_scope::for_everyone)
            options.machine_folder_text = path_text(root.directory);
        if (root.scope == plug::plugin_scope::for_user)
        {
            options.user_folder_text = path_text(root.directory);
            options.user_root = root.directory;
        }
    }
    const QString data =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!data.isEmpty())
    {
        const fs::path folder = to_path(data) / "plugins";
        options.trust_file = folder / "trust.json";
        options.data_root = folder / "data";
    }
    options.studio_version =
        QCoreApplication::applicationVersion().toStdString();
    options.seed = std::random_device{}();
    return options;
}

ServicePluginBackend::ServicePluginBackend(service_plugin_options options,
                                           QObject* parent)
    : PluginBackend(parent), options_(std::move(options)),
      guard_(boost::asio::make_work_guard(io_))
{
    // The service writes the trust store and the plugins write their data
    // here; neither makes the folders above them.
    std::error_code ignored;
    if (!options_.trust_file.empty())
        fs::create_directories(options_.trust_file.parent_path(), ignored);
    if (!options_.data_root.empty())
        fs::create_directories(options_.data_root, ignored);
    io_thread_ = std::thread([this] { io_.run(); });

    host::service_options service;
    service.roots = options_.roots;
    service.computer.program_directory = options_.program_directory;
    service.computer.policy_file = options_.policy_file;
    service.computer.trust_file = options_.trust_file;
    service.plan.data_root = options_.data_root;
    service.plan.seed = options_.seed;
    // Studio's own engine capabilities are not offered to plugins yet.
    service.licence = [this]
    {
        const std::scoped_lock lock(mutex_);
        return licence_;
    };
    service_ = host::plugin_service::create(
        io_.get_executor(), slow_.get_executor(), std::move(service));
    service_->on_change([this] { after_change(); });
    service_->rescan();
}

ServicePluginBackend::~ServicePluginBackend()
{
    try
    {
        shut_down();
    }
    catch (...)
    {
        // Nothing more can be done at the end of the program.
    }
}

void ServicePluginBackend::shut_down()
{
    if (down_.exchange(true))
        return;
    // Every plugin is asked to stop and then ended. The service bounds this
    // at five seconds; the wait here is a little longer, and nothing on the
    // GUI thread depends on a plugin answering.
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    service_->shut_down([done] { done->set_value(); });
    finished.wait_for(std::chrono::seconds(7));
    guard_.reset();
    io_.stop();
    if (io_thread_.joinable())
        io_thread_.join();
    slow_.join();
}

void ServicePluginBackend::set_licence(const vss::license::status& status)
{
    {
        const std::scoped_lock lock(mutex_);
        licence_ = status;
    }
    if (!down_.load())
        service_->rescan();
}

void ServicePluginBackend::after_change()
{
    if (down_.load())
        return;
    // Called on the service's strand: nothing here waits.
    boost::asio::post(
        slow_,
        [this]
        {
            refresh_extras();
            if (rescan_asked_.load())
            {
                const auto now = service_->snapshot();
                if (now && !now->scanning && rescan_asked_.exchange(false))
                {
                    plugin_request_result result;
                    result.kind = plugin_request_kind::rescan;
                    result.count = static_cast<int>(now->plugins.size());
                    notify_request_finished(result);
                }
            }
            notify_snapshot_changed();
        });
}

plug::inspect_context ServicePluginBackend::context_now() const
{
    plug::computer_options computer;
    computer.program_directory = options_.program_directory;
    computer.policy_file = options_.policy_file;
    computer.trust_file = options_.trust_file;
    plug::inspect_context context = plug::context_for_this_computer(computer);
    context.sandbox = host::available_sandbox().level();
    {
        const std::scoped_lock lock(mutex_);
        context.licence = licence_;
    }
    context.licence_known = true;
    return context;
}

void ServicePluginBackend::refresh_extras()
{
    // What the service does not carry: the manifests, the decisions and keys,
    // the policy and what this computer can apply. Read off the GUI thread.
    const auto snapshot = service_->snapshot();
    auto next = std::make_shared<service_mapping_inputs>();
    plug::computer_options computer;
    computer.program_directory = options_.program_directory;
    computer.policy_file = options_.policy_file;
    computer.trust_file = options_.trust_file;
    const plug::inspect_context context =
        plug::context_for_this_computer(computer);
    next->rules = context.rules;
    next->install = context.install;
    if (context.trust.has_value())
        next->trust = *context.trust;
    next->available = host::available_sandbox();
    next->studio_version = options_.studio_version;
    next->user_folder = options_.user_folder_text;
    next->machine_folder = options_.machine_folder_text;

    std::shared_ptr<const service_mapping_inputs> before;
    {
        const std::scoped_lock lock(mutex_);
        next->licence = licence_;
        before = extras_;
    }
    for (const host::service_plugin& plugin : snapshot->plugins)
    {
        const std::string key = manifest_key(plugin);
        if (before != nullptr)
        {
            if (const auto known = before->manifests.find(key);
                known != before->manifests.end())
            {
                next->manifests.emplace(key, known->second);
                continue;
            }
        }
        if (plugin.hash == plug::plugin_hash{})
            continue;
        std::ifstream stream(plugin.directory / "plugin.json",
                             std::ios::binary);
        if (!stream)
            continue;
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        auto manifest = plug::parse_manifest(text);
        if (manifest.has_value())
            next->manifests.emplace(key, std::move(manifest.value()));
    }
    const std::scoped_lock lock(mutex_);
    extras_ = std::move(next);
    ++extras_version_;
}

plugin_snapshot_ptr ServicePluginBackend::snapshot() const
{
    const std::shared_ptr<const host::service_snapshot> found =
        service_->snapshot();
    const std::scoped_lock lock(mutex_);
    if (cached_ && found && cached_revision_ == found->revision &&
        cached_extras_ == extras_version_)
        return cached_;
    service_mapping_inputs fallback;
    const service_mapping_inputs& inputs = extras_ ? *extras_ : fallback;
    plugin_snapshot mapped =
        found ? map_service_snapshot(*found, inputs) : plugin_snapshot{};
    cached_ = std::make_shared<const plugin_snapshot>(std::move(mapped));
    cached_revision_ = found ? found->revision : 0;
    cached_extras_ = extras_version_;
    return cached_;
}

// -- requests -----------------------------------------------------------------

void ServicePluginBackend::decide(const plugin_decision_request& request)
{
    boost::asio::post(
        slow_,
        [this, request]
        {
            const auto now = service_->snapshot();
            const host::service_plugin* plugin =
                find_plugin(*now, request.plugin_id, request.hash);
            if (plugin == nullptr)
            {
                // The version the question was about is gone: nothing is
                // stored, and the page shows what is there now.
                service_->rescan();
                return;
            }
            plug::plugin_decision decision = decision_of(*plugin);
            decision.enabled = request.switch_on;
            decision.granted.clear();
            for (const QString& id : request.granted)
            {
                const auto parsed = plug::parse_permission(id.toStdString());
                if (parsed.has_value() &&
                    std::find(decision.granted.begin(), decision.granted.end(),
                              *parsed) == decision.granted.end())
                    decision.granted.push_back(*parsed);
            }
            // An earlier override for this copy stays.
            const plug::trust_store store = load_store(options_.trust_file);
            if (const plug::plugin_decision* before =
                    store.find(plugin->id, plugin->hash, plugin->key_id))
                decision.run_without_sandbox = before->run_without_sandbox;
            service_->decide(std::move(decision));
            plugin_request_result result;
            result.kind = plugin_request_kind::decide;
            result.plugin_id = request.plugin_id;
            notify_request_finished(result);
        });
}

void ServicePluginBackend::set_enabled(const QString& plugin_id,
                                       const QString& /*hash*/, bool on)
{
    boost::asio::post(slow_, [this, plugin_id, on]
                      { service_->set_enabled(plugin_id.toStdString(), on); });
}

void ServicePluginBackend::set_no_isolation(const QString& plugin_id,
                                            const QString& hash, bool on)
{
    boost::asio::post(slow_,
                      [this, plugin_id, hash, on]
                      {
                          const auto now = service_->snapshot();
                          const host::service_plugin* plugin =
                              find_plugin(*now, plugin_id, hash);
                          if (plugin == nullptr)
                              return;
                          plug::plugin_decision decision = decision_of(*plugin);
                          decision.run_without_sandbox = on;
                          service_->decide(std::move(decision));
                      });
}

void ServicePluginBackend::start_again(const QString& plugin_id)
{
    // "Call it on the service's strand": the io_context carries it.
    boost::asio::post(io_,
                      [this, id = plugin_id.toStdString()]
                      {
                          if (down_.load())
                              return;
                          if (const auto runner = service_->runner(id))
                              runner->start();
                      });
}

void ServicePluginBackend::run_expired(const QString& /*plugin_id*/,
                                       const QString& /*hash*/)
{
    // The service has no "run it until Studio closes" for an expired plugin.
}

void ServicePluginBackend::rescan()
{
    rescan_asked_.store(true);
    boost::asio::post(slow_, [this] { service_->rescan(); });
}

quint64 ServicePluginBackend::read_key_file(const QString& path)
{
    const quint64 number = next_request_.fetch_add(1);
    boost::asio::post(slow_,
                      [this, number, path]
                      {
                          const plug::inspect_context context = context_now();
                          key_check_context check;
                          check.official = context.official;
                          check.rules = context.rules;
                          for (const host::service_plugin& each :
                               service_->snapshot()->plugins)
                              check.plugin_folders.push_back(each.directory);
                          const plug::trust_store store =
                              load_store(options_.trust_file);
                          notify_key_file_read(
                              number, read_key_file_view(to_path(path), check,
                                                         store, nullptr));
                      });
    return number;
}

void ServicePluginBackend::approve_key(const QString& path,
                                       const QString& fingerprint)
{
    boost::asio::post(
        slow_,
        [this, path, fingerprint]
        {
            const plug::inspect_context context = context_now();
            key_check_context check;
            check.official = context.official;
            check.rules = context.rules;
            plug::trust_store store = load_store(options_.trust_file);
            plug::approved_key key;
            const plugin_key_file_view view =
                read_key_file_view(to_path(path), check, store, &key);
            plugin_request_result result;
            result.kind = plugin_request_kind::approve_key;
            if (view.failure != plugin_key_file_view::problem::none ||
                view.fingerprint != fingerprint)
            {
                // Not a key file any more, or not the one that was compared.
                result.error = plugin_request_error::fingerprint_changed;
            }
            else
            {
                key.approved_on = today_date();
                std::error_code ignored;
                fs::create_directories(options_.trust_file.parent_path(),
                                       ignored);
                if (store.approve_key(key).has_value() &&
                    plug::save_trust_store(options_.trust_file, store)
                        .has_value())
                    result.author = view.author;
                else
                    result.error = plugin_request_error::fingerprint_changed;
            }
            service_->rescan();
            notify_request_finished(result);
        });
}

void ServicePluginBackend::remove_key(const QString& key_id)
{
    boost::asio::post(slow_,
                      [this, key_id]
                      {
                          plug::trust_store store =
                              load_store(options_.trust_file);
                          if (store.forget_key(key_id.toStdString()))
                              static_cast<void>(plug::save_trust_store(
                                  options_.trust_file, store));
                          service_->rescan();
                          plugin_request_result result;
                          result.kind = plugin_request_kind::remove_key;
                          notify_request_finished(result);
                      });
}

quint64 ServicePluginBackend::inspect_plugin(const QString& folder)
{
    const quint64 number = next_request_.fetch_add(1);
    boost::asio::post(
        slow_,
        [this, number, folder]
        {
            plugin_inspection_view out;
            out.folder = folder;
            const fs::path source = to_path(folder);
            const plug::inspect_context context = context_now();
            const plug::inspection found =
                plug::inspect_plugin(source, context);
            if (!found.declared.has_value())
            {
                out.failure = plugin_inspection_view::problem::not_a_plugin;
                notify_plugin_inspected(number, out);
                return;
            }
            plugin_view& view = out.plugin;
            fill_view_from_manifest(view, *found.declared);
            view.folder = path_text(source);
            view.installed_for = plugin_installed_for::you_only;
            view.key_id = QString::fromStdString(found.key_id);
            if (!found.files.empty())
            {
                view.hash = hash_text(found.hash);
                view.fingerprint =
                    QString::fromStdString(found.hash.short_text());
            }
            if (found.level.has_value())
            {
                switch (*found.level)
                {
                case plug::trust_level::official:
                    view.trust = plugin_trust::official;
                    break;
                case plug::trust_level::third_party_signed:
                    view.trust = plugin_trust::third_party;
                    break;
                case plug::trust_level::signed_unknown_key:
                    view.trust = plugin_trust::unknown_key;
                    break;
                case plug::trust_level::unsigned_plugin:
                    view.trust = plugin_trust::unsigned_plugin;
                    break;
                }
            }
            view.isolation =
                context.sandbox == plug::sandbox_level::full
                    ? plugin_isolation::full
                    : (context.sandbox == plug::sandbox_level::partial
                           ? plugin_isolation::partial
                           : plugin_isolation::none);
            if (view.isolation == plugin_isolation::partial)
                view.isolation_missing =
                    sandbox_missing_text(host::available_sandbox());

            // What the plugin would be once it was installed for this user.
            if (found.problem.failed())
            {
                view.state = plugin_state::cannot_be_used;
                view.note = plugin_note::invalid_manifest;
                view.reason_code =
                    QString::fromStdString(found.problem.message());
                if (found.problem.category() == plug::plugin_category())
                {
                    const auto reason = state_for_reason(
                        static_cast<plug::plugin_errc>(found.problem.value()),
                        false);
                    view.state = reason.state;
                    view.note = reason.note;
                }
            }
            else if (!found.level.has_value())
            {
                view.state = plugin_state::cannot_be_used;
                view.note = plugin_note::invalid_signature;
            }
            else if (found.run.failed() &&
                     found.run.category() == plug::plugin_category())
            {
                const auto reason = state_for_reason(
                    static_cast<plug::plugin_errc>(found.run.value()),
                    plug::may_override_sandbox(context.rules ? &*context.rules
                                                             : nullptr,
                                               context.install));
                view.state = reason.state;
                view.note = reason.note;
            }
            else
            {
                // Nothing is wrong: it asks for its decision first.
                view.state = plugin_state::needs_decision;
                view.note = plugin_note::decision_first;
            }
            view.detail = QString::fromStdString(found.run_detail);

            out.failure = plugin_inspection_view::problem::none;
            out.can_install = !plug::blocks_install(found);
            if (options_.user_root.empty() ||
                (context.rules.has_value() && !context.rules->user_plugins))
            {
                out.failure = plugin_inspection_view::problem::cannot_write;
                out.can_install = false;
            }
            for (const host::service_plugin& each :
                 service_->snapshot()->plugins)
            {
                if (each.scope == plug::plugin_scope::for_user &&
                    each.id == found.declared->id)
                {
                    out.failure =
                        plugin_inspection_view::problem::already_installed;
                    out.can_install = false;
                }
            }
            notify_plugin_inspected(number, out);
        });
    return number;
}

void ServicePluginBackend::add_plugin(const QString& folder)
{
    boost::asio::post(
        slow_,
        [this, folder]
        {
            plugin_request_result result;
            result.kind = plugin_request_kind::add_plugin;
            plug::install_options install;
            // The person saw the verdict, which says so for a plugin nobody
            // signed.
            install.allow_unsigned = true;
            install.context = context_now();
            std::string detail;
            const auto done = plug::install_plugin(
                to_path(folder), options_.user_root, install, &detail);
            if (!done.has_value())
            {
                const vss::error_code why = done.error();
                if (why == vss::errc::already_exists)
                    result.error = plugin_request_error::already_installed;
                else if (why == vss::errc::permission_denied ||
                         why == vss::errc::io_error)
                    result.error = plugin_request_error::cannot_write;
                else
                    result.error = plugin_request_error::not_a_plugin;
            }
            else if (done->found.declared.has_value())
            {
                result.plugin_name =
                    QString::fromStdString(done->found.declared->name);
            }
            service_->rescan();
            notify_request_finished(result);
        });
}

void ServicePluginBackend::remove_plugin(const QString& plugin_id,
                                         const QString& hash,
                                         bool /*delete_saved_data*/)
{
    boost::asio::post(slow_,
                      [this, plugin_id, hash]
                      {
                          const auto now = service_->snapshot();
                          const host::service_plugin* plugin =
                              find_plugin(*now, plugin_id, hash);
                          // Only a plugin the user installed is the user's to
                          // remove.
                          if (plugin != nullptr &&
                              plugin->scope == plug::plugin_scope::for_user)
                              static_cast<void>(plug::remove_plugin(
                                  options_.user_root, plugin->id));
                          service_->rescan();
                          plugin_request_result result;
                          result.kind = plugin_request_kind::remove_plugin;
                          result.plugin_id = plugin_id;
                          notify_request_finished(result);
                      });
}

} // namespace vss::gui
