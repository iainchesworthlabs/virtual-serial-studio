//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audit log of the remote-control gateway: the shape of a record, the
// chain of hashes, text from a client, rotation by size, and what
// `verify_audit_log` finds when a line is removed, changed or moved.  What the
// operating system does with the permissions of the files is tested in
// tests/unit/platform.
//

#include <vss/control/audit_log.hpp>
#include <vss/core/error.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <boost/hash2/sha2.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace control = vss::control;
namespace json = boost::json;

namespace
{

constexpr std::size_t chain_length = 6;

std::string read_file(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    REQUIRE(static_cast<bool>(in));
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& file, const std::string& bytes)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(static_cast<bool>(out));
}

/// The lines of a text, without their line ends.
std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        const std::size_t end = text.find('\n', begin);
        if (end == std::string::npos)
        {
            lines.push_back(text.substr(begin));
            break;
        }
        lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return lines;
}

std::vector<std::string> read_lines(const std::filesystem::path& file)
{
    return split_lines(read_file(file));
}

/// Every line with a line end, as the log writes them.
void write_lines(const std::filesystem::path& file,
                 const std::vector<std::string>& lines)
{
    std::string text;
    for (const std::string& line : lines)
    {
        text += line;
        text += '\n';
    }
    write_file(file, text);
}

json::object parse_line(const std::string& line)
{
    json::value parsed = json::parse(line);
    REQUIRE(parsed.is_object());
    return std::move(parsed.as_object());
}

std::string text_of(const json::value& item)
{
    REQUIRE(item.is_string());
    return std::string(item.get_string().data(), item.get_string().size());
}

std::uint64_t number_of(const json::value& item)
{
    REQUIRE(item.is_int64() || item.is_uint64());
    return item.is_uint64() ? item.get_uint64()
                            : static_cast<std::uint64_t>(item.get_int64());
}

std::vector<std::string> keys_of(const json::object& object)
{
    std::vector<std::string> keys;
    for (const auto& member : object)
        keys.emplace_back(member.key().data(), member.key().size());
    return keys;
}

/// The SHA-256 of `bytes` as lower-case hex, worked out here and not by the
/// code under test.
std::string sha256_hex(const std::string& bytes)
{
    boost::hash2::sha2_256 hash;
    hash.update(bytes.data(), bytes.size());
    const auto digest = hash.result();
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char byte : digest)
    {
        out.push_back(digits[static_cast<std::size_t>(byte >> 4)]);
        out.push_back(digits[static_cast<std::size_t>(byte & 0x0fu)]);
    }
    return out;
}

std::unique_ptr<control::audit_log>
open_log(const std::filesystem::path& directory,
         std::uintmax_t max_file_size = control::audit_options{}.max_file_size,
         std::size_t max_files = control::audit_options{}.max_files)
{
    control::audit_options options;
    options.directory = directory;
    options.max_file_size = max_file_size;
    options.max_files = max_files;
    auto opened = control::audit_log::open(std::move(options));
    REQUIRE(opened);
    return std::move(*opened);
}

/// Records numbered `first` to `first + count - 1`.
void write_records(control::audit_log& log, std::uint64_t first,
                   std::uint64_t count)
{
    for (std::uint64_t number = first; number < first + count; ++number)
        log.record("test.event", {{"n", number}, {"note", "hello"}});
}

/// A log of `count` records in a new directory, closed again.
void make_chain(const std::filesystem::path& directory, std::uint64_t count)
{
    const auto log = open_log(directory);
    write_records(*log, 1, count);
}

std::vector<std::string> file_names(const std::filesystem::path& directory)
{
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

control::audit_verdict verdict_of(const std::filesystem::path& directory)
{
    const auto verdict = control::verify_audit_log(directory);
    REQUIRE(verdict);
    return *verdict;
}

} // namespace

TEST_CASE("control audit: records are written one to a line, in order")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    {
        const auto log = open_log(directory);
        log->record("session.opened", {{"principal", "local:1000"},
                                       {"count", std::uint64_t{7}},
                                       {"delta", std::int64_t{-3}},
                                       {"allowed", true}});
        log->record("session.closed", {{"principal", "local:1000"}});
        log->record("rate.tripped");
    }

    const auto lines = read_lines(directory / "audit.jsonl");
    REQUIRE_EQ(lines.size(), 3U);

    const json::object first = parse_line(lines[0]);
    const std::vector<std::string> expected_keys{"seq",       "time",  "event",
                                                 "principal", "count", "delta",
                                                 "allowed",   "prev"};
    REQUIRE(keys_of(first) == expected_keys);
    REQUIRE_EQ(number_of(first.at("seq")), 1U);
    REQUIRE_EQ(text_of(first.at("event")), "session.opened");
    REQUIRE_EQ(text_of(first.at("principal")), "local:1000");
    REQUIRE(first.at("count").is_int64());
    REQUIRE_EQ(number_of(first.at("count")), 7U);
    REQUIRE(first.at("delta").is_int64());
    REQUIRE_EQ(first.at("delta").get_int64(), -3);
    REQUIRE(first.at("allowed").is_bool());
    REQUIRE(first.at("allowed").get_bool());
    REQUIRE_EQ(text_of(first.at("prev")), std::string(64, '0'));

    // 2026-11-13T09:30:00.123Z
    const std::string time = text_of(first.at("time"));
    REQUIRE_EQ(time.size(), 24U);
    REQUIRE_EQ(time[4], '-');
    REQUIRE_EQ(time[7], '-');
    REQUIRE_EQ(time[10], 'T');
    REQUIRE_EQ(time[13], ':');
    REQUIRE_EQ(time[16], ':');
    REQUIRE_EQ(time[19], '.');
    REQUIRE_EQ(time[23], 'Z');
    REQUIRE(time >= "2026-01-01T00:00:00.000Z");

    const json::object second = parse_line(lines[1]);
    REQUIRE_EQ(number_of(second.at("seq")), 2U);
    REQUIRE_EQ(text_of(second.at("event")), "session.closed");
    REQUIRE_EQ(text_of(second.at("prev")), sha256_hex(lines[0]));

    const json::object third = parse_line(lines[2]);
    REQUIRE_EQ(number_of(third.at("seq")), 3U);
    REQUIRE_EQ(text_of(third.at("event")), "rate.tripped");
    REQUIRE_EQ(text_of(third.at("prev")), sha256_hex(lines[1]));

    // Each line ends with a single line end.
    const std::string whole = read_file(directory / "audit.jsonl");
    REQUIRE(!whole.empty());
    REQUIRE_EQ(whole.back(), '\n');
    REQUIRE_EQ(std::count(whole.begin(), whole.end(), '\n'), 3);

    const auto verdict = verdict_of(directory);
    REQUIRE(verdict.intact);
    REQUIRE_EQ(verdict.records, 3U);
}

TEST_CASE("control audit: a reopened log continues the numbers and the chain")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    {
        const auto log = open_log(directory);
        write_records(*log, 1, 3);
    }
    {
        const auto log = open_log(directory);
        write_records(*log, 4, 2);
        REQUIRE_EQ(log->failed_records(), 0U);
    }

    const auto lines = read_lines(directory / "audit.jsonl");
    REQUIRE_EQ(lines.size(), 5U);
    for (std::size_t index = 0; index < lines.size(); ++index)
        REQUIRE_EQ(number_of(parse_line(lines[index]).at("seq")), index + 1);
    REQUIRE_EQ(text_of(parse_line(lines[3]).at("prev")), sha256_hex(lines[2]));

    const auto verdict = verdict_of(directory);
    REQUIRE(verdict.intact);
    REQUIRE_EQ(verdict.records, 5U);
}

TEST_CASE("control audit: a line cut short by a crash is kept apart")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_chain(directory, 3);

    const auto file = directory / "audit.jsonl";
    std::string bytes = read_file(file);
    bytes.resize(bytes.size() - 10);
    write_file(file, bytes);

    {
        const auto log = open_log(directory);
        write_records(*log, 4, 1);
    }
    const auto lines = read_lines(file);
    REQUIRE_EQ(lines.size(), 4U);
    // The new record is a line of its own and a complete one.
    REQUIRE_EQ(number_of(parse_line(lines[3]).at("n")), 4U);

    const auto verdict = verdict_of(directory);
    REQUIRE(!verdict.intact);
    REQUIRE_EQ(verdict.file, file);
    REQUIRE_EQ(verdict.line, 3U);
    REQUIRE(!verdict.problem.empty());
}

TEST_CASE("control audit: reserved and repeated keys are dropped")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    {
        const auto log = open_log(directory);
        log->record("test.event", {{"seq", std::uint64_t{99}},
                                   {"time", "yesterday"},
                                   {"event", "other"},
                                   {"prev", "nothing"},
                                   {"a", "first"},
                                   {"a", "second"},
                                   {"b", true}});
    }

    const auto lines = read_lines(directory / "audit.jsonl");
    REQUIRE_EQ(lines.size(), 1U);
    const json::object record = parse_line(lines[0]);
    const std::vector<std::string> expected_keys{"seq", "time", "event",
                                                 "a",   "b",    "prev"};
    REQUIRE(keys_of(record) == expected_keys);
    REQUIRE_EQ(number_of(record.at("seq")), 1U);
    REQUIRE_EQ(text_of(record.at("event")), "test.event");
    REQUIRE_EQ(text_of(record.at("a")), "first");
    REQUIRE(record.at("b").get_bool());
    REQUIRE_EQ(text_of(record.at("prev")), std::string(64, '0'));
    REQUIRE(verdict_of(directory).intact);
}

TEST_CASE("control audit: text from a client stays one line of valid UTF-8")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";

    std::string nasty = "a\nb\"c";
    nasty += '\x01';
    nasty += '\x1f';
    nasty += "\xff\xfe";
    nasty += "\xc3(";
    nasty += "\xe2\x82";
    nasty.append(2000, 'x');

    std::string two_byte;
    for (int index = 0; index < 600; ++index)
        two_byte += "\xc3\xa9";
    std::string three_byte;
    for (int index = 0; index < 500; ++index)
        three_byte += "\xe2\x82\xac";

    const std::string evil_event = "evil\n{\"seq\":99}";
    {
        const auto log = open_log(directory);
        log->record(evil_event, {{"detail", nasty},
                                 {"cafe", "caf\xc3\xa9"},
                                 {"two", two_byte},
                                 {"three", three_byte}});
    }

    // One record, one line: nothing in the text started a line of its own.
    const std::string whole = read_file(directory / "audit.jsonl");
    REQUIRE_EQ(std::count(whole.begin(), whole.end(), '\n'), 1);
    const auto lines = split_lines(whole);
    REQUIRE_EQ(lines.size(), 1U);

    const json::object record = parse_line(lines[0]);
    REQUIRE_EQ(text_of(record.at("event")), evil_event);

    std::string expected = "a\nb\"c";
    expected += '\x01';
    expected += '\x1f';
    expected += "??"; // 0xff 0xfe
    expected += "?("; // 0xc3 and the '(' after it
    expected += "??"; // 0xe2 and 0x82
    const std::string detail = text_of(record.at("detail"));
    REQUIRE_EQ(detail.size(), 1024U);
    REQUIRE(detail.starts_with(expected));
    REQUIRE(detail.find_first_not_of('x', expected.size()) ==
            std::string::npos);

    // Valid characters are kept, and a cut falls between characters.
    REQUIRE_EQ(text_of(record.at("cafe")), "caf\xc3\xa9");
    const std::string two = text_of(record.at("two"));
    REQUIRE_EQ(two.size(), 1024U);
    REQUIRE(two == two_byte.substr(0, 1024));
    const std::string three = text_of(record.at("three"));
    REQUIRE_EQ(three.size(), 1023U);
    REQUIRE(three == three_byte.substr(0, 1023));

    REQUIRE(verdict_of(directory).intact);
}

TEST_CASE("control audit: a full file is closed and the oldest one removed")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    constexpr std::uintmax_t limit = 400;
    constexpr std::uint64_t total = 20;
    {
        const auto log = open_log(directory, limit, 3);
        write_records(*log, 1, total);
        REQUIRE_EQ(log->failed_records(), 0U);
    }

    const std::vector<std::string> expected_names{
        "audit.1.jsonl", "audit.2.jsonl", "audit.jsonl"};
    REQUIRE(file_names(directory) == expected_names);

    // Oldest first: the numbers follow each other across the files, the oldest
    // records are gone, and the newest is in the file being written.
    std::uint64_t records = 0;
    std::uint64_t last = 0;
    for (const char* name : {"audit.2.jsonl", "audit.1.jsonl", "audit.jsonl"})
    {
        const auto file = directory / name;
        REQUIRE(std::filesystem::file_size(file) <= limit);
        const auto lines = read_lines(file);
        REQUIRE(!lines.empty());
        for (const std::string& line : lines)
        {
            const std::uint64_t seq = number_of(parse_line(line).at("seq"));
            if (last != 0)
                REQUIRE_EQ(seq, last + 1);
            else
                REQUIRE(seq > 1);
            last = seq;
            ++records;
        }
    }
    REQUIRE_EQ(last, total);

    const auto verdict = verdict_of(directory);
    REQUIRE(verdict.intact);
    REQUIRE_EQ(verdict.records, records);
}

TEST_CASE("control audit: one file is restarted when it is full")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    {
        const auto log = open_log(directory, 400, 1);
        write_records(*log, 1, 10);
        REQUIRE_EQ(log->failed_records(), 0U);
    }

    const std::vector<std::string> expected_names{"audit.jsonl"};
    REQUIRE(file_names(directory) == expected_names);
    const auto lines = read_lines(directory / "audit.jsonl");
    REQUIRE(!lines.empty());
    REQUIRE_EQ(number_of(parse_line(lines.back()).at("seq")), 10U);
    REQUIRE(number_of(parse_line(lines.front()).at("seq")) > 1);
    REQUIRE(verdict_of(directory).intact);
}

TEST_CASE("control audit: a record over the limit gets a file of its own")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    {
        const auto log = open_log(directory, 100, 3);
        write_records(*log, 1, 3);
        REQUIRE_EQ(log->failed_records(), 0U);
    }

    // Every record is longer than 100 bytes: each one goes to an empty file.
    const std::vector<std::string> expected_names{
        "audit.1.jsonl", "audit.2.jsonl", "audit.jsonl"};
    REQUIRE(file_names(directory) == expected_names);
    for (const auto& name : expected_names)
        REQUIRE_EQ(read_lines(directory / name).size(), 1U);
    const auto verdict = verdict_of(directory);
    REQUIRE(verdict.intact);
    REQUIRE_EQ(verdict.records, 3U);
}

TEST_CASE("control audit: a log that is reopened after a rotation goes on")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    {
        const auto log = open_log(directory, 400, 3);
        write_records(*log, 1, 7);
    }
    {
        const auto log = open_log(directory, 400, 3);
        write_records(*log, 8, 7);
    }
    const auto verdict = verdict_of(directory);
    REQUIRE(verdict.intact);
    REQUIRE(verdict.records > 0);
    const auto lines = read_lines(directory / "audit.jsonl");
    REQUIRE(!lines.empty());
    REQUIRE_EQ(number_of(parse_line(lines.back()).at("seq")), 14U);
}

TEST_CASE("control audit: a removed line is found")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_chain(directory, chain_length);

    const auto file = directory / "audit.jsonl";
    auto lines = read_lines(file);
    REQUIRE_EQ(lines.size(), chain_length);
    lines.erase(lines.begin() + 2); // the third line
    write_lines(file, lines);

    const auto verdict = verdict_of(directory);
    REQUIRE(!verdict.intact);
    REQUIRE_EQ(verdict.file, file);
    REQUIRE(verdict.line >= 3 && verdict.line <= 4);
    REQUIRE_EQ(verdict.records, verdict.line - 1);
    REQUIRE(!verdict.problem.empty());
}

TEST_CASE("control audit: a changed line is found")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_chain(directory, chain_length);

    const auto file = directory / "audit.jsonl";
    auto lines = read_lines(file);
    REQUIRE_EQ(lines.size(), chain_length);
    // One letter of the event name: still a record, no longer the one that
    // the next line has the hash of.
    const std::size_t at = lines[2].find("test.event");
    REQUIRE(at != std::string::npos);
    lines[2][at] = 'T';
    write_lines(file, lines);

    const auto verdict = verdict_of(directory);
    REQUIRE(!verdict.intact);
    REQUIRE_EQ(verdict.file, file);
    REQUIRE(verdict.line >= 3 && verdict.line <= 4);
    REQUIRE(!verdict.problem.empty());
}

TEST_CASE("control audit: two lines that were swapped are found")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_chain(directory, chain_length);

    const auto file = directory / "audit.jsonl";
    auto lines = read_lines(file);
    REQUIRE_EQ(lines.size(), chain_length);
    std::swap(lines[2], lines[3]);
    write_lines(file, lines);

    const auto verdict = verdict_of(directory);
    REQUIRE(!verdict.intact);
    REQUIRE_EQ(verdict.file, file);
    REQUIRE(verdict.line >= 3 && verdict.line <= 4);
    REQUIRE(!verdict.problem.empty());
}

TEST_CASE("control audit: a line that is not a record is found")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_chain(directory, chain_length);

    const auto file = directory / "audit.jsonl";
    auto lines = read_lines(file);
    lines[3] = "this is not json";
    write_lines(file, lines);

    const auto verdict = verdict_of(directory);
    REQUIRE(!verdict.intact);
    REQUIRE_EQ(verdict.file, file);
    REQUIRE_EQ(verdict.line, 4U);
    REQUIRE_EQ(verdict.records, 3U);
    REQUIRE(!verdict.problem.empty());
}

TEST_CASE("control audit: a last line without its line end is found")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_chain(directory, chain_length);

    const auto file = directory / "audit.jsonl";
    std::string bytes = read_file(file);
    REQUIRE_EQ(bytes.back(), '\n');
    bytes.pop_back();
    write_file(file, bytes);

    const auto verdict = verdict_of(directory);
    REQUIRE(!verdict.intact);
    REQUIRE_EQ(verdict.file, file);
    REQUIRE_EQ(verdict.line, chain_length);
    REQUIRE_EQ(verdict.records, chain_length - 1);
    REQUIRE(verdict.problem.find("truncated") != std::string::npos);
}

TEST_CASE("control audit: the first record is taken on trust")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    make_chain(directory, chain_length);

    // What rotation does to the oldest records.
    const auto file = directory / "audit.jsonl";
    auto lines = read_lines(file);
    lines.erase(lines.begin(), lines.begin() + 2);
    write_lines(file, lines);

    const auto verdict = verdict_of(directory);
    REQUIRE(verdict.intact);
    REQUIRE_EQ(verdict.records, chain_length - 2);
}

TEST_CASE("control audit: an empty or missing log is intact")
{
    const vss_test_support::scratch_directory scratch;

    const auto empty = scratch.path() / "empty";
    std::filesystem::create_directories(empty);
    const auto none = verdict_of(empty);
    REQUIRE(none.intact);
    REQUIRE_EQ(none.records, 0U);

    const auto missing = scratch.path() / "missing";
    const auto absent = verdict_of(missing);
    REQUIRE(absent.intact);
    REQUIRE_EQ(absent.records, 0U);
}

TEST_CASE("control audit: records from many threads keep one chain")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    constexpr std::uint64_t threads = 4;
    constexpr std::uint64_t each = 200;
    {
        const auto log = open_log(directory);
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::uint64_t worker = 0; worker < threads; ++worker)
        {
            workers.emplace_back(
                [&log, worker]
                {
                    for (std::uint64_t number = 0; number < each; ++number)
                        log->record("thread.event",
                                    {{"worker", worker}, {"n", number}});
                });
        }
        for (std::thread& worker : workers)
            worker.join();
        REQUIRE_EQ(log->failed_records(), 0U);
    }

    const auto lines = read_lines(directory / "audit.jsonl");
    REQUIRE_EQ(lines.size(), threads * each);
    for (std::size_t index = 0; index < lines.size(); ++index)
        REQUIRE_EQ(number_of(parse_line(lines[index]).at("seq")), index + 1);

    const auto verdict = verdict_of(directory);
    REQUIRE(verdict.intact);
    REQUIRE_EQ(verdict.records, threads * each);
}

TEST_CASE("control audit: the current file and the count of failures")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    const auto log = open_log(directory);
    REQUIRE_EQ(log->current_file(), directory / "audit.jsonl");
    REQUIRE_EQ(log->failed_records(), 0U);
    write_records(*log, 1, 5);
    log->record("no.fields");
    REQUIRE_EQ(log->failed_records(), 0U);
    REQUIRE(std::filesystem::exists(log->current_file()));
}

TEST_CASE("control audit: a log needs a directory")
{
    control::audit_options options;
    const auto opened = control::audit_log::open(std::move(options));
    REQUIRE(!opened);
    REQUIRE_EQ(opened.error(),
               vss::make_error_code(vss::errc::invalid_argument));
}
