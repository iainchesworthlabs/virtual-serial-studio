//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/common.hpp"

#include "backend/driver_error.hpp"

#include <vss/ports/permissions.hpp>

#include <QObject>

namespace vss::gui
{
namespace
{

bool is_studio_only_path(const QString& path)
{
    return path.startsWith(QStringLiteral("loopback:")) || path.isEmpty();
}

} // namespace

platform_kind host_platform()
{
#if defined(Q_OS_WIN)
    return platform_kind::windows;
#elif defined(Q_OS_MACOS)
    return platform_kind::macos;
#else
    return platform_kind::linux;
#endif
}

bool windows_platform(platform_kind platform)
{
    return platform == platform_kind::windows;
}

bool pseudo_terminal_platform(platform_kind platform)
{
    return platform == platform_kind::linux || platform == platform_kind::macos;
}

QFont flow_semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

QString bold(const QString& text)
{
    return QStringLiteral("<b>%1</b>").arg(text.toHtmlEscaped());
}

QString port_display_name(const port_view& port)
{
    if (!port.studio_only && !is_studio_only_path(port.path))
        return port.path;
    return port.name;
}

QString com_number_refusal(const vss::error_code& error, unsigned number,
                           bool changing)
{
    if (const auto problem = driver_problem_of(error))
        return driver_problem_body(*problem);
    if (error == vss::errc::already_exists)
    {
        return QObject::tr("COM%1 already belongs to another device on this "
                           "PC, such as a real port, com0com or an earlier "
                           "port that Windows remembers. Nothing was "
                           "changed. Choose a different number.")
            .arg(number);
    }
    if (error == vss::errc::invalid_argument)
        return QObject::tr("Choose a number from 1 to 255.");
    if (error == vss::errc::unsupported)
    {
        return QObject::tr("Choosing a COM number needs the Virtual Serial "
                           "Studio helper service, and it is not installed "
                           "or is too old. Nothing was changed. Install the "
                           "helper from the driver package, or leave the "
                           "number on Automatic.");
    }
    if (error == vss::errc::device_busy)
    {
        if (changing)
        {
            return QObject::tr("A program has this port open, and Windows "
                               "restarts the port to change its number. "
                               "Nothing was changed. Close the program, or "
                               "use Release port in Advanced mode, then try "
                               "again.");
        }
        return QObject::tr("Every port of the driver is in use. Nothing was "
                           "changed. Remove a port you no longer need and "
                           "try again.");
    }
    if (error == vss::errc::port_exhausted)
    {
        return QObject::tr("No more ports can be made: one user can hold 16 "
                           "driver ports and the driver 32 in all. Nothing "
                           "was changed. Remove a port you no longer need "
                           "and try again.");
    }
    if (error == vss::errc::timeout)
    {
        return QObject::tr("The port did not appear in time. Nothing was "
                           "changed. Try again.");
    }
    if (error == vss::errc::permission_denied)
    {
        return QObject::tr("Windows did not allow this change. Nothing was "
                           "changed. Open Tools, Setup check to see what is "
                           "missing, or run Virtual Serial Studio as "
                           "administrator and try again.");
    }
    return QObject::tr("The COM number could not be set. Nothing was "
                       "changed. Open Tools, Setup check to see whether the "
                       "driver and its helper service are healthy.");
}

QString error_title(const QString& verb, const QString& thing)
{
    return QObject::tr("Could not %1 %2").arg(verb, thing);
}

QString error_details(const vss::error_code& error)
{
    if (driver_problem_of(error))
        return last_driver_details();
    return error_text(error);
}

bool apply_driver_problem(flow_error& error, const vss::error_code& failure)
{
    const auto problem = driver_problem_of(failure);
    if (!problem)
        return false;
    // The title stays the dialog's own ("Could not create the port"): the
    // copy deck's error template. The body says which piece is wrong.
    error.body = driver_problem_body(*problem);
    error.details = last_driver_details();
    error.offer_setup = driver_problem_offers_setup(*problem);
    return true;
}

QString error_body(const vss::error_code& error, const QString& cause,
                   const QString& next_step)
{
    if (const auto problem = driver_problem_of(error))
        return driver_problem_body(*problem);
    if (error == vss::errc::permission_denied)
    {
        if (vss::permissions::host_name() != "windows")
        {
            return QObject::tr(
                "The system did not allow this. Nothing was changed. Open "
                "Tools, Setup check: it says whether your account is missing "
                "the serial device group or AppArmor, SELinux or a sandbox is "
                "in the way, and shows the command to fix it.");
        }
        return QObject::tr("Windows did not allow this change. Nothing was "
                           "changed. Open Tools, Setup check to see what is "
                           "missing, or run Virtual Serial Studio as "
                           "administrator and try again.");
    }
    if (error == vss::errc::device_busy)
    {
        return QObject::tr("Another program may be using the device. Nothing "
                           "was changed. Close that program and try again.");
    }
    return QStringLiteral("%1 %2").arg(cause, next_step);
}

} // namespace vss::gui
