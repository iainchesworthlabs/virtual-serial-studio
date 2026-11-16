//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin notices at the top of the main window
// (docs/gui-design/docs/screens/10-plugins.md, "Plugin notices"): one at a
// time, never modal, never taking focus, in the place of the licence notices
// and below them. A notice follows from the host's snapshot changing; the
// first snapshot is the baseline and says nothing.
//

#pragma once

#include "backend/plugin_backend.hpp"
#include "support/plugin_parts.hpp"
#include "widgets/notes.hpp"

#include <deque>
#include <optional>

class QPushButton;

namespace vss::gui
{

class PluginNotice final : public NoteBanner
{
    Q_OBJECT

public:
    explicit PluginNotice(QWidget* parent = nullptr);

    /// Where the snapshots come from. The snapshot at this moment is what
    /// later changes are measured against.
    void set_backend(PluginBackend* host);

    /// The ids of the plugins this setup uses, for "a plugin that is used in
    /// this setup stopped".
    void set_used_plugins(QStringList ids) { used_ = std::move(ids); }

    /// The notice on show, if any.
    const std::optional<support::plugin_notice_text>& shown() const noexcept
    {
        return shown_;
    }
    int waiting() const noexcept { return static_cast<int>(queue_.size()); }

    /// "Start again", for a plugin that stopped; hidden for the others.
    QPushButton* second_button() const noexcept { return second_; }
    QPushButton* dismiss_button() const noexcept { return dismiss_; }

    /// Hides the notice on show and shows the next, if any.
    void dismiss();
    /// Reads the host's snapshot and shows what follows from the change.
    void refresh();

Q_SIGNALS:
    /// The first action of a notice that opens Plugins.
    void plugins_requested();
    /// The first action of a notice about a plugin that must be decided.
    void decision_requested(const QString& plugin_id, const QString& folder);
    /// "Start again".
    void start_again_requested(const QString& plugin_id);

private:
    void show_next();

    PluginBackend* host_ = nullptr;
    plugin_snapshot_ptr before_;
    QStringList used_;
    std::deque<support::plugin_notice_text> queue_;
    std::optional<support::plugin_notice_text> shown_;
    QPushButton* second_ = nullptr;
    QPushButton* dismiss_ = nullptr;
};

} // namespace vss::gui
