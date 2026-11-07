//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/setup_facts.hpp"

#include "backend/driver_error.hpp"
#include "support/permission_rows.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QUrl>

#if defined(Q_OS_WIN)
#include <windows.h>

#include <shellapi.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif

#if defined(Q_OS_LINUX)
#include <grp.h>

#include <vector>
#endif

#include <algorithm>

namespace vss::gui::support
{
namespace
{

QString plural(const char* single, int count)
{
    return QCoreApplication::translate("SetupCheck", single, nullptr, count);
}

QString device_sentence(const QStringList& paths, int count)
{
    if (count <= 0)
        return QCoreApplication::translate(
            "SetupCheck",
            "No serial devices are visible to Virtual Serial Studio. "
            "You can still use virtual ports.");
    if (paths.isEmpty())
        return plural(QT_TRANSLATE_NOOP(
                          "SetupCheck",
                          "%n serial device(s) are visible to Virtual Serial "
                          "Studio."),
                      count);

    QStringList shown = paths.mid(0, 3);
    if (count > shown.size())
    {
        return QCoreApplication::translate(
                   "SetupCheck",
                   "%1 and %2 more are visible to Virtual Serial Studio.")
            .arg(shown.join(QStringLiteral(", ")))
            .arg(count - shown.size());
    }
    if (shown.size() == 1)
        return QCoreApplication::translate(
                   "SetupCheck", "%1 is visible to Virtual Serial Studio.")
            .arg(shown.first());
    const QString last = shown.takeLast();
    return QCoreApplication::translate(
               "SetupCheck", "%1 and %2 are visible to Virtual Serial Studio.")
        .arg(shown.join(QStringLiteral(", ")), last);
}

setup_row ok_row(const QString& title, const QString& sentence)
{
    return {setup_state::ok,
            QStringLiteral("done"),
            title,
            sentence,
            QCoreApplication::translate("SetupCheck", "OK"),
            {},
            setup_action::none,
            false};
}

setup_row not_checked_row(const QString& title, const QString& sentence)
{
    return {setup_state::not_checked,
            QStringLiteral("info"),
            title,
            sentence,
            QCoreApplication::translate("SetupCheck", "Not checked yet"),
            {},
            setup_action::none,
            false};
}

setup_row devices_row(const setup_facts& facts, const QStringList& paths,
                      bool checks_available)
{
    setup_row row;
    row.title =
        QCoreApplication::translate("SetupCheck", "Real serial devices");
    row.icon = QStringLiteral("device");
    row.action_text = QCoreApplication::translate("SetupCheck", "Refresh");
    row.action = setup_action::refresh_devices;
    if (!checks_available)
    {
        return not_checked_row(
            row.title,
            QCoreApplication::translate(
                "SetupCheck", "Studio did not check the serial devices."));
    }
    row.state = setup_state::ok;
    row.chip = QCoreApplication::translate("SetupCheck", "OK");
    row.sentence = device_sentence(paths, facts.serial_devices);
    return row;
}

/// Either driver is enough for other programs to see Studio's ports.
bool windows_ports_ready(const setup_facts& facts)
{
    return (facts.driver_found &&
            !driver_problem_offers_setup(facts.driver.problem)) ||
           facts.com0com_found;
}

void add_windows_rows(QVector<setup_row>& rows, const setup_facts& facts,
                      const platform_facts& platform, bool checks_available,
                      const QStringList& paths)
{
    setup_row driver;
    driver.title =
        QCoreApplication::translate("SetupCheck", "Virtual port driver");
    driver.icon = QStringLiteral("warning");
    if (!checks_available)
    {
        rows.push_back(not_checked_row(
            driver.title,
            QCoreApplication::translate(
                "SetupCheck", "Studio did not check for the virtual port "
                              "driver.")));
    }
    else if (facts.driver_found &&
             !driver_problem_offers_setup(facts.driver.problem))
    {
        QString sentence = QCoreApplication::translate(
            "SetupCheck", "The Virtual Serial Studio driver is installed, so "
                          "other programs can see ports Studio makes as COM "
                          "ports.");
        if (!facts.driver_version.isEmpty())
        {
            sentence += QLatin1Char(' ');
            sentence +=
                QCoreApplication::translate("SetupCheck", "Driver version %1.")
                    .arg(facts.driver_version);
        }
        if (facts.com0com_found)
        {
            sentence += QLatin1Char(' ');
            sentence += QCoreApplication::translate(
                "SetupCheck",
                "com0com is also installed, and Studio tries its own driver "
                "first.");
        }
        rows.push_back(ok_row(driver.title, sentence));
    }
    else if (facts.com0com_found)
    {
        const QString sentence =
            facts.com0com_pairs > 0
                ? plural(QT_TRANSLATE_NOOP(
                             "SetupCheck",
                             "com0com is installed with %n pair(s), so other "
                             "programs can see virtual ports as COM ports."),
                         facts.com0com_pairs)
                : QCoreApplication::translate(
                      "SetupCheck",
                      "com0com is installed, so other programs can see virtual "
                      "ports as COM ports.");
        rows.push_back(ok_row(driver.title, sentence));
    }
    else if (driver_problem_offers_setup(facts.driver.problem))
    {
        // One piece is missing, old or refused, and the guided set-up fixes
        // it: say which, and offer the one action.
        driver.state = setup_state::attention;
        driver.sentence = driver_problem_body(facts.driver.problem);
        driver.chip =
            QCoreApplication::translate("SetupCheck", "Needs attention");
        driver.action_text = QCoreApplication::translate(
            "SetupCheck", "Set up Virtual Serial Studio");
        driver.action = setup_action::setup_driver;
        driver.highlight = true;
        rows.push_back(driver);
    }
    else
    {
        driver.state = setup_state::attention;
        driver.sentence = QCoreApplication::translate(
            "SetupCheck", "Neither the Virtual Serial Studio driver nor "
                          "com0com was found. Windows needs a driver before "
                          "other programs can see virtual ports as COM "
                          "ports.");
        driver.chip =
            QCoreApplication::translate("SetupCheck", "Needs attention");
        driver.action_text =
            QCoreApplication::translate("SetupCheck", "Install com0com");
        driver.action = setup_action::install_driver;
        driver.highlight = true;
        rows.push_back(driver);
    }

    setup_row administrator;
    administrator.title =
        QCoreApplication::translate("SetupCheck", "Administrator rights");
    administrator.icon = QStringLiteral("info");
    if (!checks_available || !platform.elevation_known)
    {
        rows.push_back(not_checked_row(
            administrator.title,
            QCoreApplication::translate(
                "SetupCheck",
                "Studio could not check for administrator rights.")));
    }
    else if (facts.driver_found &&
             !driver_problem_offers_setup(facts.driver.problem))
    {
        rows.push_back(ok_row(
            administrator.title,
            QCoreApplication::translate(
                "SetupCheck", "The Virtual Serial Studio driver makes ports "
                              "without administrator rights.")));
    }
    else if (platform.elevated)
    {
        rows.push_back(ok_row(
            administrator.title,
            QCoreApplication::translate(
                "SetupCheck",
                "Studio is running as administrator, so it can create linked "
                "pairs.")));
    }
    else
    {
        administrator.state = setup_state::info;
        administrator.sentence = QCoreApplication::translate(
            "SetupCheck",
            "Creating a new linked pair needs administrator rights, once "
            "per pair. Everything else runs without them.");
        if (const QString who = elevation_sentence(platform.permissions);
            !who.isEmpty())
        {
            administrator.sentence += QLatin1Char(' ') + who;
            // Said before the prompt, so the prompt is not a surprise.
            administrator.sentence +=
                QLatin1Char(' ') +
                QCoreApplication::translate(
                    "SetupCheck",
                    "The Windows prompt names the program and its publisher: "
                    "choose Yes if they match. Studio then restarts and "
                    "starts the same ports.");
        }
        administrator.chip =
            QCoreApplication::translate("SetupCheck", "Needed later");
        administrator.action_text = QCoreApplication::translate(
            "SetupCheck", "Restart as administrator");
        administrator.action = setup_action::restart_elevated;
        rows.push_back(administrator);
    }

    rows.push_back(devices_row(facts, paths, checks_available));

    setup_row carry_on;
    carry_on.state = setup_state::info;
    carry_on.icon = QStringLiteral("run");
    carry_on.title =
        QCoreApplication::translate("SetupCheck", "Carry on without a driver");
    carry_on.sentence = QCoreApplication::translate(
        "SetupCheck", "You can still use in-process ports, the monitor "
                      "and the Send panel to try things out.");
    carry_on.action_text =
        QCoreApplication::translate("SetupCheck", "Continue in test mode");
    carry_on.action = setup_action::continue_test;
    rows.push_back(carry_on);
}

void add_unix_rows(QVector<setup_row>& rows, const setup_facts& facts,
                   const platform_facts& platform, bool checks_available,
                   const QStringList& paths)
{
    const bool mac = facts.platform == QLatin1String("macos");

    setup_row pseudo;
    pseudo.title =
        QCoreApplication::translate("SetupCheck", "Pseudo terminals");
    pseudo.icon = QStringLiteral("device");
    if (!checks_available || !platform.pty_known)
    {
        rows.push_back(not_checked_row(
            pseudo.title,
            QCoreApplication::translate("SetupCheck",
                                        "Studio did not check whether pseudo "
                                        "terminals are available.")));
    }
    else if (platform.pty_available)
    {
        rows.push_back(ok_row(
            pseudo.title, QCoreApplication::translate(
                              "SetupCheck", "Pseudo terminals are available, "
                                            "so other programs can open new "
                                            "virtual ports.")));
    }
    else
    {
        pseudo.state = setup_state::attention;
        pseudo.sentence = QCoreApplication::translate(
            "SetupCheck", "Pseudo terminals are not available. Other "
                          "programs will not be able to open new virtual "
                          "ports.");
        pseudo.chip =
            QCoreApplication::translate("SetupCheck", "Needs attention");
        pseudo.highlight = true;
        rows.push_back(pseudo);
    }

    setup_row group;
    group.title =
        mac ? QCoreApplication::translate("SetupCheck", "Serial port access")
            : QCoreApplication::translate("SetupCheck",
                                          "Serial port group membership");
    group.icon = QStringLiteral("info");
    if (mac || !checks_available || !platform.dialout_known)
    {
        rows.push_back(not_checked_row(
            group.title,
            mac ? QCoreApplication::translate(
                      "SetupCheck",
                      "Studio has no group check for macOS. If a device cannot "
                      "be opened, check its permissions in /dev.")
                : QCoreApplication::translate(
                      "SetupCheck",
                      "Studio did not check your membership of the dialout "
                      "group.")));
    }
    else if (platform.dialout_member)
    {
        rows.push_back(ok_row(
            group.title,
            QCoreApplication::translate(
                "SetupCheck",
                "You are in the dialout group, so programs can open serial "
                "devices.")));
    }
    else
    {
        group.state = setup_state::attention;
        group.sentence = QCoreApplication::translate(
            "SetupCheck", "Your account is not in the dialout group, so "
                          "programs may not be able to open serial devices.");
        group.chip =
            QCoreApplication::translate("SetupCheck", "Needs attention");
        group.action_text =
            QCoreApplication::translate("SetupCheck", "How to add me");
        group.action = setup_action::dialout_help;
        group.highlight = true;
        rows.push_back(group);
    }

    rows.push_back(devices_row(facts, paths, checks_available));
}

} // namespace

QString host_platform()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

platform_facts probe_platform_facts()
{
    platform_facts facts;
#if defined(Q_OS_WIN)
    facts.elevation_known = true;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0)
    {
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation,
                                sizeof(elevation), &size) != 0)
        {
            facts.elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    // Windows has no pseudo terminals to offer other programs.
    facts.pty_known = true;
    facts.pty_available = false;
#else
    facts.elevation_known = true;
    facts.elevated = geteuid() == 0;
    facts.pty_known = true;
    facts.pty_available = QFileInfo(QStringLiteral("/dev/ptmx")).exists();
#endif

#if defined(Q_OS_LINUX)
    facts.dialout_known = true;
    if (facts.elevated)
    {
        facts.dialout_member = true;
    }
    else
    {
        const group* dialout = getgrnam("dialout");
        if (dialout == nullptr)
        {
            facts.dialout_known = false;
        }
        else
        {
            const gid_t target = dialout->gr_gid;
            facts.dialout_member = getegid() == target;
            if (!facts.dialout_member)
            {
                const int count = getgroups(0, nullptr);
                std::vector<gid_t> groups(
                    static_cast<std::size_t>(std::max(count, 0)));
                if (count > 0 && getgroups(count, groups.data()) == count)
                {
                    facts.dialout_member =
                        std::find(groups.begin(), groups.end(), target) !=
                        groups.end();
                }
            }
        }
    }
#endif
    return facts;
}

setup_report build_setup_report(const setup_facts& facts,
                                const platform_facts& platform,
                                const QStringList& device_paths,
                                bool checks_available)
{
    setup_report report;
    const QString platform_name =
        facts.platform.isEmpty() ? host_platform() : facts.platform;
    const bool windows = platform_name == QLatin1String("windows");

    setup_facts normalised = facts;
    normalised.platform = platform_name;
    if (windows)
    {
        add_windows_rows(report.rows, normalised, platform, checks_available,
                         device_paths);
    }
    else
    {
        add_unix_rows(report.rows, normalised, platform, checks_available,
                      device_paths);
    }
    append_permission_rows(report.rows, platform.permissions);

    int attention = 0;
    int not_checked = 0;
    for (const setup_row& row : report.rows)
    {
        if (row.state == setup_state::attention)
            ++attention;
        else if (row.state == setup_state::not_checked)
            ++not_checked;
    }
    if (!checks_available || (attention == 0 && not_checked > 0))
        report.header_chip =
            QCoreApplication::translate("SetupCheck", "Not checked yet");
    else if (attention > 0)
    {
        report.header_chip =
            attention == 1
                ? QCoreApplication::translate("SetupCheck", "1 thing to fix")
                : plural(QT_TRANSLATE_NOOP("SetupCheck", "%n things to fix"),
                         attention);
    }
    else
        report.header_chip =
            QCoreApplication::translate("SetupCheck", "All set");

    if (windows && checks_available && !windows_ports_ready(normalised))
    {
        report.driver_note = QCoreApplication::translate(
            "SetupCheck",
            "Until a driver is installed, new ports only work inside "
            "Virtual Serial Studio. Other programs will not see them.");
    }

    if (!checks_available || (not_checked > 0 && attention == 0))
        report.state = setup_report::readiness::not_checked;
    else if (attention > 0)
        report.state = setup_report::readiness::needs_attention;
    else
        report.state = setup_report::readiness::ready;

    if (windows && checks_available &&
        driver_problem_offers_setup(normalised.driver.problem))
        report.problem = normalised.driver.problem;

    if (!checks_available)
        report.ports_ready = false;
    else if (windows)
        report.ports_ready = windows_ports_ready(normalised);
    else
        report.ports_ready = platform.pty_known && platform.pty_available;

    report.platform_note = QCoreApplication::translate(
        "SetupCheck", "On Linux and macOS this check looks at "
                      "pseudo terminals and your serial port "
                      "group membership instead.");
    return report;
}

setup_report query_setup_report(const StudioBackend& backend)
{
    VSS_PROFILE_ZONE("gui.setup.query_report");
    setup_facts facts;
    {
        VSS_PROFILE_ZONE("gui.setup.facts");
        facts = backend.facts();
    }
    platform_facts platform;
    {
        VSS_PROFILE_ZONE("gui.setup.platform_facts");
        platform = probe_platform_facts();
    }
    {
        VSS_PROFILE_ZONE("gui.setup.permissions");
        platform.permissions =
            vss::permissions::probe(permission_context(backend));
    }
    const bool checks_available = backend.caps().setup_check;

    QStringList paths;
    {
        VSS_PROFILE_ZONE("gui.setup.devices");
        for (const device_view& device : backend.devices())
        {
            if (!device.path.isEmpty())
                paths.push_back(device.path);
        }
    }
    return build_setup_report(facts, platform, paths, checks_available);
}

const setup_report& setup_report_cache::get(const StudioBackend& backend)
{
    VSS_PROFILE_ZONE("gui.setup.cache_get");
    // The firewall rows depend on the listening ports, so a change of those
    // asks again; any other port coming or going does not.
    QString key;
    for (const port_view& port : backend.ports())
    {
        if (port.path.contains(QLatin1String("listen")))
            key += port.path + QLatin1Char('\n');
    }

    const auto now = std::chrono::steady_clock::now();
    const auto age = backend.probe_cache_age();
    if (valid_ && age.count() > 0 && key == ports_key_ && now - taken_ < age)
        return report_;

    report_ = query_setup_report(backend);
    ports_key_ = std::move(key);
    taken_ = now;
    valid_ = true;
    return report_;
}

QString com0com_project_url()
{
    return QStringLiteral("https://sourceforge.net/projects/com0com/");
}

bool open_com0com_project_page()
{
    return QDesktopServices::openUrl(QUrl(com0com_project_url()));
}

QString dialout_add_command()
{
    return QStringLiteral("sudo usermod -aG dialout $USER");
}

bool open_system_accessibility_settings()
{
#if defined(Q_OS_WIN)
    return QDesktopServices::openUrl(
        QUrl(QStringLiteral("ms-settings:easeofaccess")));
#elif defined(Q_OS_MACOS)
    return QDesktopServices::openUrl(QUrl(QStringLiteral(
        "x-apple.systempreferences:com.apple.preference.universalaccess")));
#else
    if (QProcess::startDetached(QStringLiteral("gnome-control-center"),
                                {QStringLiteral("universal-access")}))
    {
        return true;
    }
    return QProcess::startDetached(QStringLiteral("systemsettings"),
                                   {QStringLiteral("kcm_access")});
#endif
}

bool relaunch_as_administrator(QString* error)
{
#if defined(Q_OS_WIN)
    const QString executable = QCoreApplication::applicationFilePath();
    if (executable.isEmpty())
    {
        if (error != nullptr)
            *error = QCoreApplication::translate(
                "SetupCheck", "The application path is not known.");
        return false;
    }
    const std::wstring path =
        QDir::toNativeSeparators(executable).toStdWString();
    // ShellExecuteEx reports a declined UAC prompt as ERROR_CANCELLED; the
    // older ShellExecute folded it into SE_ERR_ACCESSDENIED, so the user could
    // not tell "I said no" from "Windows refused".
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = path.c_str();
    // The new Studio waits for this one to quit and starts the same ports.
    info.lpParameters = L"--replace";
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info) != 0)
        return true;
    if (error != nullptr)
    {
        *error =
            GetLastError() == ERROR_CANCELLED
                ? QCoreApplication::translate(
                      "SetupCheck",
                      "You declined the Windows permission prompt, so Studio "
                      "keeps running as before. Nothing was changed.")
                : QCoreApplication::translate(
                      "SetupCheck",
                      "Windows could not restart Virtual Serial Studio as "
                      "administrator.");
    }
    return false;
#else
    if (error != nullptr)
        *error = QCoreApplication::translate(
            "SetupCheck", "Restarting as administrator is only available on "
                          "Windows.");
    return false;
#endif
}

} // namespace vss::gui::support
