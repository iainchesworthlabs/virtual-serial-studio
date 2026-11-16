//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Plugins page (docs/gui-design/docs/screens/10-plugins.md): the list of
// installed plugins with their state, and for the selected one its detail,
// what it may do, where it is used and its recent messages. It shows what
// `PluginBackend::snapshot()` reports and never reads a plugin's files,
// checks a signature or decides a state. Nothing here waits for the host: a
// request returns at once and its answer arrives as a changed snapshot.
//

#pragma once

#include "backend/plugin_backend.hpp"
#include "support/plugin_text.hpp"
#include "widgets/chips.hpp"
#include "widgets/notes.hpp"

#include <QDialog>
#include <QPointer>
#include <QString>

#include <functional>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QTableWidget;
class QTimer;
class QKeyEvent;
class QVBoxLayout;
class QWidget;

namespace vss::gui
{
class CopyButton;
class Switch;
} // namespace vss::gui

namespace vss::gui::support
{

class PluginsDialog final : public QDialog
{
    Q_OBJECT

public:
    /// Chooses a folder (a plugin's folder) or a file (a key file) and returns
    /// its path; empty when the person chose none. The default opens the
    /// native dialog.
    using path_picker = std::function<QString()>;
    /// Opens a folder in the file manager.
    using path_opener = std::function<void(const QString&)>;

    explicit PluginsDialog(PluginBackend& backend, QWidget* parent = nullptr);

    void set_folder_picker(path_picker picker)
    {
        folder_picker_ = std::move(picker);
    }
    void set_key_picker(path_picker picker) { key_picker_ = std::move(picker); }
    void set_path_opener(path_opener opener) { opener_ = std::move(opener); }

    /// Selects the row of the plugin with this id (and folder, to tell two
    /// copies apart). Returns false when there is none.
    bool select(const QString& plugin_id, const QString& folder = {});
    /// The plugin the detail shows, or null.
    const plugin_view* selected() const;

    // -- the actions of the page, as the buttons call them
    void add_plugin();
    void import_key();
    void open_plugin_folder();
    void check_again();
    /// Opens the decision dialog for `plugin`.
    void review(const plugin_view& plugin, bool permissions_step = false);
    void run_without_isolation(const plugin_view& plugin);
    void remove_selected();
    void remove_key(const plugin_view& plugin);
    void copy_details();

    // -- for tests
    QListWidget* list() const noexcept { return list_; }
    QLineEdit* filter() const noexcept { return filter_; }
    StatusChip* header_chip() const noexcept { return header_chip_; }
    NoteBanner* top_banner() const noexcept { return top_note_; }
    /// A message about the last add or key import that failed.
    NoteBanner* message_banner() const noexcept { return message_note_; }
    QWidget* detail() const noexcept { return detail_; }
    QPushButton* close_button() const noexcept { return close_; }
    QPushButton* remove_button() const noexcept { return remove_; }
    QPushButton* copy_details_button() const noexcept { return copy_; }
    QPushButton* add_button() const noexcept { return add_; }
    QPushButton* import_key_button() const noexcept { return import_; }
    QPushButton* folder_button() const noexcept { return folder_; }
    QPushButton* check_button() const noexcept { return check_; }
    /// The Switch of the row of this plugin, or null.
    Switch* row_switch(const QString& plugin_id,
                       const QString& folder = {}) const;
    /// The rows of the list in the order they are shown, as "<id>|<folder>".
    QStringList row_keys() const;
    /// The text the footer's "Copy details" would copy.
    QString details_text() const;

protected:
    /// F6 moves between the list and the detail.
    void keyPressEvent(QKeyEvent* event) override;

Q_SIGNALS:
    /// "Licence" was pressed.
    void licence_requested();
    /// "Show" on a use of the plugin: selects it on the canvas.
    void show_on_canvas(const QString& target_id);
    /// A line for the status bar, such as "<plugin name> is on.".
    void status_message(const QString& text);

private:
    class Row;

    void build();
    void on_snapshot_changed();
    void on_request_finished(const plugin_request_result& result);
    void on_key_file_read(quint64 request, const plugin_key_file_view& key);
    void on_plugin_inspected(quint64 request,
                             const plugin_inspection_view& found);
    void refresh_all();
    void refresh_header(const plugin_snapshot& snapshot);
    void refresh_list(const plugin_snapshot& snapshot);
    void refresh_detail();
    void apply_filter();
    void refresh_row_sizes();
    void on_current_row_changed();
    void on_switch_clicked(const QString& plugin_id, const QString& folder,
                           bool on);
    void do_action(plugin_action action, const plugin_view& plugin);
    void show_message(NoteBanner::kind kind, const QString& text);
    void clear_message();
    void move_focus_between_panes();
    QString key_of(const plugin_view& plugin) const;
    static QString detail_signature(const plugin_view& plugin,
                                    const plugin_snapshot& snapshot);

    PluginBackend& backend_;
    plugin_snapshot_ptr snapshot_;
    path_picker folder_picker_;
    path_picker key_picker_;
    path_opener opener_;

    QString selected_key_;
    QString shown_detail_signature_;
    QString announced_state_key_;
    std::vector<QString> before_add_;
    bool waiting_for_add_ = false;
    void add_instance_of(const plugin_view& plugin,
                         const plugin_addition& point);
    void refresh_found();

    /// The table of what one decoder instance found, and what it showed last.
    struct found_view
    {
        QString plugin_id;
        quint64 number = 0;
        QPointer<QTableWidget> table;
        QPointer<QLabel> counts;
        QPointer<QLabel> empty;
        quint64 revision = ~quint64{0};
    };
    std::vector<found_view> found_views_;
    QTimer* found_timer_ = nullptr;

    bool rebuilding_ = false;
    quint64 pending_key_request_ = 0;
    quint64 pending_inspection_ = 0;

    StatusChip* header_chip_ = nullptr;
    NoteBanner* top_note_ = nullptr;
    NoteBanner* message_note_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QListWidget* list_ = nullptr;
    QWidget* empty_ = nullptr;
    QLabel* empty_title_ = nullptr;
    QLabel* empty_text_ = nullptr;
    QPushButton* empty_button_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* import_ = nullptr;
    QPushButton* folder_ = nullptr;
    QPushButton* check_ = nullptr;
    QLabel* machine_line_ = nullptr;
    CopyButton* machine_copy_ = nullptr;
    QScrollArea* detail_scroll_ = nullptr;
    QWidget* detail_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* close_ = nullptr;
};

} // namespace vss::gui::support
