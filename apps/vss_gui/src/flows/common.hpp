//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_FLOWS_COMMON_HPP
#define VSS_GUI_FLOWS_COMMON_HPP

#include "backend/view_types.hpp"

#include <QFont>
#include <QString>

namespace vss::gui
{

/// The platform whose copy variants a dialog should show.  It is a parameter
/// so tests can render the Linux and macOS variants on any host.
enum class platform_kind
{
    windows,
    linux,
    macos
};

platform_kind host_platform();
bool windows_platform(platform_kind platform);
bool pseudo_terminal_platform(platform_kind platform);

QFont flow_semibold(QFont font);
QString bold(const QString& text);

/// The name a person sees for a port: its path when it has one that an
/// outside program can open, otherwise its Studio name.
QString port_display_name(const port_view& port);

/// A failure that is ready for `ErrorDialog`.  The engine text stays in
/// `details`, never in `body`, per the copy deck's error template.
struct flow_error
{
    QString title;
    QString body;
    QString details;
    /// True when the guided "Set up Virtual Serial Studio" fixes the cause, so
    /// the dialog adds that action.
    bool offer_setup = false;
};

/// Why a COM number was refused, in words a person can act on.  `changing` is
/// true for a port that exists and false for one being made.  `number` is the
/// COM number that was asked for.
QString com_number_refusal(const vss::error_code& error, unsigned number,
                           bool changing);

/// The text behind "Copy details": the engine's code, or for a driver set-up
/// problem what the check saw (the piece, the Win32 error, the failed call).
QString error_details(const vss::error_code& error);

/// If `failure` is a driver set-up problem (see `driver_aware_error`), gives
/// `error` the body and the details of that problem and the set-up action; the
/// title stays the caller's own. Returns true when it did.
bool apply_driver_problem(flow_error& error, const vss::error_code& failure);

QString error_title(const QString& verb, const QString& thing);
QString error_body(const vss::error_code& error, const QString& cause,
                   const QString& next_step);

} // namespace vss::gui

#endif // VSS_GUI_FLOWS_COMMON_HPP
