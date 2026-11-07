//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_BACKEND_VIEW_TYPES_HPP
#define VSS_GUI_BACKEND_VIEW_TYPES_HPP

#include <vss/core/control_lines.hpp>
#include <vss/core/error.hpp>
#include <vss/core/event.hpp>
#include <vss/core/link.hpp>
#include <vss/core/serial_settings.hpp>
#include <vss/core/stats.hpp>
#include <vss/core/types.hpp>

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QVector>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace vss::gui
{

/// What the studio can know about the program on the other side of a port.
///
/// `unknown` is the honest answer for every transport the engine cannot
/// observe today.  The GUI shows "Open" and omits the waiting sentence; it
/// never claims a program has or has not connected.
enum class peer_state
{
    unknown,
    not_open,
    open
};

/// The plain-language shape of a link, derived from the engine's mode,
/// arity and modem-line setting.  Mirrors `studio::snapshot()`.
enum class link_shape
{
    two_way,
    split,
    join,
    one_way,
    monitor,
    /// A shared bus.  The members are `sources`; `targets` is empty.
    bus
};

/// One endpoint as the GUI needs it.  Engine snapshots are converted here,
/// once, so no screen includes a core header for display data.
struct port_view
{
    vss::endpoint_id id = 0;
    QString name;
    QString path;
    QString description;
    vss::endpoint_kind kind = vss::endpoint_kind::virtual_port;
    vss::endpoint_state state = vss::endpoint_state::created;
    vss::serial_settings serial;
    vss::control_lines lines;
    vss::endpoint_stats stats;
    bool supports_control_lines = false;
    bool open = false;
    /// A loopback or mock endpoint: no other program can open it.
    bool studio_only = false;
    peer_state peer = peer_state::unknown;
    /// The baud rate and framing the program on the other side has set, where
    /// the engine can tell (a Windows driver port, a Linux or macOS pseudo
    /// terminal).  Empty means unknown, never "default".
    std::optional<vss::line_format> peer_format;
    /// Connected remote clients of a listening endpoint; empty for any other
    /// kind of port, so "none connected" and "not a listener" differ.
    std::optional<std::size_t> clients;
    /// The names of the ports on a shared link that run at a different baud
    /// rate or framing from this one.  Empty when they all agree or the
    /// formats are not known.
    QStringList format_conflicts;
    /// A port the Windows driver made for this studio.  Its COM number can be
    /// changed, and a program that holds it can be made to let go.
    bool driver_port = false;
    /// The number in the COM name of a driver port, or 0.
    unsigned com_number = 0;
};

/// The program that has a driver port open, as the driver names it.
struct holder_view
{
    quint32 process_id = 0;
    /// The program's file name, or empty when Windows will not say.
    QString process_name;
    /// The program runs as the user who runs Studio.  Studio ends only such a
    /// program, unless it was started as administrator.
    bool own_user = false;
    /// Studio may end the program.
    bool can_end = false;
};

/// How a program is made to let go of a driver port.
enum class release_method
{
    /// Take the port back: the program keeps running and keeps its handle,
    /// which stops working, and another program can open the port.
    take_back,
    /// End the program, as Task Manager's End task does.
    end_program
};

/// One link as the GUI needs it.
struct link_view
{
    vss::link_id id = 0;
    link_shape shape = link_shape::two_way;
    vss::link_mode mode = vss::link_mode::duplex;
    vss::link_options options;
    QVector<vss::endpoint_id> sources;
    QVector<vss::endpoint_id> targets;
    vss::endpoint_id owner = 0;
    vss::link_stats stats;
};

/// Which input lines each output line drives on the port at the other end of a
/// link.
///
/// The engine only has the boolean null-modem crossing
/// (`link_options::emulate_modem_lines`), so `Capabilities::line_matrix` is
/// false and the real adapter neither reads nor writes this.  The type is the
/// adapter interface the line matrix dialog is built and tested against; it does
/// not change the topology JSON schema or `vss_core`.
struct line_wiring
{
    bool dtr_to_dsr = true;
    bool dtr_to_cts = false;
    bool dtr_to_dcd = true;
    bool dtr_to_ri = false;
    bool rts_to_dsr = false;
    bool rts_to_cts = true;
    bool rts_to_dcd = false;
    bool rts_to_ri = false;

    friend bool operator==(const line_wiring&, const line_wiring&) = default;
};

/// One captured chunk of traffic from the engine's bounded ring buffer.
struct traffic_view
{
    vss::endpoint_id endpoint = 0;
    QString name;
    vss::data_direction direction = vss::data_direction::rx;
    QByteArray data;
    vss::time_point when{};
};

/// One serial device, as returned by the platform enumeration.
struct device_view
{
    QString path;
    QString description;
    QString hardware_id;
    bool virtual_device = false;
    bool com0com = false;
    int com0com_pair = -1;
    QString com0com_side;
    /// True when another process holds the device.  The engine cannot prove
    /// this today, so `EngineStudioBackend` always leaves it false; tests and
    /// a future platform probe can set it.
    bool in_use = false;
};

/// Facts the setup check, launcher strip and status chip read.
/// What stands between this PC and a virtual COM port that other programs can
/// open (Windows).  Each value is one thing to fix, and the first four are
/// fixed by the same guided set-up.
enum class driver_problem
{
    /// Nothing is wrong, or this system needs no driver.
    none,
    /// The driver package is not installed.
    driver_missing,
    /// The helper service is not installed.
    helper_missing,
    /// The helper service is installed and not running.
    helper_stopped,
    /// The helper is older than this program, or speaks another version.
    helper_incompatible,
    /// The helper is there and refused this user.
    helper_denied,
    /// Something else, or the answer was not understood.
    unknown
};

/// The answer of a check of the driver set-up.
struct driver_check
{
    driver_problem problem = driver_problem::none;
    /// One English line with what was seen, and the Win32 error behind it; for
    /// the "Copy details" text and the log, never a sentence for the person.
    QString details;

    bool operator==(const driver_check&) const = default;
};

struct setup_facts
{
    /// The Virtual Serial Studio COM port driver (`vsscom` or `vsscom_um`) is
    /// installed.  Studio tries it before com0com, and it makes ports on
    /// demand, so there is no pair count for it.
    bool driver_found = false;
    /// The driver's interface version, or empty when it could not be read.
    QString driver_version;
    bool com0com_found = false;
    int com0com_pairs = 0;
    int serial_devices = 0;
    QString platform;
    /// False until a platform helper can prove elevation; the GUI must not
    /// guess.
    bool elevation_known = false;
    bool elevated = false;
    /// What is missing for the driver (Windows).  `facts()` fills it from a
    /// check that sends the helper one harmless request.
    driver_check driver;
};

/// One selected event from the hub, converted for the GUI thread.
struct gui_event
{
    vss::event_kind kind = vss::event_kind::log_message;
    vss::endpoint_id endpoint = 0;
    QString name;
    vss::data_direction direction = vss::data_direction::rx;
    QByteArray data;
    vss::control_lines lines;
    QString message;
    QString error;
    vss::time_point when{};
};

using event_batch = QVector<gui_event>;

/// The result of `studio::share_serial`, flattened for the share wizard.
struct share_view
{
    vss::endpoint_id source = 0;
    QVector<vss::endpoint_id> listeners;
    vss::link_id link = 0;
};

/// One-line rendering of an engine error, used by dialogs and the status bar.
QString error_text(const vss::error_code& error);

} // namespace vss::gui

Q_DECLARE_METATYPE(vss::gui::gui_event)

#endif // VSS_GUI_BACKEND_VIEW_TYPES_HPP
