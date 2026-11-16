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

#include "backend/licence_view.hpp"
#include "backend/plugin_backend.hpp"
#include "backend/service_mapping.hpp"

#include <vss/core/bytes.hpp>
#include <vss/core/hub.hpp>
#include <vss/license/license.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/host/capture_point.hpp>
#include <vss/plugin/host/control_point.hpp>
#include <vss/plugin/host/generator_point.hpp>
#include <vss/plugin/host/licence_point.hpp>
#include <vss/plugin/host/secret_point.hpp>
#include <vss/plugin/host/service.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
    /// Studio's hub, for the plugins that record. Null: no plugin can record.
    /// It must outlive the backend.
    vss::hub* hub = nullptr;
    /// Where the instances the user asked of each plugin are kept, so that
    /// they come back at the next start. Empty: they are not kept.
    std::filesystem::path instances_file;
    /// The folders the licence files are in, so that the file Studio uses
    /// can be handed to a plugin that checks the licence itself.
    std::filesystem::path licence_user_directory;
    std::filesystem::path licence_machine_directory;
};

/// The licence file Studio is using, by name and folder.
struct licence_file_ref
{
    std::string file_name;
    bool machine_wide = false;
};

/// The places a plugin may record, by the name it uses for them: a port by
/// its name, a link by the names of its ends. Immutable once published.
using plugin_place_table =
    std::map<std::string, vss::plugin::host::capture_place, std::less<>>;

/// The ports a plugin may write to, by the name it uses for them. Immutable
/// once published.
using plugin_endpoint_table =
    std::map<std::string, std::shared_ptr<vss::endpoint>, std::less<>>;

/// The licence file in use in what the backend knows of the licence, or
/// nothing for the Community edition.
std::optional<licence_file_ref> licence_file_in_use(const licence_view& view);

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
    /// `file` is the file in use, or nothing for the Community edition.
    /// When the licence is a different one, the official plugins that run
    /// are started again, so that they ask for the new file.
    void set_licence(const vss::license::status& status,
                     const std::optional<licence_file_ref>& file = {});

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
    void add_instance(const QString& plugin_id, const QString& point_id,
                      const QMap<QString, QString>& values) override;
    QStringList place_names() const override;
    void remove_instance(const QString& plugin_id, quint64 number) override;
    void answer_secret(quint64 id, bool seen) override;
    void answer_control(quint64 id, bool allowed) override;

    /// Publishes the places a plugin may record. Call it from the GUI thread
    /// whenever the ports or links change; the service reads the table it
    /// last got, never a GUI object.
    void set_places(std::shared_ptr<const plugin_place_table> places);
    /// Publishes the ports a plugin may write to, the same way.
    void set_writable_ports(std::shared_ptr<const plugin_endpoint_table> ports);

private:
    void after_change();
    void refresh_extras();
    void delete_data_of_gone_plugins();
    void process_removals();
    std::vector<std::unique_ptr<vss::plugin::host::extension_handler>>
    make_handlers(const vss::plugin::host::extension_context& context);
    void ask_secret(const vss::plugin::host::secret_to_show& secret,
                    std::function<void(bool)> done);
    void ask_control(const vss::plugin::host::control_approval_request& request,
                     std::function<void(bool)> done);
    void answer_all_prompts_no();
    void load_instances();
    void restart_waiting_plugins();
    void save_instances() const;
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
    struct pending_removal
    {
        std::string id;
        bool delete_data = false;
        int attempts = 0;
    };
    /// Plugins switched off to be removed once they have stopped.
    std::vector<pending_removal> removals_;
    std::shared_ptr<const plugin_place_table> places_;
    std::shared_ptr<const plugin_endpoint_table> writable_;
    /// The answers owed for the dialogs asked for and not yet answered.
    std::map<quint64, std::function<void(bool)>> secret_done_;
    std::map<quint64, std::function<void(bool)>> control_done_;
    std::atomic<quint64> next_prompt_{1};
    /// What the user asked of each plugin, by plugin id. Read and changed on
    /// the slow thread only, after the constructor.
    struct kept_instances
    {
        std::uint64_t next = 1;
        std::vector<vss::plugin::host::instance_spec> list;
    };
    std::map<std::string, kept_instances> instances_;
    /// The bytes of the licence file in use, read on the slow thread; empty
    /// for none. Swapped whole, so the service's strand copies a snapshot.
    std::shared_ptr<const vss::byte_buffer> licence_bytes_;
    /// Which licence the plugins were last told of, to start them again once
    /// for each change.
    std::string licence_key_;
    bool licence_key_known_ = false;
    /// Official plugins stopped for a licence change and not yet started.
    std::set<std::string> restart_;
    /// Plugins taken away whose saved data is to go once they have stopped.
    std::vector<std::string> data_to_delete_;
    mutable plugin_snapshot_ptr cached_;
    mutable std::uint64_t cached_revision_ = 0;
    mutable quint64 cached_extras_ = ~quint64{0};
};

} // namespace vss::gui
