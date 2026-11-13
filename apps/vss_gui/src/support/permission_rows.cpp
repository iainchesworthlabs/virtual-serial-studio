//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/permission_rows.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <string>
#include <string_view>

namespace vss::gui::support
{
namespace
{

namespace perm = vss::permissions;

QString port_text(const perm::fact& fact)
{
    const QString protocol = fact.protocol == perm::transport_protocol::tcp
                                 ? QStringLiteral("TCP")
                                 : QStringLiteral("UDP");
    return fact.port == 0
               ? QCoreApplication::translate("SetupCheck",
                                             "the port you choose")
               : QStringLiteral("%1 %2").arg(protocol).arg(fact.port);
}

setup_row firewall_row(const perm::fact& fact)
{
    setup_row row;
    row.icon = QStringLiteral("info");
    row.title = QCoreApplication::translate("SetupCheck", "Windows Firewall");

    if (fact.status == perm::state::unknown)
    {
        row.state = setup_state::not_checked;
        row.chip = QCoreApplication::translate("SetupCheck", "Not checked yet");
        row.sentence = QCoreApplication::translate(
            "SetupCheck",
            "Studio could not read the Windows Firewall rules, so it cannot "
            "say whether other computers can connect.");
        return row;
    }

    if (fact.why == perm::reason::loopback_only)
    {
        row.state = setup_state::ok;
        row.chip = QCoreApplication::translate("SetupCheck", "OK");
        row.sentence = QCoreApplication::translate(
            "SetupCheck",
            "Windows Firewall is not involved. Studio is not waiting for other "
            "computers to connect.");
        return row;
    }
    if (fact.why == perm::reason::firewall_off)
    {
        row.state = setup_state::ok;
        row.chip = QCoreApplication::translate("SetupCheck", "OK");
        row.sentence = QCoreApplication::translate(
            "SetupCheck",
            "Windows Firewall is off for the network you are on, so it does "
            "not "
            "stop other computers. Studio does not change that setting.");
        return row;
    }
    if (fact.why == perm::reason::allow_rule)
    {
        row.state = setup_state::ok;
        row.chip = QCoreApplication::translate("SetupCheck", "OK");
        row.sentence =
            QCoreApplication::translate(
                "SetupCheck", "Windows Firewall has a rule that lets other "
                              "computers reach %1.")
                .arg(port_text(fact));
        return row;
    }

    row.state = setup_state::attention;
    row.chip = QCoreApplication::translate("SetupCheck", "Needs attention");
    row.highlight = true;
    row.icon = QStringLiteral("warning");
    row.action = setup_action::firewall_help;
    if (fact.why == perm::reason::no_allow_rule)
    {
        row.sentence = QCoreApplication::translate(
                           "SetupCheck",
                           "Windows Firewall may stop other computers "
                           "connecting to %1. Studio can add one rule for this "
                           "program and this port, after you agree.")
                           .arg(port_text(fact));
        row.action_text =
            fact.can_act
                ? QCoreApplication::translate("SetupCheck",
                                              "Allow through the firewall")
                : QCoreApplication::translate("SetupCheck", "How to allow it");
    }
    else if (fact.why == perm::reason::blocked_by_rule)
    {
        row.sentence = QCoreApplication::translate(
                           "SetupCheck",
                           "A Windows Firewall rule blocks this program on %1. "
                           "Studio never removes a block rule; change it in "
                           "Windows Defender Firewall.")
                           .arg(port_text(fact));
        row.action_text =
            QCoreApplication::translate("SetupCheck", "How to change it");
    }
    else
    {
        row.sentence = QCoreApplication::translate(
            "SetupCheck",
            "Windows Firewall is set to block all incoming connections, which "
            "overrides every rule. Change that setting in Windows Security if "
            "you want other computers to connect.");
        row.action_text =
            QCoreApplication::translate("SetupCheck", "How to change it");
    }
    return row;
}

QString subject_text(const perm::fact& fact)
{
    return QString::fromStdString(fact.subject);
}

/// A row for the serial device a user may not open, or "none" when the fact has
/// nothing to show.
std::optional<setup_row> serial_access_row(const perm::fact& fact)
{
    if (fact.why == perm::reason::no_devices)
        return std::nullopt;

    setup_row row;
    row.icon = QStringLiteral("device");
    row.title =
        QCoreApplication::translate("SetupCheck", "Serial device access");
    if (fact.status == perm::state::unknown)
    {
        row.state = setup_state::not_checked;
        row.chip = QCoreApplication::translate("SetupCheck", "Not checked yet");
        row.sentence = QCoreApplication::translate(
            "SetupCheck", "Studio could not check whether you may open the "
                          "serial devices.");
        return row;
    }
    if (fact.status == perm::state::ok)
    {
        row.state = setup_state::ok;
        row.chip = QCoreApplication::translate("SetupCheck", "OK");
        row.sentence = QCoreApplication::translate(
            "SetupCheck", "You may open the serial devices Studio found.");
        return row;
    }

    row.state = setup_state::attention;
    row.chip = QCoreApplication::translate("SetupCheck", "Needs attention");
    row.icon = QStringLiteral("warning");
    row.highlight = true;
    row.action = setup_action::serial_access_help;
    if (fact.why == perm::reason::not_in_group)
    {
        row.sentence = QCoreApplication::translate(
                           "SetupCheck", "Your account is not in the group "
                                         "that owns %1, so programs cannot "
                                         "open it.")
                           .arg(subject_text(fact));
        row.action_text =
            fact.can_act
                ? QCoreApplication::translate("SetupCheck",
                                              "Add me to the group")
                : QCoreApplication::translate("SetupCheck", "How to add me");
    }
    else if (fact.why == perm::reason::relogin_needed)
    {
        row.sentence = QCoreApplication::translate(
                           "SetupCheck", "Your account is in the group that "
                                         "owns %1, but this login does not "
                                         "know yet. Sign out and back in.")
                           .arg(subject_text(fact));
        row.action_text =
            QCoreApplication::translate("SetupCheck", "How to refresh");
    }
    else if (fact.why == perm::reason::mode_denies)
    {
        row.sentence = QCoreApplication::translate(
                           "SetupCheck", "The group that owns %1 may not read "
                                         "and write it.")
                           .arg(subject_text(fact));
        row.action_text =
            QCoreApplication::translate("SetupCheck", "How to check it");
    }
    else
    {
        row.sentence = QCoreApplication::translate(
                           "SetupCheck", "Something outside the file "
                                         "permissions refused %1: AppArmor, "
                                         "SELinux or a sandbox.")
                           .arg(subject_text(fact));
        row.action_text =
            QCoreApplication::translate("SetupCheck", "Show the fix");
    }
    return row;
}

/// Confinement is shown only when something confines the program, and as
/// information: most confined programs run without trouble.
std::optional<setup_row> confinement_row(const perm::fact& fact)
{
    if (fact.status != perm::state::info)
        return std::nullopt;

    setup_row row;
    row.state = setup_state::info;
    row.icon = QStringLiteral("info");
    row.title = QCoreApplication::translate("SetupCheck", "Confinement");
    row.chip = QCoreApplication::translate("SetupCheck", "For information");
    row.action = setup_action::confinement_help;
    row.action_text = QCoreApplication::translate("SetupCheck", "Show the fix");
    if (fact.why == perm::reason::flatpak)
    {
        row.sentence = QCoreApplication::translate(
            "SetupCheck", "Studio runs in a Flatpak sandbox. It sees only the "
                          "devices it was given.");
    }
    else if (fact.why == perm::reason::snap)
    {
        row.sentence = QCoreApplication::translate(
            "SetupCheck", "Studio runs in a Snap sandbox. It reaches a serial "
                          "device only through a connected interface.");
    }
    else if (fact.why == perm::reason::apparmor_profile)
    {
        row.sentence = QCoreApplication::translate(
                           "SetupCheck", "AppArmor confines Studio with the "
                                         "profile %1. It may refuse serial "
                                         "devices.")
                           .arg(subject_text(fact));
    }
    else if (fact.why == perm::reason::macos_sandbox)
    {
        row.sentence = QCoreApplication::translate(
            "SetupCheck", "Studio runs in the macOS App Sandbox. It reaches a "
                          "serial device or the network only with the right "
                          "entitlements.");
    }
    else
    {
        row.sentence = QCoreApplication::translate(
            "SetupCheck", "SELinux is enforcing. That is normal; this only "
                          "matters if a device is refused.");
    }
    return row;
}

std::optional<setup_row> linux_firewall_row(const perm::fact& fact)
{
    // Nothing to say while Studio listens on this computer only.
    if (fact.why == perm::reason::loopback_only)
        return std::nullopt;

    setup_row row;
    row.icon = QStringLiteral("info");
    row.title = QCoreApplication::translate("SetupCheck", "Firewall");
    row.action = setup_action::firewall_help;
    if (fact.why == perm::reason::ufw_active ||
        fact.why == perm::reason::firewalld_active)
    {
        row.state = setup_state::info;
        row.chip = QCoreApplication::translate("SetupCheck", "For information");
        row.sentence = QCoreApplication::translate(
                           "SetupCheck", "A firewall is on. If other "
                                         "computers cannot reach %1, open "
                                         "the port.")
                           .arg(port_text(fact));
        row.action_text =
            QCoreApplication::translate("SetupCheck", "Show the command");
    }
    else
    {
        row.state = setup_state::not_checked;
        row.chip = QCoreApplication::translate("SetupCheck", "Not checked yet");
        row.sentence = QCoreApplication::translate(
            "SetupCheck", "Studio cannot read the firewall rules without "
                          "administrator rights, so it cannot say whether "
                          "other computers can connect.");
        row.action_text =
            QCoreApplication::translate("SetupCheck", "How to check");
    }
    return row;
}

std::optional<setup_row> macos_firewall_row(const perm::fact& fact)
{
    // Nothing to say while Studio listens on this computer only.
    if (fact.why == perm::reason::loopback_only)
        return std::nullopt;

    setup_row row;
    row.icon = QStringLiteral("info");
    row.title = QCoreApplication::translate("SetupCheck", "macOS firewall");
    row.action = setup_action::firewall_help;
    if (fact.why == perm::reason::firewall_off)
    {
        row.state = setup_state::ok;
        row.chip = QCoreApplication::translate("SetupCheck", "OK");
        row.sentence = QCoreApplication::translate(
            "SetupCheck",
            "The macOS firewall is off, so it does not stop other "
            "computers. Studio does not change that setting.");
        row.action = setup_action::none;
    }
    else if (fact.why == perm::reason::alf_prompt)
    {
        row.state = setup_state::info;
        row.chip = QCoreApplication::translate("SetupCheck", "For information");
        row.sentence = QCoreApplication::translate(
            "SetupCheck", "The macOS firewall is on. The first time another "
                          "computer connects, macOS asks whether Studio may "
                          "accept incoming connections. Choose Allow.");
        row.action_text = QCoreApplication::translate(
            "SetupCheck", "If you chose Don't Allow");
    }
    else if (fact.why == perm::reason::block_all_inbound)
    {
        row.state = setup_state::attention;
        row.chip = QCoreApplication::translate("SetupCheck", "Needs attention");
        row.icon = QStringLiteral("warning");
        row.highlight = true;
        row.sentence = QCoreApplication::translate(
            "SetupCheck", "The macOS firewall is set to block all incoming "
                          "connections, which no program can get around. Turn "
                          "that off in System Settings if you want other "
                          "computers to connect.");
        row.action_text =
            QCoreApplication::translate("SetupCheck", "How to change it");
    }
    else
    {
        row.state = setup_state::not_checked;
        row.chip = QCoreApplication::translate("SetupCheck", "Not checked yet");
        row.sentence = QCoreApplication::translate(
            "SetupCheck",
            "Studio could not read the macOS firewall setting, so "
            "it cannot say whether other computers can connect.");
        row.action_text =
            QCoreApplication::translate("SetupCheck", "How to check");
    }
    return row;
}

std::optional<setup_row> local_network_row(const perm::fact&)
{
    setup_row row;
    row.state = setup_state::info;
    row.icon = QStringLiteral("info");
    row.title =
        QCoreApplication::translate("SetupCheck", "Local Network access");
    row.chip = QCoreApplication::translate("SetupCheck", "For information");
    row.sentence = QCoreApplication::translate(
        "SetupCheck",
        "macOS 15 and later asks once whether apps may connect to "
        "devices on your local network. It cannot be checked from "
        "here; if you chose Don't Allow, turn it on again.");
    row.action = setup_action::network_privacy_help;
    row.action_text = QCoreApplication::translate("SetupCheck", "Show where");
    return row;
}

} // namespace

perm::context permission_context(const StudioBackend& backend)
{
    perm::context ask;
    for (const port_view& port : backend.ports())
    {
        perm::listener each;
        if (perm::parse_listen_uri(port.path.toStdString(), each))
            ask.listeners.push_back(each);
    }
    // The real serial devices the machine has.  A device with no path (a
    // studio-only fixture) is not a file to check.
    for (const device_view& device : backend.devices())
    {
        if (device.path.startsWith(QLatin1Char('/')))
            ask.device_paths.push_back(device.path.toStdString());
    }
    return ask;
}

perm::context permission_context_for(const QString& uri)
{
    VSS_PROFILE_ZONE_NAMED(kProfileZone,
                           "gui.support.permission_rows.context_for");
    VSS_PROFILE_ZONE_TEXT(kProfileZone, uri.toStdString());
    perm::context ask;
    perm::listener each;
    if (perm::parse_listen_uri(uri.toStdString(), each))
        ask.listeners.push_back(each);
    return ask;
}

void append_permission_rows(QVector<setup_row>& rows,
                            const std::vector<perm::fact>& facts)
{
    VSS_PROFILE_ZONE("gui.support.permission_rows.append");
    // Before "Continue without a driver", which stays the last row.
    for (const perm::fact& fact : facts)
    {
        std::optional<setup_row> row;
        switch (fact.what)
        {
        case perm::topic::windows_firewall:
            row = firewall_row(fact);
            break;
        case perm::topic::serial_access:
            row = serial_access_row(fact);
            break;
        case perm::topic::confinement:
            row = confinement_row(fact);
            break;
        case perm::topic::linux_firewall:
            row = linux_firewall_row(fact);
            break;
        case perm::topic::macos_firewall:
            row = macos_firewall_row(fact);
            break;
        case perm::topic::macos_local_network:
            row = local_network_row(fact);
            break;
        case perm::topic::windows_elevation:
            break;
        }
        if (!row)
            continue;
        const auto carry_on = std::find_if(
            rows.begin(), rows.end(), [](const setup_row& each)
            { return each.action == setup_action::continue_test; });
        rows.insert(carry_on, *row);
    }
}

QString elevation_sentence(const std::vector<perm::fact>& facts)
{
    for (const perm::fact& fact : facts)
    {
        if (fact.what != perm::topic::windows_elevation)
            continue;
        if (fact.why == perm::reason::admin_not_elevated)
            return QCoreApplication::translate(
                "SetupCheck",
                "Your account is an administrator, so Windows will "
                "ask you to confirm.");
        if (fact.why == perm::reason::standard_user)
            return QCoreApplication::translate(
                "SetupCheck",
                "Your account is not an administrator, so Windows "
                "will ask for an administrator's password.");
    }
    return {};
}

std::optional<perm::fact> find_fact(const std::vector<perm::fact>& facts,
                                    perm::topic what)
{
    for (const perm::fact& fact : facts)
    {
        if (fact.what == what)
            return fact;
    }
    return std::nullopt;
}

std::optional<perm::fact> firewall_fact(const std::vector<perm::fact>& facts)
{
    for (const perm::fact& fact : facts)
    {
        if (fact.what == perm::topic::windows_firewall)
            return fact;
    }
    return std::nullopt;
}

} // namespace vss::gui::support
