//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_STUDIO_STUDIO_HPP
#define VSS_STUDIO_STUDIO_HPP

#include <vss/config/document.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/core/hub.hpp>
#include <vss/ports/com0com.hpp>
#include <vss/ports/peer_open.hpp>
#include <vss/ports/serial_enumeration.hpp>
#include <vss/ports/serial_transport.hpp>
#include <vss/ports/stable_link.hpp>
#include <vss/ports/vcom_driver.hpp>
#include <vss/ports/virtual_port.hpp>
#include <vss/studio/format_check.hpp>
#include <vss/studio/reconnect.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vss
{

class reconnector;

/// A fault a mock fixture can raise.  Each one is reported as an error on the
/// fixture's next read, which stops the endpoint and emits an error event.
enum class fault_kind
{
    parity,
    framing,
    overrun,
    break_condition,
    hangup,
    disconnect,
};

struct studio_options
{
    hub_options hub;
    /// Start the hub's worker threads immediately.
    bool start_hub = true;
    /// Capture traffic in the ring buffer by default.
    bool capture = true;
    /// Give every pseudo terminal a stable name (a symbolic link named after
    /// the port) and advertise that name instead of `/dev/pts/N`.  Linux and
    /// macOS only; creating a port fails if the link cannot be made.
    bool stable_links = false;
    /// Directory for the links.  Empty means `resolve_link_directory`.
    std::string stable_dir;
};

/// The application service shared by the command line tool and the GUI.
///
/// A studio owns a #hub, remembers where every endpoint came from (so the
/// topology can be saved and reloaded) and offers the composite operations
/// the UI is built from: create a virtual port, share a real one, join many
/// into one, bridge to the network.
class studio
{
public:
    explicit studio(const studio_options& options = {});
    ~studio();

    studio(const studio&) = delete;
    studio& operator=(const studio&) = delete;

    vss::hub& engine() noexcept { return *hub_; }
    const studio_options& options() const noexcept { return options_; }

    void start();
    void stop();

    // -- ports -------------------------------------------------------------

    /// Creates an OS virtual port.  `auto` picks the best available: a
    /// pseudo terminal on POSIX, a com0com pair on Windows when one exists,
    /// and an in-process loopback otherwise.
    /// `com_number` is the COM number a port of the Windows driver should have,
    /// 1 to 255; 0 takes whichever Windows picks.  It means something only to
    /// that driver (`virtual_port_kind::windows_driver`, or `auto` where the
    /// driver is installed); every other port ignores it.  A number that
    /// another device has fails with `errc::already_exists` and no port is
    /// made, and a number with no helper service to give it fails with
    /// `errc::unsupported`.
    result<std::shared_ptr<endpoint>>
    create_virtual_port(const std::string& name,
                        virtual_port_kind kind = virtual_port_kind::pty,
                        const endpoint_options& options = {}, unsigned com_number = 0);

    /// Gives a port of the Windows driver a different COM number, as Device
    /// Manager's Advanced > COM Port Number does, and returns the new COM name.
    /// The port is restarted, so no program may have it open; see
    /// `set_driver_com_number` for the errors.  The studio remembers the number
    /// the port was given, and `snapshot()` saves it.  `errc::not_found` for an
    /// unknown port and `errc::unsupported` for a port the driver did not make.
    result<std::string> set_com_number(endpoint_id id, unsigned com_number);

    /// Attaches a real serial port or a network backend from a URI.
    result<std::shared_ptr<endpoint>>
    attach_serial(const std::string& name, const std::string& uri,
                  const endpoint_options& options = {});

    result<std::shared_ptr<endpoint>>
    attach_serial(const std::string& name, const std::string& uri,
                  const serial_settings& settings,
                  endpoint_options options = {});

    /// Creates a scriptable in-memory device.
    result<std::shared_ptr<endpoint>>
    create_mock_port(const std::string& name,
                     const serial_settings& settings = {},
                     const std::string& target = "scripted");

    /// Makes a mock fixture report `fault` on its next read.  The call is
    /// queued on the endpoint's strand, so it is safe from any thread and
    /// returns before the fault is delivered.  `errc::not_found` for an
    /// unknown endpoint and `errc::unsupported` for a port that is not a
    /// fixture.
    result<void> inject_fault(endpoint_id id, fault_kind fault);

    /// Makes a mock fixture speak: `data` arrives on the fixture as if the
    /// device had sent it, so the hub carries it along the fixture's links.
    /// (`endpoint::write` is the other way round: it hands bytes to the
    /// device, which a mock only records.)  Like `inject_fault` the call is
    /// queued on the endpoint's strand, so it is safe from any thread.
    /// `errc::not_found` for an unknown endpoint and `errc::unsupported` for a
    /// port that is not a fixture.
    result<void> inject_data(endpoint_id id, bytes_view data);

    /// Makes a mock fixture answer every byte the hub writes to it with the
    /// same byte, as if the device had sent it back, or stops that.  The
    /// same errors as `inject_data`.  Two echoing fixtures linked to each
    /// other pass a byte back and forth for ever.
    result<void> set_fixture_echo(endpoint_id id, bool on);

    /// True while the fixture echoes.
    bool fixture_echo(endpoint_id id) const;

    /// Gives a port new serial settings and keeps its name, id and links.
    /// What happens depends on how the port was made:
    /// - a mock fixture gets a new fixture with the settings, under the same
    ///   endpoint (an echo that was on stays on);
    /// - a port made from an address (a serial device, `rfc2217://`) is
    ///   opened again with the settings, as `reopen` does;
    /// - a virtual port or a loopback keeps running: the program that opens
    ///   it sets its own speed, so only the values the studio saves change.
    /// A `tcp://` or `udp://` port has no line settings: `errc::unsupported`.
    /// `errc::invalid_argument` for settings `validate` refuses and
    /// `errc::not_found` for an unknown port.  A port that cannot be opened
    /// with the new settings keeps the old ones and reports why.  It can block
    /// while a transport opens, so do not call it from the GUI thread.
    /// `endpoint_info::serial` keeps the value the endpoint was made with;
    /// `port_settings` returns the current one.
    result<void> reconfigure_port(endpoint_id id,
                                  const serial_settings& settings);

    /// The serial settings the studio keeps for a port, which `snapshot` saves.
    std::optional<serial_settings> port_settings(endpoint_id id) const;

    result<void> remove_port(endpoint_id id);
    result<void> remove_port(std::string_view name);

    /// Gives a port a new name, in the hub and in the studio's records, so
    /// events, links and a saved snapshot all use it.  A pseudo terminal is
    /// advertised by a link named after the port; the link is made again
    /// under the new name and the old one removed.  Every other kind keeps
    /// the path it was created with.  `errc::invalid_argument` for an empty
    /// name, `errc::already_exists` when another port has the name and
    /// `errc::not_found` for an unknown port.
    result<void> rename_port(endpoint_id id, const std::string& name);

    /// The ports the studio made through the Windows driver, with the COM name
    /// each has now.  Empty everywhere but Windows.
    std::unordered_map<endpoint_id, std::string> driver_ports() const;

    /// What the driver says about the program that has the driver port `id`
    /// open: its process and line settings.  Empty for a port that is not a
    /// driver port, and for one the driver has not yet reported on.
    std::optional<driver_peer_info> driver_holder(endpoint_id id) const;

    /// Takes the driver port `id` back from the program that has it open: the
    /// program keeps running, but its handle stops working and another program
    /// can open the port.  The answer is the process that had it, or 0 when
    /// none did. `errc::not_found` for an unknown port and `errc::unsupported`
    /// for a port that is not a driver port; see `revoke_driver_port` for the
    /// rest.
    result<std::uint32_t> revoke_port(endpoint_id id);

    /// For each pseudo terminal port, whether a program other than the studio
    /// has its device open.  A port the platform cannot observe is left out,
    /// so a missing entry means "unknown", never "not open".  The process
    /// table is read once for every port, so call it for all of them
    /// together and not more than about once a second.
    std::unordered_map<endpoint_id, peer_open_state> peer_states() const;

    /// Opens the port again from what the studio kept of how it was made,
    /// and puts the new transport under the same endpoint, so the name, the id
    /// and every link stay.  For a port made from an address (a serial device,
    /// `tcp://`, `rfc2217://`, a listener) it opens that address again with the
    /// settings it had; for a port of the Windows driver it makes a new driver
    /// port with the COM number it was asked to have, after closing the dead
    /// one so its number is free, and the COM name can differ when no number
    /// was asked for.  Works on a port that is open too (a restart).
    /// `errc::not_found` for an unknown port and `errc::unsupported` for one
    /// that cannot be made again (a mock fixture, a pseudo terminal, an
    /// in-process loopback).  A port that cannot be opened reports why and is
    /// left as it was.  It can block while the transport connects, so do not
    /// call it from the GUI thread.
    result<void> reopen(endpoint_id id);

    /// Turns the retry of lost connections on or off.  With the policy on, a
    /// `tcp://` or `rfc2217://` client whose connection drops (not a listener,
    /// not a serial device) is opened again after a delay that doubles with
    /// each failure; every step is logged as a `log_message` event on the
    /// endpoint.  Off by default.
    void set_reconnect_policy(const reconnect_policy& policy);
    reconnect_policy current_reconnect_policy() const;

    /// The ports of the Windows driver whose endpoint has stopped: the helper
    /// service or the driver was restarted, or the port was removed from
    /// outside, and the programs that had the COM port lost it.  The ports
    /// are still in the topology with their links; #recreate_lost_ports makes
    /// them again.  Empty on every other platform.
    std::vector<endpoint_id> lost_ports() const;

    /// What #recreate_lost_ports did.
    struct recreate_report
    {
        std::vector<endpoint_id> recreated;
        std::vector<std::pair<endpoint_id, error_code>> failed;
    };

    /// Calls #reopen for every lost port.  A port that fails is reported and
    /// the rest go on.  Blocks for as long as the driver takes to make the
    /// ports, a second or two each.
    recreate_report recreate_lost_ports();
    /// The links whose members run at different baud rates or framing, from
    /// what each port reports now (see #find_format_mismatches).  Cheap; the
    /// window and `vss_cli run` ask on every refresh.
    std::vector<format_mismatch> format_mismatches() const;

    /// What other applications should open for this endpoint.
    std::string advertised_path(endpoint_id id) const;

    endpoint_options default_options() const;

    std::shared_ptr<endpoint> find(std::string_view name) const;
    std::vector<endpoint_info> endpoint_infos() const;
    std::vector<link_info> link_infos() const;

    // -- topology ----------------------------------------------------------

    result<link_id> connect(std::string_view a, std::string_view b);
    result<link_id> split(std::string_view source,
                          const std::vector<std::string>& targets);
    result<link_id> join(const std::vector<std::string>& sources,
                         std::string_view target);
    result<link_id> redirect(std::string_view from, std::string_view to);
    result<link_id> monitor(std::string_view from, std::string_view to);

    /// Puts the named ports on one shared multi-drop bus: each hears every
    /// other and none hears itself.  Modem lines are not carried.  See
    /// #hub::bus for the rules, including how a bus meets other links.
    ///
    /// `options` may carry a `delay_ms` and `jitter_ms`, and a `half_duplex`
    /// line; the other fields are fixed for a bus.
    result<link_id> bus(const std::vector<std::string>& members,
                        link_options options = {});
    result<void> disconnect(link_id id);

    // -- scenarios ---------------------------------------------------------

    struct share_result
    {
        endpoint_id source = 0;
        std::vector<endpoint_id> listeners;
        link_id link = 0;

        friend bool operator==(const share_result&,
                               const share_result&) = default;
    };

    /// Opens `uri` once and gives every listener its own virtual port.
    result<share_result> share_serial(const std::string& uri, int listeners);

    /// Merges several ports into one endpoint named `target_name`.
    result<link_id> join_ports(const std::vector<std::string>& source_uris,
                               const std::string& target_uri);

    /// Connects two endpoints named by URI and returns the link.
    result<link_id> bridge(const std::string& from_uri,
                           const std::string& to_uri);

    // -- persistence -------------------------------------------------------

    result<void> apply(const config::document& value,
                       bool rollback_on_error = true);
    config::document snapshot() const;

    // -- environment -------------------------------------------------------

    com0com_installation detect_com0com() const;
    std::vector<serial_device_info> serial_devices() const;

private:
    struct port_record
    {
        endpoint_id id = 0;
        std::string name;
        std::string kind = "virtual";
        std::string uri;
        std::string target;
        std::string virtual_kind = "auto";
        std::string advertised;
        std::optional<stable_link> link;
        serial_settings serial;
        bool capture = true;
        /// The fixture behind a mock port, or empty for every other kind.
        std::weak_ptr<serial_transport> fixture;
        /// The hub subscription of a fixture's echo, or 0.
        subscriber_id echo = 0;
        /// The device a mock fixture plays, so it can be made again.
        std::string mock_target;
        /// For a port of the Windows driver: the number it was asked to have
        /// (0 for whichever Windows picked), and its transport, which the
        /// number is changed through.
        unsigned com_number = 0;
        std::weak_ptr<transport> driver;
        /// The port was opened with settings given apart from its address, so
        /// a reopen gives them again; otherwise the address alone describes it.
        bool settings_given = false;
    };

    /// The transport behind a mock port: `errc::not_found` for an unknown id
    /// and `errc::unsupported` for a port that is not a fixture.
    result<std::shared_ptr<serial_transport>> fixture_for(endpoint_id id) const;

    result<std::shared_ptr<endpoint>>
    register_port(port_record record, std::shared_ptr<transport> transport,
                  const endpoint_options& options);

    result<std::shared_ptr<endpoint>>
    create_auto_virtual_port(const std::string& name, const endpoint_options& options,
                             unsigned com_number = 0);

    /// Makes the stable link for a new pseudo terminal when the options ask
    /// for one, and points the record's advertised path at it.
    result<void> link_pty(port_record& record);

    std::string unique_name(std::string_view base) const;
    std::vector<endpoint_id> resolve_all(const std::vector<std::string>& names,
                                         error_code& ec) const;

    /// True when a physical path or an advertised path is already claimed by
    /// this studio, so the same OS port is never wired into the hub twice.
    bool path_in_use(const std::string& path) const;
    void claim_paths(const std::string& target, const std::string& advertised);

    /// One try of the retry policy for a lost connection.
    result<void> reconnect_attempt(endpoint_id id);
    /// True for an endpoint the retry policy applies to.
    static bool reconnects(const port_record& record);

    studio_options options_;
    std::shared_ptr<vss::hub> hub_;
    std::shared_ptr<reconnector> reconnector_;
    subscriber_id reconnect_subscription_ = 0;
    mutable std::mutex record_mutex_;
    std::unordered_map<endpoint_id, port_record> records_;
    std::unordered_set<std::string> claimed_paths_;
};

} // namespace vss

#endif // VSS_STUDIO_STUDIO_HPP
