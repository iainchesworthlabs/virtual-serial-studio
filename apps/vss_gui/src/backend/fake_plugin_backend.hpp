//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The in-memory plugin backend: any plugin state can be set, and every
// request is recorded, so a test can see what a dialog sent and what it did
// not. The offscreen pictures of the Plugins page are made from it too.
//

#pragma once

#include "backend/plugin_backend.hpp"

#include <QHash>

#include <functional>
#include <utility>
#include <vector>

namespace vss::gui
{

/// What a request carried, for a test to look at.
struct fake_plugin_request
{
    plugin_request_kind kind = plugin_request_kind::rescan;
    QString plugin_id;
    QString hash;
    QString key_id;
    QStringList granted;
    /// Switch on, the choice, or "also delete saved data".
    bool flag = false;
    /// A key file, or a folder.
    QString path;
    QString fingerprint;
    /// An instance number.
    quint64 number = 0;
    /// The settings an instance was asked with.
    QMap<QString, QString> values;
};

class FakePluginBackend final : public PluginBackend
{
    Q_OBJECT

public:
    explicit FakePluginBackend(QObject* parent = nullptr);

    plugin_snapshot_ptr snapshot() const override { return snapshot_; }

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
    QStringList place_names() const override { return place_names_; }
    void set_place_names(QStringList names) { place_names_ = std::move(names); }
    void remove_instance(const QString& plugin_id, quint64 number) override;
    void answer_secret(quint64 id, bool seen) override;
    void answer_control(quint64 id, bool allowed) override;

    /// A test asks the page's side to show a secret or a control request, as
    /// the service would.
    void ask_secret(const plugin_secret_request& request);
    void ask_control(const plugin_control_request& request);
    /// The answers given so far, as (request number, answer).
    const std::vector<std::pair<quint64, bool>>& secret_answers() const noexcept
    {
        return secret_answers_;
    }
    const std::vector<std::pair<quint64, bool>>&
    control_answers() const noexcept
    {
        return control_answers_;
    }

    // -- what a test sets

    void set_snapshot(plugin_snapshot snapshot);
    void set_plugins(std::vector<plugin_view> plugins);
    /// Changes one plugin by id and announces it. Returns false when there is
    /// no such plugin.
    bool change(const QString& plugin_id,
                const std::function<void(plugin_view&)>& edit);
    /// What `read_key_file(path)` answers.
    void set_key_file(const QString& path, plugin_key_file_view key);
    /// What `inspect_plugin(folder)` answers. A folder nobody set is not a
    /// plugin.
    void set_inspection(const QString& folder, plugin_inspection_view found);
    /// The error the next `add_plugin` fails with.
    void fail_next_add(plugin_request_error error, const QString& plugin_name);

    const std::vector<fake_plugin_request>& requests() const noexcept
    {
        return requests_;
    }
    void clear_requests() { requests_.clear(); }
    /// The folders `inspect_plugin` was asked about.
    const QStringList& inspected() const noexcept { return inspected_; }
    int count_of(plugin_request_kind kind) const;

    /// Delivers what was queued, so a test sees the signals now.
    void flush();

private:
    void record(const fake_plugin_request& request);
    plugin_view* find(const QString& plugin_id);
    void settle(plugin_view& plugin) const;

    std::shared_ptr<plugin_snapshot> snapshot_;
    std::vector<fake_plugin_request> requests_;
    QStringList place_names_;
    std::vector<std::pair<quint64, bool>> secret_answers_;
    std::vector<std::pair<quint64, bool>> control_answers_;
    QHash<QString, plugin_key_file_view> key_files_;
    QHash<QString, plugin_inspection_view> inspections_;
    QStringList inspected_;
    quint64 next_request_ = 1;
    plugin_request_error add_error_ = plugin_request_error::none;
    QString add_error_name_;
};

/// One plugin in each state of the spec's table, with every row of its detail
/// filled in. For tests and for the pictures.
std::vector<plugin_view> fake_plugin_samples();

/// A plugin whose every text is hostile: control characters, direction
/// overrides, markup, a format placeholder and a very long name.
plugin_view fake_hostile_plugin();

} // namespace vss::gui
