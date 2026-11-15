//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/fake_plugin_backend.hpp"

#include <QCoreApplication>
#include <QEvent>

#include <algorithm>
#include <utility>

namespace vss::gui
{

FakePluginBackend::FakePluginBackend(QObject* parent)
    : PluginBackend(parent), snapshot_(std::make_shared<plugin_snapshot>())
{
    snapshot_->machine_folder = QStringLiteral("C:/ProgramData/Studio/plugins");
    snapshot_->user_folder = QStringLiteral("C:/Users/you/Studio/plugins");
}

void FakePluginBackend::record(const fake_plugin_request& request)
{
    requests_.push_back(request);
}

plugin_view* FakePluginBackend::find(const QString& plugin_id)
{
    for (plugin_view& each : snapshot_->plugins)
    {
        if (each.id == plugin_id)
            return &each;
    }
    return nullptr;
}

void FakePluginBackend::settle(plugin_view& plugin) const
{
    // Only the states a switch decides. Every other state is the host's.
    if (plugin.state != plugin_state::off &&
        plugin.state != plugin_state::available)
        return;
    plugin.state = plugin.wished_on && snapshot_->can_run_plugins
                       ? plugin_state::available
                       : plugin_state::off;
}

void FakePluginBackend::set_snapshot(plugin_snapshot snapshot)
{
    snapshot_ = std::make_shared<plugin_snapshot>(std::move(snapshot));
    notify_snapshot_changed();
}

void FakePluginBackend::set_plugins(std::vector<plugin_view> plugins)
{
    auto next = std::make_shared<plugin_snapshot>(*snapshot_);
    next->plugins = std::move(plugins);
    snapshot_ = std::move(next);
    notify_snapshot_changed();
}

bool FakePluginBackend::change(const QString& plugin_id,
                               const std::function<void(plugin_view&)>& edit)
{
    // A new snapshot each time: a screen may still hold the old one.
    auto next = std::make_shared<plugin_snapshot>(*snapshot_);
    snapshot_ = next;
    plugin_view* target = find(plugin_id);
    if (target == nullptr)
        return false;
    edit(*target);
    notify_snapshot_changed();
    return true;
}

void FakePluginBackend::set_key_file(const QString& path,
                                     plugin_key_file_view key)
{
    key.path = path;
    key_files_.insert(path, std::move(key));
}

void FakePluginBackend::set_inspection(const QString& folder,
                                       plugin_inspection_view found)
{
    found.folder = folder;
    inspections_.insert(folder, std::move(found));
}

quint64 FakePluginBackend::inspect_plugin(const QString& folder)
{
    const quint64 number = next_request_++;
    // Looking is not a change: it is recorded apart from the requests that
    // change something.
    inspected_.push_back(folder);
    plugin_inspection_view answer;
    const auto found = inspections_.constFind(folder);
    if (found != inspections_.constEnd())
        answer = found.value();
    else
        answer.folder = folder;
    notify_plugin_inspected(number, answer);
    return number;
}

void FakePluginBackend::fail_next_add(plugin_request_error error,
                                      const QString& plugin_name)
{
    add_error_ = error;
    add_error_name_ = plugin_name;
}

int FakePluginBackend::count_of(plugin_request_kind kind) const
{
    return static_cast<int>(std::count_if(
        requests_.begin(), requests_.end(),
        [kind](const fake_plugin_request& each) { return each.kind == kind; }));
}

void FakePluginBackend::flush()
{
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
}

void FakePluginBackend::decide(const plugin_decision_request& request)
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::decide;
    seen.plugin_id = request.plugin_id;
    seen.hash = request.hash;
    seen.key_id = request.key_id;
    seen.granted = request.granted;
    seen.flag = request.switch_on;
    record(seen);

    auto next = std::make_shared<plugin_snapshot>(*snapshot_);
    snapshot_ = next;
    if (plugin_view* target = find(request.plugin_id))
    {
        if (target->hash == request.hash &&
            target->state == plugin_state::needs_decision)
        {
            target->state = plugin_state::off;
            target->note = plugin_note::none;
            target->wished_on = request.switch_on;
            target->differs_from_approved = false;
            for (plugin_permission_view& permission : target->permissions)
            {
                permission.turned_down =
                    !permission.required &&
                    !request.granted.contains(permission.id);
                permission.is_new = false;
            }
            settle(*target);
        }
    }
    notify_snapshot_changed();
    plugin_request_result result;
    result.kind = plugin_request_kind::decide;
    result.plugin_id = request.plugin_id;
    notify_request_finished(result);
}

void FakePluginBackend::set_enabled(const QString& plugin_id,
                                    const QString& hash, bool on)
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::set_enabled;
    seen.plugin_id = plugin_id;
    seen.hash = hash;
    seen.flag = on;
    record(seen);
    change(plugin_id,
           [this, on](plugin_view& plugin)
           {
               plugin.wished_on = on;
               settle(plugin);
           });
}

quint64 FakePluginBackend::read_key_file(const QString& path)
{
    const quint64 number = next_request_++;
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::approve_key;
    seen.path = path;
    seen.flag = false;
    // Reading is not a request that changes anything; it is not recorded as
    // an approval. A test looks at `key_files` through the answer.
    plugin_key_file_view answer;
    const auto found = key_files_.constFind(path);
    if (found != key_files_.constEnd())
        answer = found.value();
    else
    {
        answer.failure = plugin_key_file_view::problem::not_a_key_file;
        answer.path = path;
    }
    notify_key_file_read(number, answer);
    return number;
}

void FakePluginBackend::approve_key(const QString& path,
                                    const QString& fingerprint)
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::approve_key;
    seen.path = path;
    seen.fingerprint = fingerprint;
    seen.flag = true;
    record(seen);
    plugin_request_result result;
    result.kind = plugin_request_kind::approve_key;
    const auto found = key_files_.constFind(path);
    if (found != key_files_.constEnd())
        result.author = found->author;
    notify_request_finished(result);
}

void FakePluginBackend::remove_key(const QString& key_id)
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::remove_key;
    seen.key_id = key_id;
    record(seen);
    plugin_request_result result;
    result.kind = plugin_request_kind::remove_key;
    notify_request_finished(result);
}

void FakePluginBackend::set_no_isolation(const QString& plugin_id,
                                         const QString& hash, bool on)
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::set_no_isolation;
    seen.plugin_id = plugin_id;
    seen.hash = hash;
    seen.flag = on;
    record(seen);
    change(plugin_id,
           [on](plugin_view& plugin)
           {
               plugin.override_active = on;
               if (on)
                   plugin.override_on = QDate(2026, 11, 15);
           });
}

void FakePluginBackend::start_again(const QString& plugin_id)
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::start_again;
    seen.plugin_id = plugin_id;
    record(seen);
    change(plugin_id,
           [this](plugin_view& plugin)
           {
               plugin.state = snapshot_->can_run_plugins
                                  ? plugin_state::starting
                                  : plugin_state::off;
               plugin.note = plugin_note::none;
           });
}

void FakePluginBackend::run_expired(const QString& plugin_id,
                                    const QString& hash)
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::run_expired;
    seen.plugin_id = plugin_id;
    seen.hash = hash;
    record(seen);
}

void FakePluginBackend::rescan()
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::rescan;
    record(seen);
    plugin_request_result result;
    result.kind = plugin_request_kind::rescan;
    result.count = static_cast<int>(snapshot_->plugins.size());
    notify_request_finished(result);
}

void FakePluginBackend::add_plugin(const QString& folder)
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::add_plugin;
    seen.path = folder;
    record(seen);
    plugin_request_result result;
    result.kind = plugin_request_kind::add_plugin;
    result.error = add_error_;
    result.plugin_name = add_error_name_;
    add_error_ = plugin_request_error::none;
    notify_request_finished(result);
}

void FakePluginBackend::remove_plugin(const QString& plugin_id,
                                      const QString& hash,
                                      bool delete_saved_data)
{
    fake_plugin_request seen;
    seen.kind = plugin_request_kind::remove_plugin;
    seen.plugin_id = plugin_id;
    seen.hash = hash;
    seen.flag = delete_saved_data;
    record(seen);
    auto next = std::make_shared<plugin_snapshot>(*snapshot_);
    std::erase_if(next->plugins, [&](const plugin_view& each)
                  { return each.id == plugin_id && each.hash == hash; });
    snapshot_ = std::move(next);
    notify_snapshot_changed();
    plugin_request_result result;
    result.kind = plugin_request_kind::remove_plugin;
    result.plugin_id = plugin_id;
    notify_request_finished(result);
}

// -- samples ------------------------------------------------------------------

namespace
{

plugin_permission_view permission(const QString& id, bool required,
                                  const QString& reason = {},
                                  const QStringList& scope = {})
{
    plugin_permission_view view;
    view.id = id;
    view.required = required;
    view.reason = reason;
    view.scope = scope;
    return view;
}

plugin_view base(const QString& id, const QString& name, plugin_state state,
                 plugin_trust trust)
{
    plugin_view view;
    view.id = id;
    view.name = name;
    view.version = QStringLiteral("1.2.0");
    view.state = state;
    view.trust = trust;
    view.description = QStringLiteral("Answers like a device on the bus.");
    view.installed_for = plugin_installed_for::you_only;
    view.hash = QStringLiteral("0123456789abcdef") +
                QString::number(qHash(id), 16)
                    .rightJustified(16, QLatin1Char('0'))
                    .repeated(7);
    view.fingerprint = view.hash.left(16);
    view.folder = QStringLiteral("C:/Users/you/Studio/plugins/") + name;
    view.isolation = plugin_isolation::none;
    view.adds.push_back(
        {plugin_addition_kind::fixture, name + QStringLiteral(" device")});
    view.permissions.push_back(permission(QStringLiteral("ports.read"), true));
    switch (trust)
    {
    case plugin_trust::official:
        view.author = QStringLiteral("Virtual Serial Studio");
        view.installed_for = plugin_installed_for::with_studio;
        view.key_id = QStringLiteral("plugins-2026");
        break;
    case plugin_trust::third_party:
        view.author = QStringLiteral("Example Instruments");
        view.key_id = QStringLiteral("example-1");
        view.key_name = QStringLiteral("Example Instruments");
        view.key_approved_on = QDate(2026, 10, 2);
        break;
    case plugin_trust::unknown_key:
        view.author = QStringLiteral("Unknown Maker");
        view.key_id = QStringLiteral("unknown-7");
        break;
    case plugin_trust::unsigned_plugin:
        view.author = QStringLiteral("A. Hobbyist");
        break;
    }
    return view;
}

} // namespace

std::vector<plugin_view> fake_plugin_samples()
{
    std::vector<plugin_view> list;

    plugin_view available =
        base(QStringLiteral("com.virtualserialstudio.modbus"),
             QStringLiteral("Modbus fixture"), plugin_state::available,
             plugin_trust::official);
    available.wished_on = true;
    available.isolation = plugin_isolation::full;
    available.timing = plugin_timing::by_studio;
    available.permissions.push_back(
        permission(QStringLiteral("traffic.tap"), false,
                   QStringLiteral("To show frames in the decoder.")));
    available.used_in.push_back(
        {QStringLiteral("Modbus device on COM7"), QStringLiteral("port:7")});
    for (int line = 0; line < 3; ++line)
        available.log.push_back(
            {QDateTime(QDate(2026, 11, 15), QTime(9, 30, line)),
             QStringLiteral("Answered request %1").arg(line)});
    list.push_back(available);

    plugin_view starting =
        base(QStringLiteral("org.example.replay"), QStringLiteral("Replay"),
             plugin_state::starting, plugin_trust::third_party);
    starting.wished_on = true;
    starting.note = plugin_note::backing_off;
    starting.back_off_seconds = 4;
    starting.attempt = 3;
    starting.reason_code = QStringLiteral("start_failed");
    starting.isolation = plugin_isolation::partial;
    starting.isolation_missing = QStringLiteral("No network isolation");
    starting.timing = plugin_timing::beside;
    list.push_back(starting);

    plugin_view off =
        base(QStringLiteral("org.example.noise"), QStringLiteral("Line noise"),
             plugin_state::off, plugin_trust::third_party);
    off.timing = plugin_timing::in_path;
    off.budget_us = 800;
    off.isolation = plugin_isolation::full;
    off.permissions.push_back(permission(QStringLiteral("modifier.inline"),
                                         true,
                                         QStringLiteral("To corrupt bytes.")));
    off.permissions.push_back(permission(QStringLiteral("net.listen"), false,
                                         {},
                                         {QStringLiteral("127.0.0.1:5020")}));
    off.permissions.push_back(permission(QStringLiteral("net.outbound"), false,
                                         {}, {QStringLiteral("example.org")}));
    off.adds.push_back(
        {plugin_addition_kind::link_behaviour, QStringLiteral("Noisy line")});
    list.push_back(off);

    plugin_view decision = base(
        QStringLiteral("org.hobby.scanner"), QStringLiteral("Barcode scanner"),
        plugin_state::needs_decision, plugin_trust::unsigned_plugin);
    decision.note = plugin_note::decision_first;
    decision.reason_code = QStringLiteral("untrusted");
    decision.isolation = plugin_isolation::partial;
    decision.isolation_missing = QStringLiteral("No network isolation");
    decision.runtime_name = QStringLiteral("python3");
    decision.permissions.push_back(permission(
        QStringLiteral("fs.write"), false, QStringLiteral("To save a log.")));
    decision.permissions.push_back(
        permission(QStringLiteral("process.spawn"), true));
    list.push_back(decision);

    plugin_view changed = base(
        QStringLiteral("org.example.changed"), QStringLiteral("Changed plugin"),
        plugin_state::needs_decision, plugin_trust::third_party);
    changed.note = plugin_note::decision_changed;
    changed.reason_code = QStringLiteral("changed");
    changed.differs_from_approved = true;
    changed.isolation = plugin_isolation::full;
    changed.permissions.push_back(
        permission(QStringLiteral("net.outbound"), false,
                   QStringLiteral("To check for updates."),
                   {QStringLiteral("updates.example.org")}));
    changed.permissions.back().is_new = true;
    list.push_back(changed);

    plugin_view unknown =
        base(QStringLiteral("org.unknown.thing"),
             QStringLiteral("Unknown maker tool"), plugin_state::needs_decision,
             plugin_trust::unknown_key);
    unknown.note = plugin_note::decision_first;
    unknown.reason_code = QStringLiteral("key_unknown");
    unknown.isolation = plugin_isolation::full;
    list.push_back(unknown);

    plugin_view blocked = base(
        QStringLiteral("org.example.blocked"), QStringLiteral("Blocked tool"),
        plugin_state::blocked, plugin_trust::unsigned_plugin);
    blocked.note = plugin_note::blocked_official_only;
    blocked.reason_code = QStringLiteral("policy");
    blocked.installed_for = plugin_installed_for::everyone;
    list.push_back(blocked);

    plugin_view named =
        base(QStringLiteral("org.example.named"), QStringLiteral("Named tool"),
             plugin_state::blocked, plugin_trust::third_party);
    named.note = plugin_note::blocked_named;
    named.reason_code = QStringLiteral("policy");
    named.installed_for = plugin_installed_for::everyone;
    list.push_back(named);

    plugin_view protection = base(
        QStringLiteral("org.example.protect"), QStringLiteral("Protected tool"),
        plugin_state::blocked, plugin_trust::third_party);
    protection.note = plugin_note::blocked_protection;
    protection.reason_code = QStringLiteral("sandbox_insufficient");
    protection.installed_for = plugin_installed_for::everyone;
    list.push_back(protection);

    plugin_view isolation = base(
        QStringLiteral("org.example.isolate"), QStringLiteral("Loop tester"),
        plugin_state::needs_isolation, plugin_trust::third_party);
    isolation.note = plugin_note::isolation_override;
    isolation.reason_code = QStringLiteral("sandbox_insufficient");
    isolation.override_allowed = true;
    list.push_back(isolation);

    plugin_view no_override = base(
        QStringLiteral("org.example.isolate2"), QStringLiteral("Shared tester"),
        plugin_state::blocked, plugin_trust::third_party);
    no_override.note = plugin_note::isolation_no_override;
    no_override.reason_code = QStringLiteral("sandbox_insufficient");
    no_override.installed_for = plugin_installed_for::everyone;
    list.push_back(no_override);

    plugin_view unused =
        base(QStringLiteral("com.virtualserialstudio.modbus"),
             QStringLiteral("Modbus fixture"), plugin_state::not_used,
             plugin_trust::unsigned_plugin);
    unused.note = plugin_note::not_used_different_signer;
    unused.reason_code = QStringLiteral("shadowed");
    unused.version = QStringLiteral("1.3.0");
    unused.used_copy_folder =
        QStringLiteral("C:/Program Files/Studio/studio-plugins/modbus");
    unused.used_copy_trust = plugin_trust::official;
    unused.used_copy_id = available.id;
    unused.used_copy_folder = available.folder;
    unused.folder = QStringLiteral(
        "C:/Users/you/Studio/plugins/Modbus fixture (your copy)");
    unused.hash = QStringLiteral("fedcba9876543210") + unused.hash.mid(16);
    unused.fingerprint = unused.hash.left(16);
    list.push_back(unused);

    plugin_view reserved =
        base(QStringLiteral("com.virtualserialstudio.fake"),
             QStringLiteral("Pretend official"), plugin_state::not_used,
             plugin_trust::unsigned_plugin);
    reserved.note = plugin_note::not_used_reserved;
    reserved.reason_code = QStringLiteral("reserved_id");
    list.push_back(reserved);

    plugin_view licence =
        base(QStringLiteral("com.virtualserialstudio.decode"),
             QStringLiteral("Protocol decoding"), plugin_state::needs_licence,
             plugin_trust::official);
    licence.note = plugin_note::needs_licence;
    licence.reason_code = QStringLiteral("not_licensed");
    licence.wished_on = true;
    licence.licence_reason = vss::license::reason::no_licence;
    licence.isolation = plugin_isolation::full;
    list.push_back(licence);

    plugin_view stopped =
        base(QStringLiteral("org.example.crashy"), QStringLiteral("Crashy"),
             plugin_state::stopped, plugin_trust::third_party);
    stopped.note = plugin_note::stopped_failures;
    stopped.failure_count = 5;
    stopped.reason_code = QStringLiteral("quarantined");
    stopped.wished_on = true;
    stopped.isolation = plugin_isolation::full;
    stopped.log.push_back({QDateTime(QDate(2026, 11, 15), QTime(9, 30, 0)),
                           QStringLiteral("Segmentation fault")});
    list.push_back(stopped);

    plugin_view newer = base(
        QStringLiteral("org.example.future"), QStringLiteral("Future plugin"),
        plugin_state::needs_newer_studio, plugin_trust::third_party);
    newer.note = plugin_note::newer_studio_version;
    newer.studio_version = QStringLiteral("0.1.0");
    newer.reason_code = QStringLiteral("api_incompatible");
    list.push_back(newer);

    plugin_view broken = base(
        QStringLiteral("org.example.broken"), QStringLiteral("Broken plugin"),
        plugin_state::cannot_be_used, plugin_trust::unsigned_plugin);
    broken.note = plugin_note::invalid_signature;
    broken.reason_code = QStringLiteral("signature_invalid");
    list.push_back(broken);

    plugin_view expired = base(
        QStringLiteral("org.example.expired"), QStringLiteral("Expired plugin"),
        plugin_state::cannot_be_used, plugin_trust::third_party);
    expired.note = plugin_note::expired;
    expired.expires = QDate(2026, 6, 30);
    expired.can_run_expired = true;
    expired.reason_code = QStringLiteral("expired");
    list.push_back(expired);

    plugin_view withdrawn =
        base(QStringLiteral("org.example.withdrawn"),
             QStringLiteral("Withdrawn plugin"), plugin_state::cannot_be_used,
             plugin_trust::third_party);
    withdrawn.note = plugin_note::withdrawn;
    withdrawn.reason_code = QStringLiteral("revoked");
    list.push_back(withdrawn);

    plugin_view invalid =
        base(QStringLiteral("org.example.invalid"),
             QStringLiteral("Invalid description"),
             plugin_state::cannot_be_used, plugin_trust::unsigned_plugin);
    invalid.note = plugin_note::invalid_manifest;
    invalid.reason_code = QStringLiteral("manifest_invalid");
    list.push_back(invalid);

    plugin_view missing = base(
        QStringLiteral("org.example.missing"), QStringLiteral("Missing plugin"),
        plugin_state::not_installed, plugin_trust::unsigned_plugin);
    missing.note = plugin_note::not_installed;
    missing.folder.clear();
    missing.fingerprint.clear();
    missing.hash.clear();
    list.push_back(missing);

    return list;
}

plugin_view fake_hostile_plugin()
{
    plugin_view view =
        base(QStringLiteral("org.evil.hostile"), QStringLiteral("Hostile"),
             plugin_state::off, plugin_trust::unsigned_plugin);
    const QString bidi = QStringLiteral("\u202e\u2066\u200f");
    view.name = QStringLiteral("Virtual Serial Studio\u0007") + bidi +
                QStringLiteral(" <b>bold</b> %1 %2 ") +
                QString(300, QLatin1Char('W'));
    view.author = QStringLiteral("<a href=\"https://example.invalid\">Virtual "
                                 "Serial Studio</a>\r\n") +
                  bidi;
    view.description = QStringLiteral("<h1>Big</h1><img src=\"x\"/>\u001b[31m");
    view.permissions.push_back(permission(
        QStringLiteral("fs.read"), false,
        QStringLiteral(
            "<b>Trust me</b>\u202e. Safe. Verified by Virtual Serial "
            "Studio.")));
    view.log.push_back(
        {QDateTime(QDate(2026, 11, 15), QTime(9, 0, 0)),
         QStringLiteral("\u001b[2J<script>alert(1)</script>") + bidi});
    view.detail = QStringLiteral("<b>detail</b>\u202e");
    return view;
}

} // namespace vss::gui
