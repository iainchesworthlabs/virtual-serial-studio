//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/engine_studio_backend.hpp"

#include "backend/com_numbers.hpp"
#include "backend/event_bridge.hpp"
#include "backend/platform/com0com_platform.hpp"

#include <vss/core/hub.hpp>
#include <vss/core/profile.hpp>
#include <vss/ports/com0com.hpp>
#include <vss/ports/driver_setup.hpp>
#include <vss/ports/peer_open.hpp>
#include <vss/ports/permissions.hpp>
#include <vss/ports/process_control.hpp>
#include <vss/ports/vcom_driver.hpp>
#include <vss/studio/studio.hpp>

#include <QMetaObject>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace vss::gui
{
namespace
{

QVector<vss::endpoint_id> to_vector(const std::vector<vss::endpoint_id>& ids)
{
    QVector<vss::endpoint_id> out;
    out.reserve(static_cast<qsizetype>(ids.size()));
    for (const vss::endpoint_id id : ids)
        out.push_back(id);
    return out;
}

peer_state to_peer_state(vss::peer_open_state state)
{
    switch (state)
    {
    case vss::peer_open_state::open:
        return peer_state::open;
    case vss::peer_open_state::not_open:
        return peer_state::not_open;
    case vss::peer_open_state::unknown:
        break;
    }
    return peer_state::unknown;
}

/// The number in a COM name such as `COM7`, or 0.
unsigned com_number_of(const std::string& name)
{
    if (name.size() < 4 || name.compare(0, 3, "COM") != 0)
        return 0;
    unsigned number = 0;
    for (std::size_t index = 3; index < name.size(); ++index)
    {
        if (name[index] < '0' || name[index] > '9' || number > 1000)
            return 0;
        number = number * 10 + static_cast<unsigned>(name[index] - '0');
    }
    return number;
}

/// Whether the Windows driver is installed.  The answer needs a walk of the
/// device list and the window asks about once a second, so it is looked at no
/// more often than every few seconds.
bool driver_installed()
{
    using clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static clock::time_point checked{};
    static bool installed = false;

    const std::lock_guard lock(mutex);
    const auto now = clock::now();
    if (checked == clock::time_point{} ||
        now - checked > std::chrono::seconds(5))
    {
        installed = vss::driver_available();
        checked = now;
    }
    return installed;
}

port_view make_port(
    const vss::endpoint_info& info,
    const std::unordered_map<vss::endpoint_id, vss::peer_open_state>& peers,
    const std::unordered_map<vss::endpoint_id, std::string>& drivers)
{
    port_view view;
    view.id = info.id;
    view.name = QString::fromStdString(info.name);
    view.path = QString::fromStdString(info.path);
    view.description = QString::fromStdString(info.description);
    view.kind = info.kind;
    view.state = info.state;
    view.serial = info.serial;
    view.lines = info.lines;
    view.stats = info.stats;
    view.supports_control_lines = info.supports_control_lines;
    view.open = info.open;
    view.peer_format = info.peer_format;
    view.clients = info.clients;
    view.studio_only = info.kind == vss::endpoint_kind::mock ||
                       view.path.startsWith(QStringLiteral("loopback:"));
    // Only a pseudo terminal on a platform that can inspect other processes has
    // a definite answer.  Every other port stays unknown, and the GUI must
    // not claim what it cannot know.
    if (const auto found = peers.find(info.id); found != peers.end())
        view.peer = to_peer_state(found->second);
    if (const auto found = drivers.find(info.id); found != drivers.end())
    {
        view.driver_port = true;
        view.com_number = com_number_of(found->second);
    }
    return view;
}

link_view make_link(const vss::link_info& info)
{
    link_view view;
    view.id = info.id;
    view.mode = info.mode;
    view.options = info.options;
    view.sources = to_vector(info.sources);
    view.targets = to_vector(info.targets);
    view.owner = info.owner;
    view.stats = info.stats;

    if (info.mode == vss::link_mode::bus)
    {
        view.shape = link_shape::bus;
    }
    else if (info.mode == vss::link_mode::duplex)
    {
        view.shape = info.sources.size() == 1 && info.targets.size() == 1
                         ? link_shape::two_way
                         : link_shape::split;
    }
    else if (info.sources.size() > 1)
    {
        // Many sources are a join with or without modem lines; a tap on a
        // bus is a join without them.
        view.shape = link_shape::join;
    }
    else
    {
        view.shape = info.options.emulate_modem_lines ? link_shape::one_way
                                                      : link_shape::monitor;
    }
    return view;
}

/// Everything about a port except its traffic counters.
bool same_port_shape(const port_view& left, const port_view& right)
{
    return left.id == right.id && left.name == right.name &&
           left.path == right.path && left.description == right.description &&
           left.kind == right.kind && left.state == right.state &&
           left.lines == right.lines && left.open == right.open &&
           left.supports_control_lines == right.supports_control_lines &&
           left.studio_only == right.studio_only && left.peer == right.peer &&
           left.peer_format == right.peer_format &&
           left.clients == right.clients &&
           left.format_conflicts == right.format_conflicts &&
           left.driver_port == right.driver_port &&
           left.com_number == right.com_number;
}

bool same_port(const port_view& left, const port_view& right)
{
    return same_port_shape(left, right) &&
           left.stats.bytes_rx == right.stats.bytes_rx &&
           left.stats.bytes_tx == right.stats.bytes_tx &&
           left.stats.read_ops == right.stats.read_ops &&
           left.stats.write_ops == right.stats.write_ops &&
           left.stats.errors == right.stats.errors &&
           left.stats.dropped == right.stats.dropped;
}

/// Everything about a link except its traffic counters.
bool same_link_shape(const link_view& left, const link_view& right)
{
    return left.id == right.id && left.shape == right.shape &&
           left.mode == right.mode &&
           left.options.policy == right.options.policy &&
           left.options.emulate_modem_lines ==
               right.options.emulate_modem_lines &&
           left.sources == right.sources && left.targets == right.targets &&
           left.owner == right.owner;
}

bool same_link(const link_view& left, const link_view& right)
{
    return same_link_shape(left, right) &&
           left.stats.bytes == right.stats.bytes &&
           left.stats.messages == right.stats.messages &&
           left.stats.dropped == right.stats.dropped;
}

QString platform_name()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

} // namespace

EngineStudioBackend::EngineStudioBackend(vss::studio& studio, QObject* parent)
    : StudioBackend(parent), studio_(studio),
      bridge_(std::make_unique<EventBridge>(studio.engine()))
{
    connect(bridge_.get(), &EventBridge::batch_ready, this,
            [this](const event_batch& events)
            {
                VSS_PROFILE_ZONE("gui.backend.batch");
                for (const auto& event : events)
                {
                    if (event.kind == vss::event_kind::lines)
                        Q_EMIT lines_changed(event.endpoint);
                }
                {
                    VSS_PROFILE_ZONE("gui.backend.emit_batch_ready");
                    Q_EMIT batch_ready(events);
                }
                schedule_rebuild();
            });
    bridge_->start();

    // Opening a pseudo terminal raises no engine event, so a program that
    // connects or goes away is noticed by looking again once a second.
    if (vss::peer_open_supported())
    {
        peer_timer_ = new QTimer(this);
        peer_timer_->setInterval(1000);
        connect(peer_timer_, &QTimer::timeout, this, [this] { rebuild(); });
        peer_timer_->start();
    }
    rebuild();
}

EngineStudioBackend::~EngineStudioBackend()
{
    bridge_->stop();
}

capabilities EngineStudioBackend::caps() const
{
    capabilities caps = capabilities{};
    // Boost.VirtualSerial's network backends only connect. Waiting for other
    // computers is `vss_ports`' listen transport (`tcp-listen://`,
    // `udp-listen://` and `rfc2217-listen://`), which
    // `vss::studio::attach_serial` opens.
    caps.network_server = true;
    // `vss::run_benchmark` is the core `vss_cli bench` uses, shared with the
    // GUI.
    caps.benchmark = true;
    // `vss::studio::inject_fault` reaches the mock fixture behind a port.
    caps.faults = true;
    // `vss::studio::set_fixture_echo` sends every byte written to a fixture
    // back as if the device had sent it.
    caps.fixture_echo = true;
    // A pseudo terminal on Linux can be checked for a program that has it
    // open; every other port reads as unknown and the GUI says nothing.
    caps.peer_open = vss::peer_open_supported();
    caps.line_format = true;
    // The Studio driver takes a COM number when a port is made or afterwards,
    // and its revoke request takes a port back from a program.
    caps.com_numbers = driver_installed();
    caps.release_port = driver_installed();
    caps.recreate_ports = true;
    // The window's Undo history reverses changes with exact inverse
    // operations (see `shell/undo_steps.cpp`), not by restoring a snapshot.
    caps.undo = true;
    // `vss::hub::set_link_wiring` holds the wires of every link.
    caps.line_matrix = true;
    // `vss::hub::bus` is a shared multi-drop link, in the JSON as "bus".
    caps.shared_bus = true;
    // The JSON editor places a schema error on its line with a path index of
    // the text, and asks `vss::config::from_json` which field it refused.
    caps.json_positions = true;
    // `vss::studio::rename_port` renames the endpoint, the studio record
    // and a pseudo terminal's stable link together.
    caps.rename_port = true;
    // `vss::studio::reconfigure_port` gives a port new serial settings.
    caps.port_settings = true;
    caps.com0com_pair_creation =
        detail::platform_supports_com0com_pair_creation();
    // The rule is made by the elevated command-line helper; the platform must
    // have the step and be able to start a program through its own prompt.
    caps.firewall_rule = vss::permissions::can_manage_firewall_rules() &&
                         vss::permissions::can_run_elevated();
    caps.serial_group_step = vss::permissions::can_add_user_to_group();
    return caps;
}

vss::result<vss::benchmark_result> EngineStudioBackend::run_benchmark(
    vss::endpoint_id from, vss::endpoint_id to,
    const vss::benchmark_options& options,
    const vss::benchmark_progress_handler& progress,
    const std::atomic<bool>* cancel)
{
    // Runs on a worker thread: it uses only the hub's lookup, which is safe
    // from any thread, and holds the endpoints so a removal cannot free them.
    const auto sender = studio_.engine().find_endpoint(from);
    const auto receiver = studio_.engine().find_endpoint(to);
    if (!sender || !receiver)
        return vss::make_error_code(vss::errc::not_found);
    return vss::run_benchmark(*sender, *receiver, options, progress, cancel);
}

std::optional<line_wiring>
EngineStudioBackend::link_wiring(vss::link_id id) const
{
    const auto info = studio_.engine().find_link(id);
    if (!info)
        return std::nullopt;
    const vss::modem_wiring& wires = info->options.wiring;
    line_wiring out;
    out.dtr_to_dsr = wires.dtr_to_dsr;
    out.dtr_to_cts = wires.dtr_to_cts;
    out.dtr_to_dcd = wires.dtr_to_dcd;
    out.dtr_to_ri = wires.dtr_to_ri;
    out.rts_to_dsr = wires.rts_to_dsr;
    out.rts_to_cts = wires.rts_to_cts;
    out.rts_to_dcd = wires.rts_to_dcd;
    out.rts_to_ri = wires.rts_to_ri;
    return out;
}

vss::result<void>
EngineStudioBackend::set_link_wiring(vss::link_id id, const line_wiring& wiring)
{
    vss::modem_wiring wires;
    wires.dtr_to_dsr = wiring.dtr_to_dsr;
    wires.dtr_to_cts = wiring.dtr_to_cts;
    wires.dtr_to_dcd = wiring.dtr_to_dcd;
    wires.dtr_to_ri = wiring.dtr_to_ri;
    wires.rts_to_dsr = wiring.rts_to_dsr;
    wires.rts_to_cts = wiring.rts_to_cts;
    wires.rts_to_dcd = wiring.rts_to_dcd;
    wires.rts_to_ri = wiring.rts_to_ri;
    const auto changed = studio_.engine().set_link_wiring(id, wires);
    if (!changed)
        return changed.error();
    rebuild();
    return {};
}

vss::result<void> EngineStudioBackend::inject_fault(vss::endpoint_id id,
                                                    vss::fault_kind fault)
{
    return studio_.inject_fault(id, fault);
}

vss::result<void> EngineStudioBackend::set_echo(vss::endpoint_id id, bool on)
{
    return studio_.set_fixture_echo(id, on);
}

bool EngineStudioBackend::echo(vss::endpoint_id id) const
{
    return studio_.fixture_echo(id);
}

vss::result<vss::endpoint_id> EngineStudioBackend::create_virtual_port(
    const QString& name, vss::virtual_port_kind kind, bool capture)
{
    vss::endpoint_options options = studio_.default_options();
    options.capture_traffic = capture;
    const auto created = create_port_with_com_number(name, kind, options);
    if (!created)
        return created.error();
    rebuild();
    return (*created)->id();
}

vss::result<std::shared_ptr<vss::endpoint>>
EngineStudioBackend::create_port_with_com_number(
    const QString& name, vss::virtual_port_kind kind,
    const vss::endpoint_options& options)
{
    const std::string port_name = name.toStdString();
    const bool driver_port = kind == vss::virtual_port_kind::pty ||
                             kind == vss::virtual_port_kind::windows_driver;
    if (!driver_port || !driver_installed())
        return studio_.create_virtual_port(port_name, kind, options);

    // Ask the driver for a number that is not COM1 or COM2 and is above every
    // port the computer already has, so a program that was set up for an
    // on-board port does not find ours, and a port here does not take the
    // number of one that is only unplugged.  Windows keeps names in its COM
    // name database that no list of ports shows; the driver refuses those with
    // `already_exists` and makes no port, so the next number is tried.
    std::vector<std::string> paths;
    for (const auto& device : studio_.serial_devices())
        paths.push_back(device.path);
    for (const auto& info : studio_.endpoint_infos())
        paths.push_back(info.path);
    const std::vector<unsigned> in_use = com_numbers_in_use(paths);

    const unsigned start = first_com_candidate(in_use);
    unsigned candidate = start;
    while (candidate != 0)
    {
        auto created =
            studio_.create_virtual_port(port_name, kind, options, candidate);
        if (created)
            return created;
        if (created.error() != vss::errc::already_exists)
        {
            // No helper service to give a number: take the one Windows picks.
            if (created.error() == vss::errc::unsupported)
                break;
            return created;
        }
        candidate = next_com_candidate(candidate, start, in_use);
    }
    return studio_.create_virtual_port(port_name, kind, options);
}

vss::result<vss::endpoint_id>
EngineStudioBackend::create_auto_port(const QString& name, bool capture)
{
    // `studio::create_virtual_port` treats pty as the auto selector and falls
    // back to the best available kind on platforms without a pseudo
    // terminal.  This is the documented auto path, not a simulation.
    return create_virtual_port(name, vss::virtual_port_kind::pty, capture);
}

vss::result<vss::endpoint_id>
EngineStudioBackend::create_mock_port(const QString& name,
                                      const vss::serial_settings& settings,
                                      const QString& target)
{
    const auto created = studio_.create_mock_port(name.toStdString(), settings,
                                                  target.toStdString());
    if (!created)
        return created.error();
    rebuild();
    return (*created)->id();
}

vss::result<vss::endpoint_id>
EngineStudioBackend::attach_serial(const QString& name, const QString& uri,
                                   const vss::serial_settings& settings,
                                   bool capture)
{
    vss::endpoint_options options = studio_.default_options();
    options.capture_traffic = capture;
    const auto attached = studio_.attach_serial(
        name.toStdString(), uri.toStdString(), settings, options);
    if (!attached)
        return attached.error();
    rebuild();
    return (*attached)->id();
}

vss::result<void> EngineStudioBackend::remove_port(vss::endpoint_id id)
{
    const auto removed = studio_.remove_port(id);
    if (!removed)
        return removed.error();
    rebuild();
    return {};
}

vss::result<vss::endpoint_id>
EngineStudioBackend::create_driver_port(const QString& name, bool capture,
                                        unsigned com_number)
{
    vss::endpoint_options options = studio_.default_options();
    options.capture_traffic = capture;
    const auto created = studio_.create_virtual_port(
        name.toStdString(), vss::virtual_port_kind::windows_driver, options,
        com_number);
    if (!created)
        return created.error();
    rebuild();
    return (*created)->id();
}

vss::result<QString> EngineStudioBackend::set_com_number(vss::endpoint_id id,
                                                         unsigned com_number)
{
    // Runs on a worker thread: the studio call is safe from any thread, and
    // nothing here touches the snapshots.
    const auto renumbered = studio_.set_com_number(id, com_number);
    if (!renumbered)
        return renumbered.error();
    return QString::fromStdString(*renumbered);
}

std::optional<holder_view>
EngineStudioBackend::port_holder(vss::endpoint_id id) const
{
    const auto peer = studio_.driver_holder(id);
    if (!peer || !peer->open || peer->process_id == 0)
        return std::nullopt;
    holder_view view;
    view.process_id = peer->process_id;
    if (const auto process = vss::describe_process(peer->process_id))
    {
        view.process_name = QString::fromStdString(process->name);
        view.own_user = process->own_user;
        view.can_end = process->can_end;
    }
    return view;
}

vss::result<void> EngineStudioBackend::release_port(vss::endpoint_id id,
                                                    release_method method)
{
    if (method == release_method::take_back)
    {
        const auto taken = studio_.revoke_port(id);
        if (!taken)
            return taken.error();
        rebuild();
        return {};
    }

    const auto peer = studio_.driver_holder(id);
    if (!peer || !peer->open || peer->process_id == 0)
        return vss::make_error_code(vss::errc::not_found);
    const auto ended = vss::terminate_process(peer->process_id);
    if (!ended)
        return ended.error();
    rebuild();
    return {};
}

vss::result<void> EngineStudioBackend::rename_port(vss::endpoint_id id,
                                                   const QString& name)
{
    const auto renamed = studio_.rename_port(id, name.toStdString());
    if (!renamed)
        return renamed.error();
    rebuild();
    return {};
}

vss::result<void>
EngineStudioBackend::set_port_settings(vss::endpoint_id id,
                                       const vss::serial_settings& settings)
{
    // Called from a worker thread: the caller refreshes on the GUI thread.
    return studio_.reconfigure_port(id, settings);
}

std::vector<vss::endpoint_id> EngineStudioBackend::lost_ports() const
{
    return studio_.lost_ports();
}

vss::result<std::size_t> EngineStudioBackend::recreate_lost_ports()
{
    // Runs on a worker thread: the studio is safe to call from any thread, and
    // the refresh that shows the result is the caller's, on the GUI thread.
    const auto report = studio_.recreate_lost_ports();
    if (!report.failed.empty())
        return report.failed.front().second;
    return report.recreated.size();
}

std::vector<device_view> EngineStudioBackend::devices() const
{
    std::vector<device_view> out;
    for (const auto& device : studio_.serial_devices())
    {
        device_view view;
        view.path = QString::fromStdString(device.path);
        view.description = QString::fromStdString(device.description);
        view.hardware_id = QString::fromStdString(device.hardware_id);
        // A port made by the Studio driver is a virtual port, not a device.
        // Its hardware id starts with VSSCOM\ (VSSCOM\PORT, VSSCOM\VSSCOM0).
        view.virtual_device =
            device.virtual_device ||
            view.hardware_id.startsWith(QStringLiteral("VSSCOM\\"),
                                        Qt::CaseInsensitive);
        view.com0com = device.com0com;
        view.com0com_pair = device.com0com_pair;
        view.com0com_side = QString::fromStdString(device.com0com_side);
        out.push_back(std::move(view));
    }
    return out;
}

std::vector<traffic_view> EngineStudioBackend::traffic(int limit) const
{
    std::vector<traffic_view> out;
    const auto records =
        studio_.engine().traffic(static_cast<std::size_t>(std::max(limit, 0)));
    out.reserve(records.size());
    for (const auto& record : records)
    {
        traffic_view view;
        view.endpoint = record.endpoint;
        view.name = QString::fromStdString(record.name);
        view.direction = record.direction;
        view.when = record.when;
        if (record.data)
        {
            view.data =
                QByteArray(reinterpret_cast<const char*>(record.data->data()),
                           static_cast<qsizetype>(record.data->size()));
        }
        out.push_back(std::move(view));
    }
    return out;
}

driver_check EngineStudioBackend::check_driver() const
{
    VSS_PROFILE_ZONE("gui.setup.facts.driver_check");
    const vss::driver_setup_report report = vss::probe_driver_setup();
    driver_check check;
    switch (report.state)
    {
    case vss::driver_setup_state::driver_missing:
        check.problem = driver_problem::driver_missing;
        break;
    case vss::driver_setup_state::helper_missing:
        check.problem = driver_problem::helper_missing;
        break;
    case vss::driver_setup_state::helper_stopped:
        check.problem = driver_problem::helper_stopped;
        break;
    case vss::driver_setup_state::helper_incompatible:
        check.problem = driver_problem::helper_incompatible;
        break;
    case vss::driver_setup_state::helper_denied:
        check.problem = driver_problem::helper_denied;
        break;
    case vss::driver_setup_state::unknown:
        check.problem = driver_problem::unknown;
        break;
    case vss::driver_setup_state::not_applicable:
    case vss::driver_setup_state::ready:
        break;
    }
    check.details = QString::fromStdString(report.detail);
    return check;
}

setup_facts EngineStudioBackend::facts() const
{
    setup_facts facts;
    {
        VSS_PROFILE_ZONE("gui.setup.facts.driver");
        facts.driver_found = vss::driver_available();
        if (facts.driver_found)
            facts.driver_version =
                QString::fromStdString(vss::driver_version());
    }
    {
        VSS_PROFILE_ZONE("gui.setup.facts.com0com");
        const auto installation = studio_.detect_com0com();
        facts.com0com_found = installation.found;
        facts.com0com_pairs = static_cast<int>(installation.pairs.size());
    }
    {
        VSS_PROFILE_ZONE("gui.setup.facts.serial_devices");
        facts.serial_devices =
            static_cast<int>(studio_.serial_devices().size());
    }
    facts.driver = check_driver();
    facts.platform = platform_name();
    // No platform helper proves elevation yet.  Leave it unknown rather than
    // telling the user something the build cannot check.
    facts.elevation_known = false;
    facts.elevated = false;
    return facts;
}

std::optional<port_view> EngineStudioBackend::port(vss::endpoint_id id) const
{
    const auto it =
        std::find_if(ports_.begin(), ports_.end(),
                     [id](const port_view& view) { return view.id == id; });
    if (it == ports_.end())
        return std::nullopt;
    return *it;
}

std::optional<link_view> EngineStudioBackend::link(vss::link_id id) const
{
    const auto it =
        std::find_if(links_.begin(), links_.end(),
                     [id](const link_view& view) { return view.id == id; });
    if (it == links_.end())
        return std::nullopt;
    return *it;
}

vss::result<vss::link_id>
EngineStudioBackend::connect_ports(vss::endpoint_id first,
                                   vss::endpoint_id second,
                                   const vss::link_options& options)
{
    const auto linked = studio_.engine().connect(first, second, options);
    if (!linked)
        return linked.error();
    rebuild();
    return *linked;
}

vss::result<vss::link_id>
EngineStudioBackend::split(vss::endpoint_id source,
                           const std::vector<vss::endpoint_id>& targets,
                           const vss::link_options& options)
{
    const auto linked = studio_.engine().split(source, targets, options);
    if (!linked)
        return linked.error();
    rebuild();
    return *linked;
}

vss::result<vss::link_id>
EngineStudioBackend::join(const std::vector<vss::endpoint_id>& sources,
                          vss::endpoint_id target,
                          const vss::link_options& options)
{
    const auto linked = studio_.engine().join(sources, target, options);
    if (!linked)
        return linked.error();
    rebuild();
    return *linked;
}

vss::result<vss::link_id>
EngineStudioBackend::redirect(vss::endpoint_id from, vss::endpoint_id to,
                              const vss::link_options& options)
{
    const auto linked = studio_.engine().redirect(from, to, options);
    if (!linked)
        return linked.error();
    rebuild();
    return *linked;
}

vss::result<vss::link_id> EngineStudioBackend::monitor(vss::endpoint_id from,
                                                       vss::endpoint_id to)
{
    const auto linked = studio_.engine().monitor(from, to);
    if (!linked)
        return linked.error();
    rebuild();
    return *linked;
}

vss::result<vss::link_id>
EngineStudioBackend::bus(const std::vector<vss::endpoint_id>& members)
{
    const auto linked = studio_.engine().bus(members);
    if (!linked)
        return linked.error();
    rebuild();
    return *linked;
}

vss::result<void> EngineStudioBackend::disconnect(vss::link_id id)
{
    const auto removed = studio_.engine().disconnect(id);
    if (!removed)
        return removed.error();
    rebuild();
    return {};
}

vss::result<void> EngineStudioBackend::set_owner(vss::link_id id,
                                                 vss::endpoint_id owner)
{
    const auto changed = studio_.engine().set_owner(id, owner);
    if (!changed)
        return changed.error();
    rebuild();
    return {};
}

vss::result<void> EngineStudioBackend::release_owner(vss::link_id id)
{
    const auto changed = studio_.engine().release_owner(id);
    if (!changed)
        return changed.error();
    rebuild();
    return {};
}

vss::result<void> EngineStudioBackend::send(vss::endpoint_id id,
                                            const QByteArray& data)
{
    const auto endpoint = studio_.engine().find_endpoint(id);
    if (!endpoint)
        return vss::make_error_code(vss::errc::not_found);
    if (!endpoint->is_open())
        return vss::make_error_code(vss::errc::not_open);

    const vss::bytes_view bytes(
        reinterpret_cast<const vss::byte*>(data.constData()),
        static_cast<std::size_t>(data.size()));
    // A mock fixture is a pretend device.  Writing to it hands the bytes to
    // the device, which only records them, so "send from the fixture" is the
    // device speaking: the bytes arrive on the fixture and the hub carries
    // them along its links.
    if (endpoint->kind() == vss::endpoint_kind::mock)
        return studio_.inject_data(id, bytes);
    // A queue-full rejection is published as a hub error event and reaches
    // the GUI through the coalesced batch.  There is deliberately no
    // per-write callback into the backend: it would have to touch a QObject
    // from the endpoint strand.
    endpoint->write(bytes);
    return {};
}

vss::result<void> EngineStudioBackend::set_line(vss::endpoint_id id,
                                                vss::control_line line,
                                                bool state)
{
    const auto endpoint = studio_.engine().find_endpoint(id);
    if (!endpoint)
        return vss::make_error_code(vss::errc::not_found);
    if (!endpoint->info().supports_control_lines)
        return vss::make_error_code(vss::errc::unsupported);
    if (const auto error = endpoint->set_line(line, state))
        return error;
    Q_EMIT lines_changed(id);
    return {};
}

void EngineStudioBackend::refresh_lines(vss::endpoint_id id)
{
    const auto endpoint = studio_.engine().find_endpoint(id);
    if (!endpoint)
        return;
    endpoint->refresh_lines();
    Q_EMIT lines_changed(id);
}

vss::config::document EngineStudioBackend::snapshot() const
{
    return studio_.snapshot();
}

vss::result<void>
EngineStudioBackend::apply(const vss::config::document& document,
                           bool rollback_on_error)
{
    const auto applied = studio_.apply(document, rollback_on_error);
    if (!applied)
        return applied.error();
    rebuild();
    return {};
}

vss::result<share_view> EngineStudioBackend::share_serial(const QString& uri,
                                                          int listeners)
{
    const auto shared = studio_.share_serial(uri.toStdString(), listeners);
    if (!shared)
        return shared.error();

    share_view view;
    view.source = shared->source;
    view.link = shared->link;
    view.listeners.reserve(static_cast<qsizetype>(shared->listeners.size()));
    for (const vss::endpoint_id id : shared->listeners)
        view.listeners.push_back(id);
    rebuild();
    return view;
}

vss::result<vss::link_id> EngineStudioBackend::bridge(const QString& from_uri,
                                                      const QString& to_uri)
{
    const auto linked =
        studio_.bridge(from_uri.toStdString(), to_uri.toStdString());
    if (!linked)
        return linked.error();
    rebuild();
    return *linked;
}

vss::result<void> EngineStudioBackend::create_com0com_pair()
{
    return detail::platform_create_com0com_pair(studio_);
}

bool EngineStudioBackend::hub_running() const
{
    return studio_.engine().running();
}

void EngineStudioBackend::refresh()
{
    rebuild();
}

void EngineStudioBackend::set_refresh_interval(int milliseconds)
{
    bridge_->set_interval(milliseconds);
}

void EngineStudioBackend::schedule_rebuild()
{
    if (rebuild_scheduled_)
        return;
    rebuild_scheduled_ = true;
    QTimer::singleShot(50, this,
                       [this]
                       {
                           rebuild_scheduled_ = false;
                           rebuild();
                       });
}

void EngineStudioBackend::rebuild()
{
    VSS_PROFILE_ZONE("gui.backend.rebuild");
    std::vector<port_view> next_ports;
    const auto infos = studio_.endpoint_infos();
    next_ports.reserve(infos.size());
    std::unordered_map<vss::endpoint_id, vss::peer_open_state> peers;
    if (vss::peer_open_supported())
    {
        VSS_PROFILE_ZONE("gui.backend.peer_states");
        peers = studio_.peer_states();
    }
    const auto drivers = studio_.driver_ports();
    for (const auto& info : infos)
    {
        next_ports.push_back(make_port(info, peers, drivers));
        // The endpoint keeps the settings it was made with; the studio keeps
        // the ones the user gave it since.
        if (const auto settings = studio_.port_settings(info.id))
            next_ports.back().serial = *settings;
    }

    std::vector<link_view> next_links;
    const auto links = studio_.link_infos();

    // Name, on each port, the others on its links that run at another speed.
    for (const auto& mismatch : vss::find_format_mismatches(infos, links))
    {
        for (const auto& member : mismatch.members)
        {
            const auto port =
                std::find_if(next_ports.begin(), next_ports.end(),
                             [&member](const port_view& view)
                             { return view.id == member.endpoint; });
            if (port == next_ports.end())
                continue;
            for (const auto& other : mismatch.members)
            {
                if (vss::same_format(other.format, member.format))
                    continue;
                const auto peer =
                    std::find_if(next_ports.begin(), next_ports.end(),
                                 [&other](const port_view& view)
                                 { return view.id == other.endpoint; });
                if (peer != next_ports.end() &&
                    !port->format_conflicts.contains(peer->name))
                    port->format_conflicts << peer->name;
            }
        }
    }
    next_links.reserve(links.size());
    for (const auto& info : links)
        next_links.push_back(make_link(info));

    const bool ports_differ = next_ports.size() != ports_.size() ||
                              !std::equal(next_ports.begin(), next_ports.end(),
                                          ports_.begin(), same_port_shape);
    const bool links_differ = next_links.size() != links_.size() ||
                              !std::equal(next_links.begin(), next_links.end(),
                                          links_.begin(), same_link_shape);
    // Counters move with every byte; they are a signal of their own so a
    // window does not rebuild itself for a number.
    const bool stats_differ = !ports_differ && !links_differ &&
                              (!std::equal(next_ports.begin(), next_ports.end(),
                                           ports_.begin(), same_port) ||
                               !std::equal(next_links.begin(), next_links.end(),
                                           links_.begin(), same_link));

    ports_ = std::move(next_ports);
    links_ = std::move(next_links);

    if (ports_differ)
    {
        VSS_PROFILE_ZONE("gui.backend.emit_ports_changed");
        Q_EMIT ports_changed();
    }
    if (links_differ)
    {
        VSS_PROFILE_ZONE("gui.backend.emit_links_changed");
        Q_EMIT links_changed();
    }
    if (stats_differ)
    {
        VSS_PROFILE_ZONE("gui.backend.emit_stats_changed");
        Q_EMIT stats_changed();
    }
}

} // namespace vss::gui
