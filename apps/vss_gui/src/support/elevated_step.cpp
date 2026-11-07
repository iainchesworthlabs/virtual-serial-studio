//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/elevated_step.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

#include <string>
#include <vector>

namespace vss::gui::support
{
namespace
{

namespace perm = vss::permissions;

QString report_text(perm::step_status status, bool remove)
{
    switch (status)
    {
    case perm::step_status::done:
        return remove ? QCoreApplication::translate(
                            "SetupCheck", "The firewall rule was removed.")
                      : QCoreApplication::translate(
                            "SetupCheck", "The firewall rule was added.");
    case perm::step_status::declined:
        return QCoreApplication::translate(
            "SetupCheck",
            "You declined the Windows permission prompt, so nothing was "
            "changed.");
    case perm::step_status::unsupported:
        return QCoreApplication::translate("SetupCheck",
                                           "Not available in this build.");
    case perm::step_status::not_elevated:
        return QCoreApplication::translate(
            "SetupCheck",
            "Windows did not give the helper administrator rights, so nothing "
            "was changed.");
    case perm::step_status::invalid:
        return QCoreApplication::translate(
            "SetupCheck",
            "The helper refused the rule, so nothing was changed. Use the "
            "command shown instead.");
    case perm::step_status::failed:
        break;
    }
    return QCoreApplication::translate(
        "SetupCheck",
        "The firewall rule could not be changed. Use the command shown "
        "instead, in a terminal opened as administrator.");
}

step_report run(const perm::firewall_rule_spec& spec, bool remove)
{
    step_report report;
    if (!firewall_step_available())
    {
        report.status = perm::step_status::unsupported;
        report.message = report_text(report.status, remove);
        return report;
    }
    const perm::step_result result = perm::run_elevated(
        helper_path().toStdString(), perm::elevated_arguments(spec, remove));
    report.status = result.status;
    report.message = report_text(result.status, remove);
    return report;
}

} // namespace

QString helper_path()
{
    const QDir directory(QCoreApplication::applicationDirPath());
    for (const char* name : {"vss_cli.exe", "vss_cli"})
    {
        const QFileInfo candidate(
            directory.filePath(QString::fromLatin1(name)));
        if (candidate.isFile())
            return QDir::toNativeSeparators(candidate.absoluteFilePath());
    }
    return {};
}

bool firewall_step_available()
{
    return perm::can_manage_firewall_rules() && perm::can_run_elevated() &&
           !helper_path().isEmpty();
}

step_report allow_through_firewall(const perm::firewall_rule_spec& spec)
{
    return run(spec, false);
}

step_report remove_firewall_rule(const perm::firewall_rule_spec& spec)
{
    return run(spec, true);
}

bool group_step_available()
{
    return perm::can_add_user_to_group();
}

step_report add_to_serial_group(const perm::group_membership_spec& spec)
{
    step_report report;
    if (!group_step_available())
    {
        report.status = perm::step_status::unsupported;
        report.message = QCoreApplication::translate(
            "SetupCheck", "Not available in this build.");
        return report;
    }
    const perm::step_result result = perm::add_user_to_group(spec);
    report.status = result.status;
    switch (result.status)
    {
    case perm::step_status::done:
        report.message = QCoreApplication::translate(
            "SetupCheck",
            "Done. Sign out and back in so that the new group takes effect, "
            "then choose Re-check.");
        break;
    case perm::step_status::declined:
        report.message = QCoreApplication::translate(
            "SetupCheck",
            "You dismissed the authentication prompt, so nothing was "
            "changed.");
        break;
    case perm::step_status::not_elevated:
        report.message = QCoreApplication::translate(
            "SetupCheck",
            "The system did not authorise the change, so nothing was "
            "changed. Use the command shown instead.");
        break;
    default:
        report.message = QCoreApplication::translate(
            "SetupCheck",
            "The group could not be changed. Use the command shown instead, "
            "in a terminal.");
        break;
    }
    return report;
}

namespace
{

constexpr int kExitSetupDone = 0;
constexpr int kExitSetupRestart = 10;
constexpr int kExitSetupNotAdmin = 2;
constexpr int kExitSetupUntrusted = 6;
constexpr int kExitSetupDriver = 7;
constexpr int kExitSetupHelper = 8;
constexpr int kExitSetupIncomplete = 9;

/// The wait for the script: a driver install and a service start take a while
/// on a slow disk, and a hang must still end.
constexpr int kSetupWaitSeconds = 300;

bool package_dir_valid(const QDir& directory)
{
    return directory.exists() &&
           QFileInfo(directory.filePath(QStringLiteral("setup-driver.ps1")))
               .isFile();
}

QString powershell_path()
{
    const QString root = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("SystemRoot"));
    if (root.isEmpty())
        return {};
    const QFileInfo candidate(QDir(root).filePath(
        QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe")));
    return candidate.isFile()
               ? QDir::toNativeSeparators(candidate.absoluteFilePath())
               : QString();
}

} // namespace

QString driver_package_dir()
{
    const QString named = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("VSS_DRIVER_PACKAGE"));
    if (!named.isEmpty() && package_dir_valid(QDir(named)))
        return QDir::toNativeSeparators(QFileInfo(named).absoluteFilePath());
    const QDir beside(QDir(QCoreApplication::applicationDirPath())
                          .filePath(QStringLiteral("driver")));
    if (package_dir_valid(beside))
        return QDir::toNativeSeparators(beside.absolutePath());
    return {};
}

bool driver_setup_available()
{
    return perm::can_run_elevated() && !powershell_path().isEmpty() &&
           !driver_package_dir().isEmpty();
}

QString driver_setup_command()
{
    const QString directory = driver_package_dir();
    if (directory.isEmpty())
        return {};
    return QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -File "
                          "\"%1\\setup-driver.ps1\"")
        .arg(directory);
}

driver_setup_report report_for_driver_setup(const perm::step_result& result)
{
    driver_setup_report report;
    report.exit_code = result.exit_code;
    if (result.status == perm::step_status::unsupported)
    {
        report.result = driver_setup_result::unavailable;
    }
    else if (result.status == perm::step_status::declined)
    {
        report.result = driver_setup_result::declined;
    }
    else
    {
        switch (result.exit_code)
        {
        case kExitSetupDone:
            report.result = driver_setup_result::done;
            break;
        case kExitSetupRestart:
            report.result = driver_setup_result::done_restart;
            break;
        case kExitSetupNotAdmin:
            report.result = driver_setup_result::not_elevated;
            break;
        case kExitSetupUntrusted:
            report.result = driver_setup_result::untrusted;
            break;
        case kExitSetupDriver:
            report.result = driver_setup_result::driver_failed;
            break;
        case kExitSetupHelper:
            report.result = driver_setup_result::helper_failed;
            break;
        case kExitSetupIncomplete:
            report.result = driver_setup_result::incomplete;
            break;
        default:
            report.result = driver_setup_result::failed;
            break;
        }
    }

    switch (report.result)
    {
    case driver_setup_result::done:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "Virtual Serial Studio is set up. The driver and the helper "
            "service are installed and running.");
        break;
    case driver_setup_result::done_restart:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "Setup finished, but Windows needs a restart before the driver "
            "can be used. Restart Windows, then open Virtual Serial Studio "
            "again.");
        break;
    case driver_setup_result::declined:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "You declined the Windows permission prompt, so nothing was "
            "changed.");
        break;
    case driver_setup_result::untrusted:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "Windows does not trust the signature on the driver files, so "
            "nothing was installed. Released builds are signed and install "
            "without this step. For a build you made yourself, trust its test "
            "certificate or turn on test signing first (the Windows driver "
            "notes say how), then try again.");
        break;
    case driver_setup_result::driver_failed:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "Windows could not install the driver, and nothing else was "
            "changed. Copy the details, or run the command shown in a "
            "terminal opened as administrator to see Windows' own message.");
        break;
    case driver_setup_result::helper_failed:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "The driver is installed, but the helper service could not be "
            "installed or started. Choose Set up Virtual Serial Studio again "
            "to retry.");
        break;
    case driver_setup_result::incomplete:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "The setup files are not all there, so nothing was changed. "
            "Install Virtual Serial Studio again, or copy the whole driver "
            "folder next to the program.");
        break;
    case driver_setup_result::not_elevated:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "Windows did not give setup administrator rights, so nothing "
            "was changed.");
        break;
    case driver_setup_result::unavailable:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "The driver files are not with this copy of Virtual Serial "
            "Studio, so it cannot set up for you. Install Virtual Serial "
            "Studio from its installer, or follow the steps for a build you "
            "made yourself.");
        break;
    case driver_setup_result::failed:
        report.message = QCoreApplication::translate(
            "DriverSetup",
            "Setup did not finish. Copy the details, or run the command "
            "shown in a terminal opened as administrator to see what "
            "happened.");
        break;
    }
    return report;
}

driver_setup_report set_up_driver()
{
    if (!driver_setup_available())
    {
        perm::step_result none;
        none.status = perm::step_status::unsupported;
        return report_for_driver_setup(none);
    }
    const QString directory = driver_package_dir();
    const QString script =
        QDir(directory).filePath(QStringLiteral("setup-driver.ps1"));
    const std::vector<std::string> arguments = {
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        QDir::toNativeSeparators(script).toStdString(),
        "-PackageRoot",
        directory.toStdString()};
    return report_for_driver_setup(perm::run_elevated(
        powershell_path().toStdString(), arguments, kSetupWaitSeconds));
}

} // namespace vss::gui::support
