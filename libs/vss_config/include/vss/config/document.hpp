//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_CONFIG_DOCUMENT_HPP
#define VSS_CONFIG_DOCUMENT_HPP

#include <vss/core/error.hpp>
#include <vss/core/link.hpp>
#include <vss/core/serial_settings.hpp>
#include <vss/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vss::config {

/// The most plugin bindings one port or one link may have.
constexpr std::size_t max_plugin_bindings = 16;
/// The most bytes the parameters of one binding may take, written as compact
/// JSON, and how deep they may nest.
constexpr std::size_t max_binding_params_size = 64 * 1024;
constexpr std::size_t max_binding_params_depth = 8;

/// What a plugin provides at a port or a link, as a saved topology records
/// it.  It is there so that a setup that is shared says which plugins it
/// needs.  This library does not know any plugin: it checks the shape of a
/// binding and keeps it, and a setup is never refused because a plugin it
/// names is not installed.
struct plugin_binding
{
    /// The plugin's id, such as `com.example.modbus-fixture`: lower-case
    /// names of letters and digits joined by `.` or `-`, with at least one
    /// dot, 3 to 128 characters.
    std::string id;
    /// What the plugin provides here.  `fixture` and `generator` belong on a
    /// port, `modifier` on a link, `decoder` on either.  Another kind
    /// (`[a-z][a-z0-9_-]*`, at most 64 characters) is kept as it is, so a
    /// setup written by a newer version still opens.
    std::string kind;
    /// Which of the plugin's types: `[a-z][a-z0-9_-]*`, at most 64
    /// characters.
    std::string type;
    /// For a person who opens the setup without the plugin.  Plain text with
    /// no control characters and no marks that change its direction; at most
    /// 100 characters for `name` and `publisher`, 64 for `version`, 128 for
    /// `key_id` and 256 for `get_it_from`.  Each may be empty and is then
    /// left out of the file.  `get_it_from` is shown and never opened.
    std::string name;
    std::string publisher;
    std::string key_id;
    std::string version;
    std::string get_it_from;
    /// For a `modifier`: `fail_open`, `fail_closed`, or empty for the
    /// plugin's own default.
    std::string on_timeout;
    /// The plugin's parameters as the text of one JSON object, or empty for
    /// none.  The values are the plugin's and are not looked at, beyond
    /// `max_binding_params_size` and `max_binding_params_depth`.  After a
    /// read it is compact JSON with the keys in the order of the file.
    std::string params;

    friend bool operator==(const plugin_binding&,
                           const plugin_binding&) = default;
};

/// One endpoint in a saved topology.
struct port_entry
{
    std::string name;
    /// `virtual`, `physical`, `tcp`, `udp`, `rfc2217` or `mock`.
    std::string kind = "virtual";
    /// Backend URI for everything except `virtual` and `mock`.
    std::string uri;
    /// For `virtual`: `auto`, `pty`, `named-pipe` or `loopback`.
    std::string virtual_kind = "auto";
    /// For `mock`: the fixture's name.
    std::string mock_target = "scripted";
    serial_settings serial;
    bool capture = true;
    /// For `virtual` ports of kind `auto` or `windows-driver`: the COM number the
    /// Windows driver gives the port, 1 to 255.  0 leaves it to Windows, which
    /// takes the lowest free number.  Other systems and other kinds of port have no
    /// COM numbers and ignore it.
    unsigned com_number = 0;
    /// What plugins provide at this port: at most one `fixture` or
    /// `generator`, and at most `max_plugin_bindings` in all.  In the file
    /// it is the list `plugins`, left out when empty, so a port without
    /// plugins is written as before.
    std::vector<plugin_binding> plugins;

    friend bool operator==(const port_entry&, const port_entry&) = default;
};

/// One link between ports.
struct link_entry
{
    /// `duplex`, `split`, `join`, `redirect`, `monitor` or `bus`.
    std::string mode = "duplex";
    std::vector<std::string> sources;
    std::vector<std::string> targets;
    /// For `bus` only: the ports on the shared bus, 2 to
    /// `vss::max_bus_members`.  A bus has no `sources` or `targets`.
    std::vector<std::string> members;
    /// A `bus` has no modem lines, so it must be `false` there; in the file
    /// it may be left out for a bus.
    bool emulate_modem_lines = true;
    /// A `bus` supports `interleave` only.
    merge_policy policy = merge_policy::interleave;
    /// Milliseconds each chunk is held on the link, 0 to
    /// `vss::max_link_delay_ms`.  Left out of the file when zero, on every
    /// mode including `bus`.
    std::uint32_t delay_ms = 0;
    /// Up to this much either side of `delay_ms`; at most `delay_ms`.
    std::uint32_t jitter_ms = 0;
    /// For `bus` only: the half-duplex line (`vss::half_duplex_options`).
    /// In the file it is the object `half_duplex` with `bit_rate`,
    /// `turnaround_ms`, `collisions` (`report` or `garble`) and
    /// `rts_gating`, each optional; the object being there turns the option
    /// on.  Left out of the file when the option is off, so a bus without
    /// it is written as before.
    half_duplex_options half_duplex;
    /// What plugins provide on this link, at most `max_plugin_bindings`.
    /// The order of the list is the order `modifier` bindings are applied
    /// in.  In the file it is the list `plugins`, left out when empty.
    std::vector<plugin_binding> plugins;

    friend bool operator==(const link_entry&, const link_entry&) = default;
};

/// A complete saved topology.
struct document
{
    std::string name = "untitled";
    std::string description;
    std::vector<port_entry> ports;
    std::vector<link_entry> links;

    friend bool operator==(const document&, const document&) = default;
};

/// Serialises to a stable, human editable JSON shape.
result<std::string> to_json(const document& value, bool pretty = true);

result<document> from_json(std::string_view text);

/// Checks names, kinds, URIs and link arity.
error_code validate(const document& value);

/// Reads a topology file. A missing path gives `not_found`; a path that is not
/// a regular file (a directory, a device, a pipe) gives `invalid_argument`.
result<document> load_file(const std::string& path);
result<void> save_file(const std::string& path, const document& value,
                       bool pretty = true);

/// Maps a document port kind to the endpoint kind it creates.
result<endpoint_kind> to_endpoint_kind(std::string_view kind);

/// The numbers a COM port may be given.
constexpr unsigned min_com_number = 1;
constexpr unsigned max_com_number = 255;

constexpr const char* port_kinds[] = {"virtual", "physical", "tcp", "udp",
                                      "rfc2217", "mock"};
constexpr const char* virtual_port_kinds[] = {"auto", "pty", "named-pipe",
                                              "loopback", "windows-driver"};
constexpr const char* link_modes[] = {"duplex",   "split",   "join",
                                      "redirect", "monitor", "bus"};

} // namespace vss::config

#endif // VSS_CONFIG_DOCUMENT_HPP
