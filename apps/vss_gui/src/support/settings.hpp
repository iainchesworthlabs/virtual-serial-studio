//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_SETTINGS_HPP
#define VSS_GUI_SUPPORT_SETTINGS_HPP

#include "backend/view_types.hpp"
#include "theme/theme.hpp"

#include <QString>
#include <QStringList>

namespace vss::gui::support
{

/// The persisted user preferences from `03-information-architecture.md`.
///
/// Changes apply immediately: the Settings dialog writes this value to
/// QSettings and applies the live effect after every edit, so there is no OK
/// or Apply button.
struct ui_settings
{
    bool advanced = false;
    theme_mode theme = theme_mode::system;
    density_mode density = density_mode::comfortable;
    /// How the canvas draws links: smooth curves, or right angles with
    /// rounded corners.
    link_style links = link_style::curved;
    int refresh_ms = 250;
    int default_baud = 115200;
    bool ask_on_discard = true;
    /// Closing the window with ports running keeps them running in the
    /// background (`ui/closeAction` is `background`), or stops them and
    /// quits (`quit`).
    bool keep_running_on_close = true;
    /// Record a performance trace from the start (`ui/profileOnStart`).
    /// Recording listens on this computer only; see `docs/profiling.md`.
    bool profile_on_start = false;
};

inline constexpr int kRefreshMinMs = 50;
inline constexpr int kRefreshMaxMs = 5000;
inline constexpr int kRefreshDefaultMs = 250;
inline constexpr int kDefaultBaudRate = 115200;

ui_settings default_ui_settings();
ui_settings load_ui_settings();
void save_ui_settings(const ui_settings& settings);

/// Keys used in QSettings; exposed so a later work package reads the same
/// values through one place rather than re-typing string literals.
QString theme_key(theme_mode mode);
theme_mode theme_from_key(const QString& key);
QString density_key(density_mode mode);
density_mode density_from_key(const QString& key);
QString link_style_key(link_style style);
link_style link_style_from_key(const QString& key);

/// The baud rates offered in the Settings combo, most likely first.
QStringList baud_rate_choices();

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_SETTINGS_HPP
