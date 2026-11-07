//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/ports/permissions.hpp>

#include <QString>

namespace vss::gui::support
{

/// What came of a step that needed administrator rights.
struct step_report
{
    vss::permissions::step_status status =
        vss::permissions::step_status::failed;
    /// A sentence for the user, translated.
    QString message;

    bool done() const noexcept
    {
        return status == vss::permissions::step_status::done;
    }
};

/// The command-line helper, which sits beside this program and is the only
/// thing started elevated.  Empty when it is not there.
QString helper_path();

/// True when this build can make the firewall rule: the platform has the step
/// and the helper is in place.  The Network endpoint dialog and the Setup check
/// show "Not available in this build" otherwise.
bool firewall_step_available();

/// Starts the helper through the operating system's own elevation prompt to add
/// the one rule of `spec`, and waits.  Call it only after the user has seen the
/// rule and agreed; nothing here asks again.
step_report
allow_through_firewall(const vss::permissions::firewall_rule_spec& spec);

/// The same, to remove the rule again.
step_report
remove_firewall_rule(const vss::permissions::firewall_rule_spec& spec);

/// True when this build can put the user in a serial device group: Linux with
/// polkit's pkexec installed.
bool group_step_available();

/// Asks polkit, through the desktop's own authentication prompt, to put the
/// user in the group.  Call it only after the user has seen the command and
/// agreed. The group takes effect at the next login; the message says so.
step_report
add_to_serial_group(const vss::permissions::group_membership_spec& spec);

// -- Set up Virtual Serial Studio (Windows) ----------------------------------

/// How the guided set-up ended.
enum class driver_setup_result
{
    /// The driver and the helper service are installed and running.
    done,
    /// Installed; Windows asks for a restart before the driver is used.
    done_restart,
    /// The user declined the Windows permission prompt.
    declined,
    /// A file is unsigned or not trusted on this PC; nothing was changed.
    untrusted,
    /// The driver package could not be installed.
    driver_failed,
    /// The driver is in, and the helper service could not be installed or
    /// started.
    helper_failed,
    /// The folder of driver files is missing something.
    incomplete,
    /// The elevated script did not get administrator rights.
    not_elevated,
    /// Anything else.
    failed,
    /// This build has no set-up to run: not Windows, or no driver files.
    unavailable
};

struct driver_setup_report
{
    driver_setup_result result = driver_setup_result::unavailable;
    /// A sentence for the user with the next step, translated.
    QString message;
    /// The exit code of the script, or -1.
    int exit_code = -1;

    bool installed() const noexcept
    {
        return result == driver_setup_result::done ||
               result == driver_setup_result::done_restart;
    }
};

/// The folder that holds `setup-driver.ps1` and the driver files: the folder
/// named by `VSS_DRIVER_PACKAGE`, or `driver` beside this program. Empty when
/// there is none.
QString driver_package_dir();

/// True when this build can run the set-up: Windows, a permission prompt can be
/// shown, and the driver files are in place.
bool driver_setup_available();

/// The command that does the same as the button, for a person who prefers a
/// terminal opened as administrator. Empty when there are no driver files.
QString driver_setup_command();

/// Maps what the elevated script returned. Pure.
driver_setup_report
report_for_driver_setup(const vss::permissions::step_result& result);

/// Starts the set-up through the operating system's permission prompt and waits
/// for it (minutes, not seconds), so call it off the GUI thread. Call it only
/// after the user has seen what it does and agreed.
driver_setup_report set_up_driver();

} // namespace vss::gui::support
