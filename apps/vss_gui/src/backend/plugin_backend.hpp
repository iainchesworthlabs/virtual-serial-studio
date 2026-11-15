//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one narrow interface between the Plugins page and the plugin host
// (docs/gui-design/docs/screens/10-plugins.md). Every request returns at
// once: none of them reads a file, hashes, verifies or waits on the GUI
// thread. The answer arrives as a changed snapshot or as `request_finished`,
// both on the GUI thread through queued connections, and a burst of changes
// is coalesced into one signal.
//

#pragma once

#include "backend/plugin_view.hpp"

#include <QObject>

#include <atomic>

namespace vss::gui
{

class PluginBackend : public QObject
{
    Q_OBJECT

public:
    explicit PluginBackend(QObject* parent = nullptr);
    ~PluginBackend() override;

    /// The plugins as they are now. Immutable: a screen keeps the pointer for
    /// as long as it likes. Safe to call from the GUI thread at any time.
    virtual plugin_snapshot_ptr snapshot() const = 0;

    /// Stores the user's decision for the plugin version with `hash` only.
    virtual void decide(const plugin_decision_request& request) = 0;
    /// Switches a plugin on or off. In a build that cannot run plugins this
    /// records the wish and the plugin stays Off.
    virtual void set_enabled(const QString& plugin_id, const QString& hash,
                             bool on) = 0;
    /// Reads a key file the user chose. The answer is `key_file_read` with
    /// the number this returns.
    virtual quint64 read_key_file(const QString& path) = 0;
    /// Approves the key in the file at `path`, when its fingerprint is still
    /// `fingerprint` (the one the user compared). Never switches a plugin on.
    virtual void approve_key(const QString& path,
                             const QString& fingerprint) = 0;
    /// Takes an approved key back; plugins signed with it ask again.
    virtual void remove_key(const QString& key_id) = 0;
    /// Stores, or removes, the choice to run this plugin version without
    /// isolation.
    virtual void set_no_isolation(const QString& plugin_id, const QString& hash,
                                  bool on) = 0;
    virtual void start_again(const QString& plugin_id) = 0;
    /// Runs an expired, unofficial plugin until Studio closes.
    virtual void run_expired(const QString& plugin_id, const QString& hash) = 0;
    virtual void rescan() = 0;
    /// Looks into a plugin folder the user chose and says what it found,
    /// without copying anything. The answer is `plugin_inspected` with the
    /// number this returns.
    virtual quint64 inspect_plugin(const QString& folder) = 0;
    /// Copies a plugin's folder into the user's plugin folder, for this user
    /// only. The page asks for it after the user saw the inspection.
    virtual void add_plugin(const QString& folder) = 0;
    /// Moves a plugin's folder to the bin.
    virtual void remove_plugin(const QString& plugin_id, const QString& hash,
                               bool delete_saved_data) = 0;

Q_SIGNALS:
    /// The snapshot changed. Coalesced; on the GUI thread.
    void snapshot_changed();
    void request_finished(const vss::gui::plugin_request_result& result);
    void key_file_read(quint64 request,
                       const vss::gui::plugin_key_file_view& key);
    void plugin_inspected(quint64 request,
                          const vss::gui::plugin_inspection_view& found);

protected:
    /// Safe from any thread: asks the GUI thread to emit `snapshot_changed`
    /// once, however often this is called before it does.
    void notify_snapshot_changed();
    /// Safe from any thread.
    void notify_request_finished(const plugin_request_result& result);
    void notify_key_file_read(quint64 request, const plugin_key_file_view& key);
    void notify_plugin_inspected(quint64 request,
                                 const plugin_inspection_view& found);

private:
    std::atomic<bool> snapshot_pending_{false};
};

} // namespace vss::gui
