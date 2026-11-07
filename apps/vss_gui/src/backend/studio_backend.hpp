//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_BACKEND_STUDIO_BACKEND_HPP
#define VSS_GUI_BACKEND_STUDIO_BACKEND_HPP

#include "backend/capabilities.hpp"
#include "backend/view_types.hpp"

#include <vss/config/document.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/error.hpp>
#include <vss/core/serial_settings.hpp>
#include <vss/core/types.hpp>
#include <vss/ports/virtual_port.hpp>
#include <vss/studio/benchmark.hpp>
#include <vss/studio/studio.hpp>

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <optional>
#include <vector>

namespace vss::gui
{

/// The one narrow interface between every screen and the engine.
///
/// Screens call this, never `vss::studio` directly.  The real implementation
/// (`EngineStudioBackend`) wraps a studio; `FakeStudioBackend` is an
/// in-memory test double.  All signals are emitted on the thread that owns
/// the backend (the GUI thread).
class StudioBackend : public QObject
{
    Q_OBJECT

public:
    explicit StudioBackend(QObject* parent = nullptr);
    ~StudioBackend() override;

    /// What this build can do.  Constant for the lifetime of the backend.
    virtual capabilities caps() const = 0;

    // -- ports -------------------------------------------------------------

    virtual vss::result<vss::endpoint_id>
    create_virtual_port(const QString& name, vss::virtual_port_kind kind,
                        bool capture) = 0;

    /// The "Best available" kind.  The studio treats `pty` as its auto
    /// selector on platforms without a pseudo terminal and falls back to the
    /// best available port, so this is the real auto path.
    virtual vss::result<vss::endpoint_id> create_auto_port(const QString& name,
                                                           bool capture) = 0;

    virtual vss::result<vss::endpoint_id>
    create_mock_port(const QString& name, const vss::serial_settings& settings,
                     const QString& target) = 0;

    virtual vss::result<vss::endpoint_id>
    attach_serial(const QString& name, const QString& uri,
                  const vss::serial_settings& settings, bool capture) = 0;

    virtual vss::result<void> remove_port(vss::endpoint_id id) = 0;

    // -- Windows driver ports (capability: com_numbers, release_port) ------

    /// Makes a port of the Windows driver with the COM number `com_number`
    /// (1 to 255; 0 takes whichever Windows picks).  The default is
    /// `unsupported`.  A number that another device has fails with
    /// `errc::already_exists`, and a number with no helper service to give it
    /// with `errc::unsupported`.
    virtual vss::result<vss::endpoint_id>
    create_driver_port(const QString& /*name*/, bool /*capture*/,
                       unsigned /*com_number*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    /// Gives a driver port another COM number and returns the new COM name.
    /// The port restarts, which takes a second or two, so the call belongs on
    /// a worker thread and touches no GUI state; call `refresh()` on the GUI
    /// thread afterwards.  Errors: see `vss::set_driver_com_number`.  The
    /// default is `unsupported`.
    virtual vss::result<QString> set_com_number(vss::endpoint_id /*id*/,
                                                unsigned /*com_number*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    /// The program that has the driver port open, or none.
    virtual std::optional<holder_view>
    port_holder(vss::endpoint_id /*id*/) const
    {
        return std::nullopt;
    }

    /// Makes the program that has a driver port open let go of it.  The
    /// default is `unsupported`.
    virtual vss::result<void> release_port(vss::endpoint_id /*id*/,
                                           release_method /*method*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    /// Renames a port everywhere the studio keeps its name.  The default is
    /// `unsupported`: a build without `Capabilities::rename_port` never
    /// changes only part of the records.
    virtual vss::result<void> rename_port(vss::endpoint_id /*id*/,
                                          const QString& /*name*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    /// Gives a port new serial settings and keeps its name, id and links
    /// (capability: port_settings).  A fixture or a device is opened again
    /// with them; a virtual port only saves them, because the program that
    /// opens it sets its own speed.  It can block while a device opens, so a
    /// dialog calls it from a worker thread and calls `refresh()` afterwards.
    /// The default is `unsupported`.
    virtual vss::result<void>
    set_port_settings(vss::endpoint_id /*id*/,
                      const vss::serial_settings& /*settings*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    // -- snapshots ---------------------------------------------------------

    virtual std::vector<port_view> ports() const = 0;
    virtual std::vector<link_view> links() const = 0;
    virtual std::vector<device_view> devices() const = 0;
    /// The engine's bounded traffic ring, oldest first.  A limit of zero
    /// returns everything retained.  Used by the diagnostic bundle only when
    /// the user opts in to traffic contents.
    virtual std::vector<traffic_view> traffic(int limit) const = 0;
    /// Virtual ports on this machine that another program made and this
    /// studio does not hold (a Windows driver port of a `vss_cli` process, for
    /// example).  The engine cannot attach to them or read their traffic, so
    /// they never appear in `ports()`; the window lists them only to say so.
    /// Empty when `Capabilities::external_ports` is on, because then `ports()`
    /// already has them.  Reads the device list, so call it when a screen
    /// opens or on a slow timer, not for every event.
    virtual std::vector<device_view> other_program_ports() const;
    virtual setup_facts facts() const = 0;

    /// A fresh check of the driver set-up: the driver package, the helper
    /// service and this user's access to it.  The answer of `facts()` may be a
    /// few seconds old; a failed "add port" asks this, so the message names the
    /// piece that is wrong now.  Makes no port.  The default reads `facts()`.
    virtual driver_check check_driver() const { return facts().driver; }

    /// How long the answer of `facts()` and the platform probes behind the
    /// setup report may be reused.  A real backend asks the operating system
    /// (the driver, com0com, the serial devices, the firewall), which takes
    /// tens of milliseconds, so the window must not ask for every event. Zero,
    /// the default, means always ask; the in-memory fake keeps it so a test
    /// sees a change at once.
    virtual std::chrono::milliseconds probe_cache_age() const { return {}; }
    virtual std::optional<port_view> port(vss::endpoint_id id) const = 0;
    virtual std::optional<link_view> link(vss::link_id id) const = 0;

    // -- links -------------------------------------------------------------

    virtual vss::result<vss::link_id>
    connect_ports(vss::endpoint_id first, vss::endpoint_id second,
                  const vss::link_options& options) = 0;
    virtual vss::result<vss::link_id>
    split(vss::endpoint_id source, const std::vector<vss::endpoint_id>& targets,
          const vss::link_options& options) = 0;
    virtual vss::result<vss::link_id>
    join(const std::vector<vss::endpoint_id>& sources, vss::endpoint_id target,
         const vss::link_options& options) = 0;
    virtual vss::result<vss::link_id>
    redirect(vss::endpoint_id from, vss::endpoint_id to,
             const vss::link_options& options) = 0;
    virtual vss::result<vss::link_id> monitor(vss::endpoint_id from,
                                              vss::endpoint_id to) = 0;
    /// Puts the ports on one shared multi-drop bus (capability:
    /// shared_bus).  The default is `unsupported`: a build without the
    /// capability never fakes a bus out of other links.
    virtual vss::result<vss::link_id>
    bus(const std::vector<vss::endpoint_id>& /*members*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }
    virtual vss::result<void> disconnect(vss::link_id id) = 0;
    virtual vss::result<void> set_owner(vss::link_id id,
                                        vss::endpoint_id owner) = 0;
    virtual vss::result<void> release_owner(vss::link_id id) = 0;

    // -- traffic and control lines ----------------------------------------

    /// Queues bytes for an open endpoint.  Completion arrives as
    /// `send_finished` on the GUI thread.
    virtual vss::result<void> send(vss::endpoint_id id,
                                   const QByteArray& data) = 0;
    virtual vss::result<void> set_line(vss::endpoint_id id,
                                       vss::control_line line, bool state) = 0;
    virtual void refresh_lines(vss::endpoint_id id) = 0;

    // -- per-link line wiring (capability: line_matrix) --------------------

    /// The wiring the engine holds for a link.  Empty when the adapter cannot
    /// report one, which is every real build today.
    virtual std::optional<line_wiring> link_wiring(vss::link_id /*id*/) const
    {
        return std::nullopt;
    }

    /// Applies a wiring change to an existing link.  The default is
    /// `unsupported`: the engine only has the boolean null-modem crossing, and
    /// a build without `Capabilities::line_matrix` must never simulate it.
    virtual vss::result<void> set_link_wiring(vss::link_id /*id*/,
                                              const line_wiring& /*wiring*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    // -- benchmark (capability: benchmark) ---------------------------------

    /// Sends a test payload from one port and counts what arrives on another.
    ///
    /// The call blocks until the run ends, so it belongs on a worker thread;
    /// `progress` and the checks of `cancel` happen on that thread.  It touches
    /// no GUI state.  The default is `unsupported`: a build without
    /// `Capabilities::benchmark` never invents a result.
    virtual vss::result<vss::benchmark_result>
    run_benchmark(vss::endpoint_id /*from*/, vss::endpoint_id /*to*/,
                  const vss::benchmark_options& /*options*/,
                  const vss::benchmark_progress_handler& /*progress*/,
                  const std::atomic<bool>* /*cancel*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    // -- fault injection (capability: faults) ------------------------------

    /// Makes a mock fixture report `fault`.  The default is `unsupported`: a
    /// build without `Capabilities::faults` must never fake an injection.
    virtual vss::result<void> inject_fault(vss::endpoint_id /*id*/,
                                           vss::fault_kind /*fault*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    // -- lost ports (capability: recreate_ports) ----------------------------

    /// The ports of the Windows driver that were taken away under the programs
    /// that had them, because the helper service or the driver restarted.  They
    /// are still in the topology, stopped.  Empty everywhere else.
    virtual std::vector<vss::endpoint_id> lost_ports() const { return {}; }

    /// Makes the lost ports again, each with the COM number it asked for, and
    /// keeps their links.  Blocks for a second or two a port, so it belongs on
    /// a worker thread.  Returns how many were made; an error when any could
    /// not be, and then the others have still been made.  The default is
    /// `unsupported`: a build without `Capabilities::recreate_ports` never
    /// pretends.
    virtual vss::result<std::size_t> recreate_lost_ports()
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    // -- fixture echo (capability: fixture_echo) ---------------------------

    /// Makes a mock fixture write back every byte it receives, or stops it.
    /// The default is `unsupported`: a build without
    /// `Capabilities::fixture_echo` never fakes an echo.
    virtual vss::result<void> set_echo(vss::endpoint_id /*id*/, bool /*on*/)
    {
        return vss::make_error_code(vss::errc::unsupported);
    }

    /// True while the fixture echoes.
    virtual bool echo(vss::endpoint_id /*id*/) const { return false; }

    // -- documents ---------------------------------------------------------

    virtual vss::config::document snapshot() const = 0;
    virtual vss::result<void> apply(const vss::config::document& document,
                                    bool rollback_on_error) = 0;

    // -- scenarios ---------------------------------------------------------

    virtual vss::result<share_view> share_serial(const QString& uri,
                                                 int listeners) = 0;
    virtual vss::result<vss::link_id> bridge(const QString& from_uri,
                                             const QString& to_uri) = 0;
    /// Creates one more com0com null-modem pair, prompting for elevation on
    /// Windows.  Returns `errc::unsupported` where the platform has no driver.
    virtual vss::result<void> create_com0com_pair() = 0;

    // -- lifecycle ---------------------------------------------------------

    /// True while the engine hub is running.  A display fact, not a feature
    /// capability: the GUI only reports it.
    virtual bool hub_running() const = 0;

    /// Re-reads the engine snapshots and emits the changed signals.
    virtual void refresh() = 0;

    /// Coalescing interval for engine events, in milliseconds.
    virtual void set_refresh_interval(int milliseconds) = 0;

Q_SIGNALS:
    /// A port was added, removed, renamed, opened or closed, or anything
    /// about it other than its traffic counters changed.
    void ports_changed();
    /// A link was added, removed or changed, other than its counters.
    void links_changed();
    /// Only traffic counters changed: bytes, operations, errors, drops.  Under
    /// load this fires with every batch of engine events, so a listener must
    /// do no more than redraw the numbers.  A change of anything else emits
    /// `ports_changed` or `links_changed` and the counters come with it.
    void stats_changed();
    void lines_changed(vss::endpoint_id id);
    /// One coalesced batch of engine events, on the GUI thread.
    void batch_ready(const vss::gui::event_batch& events);
};

} // namespace vss::gui

#endif // VSS_GUI_BACKEND_STUDIO_BACKEND_HPP
