//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/studio_backend.hpp"
#include "simple/job_form.hpp"
#include "simple/setup_kinds.hpp"
#include "simple/setup_page.hpp"

#include <QHash>
#include <QPointer>
#include <QWidget>

#include <optional>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace vss::gui::simple
{

class JobGrid;

/// The body of the window in Simple mode: the setups list on the left, and on
/// the right the job grid, a job form or the page of the chosen setup.
class SimplePage final : public QWidget
{
    Q_OBJECT

public:
    explicit SimplePage(StudioBackend& backend, QWidget* parent = nullptr);

    /// Shows "What do you want to do?".
    void show_jobs();
    /// Opens the form of `kind`.
    void open_job(job kind);
    /// Shows the page of the setup that holds `port`.
    void select_setup(vss::endpoint_id port);

    const std::vector<setup_view>& setups() const noexcept { return setups_; }
    JobGrid* jobs() const noexcept { return grid_; }
    JobForm* form() const { return form_; }
    SetupPage* setup_page() const { return page_; }
    QListWidget* list() const noexcept { return list_; }

    /// Re-reads the setups.  Called when the engine's ports or links change.
    void refresh();
    /// Traffic from the engine.
    void add_events(const event_batch& events);

Q_SIGNALS:
    void check_requested();
    void driver_setup_requested();
    void open_requested();
    void save_requested();
    void examples_requested();
    void advanced_requested();
    void troubleshoot_requested();
    /// Remove the setup that holds `port`, after the usual confirmation.
    void remove_setup_requested(vss::endpoint_id port);
    void remove_all_requested();
    /// The setup that holds `port` is the one shown, for the window's
    /// selection; 0 when none is.
    void setup_selected(vss::endpoint_id port);
    void message(const QString& text);

private:
    void build();
    void rebuild_list();
    void show_form(job kind, const std::optional<setup_view>& previous);
    void show_setup(const setup_view& setup);
    /// Takes the setup by value: `refresh()` below rebuilds `setups_`, and a
    /// reference into it would dangle for the rest of the function.
    void change_programs(setup_view setup, vss::endpoint_id close);
    const setup_view* find(vss::endpoint_id port) const;
    setup_prefs prefs_of(const setup_view& setup) const;
    void store_prefs(const setup_view& setup, const setup_prefs& prefs);

    StudioBackend& backend_;
    std::vector<setup_view> setups_;
    QHash<vss::endpoint_id, setup_prefs> prefs_;
    /// The setup shown, by any one of its ports; 0 for the grid or a form.
    vss::endpoint_id shown_ = 0;
    /// Where Cancel on a form goes back to.
    vss::endpoint_id form_return_ = 0;
    bool refresh_queued_ = false;

    QLabel* count_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* remove_all_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    JobGrid* grid_ = nullptr;
    QPointer<JobForm> form_;
    QPointer<SetupPage> page_;
};

} // namespace vss::gui::simple
