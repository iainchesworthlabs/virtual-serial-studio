//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/config/document.hpp>
#include <vss/core/profile.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

namespace vss::config {
namespace json = boost::json;

namespace {

json::object serial_to_json(const serial_settings& settings)
{
    json::object out;
    out["baud_rate"] = settings.baud_rate;
    out["data_bits"] = settings.data_bits;
    out["parity"] = to_string(settings.parity);
    out["stop_bits"] = to_string(settings.stop_bits);
    out["flow_control"] = to_string(settings.flow_control);
    out["read_timeout_ms"] = settings.read_timeout.count();
    out["write_timeout_ms"] = settings.write_timeout.count();
    out["exclusive"] = settings.exclusive;
    return out;
}

error_code serial_from_json(const json::value& value, serial_settings& out)
{
    if (value.is_null())
        return {};
    if (!value.is_object())
        return make_error_code(errc::config_error);

    const json::object& object = value.as_object();

    if (const auto* item = object.if_contains("baud_rate"))
    {
        if (!item->is_int64() || item->as_int64() < 0
            || item->as_int64() > 40'000'000)
            return make_error_code(errc::config_error);
        out.baud_rate = static_cast<unsigned>(item->as_int64());
    }
    if (const auto* item = object.if_contains("data_bits"))
    {
        if (!item->is_int64())
            return make_error_code(errc::config_error);
        out.data_bits = static_cast<unsigned>(item->as_int64());
    }
    if (const auto* item = object.if_contains("parity"))
    {
        if (!item->is_string())
            return make_error_code(errc::config_error);
        const auto parsed = parse_parity(item->as_string());
        if (!parsed)
            return make_error_code(errc::config_error);
        out.parity = *parsed;
    }
    if (const auto* item = object.if_contains("stop_bits"))
    {
        if (!item->is_string())
            return make_error_code(errc::config_error);
        const auto parsed = parse_stop_bits(item->as_string());
        if (!parsed)
            return make_error_code(errc::config_error);
        out.stop_bits = *parsed;
    }
    if (const auto* item = object.if_contains("flow_control"))
    {
        if (!item->is_string())
            return make_error_code(errc::config_error);
        const auto parsed = parse_flow_control(item->as_string());
        if (!parsed)
            return make_error_code(errc::config_error);
        out.flow_control = *parsed;
    }
    if (const auto* item = object.if_contains("read_timeout_ms"))
    {
        if (!item->is_int64() || item->as_int64() < 0)
            return make_error_code(errc::config_error);
        out.read_timeout = std::chrono::milliseconds(item->as_int64());
    }
    if (const auto* item = object.if_contains("write_timeout_ms"))
    {
        if (!item->is_int64() || item->as_int64() < 0)
            return make_error_code(errc::config_error);
        out.write_timeout = std::chrono::milliseconds(item->as_int64());
    }
    if (const auto* item = object.if_contains("exclusive"))
    {
        if (!item->is_bool())
            return make_error_code(errc::config_error);
        out.exclusive = item->as_bool();
    }

    return validate(out);
}

std::string string_member(const json::object& object, std::string_view key,
                          std::string_view fallback = {})
{
    const auto* item = object.if_contains(key);
    if (!item || !item->is_string())
        return std::string(fallback);
    return std::string(item->as_string());
}

bool bool_member(const json::object& object, std::string_view key, bool fallback)
{
    const auto* item = object.if_contains(key);
    if (!item || !item->is_bool())
        return fallback;
    return item->as_bool();
}

result<std::vector<std::string>> string_array(const json::object& object,
                                              std::string_view key)
{
    std::vector<std::string> out;
    const auto* item = object.if_contains(key);
    if (!item)
        return out;
    if (!item->is_array())
        return make_error_code(errc::config_error);
    for (const auto& element : item->as_array())
    {
        if (!element.is_string())
            return make_error_code(errc::config_error);
        out.emplace_back(element.as_string());
    }
    return out;
}

/// Writes `delay_ms` and `jitter_ms` only when they are set, so a file
/// without latency is the same as before.
void latency_to_json(const link_entry& link, json::object& object)
{
    if (link.delay_ms != 0)
        object["delay_ms"] = link.delay_ms;
    if (link.jitter_ms != 0)
        object["jitter_ms"] = link.jitter_ms;
}

error_code milliseconds_member(const json::object& object, std::string_view key,
                               std::uint32_t& out)
{
    const auto* item = object.if_contains(key);
    if (!item)
        return {};
    if (!item->is_int64() || item->as_int64() < 0 ||
        item->as_int64() > static_cast<std::int64_t>(max_link_delay_ms))
        return make_error_code(errc::config_error);
    out = static_cast<std::uint32_t>(item->as_int64());
    return {};
}

error_code latency_from_json(const json::object& object, link_entry& link)
{
    if (const error_code error =
            milliseconds_member(object, "delay_ms", link.delay_ms))
        return error;
    return milliseconds_member(object, "jitter_ms", link.jitter_ms);
}

/// Writes the half-duplex line of a bus, and nothing when it is off.
void half_duplex_to_json(const link_entry& link, json::object& object)
{
    if (!link.half_duplex.enabled)
        return;
    json::object line;
    line["bit_rate"] = link.half_duplex.bit_rate;
    line["turnaround_ms"] = link.half_duplex.turnaround_ms;
    line["collisions"] = to_string(link.half_duplex.on_collision);
    line["rts_gating"] = link.half_duplex.rts_gating;
    object["half_duplex"] = std::move(line);
}

/// Reads the `half_duplex` object of a link.  Its presence turns the option
/// on; each member is optional.  Which links may have one, and the limits of
/// the numbers, are checked in validate().
error_code half_duplex_from_json(const json::object& object, link_entry& link)
{
    const auto* item = object.if_contains("half_duplex");
    if (!item)
        return {};
    if (!item->is_object())
        return make_error_code(errc::config_error);
    const json::object& line = item->as_object();

    half_duplex_options options;
    options.enabled = true;
    const auto number = [&line](std::string_view key, std::uint32_t& out)
    {
        const auto* value = line.if_contains(key);
        if (!value)
            return true;
        if (!value->is_int64() || value->as_int64() < 0 ||
            value->as_int64() > static_cast<std::int64_t>(max_bus_bit_rate))
            return false;
        out = static_cast<std::uint32_t>(value->as_int64());
        return true;
    };
    if (!number("bit_rate", options.bit_rate) ||
        !number("turnaround_ms", options.turnaround_ms))
        return make_error_code(errc::config_error);

    if (const auto* collisions = line.if_contains("collisions"))
    {
        if (!collisions->is_string())
            return make_error_code(errc::config_error);
        if (collisions->as_string() == "garble")
            options.on_collision = collision_action::garble;
        else if (collisions->as_string() != "report")
            return make_error_code(errc::config_error);
    }
    if (const auto* gating = line.if_contains("rts_gating"))
    {
        if (!gating->is_bool())
            return make_error_code(errc::config_error);
        options.rts_gating = gating->as_bool();
    }

    link.half_duplex = options;
    return {};
}

// -- plugin bindings ---------------------------------------------------------
//
// The rules for an id and for a text are the ones a plugin's manifest has
// (vss_plugin, docs/plugins.md); they are repeated here because this library
// must read a setup whether or not the plugin code is built.

constexpr std::size_t max_binding_id_length = 128;
constexpr std::size_t max_binding_local_id_length = 64;
constexpr std::size_t max_binding_name_length = 100;
constexpr std::size_t max_binding_version_length = 64;
constexpr std::size_t max_binding_key_id_length = 128;
constexpr std::size_t max_binding_address_length = 256;

bool is_lower_or_digit(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/// Lower-case names of letters and digits joined by `.` or `-`, with at
/// least one dot.
bool is_plugin_id(std::string_view text)
{
    if (text.size() < 3 || text.size() > max_binding_id_length)
        return false;
    bool dot = false;
    bool after_separator = true;
    for (const char c : text)
    {
        if (is_lower_or_digit(c))
        {
            after_separator = false;
            continue;
        }
        if ((c != '.' && c != '-') || after_separator)
            return false;
        dot = dot || c == '.';
        after_separator = true;
    }
    return dot && !after_separator;
}

/// `[a-z][a-z0-9_-]*`, at most 64 characters.
bool is_local_id(std::string_view text)
{
    if (text.empty() || text.size() > max_binding_local_id_length ||
        text.front() < 'a' || text.front() > 'z')
        return false;
    return std::ranges::all_of(text, [](char c) {
        return is_lower_or_digit(c) || c == '_' || c == '-';
    });
}

/// Whether a text may be shown to a person as it is: no control characters,
/// no line or paragraph separators, no byte order mark and none of the marks
/// that change the direction of the text around them.
bool is_clean_text(std::string_view text)
{
    const auto byte_at = [&text](std::size_t index) {
        return index < text.size() ? static_cast<std::uint8_t>(text[index])
                                   : std::uint8_t{0};
    };
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const std::uint8_t first = byte_at(i);
        const std::uint8_t second = byte_at(i + 1);
        const std::uint8_t third = byte_at(i + 2);
        if (first < 0x20 || first == 0x7f)
            return false;
        // U+0080 to U+009F, and U+061C.
        if (first == 0xc2 && second >= 0x80 && second <= 0x9f)
            return false;
        if (first == 0xd8 && second == 0x9c)
            return false;
        // U+200E, U+200F, U+2028, U+2029, U+202A to U+202E.
        if (first == 0xe2 && second == 0x80 &&
            (third == 0x8e || third == 0x8f ||
             (third >= 0xa8 && third <= 0xae)))
            return false;
        // U+2066 to U+2069, and U+FEFF.
        if (first == 0xe2 && second == 0x81 && third >= 0xa6 && third <= 0xa9)
            return false;
        if (first == 0xef && second == 0xbb && third == 0xbf)
            return false;
    }
    return true;
}

bool is_binding_text(std::string_view text, std::size_t limit)
{
    return text.size() <= limit && is_clean_text(text);
}

std::size_t depth_of(const json::value& value)
{
    std::size_t deepest = 0;
    if (value.is_object())
    {
        for (const auto& entry : value.as_object())
            deepest = std::max(deepest, depth_of(entry.value()));
        return deepest + 1;
    }
    if (value.is_array())
    {
        for (const auto& element : value.as_array())
            deepest = std::max(deepest, depth_of(element));
        return deepest + 1;
    }
    return 0;
}

/// The parameters of a binding as a JSON object, or nothing when the text is
/// not one object inside the limits.
std::optional<json::object> binding_params(std::string_view text)
{
    if (text.size() > max_binding_params_size)
        return std::nullopt;
    boost::system::error_code ec;
    json::value parsed = json::parse(text, ec);
    if (ec || !parsed.is_object() ||
        depth_of(parsed) > max_binding_params_depth)
        return std::nullopt;
    // The limit is on the compact text, which is what a read gives back.
    if (json::serialize(parsed).size() > max_binding_params_size)
        return std::nullopt;
    json::object params = std::move(parsed.as_object());
    return params;
}

bool valid_binding(const plugin_binding& binding)
{
    if (!is_plugin_id(binding.id) || !is_local_id(binding.kind) ||
        !is_local_id(binding.type))
        return false;
    if (!is_binding_text(binding.name, max_binding_name_length) ||
        !is_binding_text(binding.publisher, max_binding_name_length) ||
        !is_binding_text(binding.version, max_binding_version_length) ||
        !is_binding_text(binding.key_id, max_binding_key_id_length) ||
        !is_binding_text(binding.get_it_from, max_binding_address_length))
        return false;
    if (!binding.on_timeout.empty() && binding.on_timeout != "fail_open" &&
        binding.on_timeout != "fail_closed")
        return false;
    return binding.params.empty() || binding_params(binding.params).has_value();
}

/// Checks the bindings of one port or one link.  `on_port` says which: a
/// fixture or a generator stands at a port, and at most one of them does; a
/// modifier sits on a link.  A kind this version does not know is accepted.
bool valid_bindings(const std::vector<plugin_binding>& bindings, bool on_port)
{
    if (bindings.size() > max_plugin_bindings)
        return false;
    std::size_t devices = 0;
    for (const plugin_binding& binding : bindings)
    {
        if (!valid_binding(binding))
            return false;
        const bool device =
            binding.kind == "fixture" || binding.kind == "generator";
        if (device)
            ++devices;
        if (on_port ? binding.kind == "modifier" : device)
            return false;
    }
    return devices <= 1;
}

/// Writes `plugins` only when there are bindings, and in a binding only the
/// members that are set, so a setup without plugins is the same as before.
void bindings_to_json(const std::vector<plugin_binding>& bindings,
                      json::object& object)
{
    if (bindings.empty())
        return;
    json::array list;
    for (const plugin_binding& binding : bindings)
    {
        json::object entry;
        entry["id"] = binding.id;
        entry["kind"] = binding.kind;
        entry["type"] = binding.type;
        const auto text = [&entry](std::string_view key,
                                   const std::string& value) {
            if (!value.empty())
                entry[key] = value;
        };
        text("name", binding.name);
        text("publisher", binding.publisher);
        text("key_id", binding.key_id);
        text("version", binding.version);
        text("get_it_from", binding.get_it_from);
        text("on_timeout", binding.on_timeout);
        // validate() has passed, so the text is one object.
        if (auto params = binding_params(binding.params))
            entry["params"] = std::move(*params);
        list.push_back(std::move(entry));
    }
    object["plugins"] = std::move(list);
}

/// Reads the `plugins` list of a port or a link.  Unlike the older members,
/// which fall back to a default, a member of a binding that is there must
/// have the right type.  The values are checked in validate().
error_code bindings_from_json(const json::object& object,
                              std::vector<plugin_binding>& out)
{
    const auto* item = object.if_contains("plugins");
    if (!item)
        return {};
    if (!item->is_array() || item->as_array().size() > max_plugin_bindings)
        return make_error_code(errc::config_error);

    for (const auto& element : item->as_array())
    {
        if (!element.is_object())
            return make_error_code(errc::config_error);
        const json::object& entry = element.as_object();

        plugin_binding binding;
        bool well_typed = true;
        const auto text = [&entry, &well_typed](std::string_view key,
                                                std::string& value) {
            const auto* member = entry.if_contains(key);
            if (!member)
                return;
            if (member->is_string())
                value = std::string(member->as_string());
            else
                well_typed = false;
        };
        text("id", binding.id);
        text("kind", binding.kind);
        text("type", binding.type);
        text("name", binding.name);
        text("publisher", binding.publisher);
        text("key_id", binding.key_id);
        text("version", binding.version);
        text("get_it_from", binding.get_it_from);
        text("on_timeout", binding.on_timeout);
        if (!well_typed)
            return make_error_code(errc::config_error);

        if (const auto* params = entry.if_contains("params"))
        {
            if (!params->is_object())
                return make_error_code(errc::config_error);
            binding.params = json::serialize(*params);
        }
        out.push_back(std::move(binding));
    }
    return {};
}

bool contains_kind(std::string_view value)
{
    for (const char* kind : port_kinds)
    {
        if (value == kind)
            return true;
    }
    return false;
}

bool contains_virtual_kind(std::string_view value)
{
    for (const char* kind : virtual_port_kinds)
    {
        if (value == kind)
            return true;
    }
    return false;
}

bool contains_link_mode(std::string_view value)
{
    for (const char* mode : link_modes)
    {
        if (value == mode)
            return true;
    }
    return false;
}

void pretty_write(std::string& out, const json::value& value, int depth)
{
    const std::string pad(static_cast<std::size_t>(depth) * 2, ' ');

    if (value.is_object())
    {
        const auto& object = value.as_object();
        if (object.empty())
        {
            out += "{}";
            return;
        }
        out += "{\n";
        bool first = true;
        for (const auto& entry : object)
        {
            if (!first)
                out += ",\n";
            first = false;
            out += pad;
            out += "  ";
            out += json::serialize(json::value(entry.key()));
            out += ": ";
            pretty_write(out, entry.value(), depth + 1);
        }
        out += '\n';
        out += pad;
        out += '}';
        return;
    }

    if (value.is_array())
    {
        const auto& array = value.as_array();
        if (array.empty())
        {
            out += "[]";
            return;
        }
        out += "[\n";
        for (std::size_t index = 0; index < array.size(); ++index)
        {
            if (index != 0)
                out += ",\n";
            out += pad;
            out += "  ";
            pretty_write(out, array[index], depth + 1);
        }
        out += '\n';
        out += pad;
        out += ']';
        return;
    }

    out += json::serialize(value);
}

} // namespace

result<endpoint_kind> to_endpoint_kind(std::string_view kind)
{
    if (kind == "virtual")
        return endpoint_kind::virtual_port;
    if (kind == "physical")
        return endpoint_kind::physical;
    if (kind == "tcp")
        return endpoint_kind::tcp;
    if (kind == "udp")
        return endpoint_kind::udp;
    if (kind == "rfc2217")
        return endpoint_kind::rfc2217;
    if (kind == "mock")
        return endpoint_kind::mock;
    return make_error_code(errc::config_error);
}

error_code validate(const document& value)
{
    // The document needs a name.  Then the ports are checked, followed by
    // the links, which refer to ports by name; the first problem is returned.
    if (value.name.empty())
        return make_error_code(errc::config_error);

    // Port names must be non-empty and unique, and the kinds must be known.
    std::set<std::string> names;
    for (const auto& port : value.ports)
    {
        if (port.name.empty())
            return make_error_code(errc::config_error);
        if (!names.insert(port.name).second)
            return make_error_code(errc::already_exists);
        if (!contains_kind(port.kind))
            return make_error_code(errc::config_error);
        if (!contains_virtual_kind(port.virtual_kind))
            return make_error_code(errc::config_error);

        // Virtual and mock ports are made by Studio, so they must not have a
        // uri; every other kind opens something and must have one.
        if (port.kind == "virtual" || port.kind == "mock")
        {
            if (!port.uri.empty())
                return make_error_code(errc::config_error);
        }
        else if (port.uri.empty())
        {
            return make_error_code(errc::config_error);
        }

        // A COM number means something to the Windows driver only.
        if (port.com_number != 0)
        {
            if (port.com_number < min_com_number
                || port.com_number > max_com_number
                || port.kind != "virtual"
                || (port.virtual_kind != "auto"
                    && port.virtual_kind != "windows-driver"))
                return make_error_code(errc::config_error);
        }

        // The serial settings have their own rules, shared with other code.
        if (const error_code settings_error = vss::validate(port.serial))
            return settings_error;

        if (!valid_bindings(port.plugins, true))
            return make_error_code(errc::config_error);
    }

    // Links: the mode must be known and the delay within its limit, and
    // jitter cannot exceed the delay.
    for (const auto& link : value.links)
    {
        if (!contains_link_mode(link.mode))
            return make_error_code(errc::config_error);
        if (link.delay_ms > max_link_delay_ms ||
            link.jitter_ms > link.delay_ms)
            return make_error_code(errc::config_error);
        if (!valid_bindings(link.plugins, false))
            return make_error_code(errc::config_error);
        // Only a bus is one wire that its members take turns on.
        if (link.half_duplex.enabled &&
            (link.mode != "bus" || !valid_half_duplex(link.half_duplex)))
            return make_error_code(errc::config_error);

        if (link.mode == "bus")
        {
            // One broadcast domain: members only, no modem lines, no owner.
            if (!link.sources.empty() || !link.targets.empty())
                return make_error_code(errc::config_error);
            if (link.members.size() < 2 ||
                link.members.size() > max_bus_members)
                return make_error_code(errc::config_error);
            if (link.emulate_modem_lines ||
                link.policy != merge_policy::interleave)
                return make_error_code(errc::config_error);

            // Each member must be a known port and may be listed once.
            std::set<std::string> members;
            for (const auto& name : link.members)
            {
                if (names.find(name) == names.end())
                    return make_error_code(errc::not_found);
                if (!members.insert(name).second)
                    return make_error_code(errc::config_error);
            }
            continue;
        }
        // Any other mode uses sources and targets, never members, and both
        // sides need at least one port.
        if (!link.members.empty())
            return make_error_code(errc::config_error);
        if (link.sources.empty() || link.targets.empty())
            return make_error_code(errc::config_error);

        // Every port a link names must exist in the document.
        for (const auto& name : link.sources)
        {
            if (names.find(name) == names.end())
                return make_error_code(errc::not_found);
        }
        for (const auto& name : link.targets)
        {
            if (names.find(name) == names.end())
                return make_error_code(errc::not_found);
        }

        // How many ports a side may have depends on the mode: one to one for
        // duplex, redirect and monitor, one source for split and one target
        // for join.
        if (link.mode == "duplex" || link.mode == "redirect"
            || link.mode == "monitor")
        {
            if (link.sources.size() != 1 || link.targets.size() != 1)
                return make_error_code(errc::config_error);
        }
        else if (link.mode == "split")
        {
            if (link.sources.size() != 1)
                return make_error_code(errc::config_error);
        }
        else if (link.mode == "join")
        {
            if (link.targets.size() != 1)
                return make_error_code(errc::config_error);
        }
    }

    return {};
}

result<std::string> to_json(const document& value, bool pretty)
{
    VSS_PROFILE_ZONE("config.to_json");

    if (const error_code invalid = validate(value))
        return invalid;

    json::object root;
    root["version"] = 1;
    root["name"] = value.name;
    if (!value.description.empty())
        root["description"] = value.description;

    json::array ports;
    for (const auto& port : value.ports)
    {
        json::object object;
        object["name"] = port.name;
        object["kind"] = port.kind;
        if (!port.uri.empty())
            object["uri"] = port.uri;
        if (port.kind == "virtual")
            object["virtual_kind"] = port.virtual_kind;
        if (port.kind == "mock")
            object["mock_target"] = port.mock_target;
        object["serial"] = serial_to_json(port.serial);
        object["capture"] = port.capture;
        if (port.com_number != 0)
            object["com_number"] = port.com_number;
        bindings_to_json(port.plugins, object);
        ports.push_back(std::move(object));
    }
    root["ports"] = std::move(ports);

    json::array links;
    for (const auto& link : value.links)
    {
        json::object object;
        object["mode"] = link.mode;
        if (link.mode == "bus")
        {
            json::array members;
            for (const auto& name : link.members)
                members.push_back(json::value(name));
            object["members"] = std::move(members);
            object["emulate_modem_lines"] = false;
            object["policy"] = to_string(link.policy);
            latency_to_json(link, object);
            half_duplex_to_json(link, object);
            bindings_to_json(link.plugins, object);
            links.push_back(std::move(object));
            continue;
        }
        json::array sources;
        for (const auto& name : link.sources)
            sources.push_back(json::value(name));
        json::array targets;
        for (const auto& name : link.targets)
            targets.push_back(json::value(name));
        object["sources"] = std::move(sources);
        object["targets"] = std::move(targets);
        object["emulate_modem_lines"] = link.emulate_modem_lines;
        object["policy"] = to_string(link.policy);
        latency_to_json(link, object);
        bindings_to_json(link.plugins, object);
        links.push_back(std::move(object));
    }
    root["links"] = std::move(links);

    const json::value root_value(std::move(root));
    if (!pretty)
        return json::serialize(root_value);

    std::string out;
    pretty_write(out, root_value, 0);
    out.push_back('\n');
    return out;
}

result<document> from_json(std::string_view text)
{
    VSS_PROFILE_ZONE("config.from_json");

    // The text must be valid JSON with an object at the top level.
    boost::system::error_code ec;
    const json::value root = json::parse(text, ec);
    if (ec)
        return make_error_code(errc::config_error);
    if (!root.is_object())
        return make_error_code(errc::config_error);

    // Name and description are optional; a missing name reads "untitled".
    const json::object& object = root.as_object();
    document out;
    out.name = string_member(object, "name", "untitled");
    out.description = string_member(object, "description");

    // Ports: the list is optional, but if present it must be an array of
    // objects.  Missing members take the defaults chosen here.
    const auto* ports = object.if_contains("ports");
    if (ports && !ports->is_array())
        return make_error_code(errc::config_error);

    if (ports)
    {
        for (const auto& element : ports->as_array())
        {
            if (!element.is_object())
                return make_error_code(errc::config_error);
            const json::object& entry = element.as_object();

            port_entry port;
            port.name = string_member(entry, "name");
            port.kind = string_member(entry, "kind", "virtual");
            port.uri = string_member(entry, "uri");
            port.virtual_kind = string_member(entry, "virtual_kind", "auto");
            port.mock_target = string_member(entry, "mock_target", "scripted");
            port.capture = bool_member(entry, "capture", true);

            // A com_number must be an integer in the allowed range.  Whether
            // the port may have one is decided later, in validate().
            if (const auto* number = entry.if_contains("com_number"))
            {
                if (!number->is_int64() || number->as_int64() < min_com_number
                    || number->as_int64() > max_com_number)
                    return make_error_code(errc::config_error);
                port.com_number = static_cast<unsigned>(number->as_int64());
            }

            if (const auto* serial = entry.if_contains("serial"))
            {
                if (const error_code parse_error =
                        serial_from_json(*serial, port.serial))
                    return parse_error;
            }

            if (const error_code binding_error =
                    bindings_from_json(entry, port.plugins))
                return binding_error;

            out.ports.push_back(std::move(port));
        }
    }

    // Links follow the same rule as ports: optional, but an array of objects.
    const auto* links = object.if_contains("links");
    if (links && !links->is_array())
        return make_error_code(errc::config_error);

    if (links)
    {
        for (const auto& element : links->as_array())
        {
            if (!element.is_object())
                return make_error_code(errc::config_error);
            const json::object& entry = element.as_object();

            link_entry link;
            link.mode = string_member(entry, "mode", "duplex");
            // A bus has no modem lines, so absent means off there.
            link.emulate_modem_lines =
                bool_member(entry, "emulate_modem_lines", link.mode != "bus");

            // A missing list reads as empty; a list of anything but strings
            // is an error.  Which lists a mode needs is checked in validate().
            auto sources = string_array(entry, "sources");
            if (!sources)
                return make_error_code(errc::config_error);
            link.sources = std::move(*sources);

            auto targets = string_array(entry, "targets");
            if (!targets)
                return make_error_code(errc::config_error);
            link.targets = std::move(*targets);

            auto members = string_array(entry, "members");
            if (!members)
                return make_error_code(errc::config_error);
            link.members = std::move(*members);

            // The merge policy is either owner or interleave (the default).
            const std::string policy = string_member(entry, "policy", "interleave");
            if (policy == "owner")
                link.policy = merge_policy::owner;
            else if (policy == "interleave")
                link.policy = merge_policy::interleave;
            else
                return make_error_code(errc::config_error);

            if (const error_code latency_error =
                    latency_from_json(entry, link))
                return latency_error;

            if (const error_code line_error =
                    half_duplex_from_json(entry, link))
                return line_error;

            if (const error_code binding_error =
                    bindings_from_json(entry, link.plugins))
                return binding_error;

            out.links.push_back(std::move(link));
        }
    }

    // Parsing only checks types; validate() checks that the document makes
    // sense as a whole, so a caller never receives an invalid document.
    if (const error_code invalid = validate(out))
        return invalid;
    return out;
}

result<document> load_file(const std::string& path)
{
    // A topology is a small regular file. A directory, a device or a pipe would
    // open on some systems and then read as nothing, or never stop.
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error || status.type() == std::filesystem::file_type::not_found)
        return make_error_code(errc::not_found);
    if (!std::filesystem::is_regular_file(status))
        return make_error_code(errc::invalid_argument);

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return make_error_code(errc::not_found);

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return from_json(buffer.str());
}

result<void> save_file(const std::string& path, const document& value,
                       bool pretty)
{
    const auto text = to_json(value, pretty);
    if (!text)
        return text.error();

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return make_error_code(errc::permission_denied);
    stream << *text;
    if (!stream)
        return make_error_code(errc::io_error);
    return {};
}

} // namespace vss::config
