//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin backend of the real program. A worker thread of its own uses
// `libs/vss_plugin` to find the installed plugins, read their manifests, check
// their signatures, read the policy and the trust store, ask `verify()` and
// the licence check, and store the user's decisions and approved keys. The
// GUI thread only posts requests and reads an immutable snapshot, so it never
// waits for a file, a hash or a signature.
//
// It starts no plugin and cannot: no sandbox exists yet. Every plugin that
// would be allowed to run is therefore reported as Off, and
// `plugin_snapshot::can_run_plugins` says why.
//

#pragma once

#include "backend/plugin_backend.hpp"
#include "backend/plugin_mapping.hpp"

#include <QThread>

#include <memory>
#include <mutex>

namespace vss::gui
{

/// The folders and files of this computer: Studio's own plugin folder, the
/// machine's and the user's, the policy file, and the trust store.
plugin_gather_options default_plugin_gather_options();

class LibraryPluginBackend final : public PluginBackend
{
    Q_OBJECT

public:
    explicit LibraryPluginBackend(plugin_gather_options options,
                                  QObject* parent = nullptr);
    ~LibraryPluginBackend() override;

    plugin_snapshot_ptr snapshot() const override;

    /// The licence in use, so an official plugin can say whether it is
    /// licensed. Call it again when the licence changes.
    void set_licence(const vss::license::status& status);

    void decide(const plugin_decision_request& request) override;
    void set_enabled(const QString& plugin_id, const QString& hash,
                     bool on) override;
    quint64 read_key_file(const QString& path) override;
    void approve_key(const QString& path, const QString& fingerprint) override;
    void remove_key(const QString& key_id) override;
    void set_no_isolation(const QString& plugin_id, const QString& hash,
                          bool on) override;
    void start_again(const QString& plugin_id) override;
    void run_expired(const QString& plugin_id, const QString& hash) override;
    void rescan() override;
    void add_plugin(const QString& folder) override;
    void remove_plugin(const QString& plugin_id, const QString& hash,
                       bool delete_saved_data) override;

private:
    class Worker;

    void publish(plugin_snapshot_ptr snapshot);

    QThread thread_;
    Worker* worker_ = nullptr;
    mutable std::mutex mutex_;
    plugin_snapshot_ptr snapshot_;
    std::atomic<quint64> next_request_{1};
};

} // namespace vss::gui
