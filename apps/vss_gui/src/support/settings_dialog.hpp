//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_SETTINGS_DIALOG_HPP
#define VSS_GUI_SUPPORT_SETTINGS_DIALOG_HPP

#include "backend/studio_backend.hpp"
#include "support/settings.hpp"
#include "widgets/controls.hpp"

#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QRadioButton;
class QSlider;
class QStackedWidget;

namespace vss::gui::updates
{
class UpdateService;
} // namespace vss::gui::updates

namespace vss::gui::support
{

/// Settings: General, Appearance and Performance.  Every edit applies at once
/// and is written to QSettings, so there is no OK or Apply button.
class SettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    SettingsDialog(StudioBackend& backend, const ui_settings& current,
                   QWidget* parent = nullptr);
    /// With `updates`, the General page ends with the Updates group. Null
    /// means no such group. `updates` outlives the dialog.
    SettingsDialog(StudioBackend& backend, const ui_settings& current,
                   updates::UpdateService* updates, QWidget* parent = nullptr);

    const ui_settings& settings() const noexcept { return settings_; }

    /// Restores every preference to its default and applies it immediately.
    /// The update settings are not preferences of this dialog: they are the
    /// service's, and an answer given to the question of the first start
    /// must not be undone here, so they stay as they are.
    void reset_to_defaults();

    /// Opens on a page: 0 General, 1 Running, 2 Appearance, 3 Performance.
    void show_page(int index);

Q_SIGNALS:
    /// The shell owns the LevelService; advanced mode is applied by its owner.
    void advanced_changed(bool advanced);
    void settings_changed(const ui_settings& settings);

private:
    void build();
    void select_page(int index);
    void apply_theme();
    void apply_density();
    void apply_refresh();
    void sync_profiling();
    void sync_render_status();
    /// Reads the update service again: the check boxes and the line that
    /// says why they are disabled.
    void sync_updates();
    void persist();

    StudioBackend& backend_;
    ui_settings settings_;

    Switch* advanced_ = nullptr;
    QComboBox* baud_ = nullptr;
    QCheckBox* ask_on_discard_ = nullptr;
    QRadioButton* theme_system_ = nullptr;
    QRadioButton* theme_light_ = nullptr;
    QRadioButton* theme_dark_ = nullptr;
    SegmentedControl* density_ = nullptr;
    SegmentedControl* link_style_ = nullptr;
    QRadioButton* close_background_ = nullptr;
    QRadioButton* close_quit_ = nullptr;
    Switch* sign_in_ = nullptr;
    QSlider* refresh_ = nullptr;
    QLabel* refresh_value_ = nullptr;
    SegmentedControl* render_ = nullptr;
    QLabel* render_status_ = nullptr;
    bool render_changed_ = false;
    Switch* profiling_ = nullptr;
    Switch* profiling_remote_ = nullptr;
    QLabel* profiling_status_ = nullptr;
    updates::UpdateService* updates_ = nullptr;
    QCheckBox* update_automatic_ = nullptr;
    QCheckBox* update_prerelease_ = nullptr;
    QLabel* update_note_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QButtonGroup* nav_ = nullptr;
};

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_SETTINGS_DIALOG_HPP
