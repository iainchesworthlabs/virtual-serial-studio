//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The writer and the reader of a traffic recording.  The format is in
// recording.hpp and docs/plugins.md, section 19.6.
//
// Where section 19.6 is silent, this code takes the strictest reading, and
// each such choice is named where it is made: "Choice".
//

#include <vss/plugin/host/recording.hpp>

#include <vss/plugin/manifest.hpp>

#include <boost/hash2/sha2.hpp>
#include <boost/json.hpp>

// The strict reader of the plugin library: one JSON document, no repeated key,
// and the rules for an id, a version and a text.  It is not installed, so the
// build gives this target the library's source folder.
#include "reader.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss::plugin::host
{

namespace json = boost::json;
namespace fs = std::filesystem;
using namespace std::string_view_literals;

// -- errors -------------------------------------------------------------------

namespace
{

class recording_category_impl final : public boost::system::error_category
{
public:
    const char* name() const noexcept override { return "vss.recording"; }

    std::string message(int value) const override
    {
        switch (static_cast<recording_errc>(value))
        {
        case recording_errc::success:
            return "success";
        case recording_errc::invalid_argument:
            return "a value of the recording is not allowed";
        case recording_errc::folder_not_usable:
            return "the folder for the recording is not an empty folder";
        case recording_errc::io_failed:
            return "a file of the recording could not be written or read";
        case recording_errc::writer_failed:
            return "an earlier write failed, so nothing more is written";
        case recording_errc::finished:
            return "the recording is finished";
        case recording_errc::time_went_back:
            return "a time is lower than the one before it";
        case recording_errc::not_built:
            return "not built";
        case recording_errc::index_invalid:
            return "the recording's index is not valid";
        case recording_errc::format_unsupported:
            return "the recording needs a newer version";
        case recording_errc::place_invalid:
            return "a recorded place's file is not valid";
        case recording_errc::layout_invalid:
            return "the recording's folder breaks the layout rules";
        case recording_errc::file_missing:
            return "a file of the recording is missing";
        case recording_errc::size_mismatch:
            return "a recorded place does not hold the traffic the index says";
        case recording_errc::hash_mismatch:
            return "a recorded place's file is not the one the index lists";
        }
        return "unknown recording error";
    }
};

} // namespace

const boost::system::error_category& recording_category() noexcept
{
    static const recording_category_impl category;
    return category;
}

boost::system::error_code make_error_code(recording_errc value) noexcept
{
    return {static_cast<int>(value), recording_category()};
}

std::string_view to_string(recording_errc value) noexcept
{
    switch (value)
    {
    case recording_errc::success:
        return "success";
    case recording_errc::invalid_argument:
        return "invalid_argument";
    case recording_errc::folder_not_usable:
        return "folder_not_usable";
    case recording_errc::io_failed:
        return "io_failed";
    case recording_errc::writer_failed:
        return "writer_failed";
    case recording_errc::finished:
        return "finished";
    case recording_errc::time_went_back:
        return "time_went_back";
    case recording_errc::not_built:
        return "not_built";
    case recording_errc::index_invalid:
        return "index_invalid";
    case recording_errc::format_unsupported:
        return "format_unsupported";
    case recording_errc::place_invalid:
        return "place_invalid";
    case recording_errc::layout_invalid:
        return "layout_invalid";
    case recording_errc::file_missing:
        return "file_missing";
    case recording_errc::size_mismatch:
        return "size_mismatch";
    case recording_errc::hash_mismatch:
        return "hash_mismatch";
    }
    return "unknown";
}

std::string_view to_string(place_kind value) noexcept
{
    return value == place_kind::port ? "port"sv : "link"sv;
}

// -- shared helpers -----------------------------------------------------------

namespace
{

constexpr std::size_t max_name_bytes = 256;
constexpr std::size_t max_settings_bytes = 256;
constexpr std::size_t max_fault_bytes = 64;
constexpr std::size_t max_json_depth = 8;
constexpr std::size_t read_block_bytes = 65'536;
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;

/// The span of a ring's segment.  A place's file is kept as one file for each
/// of these, and the oldest goes when a newer one starts.
constexpr std::uint64_t ring_segment_ns = 60 * nanoseconds_per_second;

constexpr std::string_view index_temp_name = "recording.json.tmp"sv;
constexpr std::string_view hex_digits = "0123456789abcdef"sv;
constexpr std::string_view only_way_to_redact = "blanked"sv;

/// The names of the six lines in the order the writer lists them.
constexpr std::array line_names{std::pair{control_line::dtr, "dtr"sv},
                                std::pair{control_line::rts, "rts"sv},
                                std::pair{control_line::cts, "cts"sv},
                                std::pair{control_line::dsr, "dsr"sv},
                                std::pair{control_line::dcd, "dcd"sv},
                                std::pair{control_line::ri, "ri"sv}};

constexpr unsigned all_six_lines =
    line_mask(control_line::dtr) | line_mask(control_line::rts) |
    line_mask(control_line::cts) | line_mask(control_line::dsr) |
    line_mask(control_line::dcd) | line_mask(control_line::ri);

using sha256_hasher = boost::hash2::sha2_256;

/// The parts one after the other as one text, for a message.
std::string words(std::initializer_list<std::string_view> parts)
{
    std::string out;
    for (const std::string_view part : parts)
        out.append(part);
    return out;
}

void append_number(std::string& out, std::uint64_t value)
{
    std::array<char, 24> digits{};
    const auto done =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    out.append(digits.data(), done.ptr);
}

/// `value` in decimal, padded with zeros to `width` digits.
void append_padded(std::string& out, std::uint64_t value, std::size_t width)
{
    std::array<char, 24> digits{};
    const auto done =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    const auto length = static_cast<std::size_t>(done.ptr - digits.data());
    if (length < width)
        out.append(width - length, '0');
    out.append(digits.data(), length);
}

void append_hex(std::string& out, bytes_view bytes)
{
    for (const byte value : bytes)
    {
        out.push_back(hex_digits[static_cast<std::size_t>(value >> 4)]);
        out.push_back(hex_digits[static_cast<std::size_t>(value & 0x0fu)]);
    }
}

/// `text` as a JSON string.  Only text that passed the checks below is given
/// to this, so the escapes are the ones the format needs: the quote, the
/// backslash and the three white space characters rules may hold.
void append_json_text(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (const char letter : text)
    {
        switch (letter)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            out.push_back(letter);
            break;
        }
    }
    out.push_back('"');
}

bool valid_utf8(std::string_view text) noexcept
{
    std::size_t at = 0;
    while (at < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[at]);
        if (lead < 0x80)
        {
            ++at;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code = 0;
        std::uint32_t least = 0;
        if (lead >= 0xc2 && lead <= 0xdf)
        {
            extra = 1;
            code = lead & 0x1fu;
            least = 0x80;
        }
        else if ((lead & 0xf0u) == 0xe0u)
        {
            extra = 2;
            code = lead & 0x0fu;
            least = 0x800;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            extra = 3;
            code = lead & 0x07u;
            least = 0x10000;
        }
        else
        {
            return false;
        }
        if (text.size() - at <= extra)
            return false;
        for (std::size_t i = 1; i <= extra; ++i)
        {
            const auto next = static_cast<unsigned char>(text[at + i]);
            if ((next & 0xc0u) != 0x80u)
                return false;
            code = (code << 6) | (next & 0x3fu);
        }
        if (code < least || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff))
            return false;
        at += extra + 1;
    }
    return true;
}

/// Plain text: valid UTF-8, no control character and no mark that changes the
/// direction of the text.
bool plain_text(std::string_view text) noexcept
{
    return valid_utf8(text) && plugin::detail::clean_text(text);
}

/// Plain text that may also hold line feeds, carriage returns and tabs.
bool plain_multiline_text(std::string_view text)
{
    std::string plain;
    plain.reserve(text.size());
    for (const char letter : text)
    {
        if (letter != '\n' && letter != '\r' && letter != '\t')
            plain.push_back(letter);
    }
    return plain_text(plain);
}

std::string sha256_text(sha256_hasher& hasher)
{
    const auto digest = hasher.result();
    std::string out;
    out.reserve(64);
    for (const unsigned char value : digest)
    {
        out.push_back(hex_digits[static_cast<std::size_t>(value >> 4)]);
        out.push_back(hex_digits[static_cast<std::size_t>(value & 0x0fu)]);
    }
    return out;
}

char direction_letter(data_direction direction) noexcept
{
    return direction == data_direction::rx ? 'R' : 'W';
}

std::string place_file_name(std::size_t index)
{
    std::string name;
    append_number(name, index + 1);
    name.append(".jsonl");
    return name;
}

std::string lower_ascii(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](char letter)
                           {
                               return letter >= 'A' && letter <= 'Z'
                                          ? static_cast<char>(letter + 32)
                                          : letter;
                           });
    return out;
}

// -- the lines of a place file ------------------------------------------------

void begin_line(std::string& out, std::uint64_t t_ns, std::string_view kind)
{
    out.clear();
    out.append("{\"t_ns\":");
    append_number(out, t_ns);
    out.append(",\"kind\":\"");
    out.append(kind);
    out.push_back('"');
}

void end_line(std::string& out)
{
    out.append("}\n");
}

void format_data(std::string& out, std::uint64_t t_ns, data_direction direction,
                 bytes_view bytes)
{
    begin_line(out, t_ns, "data");
    out.append(",\"direction\":\"");
    out.push_back(direction_letter(direction));
    out.append("\",\"bytes\":");
    append_number(out, bytes.size());
    out.append(",\"hex\":\"");
    append_hex(out, bytes);
    out.push_back('"');
    end_line(out);
}

void format_line_change(std::string& out, std::uint64_t t_ns,
                        control_lines lines, control_lines changed)
{
    begin_line(out, t_ns, "line");
    out.append(",\"lines\":{");
    bool first = true;
    for (const auto& [line, name] : line_names)
    {
        if (!first)
            out.push_back(',');
        first = false;
        out.push_back('"');
        out.append(name);
        out.append("\":");
        out.append(lines.test(line) ? "true" : "false");
    }
    out.append("},\"changed\":[");
    first = true;
    for (const auto& [line, name] : line_names)
    {
        if (!changed.test(line))
            continue;
        if (!first)
            out.push_back(',');
        first = false;
        out.push_back('"');
        out.append(name);
        out.push_back('"');
    }
    out.push_back(']');
    end_line(out);
}

void format_fault(std::string& out, std::uint64_t t_ns, std::string_view fault)
{
    begin_line(out, t_ns, "fault");
    out.append(",\"fault\":");
    append_json_text(out, fault);
    end_line(out);
}

void format_dropped(std::string& out, std::uint64_t t_ns, std::uint64_t lost)
{
    begin_line(out, t_ns, "dropped");
    out.append(",\"bytes\":");
    append_number(out, lost);
    end_line(out);
}

// -- the index ----------------------------------------------------------------

/// What `finish` knows of a place that the options do not say.
struct place_totals
{
    std::uint64_t bytes = 0;
    std::string sha256;
};

std::string origin_text(std::chrono::sys_seconds origin)
{
    const auto day = std::chrono::floor<std::chrono::days>(origin);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss<std::chrono::seconds> time_of_day{origin - day};
    std::string out;
    append_padded(out,
                  static_cast<std::uint64_t>(static_cast<int>(date.year())), 4);
    out.push_back('-');
    append_padded(out, static_cast<unsigned>(date.month()), 2);
    out.push_back('-');
    append_padded(out, static_cast<unsigned>(date.day()), 2);
    out.push_back('T');
    append_padded(out, static_cast<std::uint64_t>(time_of_day.hours().count()),
                  2);
    out.push_back(':');
    append_padded(out,
                  static_cast<std::uint64_t>(time_of_day.minutes().count()), 2);
    out.push_back(':');
    append_padded(out,
                  static_cast<std::uint64_t>(time_of_day.seconds().count()), 2);
    out.push_back('Z');
    return out;
}

/// The index as the writer writes it: members in the order of section 19.6,
/// no spaces, and a line feed at the end.
std::string render_index(const recording_options& options,
                         std::uint64_t duration_ns, std::uint64_t dropped,
                         const std::vector<place_totals>& totals)
{
    std::string out;
    out.append("{\"recording_format\":");
    append_number(out, recording_format_number);
    out.append(",\"origin_utc\":\"");
    out.append(origin_text(options.origin_utc));
    out.append("\",\"time_unit\":\"ns\",\"duration_ns\":");
    append_number(out, duration_ns);
    out.append(",\"seed\":");
    append_number(out, options.seed);
    out.append(",\"places\":[");
    for (std::size_t i = 0; i < options.places.size(); ++i)
    {
        const place_spec& place = options.places[i];
        if (i != 0)
            out.push_back(',');
        out.append("{\"file\":");
        append_json_text(out, place_file_name(i));
        out.append(",\"kind\":\"");
        out.append(to_string(place.kind));
        out.append("\",\"name\":");
        append_json_text(out, place.name);
        out.append(",\"settings\":");
        append_json_text(out, place.settings);
        out.append(",\"bytes\":");
        append_number(out, totals[i].bytes);
        out.append(",\"sha256\":\"");
        out.append(totals[i].sha256);
        out.append("\"}");
    }
    out.append("],\"rule_sets\":[");
    for (std::size_t i = 0; i < options.rule_sets.size(); ++i)
    {
        if (i != 0)
            out.push_back(',');
        out.append("{\"generation\":");
        append_number(out, options.rule_sets[i].generation);
        out.append(",\"text\":");
        append_json_text(out, options.rule_sets[i].text);
        out.push_back('}');
    }
    out.append("],\"redactions\":[");
    for (std::size_t i = 0; i < options.redactions.size(); ++i)
    {
        const recording_redaction& redaction = options.redactions[i];
        if (i != 0)
            out.push_back(',');
        out.append("{\"place\":");
        append_json_text(out, redaction.place);
        out.append(",\"from_ns\":");
        append_number(out, redaction.from_ns);
        out.append(",\"to_ns\":");
        append_number(out, redaction.to_ns);
        out.append(",\"how\":");
        append_json_text(out, redaction.how);
        out.push_back('}');
    }
    out.append("],\"dropped\":");
    append_number(out, dropped);
    out.append(",\"made_by\":{\"plugin\":");
    append_json_text(out, options.made_by.plugin);
    out.append(",\"version\":");
    append_json_text(out, options.made_by.version);
    out.append("}}\n");
    return out;
}

/// The first thing wrong with the options, as words; empty when there is none.
std::string problem_with(const recording_options& options)
{
    const std::vector<place_spec>& places = options.places;
    if (places.empty() || places.size() > recording_max_places)
        return "a recording has 1 to 15 places";
    std::set<std::string_view> names;
    for (const place_spec& place : places)
    {
        if (place.name.empty() || place.name.size() > max_name_bytes ||
            !plain_text(place.name))
            return "a place's name is not 1 to 256 bytes of plain text";
        if (place.settings.size() > max_settings_bytes ||
            !plain_text(place.settings))
            return "a place's settings are not at most 256 bytes of plain text";
        if (!names.insert(place.name).second)
            return "two places have the same name";
    }
    if (options.seed > recording_max_number)
        return "the seed is above 2^53 - 1";

    const auto day = std::chrono::floor<std::chrono::days>(options.origin_utc);
    const std::chrono::year_month_day date{day};
    if (!date.ok() || static_cast<int>(date.year()) < 1970 ||
        static_cast<int>(date.year()) > 9999)
        return "the origin is not between the years 1970 and 9999";

    if (!plugin::detail::valid_plugin_id(options.made_by.plugin))
        return "made_by.plugin is not a plugin id";
    if (!plugin::detail::version_from_text(options.made_by.version))
        return "made_by.version is not a version";

    for (const recording_rule_set& rules : options.rule_sets)
    {
        if (rules.generation > recording_max_number ||
            !plain_multiline_text(rules.text))
            return "a rule set is not a generation and plain text";
    }
    for (const recording_redaction& redaction : options.redactions)
    {
        if (!names.contains(redaction.place))
            return "a redaction names a place that is not recorded";
        if (redaction.from_ns > redaction.to_ns ||
            redaction.to_ns > recording_max_number)
            return "a redaction's span is not a span of the recording";
        if (redaction.how != only_way_to_redact)
            return "a redaction is not \"blanked\"";
    }

    const recording_limits& limits = options.limits;
    if (limits.limit_seconds > recording_max_number / nanoseconds_per_second)
        return "limit_seconds is too long";
    if (limits.ring_minutes > 0 &&
        (limits.limit_bytes != 0 || limits.limit_seconds != 0))
        return "a ring recording has no limit_bytes or limit_seconds";

    // The index with the largest numbers it can have.
    std::vector<place_totals> largest(places.size());
    for (place_totals& total : largest)
    {
        total.bytes = recording_max_number;
        total.sha256.assign(64, 'f');
    }
    if (render_index(options, recording_max_number, recording_max_number,
                     largest)
            .size() > recording_max_index_bytes)
        return "the index would be larger than 1 MiB";
    return {};
}

} // namespace

// -- the writer ---------------------------------------------------------------

namespace
{

/// One file of a ring: the traffic of one minute of a place.
struct ring_segment
{
    /// The minute, counted from the origin.
    std::uint64_t window = 0;
    fs::path path;
    std::uint64_t bytes = 0;
    std::uint64_t dropped = 0;
};

struct place_state
{
    place_state() = default;
    ~place_state() = default;
    // A place holds an open file and stays where it was made.
    place_state(const place_state&) = delete;
    place_state& operator=(const place_state&) = delete;

    fs::path path;
    std::ofstream out;
    /// The hash of the place file as it is written.  A ring writes the file
    /// only at `finish`, so the hash is made then, as the segments are joined.
    sha256_hasher hasher;
    std::uint64_t last_t_ns = 0;
    std::uint64_t bytes = 0;
    std::uint64_t dropped = 0;
    std::vector<ring_segment> segments;
    std::uint64_t next_segment = 1;
    std::string sha256;
};

} // namespace

struct recording_writer::state
{
    fs::path folder;
    recording_options options;
    std::deque<place_state> places;
    std::uint64_t limit_ns = 0;
    std::uint64_t bytes = 0;
    std::uint64_t dropped = 0;
    std::uint64_t duration_ns = 0;
    bool full = false;
    bool failed = false;
    bool finished = false;
    std::string detail;
    /// The line being written.
    std::string line;

    bool ring() const noexcept { return options.limits.ring_minutes > 0; }

    error_code refuse(recording_errc code, std::string text)
    {
        detail = std::move(text);
        return make_error_code(code);
    }

    /// A failed write: this and every later add is an error.
    error_code fail_io(std::string text)
    {
        failed = true;
        return refuse(recording_errc::io_failed, std::move(text));
    }

    /// What every add checks first.  A success value when it may go on.
    error_code check_ready(std::size_t place, std::uint64_t t_ns)
    {
        if (finished)
            return refuse(recording_errc::finished,
                          "the recording is finished");
        if (failed)
            return refuse(recording_errc::writer_failed,
                          "an earlier write failed");
        if (place >= places.size())
            return refuse(recording_errc::invalid_argument,
                          "there is no such place");
        if (t_ns > recording_max_number)
            return refuse(recording_errc::invalid_argument,
                          "t_ns is above 2^53 - 1");
        // Choice: the order is kept for each place, not across places, as
        // the events of two places arrive on two strands.
        if (t_ns < places[place].last_t_ns)
            return refuse(recording_errc::time_went_back,
                          "t_ns is lower than the last one of the place");
        return {};
    }

    /// Whether a limit stops this add: the outcome when it does.
    std::optional<add_outcome> hold_back(std::uint64_t t_ns)
    {
        if (full)
            return add_outcome::was_full;
        // Choice: a line at exactly the limit's time is written; the first
        // one after it is not, and ends the recording.
        if (limit_ns != 0 && t_ns > limit_ns)
        {
            full = true;
            return add_outcome::limit_reached;
        }
        return std::nullopt;
    }

    /// Drops the segments of a place that end more than `ring_minutes` before
    /// the minute `window`.
    error_code drop_old_segments(place_state& place, std::uint64_t window)
    {
        while (!place.segments.empty() &&
               place.segments.front().window + options.limits.ring_minutes <
                   window)
        {
            const ring_segment& oldest = place.segments.front();
            std::error_code removed;
            fs::remove(oldest.path, removed);
            if (removed)
                return fail_io(
                    words({"could not remove ", oldest.path.filename().string(),
                           ": ", removed.message()}));
            place.bytes -= oldest.bytes;
            place.dropped -= oldest.dropped;
            bytes -= oldest.bytes;
            dropped -= oldest.dropped;
            place.segments.erase(place.segments.begin());
        }
        return {};
    }

    /// A ring: makes sure the segment for the minute of `t_ns` is the one
    /// open for writing.
    error_code start_segment(std::size_t index, std::uint64_t t_ns)
    {
        place_state& place = places[index];
        const std::uint64_t window = t_ns / ring_segment_ns;
        if (!place.segments.empty() && place.segments.back().window == window)
            return {};
        if (place.out.is_open())
        {
            place.out.close();
            if (place.out.fail())
                return fail_io(words(
                    {"could not write ", place.path.filename().string()}));
        }
        std::string part_name = place_file_name(index);
        part_name.push_back('.');
        append_number(part_name, place.next_segment);
        part_name.append(".part");
        ring_segment next;
        next.window = window;
        next.path = folder / part_name;
        ++place.next_segment;
        place.out.open(next.path, std::ios::binary | std::ios::trunc);
        if (!place.out)
            return fail_io(words({"could not create ", part_name}));
        place.segments.push_back(std::move(next));
        return drop_old_segments(place, window);
    }

    /// Writes `line` to a place, which holds `data_bytes` of traffic and
    /// `lost` bytes of loss.
    error_code put(std::size_t index, std::uint64_t t_ns,
                   std::uint64_t data_bytes, std::uint64_t lost)
    {
        place_state& place = places[index];
        if (ring())
        {
            if (const error_code bad = start_segment(index, t_ns); bad)
                return bad;
        }
        place.out.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (!place.out)
            return fail_io(
                words({"could not write ", place.path.filename().string()}));
        if (ring())
        {
            place.segments.back().bytes += data_bytes;
            place.segments.back().dropped += lost;
        }
        else
        {
            place.hasher.update(line.data(), line.size());
        }
        place.last_t_ns = t_ns;
        place.bytes += data_bytes;
        place.dropped += lost;
        bytes += data_bytes;
        dropped += lost;
        duration_ns = std::max(duration_ns, t_ns);
        return {};
    }

    /// A ring: joins the segments of a place into its one file, hashing the
    /// bytes as they are written to it, and removes the segments.
    error_code join_segments(std::size_t index)
    {
        place_state& place = places[index];
        place.out.open(place.path, std::ios::binary | std::ios::trunc);
        if (!place.out)
            return fail_io(
                words({"could not create ", place.path.filename().string()}));
        std::vector<char> block(read_block_bytes);
        for (const ring_segment& segment : place.segments)
        {
            std::ifstream in(segment.path, std::ios::binary);
            if (!in)
                return fail_io(words(
                    {"could not read ", segment.path.filename().string()}));
            while (in)
            {
                in.read(block.data(),
                        static_cast<std::streamsize>(block.size()));
                const std::streamsize got = in.gcount();
                if (got <= 0)
                    break;
                place.out.write(block.data(), got);
                if (!place.out)
                    return fail_io(words(
                        {"could not write ", place.path.filename().string()}));
                place.hasher.update(block.data(),
                                    static_cast<std::size_t>(got));
            }
            if (in.bad())
                return fail_io(words(
                    {"could not read ", segment.path.filename().string()}));
            in.close();
            std::error_code removed;
            fs::remove(segment.path, removed);
            if (removed)
                return fail_io(words({"could not remove ",
                                      segment.path.filename().string(), ": ",
                                      removed.message()}));
        }
        place.segments.clear();
        return {};
    }

    error_code close_places()
    {
        const std::uint64_t last_window = duration_ns / ring_segment_ns;
        for (std::size_t i = 0; i < places.size(); ++i)
        {
            place_state& place = places[i];
            if (place.out.is_open())
            {
                place.out.close();
                if (place.out.fail())
                    return fail_io(words(
                        {"could not write ", place.path.filename().string()}));
            }
            if (!ring())
                continue;
            // The window is the recording's, not each place's, so a place
            // that went quiet does not keep traffic older than the others'.
            if (const error_code bad = drop_old_segments(place, last_window);
                bad)
                return bad;
            if (const error_code bad = join_segments(i); bad)
                return bad;
            place.out.close();
            if (place.out.fail())
                return fail_io(words(
                    {"could not write ", place.path.filename().string()}));
        }
        return {};
    }

    /// Writes the index to a temporary name and renames it.
    error_code write_index(const std::string& text)
    {
        const fs::path temporary = folder / index_temp_name;
        const fs::path index = folder / recording_index_name;
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out)
                return fail_io("could not create recording.json.tmp");
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            out.close();
            if (out.fail())
            {
                std::error_code ignored;
                fs::remove(temporary, ignored);
                return fail_io("could not write recording.json.tmp");
            }
        }
        std::error_code renamed;
        fs::rename(temporary, index, renamed);
        if (renamed)
        {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return fail_io(words(
                {"could not rename recording.json.tmp: ", renamed.message()}));
        }
        return {};
    }
};

recording_writer::recording_writer(std::unique_ptr<state> impl) noexcept
    : state_(std::move(impl))
{
}

recording_writer::~recording_writer() = default;
recording_writer::recording_writer(recording_writer&&) noexcept = default;
recording_writer&
recording_writer::operator=(recording_writer&&) noexcept = default;

result<recording_writer> recording_writer::open(const fs::path& folder,
                                                recording_options options,
                                                std::string* detail)
{
    const auto refuse = [&](recording_errc code,
                            std::string text) -> result<recording_writer>
    {
        if (detail != nullptr)
            *detail = std::move(text);
        return make_error_code(code);
    };

    if (std::string problem = problem_with(options); !problem.empty())
        return refuse(recording_errc::invalid_argument, std::move(problem));

    // Choice: a folder that exists must be an empty folder and not a link, so
    // a recording never mixes with files that were there before it.
    std::error_code failure;
    const fs::file_status status = fs::symlink_status(folder, failure);
    if (status.type() == fs::file_type::not_found)
    {
        failure.clear();
        fs::create_directories(folder, failure);
        if (failure)
            return refuse(
                recording_errc::io_failed,
                words({"could not create the folder: ", failure.message()}));
    }
    else if (failure)
    {
        return refuse(
            recording_errc::io_failed,
            words({"could not look at the folder: ", failure.message()}));
    }
    else if (status.type() != fs::file_type::directory)
    {
        return refuse(recording_errc::folder_not_usable,
                      "the folder is not a folder, or is a link");
    }
    else
    {
        const bool empty = fs::is_empty(folder, failure);
        if (failure)
            return refuse(
                recording_errc::io_failed,
                words({"could not look in the folder: ", failure.message()}));
        if (!empty)
            return refuse(recording_errc::folder_not_usable,
                          "the folder is not empty");
    }

    auto impl = std::make_unique<state>();
    impl->folder = folder;
    impl->limit_ns = options.limits.limit_seconds * nanoseconds_per_second;
    impl->options = std::move(options);
    impl->places.resize(impl->options.places.size());
    for (std::size_t i = 0; i < impl->places.size(); ++i)
    {
        place_state& place = impl->places[i];
        place.path = folder / place_file_name(i);
        // A ring opens a file for each minute as it goes.
        if (impl->ring())
            continue;
        place.out.open(place.path, std::ios::binary | std::ios::trunc);
        if (!place.out)
            return refuse(
                recording_errc::io_failed,
                words({"could not create ", place.path.filename().string()}));
    }
    return recording_writer(std::move(impl));
}

result<add_outcome> recording_writer::add_data(std::size_t place,
                                               std::uint64_t t_ns,
                                               data_direction direction,
                                               bytes_view bytes)
{
    state& s = *state_;
    if (const error_code bad = s.check_ready(place, t_ns); bad)
        return bad;
    if (const auto held = s.hold_back(t_ns); held.has_value())
        return *held;
    if (bytes.empty())
        return add_outcome::written;

    std::size_t usable = bytes.size();
    bool reached = false;
    if (const std::uint64_t cap = s.options.limits.limit_bytes; cap != 0)
    {
        const std::uint64_t room = cap - s.bytes;
        if (usable >= room)
        {
            usable = static_cast<std::size_t>(room);
            reached = true;
        }
    }
    for (std::size_t at = 0; at < usable; at += recording_max_chunk_bytes)
    {
        const std::size_t count =
            std::min(recording_max_chunk_bytes, usable - at);
        format_data(s.line, t_ns, direction, bytes.subspan(at, count));
        if (const error_code bad = s.put(place, t_ns, count, 0); bad)
            return bad;
    }
    if (!reached)
        return add_outcome::written;
    s.full = true;
    return add_outcome::limit_reached;
}

result<add_outcome> recording_writer::add_line(std::size_t place,
                                               std::uint64_t t_ns,
                                               control_lines lines,
                                               control_lines changed)
{
    state& s = *state_;
    if (const error_code bad = s.check_ready(place, t_ns); bad)
        return bad;
    if ((lines.bits & ~all_six_lines) != 0 || changed.bits == 0 ||
        (changed.bits & ~all_six_lines) != 0)
        return s.refuse(recording_errc::invalid_argument,
                        "changed names no line, or a line the format lacks");
    if (const auto held = s.hold_back(t_ns); held.has_value())
        return *held;
    format_line_change(s.line, t_ns, lines, changed);
    if (const error_code bad = s.put(place, t_ns, 0, 0); bad)
        return bad;
    return add_outcome::written;
}

result<add_outcome> recording_writer::add_fault(std::size_t place,
                                                std::uint64_t t_ns,
                                                std::string_view fault)
{
    state& s = *state_;
    if (const error_code bad = s.check_ready(place, t_ns); bad)
        return bad;
    if (fault.empty() || fault.size() > max_fault_bytes || !plain_text(fault))
        return s.refuse(recording_errc::invalid_argument,
                        "a fault is 1 to 64 bytes of plain text");
    if (const auto held = s.hold_back(t_ns); held.has_value())
        return *held;
    format_fault(s.line, t_ns, fault);
    if (const error_code bad = s.put(place, t_ns, 0, 0); bad)
        return bad;
    return add_outcome::written;
}

result<add_outcome> recording_writer::add_dropped(std::size_t place,
                                                  std::uint64_t t_ns,
                                                  std::uint64_t bytes)
{
    state& s = *state_;
    if (const error_code bad = s.check_ready(place, t_ns); bad)
        return bad;
    if (bytes == 0 || bytes > recording_max_number ||
        s.dropped > recording_max_number - bytes)
        return s.refuse(recording_errc::invalid_argument,
                        "dropped is 1 or more bytes, and at most 2^53 - 1 in "
                        "all");
    if (const auto held = s.hold_back(t_ns); held.has_value())
        return *held;
    format_dropped(s.line, t_ns, bytes);
    if (const error_code bad = s.put(place, t_ns, 0, bytes); bad)
        return bad;
    return add_outcome::written;
}

bool recording_writer::full() const noexcept
{
    return state_->full;
}

bool recording_writer::failed() const noexcept
{
    return state_->failed;
}

std::uint64_t recording_writer::bytes() const noexcept
{
    return state_->bytes;
}

std::uint64_t recording_writer::dropped() const noexcept
{
    return state_->dropped;
}

std::uint64_t recording_writer::duration_ns() const noexcept
{
    return state_->duration_ns;
}

std::uint64_t recording_writer::seconds() const noexcept
{
    return state_->duration_ns / nanoseconds_per_second;
}

std::string_view recording_writer::detail() const noexcept
{
    return state_->detail;
}

result<recording_summary> recording_writer::finish()
{
    state& s = *state_;
    if (s.finished)
        return s.refuse(recording_errc::finished, "the recording is finished");
    if (s.failed)
        return s.refuse(recording_errc::writer_failed,
                        "an earlier write failed");

    if (const error_code bad = s.close_places(); bad)
        return bad;

    std::vector<place_totals> totals(s.places.size());
    for (std::size_t i = 0; i < s.places.size(); ++i)
    {
        totals[i].bytes = s.places[i].bytes;
        totals[i].sha256 = sha256_text(s.places[i].hasher);
    }
    const std::string index =
        render_index(s.options, s.duration_ns, s.dropped, totals);
    // The index goes last, so a folder without one is not a recording.
    if (const error_code bad = s.write_index(index); bad)
        return bad;
    s.finished = true;

    recording_summary summary;
    summary.folder = s.folder;
    summary.bytes = s.bytes;
    summary.dropped = s.dropped;
    summary.duration_ns = s.duration_ns;
    summary.limit_reached = s.full;
    std::error_code sizing;
    summary.size_on_disk = index.size();
    for (const place_state& place : s.places)
    {
        const std::uintmax_t size = fs::file_size(place.path, sizing);
        if (sizing)
            return s.fail_io(
                words({"could not measure ", place.path.filename().string()}));
        summary.size_on_disk += size;
    }
    return summary;
}

// -- the reader ---------------------------------------------------------------

namespace
{

/// The number in `value`, when it is a whole number of 0 or more.
std::optional<std::uint64_t> whole_number(const json::value* value) noexcept
{
    if (value == nullptr)
        return std::nullopt;
    if (value->is_uint64())
        return value->get_uint64();
    if (value->is_int64() && value->get_int64() >= 0)
        return static_cast<std::uint64_t>(value->get_int64());
    return std::nullopt;
}

std::optional<std::string_view> text_of(const json::value* value) noexcept
{
    if (value == nullptr || !value->is_string())
        return std::nullopt;
    const json::string& text = value->get_string();
    return std::string_view(text.data(), text.size());
}

bool is_lower_hex(std::string_view text) noexcept
{
    return std::ranges::all_of(text,
                               [](char letter)
                               {
                                   return (letter >= '0' && letter <= '9') ||
                                          (letter >= 'a' && letter <= 'f');
                               });
}

/// What is wrong with a file of a recording that must be a plain file with one
/// name: the error and the end of a sentence about it.  Nothing when it is.
struct file_problem
{
    recording_errc code = recording_errc::layout_invalid;
    std::string text;
};

std::optional<file_problem> check_plain_file(const fs::path& path)
{
    std::error_code failure;
    const fs::file_status status = fs::symlink_status(path, failure);
    if (status.type() == fs::file_type::not_found)
        return file_problem{recording_errc::file_missing, "is missing"};
    if (failure)
        return file_problem{
            recording_errc::io_failed,
            words({"cannot be looked at: ", failure.message()})};
    if (status.type() != fs::file_type::regular)
        return file_problem{recording_errc::layout_invalid,
                            "is not a plain file: a link or a folder is "
                            "refused"};
    // Choice: a file with another name for it (a hard link) is a link too.
    const std::uintmax_t names = fs::hard_link_count(path, failure);
    if (failure)
        return file_problem{
            recording_errc::io_failed,
            words({"cannot be looked at: ", failure.message()})};
    if (names != 1)
        return file_problem{recording_errc::layout_invalid,
                            "has more than one name: a link is refused"};
    return std::nullopt;
}

/// The index, as `read_recording` reads it.
class index_parser
{
public:
    explicit index_parser(std::string& why) : why_(why) {}

    result<recording_index> parse(std::string_view text)
    {
        std::string what;
        const std::optional<json::value> root =
            plugin::detail::parse_strict(text, max_json_depth, what);
        if (!root)
            return fail("recording.json " + what);
        if (!root->is_object())
            return fail("recording.json is not a JSON object");
        const json::object& object = root->get_object();

        // The format number is read before anything else: a newer format may
        // change every other member.
        const auto format =
            whole_number(object.if_contains("recording_format"));
        if (!format)
            return fail("recording_format is not a whole number");
        if (*format > recording_format_number)
        {
            why_ = "recording_format is ";
            append_number(why_, *format);
            why_.append(", and this reader reads up to 1");
            return make_error_code(recording_errc::format_unsupported);
        }
        if (*format != recording_format_number)
            return fail("recording_format is not 1");

        recording_index index;
        index.format = *format;

        if (!read_origin(object, index))
            return fail("origin_utc is not a time like 2026-11-13T09:41:07Z");
        const auto unit = text_of(object.if_contains("time_unit"));
        if (!unit || *unit != "ns")
            return fail("time_unit is not \"ns\"");
        if (!read_number(object, "duration_ns", index.duration_ns) ||
            !read_number(object, "seed", index.seed) ||
            !read_number(object, "dropped", index.dropped))
            return fail("duration_ns, seed and dropped are whole numbers of 0 "
                        "to 2^53 - 1");
        if (!read_places(object, index) || !read_rule_sets(object, index) ||
            !read_redactions(object, index) || !read_made_by(object, index))
            return make_error_code(recording_errc::index_invalid);
        return index;
    }

private:
    error_code fail(std::string text)
    {
        why_ = std::move(text);
        return make_error_code(recording_errc::index_invalid);
    }

    bool bad(std::string text)
    {
        why_ = std::move(text);
        return false;
    }

    static bool read_number(const json::object& object, std::string_view key,
                            std::uint64_t& out)
    {
        const auto number = whole_number(object.if_contains(key));
        if (!number || *number > recording_max_number)
            return false;
        out = *number;
        return true;
    }

    /// `2026-11-13T09:41:07Z`, nothing else.
    static bool read_origin(const json::object& object, recording_index& index)
    {
        const auto text = text_of(object.if_contains("origin_utc"));
        if (!text || text->size() != 20)
            return false;
        const std::string_view t = *text;
        const auto digits = [&](std::size_t from, std::size_t count)
        {
            std::uint32_t value = 0;
            for (std::size_t i = from; i < from + count; ++i)
            {
                if (t[i] < '0' || t[i] > '9')
                    return std::optional<std::uint32_t>{};
                value = value * 10 + static_cast<std::uint32_t>(t[i] - '0');
            }
            return std::optional<std::uint32_t>{value};
        };
        if (t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' ||
            t[16] != ':' || t[19] != 'Z')
            return false;
        const auto year = digits(0, 4);
        const auto month = digits(5, 2);
        const auto day = digits(8, 2);
        const auto hour = digits(11, 2);
        const auto minute = digits(14, 2);
        const auto second = digits(17, 2);
        if (!year || !month || !day || !hour || !minute || !second)
            return false;
        if (*year < 1970 || *hour > 23 || *minute > 59 || *second > 59)
            return false;
        const std::chrono::year_month_day date{
            std::chrono::year{static_cast<int>(*year)},
            std::chrono::month{*month}, std::chrono::day{*day}};
        if (!date.ok())
            return false;
        index.origin_utc =
            std::chrono::sys_days{date} + std::chrono::hours{*hour} +
            std::chrono::minutes{*minute} + std::chrono::seconds{*second};
        return true;
    }

    bool read_places(const json::object& object, recording_index& index)
    {
        const json::value* list = object.if_contains("places");
        if (list == nullptr || !list->is_array())
            return bad("places is not a list");
        const json::array& places = list->get_array();
        if (places.empty() || places.size() > recording_max_places)
            return bad("places holds 1 to 15 places");
        std::set<std::string> names;
        std::set<std::string> files;
        for (const json::value& item : places)
        {
            if (!item.is_object())
                return bad("a place is not an object");
            const json::object& entry = item.get_object();
            recording_place place;
            const auto file = text_of(entry.if_contains("file"));
            if (!file)
                return bad("a place has no file");
            place.file = std::string(*file);
            // Choice: a place file is a plain name in the recording's folder,
            // by the path rules of a manifest, and is neither the index nor
            // another place's file, however the letters are cased.
            if (const auto problem = check_relative_path(place.file))
                return bad(
                    words({"the file ", plugin::detail::quoted(place.file), " ",
                           *problem}));
            if (place.file.find('/') != std::string::npos)
                return bad(
                    words({"the file ", plugin::detail::quoted(place.file),
                           " has a separator"}));
            const std::string folded = lower_ascii(place.file);
            if (folded == recording_index_name || folded == index_temp_name ||
                !files.insert(folded).second)
                return bad(
                    words({"the file ", plugin::detail::quoted(place.file),
                           " is the index or is listed twice"}));
            // Choice: only the two kinds of place the design names.
            const auto kind = text_of(entry.if_contains("kind"));
            if (!kind || (*kind != "port" && *kind != "link"))
                return bad("a place's kind is not \"port\" or \"link\"");
            place.kind = *kind == "port" ? place_kind::port : place_kind::link;
            const auto name = text_of(entry.if_contains("name"));
            const auto settings = text_of(entry.if_contains("settings"));
            if (!name || name->empty() || name->size() > max_name_bytes ||
                !plain_text(*name))
                return bad("a place's name is not 1 to 256 bytes of plain "
                           "text");
            if (!settings || settings->size() > max_settings_bytes ||
                !plain_text(*settings))
                return bad("a place's settings are not at most 256 bytes of "
                           "plain text");
            place.name = std::string(*name);
            place.settings = std::string(*settings);
            if (!names.insert(place.name).second)
                return bad("two places have the same name");
            if (!read_number(entry, "bytes", place.bytes))
                return bad("a place's bytes is not a whole number of 0 to "
                           "2^53 - 1");
            const auto digest = text_of(entry.if_contains("sha256"));
            if (!digest || digest->size() != 64 || !is_lower_hex(*digest))
                return bad("a place's sha256 is not 64 lower-case hex digits");
            place.sha256 = std::string(*digest);
            index.places.push_back(std::move(place));
        }
        return true;
    }

    bool read_rule_sets(const json::object& object, recording_index& index)
    {
        const json::value* list = object.if_contains("rule_sets");
        if (list == nullptr || !list->is_array())
            return bad("rule_sets is not a list");
        // Choice: section 19.6 says a rule set has its text and its
        // generation; they are the members `generation` and `text`.
        for (const json::value& item : list->get_array())
        {
            if (!item.is_object())
                return bad("a rule set is not an object");
            recording_rule_set rules;
            if (!read_number(item.get_object(), "generation", rules.generation))
                return bad("a rule set's generation is not a whole number");
            const auto text = text_of(item.get_object().if_contains("text"));
            if (!text || !plain_multiline_text(*text))
                return bad("a rule set's text is not plain text");
            rules.text = std::string(*text);
            index.rule_sets.push_back(std::move(rules));
        }
        return true;
    }

    bool read_redactions(const json::object& object, recording_index& index)
    {
        const json::value* list = object.if_contains("redactions");
        if (list == nullptr || !list->is_array())
            return bad("redactions is not a list");
        for (const json::value& item : list->get_array())
        {
            if (!item.is_object())
                return bad("a redaction is not an object");
            const json::object& entry = item.get_object();
            recording_redaction redaction;
            const auto place = text_of(entry.if_contains("place"));
            const auto how = text_of(entry.if_contains("how"));
            if (!place || !how ||
                !read_number(entry, "from_ns", redaction.from_ns) ||
                !read_number(entry, "to_ns", redaction.to_ns))
                return bad("a redaction needs place, from_ns, to_ns and how");
            redaction.place = std::string(*place);
            redaction.how = std::string(*how);
            const bool known =
                std::ranges::any_of(index.places, [&](const auto& listed)
                                    { return listed.name == redaction.place; });
            // Choice: a redaction names a recorded place, covers a span that
            // starts no later than it ends and lies inside the recording, and
            // is "blanked", the one way section 19.6 describes.
            if (!known || redaction.from_ns > redaction.to_ns ||
                redaction.to_ns > index.duration_ns ||
                redaction.how != only_way_to_redact)
                return bad("a redaction does not name a recorded place, a "
                           "span inside the recording and \"blanked\"");
            index.redactions.push_back(std::move(redaction));
        }
        return true;
    }

    bool read_made_by(const json::object& object, recording_index& index)
    {
        const json::value* member = object.if_contains("made_by");
        if (member == nullptr || !member->is_object())
            return bad("made_by is not an object");
        const auto plugin_id =
            text_of(member->get_object().if_contains("plugin"));
        const auto version =
            text_of(member->get_object().if_contains("version"));
        if (!plugin_id || !plugin::detail::valid_plugin_id(*plugin_id) ||
            !version || !plugin::detail::version_from_text(*version))
            return bad("made_by needs a plugin id and a version");
        index.made_by.plugin = std::string(*plugin_id);
        index.made_by.version = std::string(*version);
        return true;
    }

    std::string& why_;
};

/// One line of a place file as an event, or the words for what is wrong.
std::optional<recording_event> parse_event(std::string_view line,
                                           std::string& why)
{
    std::string what;
    const std::optional<json::value> root =
        plugin::detail::parse_strict(line, max_json_depth, what);
    if (!root)
    {
        why = what;
        return std::nullopt;
    }
    if (!root->is_object())
    {
        why = "is not a JSON object";
        return std::nullopt;
    }
    const json::object& object = root->get_object();

    recording_event event;
    const auto time = whole_number(object.if_contains("t_ns"));
    if (!time || *time > recording_max_number)
    {
        why = "has no t_ns that is a whole number of 0 to 2^53 - 1";
        return std::nullopt;
    }
    event.t_ns = *time;
    const auto kind = text_of(object.if_contains("kind"));
    if (!kind)
    {
        why = "has no kind that is text";
        return std::nullopt;
    }

    if (*kind == "data")
    {
        event.kind = recording_event_kind::data;
        const auto direction = text_of(object.if_contains("direction"));
        if (!direction || (*direction != "R" && *direction != "W"))
        {
            why = "has a direction that is not \"R\" or \"W\"";
            return std::nullopt;
        }
        event.direction =
            *direction == "R" ? data_direction::rx : data_direction::tx;
        const auto count = whole_number(object.if_contains("bytes"));
        if (!count || *count < 1 || *count > recording_max_chunk_bytes)
        {
            why = "has bytes that is not a whole number of 1 to 4096";
            return std::nullopt;
        }
        const auto hex = text_of(object.if_contains("hex"));
        const auto size = static_cast<std::size_t>(*count);
        if (!hex || hex->size() != 2 * size)
        {
            why = "has a hex that is not 2 x bytes digits";
            return std::nullopt;
        }
        event.bytes.resize(size);
        if (!plugin::detail::hex_to_bytes(*hex, event.bytes))
        {
            why = "has a hex with a character that is not a lower-case hex "
                  "digit";
            return std::nullopt;
        }
        return event;
    }
    if (*kind == "line")
    {
        event.kind = recording_event_kind::line;
        const json::value* states = object.if_contains("lines");
        if (states == nullptr || !states->is_object() ||
            states->get_object().size() != line_names.size())
        {
            why = "has lines that is not an object of exactly the six lines";
            return std::nullopt;
        }
        for (const auto& [line_id, name] : line_names)
        {
            const json::value* state = states->get_object().if_contains(name);
            if (state == nullptr || !state->is_bool())
            {
                why = "has a line state that is missing or is not true or "
                      "false";
                return std::nullopt;
            }
            event.lines.set(line_id, state->get_bool());
        }
        const json::value* names = object.if_contains("changed");
        if (names == nullptr || !names->is_array() ||
            names->get_array().empty() ||
            names->get_array().size() > line_names.size())
        {
            why = "has changed that is not a list of 1 to 6 names";
            return std::nullopt;
        }
        for (const json::value& item : names->get_array())
        {
            const auto name = text_of(&item);
            auto found = line_names.end();
            if (name)
            {
                found = std::ranges::find_if(line_names, [&](const auto& pair)
                                             { return pair.second == *name; });
            }
            if (found == line_names.end() || event.changed.test(found->first))
            {
                why = "has changed with a name that is not a line, or is "
                      "given twice";
                return std::nullopt;
            }
            event.changed.set(found->first, true);
        }
        return event;
    }
    if (*kind == "fault")
    {
        event.kind = recording_event_kind::fault;
        const auto text = text_of(object.if_contains("fault"));
        if (!text || text->empty() || text->size() > max_fault_bytes ||
            !plain_text(*text))
        {
            why = "has a fault that is not 1 to 64 bytes of plain text";
            return std::nullopt;
        }
        event.fault = std::string(*text);
        return event;
    }
    if (*kind == "dropped")
    {
        event.kind = recording_event_kind::dropped;
        const auto count = whole_number(object.if_contains("bytes"));
        if (!count || *count < 1 || *count > recording_max_number)
        {
            why = "has bytes that is not a whole number of 1 or more";
            return std::nullopt;
        }
        event.lost = *count;
        return event;
    }
    why = words({"has the kind ", plugin::detail::quoted(*kind),
                 ", which this reader does not know"});
    return std::nullopt;
}

} // namespace

struct place_reader::state
{
    std::ifstream in;
    std::string file;
    recording_place place;
    std::uint64_t duration_ns = 0;
    std::vector<char> block;
    std::size_t begin = 0;
    std::size_t end = 0;
    bool at_start = true;
    bool at_end = false;
    std::string line;
    sha256_hasher hasher;
    std::uint64_t line_number = 0;
    std::uint64_t last_t_ns = 0;
    std::uint64_t bytes_seen = 0;
    bool done = false;
    error_code failure;
    std::string detail;

    error_code fail(recording_errc code, const std::string& text)
    {
        detail = file;
        detail.append(": ");
        detail.append(text);
        failure = make_error_code(code);
        return failure;
    }

    error_code fail_line(recording_errc code, const std::string& text)
    {
        std::string where = "line ";
        append_number(where, line_number);
        where.push_back(' ');
        where.append(text);
        return fail(code, where);
    }

    /// The next line of the file, without its line feed, or nothing at the
    /// end.  Memory is the block and one line, never the file.
    result<std::optional<std::string_view>> read_line()
    {
        line.clear();
        while (true)
        {
            if (begin == end)
            {
                if (at_end)
                    break;
                in.read(block.data(),
                        static_cast<std::streamsize>(block.size()));
                const auto got = static_cast<std::size_t>(in.gcount());
                if (in.bad())
                    return fail(recording_errc::io_failed, "could not be read");
                if (got < block.size())
                    at_end = true;
                begin = 0;
                end = got;
                hasher.update(block.data(), got);
                // Choice: a byte-order mark is refused, though a JSON reader
                // would also fail on it, so the reason is said.
                if (at_start && got >= 3 &&
                    static_cast<unsigned char>(block[0]) == 0xef &&
                    static_cast<unsigned char>(block[1]) == 0xbb &&
                    static_cast<unsigned char>(block[2]) == 0xbf)
                {
                    line_number = 1;
                    return fail_line(recording_errc::place_invalid,
                                     "starts with a byte-order mark");
                }
                at_start = false;
                if (got == 0)
                    break;
            }
            const char* from = block.data() + begin;
            const char* stop = block.data() + end;
            const char* feed = static_cast<const char*>(
                std::memchr(from, '\n', static_cast<std::size_t>(stop - from)));
            const char* piece_end = feed != nullptr ? feed : stop;
            line.append(from, piece_end);
            begin = static_cast<std::size_t>(piece_end - block.data()) +
                    (feed != nullptr ? 1 : 0);
            if (line.size() > recording_max_line_bytes)
            {
                ++line_number;
                return fail_line(recording_errc::place_invalid,
                                 "is over 16384 bytes");
            }
            if (feed != nullptr)
            {
                ++line_number;
                return std::optional<std::string_view>(line);
            }
        }
        if (!line.empty())
        {
            ++line_number;
            return fail_line(recording_errc::place_invalid,
                             "has no line feed at the end");
        }
        return std::optional<std::string_view>();
    }

    result<std::optional<recording_event>> next()
    {
        if (failure)
            return failure;
        if (done)
            return std::optional<recording_event>();
        const auto read = read_line();
        if (!read)
            return read.error();
        if (!read->has_value())
            return finish_file();
        const std::string_view text = **read;
        if (text.empty())
            return fail_line(recording_errc::place_invalid, "is blank");
        if (text.find('\r') != std::string_view::npos)
            return fail_line(recording_errc::place_invalid,
                             "has a carriage return");
        std::string why;
        std::optional<recording_event> event = parse_event(text, why);
        if (!event)
            return fail_line(recording_errc::place_invalid, why);
        if (event->t_ns < last_t_ns)
            return fail_line(recording_errc::place_invalid,
                             "has a t_ns lower than the line before");
        if (event->t_ns > duration_ns)
            return fail_line(recording_errc::place_invalid,
                             "has a t_ns above the index's duration_ns");
        last_t_ns = event->t_ns;
        if (event->kind == recording_event_kind::data)
            bytes_seen += event->bytes.size();
        return event;
    }

    /// At the end of the file: its hash and its traffic against the index.
    result<std::optional<recording_event>> finish_file()
    {
        if (sha256_text(hasher) != place.sha256)
            return fail(recording_errc::hash_mismatch,
                        "is not the file the index lists: the SHA-256 "
                        "differs");
        if (bytes_seen != place.bytes)
            return fail(recording_errc::size_mismatch,
                        "holds a different number of bytes of traffic than "
                        "the index says");
        done = true;
        return std::optional<recording_event>();
    }
};

place_reader::place_reader(std::unique_ptr<state> impl) noexcept
    : state_(std::move(impl))
{
}

place_reader::~place_reader() = default;
place_reader::place_reader(place_reader&&) noexcept = default;
place_reader& place_reader::operator=(place_reader&&) noexcept = default;

result<std::optional<recording_event>> place_reader::next()
{
    return state_->next();
}

std::string_view place_reader::detail() const noexcept
{
    return state_->detail;
}

result<place_reader> recording::open_place(std::size_t place) const
{
    if (place >= index.places.size())
        return make_error_code(recording_errc::invalid_argument);
    const recording_place& listed = index.places[place];
    const fs::path path = folder / listed.file;
    if (const auto problem = check_plain_file(path))
        return make_error_code(problem->code);
    auto impl = std::make_unique<place_reader::state>();
    impl->in.open(path, std::ios::binary);
    if (!impl->in)
        return make_error_code(recording_errc::io_failed);
    impl->file = listed.file;
    impl->place = listed;
    impl->duration_ns = index.duration_ns;
    impl->block.resize(read_block_bytes);
    return place_reader(std::move(impl));
}

result<recording> read_recording(const fs::path& folder, std::string* detail)
{
    const auto refuse = [&](recording_errc code,
                            std::string text) -> result<recording>
    {
        if (detail != nullptr)
            *detail = std::move(text);
        return make_error_code(code);
    };

    std::error_code failure;
    const fs::file_status status = fs::symlink_status(folder, failure);
    if (status.type() == fs::file_type::not_found)
        return refuse(recording_errc::file_missing, "the folder is missing");
    if (failure)
        return refuse(recording_errc::io_failed,
                      "could not look at the folder: " + failure.message());
    if (status.type() != fs::file_type::directory)
        return refuse(recording_errc::layout_invalid,
                      "the folder is not a folder, or is a link");

    // Everything in the folder, as names: a link, a subfolder and anything
    // else that is not a plain file is refused here, listed or not.
    std::set<std::string> present;
    for (fs::directory_iterator entry(folder, failure), last;
         !failure && entry != last; entry.increment(failure))
    {
        if (present.size() >= recording_max_places + 1)
            return refuse(recording_errc::layout_invalid,
                          "the folder holds more files than a recording has");
        const std::string name = entry->path().filename().string();
        if (const auto problem = check_plain_file(entry->path()))
            return refuse(problem->code == recording_errc::io_failed
                              ? recording_errc::io_failed
                              : recording_errc::layout_invalid,
                          words({name, " ", problem->text}));
        present.insert(name);
    }
    if (failure)
        return refuse(recording_errc::io_failed,
                      "could not list the folder: " + failure.message());

    const fs::path index_path = folder / recording_index_name;
    if (!present.contains(std::string(recording_index_name)))
        return refuse(recording_errc::file_missing,
                      "recording.json is missing: the folder is not a whole "
                      "recording");
    const std::uintmax_t index_size = fs::file_size(index_path, failure);
    if (failure)
        return refuse(recording_errc::io_failed,
                      "could not measure recording.json: " + failure.message());
    if (index_size > recording_max_index_bytes)
        return refuse(recording_errc::index_invalid,
                      "recording.json is larger than 1 MiB");
    std::string text(static_cast<std::size_t>(index_size), '\0');
    {
        std::ifstream in(index_path, std::ios::binary);
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (!in || static_cast<std::uintmax_t>(in.gcount()) != index_size)
            return refuse(recording_errc::io_failed,
                          "could not read recording.json");
    }

    std::string why;
    result<recording_index> parsed = index_parser(why).parse(text);
    if (!parsed)
        return refuse(
            parsed.error().value() ==
                    static_cast<int>(recording_errc::format_unsupported)
                ? recording_errc::format_unsupported
                : recording_errc::index_invalid,
            std::move(why));

    recording found;
    found.folder = folder;
    found.index = *std::move(parsed);

    // The folder holds the index, the listed place files and nothing else.
    std::set<std::string> listed{std::string(recording_index_name)};
    for (const recording_place& place : found.index.places)
    {
        if (!present.contains(place.file))
            return refuse(recording_errc::file_missing,
                          words({place.file, " is missing"}));
        listed.insert(place.file);
    }
    for (const std::string& name : present)
    {
        if (!listed.contains(name))
            return refuse(recording_errc::layout_invalid,
                          words({name, " is in the folder and the index does "
                                       "not list it"}));
    }

    // Every place file, line by line.
    std::uint64_t lost = 0;
    for (std::size_t i = 0; i < found.index.places.size(); ++i)
    {
        result<place_reader> walker = found.open_place(i);
        if (!walker)
            return refuse(
                static_cast<recording_errc>(walker.error().value()),
                words({found.index.places[i].file, " cannot be opened"}));
        while (true)
        {
            const auto next = walker->next();
            if (!next)
                return refuse(static_cast<recording_errc>(next.error().value()),
                              std::string(walker->detail()));
            if (!next->has_value())
                break;
            const recording_event& event = **next;
            if (event.kind != recording_event_kind::dropped)
                continue;
            if (lost > recording_max_number - event.lost)
                return refuse(recording_errc::place_invalid,
                              found.index.places[i].file +
                                  ": the dropped bytes add up to more than "
                                  "2^53 - 1");
            lost += event.lost;
        }
    }
    // Choice: the index's `dropped` is the sum of the `dropped` lines.
    if (lost != found.index.dropped)
        return refuse(recording_errc::size_mismatch,
                      "dropped in the index is not the sum of the dropped "
                      "lines");
    return found;
}

} // namespace vss::plugin::host
