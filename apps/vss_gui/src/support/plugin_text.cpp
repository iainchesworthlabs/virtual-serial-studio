//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/plugin_text.hpp"

#include "support/licence_text.hpp"

#include <QChar>
#include <QCoreApplication>
#include <QLocale>

namespace vss::gui::support
{
namespace
{

bool is_removed(char32_t code)
{
    // Control characters, line and paragraph separators, and every character
    // that changes the direction text is drawn in.
    if (code < 0x20 || (code >= 0x7f && code <= 0x9f))
        return true;
    if (code == 0x2028 || code == 0x2029 || code == 0x061c || code == 0x200e ||
        code == 0x200f)
        return true;
    if (code >= 0x202a && code <= 0x202e)
        return true;
    if (code >= 0x2066 && code <= 0x2069)
        return true;
    // Zero-width characters that hide text, and the byte order mark.
    if (code == 0x200b || code == 0x200c || code == 0x200d || code == 0x2060 ||
        code == 0xfeff)
        return true;
    return false;
}

QString date_text(const QDate& date)
{
    return QLocale().toString(date, QLocale::LongFormat);
}

} // namespace

QString plain_text(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype index = 0; index < text.size(); ++index)
    {
        const QChar unit = text.at(index);
        if (unit.isHighSurrogate() && index + 1 < text.size() &&
            text.at(index + 1).isLowSurrogate())
        {
            // A character outside the first plane: kept as it is.
            out.append(unit);
            out.append(text.at(index + 1));
            ++index;
            continue;
        }
        if (unit.isSurrogate())
            continue;
        if (!is_removed(unit.unicode()))
            out.append(unit);
    }
    return out;
}

QString wrappable_text(const QString& text)
{
    const QString clean = plain_text(text);
    QString out;
    out.reserve(clean.size() + clean.size() / 24);
    int run = 0;
    for (const QChar unit : clean)
    {
        if (unit.isSpace())
            run = 0;
        else if (++run > 24)
        {
            out.append(QChar(0x200b));
            run = 1;
        }
        out.append(unit);
    }
    return out;
}

QString quoted(const QString& text)
{
    return QString(QChar(0x201c)) + plain_text(text) + QString(QChar(0x201d));
}

QString display_author(const plugin_view& plugin)
{
    if (plugin.trust == plugin_trust::official)
    {
        return QCoreApplication::translate("PluginsDialog",
                                           "Virtual Serial Studio");
    }
    if (plugin.trust == plugin_trust::third_party &&
        !plain_text(plugin.key_name).isEmpty())
        return plain_text(plugin.key_name);
    return plain_text(plugin.author);
}

QString trust_line(plugin_trust trust, const QString& author)
{
    switch (trust)
    {
    case plugin_trust::official:
        return QCoreApplication::translate("PluginsDialog", "Official");
    case plugin_trust::third_party:
        return QCoreApplication::translate("PluginsDialog", "Signed by %1")
            .arg(plain_text(author));
    case plugin_trust::unsigned_plugin:
        return QCoreApplication::translate("PluginsDialog", "Unsigned");
    case plugin_trust::unknown_key:
        return QCoreApplication::translate(
            "PluginsDialog", "Signed by an author you have not approved");
    }
    return {};
}

QString trust_line(const plugin_view& plugin)
{
    return trust_line(plugin.trust, display_author(plugin));
}

QString state_text(plugin_state state)
{
    switch (state)
    {
    case plugin_state::available:
        return QCoreApplication::translate("PluginsDialog", "On");
    case plugin_state::starting:
        return QCoreApplication::translate("PluginsDialog", "Starting");
    case plugin_state::off:
        return QCoreApplication::translate("PluginsDialog", "Off");
    case plugin_state::needs_decision:
        return QCoreApplication::translate("PluginsDialog",
                                           "Needs your decision");
    case plugin_state::blocked:
        return QCoreApplication::translate("PluginsDialog", "Blocked");
    case plugin_state::needs_isolation:
        return QCoreApplication::translate("PluginsDialog", "Not isolated");
    case plugin_state::not_used:
        return QCoreApplication::translate("PluginsDialog", "Not used");
    case plugin_state::needs_licence:
        return QCoreApplication::translate("PluginsDialog", "Needs a licence");
    case plugin_state::stopped:
        return QCoreApplication::translate("PluginsDialog", "Stopped");
    case plugin_state::needs_newer_studio:
        return QCoreApplication::translate("PluginsDialog", "Not compatible");
    case plugin_state::cannot_be_used:
        return QCoreApplication::translate("PluginsDialog", "Can't be used");
    case plugin_state::not_installed:
        return QCoreApplication::translate("PluginsDialog", "Not installed");
    }
    return {};
}

StatusChip::kind chip_kind(plugin_state state)
{
    switch (state)
    {
    case plugin_state::available:
        return StatusChip::kind::healthy;
    case plugin_state::starting:
    case plugin_state::needs_decision:
    case plugin_state::needs_isolation:
    case plugin_state::needs_licence:
        return StatusChip::kind::waiting;
    case plugin_state::stopped:
    case plugin_state::cannot_be_used:
        return StatusChip::kind::failed;
    case plugin_state::off:
    case plugin_state::blocked:
    case plugin_state::not_used:
    case plugin_state::needs_newer_studio:
    case plugin_state::not_installed:
        return StatusChip::kind::closed;
    }
    return StatusChip::kind::closed;
}

QString row_accessible_name(const plugin_view& plugin)
{
    return QCoreApplication::translate("PluginsDialog", "%1, %2, %3")
        .arg(plain_text(plugin.name), trust_line(plugin),
             state_text(plugin.state));
}

QString row_second_line(const plugin_view& plugin)
{
    return QCoreApplication::translate("PluginsDialog", "%1, version %2")
        .arg(trust_line(plugin), plain_text(plugin.version));
}

bool switch_usable(const plugin_view& plugin)
{
    switch (plugin.state)
    {
    case plugin_state::available:
    case plugin_state::starting:
    case plugin_state::off:
    case plugin_state::needs_decision:
    case plugin_state::needs_licence:
    case plugin_state::stopped:
        return true;
    case plugin_state::blocked:
    case plugin_state::needs_isolation:
    case plugin_state::not_used:
    case plugin_state::needs_newer_studio:
    case plugin_state::cannot_be_used:
    case plugin_state::not_installed:
        return false;
    }
    return false;
}

bool switch_shown_on(const plugin_view& plugin)
{
    return plugin.state == plugin_state::available ||
           plugin.state == plugin_state::starting ||
           plugin.state == plugin_state::needs_licence ||
           plugin.state == plugin_state::stopped;
}

QString not_available_in_build()
{
    return QCoreApplication::translate("PluginsDialog",
                                       "Not available in this build");
}

QString permission_sentence(const plugin_permission_view& permission)
{
    const QString& id = permission.id;
    if (id == QLatin1String("ports.read"))
        return QCoreApplication::translate(
            "PluginsDialog",
            "See your ports and links, their state and their counters.");
    if (id == QLatin1String("traffic.tap"))
        return QCoreApplication::translate(
            "PluginsDialog",
            "Read the data that passes through the ports and links you use it "
            "on.");
    if (id == QLatin1String("ports.write"))
        return QCoreApplication::translate(
            "PluginsDialog", "Send data from the ports you use it on.");
    if (id == QLatin1String("topology.modify"))
        return QCoreApplication::translate(
            "PluginsDialog", "Add, remove and connect ports and links.");
    if (id == QLatin1String("modifier.inline"))
        return QCoreApplication::translate(
            "PluginsDialog",
            "Hold, change or drop data on the links you use it on.");
    if (id == QLatin1String("faults"))
        return QCoreApplication::translate(
            "PluginsDialog",
            "Cause line errors and disconnections, and set timed replies.");
    if (id == QLatin1String("serial.devices"))
        return QCoreApplication::translate(
            "PluginsDialog",
            "See the list of serial devices on this computer.");
    if (id == QLatin1String("licence.read"))
        return QCoreApplication::translate(
            "PluginsDialog",
            "Read your licence file. The file holds your name.");
    if (id == QLatin1String("fs.read"))
        return QCoreApplication::translate(
            "PluginsDialog", "Read files in a folder you choose.");
    if (id == QLatin1String("fs.write"))
        return QCoreApplication::translate(
            "PluginsDialog", "Write files in a folder you choose.");
    if (id == QLatin1String("net.listen"))
    {
        const QString address = plain_text(permission.scope.join(
            QCoreApplication::translate("PluginsDialog", ", ")));
        bool loopback = !permission.scope.isEmpty();
        for (const QString& each : permission.scope)
        {
            const QString text = plain_text(each);
            if (!text.startsWith(QLatin1String("127.")) &&
                !text.startsWith(QLatin1String("localhost")) &&
                !text.startsWith(QLatin1String("[::1]")))
                loopback = false;
        }
        if (loopback)
            return QCoreApplication::translate(
                       "PluginsDialog",
                       "Accept connections from programs on this computer, on "
                       "%1.")
                .arg(address);
        return QCoreApplication::translate("PluginsDialog",
                                           "Accept connections on %1.")
            .arg(address);
    }
    if (id == QLatin1String("net.outbound"))
        return QCoreApplication::translate("PluginsDialog", "Connect to %1.")
            .arg(plain_text(permission.scope.join(
                QCoreApplication::translate("PluginsDialog", ", "))));
    if (id == QLatin1String("process.spawn"))
        return QCoreApplication::translate("PluginsDialog",
                                           "Start other programs.");
    if (id == QLatin1String("bundle.section"))
        return QCoreApplication::translate(
            "PluginsDialog",
            "Add a section to a support bundle. You see it and choose whether "
            "to include it.");
    // A permission this version does not know is shown by its name, as text.
    return plain_text(id);
}

bool is_amber_permission(const QString& id)
{
    return id == QLatin1String("traffic.tap") ||
           id == QLatin1String("modifier.inline") ||
           id == QLatin1String("net.listen") ||
           id == QLatin1String("net.outbound") ||
           id == QLatin1String("fs.write") ||
           id == QLatin1String("process.spawn");
}

QString signature_sentence(const plugin_view& plugin)
{
    switch (plugin.trust)
    {
    case plugin_trust::official:
        return QCoreApplication::translate("PluginsDialog",
                                           "Signed by Virtual Serial Studio.");
    case plugin_trust::third_party:
        return QCoreApplication::translate(
                   "PluginsDialog",
                   "Signed by %1. Not verified by Virtual Serial Studio.")
            .arg(display_author(plugin));
    case plugin_trust::unknown_key:
        return QCoreApplication::translate(
                   "PluginsDialog",
                   "Signed with a key that Studio does not have (%1), so the "
                   "signature could not be checked.")
            .arg(plain_text(plugin.key_id));
    case plugin_trust::unsigned_plugin:
        return QCoreApplication::translate(
            "PluginsDialog",
            "Not signed. Studio cannot tell who made it or whether it was "
            "changed.");
    }
    return {};
}

QString isolation_text(const plugin_view& plugin)
{
    if (plugin.override_active)
    {
        return QCoreApplication::translate("PluginsDialog",
                                           "None. You allowed this on %1.")
            .arg(date_text(plugin.override_on));
    }
    switch (plugin.isolation)
    {
    case plugin_isolation::full:
        return QCoreApplication::translate("PluginsDialog", "Full");
    case plugin_isolation::partial:
        return QCoreApplication::translate("PluginsDialog", "Partial: %1")
            .arg(plain_text(plugin.isolation_missing));
    case plugin_isolation::none:
        return QCoreApplication::translate("PluginsDialog",
                                           "None on this computer");
    }
    return {};
}

QString timing_text(plugin_timing timing)
{
    switch (timing)
    {
    case plugin_timing::by_studio:
        return QCoreApplication::translate("PluginsDialog", "Timed by Studio");
    case plugin_timing::in_path:
        return QCoreApplication::translate("PluginsDialog",
                                           "In the path of the data");
    case plugin_timing::beside:
        return QCoreApplication::translate("PluginsDialog", "Beside the data");
    }
    return {};
}

QString timing_sentence(const plugin_view& plugin)
{
    switch (plugin.timing)
    {
    case plugin_timing::by_studio:
        return QCoreApplication::translate(
            "PluginsDialog",
            "Studio carries out this plugin's rules itself, so its timing is "
            "exact and it keeps working if the plugin stops.");
    case plugin_timing::in_path:
        return QCoreApplication::translate(
            "PluginsDialog",
            "Data on a link waits for this plugin, for up to %n microseconds "
            "each time.",
            nullptr, plugin.budget_us);
    case plugin_timing::beside:
        return QCoreApplication::translate(
            "PluginsDialog",
            "This plugin watches and acts, and data never waits for it.");
    }
    return {};
}

QString installed_for_text(plugin_installed_for installed)
{
    switch (installed)
    {
    case plugin_installed_for::everyone:
        return QCoreApplication::translate("PluginsDialog",
                                           "Everyone on this computer");
    case plugin_installed_for::you_only:
        return QCoreApplication::translate("PluginsDialog", "You only");
    case plugin_installed_for::with_studio:
        return QCoreApplication::translate("PluginsDialog", "Came with Studio");
    }
    return {};
}

QString addition_text(const plugin_addition& addition)
{
    const QString name = plain_text(addition.type_name);
    switch (addition.kind)
    {
    case plugin_addition_kind::fixture:
        return QCoreApplication::translate("PluginsDialog", "Fixture: %1")
            .arg(name);
    case plugin_addition_kind::generator:
        return QCoreApplication::translate("PluginsDialog", "Generator: %1")
            .arg(name);
    case plugin_addition_kind::link_behaviour:
        return QCoreApplication::translate("PluginsDialog",
                                           "Link behaviour: %1")
            .arg(name);
    case plugin_addition_kind::connection:
        return QCoreApplication::translate("PluginsDialog", "Connection: %1")
            .arg(name);
    }
    return {};
}

QString runtime_line(const plugin_view& plugin)
{
    if (plugin.runtime_name.isEmpty())
        return {};
    return QCoreApplication::translate(
               "PluginsDialog",
               "Uses the %1 on this computer. Those files are not covered by "
               "the signature.")
        .arg(plain_text(plugin.runtime_name));
}

QString plugin_mark_text()
{
    return QCoreApplication::translate("PluginsDialog", "Plugin");
}

QString official_badge_text()
{
    return QCoreApplication::translate("PluginsDialog",
                                       "Virtual Serial Studio");
}

QString action_text(plugin_action action)
{
    switch (action)
    {
    case plugin_action::start_now:
        return QCoreApplication::translate("PluginsDialog", "Start now");
    case plugin_action::review_and_decide:
        return QCoreApplication::translate("PluginsDialog",
                                           "Review and decide");
    case plugin_action::show_used_copy:
        return QCoreApplication::translate("PluginsDialog",
                                           "Show the copy that is used");
    case plugin_action::copy_details:
        return QCoreApplication::translate("PluginsDialog", "Copy details");
    case plugin_action::run_without_isolation:
        return QCoreApplication::translate("PluginsDialog",
                                           "Run without isolation...");
    case plugin_action::licence:
        return QCoreApplication::translate("PluginsDialog", "Licence");
    case plugin_action::start_again:
        return QCoreApplication::translate("PluginsDialog", "Start again");
    case plugin_action::show_log:
        return QCoreApplication::translate("PluginsDialog", "Show log");
    }
    return {};
}

plugin_note_text note_for(const plugin_view& plugin)
{
    plugin_note_text out;
    const auto set = [&out](NoteBanner::kind kind, const QString& text,
                            std::vector<plugin_action> actions = {})
    {
        out.shown = true;
        out.kind = kind;
        out.text = text;
        out.actions = std::move(actions);
    };
    using kind = NoteBanner::kind;
    switch (plugin.note)
    {
    case plugin_note::none:
        break;
    case plugin_note::starting:
        set(kind::info,
            QCoreApplication::translate("PluginsDialog", "Starting."),
            {plugin_action::start_now});
        break;
    case plugin_note::backing_off:
        if (plugin.back_off_seconds == 1)
        {
            set(kind::info,
                QCoreApplication::translate(
                    "PluginsDialog",
                    "It stopped unexpectedly. Starting again in 1 second "
                    "(attempt %1 of 5).")
                    .arg(plugin.attempt),
                {plugin_action::start_now});
            break;
        }
        set(kind::info,
            QCoreApplication::translate(
                "PluginsDialog",
                "It stopped unexpectedly. Starting again in %n seconds "
                "(attempt %1 of 5).",
                nullptr, plugin.back_off_seconds)
                .arg(plugin.attempt),
            {plugin_action::start_now});
        break;
    case plugin_note::decision_first:
        set(kind::warn,
            QCoreApplication::translate(
                "PluginsDialog",
                "You have not decided whether to run this plugin."),
            {plugin_action::review_and_decide});
        break;
    case plugin_note::decision_changed:
        set(kind::warn,
            QCoreApplication::translate(
                "PluginsDialog",
                "This plugin changed since you approved it. Review it again "
                "before it runs."),
            {plugin_action::review_and_decide});
        break;
    case plugin_note::blocked_official_only:
        set(kind::info,
            QCoreApplication::translate(
                "PluginsDialog",
                "Your administrator allows only official plugins. This one is "
                "not official."));
        break;
    case plugin_note::blocked_named:
        set(kind::info, QCoreApplication::translate(
                            "PluginsDialog",
                            "Your administrator has blocked this plugin."));
        break;
    case plugin_note::blocked_protection:
        set(kind::info,
            QCoreApplication::translate(
                "PluginsDialog",
                "Your administrator requires more isolation than this computer "
                "can give this plugin."),
            {plugin_action::copy_details});
        break;
    case plugin_note::isolation_no_override:
        set(kind::info,
            QCoreApplication::translate(
                "PluginsDialog",
                "Studio cannot keep this plugin apart from your files and "
                "other programs on this computer, so it will not run. Only "
                "your administrator can allow it."),
            {plugin_action::copy_details});
        break;
    case plugin_note::isolation_override:
        set(kind::warn,
            QCoreApplication::translate(
                "PluginsDialog",
                "Studio cannot keep this plugin apart from your files and "
                "other programs on this computer, so it will not run."),
            {plugin_action::run_without_isolation,
             plugin_action::copy_details});
        break;
    case plugin_note::not_used_same:
        if (plugin.newer_than_used_copy)
        {
            set(kind::info,
                QCoreApplication::translate(
                    "PluginsDialog",
                    "The copy installed for everyone is used. This copy is "
                    "newer (version %1), and it is still not used. Ask your "
                    "administrator to update the copy for everyone.")
                    .arg(plain_text(plugin.version)),
                {plugin_action::show_used_copy});
        }
        else
        {
            set(kind::info,
                QCoreApplication::translate(
                    "PluginsDialog",
                    "The copy installed for everyone is used. This copy, "
                    "installed for you only, is not."),
                {plugin_action::show_used_copy});
        }
        break;
    case plugin_note::not_used_different_signer:
        set(kind::warn,
            QCoreApplication::translate(
                "PluginsDialog",
                "Another plugin with the same identifier is installed, %1. "
                "This one, %2, is not used. They are not the same plugin.")
                .arg(
                    trust_line(plugin.used_copy_trust, plugin.used_copy_author),
                    trust_line(plugin)),
            {plugin_action::show_used_copy, plugin_action::copy_details});
        break;
    case plugin_note::not_used_reserved:
        set(kind::danger,
            QCoreApplication::translate(
                "PluginsDialog",
                "Its identifier is one that only official plugins may use, and "
                "it is not signed by Virtual Serial Studio. It will not run."),
            {plugin_action::copy_details});
        break;
    case plugin_note::needs_licence:
        set(kind::warn,
            QCoreApplication::translate(
                "PluginsDialog",
                "%1 Its settings are kept, and it runs again when a licence "
                "allows it.")
                .arg(reason_sentence(plugin.licence_reason,
                                     plugin.licence_status)),
            {plugin_action::licence});
        break;
    case plugin_note::stopped_failures:
        set(kind::danger,
            QCoreApplication::translate(
                "PluginsDialog",
                "It stopped %n times in 10 minutes, so Studio stopped starting "
                "it.",
                nullptr, plugin.failure_count),
            {plugin_action::start_again, plugin_action::show_log});
        break;
    case plugin_note::stopped_unresponsive:
        set(kind::danger,
            QCoreApplication::translate(
                "PluginsDialog", "It did not answer, so Studio stopped it."),
            {plugin_action::start_again, plugin_action::show_log});
        break;
    case plugin_note::stopped_limit:
        set(kind::danger,
            QCoreApplication::translate(
                "PluginsDialog",
                "It sent more than it is allowed to (%1), so Studio stopped "
                "it.")
                .arg(plain_text(plugin.limit_name)),
            {plugin_action::start_again, plugin_action::show_log});
        break;
    case plugin_note::newer_studio_version:
        set(kind::info,
            QCoreApplication::translate(
                "PluginsDialog",
                "This plugin needs a newer version of Studio. This is version "
                "%1.")
                .arg(plain_text(plugin.studio_version)));
        break;
    case plugin_note::newer_studio_capability:
        set(kind::info,
            QCoreApplication::translate(
                "PluginsDialog",
                "This plugin needs something this build does not have: %1.")
                .arg(plain_text(plugin.capability_name)));
        break;
    case plugin_note::older_plugin:
        set(kind::info,
            QCoreApplication::translate(
                "PluginsDialog",
                "This plugin was made for an older version of Studio. Ask its "
                "author for a newer one."));
        break;
    case plugin_note::invalid_manifest:
        set(kind::danger,
            QCoreApplication::translate(
                "PluginsDialog",
                "Its description file is not valid, so Studio cannot tell what "
                "it is."),
            {plugin_action::copy_details});
        break;
    case plugin_note::invalid_signature:
        set(kind::danger,
            QCoreApplication::translate(
                "PluginsDialog",
                "Its signature does not match its files. They may have been "
                "changed. Get the plugin again from its author."),
            {plugin_action::copy_details});
        break;
    case plugin_note::withdrawn:
        set(kind::danger,
            QCoreApplication::translate(
                "PluginsDialog",
                "This plugin was withdrawn by Virtual Serial Studio. It will "
                "not run."));
        break;
    case plugin_note::expired:
        set(kind::warn,
            QCoreApplication::translate(
                "PluginsDialog",
                "This plugin expired on %1. Look for a newer version.")
                .arg(date_text(plugin.expires)));
        break;
    case plugin_note::not_installed:
        set(kind::info,
            QCoreApplication::translate(
                "PluginsDialog",
                "A setup uses this plugin, and it is not installed on this "
                "computer. Its parts of the setup are kept."));
        break;
    }
    return out;
}

banner_text top_note_text(plugin_banner banner)
{
    banner_text out;
    switch (banner)
    {
    case plugin_banner::none:
        return out;
    case plugin_banner::policy_untrusted:
        out.kind = NoteBanner::kind::warn;
        out.text = QCoreApplication::translate(
            "PluginsDialog",
            "A policy file was found on this computer and could not be "
            "trusted, so only official plugins will run.");
        out.action = action_text(plugin_action::copy_details);
        break;
    case plugin_banner::official_only:
        out.text = QCoreApplication::translate(
            "PluginsDialog",
            "Your administrator allows only official plugins on this "
            "computer.");
        break;
    case plugin_banner::user_plugins_off:
        out.text = QCoreApplication::translate(
            "PluginsDialog",
            "Your administrator installs the plugins on this computer. "
            "Plugins in your own plugin folder are not used.");
        break;
    case plugin_banner::no_licence_for_official:
        out.text = QCoreApplication::translate(
            "PluginsDialog", "Official plugins need a Pro licence.");
        out.action = action_text(plugin_action::licence);
        break;
    }
    out.shown = true;
    return out;
}

QString dialog_title()
{
    return QCoreApplication::translate("PluginsDialog", "Plugins");
}

QString dialog_sub()
{
    return QCoreApplication::translate(
        "PluginsDialog",
        "Plugins add fixtures, generators, link behaviours and connections to "
        "other tools. Each one runs as its own program, apart from Studio.");
}

QString header_chip_text(int on, int attention)
{
    if (attention == 1)
        return QCoreApplication::translate("PluginsDialog",
                                           "1 needs attention");
    if (attention > 1)
        return QCoreApplication::translate("PluginsDialog", "%n need attention",
                                           nullptr, attention);
    return QCoreApplication::translate("PluginsDialog", "%n on", nullptr, on);
}

bool needs_attention(plugin_state state)
{
    return state == plugin_state::needs_decision ||
           state == plugin_state::stopped ||
           state == plugin_state::cannot_be_used;
}

QString copy_details_text(const plugin_view& plugin)
{
    // The plugin's own lines, as text, and nothing of its files or settings.
    QStringList lines;
    const auto add = [&lines](const char* label, const QString& value)
    {
        lines << QStringLiteral("%1: %2").arg(
            QCoreApplication::translate("PluginsDialog", label), value);
    };
    add("Plugin", plain_text(plugin.id));
    add("Version", plain_text(plugin.version));
    add("State", state_text(plugin.state));
    add("Reason code", plain_text(plugin.reason_code));
    add("Fingerprint", plain_text(plugin.fingerprint));
    add("Signer key id", plain_text(plugin.key_id));
    add("Installed for", installed_for_text(plugin.installed_for));
    add("Isolation", isolation_text(plugin));
    add("Message from the host", plain_text(plugin.detail));
    return lines.join(QLatin1Char('\n'));
}

QString decision_title(const plugin_view& plugin)
{
    switch (plugin.trust)
    {
    case plugin_trust::official:
        return QCoreApplication::translate("PluginDecisionDialog",
                                           "Switch on %1?")
            .arg(plain_text(plugin.name));
    case plugin_trust::third_party:
        return QCoreApplication::translate("PluginDecisionDialog",
                                           "Run a plugin from %1?")
            .arg(display_author(plugin));
    case plugin_trust::unknown_key:
    case plugin_trust::unsigned_plugin:
        return QCoreApplication::translate("PluginDecisionDialog",
                                           "Run an unofficial plugin?");
    }
    return {};
}

decision_banner_text decision_banner(const plugin_view& plugin)
{
    decision_banner_text out;
    switch (plugin.trust)
    {
    case plugin_trust::official:
        out.kind = NoteBanner::kind::info;
        out.lines << QCoreApplication::translate("PluginDecisionDialog",
                                                 "Signed by Virtual Serial "
                                                 "Studio.");
        break;
    case plugin_trust::third_party:
        if (!plugin.key_id.isEmpty() && plugin.key_approved_on.isValid())
        {
            out.kind = NoteBanner::kind::info;
            out.lines << QCoreApplication::translate(
                             "PluginDecisionDialog",
                             "Signed by %1, whose plugins you have approved "
                             "before. Not verified by Virtual Serial Studio.")
                             .arg(display_author(plugin));
        }
        else
        {
            out.kind = NoteBanner::kind::warn;
            out.lines << QCoreApplication::translate(
                             "PluginDecisionDialog",
                             "Signed by %1. Not verified by Virtual Serial "
                             "Studio. Only run it if you know who that is.")
                             .arg(display_author(plugin));
        }
        break;
    case plugin_trust::unknown_key:
    case plugin_trust::unsigned_plugin:
        out.kind = NoteBanner::kind::danger;
        out.lines << QCoreApplication::translate(
                         "PluginDecisionDialog",
                         "Unofficial plugin. It can do anything Studio can.")
                  << QCoreApplication::translate(
                         "PluginDecisionDialog",
                         "Studio cannot tell who made it or whether it was "
                         "changed.");
        if (plugin.trust == plugin_trust::unknown_key)
        {
            out.lines << QCoreApplication::translate(
                             "PluginDecisionDialog",
                             "It is signed with a key that Studio does not "
                             "have (%1), so the signature could not be "
                             "checked.")
                             .arg(plain_text(plugin.key_id));
        }
        break;
    }
    return out;
}

QString decision_run_button(const plugin_view& plugin)
{
    switch (plugin.trust)
    {
    case plugin_trust::official:
        return QCoreApplication::translate("PluginDecisionDialog", "Switch on");
    case plugin_trust::third_party:
        return QCoreApplication::translate("PluginDecisionDialog",
                                           "Run this plugin");
    case plugin_trust::unknown_key:
    case plugin_trust::unsigned_plugin:
        return QCoreApplication::translate("PluginDecisionDialog",
                                           "Run anyway");
    }
    return {};
}

decision_isolation_text decision_isolation(const plugin_view& plugin)
{
    decision_isolation_text out;
    switch (plugin.isolation)
    {
    case plugin_isolation::full:
        out.text = QCoreApplication::translate(
            "PluginDecisionDialog",
            "Studio runs it apart from your files and other programs.");
        break;
    case plugin_isolation::partial:
        out.text = QCoreApplication::translate(
                       "PluginDecisionDialog",
                       "Studio can only partly keep it apart on this computer: "
                       "%1.")
                       .arg(plain_text(plugin.isolation_missing));
        break;
    case plugin_isolation::none:
        if (plugin.trust == plugin_trust::official)
        {
            out.text = QCoreApplication::translate(
                "PluginDecisionDialog",
                "Studio cannot keep it apart on this computer. It runs with "
                "your own access to files and the network.");
        }
        else
        {
            out.text = QCoreApplication::translate(
                "PluginDecisionDialog",
                "Studio cannot keep it apart on this computer, so it will not "
                "run here.");
            out.decide_anyway = true;
        }
        break;
    }
    return out;
}

QString no_isolation_title(const plugin_view& plugin)
{
    return QCoreApplication::translate("PluginNoIsolationDialog",
                                       "Run %1 without isolation?")
        .arg(plain_text(plugin.name));
}

} // namespace vss::gui::support
