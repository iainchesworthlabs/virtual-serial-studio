//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>

#include <vss/studio/capture.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <istream>
#include <ostream>
#include <system_error>

namespace vss {

namespace json = boost::json;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using sys_clock = std::chrono::system_clock;

namespace {

constexpr std::array<const char*, 7> weekday_names{"Sun", "Mon", "Tue", "Wed",
                                                    "Thu", "Fri", "Sat"};
constexpr std::array<const char*, 12> month_names{
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/// Days from 1970-01-01 to a civil date (proleptic Gregorian).
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day)
{
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

struct civil_time
{
    std::int64_t year = 1970;
    unsigned month = 1;
    unsigned day = 1;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    unsigned millisecond = 0;
    unsigned weekday = 4; // 0 is Sunday; 1970-01-01 was a Thursday
};

civil_time civil_from_ms(std::int64_t total_ms)
{
    std::int64_t days = total_ms / 86'400'000;
    std::int64_t rest = total_ms % 86'400'000;
    if (rest < 0)
    {
        rest += 86'400'000;
        --days;
    }

    civil_time out;
    out.weekday = static_cast<unsigned>(((days % 7) + 11) % 7); // day 0 is Thursday
    out.hour = static_cast<unsigned>(rest / 3'600'000);
    out.minute = static_cast<unsigned>(rest / 60'000 % 60);
    out.second = static_cast<unsigned>(rest / 1000 % 60);
    out.millisecond = static_cast<unsigned>(rest % 1000);

    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
         day_of_era / 146096) /
        365;
    const unsigned day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned shifted_month = (5 * day_of_year + 2) / 153;
    out.day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    out.month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    out.year = era * 400 + static_cast<std::int64_t>(year_of_era) +
               (out.month <= 2 ? 1 : 0);
    return out;
}

std::int64_t ms_since_epoch(sys_clock::time_point time)
{
    return duration_cast<milliseconds>(time.time_since_epoch()).count();
}

std::string two(unsigned value)
{
    std::string out;
    if (value < 10)
        out.push_back('0');
    out += std::to_string(value);
    return out;
}

/// `Mon Jun  8 15:19:23 2026`, the layout of `ctime` without its newline, in
/// UTC.  The zone is named after it, so the line is not mistaken for local time.
std::string ctime_text(sys_clock::time_point time)
{
    const civil_time civil = civil_from_ms(ms_since_epoch(time));

    std::string out = weekday_names.at(civil.weekday % 7);
    out += ' ';
    out += month_names.at((civil.month + 11) % 12);
    out += ' ';
    if (civil.day < 10)
        out += ' ';
    out += std::to_string(civil.day);
    out += ' ';
    out += two(civil.hour) + ':' + two(civil.minute) + ':' + two(civil.second);
    out += ' ';
    out += std::to_string(civil.year);
    out += " UTC";
    return out;
}

std::string hex_of(const byte_buffer& data)
{
    return to_hex(bytes_view(data), ' ');
}

/// `0x10|0x02|...`, the byte list of a `.cap` line.
std::string cap_bytes(const byte_buffer& data)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 5);
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        if (i != 0)
            out.push_back('|');
        out += "0x";
        out.push_back(digits[(data[i] >> 4) & 0x0f]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

/// `[       142] `: the offset right aligned in ten columns, as Automate's
/// recorder prints it.
std::string cap_stamp(std::int64_t offset_ms)
{
    std::string digits = std::to_string(offset_ms);
    std::string out = "[";
    if (digits.size() < 10)
        out.append(10 - digits.size(), ' ');
    out += digits;
    out += "] ";
    return out;
}

std::string csv_field(const std::string& text)
{
    if (text.find_first_of(",\"\r\n") == std::string::npos)
        return text;
    std::string out = "\"";
    for (const char c : text)
    {
        if (c == '"')
            out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
    return text;
}

bool iequals(std::string_view a, std::string_view b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](char x, char y) {
                          return std::tolower(static_cast<unsigned char>(x)) ==
                                 std::tolower(static_cast<unsigned char>(y));
                      });
}

bool ends_with_nocase(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           iequals(text.substr(text.size() - suffix.size()), suffix);
}

std::optional<std::int64_t> parse_integer(std::string_view text)
{
    text = trim(text);
    if (text.empty())
        return std::nullopt;
    std::int64_t value = 0;
    const auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

/// Parses `2026-06-08T12:00:00[.fff]Z`.
std::optional<sys_clock::time_point> parse_utc(std::string_view text)
{
    text = trim(text);
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':')
        return std::nullopt;

    const auto year = parse_integer(text.substr(0, 4));
    const auto month = parse_integer(text.substr(5, 2));
    const auto day = parse_integer(text.substr(8, 2));
    const auto hour = parse_integer(text.substr(11, 2));
    const auto minute = parse_integer(text.substr(14, 2));
    const auto second = parse_integer(text.substr(17, 2));
    if (!year || !month || !day || !hour || !minute || !second)
        return std::nullopt;

    std::int64_t millis = 0;
    std::size_t at = 19;
    if (at < text.size() && text[at] == '.')
    {
        ++at;
        int digits = 0;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0)
        {
            if (digits < 3)
            {
                millis = millis * 10 + (text[at] - '0');
                ++digits;
            }
            ++at;
        }
        while (digits < 3)
        {
            millis *= 10;
            ++digits;
        }
    }
    if (at >= text.size() || text[at] != 'Z' || at + 1 != text.size())
        return std::nullopt;

    const std::int64_t days = days_from_civil(
        *year, static_cast<unsigned>(*month), static_cast<unsigned>(*day));
    const std::int64_t total = days * 86'400'000 + *hour * 3'600'000 +
                               *minute * 60'000 + *second * 1000 + millis;
    return sys_clock::time_point(
        duration_cast<sys_clock::duration>(milliseconds(total)));
}

std::optional<data_direction> parse_direction(std::string_view text)
{
    if (iequals(text, "R") || iequals(text, "rx"))
        return data_direction::rx;
    if (iequals(text, "W") || iequals(text, "tx"))
        return data_direction::tx;
    return std::nullopt;
}

} // namespace

const char* to_string(capture_format value) noexcept
{
    switch (value)
    {
    case capture_format::cap:
        return "cap";
    case capture_format::csv:
        return "csv";
    case capture_format::jsonl:
        return "jsonl";
    }
    return "unknown";
}

std::optional<capture_format> parse_capture_format(std::string_view text)
{
    if (iequals(text, "cap"))
        return capture_format::cap;
    if (iequals(text, "csv"))
        return capture_format::csv;
    if (iequals(text, "jsonl") || iequals(text, "ndjson"))
        return capture_format::jsonl;
    return std::nullopt;
}

std::optional<capture_format> capture_format_for_path(std::string_view path)
{
    if (ends_with_nocase(path, ".cap"))
        return capture_format::cap;
    if (ends_with_nocase(path, ".csv"))
        return capture_format::csv;
    if (ends_with_nocase(path, ".jsonl") || ends_with_nocase(path, ".ndjson"))
        return capture_format::jsonl;
    return std::nullopt;
}

char direction_letter(data_direction direction) noexcept
{
    return direction == data_direction::rx ? 'R' : 'W';
}

std::string format_utc(sys_clock::time_point time)
{
    const civil_time civil = civil_from_ms(ms_since_epoch(time));
    std::string out = std::to_string(civil.year);
    while (out.size() < 4)
        out.insert(out.begin(), '0');
    out += '-' + two(civil.month) + '-' + two(civil.day) + 'T' +
           two(civil.hour) + ':' + two(civil.minute) + ':' + two(civil.second) +
           '.';
    const std::string millis = std::to_string(civil.millisecond);
    out.append(3 - millis.size(), '0');
    out += millis;
    out += 'Z';
    return out;
}

// -- writer ------------------------------------------------------------------------

capture_writer::capture_writer(std::ostream& out, capture_format format,
                               const capture_header& header)
    : out_(out)
    , format_(format)
    , header_(header)
{
    switch (format_)
    {
    case capture_format::cap:
        out_ << "# Serial recording started at: "
             << ctime_text(header_.started) << '\n';
        out_ << "# Format: [timestamp_ms] direction byte|byte|byte|...\n";
        out_ << "# Direction: R=read (from device), W=write (to device)\n";
        out_ << "#\n";
        break;
    case capture_format::csv:
        out_ << "time_utc,offset_ms,port,direction,bytes,hex\n";
        break;
    case capture_format::jsonl:
    {
        json::object description;
        description["type"] = "capture";
        description["version"] = 1;
        description["started_utc"] = format_utc(header_.started);
        description["note"] =
            "R is bytes the port received, W is bytes written to the port";
        out_ << json::serialize(description) << '\n';
        break;
    }
    }
    out_.flush();
}

void capture_writer::write(const capture_record& record)
{
    VSS_PROFILE_ZONE("studio.capture.write");
    if (finished_)
        return;

    const char letter = direction_letter(record.direction);
    switch (format_)
    {
    case capture_format::cap:
        out_ << cap_stamp(record.offset_ms) << letter << ' '
             << cap_bytes(record.data) << '\n';
        break;
    case capture_format::csv:
        out_ << format_utc(header_.started +
                           duration_cast<sys_clock::duration>(
                               milliseconds(record.offset_ms)))
             << ',' << record.offset_ms << ',' << csv_field(record.port) << ','
             << letter << ',' << record.data.size() << ','
             << hex_of(record.data) << '\n';
        break;
    case capture_format::jsonl:
    {
        json::object row;
        row["offset_ms"] = record.offset_ms;
        row["time_utc"] =
            format_utc(header_.started + duration_cast<sys_clock::duration>(
                                             milliseconds(record.offset_ms)));
        row["port"] = record.port;
        row["direction"] = std::string(1, letter);
        row["bytes"] = static_cast<std::int64_t>(record.data.size());
        row["hex"] = hex_of(record.data);
        out_ << json::serialize(row) << '\n';
        break;
    }
    }
    // A capture of a bus that is read after a crash is only as good as what
    // reached the disk, so every line is flushed, as Automate's recorder does.
    out_.flush();
    ++records_;
    bytes_ += record.data.size();
}

void capture_writer::finish()
{
    VSS_PROFILE_ZONE("studio.capture.finish");
    if (finished_)
        return;
    finished_ = true;
    if (format_ == capture_format::cap)
        out_ << "# Recording ended\n";
    out_.flush();
}

bool capture_writer::good() const
{
    return out_.good();
}

// -- reader ------------------------------------------------------------------------

namespace {

/// Reads `0x##|0x##|...`.  Tokens that are not exactly that are dropped and
/// make the line count as skipped, the way Automate's replayer treats them.
bool parse_cap_tokens(std::string_view text, byte_buffer& out)
{
    bool clean = true;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const auto pipe = text.find('|', start);
        const auto token = trim(
            text.substr(start, pipe == std::string_view::npos
                                   ? std::string_view::npos
                                   : pipe - start));
        if (!token.empty())
        {
            unsigned value = 0;
            if (token.size() == 4 && token[0] == '0' &&
                (token[1] == 'x' || token[1] == 'X') &&
                std::from_chars(token.data() + 2, token.data() + 4, value, 16)
                        .ec == std::errc{} &&
                std::isxdigit(static_cast<unsigned char>(token[2])) != 0 &&
                std::isxdigit(static_cast<unsigned char>(token[3])) != 0)
                out.push_back(static_cast<byte>(value));
            else
                clean = false;
        }
        if (pipe == std::string_view::npos)
            break;
        start = pipe + 1;
    }
    return clean;
}

void read_cap_line(std::string_view line, capture_file& file,
                   std::int64_t& last_offset)
{
    line = trim(line);
    if (line.empty() || line.front() == '#')
        return;

    capture_record record;
    std::string_view tokens;

    if (line.front() == '[')
    {
        const auto close = line.find(']');
        const auto offset = close == std::string_view::npos
            ? std::nullopt
            : parse_integer(line.substr(1, close - 1));
        if (!offset)
        {
            ++file.skipped_lines;
            return;
        }
        auto rest = trim(line.substr(close + 1));
        if (rest.empty() || rest.front() == '#')
            return; // a note the recorder put on a timestamp
        const auto direction = parse_direction(rest.substr(0, 1));
        if (!direction)
        {
            ++file.skipped_lines;
            return;
        }
        record.offset_ms = *offset;
        record.direction = *direction;
        tokens = trim(rest.substr(1));
    }
    else
    {
        // The legacy form: bytes only, no time, no direction.
        record.offset_ms = last_offset;
        record.direction = data_direction::rx;
        tokens = line;
    }

    if (!parse_cap_tokens(tokens, record.data))
        ++file.skipped_lines;
    if (record.data.empty())
        return;
    last_offset = std::max(last_offset, record.offset_ms);
    file.records.push_back(std::move(record));
}

void read_jsonl_line(std::string_view line, capture_file& file)
{
    line = trim(line);
    if (line.empty())
        return;

    boost::system::error_code ec;
    const json::value parsed = json::parse(line, ec);
    if (ec || !parsed.is_object())
    {
        ++file.skipped_lines;
        return;
    }
    const json::object& object = parsed.as_object();

    if (const auto* started = object.if_contains("started_utc");
        started != nullptr && started->is_string() &&
        object.if_contains("direction") == nullptr)
    {
        file.started = parse_utc(started->as_string().c_str());
        return;
    }

    const auto* offset = object.if_contains("offset_ms");
    const auto* direction = object.if_contains("direction");
    const auto* hex = object.if_contains("hex");
    if (offset == nullptr || direction == nullptr || hex == nullptr ||
        !direction->is_string() || !hex->is_string() || !offset->is_number())
    {
        ++file.skipped_lines;
        return;
    }

    const auto parsed_direction = parse_direction(direction->as_string().c_str());
    const auto bytes = from_hex(hex->as_string().c_str());
    if (!parsed_direction || !bytes)
    {
        ++file.skipped_lines;
        return;
    }

    capture_record record;
    record.offset_ms = offset->is_int64()
        ? offset->as_int64()
        : static_cast<std::int64_t>(std::llround(offset->to_number<double>()));
    record.direction = *parsed_direction;
    if (const auto* port = object.if_contains("port");
        port != nullptr && port->is_string())
        record.port = std::string(port->as_string().c_str());
    record.data = *bytes;
    if (!record.data.empty())
        file.records.push_back(std::move(record));
}

} // namespace

result<capture_file> read_capture(std::istream& in,
                                  std::optional<capture_format> format)
{
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(std::move(line));

    if (!format)
    {
        format = capture_format::cap;
        for (const auto& line : lines)
        {
            const auto text = trim(line);
            if (text.empty())
                continue;
            if (text.front() == '{')
                format = capture_format::jsonl;
            else if (text.rfind("time_utc,", 0) == 0)
                format = capture_format::csv;
            break;
        }
    }
    if (*format == capture_format::csv)
        return make_error_code(errc::unsupported);

    capture_file file;
    file.format = *format;
    std::int64_t last_offset = 0;
    for (const auto& line : lines)
    {
        if (*format == capture_format::cap)
            read_cap_line(line, file, last_offset);
        else
            read_jsonl_line(line, file);
    }

    std::stable_sort(file.records.begin(), file.records.end(),
                     [](const capture_record& a, const capture_record& b) {
                         return a.offset_ms < b.offset_ms;
                     });
    return file;
}

result<capture_file> read_capture_file(const std::string& path)
{
    VSS_PROFILE_ZONE("studio.capture.read_file");
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error || status.type() == std::filesystem::file_type::not_found)
        return make_error_code(errc::not_found);
    if (!std::filesystem::is_regular_file(status))
        return make_error_code(errc::invalid_argument);

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return make_error_code(errc::not_found);
    return read_capture(stream, capture_format_for_path(path));
}

namespace {

std::ofstream open_capture_file(const std::string& path)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (stream)
    {
        // A capture holds whatever crossed the port, so it is the owner's.
        std::error_code ignored;
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace,
                                     ignored);
    }
    return stream;
}

} // namespace

result<void> write_capture_file(const std::string& path, capture_format format,
                                const capture_header& header,
                                const std::vector<capture_record>& records)
{
    std::ofstream stream = open_capture_file(path);
    if (!stream)
        return make_error_code(errc::permission_denied);

    capture_writer writer(stream, format, header);
    for (const auto& record : records)
        writer.write(record);
    writer.finish();
    stream.flush();
    if (!stream)
        return make_error_code(errc::io_error);
    return {};
}

// -- recording ---------------------------------------------------------------------

struct capture_session::state
{
    std::mutex mutex;
    std::shared_ptr<std::ostream> out;
    std::optional<capture_writer> writer;
    std::vector<std::string> ports;
    time_point origin{};
    bool closed = false;

    /// The writer, or null when there is none.  Call with `mutex` held.
    capture_writer* active() noexcept
    {
        return writer.has_value() ? &*writer : nullptr;
    }

    void on_event(const endpoint_event& event)
    {
        VSS_PROFILE_ZONE("studio.capture.record");
        if (event.kind != event_kind::data || !event.data || event.data->empty())
            return;
        if (event.when < origin)
            return; // moved before the capture began

        std::lock_guard lock(mutex);
        if (closed || !writer)
            return;
        if (!ports.empty() &&
            std::find(ports.begin(), ports.end(), event.name) == ports.end())
            return;

        capture_record record;
        record.offset_ms =
            duration_cast<milliseconds>(event.when - origin).count();
        record.port = event.name;
        record.direction = event.direction;
        record.data = *event.data;
        writer->write(record);
    }
};

capture_session::capture_session(hub& engine, std::shared_ptr<std::ostream> out,
                                 const capture_options& options)
    : hub_(engine)
    , state_(std::make_shared<state>())
{
    VSS_PROFILE_ZONE("studio.capture.start");
    VSS_PROFILE_MESSAGE("studio.capture.started");
    header_.started = sys_clock::now();

    state_->out = std::move(out);
    state_->ports = options.ports;
    state_->origin = clock_type::now();
    state_->writer.emplace(*state_->out, options.format, header_);

    event_filter filter;
    filter.kind = event_kind::data;
    // The sink owns the state, so a delivery that is already running when the
    // session is destroyed still has something to write to.
    subscription_ = hub_.subscribe(
        filter, [state = state_](const endpoint_event& event) {
            state->on_event(event);
        });
}

capture_session::~capture_session()
{
    try
    {
        stop();
    }
    catch (...)
    {
        // best effort: a destructor must not throw, and nothing can act on a failed stop
    }
}

result<std::unique_ptr<capture_session>> capture_session::start(
    hub& engine, const std::string& path, const capture_options& options)
{
    auto file = std::make_shared<std::ofstream>(open_capture_file(path));
    if (!*file)
        return make_error_code(errc::permission_denied);
    return std::unique_ptr<capture_session>(
        new capture_session(engine, std::move(file), options));
}

std::unique_ptr<capture_session> capture_session::start(
    hub& engine, std::shared_ptr<std::ostream> out,
    const capture_options& options)
{
    return std::unique_ptr<capture_session>(
        new capture_session(engine, std::move(out), options));
}

void capture_session::stop()
{
    VSS_PROFILE_ZONE("studio.capture.stop");
    VSS_PROFILE_MESSAGE("studio.capture.stopped");
    if (subscription_ != 0)
    {
        hub_.unsubscribe(subscription_);
        subscription_ = 0;
    }

    std::lock_guard lock(state_->mutex);
    if (state_->closed)
        return;
    state_->closed = true;
    if (capture_writer* writer = state_->active(); writer != nullptr)
        writer->finish();
    if (auto* file = dynamic_cast<std::ofstream*>(state_->out.get());
        file != nullptr && file->is_open())
        file->close();
}

std::uint64_t capture_session::records() const
{
    std::lock_guard lock(state_->mutex);
    const capture_writer* writer = state_->active();
    return writer != nullptr ? writer->records() : 0;
}

std::uint64_t capture_session::bytes() const
{
    std::lock_guard lock(state_->mutex);
    const capture_writer* writer = state_->active();
    return writer != nullptr ? writer->bytes() : 0;
}

bool capture_session::good() const
{
    std::lock_guard lock(state_->mutex);
    const capture_writer* writer = state_->active();
    return writer != nullptr && writer->good();
}

} // namespace vss
