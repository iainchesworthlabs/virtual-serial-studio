//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/config/document.hpp>
#include <vss/core/profile.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <thread>

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

/// Reads the members of one JSON object of a setup.  A member that is there
/// must have the right type.  The reader remembers every key it is asked for,
/// and `done` refuses a member that no call asked for, so a typo is an error
/// and not a default.  On the first refusal `problem` says which member and
/// why, with the path of the member in the file, such as `ports[1].kind`.
///
/// To add a member to the file, add one read call for it before the `done`
/// of its object.  Nothing else lists the names, so a member that is read is
/// allowed and a member that is not read is refused.  Also change the struct
/// in document.hpp, `to_json`, and `validate` if the member has rules.
class member_reader
{
public:
    member_reader(const json::object& object, std::string where,
                  std::string& problem)
        : object_(object), where_(std::move(where)), problem_(problem)
    {
    }

    /// A reader of an object inside this one, which reports into the same
    /// `problem`.
    member_reader sub(const json::object& inner, std::string where) const
    {
        return member_reader(inner, std::move(where), problem_);
    }

    std::string path(std::string_view key) const
    {
        std::string out = where_;
        if (!out.empty())
            out += '.';
        out += key;
        return out;
    }

    /// Records the refusal of `key` and returns false.
    bool fail(std::string_view key, std::string_view why) const
    {
        problem_ = path(key) + " " + std::string(why);
        return false;
    }

    bool has(std::string_view key) const { return find(key) != nullptr; }

    /// The member as it is, or null when it is not there.
    const json::value* find(std::string_view key) const
    {
        if (std::ranges::find(asked_, key) == asked_.end())
            asked_.emplace_back(key);
        return object_.if_contains(key);
    }

    /// Refuses every member that no call has asked for.  Call it after the
    /// last read of the object.
    bool done() const
    {
        for (const auto& entry : object_)
        {
            const std::string_view key = entry.key();
            if (std::ranges::find(asked_, key) == asked_.end())
                return fail(key, "is not a member of a setup");
        }
        return true;
    }

    bool text(std::string_view key, std::string& out) const
    {
        const auto* item = find(key);
        if (!item)
            return true;
        if (!item->is_string())
            return fail(key, "must be text");
        out = std::string(item->as_string());
        return true;
    }

    bool flag(std::string_view key, bool& out) const
    {
        const auto* item = find(key);
        if (!item)
            return true;
        if (!item->is_bool())
            return fail(key, "must be true or false");
        out = item->as_bool();
        return true;
    }

    /// A whole number from `low` to `high`.
    template <class T>
    bool whole(std::string_view key, std::int64_t low, std::int64_t high,
               T& out) const
    {
        const auto* item = find(key);
        if (!item)
            return true;
        if (!item->is_int64() || item->as_int64() < low ||
            item->as_int64() > high)
            return fail(key, "must be a whole number from " +
                                 std::to_string(low) + " to " +
                                 std::to_string(high));
        out = static_cast<T>(item->as_int64());
        return true;
    }

    /// A list of text.
    bool strings(std::string_view key, std::vector<std::string>& out) const
    {
        const auto* item = find(key);
        if (!item)
            return true;
        if (!item->is_array())
            return fail(key, "must be a list of text");
        std::vector<std::string> read;
        for (const auto& element : item->as_array())
        {
            if (!element.is_string())
                return fail(key, "must be a list of text");
            read.emplace_back(element.as_string());
        }
        out = std::move(read);
        return true;
    }

    /// A list; `out` is null when it is not there.
    bool list(std::string_view key, const json::array*& out) const
    {
        out = nullptr;
        const auto* item = find(key);
        if (!item)
            return true;
        if (!item->is_array())
            return fail(key, "must be a list");
        out = &item->as_array();
        return true;
    }

    /// An object; `out` is null when it is not there.
    bool object(std::string_view key, const json::object*& out) const
    {
        out = nullptr;
        const auto* item = find(key);
        if (!item)
            return true;
        if (!item->is_object())
            return fail(key, "must be an object");
        out = &item->as_object();
        return true;
    }

private:
    const json::object& object_;
    std::string where_;
    std::string& problem_;
    mutable std::vector<std::string> asked_;
};

/// `ports[2]` and the like: where an element of a list is.
std::string element_path(const std::string& list, std::size_t index)
{
    return list + "[" + std::to_string(index) + "]";
}

/// Reads the serial settings of a port.  `null` leaves the defaults.
error_code serial_from_json(const json::value& value, const std::string& where,
                            serial_settings& out, std::string& problem)
{
    if (value.is_null())
        return {};
    if (!value.is_object())
    {
        problem = where + " must be an object";
        return make_error_code(errc::config_error);
    }

    const member_reader reader(value.as_object(), where, problem);
    bool good = true;

    // A number is checked against the range of its type here; validate()
    // decides which values the line may have.
    good = good && reader.whole("baud_rate", 0, 40'000'000, out.baud_rate);
    good = good && reader.whole("data_bits", 0, 255, out.data_bits);

    const auto named =
        [&reader, &good](std::string_view key, auto parse, auto& target)
    {
        std::string name;
        if (!good || !reader.has(key))
            return;
        good = reader.text(key, name);
        if (!good)
            return;
        const auto parsed = parse(name);
        if (!parsed)
            good = reader.fail(key, "is not a value that is known");
        else
            target = *parsed;
    };
    named(
        "parity", [](std::string_view v) { return parse_parity(v); },
        out.parity);
    named(
        "stop_bits", [](std::string_view v) { return parse_stop_bits(v); },
        out.stop_bits);
    named(
        "flow_control", [](std::string_view v)
        { return parse_flow_control(v); }, out.flow_control);

    std::int64_t read_timeout = out.read_timeout.count();
    std::int64_t write_timeout = out.write_timeout.count();
    constexpr std::int64_t max_int = std::numeric_limits<std::int64_t>::max();
    good = good && reader.whole("read_timeout_ms", 0, max_int, read_timeout);
    good = good && reader.whole("write_timeout_ms", 0, max_int, write_timeout);
    good = good && reader.flag("exclusive", out.exclusive);
    good = good && reader.done();
    if (!good)
        return make_error_code(errc::config_error);
    out.read_timeout = std::chrono::milliseconds(read_timeout);
    out.write_timeout = std::chrono::milliseconds(write_timeout);

    if (const error_code invalid = validate(out))
    {
        problem = where + " is not valid: " + invalid.message();
        return invalid;
    }
    return {};
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

bool latency_from_json(const member_reader& reader, link_entry& link)
{
    const auto limit = static_cast<std::int64_t>(max_link_delay_ms);
    return reader.whole("delay_ms", 0, limit, link.delay_ms) &&
           reader.whole("jitter_ms", 0, limit, link.jitter_ms);
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
bool half_duplex_from_json(const member_reader& reader, link_entry& link)
{
    const json::object* line = nullptr;
    if (!reader.object("half_duplex", line))
        return false;
    if (line == nullptr)
        return true;

    const member_reader fields = reader.sub(*line, reader.path("half_duplex"));
    half_duplex_options options;
    options.enabled = true;
    if (!fields.whole("bit_rate", 0,
                      static_cast<std::int64_t>(max_bus_bit_rate),
                      options.bit_rate) ||
        !fields.whole("turnaround_ms", 0,
                      static_cast<std::int64_t>(max_link_delay_ms),
                      options.turnaround_ms))
        return false;

    std::string collisions;
    if (!fields.text("collisions", collisions))
        return false;
    if (collisions == "garble")
        options.on_collision = collision_action::garble;
    else if (fields.has("collisions") && collisions != "report")
        return fields.fail("collisions", "must be \"report\" or \"garble\"");
    if (!fields.flag("rts_gating", options.rts_gating) || !fields.done())
        return false;

    link.half_duplex = options;
    return true;
}

/// Writes the wire timing of a link, and nothing when it is off.
void wire_timing_to_json(const link_entry& link, json::object& object)
{
    if (!link.wire_timing.enabled)
        return;
    json::object timing;
    if (link.wire_timing.bit_rate != 0)
        timing["bit_rate"] = link.wire_timing.bit_rate;
    if (link.wire_timing.framing)
        timing["framing"] = to_text(*link.wire_timing.framing);
    object["wire_timing"] = std::move(timing);
}

/// Reads the `wire_timing` object of a link.  Its presence turns the option
/// on; each member is optional.  Which links may have one, and the limits of
/// the numbers, are checked in validate().
bool wire_timing_from_json(const member_reader& reader, link_entry& link)
{
    const json::object* timing = nullptr;
    if (!reader.object("wire_timing", timing))
        return false;
    if (timing == nullptr)
        return true;

    const member_reader fields = reader.sub(*timing, reader.path("wire_timing"));
    wire_timing_options options;
    options.enabled = true;
    std::string framing;
    if (!fields.whole("bit_rate", 0,
                      static_cast<std::int64_t>(max_bus_bit_rate),
                      options.bit_rate) ||
        !fields.text("framing", framing))
        return false;
    if (fields.has("framing"))
    {
        options.framing = parse_framing(framing);
        if (!options.framing)
            return fields.fail("framing",
                               "must be like \"8N1\", \"7E2\" or \"5N1.5\"");
    }
    if (!fields.done())
        return false;

    link.wire_timing = options;
    return true;
}

/// Writes the `direction` of a monitor, and nothing when it is `both`.
void direction_to_json(const link_entry& link, json::object& object)
{
    if (link.direction != monitor_direction::both)
        object["direction"] = to_string(link.direction);
}

/// Reads `direction`.  Which links may have one other than `both` is checked
/// in validate().
bool direction_from_json(const member_reader& reader, link_entry& link)
{
    std::string text;
    if (!reader.text("direction", text))
        return false;
    if (!reader.has("direction") || text == "both")
        link.direction = monitor_direction::both;
    else if (text == "transmit")
        link.direction = monitor_direction::transmit;
    else if (text == "receive")
        link.direction = monitor_direction::receive;
    else
        return reader.fail("direction",
                           "must be \"both\", \"transmit\" or \"receive\"");
    return true;
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

/// Parses JSON text with every number read to the nearest double.  The
/// default reads a long number a few units in the last place off, and each
/// read and write of the same text then moves it again, so the text a document
/// writes would not settle.
json::value parse_exact(std::string_view text, boost::system::error_code& ec)
{
    json::parse_options options;
    options.numbers = json::number_precision::precise;
    return json::parse(text, ec, {}, options);
}

/// The parameters of a binding as a JSON object, or nothing when the text is
/// not one object inside the limits.
std::optional<json::object> binding_params(std::string_view text)
{
    if (text.size() > max_binding_params_size)
        return std::nullopt;
    boost::system::error_code ec;
    json::value parsed = parse_exact(text, ec);
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

/// Reads the `plugins` list of a port or a link.  A member of a binding that
/// is there must have the right type, and one that a binding does not have is
/// refused.  The values are checked in validate().
bool bindings_from_json(const member_reader& reader,
                        std::vector<plugin_binding>& out)
{
    const json::array* list = nullptr;
    if (!reader.list("plugins", list))
        return false;
    if (list == nullptr)
        return true;
    if (list->size() > max_plugin_bindings)
        return reader.fail("plugins", "has too many plugins");

    const std::string where = reader.path("plugins");
    for (std::size_t index = 0; index < list->size(); ++index)
    {
        const json::value& element = (*list)[index];
        const std::string here = element_path(where, index);
        if (!element.is_object())
            return reader.fail("plugins", "must be a list of objects");
        const member_reader entry = reader.sub(element.as_object(), here);

        plugin_binding binding;
        if (!entry.text("id", binding.id) ||
            !entry.text("kind", binding.kind) ||
            !entry.text("type", binding.type) ||
            !entry.text("name", binding.name) ||
            !entry.text("publisher", binding.publisher) ||
            !entry.text("key_id", binding.key_id) ||
            !entry.text("version", binding.version) ||
            !entry.text("get_it_from", binding.get_it_from) ||
            !entry.text("on_timeout", binding.on_timeout))
            return false;

        const json::object* params = nullptr;
        if (!entry.object("params", params) || !entry.done())
            return false;
        if (params != nullptr)
            binding.params = json::serialize(*params);
        out.push_back(std::move(binding));
    }
    return true;
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

/// `std::ios::noreplace` where the standard library has it (C++23): opening
/// then fails when the file is there.  Nothing where it does not, and the name
/// being new is what is relied on.
template <class Mode = std::ios_base>
constexpr std::ios_base::openmode exclusive_create_flag()
{
    if constexpr (requires { Mode::noreplace; })
        return Mode::noreplace;
    else
        return std::ios_base::in & std::ios_base::out;  // no bits set
}

/// `.saving-` and sixteen hex digits from the system's random source, then a
/// count: no two saves, in one process or two, make the same name.
std::string unique_saving_suffix()
{
    static std::atomic<unsigned> counter{0};
    std::random_device source;
    const std::uint64_t random =
        (std::uint64_t{source()} << 32) | std::uint64_t{source()};
    char digits[17];
    std::snprintf(digits, sizeof digits, "%016llx",
                  static_cast<unsigned long long>(random));
    return std::string(saving_file_marker) + digits + "-" +
           std::to_string(counter.fetch_add(1));
}

constexpr int rename_attempts = 6;
constexpr std::chrono::milliseconds rename_pause{50};

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

        // A pause is for a port made from an address.
        if (port.paused && (port.kind == "virtual" || port.kind == "mock"))
            return make_error_code(errc::config_error);

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
        // Only a monitor chooses which bytes of its port it copies.
        if (link.direction != monitor_direction::both && link.mode != "monitor")
            return make_error_code(errc::config_error);
        // A bus has a wire of its own.
        if (link.wire_timing.enabled &&
            (link.mode == "bus" || !valid_wire_timing(link.wire_timing)))
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
        if (port.paused)
            object["paused"] = true;
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
        wire_timing_to_json(link, object);
        direction_to_json(link, object);
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
    std::string problem;
    return from_json(text, problem);
}

result<document> from_json(std::string_view text, std::string& problem)
{
    VSS_PROFILE_ZONE("config.from_json");
    problem.clear();
    const auto refused = [] { return make_error_code(errc::config_error); };

    // The text must be valid JSON with an object at the top level.
    boost::system::error_code ec;
    const json::value root = parse_exact(text, ec);
    if (ec)
    {
        problem = "the text is not JSON: " + ec.message();
        return refused();
    }
    if (!root.is_object())
    {
        problem = "the setup must be one JSON object";
        return refused();
    }

    // A member that a setup does not have is refused (`done` at the end of
    // each object): a misspelt name would otherwise open as a setup without
    // it, and saving that setup would write the loss back over the file.
    const member_reader top(root.as_object(), {}, problem);

    // Only version 1 is understood, and the file must say so, as the JSON
    // view of the window requires.
    const json::value* version = top.find("version");
    if (version == nullptr)
    {
        problem = "version is missing; it must be 1";
        return refused();
    }
    if (!version->is_int64() || version->as_int64() != 1)
    {
        problem = "version must be 1";
        return refused();
    }

    // Name and description are optional; a missing name reads "untitled".
    document out;
    if (!top.text("name", out.name) ||
        !top.text("description", out.description))
        return refused();

    // Ports: the list is optional, but if present it must be an array of
    // objects.  Missing members take the defaults chosen here.
    const json::array* ports = nullptr;
    if (!top.list("ports", ports))
        return refused();

    if (ports != nullptr)
    {
        for (std::size_t index = 0; index < ports->size(); ++index)
        {
            const std::string where = element_path("ports", index);
            const json::value& element = (*ports)[index];
            if (!element.is_object())
            {
                problem = where + " must be an object";
                return refused();
            }
            const member_reader entry = top.sub(element.as_object(), where);

            port_entry port;
            if (!entry.text("name", port.name) ||
                !entry.text("kind", port.kind) ||
                !entry.text("uri", port.uri) ||
                !entry.text("virtual_kind", port.virtual_kind) ||
                !entry.text("mock_target", port.mock_target) ||
                !entry.flag("capture", port.capture) ||
                !entry.flag("paused", port.paused))
                return refused();

            // A com_number must be an integer in the allowed range.  Whether
            // the port may have one is decided later, in validate().
            if (!entry.whole("com_number", min_com_number, max_com_number,
                             port.com_number))
                return refused();

            if (const auto* serial = entry.find("serial"))
            {
                if (const error_code parse_error = serial_from_json(
                        *serial, entry.path("serial"), port.serial, problem))
                    return parse_error;
            }

            if (!bindings_from_json(entry, port.plugins) || !entry.done())
                return refused();

            out.ports.push_back(std::move(port));
        }
    }

    // Links follow the same rule as ports: optional, but an array of objects.
    const json::array* links = nullptr;
    if (!top.list("links", links))
        return refused();

    if (links != nullptr)
    {
        for (std::size_t index = 0; index < links->size(); ++index)
        {
            const std::string where = element_path("links", index);
            const json::value& element = (*links)[index];
            if (!element.is_object())
            {
                problem = where + " must be an object";
                return refused();
            }
            const member_reader entry = top.sub(element.as_object(), where);

            link_entry link;
            std::string policy = "interleave";
            if (!entry.text("mode", link.mode))
                return refused();

            // A bus has no modem lines, so absent means off there.
            link.emulate_modem_lines = link.mode != "bus";

            // A missing list reads as empty; a list of anything but strings
            // is an error.  Which lists a mode needs is checked in validate().
            if (!entry.flag("emulate_modem_lines", link.emulate_modem_lines) ||
                !entry.strings("sources", link.sources) ||
                !entry.strings("targets", link.targets) ||
                !entry.strings("members", link.members) ||
                !entry.text("policy", policy))
                return refused();

            // The merge policy is either owner or interleave (the default).
            if (policy == "owner")
                link.policy = merge_policy::owner;
            else if (policy == "interleave")
                link.policy = merge_policy::interleave;
            else
            {
                entry.fail("policy", "must be \"owner\" or \"interleave\"");
                return refused();
            }

            if (!latency_from_json(entry, link) ||
                !half_duplex_from_json(entry, link) ||
                !wire_timing_from_json(entry, link) ||
                !direction_from_json(entry, link) ||
                !bindings_from_json(entry, link.plugins) || !entry.done())
                return refused();

            out.links.push_back(std::move(link));
        }
    }

    if (!top.done())
        return refused();

    // Parsing only checks types; validate() checks that the document makes
    // sense as a whole, so a caller never receives an invalid document.
    if (const error_code invalid = validate(out))
    {
        problem = "the setup is not valid: " + invalid.message();
        return invalid;
    }
    return out;
}

result<document> load_file(const std::string& path)
{
    std::string problem;
    return load_file(path, problem);
}

result<document> load_file(const std::string& path, std::string& problem)
{
    problem.clear();

    // A topology is a small regular file. A directory, a device or a pipe would
    // open on some systems and then read as nothing, or never stop.
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error || status.type() == std::filesystem::file_type::not_found)
        return make_error_code(errc::not_found);
    if (!std::filesystem::is_regular_file(status))
        return make_error_code(errc::invalid_argument);

    // A real setup is far below the limit.  Refusing a larger file here keeps
    // a file picked by mistake from being read into memory.
    const std::uintmax_t size = std::filesystem::file_size(path, status_error);
    if (status_error)
        return make_error_code(errc::io_error);
    if (size > max_setup_file_size)
    {
        problem = "the file is larger than " +
                  std::to_string(max_setup_file_size / (std::uintmax_t{1024} * 1024)) +
                  " MiB, which is the most a setup file may be";
        return make_error_code(errc::config_error);
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return make_error_code(errc::not_found);

    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (stream.bad())
        return make_error_code(errc::io_error);
    text.resize(static_cast<std::size_t>(stream.gcount()));
    return from_json(text, problem);
}

result<void> save_file(const std::string& path, const document& value,
                       bool pretty)
{
    const auto text = to_json(value, pretty);
    if (!text)
        return text.error();

    // The text goes to a file beside the target and replaces it in one step,
    // so a failure at any point leaves the file that was there as it was.
    // A target that is a symbolic link is replaced where the link points.
    std::error_code fs_error;
    std::filesystem::path target(path);
    if (std::filesystem::is_symlink(target, fs_error))
    {
        auto resolved = std::filesystem::canonical(target, fs_error);
        if (!fs_error)
            target = std::move(resolved);
    }

    // A file that cannot be written to is refused, as it was when the target
    // was opened to be written.  Opening to append changes nothing in it.
    if (std::filesystem::exists(target, fs_error) &&
        !std::ofstream(target, std::ios::binary | std::ios::app))
        return make_error_code(errc::permission_denied);

    // The temporary file has a name that is new: a file of that name is never
    // truncated or reused, and two saves at once do not share one.
    std::filesystem::path temporary;
    std::ofstream stream;
    for (int attempt = 0; attempt < 4 && !stream.is_open(); ++attempt)
    {
        temporary = target;
        temporary += unique_saving_suffix();
        if (std::filesystem::exists(temporary, fs_error))
            continue;
        stream.open(temporary,
                    std::ios::out | std::ios::binary | exclusive_create_flag());
    }
    if (!stream.is_open())
        return make_error_code(errc::permission_denied);

    const auto abandon = [&temporary](errc reason)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return make_error_code(reason);
    };

    stream << *text;
    stream.flush();
    if (!stream)
        return abandon(errc::io_error);
    stream.close();
    if (stream.fail())
        return abandon(errc::io_error);

    // The file keeps the permissions it had.  A file that is not there yet
    // takes the ones a new file gets.
    const auto existing = std::filesystem::status(target, fs_error);
    if (!fs_error && std::filesystem::is_regular_file(existing))
        std::filesystem::permissions(temporary, existing.permissions(),
                                     fs_error);

    // Another program may hold the target for a moment, as a tool that
    // copies or scans a file after it changes does, and the rename is then
    // refused.  It is tried again for about a quarter of a second.
    for (int attempt = 1;; ++attempt)
    {
        std::filesystem::rename(temporary, target, fs_error);
        if (!fs_error || attempt == rename_attempts)
            break;
        std::this_thread::sleep_for(rename_pause);
    }
    if (fs_error)
        return abandon(fs_error == std::errc::permission_denied
                           ? errc::permission_denied
                           : errc::io_error);
    return {};
}

} // namespace vss::config
