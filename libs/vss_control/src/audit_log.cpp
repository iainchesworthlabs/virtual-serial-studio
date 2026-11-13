//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audit log of the remote-control gateway: JSON lines in a private
// directory, each carrying the SHA-256 of the line before it, rotated by size.
//
//   audit.jsonl      the file that is being written
//   audit.1.jsonl    the one before it; audit.2.jsonl is older, and so on up
//                    to audit.<max_files - 1>.jsonl, which is the oldest
//
// The numbers (`seq`) and the hashes (`prev`) go on across the files.  All
// text that comes from a client is made valid UTF-8 and cut to 1024 bytes
// before it is stored, and the JSON writer escapes it, so it cannot start a
// line of its own.
//

#include <vss/control/audit_log.hpp>

#include "platform/private_storage_platform.hpp"

#include <boost/hash2/sha2.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace vss::control
{
namespace json = boost::json;

namespace
{

constexpr std::size_t max_text_size = 1024;
constexpr std::size_t hash_text_size = 64;
constexpr std::string_view current_name = "audit.jsonl";

// -- text --------------------------------------------------------------------

/// The length of the UTF-8 sequence that starts at `text[index]`, or 0 when
/// the bytes there are not one.
std::size_t utf8_sequence(std::string_view text, std::size_t index) noexcept
{
    const auto at = [&](std::size_t offset)
    { return static_cast<unsigned char>(text[index + offset]); };
    const unsigned char lead = at(0);
    if (lead < 0x80)
        return 1;
    std::size_t length = 0;
    std::uint32_t point = 0;
    std::uint32_t least = 0;
    if ((lead & 0xe0) == 0xc0)
    {
        length = 2;
        point = lead & 0x1fu;
        least = 0x80;
    }
    else if ((lead & 0xf0) == 0xe0)
    {
        length = 3;
        point = lead & 0x0fu;
        least = 0x800;
    }
    else if ((lead & 0xf8) == 0xf0)
    {
        length = 4;
        point = lead & 0x07u;
        least = 0x10000;
    }
    else
    {
        return 0;
    }
    if (text.size() - index < length)
        return 0;
    for (std::size_t offset = 1; offset < length; ++offset)
    {
        const unsigned char next = at(offset);
        if ((next & 0xc0) != 0x80)
            return 0;
        point = (point << 6) | (next & 0x3fu);
    }
    if (point < least || point > 0x10ffff ||
        (point >= 0xd800 && point <= 0xdfff))
        return 0;
    return length;
}

/// Text that is safe to store: valid UTF-8 (a byte that is not becomes `?`) and
/// no longer than `max_text_size`, cut between characters.  Text from a client
/// is not ours to trust.
std::string clean_text(std::string_view text)
{
    std::string out;
    out.reserve(text.size() < max_text_size ? text.size() : max_text_size);
    std::size_t index = 0;
    while (index < text.size())
    {
        const std::size_t length = utf8_sequence(text, index);
        const std::size_t step = length == 0 ? 1 : length;
        if (out.size() + step > max_text_size)
            break;
        if (length == 0)
            out.push_back('?');
        else
            out.append(text.substr(index, length));
        index += step;
    }
    return out;
}

json::value text_value(std::string_view text)
{
    const std::string cleaned = clean_text(text);
    return json::value(json::string(cleaned.data(), cleaned.size()));
}

// -- hashes and time ---------------------------------------------------------

/// The SHA-256 of `bytes` as 64 lower-case hex digits.
std::string hash_hex(std::string_view bytes)
{
    boost::hash2::sha2_256 hash;
    hash.update(bytes.data(), bytes.size());
    const auto digest = hash.result();
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(hash_text_size);
    for (const unsigned char byte : digest)
    {
        out.push_back(digits[static_cast<std::size_t>(byte >> 4)]);
        out.push_back(digits[static_cast<std::size_t>(byte & 0x0fu)]);
    }
    return out;
}

std::string first_hash()
{
    return std::string(hash_text_size, '0');
}

bool is_hex_hash(std::string_view text) noexcept
{
    if (text.size() != hash_text_size)
        return false;
    for (const char letter : text)
    {
        const bool digit = letter >= '0' && letter <= '9';
        const bool lower = letter >= 'a' && letter <= 'f';
        const bool upper = letter >= 'A' && letter <= 'F';
        if (!digit && !lower && !upper)
            return false;
    }
    return true;
}

/// `value` in decimal, padded with zeros to at least `width` digits.
void append_number(std::string& out, long long value, std::size_t width)
{
    const std::string digits = std::to_string(value);
    if (digits.size() < width)
        out.append(width - digits.size(), '0');
    out += digits;
}

/// `2026-11-13T09:30:00.123Z`: UTC, with milliseconds.
std::string utc_text(std::chrono::system_clock::time_point now)
{
    namespace chrono = std::chrono;
    const auto millis = chrono::floor<chrono::milliseconds>(now);
    const auto day = chrono::floor<chrono::days>(millis);
    const chrono::year_month_day date{day};
    const chrono::hh_mm_ss<chrono::milliseconds> time_of_day{millis - day};

    std::string out;
    out.reserve(24);
    append_number(out, static_cast<int>(date.year()), 4);
    out += '-';
    append_number(out, static_cast<unsigned>(date.month()), 2);
    out += '-';
    append_number(out, static_cast<unsigned>(date.day()), 2);
    out += 'T';
    const auto number = [](auto duration)
    { return static_cast<long long>(duration.count()); };
    append_number(out, number(time_of_day.hours()), 2);
    out += ':';
    append_number(out, number(time_of_day.minutes()), 2);
    out += ':';
    append_number(out, number(time_of_day.seconds()), 2);
    out += '.';
    append_number(out, number(time_of_day.subseconds()), 3);
    out += 'Z';
    return out;
}

// -- records -----------------------------------------------------------------

bool is_reserved_key(std::string_view key) noexcept
{
    return key == "seq" || key == "time" || key == "event" || key == "prev";
}

json::value to_json(const audit_field::value_type& value)
{
    struct visitor
    {
        json::value operator()(const std::string& text) const
        {
            return text_value(text);
        }
        json::value operator()(std::uint64_t number) const
        {
            return json::value(number);
        }
        json::value operator()(std::int64_t number) const
        {
            return json::value(number);
        }
        json::value operator()(bool flag) const { return json::value(flag); }
    };
    return std::visit(visitor{}, value);
}

/// The text of a key as a view into the object, if it is a string.
std::optional<std::string_view> view_text(const json::object& object,
                                          std::string_view key) noexcept
{
    const json::value* const item = object.if_contains(key);
    if (item == nullptr || !item->is_string())
        return std::nullopt;
    return std::string_view(item->get_string().data(),
                            item->get_string().size());
}

bool read_u64(const json::object& object, std::string_view key,
              std::uint64_t& out) noexcept
{
    const json::value* const item = object.if_contains(key);
    if (item == nullptr)
        return false;
    if (item->is_uint64())
    {
        out = item->get_uint64();
        return true;
    }
    if (item->is_int64() && item->get_int64() >= 0)
    {
        out = static_cast<std::uint64_t>(item->get_int64());
        return true;
    }
    return false;
}

std::optional<json::object> parse_object(std::string_view line)
{
    boost::system::error_code failed;
    json::value parsed = json::parse(line, failed);
    if (failed || !parsed.is_object())
        return std::nullopt;
    return std::optional<json::object>{std::in_place,
                                       std::move(parsed.as_object())};
}

// -- files -------------------------------------------------------------------

/// `audit.jsonl` for 0, `audit.<index>.jsonl` otherwise.
std::string file_name(std::size_t index)
{
    if (index == 0)
        return std::string(current_name);
    return "audit." + std::to_string(index) + ".jsonl";
}

/// The last line of a file, without its line end, and whether the file ends
/// with one.  Nothing when the file is missing, unreadable or empty.  Only the
/// end of the file is read.
std::optional<std::string> read_last_line(const std::filesystem::path& file,
                                          bool& ends_with_newline)
{
    ends_with_newline = true;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(in.tellg());
    if (size <= 0)
        return std::nullopt;

    constexpr std::streamoff chunk = 64 * 1024;
    std::string tail;
    std::streamoff start = size;
    while (start > 0)
    {
        const std::streamoff step = start < chunk ? start : chunk;
        start -= step;
        std::string block(static_cast<std::size_t>(step), '\0');
        in.clear();
        in.seekg(start);
        in.read(block.data(), step);
        if (in.gcount() != step)
            return std::nullopt;
        tail.insert(0, block);

        // Stop when the last line begins inside what has been read.
        const std::size_t end =
            tail.back() == '\n' ? tail.size() - 1 : tail.size();
        const std::size_t newline = tail.find('\n');
        if (newline != std::string::npos && newline < end)
            break;
    }

    ends_with_newline = tail.back() == '\n';
    if (ends_with_newline)
        tail.pop_back();
    const std::size_t begin = tail.rfind('\n');
    if (begin != std::string::npos)
        tail.erase(0, begin + 1);
    return tail;
}

/// The number N of a file named `audit.N.jsonl` with N >= 1; 0 for any other
/// name.
std::size_t rotated_index(const std::string& name)
{
    constexpr std::string_view prefix = "audit.";
    constexpr std::string_view suffix = ".jsonl";
    if (name.size() <= prefix.size() + suffix.size() ||
        !name.starts_with(prefix) || !name.ends_with(suffix))
        return 0;
    const std::string_view digits(name.data() + prefix.size(),
                                  name.size() - prefix.size() - suffix.size());
    if (digits.size() > 9)
        return 0;
    std::size_t value = 0;
    for (const char letter : digits)
    {
        if (letter < '0' || letter > '9')
            return 0;
        value = value * 10 + static_cast<std::size_t>(letter - '0');
    }
    return value;
}

} // namespace

// -- audit_log ---------------------------------------------------------------

class audit_log::impl
{
public:
    impl(audit_options options, std::filesystem::path current,
         std::unique_ptr<platform::private_file> file, std::uintmax_t size,
         std::uint64_t sequence, std::string last_hash)
        : options_(std::move(options)), current_(std::move(current)),
          file_(std::move(file)), size_(size), sequence_(sequence),
          last_hash_(std::move(last_hash))
    {
    }

    void record(std::string_view event, const std::vector<audit_field>& fields)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!ensure_open())
        {
            note_failure();
            return;
        }

        json::object object;
        object["seq"] = sequence_ + 1;
        object["time"] = utc_text(std::chrono::system_clock::now());
        object["event"] = text_value(event);
        for (const audit_field& field : fields)
        {
            const std::string key = clean_text(field.key);
            if (is_reserved_key(key) || object.contains(key))
                continue;
            object[key] = to_json(field.value);
        }
        object["prev"] = last_hash_;
        std::string line = json::serialize(object);
        const std::string hash = hash_hex(line);
        line.push_back('\n');

        // A file that is full is closed and a new one started.  A record that
        // is longer than the limit still goes to an empty file.
        if (size_ > 0 && size_ + line.size() > options_.max_file_size)
        {
            if (!rotate() || !ensure_open())
            {
                note_failure();
                return;
            }
        }

        if (file_->append(line))
        {
            note_failure();
            return;
        }
        size_ += line.size();
        ++sequence_;
        last_hash_ = hash;
    }

    void note_failure() noexcept
    {
        failed_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t failed_count() const noexcept
    {
        return failed_.load(std::memory_order_relaxed);
    }

    const std::filesystem::path& current_path() const noexcept
    {
        return current_;
    }

private:
    std::filesystem::path numbered(std::size_t index) const
    {
        return options_.directory / file_name(index);
    }

    /// Opens the current file again when an earlier rotation left it closed.
    bool ensure_open()
    {
        if (file_)
            return true;
        auto opened = platform::open_private_append(current_);
        if (!opened)
            return false;
        std::error_code failed;
        const std::uintmax_t size =
            std::filesystem::file_size(current_, failed);
        file_ = std::move(*opened);
        size_ = failed ? 0 : size;
        return true;
    }

    /// Moves every file one place up, dropping the oldest, and leaves the
    /// current file closed and gone.  False when a file could not be moved.
    bool rotate()
    {
        file_.reset();
        size_ = 0;
        std::error_code failed;
        if (options_.max_files <= 1)
        {
            std::filesystem::remove(current_, failed);
            return !failed;
        }

        std::filesystem::remove(numbered(options_.max_files - 1), failed);
        if (failed)
            return false;
        for (std::size_t index = options_.max_files - 1; index >= 1; --index)
        {
            const std::filesystem::path from =
                index == 1 ? current_ : numbered(index - 1);
            const bool present = std::filesystem::exists(from, failed);
            if (failed)
                return false;
            if (present)
            {
                std::filesystem::rename(from, numbered(index), failed);
                if (failed)
                    return false;
            }
            if (index == 1)
                break;
        }
        return true;
    }

    const audit_options options_;
    const std::filesystem::path current_;
    std::mutex mutex_;
    std::unique_ptr<platform::private_file> file_;
    std::uintmax_t size_;
    std::uint64_t sequence_;
    std::string last_hash_;
    std::atomic<std::uint64_t> failed_{0};
};

audit_log::audit_log(std::unique_ptr<impl> state) : impl_(std::move(state)) {}

audit_log::~audit_log() = default;

result<std::unique_ptr<audit_log>> audit_log::open(audit_options options)
{
    if (options.directory.empty())
        return make_error_code(errc::invalid_argument);
    if (options.max_files == 0)
        options.max_files = 1;

    if (const error_code failed =
            platform::ensure_private_directory(options.directory))
        return failed;

    const std::filesystem::path current = options.directory / file_name(0);
    auto file = platform::open_private_append(current);
    if (!file)
        return file.error();

    std::error_code measured;
    std::uintmax_t size = std::filesystem::file_size(current, measured);
    if (measured)
        return make_error_code(errc::io_error);

    // Go on from the last line.  A file that ends in the middle of a line (a
    // crash while writing) gets its line end first, so that the next record
    // does not join it; `verify_audit_log` reports that line.
    bool ends_with_newline = true;
    std::optional<std::string> last =
        read_last_line(current, ends_with_newline);
    if (last && !ends_with_newline)
    {
        if (const error_code failed = (*file)->append("\n"))
            return failed;
        ++size;
    }
    if (!last && size == 0)
    {
        bool ignored = true;
        last = read_last_line(options.directory / file_name(1), ignored);
    }

    std::uint64_t sequence = 0;
    std::string last_hash = first_hash();
    if (last)
    {
        // A line that is not a record still gives the next `prev`, and the
        // numbers go on from 1; the verifier reports the broken line.
        if (const auto object = parse_object(*last))
        {
            std::uint64_t found = 0;
            if (read_u64(*object, "seq", found))
                sequence = found;
        }
        last_hash = hash_hex(*last);
    }

    auto state =
        std::make_unique<impl>(std::move(options), current, std::move(*file),
                               size, sequence, std::move(last_hash));
    return std::unique_ptr<audit_log>(new audit_log(std::move(state)));
}

void audit_log::record(std::string_view event,
                       std::vector<audit_field> fields) noexcept
{
    try
    {
        impl_->record(event, fields);
    }
    catch (...)
    {
        // The audit log must not stop the gateway: the record is lost and
        // counted.
        impl_->note_failure();
    }
}

std::uint64_t audit_log::failed_records() const noexcept
{
    return impl_->failed_count();
}

std::filesystem::path audit_log::current_file() const
{
    return impl_->current_path();
}

// -- verification ------------------------------------------------------------

namespace
{

/// The audit files in `directory`, oldest first.
result<std::vector<std::filesystem::path>>
audit_files(const std::filesystem::path& directory)
{
    std::vector<std::pair<std::size_t, std::filesystem::path>> rotated;
    std::error_code failed;
    for (std::filesystem::directory_iterator it(directory, failed), end;
         !failed && it != end; it.increment(failed))
    {
        const std::size_t index = rotated_index(it->path().filename().string());
        if (index == 0)
            continue;
        std::error_code ignored;
        if (it->is_regular_file(ignored))
            rotated.emplace_back(index, it->path());
    }
    if (failed)
        return error_code(failed.value(), boost::system::system_category());

    // The highest number is the oldest file.
    std::sort(rotated.begin(), rotated.end(),
              [](const auto& left, const auto& right)
              { return left.first > right.first; });
    std::vector<std::filesystem::path> files;
    files.reserve(rotated.size() + 1);
    for (auto& entry : rotated)
        files.push_back(std::move(entry.second));

    const std::filesystem::path current = directory / file_name(0);
    std::error_code ignored;
    if (std::filesystem::is_regular_file(current, ignored))
        files.push_back(current);
    return files;
}

/// What is wrong with a line that is not a record, or empty when it is one.
std::string record_problem(const json::object& object)
{
    std::uint64_t number = 0;
    if (!read_u64(object, "seq", number))
        return "the record has no sequence number";
    if (!view_text(object, "time"))
        return "the record has no time";
    if (!view_text(object, "event"))
        return "the record has no event";
    const auto prev = view_text(object, "prev");
    if (!prev || !is_hex_hash(*prev))
        return "the record has no hash of the line before";
    return {};
}

} // namespace

result<audit_verdict> verify_audit_log(const std::filesystem::path& directory)
{
    audit_verdict verdict;
    std::error_code looked;
    if (!std::filesystem::exists(directory, looked))
    {
        verdict.intact = true;
        return verdict;
    }

    const auto files = audit_files(directory);
    if (!files)
        return files.error();

    bool have_previous = false;
    std::uint64_t previous_seq = 0;
    std::string previous_hash;

    for (const std::filesystem::path& file : *files)
    {
        errno = 0;
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            const int saved = errno;
            if (saved != 0)
                return error_code(saved, boost::system::generic_category());
            return make_error_code(errc::io_error);
        }

        const auto broken = [&](std::uint64_t where, std::string what)
        {
            verdict.intact = false;
            verdict.file = file;
            verdict.line = where;
            verdict.problem = std::move(what);
            return result<audit_verdict>(verdict);
        };

        std::string line;
        std::uint64_t line_number = 0;
        while (std::getline(in, line))
        {
            ++line_number;
            // `getline` reaches the end of the file only on a line that has no
            // line end.
            if (in.eof())
                return broken(line_number,
                              "truncated record: the line has no line end");

            const auto object = parse_object(line);
            if (!object)
                return broken(line_number, "the line is not a JSON object");
            if (std::string problem = record_problem(*object); !problem.empty())
                return broken(line_number, std::move(problem));

            std::uint64_t number = 0;
            static_cast<void>(read_u64(*object, "seq", number));
            const std::string_view prev = *view_text(*object, "prev");
            if (have_previous)
            {
                if (number != previous_seq + 1)
                    return broken(line_number,
                                  "the sequence number does not follow the "
                                  "one before: a record was removed, added "
                                  "or moved");
                if (prev != previous_hash)
                    return broken(line_number,
                                  "the hash of the line before does not "
                                  "match: that line was changed");
            }

            have_previous = true;
            previous_seq = number;
            previous_hash = hash_hex(line);
            ++verdict.records;
        }
    }

    verdict.intact = true;
    return verdict;
}

result<std::filesystem::path> default_audit_directory()
{
    const auto base = platform::state_directory();
    if (!base)
        return base.error();
    return *base / "control";
}

} // namespace vss::control
