//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_SUPPORT_SETUP_FACTS_HPP
#define VSS_GUI_SUPPORT_SETUP_FACTS_HPP

#include "backend/studio_backend.hpp"

#include <vss/ports/permissions.hpp>

#include <QString>
#include <QStringList>
#include <QVector>

#include <chrono>
#include <vector>

namespace vss::gui::support
{

/// Facts the GUI layer can prove about the host, independently of the engine.
///
/// The engine reports com0com and the device list (no elevation needed).  The
/// platform helpers here answer elevation, pseudo terminals and dialout group
/// membership only where the platform can actually be asked; everywhere else
/// `*_known` stays false and the UI says "Not checked yet".
struct platform_facts
{
    bool elevation_known = false;
    bool elevated = false;
    bool pty_known = false;
    bool pty_available = false;
    bool dialout_known = false;
    bool dialout_member = false;
    /// What the operating system may stop Studio doing: elevation, the
    /// firewall, and on other systems the equivalents (see
    /// `vss/ports/permissions.hpp`). Empty until asked.
    std::vector<vss::permissions::fact> permissions;
};

/// "windows", "linux" or "macos".
QString host_platform();

/// Probes the facts this platform can answer.
platform_facts probe_platform_facts();

enum class setup_state
{
    ok,
    info,
    attention,
    not_checked
};

enum class setup_action
{
    none,
    install_driver,
    /// Open "Set up Virtual Serial Studio": install or repair the driver and
    /// the helper service in one elevated step.
    setup_driver,
    restart_elevated,
    refresh_devices,
    continue_test,
    dialout_help,
    /// Explain the firewall fact and, where this build can, offer the one rule.
    firewall_help,
    /// Explain a refused serial device and, where this build can, offer the
    /// group.
    serial_access_help,
    /// Explain AppArmor, SELinux, Flatpak or Snap and show the exact fix.
    confinement_help,
    /// Show where the Local Network permission is (macOS).
    network_privacy_help
};

struct setup_row
{
    setup_state state = setup_state::not_checked;
    /// Icon name from `IconProvider`.
    QString icon;
    QString title;
    QString sentence;
    /// Empty for a row that has no chip (Carry on without a driver).
    QString chip;
    QString action_text;
    setup_action action = setup_action::none;
    /// True for the row that needs the user's attention now.
    bool highlight = false;
};

struct setup_report
{
    enum class readiness
    {
        ready,
        needs_attention,
        not_checked
    };

    readiness state = readiness::not_checked;
    /// True when other programs can open newly created virtual ports.
    bool ports_ready = false;
    /// The piece of the Windows driver set-up that is wrong, if the guided
    /// set-up can fix it; `none` otherwise and on other systems.
    driver_problem problem = driver_problem::none;
    /// "All set", "1 thing to fix" or "Not checked yet".
    QString header_chip;
    /// The warning above the rows; empty when nothing is wrong.
    QString driver_note;
    QVector<setup_row> rows;
    /// The muted sentence under the rows.
    QString platform_note;
};

/// Builds the report from engine facts and platform probes.  Pure, so every
/// degradation is testable without a real machine state.
setup_report build_setup_report(const setup_facts& facts,
                                const platform_facts& platform,
                                const QStringList& device_paths = {},
                                bool checks_available = true);

/// Reads the engine facts and probes the platform, then builds the report.
setup_report query_setup_report(const StudioBackend& backend);

/// Keeps the last `query_setup_report` for the surfaces that redraw with every
/// engine event (the status bar, the Simple page, the launcher).  The query
/// asks the operating system and takes tens of milliseconds, which on the GUI
/// thread for every event is a frozen window.  The report is reused while it
/// is younger than `StudioBackend::probe_cache_age()` and the ports have not
/// changed; a backend that answers zero is asked every time.  The setup
/// check window asks for a fresh report and does not use this.
class setup_report_cache
{
public:
    const setup_report& get(const StudioBackend& backend);
    void invalidate() noexcept { valid_ = false; }

private:
    setup_report report_;
    QString ports_key_;
    std::chrono::steady_clock::time_point taken_;
    bool valid_ = false;
};

/// The official com0com project page, the one place the instructions URL is
/// named.  The GUI never downloads anything itself.
QString com0com_project_url();
bool open_com0com_project_page();

/// The shell command shown by "How to add me"; it is copied, never run.
QString dialout_add_command();
bool open_system_accessibility_settings();

/// Relaunches the current executable elevated.  Windows only; returns false
/// with `error` set otherwise or when the user cancels the UAC prompt.
bool relaunch_as_administrator(QString* error = nullptr);

} // namespace vss::gui::support

#endif // VSS_GUI_SUPPORT_SETUP_FACTS_HPP
