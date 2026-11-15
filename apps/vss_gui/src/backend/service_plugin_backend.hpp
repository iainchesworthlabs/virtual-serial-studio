//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin backend of the real program: a thin adapter between the Plugins
// page and `vss::plugin::host::plugin_service`. The service finds, judges,
// starts and stops plugins on an `io_context` of its own, one thread; its slow
// reading of folders and this adapter's file work run on a pool of one
// thread; neither is the GUI thread. A change in the service is posted to the
// GUI thread as one coalesced signal, and the page reads an immutable
// snapshot. Every request returns at once.
//

#pragma once

#include "backend/plugin_backend.hpp"
#include "backend/service_mapping.hpp"

#include <vss/license/license.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/host/service.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vss::gui
{

struct service_plugin_options
{
    /// The plugin folders, widest scope first.
    std::vector<vss::plugin::plugin_root> roots;
    /// The folder of Studio's programs, for the kind of install.
    std::filesystem::path program_directory;
    /// The administrator's policy file. Empty: there is none.
    std::filesystem::path policy_file;
    /// The user's decisions and approved keys.
    std::filesystem::path trust_file;
    /// Where plugins keep their data: `<root>/<plugin id>/data`.
    std::filesystem::path data_root;
    /// The user's own plugin folder, where "Add a plugin" copies to. Empty:
    /// the user has none.
    std::filesystem::path user_root;
    QString user_folder_text;
    QString machine_folder_text;
    std::string studio_version;
    /// Seeds the waits between starts of a plugin that keeps failing.
    std::uint64_t seed = 0;
};

/// The folders and files of this computer, for the program.
service_plugin_options default_service_plugin_options();

class ServicePluginBackend final : public PluginBackend
{
    Q_OBJECT

public:
    explicit ServicePluginBackend(service_plugin_options options,
                                  QObject* parent = nullptr);
    ~ServicePluginBackend() override;

    plugin_snapshot_ptr snapshot() const override;

    /// The licence in use. Call it again when the licence changes: the
    /// plugins are judged again.
    void set_licence(const vss::license::status& status);

    /// Stops every plugin, each asked first, waits for them to have gone for
    /// at most seven seconds (the service ends what is left after five), and
    /// stops the threads. Call it when the window is gone and the program is
    /// about to end; it is called again, harmlessly, by the destructor.
    void shut_down();

    void decide(const plugin_decision_request& request) override;
    void set_enabled(const QString& plugin_id, const QString& hash,
                     bool on) override;
    quint64 read_key_file(const QString& path) override;
    void approve_key(const QString& path, const QString& fingerprint) override;
    void remove_key(const QString& key_id) override;
    void set_no_isolation(const QString& plugin_id, const QString& hash,
                          bool on) override;
    void start_again(const QString& plugin_id) override;
    void rescan() override;
    quint64 inspect_plugin(const QString& folder) override;
    void add_plugin(const QString& folder) override;
    void remove_plugin(const QString& plugin_id, const QString& hash,
                       bool delete_saved_data) override;

private:
    void after_change();
    void refresh_extras();
    vss::plugin::inspect_context context_now() const;

    service_plugin_options options_;
    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        guard_;
    std::thread io_thread_;
    boost::asio::thread_pool slow_{1};
    std::shared_ptr<vss::plugin::host::plugin_service> service_;
    std::atomic<bool> down_{false};
    std::atomic<bool> rescan_asked_{false};
    std::atomic<quint64> next_request_{1};

    mutable std::mutex mutex_;
    vss::license::status licence_ = vss::license::no_licence();
    std::shared_ptr<const service_mapping_inputs> extras_;
    quint64 extras_version_ = 0;
    mutable plugin_snapshot_ptr cached_;
    mutable std::uint64_t cached_revision_ = 0;
    mutable quint64 cached_extras_ = ~quint64{0};
};

} // namespace vss::gui
