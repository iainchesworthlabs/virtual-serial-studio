//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/rules/parse.hpp>

#include <vss/core/bytes.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss::rules
{

namespace json = boost::json;

namespace
{

/// A hostile document could name a million wrong keys; this many are listed.
constexpr std::size_t max_problems = 256;
/// The longest key or name quoted in a message.
constexpr std::size_t max_quoted = 64;
constexpr std::uint64_t max_time_us = 86'400'000'000ULL;
constexpr std::uint64_t no_limit = std::numeric_limits<std::uint64_t>::max();

template <class T> using named = std::pair<std::string_view, T>;

constexpr std::array<named<role>, 3> role_names{{{"peer", role::peer},
                                                 {"station", role::station},
                                                 {"inline", role::in_line}}};

constexpr std::array<named<ordering>, 2> order_names{
    {{"fifo", ordering::fifo}, {"independent", ordering::independent}}};

constexpr std::array<named<gap_where>, 3> gap_names{
    {{"before", gap_where::before},
     {"after", gap_where::after},
     {"both", gap_where::both}}};

constexpr std::array<named<port_event>, 2> port_names{
    {{"opened", port_event::opened}, {"closed", port_event::closed}}};

constexpr std::array<named<after_sequence>, 3> then_names{
    {{"repeat", after_sequence::repeat},
     {"hold", after_sequence::hold},
     {"stop", after_sequence::stop}}};

constexpr std::array<named<edge>, 5> edge_names{
    {{"match_end", edge::match_end},
     {"match_start", edge::match_start},
     {"previous", edge::previous},
     {"run_start", edge::run_start},
     {"cause", edge::cause}}};

constexpr std::array<named<precision>, 2> precision_names{
    {{"normal", precision::normal}, {"tight", precision::tight}}};

constexpr std::array<named<line_error>, 6> error_names{
    {{"parity", line_error::parity},
     {"framing", line_error::framing},
     {"overrun", line_error::overrun},
     {"break", line_error::break_signal},
     {"hang_up", line_error::hang_up},
     {"disconnect", line_error::disconnect}}};

constexpr std::array<named<control_line>, 7> line_names{
    {{"dtr", control_line::dtr},
     {"rts", control_line::rts},
     {"break", control_line::break_signal},
     {"cts", control_line::cts},
     {"dsr", control_line::dsr},
     {"dcd", control_line::dcd},
     {"ri", control_line::ri}}};

constexpr std::array<named<check_kind>, 5> check_names{
    {{"crc16_modbus", check_kind::crc16_modbus},
     {"crc16_ccitt", check_kind::crc16_ccitt},
     {"sum8", check_kind::sum8},
     {"xor8", check_kind::xor8},
     {"lrc", check_kind::lrc}}};

constexpr std::array<std::string_view, 13> action_keys{
    "reply",    "pass",     "hold",  "drop",       "duplicate",
    "corrupt",  "truncate", "lines", "line_error", "set_state",
    "set_slot", "mark",     "ask"};

constexpr std::array<std::string_view, 7> part_keys{
    "slot", "match", "crc16_modbus", "crc16_ccitt", "sum8", "xor8", "lrc"};

std::string join(const std::string& path, std::string_view key)
{
    if (path.empty())
        return std::string(key);
    return path + "." + std::string(key);
}

std::string index_of(const std::string& path, std::size_t index)
{
    return path + "[" + std::to_string(index) + "]";
}

/// A key or a name as a message quotes it: never longer than #max_quoted.
std::string shown(std::string_view text)
{
    if (text.size() <= max_quoted)
        return std::string(text);
    return std::string(text.substr(0, max_quoted)) + "...";
}

std::string_view view_of(const json::string& text)
{
    return std::string_view(text.data(), text.size());
}

template <class Range> std::string quoted_list(const Range& names)
{
    std::string out;
    for (const auto& name : names)
    {
        if (!out.empty())
            out += ", ";
        out += "\"" + std::string(name) + "\"";
    }
    return out;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// Everything the reader shares: the list of what is wrong so far.  Every
/// function reads what it can and reports the rest, so one pass finds all
/// the problems it can.
class reader
{
public:
    explicit reader(std::vector<problem>& problems) : problems_(problems) {}

    rule_set_source document(const json::object& root);

private:
    // -- problems and types --------------------------------------------------

    void bad(const std::string& path, std::string message)
    {
        if (full_)
            return;
        if (problems_.size() >= max_problems)
        {
            problems_.push_back(
                problem{"", "too many problems: the rest are not listed"});
            full_ = true;
            return;
        }
        problems_.push_back(problem{path, std::move(message)});
    }

    /// Reports each key of `object` that is not in `allowed`.  True when
    /// there was none.
    bool unknown_keys(const json::object& object,
                      std::initializer_list<std::string_view> allowed,
                      const std::string& path)
    {
        bool clean = true;
        for (const auto& entry : object)
        {
            const std::string_view key = entry.key();
            if (std::find(allowed.begin(), allowed.end(), key) != allowed.end())
                continue;
            bad(join(path, shown(key)), "unknown key");
            clean = false;
        }
        return clean;
    }

    const json::object* object_of(const json::value& value,
                                  const std::string& path)
    {
        if (const json::object* object = value.if_object())
            return object;
        bad(path, "must be an object");
        return nullptr;
    }

    /// The list, or null (reported) when it is not one or is over `limit`;
    /// then none of its items is read.
    const json::array* array_of(const json::value& value,
                                const std::string& path, std::size_t limit)
    {
        const json::array* list = value.if_array();
        if (list == nullptr)
        {
            bad(path, "must be a list");
            return nullptr;
        }
        if (list->size() > limit)
        {
            bad(path, "more than " + std::to_string(limit) + " items");
            return nullptr;
        }
        return list;
    }

    bool text_of(const json::value& value, const std::string& path,
                 std::string& out)
    {
        const json::string* text = value.if_string();
        if (text == nullptr)
        {
            bad(path, "must be a string");
            return false;
        }
        out.assign(text->data(), text->size());
        return true;
    }

    bool true_of(const json::value& value, const std::string& path)
    {
        const bool* flag = value.if_bool();
        if (flag == nullptr || !*flag)
        {
            bad(path, "must be true");
            return false;
        }
        return true;
    }

    /// A whole number from 0 to `limit`.
    bool whole_of(const json::value& value, const std::string& path,
                  std::uint64_t limit, std::uint64_t& out)
    {
        std::uint64_t number = 0;
        bool known = false;
        if (const std::uint64_t* big = value.if_uint64())
        {
            number = *big;
            known = true;
        }
        else if (const std::int64_t* small = value.if_int64())
        {
            if (*small >= 0)
            {
                number = static_cast<std::uint64_t>(*small);
                known = true;
            }
        }
        else
        {
            bad(path, "must be a whole number");
            return false;
        }
        if (!known || number > limit)
        {
            bad(path, limit == no_limit ? "must be a whole number, 0 or more"
                                        : "must be a whole number from 0 to " +
                                              std::to_string(limit));
            return false;
        }
        out = number;
        return true;
    }

    bool count_of(const json::value& value, const std::string& path,
                  std::uint64_t& out)
    {
        return whole_of(value, path, no_limit, out);
    }

    bool size_of(const json::value& value, const std::string& path,
                 std::size_t& out)
    {
        std::uint64_t number = 0;
        if (!count_of(value, path, number))
            return false;
        constexpr std::uint64_t largest =
            std::numeric_limits<std::size_t>::max();
        out = static_cast<std::size_t>(std::min(number, largest));
        return true;
    }

    bool time_of(const json::value& value, const std::string& path,
                 microseconds& out)
    {
        std::uint64_t number = 0;
        if (!whole_of(value, path, max_time_us, number))
            return false;
        out = microseconds(static_cast<std::int64_t>(number));
        return true;
    }

    template <class T, std::size_t N>
    bool choice_of(const json::value& value, const std::string& path,
                   const std::array<named<T>, N>& table, T& out)
    {
        const json::string* text = value.if_string();
        if (text != nullptr)
        {
            for (const auto& [name, each] : table)
            {
                if (view_of(*text) == name)
                {
                    out = each;
                    return true;
                }
            }
        }
        std::string names;
        for (std::size_t i = 0; i < table.size(); ++i)
        {
            names += i == 0 ? "" : (i + 1 == table.size() ? " or " : ", ");
            names += "\"" + std::string(table[i].first) + "\"";
        }
        bad(path, "must be " + names);
        return false;
    }

    // -- hex and patterns ----------------------------------------------------

    bool hex_of(const json::value& value, const std::string& path,
                byte_buffer& out)
    {
        const json::string* text = value.if_string();
        if (text == nullptr)
        {
            bad(path, "must be a string of hex bytes (like \"02 31 03\")");
            return false;
        }
        const auto parsed = from_hex(view_of(*text));
        if (!parsed)
        {
            bad(path, "is not hex bytes (like \"02 31 03\")");
            return false;
        }
        out = *parsed;
        return true;
    }

    /// A pattern: hex bytes, `??` for any byte, one `?` for a nibble that is
    /// ignored.  `wildcard` says whether any `?` was there.
    bool pattern_of(const json::value& value, const std::string& path,
                    byte_buffer& bytes, byte_buffer& mask, bool& wildcard)
    {
        const json::string* text = value.if_string();
        if (text == nullptr)
        {
            bad(path, "must be a string of hex bytes (like \"11 03 ?? 1?\")");
            return false;
        }
        const std::string_view all = view_of(*text);
        wildcard = all.find('?') != std::string_view::npos;
        if (!wildcard)
        {
            const auto parsed = from_hex(all);
            if (!parsed || parsed->empty())
            {
                bad(path, "is not hex bytes (like \"11 03 ?? 1?\")");
                return false;
            }
            bytes = *parsed;
            return true;
        }

        byte_buffer found;
        byte_buffer care;
        std::size_t at = 0;
        while (at < all.size())
        {
            if (is_blank(all[at]))
            {
                ++at;
                continue;
            }
            const std::size_t start = at;
            while (at < all.size() && !is_blank(all[at]))
                ++at;
            const std::string_view token = all.substr(start, at - start);
            if (token.size() != 2)
            {
                bad(path, "each byte is two characters: hex digits or ?");
                return false;
            }
            byte value_bits = 0;
            byte care_bits = 0;
            for (const char c : token)
            {
                const int digit = hex_value(c);
                if (digit < 0 && c != '?')
                {
                    bad(path, "each byte is two characters: hex digits or ?");
                    return false;
                }
                value_bits = static_cast<byte>((value_bits << 4U) |
                                               (digit < 0 ? 0 : digit));
                care_bits = static_cast<byte>((care_bits << 4U) |
                                              (digit < 0 ? 0x0 : 0xf));
            }
            found.push_back(value_bits);
            care.push_back(care_bits);
        }
        if (found.empty())
        {
            bad(path, "needs at least one byte");
            return false;
        }
        bytes = std::move(found);
        mask = std::move(care);
        return true;
    }

    bool range_of(const json::value& value, const std::string& path,
                  std::size_t& first, std::size_t& last)
    {
        const json::array* list = value.if_array();
        if (list == nullptr || list->size() != 2)
        {
            bad(path, "must be a list of two numbers: [first, last]");
            return false;
        }
        const bool low = size_of((*list)[0], index_of(path, 0), first);
        const bool high = size_of((*list)[1], index_of(path, 1), last);
        return low && high;
    }

    // -- the parts -----------------------------------------------------------

    std::vector<line_value> lines_of(const json::value& value,
                                     const std::string& path);
    trigger trigger_of(const json::value& value, const std::string& path);
    rule rule_of(const json::value& value, const std::string& path);
    step step_of(const json::object& object, const std::string& path);
    std::vector<action> actions_of(const json::value& value,
                                   const std::string& path);
    action action_of(const json::value& value, const std::string& path);
    void kind_of(const json::object& object, std::string_view kind,
                 const std::string& path, action& out);
    std::vector<reply_part> parts_of(const json::value& value,
                                     const std::string& path);
    fault_spec fault_of(const json::value& value, const std::string& path);
    fault_spec corrupt_of(const json::value& value, const std::string& path);
    void timing_of(const json::object& object, const std::string& path,
                   timing& out);
    void delay_of(const json::value& value, const std::string& path,
                  delay_spec& out);
    void table_of(const json::value& value, const std::string& path,
                  delay_spec& out);
    void attach_of(const json::value& value, attachment& out);
    timeline_entry entry_of(const json::value& value, const std::string& path);

    std::vector<problem>& problems_;
    bool full_ = false;
};

// -- the document ------------------------------------------------------------

rule_set_source reader::document(const json::object& root)
{
    rule_set_source out;
    unknown_keys(root,
                 {"version", "name", "attach", "seed", "late_after_us", "order",
                  "states", "signals", "slots", "rules", "timeline"},
                 "");

    if (const json::value* version = root.if_contains("version"))
    {
        const std::int64_t* number = version->if_int64();
        if (number == nullptr || *number != 1)
            bad("version", "must be 1");
    }
    if (const json::value* name = root.if_contains("name"))
        text_of(*name, "name", out.name);

    if (const json::value* attach = root.if_contains("attach"))
        attach_of(*attach, out.attach);
    else
        bad("attach", "the set needs an \"attach\": the port it sits at");

    if (const json::value* seed = root.if_contains("seed"))
    {
        if (const std::int64_t* signed_seed = seed->if_int64())
            out.seed = static_cast<std::uint64_t>(*signed_seed);
        else if (const std::uint64_t* big_seed = seed->if_uint64())
            out.seed = *big_seed;
        else
            bad("seed", "must be a whole number");
    }
    if (const json::value* late = root.if_contains("late_after_us"))
        time_of(*late, "late_after_us", out.late_after);
    if (const json::value* order = root.if_contains("order"))
        choice_of(*order, "order", order_names, out.order);

    if (const json::value* states = root.if_contains("states"))
    {
        if (const json::array* list = array_of(*states, "states", max_states))
        {
            for (std::size_t i = 0; i < list->size(); ++i)
            {
                std::string state;
                text_of((*list)[i], index_of("states", i), state);
                out.states.push_back(std::move(state));
            }
        }
    }

    if (const json::value* signals = root.if_contains("signals"))
    {
        if (const json::array* list =
                array_of(*signals, "signals", max_signals))
        {
            for (std::size_t i = 0; i < list->size(); ++i)
            {
                std::string signal;
                text_of((*list)[i], index_of("signals", i), signal);
                out.signals.push_back(std::move(signal));
            }
        }
    }

    if (const json::value* slots = root.if_contains("slots"))
    {
        if (const json::object* object = object_of(*slots, "slots"))
        {
            if (object->size() > max_slots)
            {
                bad("slots",
                    "more than " + std::to_string(max_slots) + " slots");
            }
            else
            {
                for (const auto& entry : *object)
                {
                    byte_buffer value;
                    const std::string key(entry.key());
                    hex_of(entry.value(), join("slots", shown(key)), value);
                    out.slots.emplace_back(key, std::move(value));
                }
            }
        }
    }

    if (const json::value* rules = root.if_contains("rules"))
    {
        if (const json::array* list = array_of(*rules, "rules", max_rules))
        {
            for (std::size_t i = 0; i < list->size(); ++i)
                out.rules.push_back(rule_of((*list)[i], index_of("rules", i)));
        }
    }

    if (const json::value* timeline = root.if_contains("timeline"))
    {
        if (const json::array* list =
                array_of(*timeline, "timeline", max_timeline))
        {
            for (std::size_t i = 0; i < list->size(); ++i)
                out.timeline.push_back(
                    entry_of((*list)[i], index_of("timeline", i)));
        }
    }
    return out;
}

void reader::attach_of(const json::value& value, attachment& out)
{
    const json::object* object = object_of(value, "attach");
    if (object == nullptr)
        return;

    const json::value* as = object->if_contains("role");
    bool known_role = false;
    if (as == nullptr)
        bad("attach.role", "is required: \"peer\", \"station\" or \"inline\"");
    else
        known_role = choice_of(*as, "attach.role", role_names, out.as);

    // Keys the role does not use are unknown keys, once the role is known.
    if (!known_role)
        unknown_keys(*object, {"role", "port", "from", "to"}, "attach");
    else if (out.as == role::in_line)
        unknown_keys(*object, {"role", "from", "to"}, "attach");
    else
        unknown_keys(*object, {"role", "port"}, "attach");

    if (known_role && out.as == role::in_line)
    {
        for (const char* key : {"from", "to"})
        {
            std::string& target =
                std::string_view(key) == "from" ? out.from : out.to;
            if (const json::value* item = object->if_contains(key))
                text_of(*item, join("attach", key), target);
            else
                bad(join("attach", key),
                    "is required: an inline set sits on one direction of a "
                    "link, from one port to another");
        }
    }
    else if (known_role)
    {
        if (const json::value* port = object->if_contains("port"))
            text_of(*port, "attach.port", out.port);
        else
            bad("attach.port", "is required: the port the set is attached to");
    }
}

// -- rules -------------------------------------------------------------------

std::vector<line_value> reader::lines_of(const json::value& value,
                                         const std::string& path)
{
    std::vector<line_value> out;
    const json::object* object = object_of(value, path);
    if (object == nullptr)
        return out;
    for (const auto& entry : *object)
    {
        const std::string_view key = entry.key();
        const auto found =
            std::find_if(line_names.begin(), line_names.end(),
                         [key](const auto& each) { return each.first == key; });
        const std::string where = join(path, shown(key));
        if (found == line_names.end())
        {
            bad(where,
                "unknown line: use " +
                    quoted_list(std::array<std::string_view, 7>{
                        "dtr", "rts", "break", "cts", "dsr", "dcd", "ri"}));
            continue;
        }
        const bool* high = entry.value().if_bool();
        if (high == nullptr)
        {
            bad(where, "must be true or false");
            continue;
        }
        out.push_back(line_value{found->second, *high});
    }
    return out;
}

trigger reader::trigger_of(const json::value& value, const std::string& path)
{
    trigger out;
    const json::object* object = object_of(value, path);
    if (object == nullptr)
        return out;
    unknown_keys(*object,
                 {"bytes", "mask", "gap_us", "gap", "lines", "port", "quiet_us",
                  "signal", "state"},
                 path);

    bool wildcard = false;
    if (const json::value* bytes = object->if_contains("bytes"))
        pattern_of(*bytes, join(path, "bytes"), out.bytes, out.mask, wildcard);
    if (const json::value* mask = object->if_contains("mask"))
    {
        byte_buffer given;
        if (hex_of(*mask, join(path, "mask"), given))
        {
            if (wildcard)
                bad(join(path, "mask"),
                    "cannot be used with \"?\" in \"bytes\": the ? says "
                    "which bits count");
            else if (given.empty())
                bad(join(path, "mask"), "needs at least one byte");
            else
                out.mask = std::move(given);
        }
    }
    if (const json::value* gap = object->if_contains("gap_us"))
        time_of(*gap, join(path, "gap_us"), out.gap);
    if (const json::value* where = object->if_contains("gap"))
        choice_of(*where, join(path, "gap"), gap_names, out.where);
    if (const json::value* lines = object->if_contains("lines"))
    {
        out.lines = lines_of(*lines, join(path, "lines"));
        if (lines->is_object() && lines->get_object().empty())
            bad(join(path, "lines"), "names no line");
    }
    if (const json::value* port = object->if_contains("port"))
    {
        port_event what = port_event::opened;
        if (choice_of(*port, join(path, "port"), port_names, what))
            out.port = what;
    }
    if (const json::value* quiet = object->if_contains("quiet_us"))
    {
        // Zero is how the model says "not a rule on silence", so it is
        // refused here and not left to look like a missing trigger.
        const std::string where = join(path, "quiet_us");
        if (time_of(*quiet, where, out.quiet) && out.quiet.count() == 0)
            bad(where, "must be at least 1");
    }
    if (const json::value* signal = object->if_contains("signal"))
        text_of(*signal, join(path, "signal"), out.signal);
    if (const json::value* state = object->if_contains("state"))
        text_of(*state, join(path, "state"), out.state);
    return out;
}

rule reader::rule_of(const json::value& value, const std::string& path)
{
    rule out;
    const json::object* object = object_of(value, path);
    if (object == nullptr)
        return out;
    unknown_keys(
        *object,
        {"name", "when", "do", "chance_ppm", "sequence", "then", "count"},
        path);

    if (const json::value* name = object->if_contains("name"))
        text_of(*name, join(path, "name"), out.name);
    else
        bad(join(path, "name"), "is required");
    if (const json::value* when = object->if_contains("when"))
        out.when = trigger_of(*when, join(path, "when"));
    else
        bad(join(path, "when"), "is required: what the rule waits for");

    const json::value* shorthand = object->if_contains("do");
    const json::value* sequence = object->if_contains("sequence");
    if (shorthand != nullptr && sequence != nullptr)
        bad(path, "a rule has a \"do\" list or a \"sequence\", not both");
    else if (shorthand == nullptr && sequence == nullptr)
        bad(path, "a rule needs a \"do\" list or a \"sequence\"");

    if (shorthand != nullptr && sequence == nullptr)
    {
        step one;
        out.written_as_do = true;
        one.actions = actions_of(*shorthand, join(path, "do"));
        if (const json::value* chance = object->if_contains("chance_ppm"))
        {
            std::uint64_t ppm = 0;
            if (whole_of(*chance, join(path, "chance_ppm"),
                         std::numeric_limits<std::uint32_t>::max(), ppm))
                one.chance_ppm = static_cast<std::uint32_t>(ppm);
        }
        out.sequence.push_back(std::move(one));
    }
    else if (sequence != nullptr)
    {
        if (object->if_contains("chance_ppm") != nullptr)
            bad(join(path, "chance_ppm"),
                "goes with \"do\" only: with a \"sequence\", each step has "
                "its own");
        const std::string where = join(path, "sequence");
        if (const json::array* list =
                array_of(*sequence, where, max_sequence_steps))
        {
            for (std::size_t i = 0; i < list->size(); ++i)
            {
                const std::string step_path = index_of(where, i);
                if (const json::object* each = object_of((*list)[i], step_path))
                    out.sequence.push_back(step_of(*each, step_path));
            }
        }
    }

    if (const json::value* then = object->if_contains("then"))
        choice_of(*then, join(path, "then"), then_names, out.then);
    if (const json::value* count = object->if_contains("count"))
        count_of(*count, join(path, "count"), out.count);
    return out;
}

step reader::step_of(const json::object& object, const std::string& path)
{
    step out;
    unknown_keys(object, {"do", "chance_ppm"}, path);
    if (const json::value* list = object.if_contains("do"))
        out.actions = actions_of(*list, join(path, "do"));
    if (const json::value* chance = object.if_contains("chance_ppm"))
    {
        std::uint64_t ppm = 0;
        if (whole_of(*chance, join(path, "chance_ppm"),
                     std::numeric_limits<std::uint32_t>::max(), ppm))
            out.chance_ppm = static_cast<std::uint32_t>(ppm);
    }
    return out;
}

timeline_entry reader::entry_of(const json::value& value,
                                const std::string& path)
{
    timeline_entry out;
    const json::object* object = object_of(value, path);
    if (object == nullptr)
        return out;
    unknown_keys(*object, {"at_us", "do"}, path);
    if (const json::value* at = object->if_contains("at_us"))
        time_of(*at, join(path, "at_us"), out.at);
    if (const json::value* list = object->if_contains("do"))
        out.actions = actions_of(*list, join(path, "do"));
    return out;
}

// -- actions -----------------------------------------------------------------

std::vector<action> reader::actions_of(const json::value& value,
                                       const std::string& path)
{
    std::vector<action> out;
    const json::array* list = array_of(value, path, max_actions);
    if (list == nullptr)
        return out;
    out.reserve(list->size());
    for (std::size_t i = 0; i < list->size(); ++i)
        out.push_back(action_of((*list)[i], index_of(path, i)));
    return out;
}

action reader::action_of(const json::value& value, const std::string& path)
{
    action out;
    const json::object* object = object_of(value, path);
    if (object == nullptr)
        return out;
    unknown_keys(*object,
                 {"reply", "pass", "hold", "drop", "duplicate", "corrupt",
                  "truncate", "lines", "line_error", "set_state", "set_slot",
                  "mark", "ask", "fault", "after", "delay", "precision"},
                 path);

    std::vector<std::string_view> present;
    for (const std::string_view key : action_keys)
    {
        if (object->if_contains(key) != nullptr)
            present.push_back(key);
    }
    if (present.empty())
        bad(path, "an action needs one of " + quoted_list(action_keys));
    else if (present.size() > 1)
        bad(path, "an action has one of " + quoted_list(action_keys) +
                      ", not several");
    else
        kind_of(*object, present.front(), path, out);

    if (present.size() == 1 && present.front() != "reply" &&
        object->if_contains("fault") != nullptr)
        bad(join(path, "fault"),
            "goes with \"reply\" only; the other actions name their fault "
            "(\"drop\", \"corrupt\", ...)");

    timing_of(*object, path, out.when);
    return out;
}

void reader::kind_of(const json::object& object, std::string_view kind,
                     const std::string& path, action& out)
{
    const json::value& value = *object.if_contains(kind);
    const std::string where = join(path, kind);

    if (kind == "reply")
    {
        out.kind = action_kind::reply;
        out.parts = parts_of(value, where);
        if (const json::value* fault = object.if_contains("fault"))
            out.fault = fault_of(*fault, join(path, "fault"));
    }
    else if (kind == "pass" || kind == "hold")
    {
        out.kind = action_kind::pass;
        true_of(value, where);
    }
    else if (kind == "drop" || kind == "duplicate")
    {
        out.kind = action_kind::pass;
        if (true_of(value, where))
            out.fault.kind =
                kind == "drop" ? fault_kind::drop : fault_kind::duplicate;
    }
    else if (kind == "corrupt")
    {
        out.kind = action_kind::pass;
        out.fault = corrupt_of(value, where);
    }
    else if (kind == "truncate")
    {
        out.kind = action_kind::pass;
        if (size_of(value, where, out.fault.keep))
            out.fault.kind = fault_kind::truncate;
    }
    else if (kind == "lines")
    {
        out.kind = action_kind::set_lines;
        out.lines = lines_of(value, where);
    }
    else if (kind == "line_error")
    {
        out.kind = action_kind::raise_error;
        choice_of(value, where, error_names, out.error);
    }
    else if (kind == "set_state")
    {
        out.kind = action_kind::set_state;
        text_of(value, where, out.state);
    }
    else if (kind == "set_slot")
    {
        out.kind = action_kind::set_slot;
        if (const json::object* body = object_of(value, where))
        {
            unknown_keys(*body, {"slot", "match"}, where);
            if (const json::value* slot = body->if_contains("slot"))
                text_of(*slot, join(where, "slot"), out.slot);
            else
                bad(join(where, "slot"), "is required");
            if (const json::value* match = body->if_contains("match"))
                range_of(*match, join(where, "match"), out.first, out.last);
            else
                bad(join(where, "match"),
                    "is required: [first, last] bytes of the match");
        }
    }
    else if (kind == "mark")
    {
        out.kind = action_kind::mark;
        text_of(value, where, out.text);
    }
    else
    {
        out.kind = action_kind::ask;
        if (const json::object* body = object_of(value, where))
        {
            unknown_keys(*body, {"timeout_us", "on_timeout"}, where);
            if (const json::value* limit = body->if_contains("timeout_us"))
                time_of(*limit, join(where, "timeout_us"), out.timeout);
            else
                bad(join(where, "timeout_us"), "is required");
            if (const json::value* list = body->if_contains("on_timeout"))
                out.on_timeout = actions_of(*list, join(where, "on_timeout"));
        }
    }
}

std::vector<reply_part> reader::parts_of(const json::value& value,
                                         const std::string& path)
{
    std::vector<reply_part> out;
    const json::array* list = array_of(value, path, max_reply_parts);
    if (list == nullptr)
        return out;
    for (std::size_t i = 0; i < list->size(); ++i)
    {
        const json::value& item = (*list)[i];
        const std::string where = index_of(path, i);
        reply_part part;
        if (item.is_string())
        {
            part.kind = part_kind::literal;
            hex_of(item, where, part.bytes);
        }
        else if (const json::object* object = item.if_object())
        {
            const bool clean =
                unknown_keys(*object,
                             {"slot", "match", "crc16_modbus", "crc16_ccitt",
                              "sum8", "xor8", "lrc"},
                             where);
            std::vector<std::string_view> present;
            for (const std::string_view key : part_keys)
            {
                if (object->if_contains(key) != nullptr)
                    present.push_back(key);
            }
            if (present.size() != 1)
            {
                if (clean)
                    bad(where,
                        "a part has exactly one of " + quoted_list(part_keys));
            }
            else
            {
                const json::value& body = *object->if_contains(present[0]);
                const std::string key_path = join(where, present[0]);
                if (present[0] == "slot")
                {
                    part.kind = part_kind::slot;
                    text_of(body, key_path, part.slot);
                }
                else if (present[0] == "match")
                {
                    part.kind = part_kind::match;
                    range_of(body, key_path, part.first, part.last);
                }
                else
                {
                    part.kind = part_kind::check;
                    for (const auto& [name, each] : check_names)
                    {
                        if (name == present[0])
                            part.check = each;
                    }
                    const json::string* word = body.if_string();
                    if (word != nullptr && view_of(*word) == "reply")
                        part.whole = true;
                    else if (body.is_array())
                    {
                        part.whole = false;
                        range_of(body, key_path, part.first, part.last);
                    }
                    else
                        bad(key_path,
                            "must be \"reply\" or a range [first, last] of "
                            "the reply");
                }
            }
        }
        else
        {
            bad(where, "must be a string of hex bytes or an object");
        }
        out.push_back(std::move(part));
    }
    return out;
}

fault_spec reader::corrupt_of(const json::value& value, const std::string& path)
{
    fault_spec out;
    const json::object* object = object_of(value, path);
    if (object == nullptr)
        return out;
    unknown_keys(*object, {"at", "xor"}, path);
    bool at_ok = false;
    bool xor_ok = false;
    if (const json::value* at = object->if_contains("at"))
        at_ok = size_of(*at, join(path, "at"), out.at);
    else
        bad(join(path, "at"), "is required: the first byte to change");
    if (const json::value* bits = object->if_contains("xor"))
        xor_ok = hex_of(*bits, join(path, "xor"), out.xor_with);
    else
        bad(join(path, "xor"), "is required: the bytes to xor in");
    if (at_ok && xor_ok)
        out.kind = fault_kind::corrupt;
    return out;
}

fault_spec reader::fault_of(const json::value& value, const std::string& path)
{
    fault_spec out;
    const std::string choices =
        "must be \"drop\", \"duplicate\", {\"corrupt\": {...}} or "
        "{\"truncate\": n}";
    if (const json::string* word = value.if_string())
    {
        if (view_of(*word) == "drop")
            out.kind = fault_kind::drop;
        else if (view_of(*word) == "duplicate")
            out.kind = fault_kind::duplicate;
        else
            bad(path, choices);
        return out;
    }
    const json::object* object = value.if_object();
    if (object == nullptr)
    {
        bad(path, choices);
        return out;
    }
    const bool clean = unknown_keys(*object, {"corrupt", "truncate"}, path);
    if (object->size() != 1)
    {
        if (clean)
            bad(path, choices);
        return out;
    }
    if (const json::value* body = object->if_contains("corrupt"))
    {
        out = corrupt_of(*body, join(path, "corrupt"));
    }
    else if (const json::value* keep = object->if_contains("truncate"))
    {
        if (size_of(*keep, join(path, "truncate"), out.keep))
            out.kind = fault_kind::truncate;
    }
    return out;
}

// -- time --------------------------------------------------------------------

void reader::timing_of(const json::object& object, const std::string& path,
                       timing& out)
{
    if (const json::value* after = object.if_contains("after"))
        choice_of(*after, join(path, "after"), edge_names, out.after);
    if (const json::value* delay = object.if_contains("delay"))
        delay_of(*delay, join(path, "delay"), out.delay);
    if (const json::value* how = object.if_contains("precision"))
        choice_of(*how, join(path, "precision"), precision_names, out.how);
}

void reader::delay_of(const json::value& value, const std::string& path,
                      delay_spec& out)
{
    const json::object* object = object_of(value, path);
    if (object == nullptr)
        return;
    const std::array<std::string_view, 5> kinds{"fixed_us", "uniform", "normal",
                                                "exponential", "table"};
    const bool clean = unknown_keys(
        *object, {"fixed_us", "uniform", "normal", "exponential", "table"},
        path);
    if (object->size() != 1)
    {
        if (clean)
            bad(path, "a delay has exactly one of " + quoted_list(kinds));
        return;
    }

    if (const json::value* fixed = object->if_contains("fixed_us"))
    {
        out.kind = delay_kind::fixed;
        time_of(*fixed, join(path, "fixed_us"), out.fixed);
        return;
    }
    if (const json::value* body = object->if_contains("table"))
    {
        out.kind = delay_kind::table;
        table_of(*body, join(path, "table"), out);
        return;
    }

    // The three that are given as numbers: which are needed, and where each
    // goes.
    struct member
    {
        std::string_view key;
        microseconds delay_spec::* field;
        bool required;
    };
    std::vector<member> members;
    const char* name = "uniform";
    if (object->if_contains("uniform") != nullptr)
    {
        out.kind = delay_kind::uniform;
        members = {{"min_us", &delay_spec::min, true},
                   {"max_us", &delay_spec::max, true}};
    }
    else if (object->if_contains("normal") != nullptr)
    {
        name = "normal";
        out.kind = delay_kind::normal;
        members = {{"mean_us", &delay_spec::mean, true},
                   {"sd_us", &delay_spec::sd, true},
                   {"min_us", &delay_spec::min, true},
                   {"max_us", &delay_spec::max, true}};
    }
    else
    {
        name = "exponential";
        out.kind = delay_kind::exponential;
        members = {{"mean_us", &delay_spec::mean, true},
                   {"max_us", &delay_spec::max, true},
                   {"min_us", &delay_spec::min, false}};
    }

    // A delay that names none of the kinds (a misspelt key) was already
    // reported as an unknown key, and has nothing more to read.
    const json::value* named = object->if_contains(name);
    if (named == nullptr)
        return;
    const std::string where = join(path, name);
    const json::object* body = object_of(*named, where);
    if (body == nullptr)
        return;
    for (const auto& entry : *body)
    {
        const std::string_view key = entry.key();
        const bool known =
            std::any_of(members.begin(), members.end(),
                        [key](const member& each) { return each.key == key; });
        if (!known)
            bad(join(where, shown(key)), "unknown key");
    }
    for (const member& each : members)
    {
        const std::string key_path = join(where, each.key);
        if (const json::value* item = body->if_contains(each.key))
            time_of(*item, key_path, out.*(each.field));
        else if (each.required)
            bad(key_path, "is required");
    }
}

void reader::table_of(const json::value& value, const std::string& path,
                      delay_spec& out)
{
    const json::object* object = object_of(value, path);
    if (object == nullptr)
        return;
    unknown_keys(*object, {"values_us", "pick"}, path);
    if (const json::value* values = object->if_contains("values_us"))
    {
        const std::string where = join(path, "values_us");
        if (const json::array* list = array_of(*values, where, max_delay_table))
        {
            for (std::size_t i = 0; i < list->size(); ++i)
            {
                microseconds one{0};
                time_of((*list)[i], index_of(where, i), one);
                out.values.push_back(one);
            }
        }
    }
    else
    {
        bad(join(path, "values_us"), "is required");
    }
    if (const json::value* pick = object->if_contains("pick"))
    {
        static constexpr std::array<named<bool>, 2> picks{
            {{"in_order", true}, {"random", false}}};
        choice_of(*pick, join(path, "pick"), picks, out.in_order);
    }
}

} // namespace

result<rule_set_source> parse_rule_set(std::string_view text,
                                       std::vector<problem>* problems)
{
    std::vector<problem> found;
    rule_set_source source;
    bool readable = false;

    if (text.size() > max_document_bytes)
    {
        found.push_back(problem{"", "the document is over " +
                                        std::to_string(max_document_bytes) +
                                        " bytes"});
    }
    else
    {
        json::parse_options options;
        options.max_depth = max_document_depth;
        boost::system::error_code failed;
        const json::value root = json::parse(text, failed, {}, options);
        if (failed == json::error::too_deep)
        {
            found.push_back(problem{"", "nested more than " +
                                            std::to_string(max_document_depth) +
                                            " levels deep"});
        }
        else if (failed)
        {
            found.push_back(problem{"", "not JSON: " + failed.message()});
        }
        else if (!root.is_object())
        {
            found.push_back(problem{"", "the document must be a JSON object"});
        }
        else
        {
            source = reader(found).document(root.get_object());
            readable = true;
        }
    }

    const bool ok = readable && found.empty();
    if (problems != nullptr)
        *problems = std::move(found);
    if (!ok)
        return make_error_code(errc::config_error);
    return source;
}

result<std::shared_ptr<const compiled_rule_set>>
compile_rule_set(std::string_view text, std::vector<problem>* problems)
{
    auto source = parse_rule_set(text, problems);
    if (!source)
        return source.error();
    return compile(*source, problems);
}

} // namespace vss::rules
