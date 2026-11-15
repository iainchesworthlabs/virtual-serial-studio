//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The writer and the reader of a traffic recording (docs/plugins.md, section
// 19.6).  The tests write recordings into a scratch folder and look at the
// bytes on disk, so the format is pinned by text written here and not by the
// code that makes it.  The reader's refusals are tried on files made by hand,
// each with one thing wrong, and on a recording the writer made with one
// thing changed in its index.
//

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/types.hpp>
#include <vss/plugin/host/recording.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <boost/hash2/sha2.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using vss::byte;
using vss::byte_buffer;
using vss::control_line;
using vss::control_lines;
using vss::data_direction;
using vss::plugin::host::add_outcome;
using vss::plugin::host::make_error_code;
using vss::plugin::host::place_kind;
using vss::plugin::host::read_recording;
using vss::plugin::host::recording_errc;
using vss::plugin::host::recording_event;
using vss::plugin::host::recording_event_kind;
using vss::plugin::host::recording_options;
using vss::plugin::host::recording_writer;
using vss_test_support::scratch_directory;

namespace
{

namespace fs = std::filesystem;
namespace json = boost::json;
using namespace std::string_view_literals;

std::string slurp(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void spit(const fs::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string hex_digits_of(std::span<const unsigned char> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char value : bytes)
    {
        out.push_back(digits[static_cast<std::size_t>(value >> 4)]);
        out.push_back(digits[static_cast<std::size_t>(value & 0x0fu)]);
    }
    return out;
}

/// SHA-256 as 64 lower-case hex digits.
std::string sha256_of(std::string_view bytes)
{
    boost::hash2::sha2_256 hash;
    hash.update(bytes.data(), bytes.size());
    const auto digest = hash.result();
    return hex_digits_of(digest);
}

std::string text_of(const json::value& value)
{
    const json::string& text = value.as_string();
    return {text.data(), text.size()};
}

std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    while (from < text.size())
    {
        const std::size_t feed = text.find('\n', from);
        if (feed == std::string::npos)
        {
            out.push_back(text.substr(from));
            break;
        }
        out.push_back(text.substr(from, feed - from));
        from = feed + 1;
    }
    return out;
}

void replace_once(std::string& text, std::string_view from, std::string_view to)
{
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
}

byte_buffer bytes_of(std::initializer_list<int> values)
{
    byte_buffer out;
    for (const int value : values)
        out.push_back(static_cast<byte>(value));
    return out;
}

control_lines lines_set(std::initializer_list<control_line> which)
{
    control_lines out;
    for (const control_line line : which)
        out.set(line, true);
    return out;
}

recording_options options_with(std::vector<std::string> names)
{
    using namespace std::chrono;
    recording_options options;
    options.origin_utc =
        sys_days{year{2026} / 11 / 13} + hours{9} + minutes{41} + seconds{7};
    options.seed = 7;
    for (std::string& name : names)
    {
        vss::plugin::host::place_spec place;
        place.name = std::move(name);
        place.settings = "19200 8N1";
        options.places.push_back(std::move(place));
    }
    options.made_by.plugin = "com.example.recorder";
    options.made_by.version = "1.0.0";
    return options;
}

recording_options one_place()
{
    return options_with({"COM7"});
}

recording_writer opened(const fs::path& folder, recording_options options)
{
    std::string detail;
    auto made = recording_writer::open(folder, std::move(options), &detail);
    if (!made.has_value())
        throw std::runtime_error("open failed: " + detail);
    return std::move(*made);
}

void expect_code(const boost::system::error_code& found, recording_errc wanted)
{
    REQUIRE_EQ(found, make_error_code(wanted));
}

/// The four events of the golden recording, on one place.
void add_golden_events(recording_writer& writer)
{
    REQUIRE(writer.add_data(0, 1000, data_direction::rx, bytes_of({0x48, 0x69}))
                .has_value());
    REQUIRE(writer
                .add_line(0, 2000,
                          lines_set({control_line::dtr, control_line::rts}),
                          lines_set({control_line::dtr, control_line::rts}))
                .has_value());
    REQUIRE(writer.add_fault(0, 3000, "parity").has_value());
    REQUIRE(writer.add_dropped(0, 4000, 16).has_value());
}

constexpr std::string_view golden_place =
    "{\"t_ns\":1000,\"kind\":\"data\",\"direction\":\"R\",\"bytes\":2,"
    "\"hex\":\"4869\"}\n"
    "{\"t_ns\":2000,\"kind\":\"line\",\"lines\":{\"dtr\":true,\"rts\":true,"
    "\"cts\":false,\"dsr\":false,\"dcd\":false,\"ri\":false},"
    "\"changed\":[\"dtr\",\"rts\"]}\n"
    "{\"t_ns\":3000,\"kind\":\"fault\",\"fault\":\"parity\"}\n"
    "{\"t_ns\":4000,\"kind\":\"dropped\",\"bytes\":16}\n"sv;

// Worked out apart from this code, with Python's hashlib.
constexpr std::string_view golden_sha256 =
    "b9ae9c32e8da7c93b88c7a54a79bf01e050ae4898844b678685bbe386fa52987"sv;

std::string golden_index()
{
    std::string text =
        "{\"recording_format\":1,\"origin_utc\":\"2026-11-13T09:41:07Z\","
        "\"time_unit\":\"ns\",\"duration_ns\":4000,\"seed\":7,"
        "\"places\":[{\"file\":\"1.jsonl\",\"kind\":\"port\","
        "\"name\":\"COM7\",\"settings\":\"19200 8N1\",\"bytes\":2,"
        "\"sha256\":\"";
    text.append(golden_sha256);
    text.append("\"}],\"rule_sets\":[],\"redactions\":[],\"dropped\":16,"
                "\"made_by\":{\"plugin\":\"com.example.recorder\","
                "\"version\":\"1.0.0\"}}\n");
    return text;
}

/// A whole recording of the golden events in `folder`.
void write_golden(const fs::path& folder)
{
    recording_writer writer = opened(folder, one_place());
    add_golden_events(writer);
    REQUIRE(writer.finish().has_value());
}

/// What `read_recording` says of `folder`: its error, or success.
boost::system::error_code reading(const fs::path& folder,
                                  std::string* detail = nullptr)
{
    const auto found = read_recording(folder, detail);
    return found.has_value() ? boost::system::error_code() : found.error();
}

} // namespace

// -- what the writer writes ---------------------------------------------------

TEST_CASE("plugin recording: each kind of line has exactly these bytes")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_writer writer = opened(folder, one_place());

    REQUIRE(
        writer.add_data(0, 0, data_direction::rx, bytes_of({0x00, 0xab, 0xff}))
            .has_value());
    REQUIRE(writer.add_data(0, 1500, data_direction::tx, bytes_of({0x41}))
                .has_value());
    REQUIRE(writer
                .add_line(0, 2000,
                          lines_set({control_line::dtr, control_line::ri}),
                          lines_set({control_line::ri}))
                .has_value());
    REQUIRE(writer
                .add_line(0, 2000, control_lines{},
                          lines_set({control_line::dtr, control_line::rts,
                                     control_line::cts, control_line::dsr,
                                     control_line::dcd, control_line::ri}))
                .has_value());
    REQUIRE(writer.add_fault(0, 3000, "overrun").has_value());
    REQUIRE(writer.add_fault(0, 3000, "say \"hi\" \\ done").has_value());
    REQUIRE(writer.add_dropped(0, 9'007'199'254'740'991, 1).has_value());
    REQUIRE(writer.finish().has_value());

    const std::string expected =
        "{\"t_ns\":0,\"kind\":\"data\",\"direction\":\"R\",\"bytes\":3,"
        "\"hex\":\"00abff\"}\n"
        "{\"t_ns\":1500,\"kind\":\"data\",\"direction\":\"W\",\"bytes\":1,"
        "\"hex\":\"41\"}\n"
        "{\"t_ns\":2000,\"kind\":\"line\",\"lines\":{\"dtr\":true,"
        "\"rts\":false,\"cts\":false,\"dsr\":false,\"dcd\":false,"
        "\"ri\":true},\"changed\":[\"ri\"]}\n"
        "{\"t_ns\":2000,\"kind\":\"line\",\"lines\":{\"dtr\":false,"
        "\"rts\":false,\"cts\":false,\"dsr\":false,\"dcd\":false,"
        "\"ri\":false},\"changed\":[\"dtr\",\"rts\",\"cts\",\"dsr\",\"dcd\","
        "\"ri\"]}\n"
        "{\"t_ns\":3000,\"kind\":\"fault\",\"fault\":\"overrun\"}\n"
        "{\"t_ns\":3000,\"kind\":\"fault\",\"fault\":\"say \\\"hi\\\" \\\\ "
        "done\"}\n"
        "{\"t_ns\":9007199254740991,\"kind\":\"dropped\",\"bytes\":1}\n";
    REQUIRE_EQ(slurp(folder / "1.jsonl"), expected);
}

TEST_CASE("plugin recording: a small whole recording is these two files")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    write_golden(folder);

    REQUIRE_EQ(slurp(folder / "1.jsonl"), std::string(golden_place));
    REQUIRE_EQ(slurp(folder / "recording.json"), golden_index());
    // The independent hash is the one in the text above.
    REQUIRE_EQ(sha256_of(golden_place), std::string(golden_sha256));
    REQUIRE_EQ(sha256_of("abc"),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb41"
                           "0ff61f20015ad"));

    // Nothing else is left in the folder.
    const auto files =
        std::distance(fs::directory_iterator(folder), fs::directory_iterator());
    REQUIRE_EQ(files, 2);
}

TEST_CASE("plugin recording: a chunk of 10000 bytes is three lines")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_writer writer = opened(folder, one_place());

    byte_buffer chunk(10'000);
    for (std::size_t i = 0; i < chunk.size(); ++i)
        chunk[i] = static_cast<byte>(i % 251);
    const auto outcome = writer.add_data(0, 777, data_direction::rx, chunk);
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == add_outcome::written);
    REQUIRE_EQ(writer.bytes(), std::uint64_t{10'000});
    REQUIRE(writer.finish().has_value());

    const auto lines = lines_of(slurp(folder / "1.jsonl"));
    REQUIRE_EQ(lines.size(), std::size_t{3});
    const std::int64_t sizes[] = {4096, 4096, 1808};
    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const json::value line = json::parse(lines[i]);
        REQUIRE_EQ(line.at("t_ns").as_int64(), 777);
        REQUIRE_EQ(line.at("bytes").as_int64(), sizes[i]);
        joined.append(text_of(line.at("hex")));
    }
    REQUIRE_EQ(joined, hex_digits_of(chunk));

    // And the reader accepts it.
    const auto found = read_recording(folder);
    REQUIRE(found.has_value());
    REQUIRE_EQ(found->index.places[0].bytes, std::uint64_t{10'000});
}

TEST_CASE("plugin recording: a time that goes back is refused")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_writer writer = opened(folder, options_with({"COM7", "COM8"}));

    REQUIRE(writer.add_data(0, 5000, data_direction::rx, bytes_of({1}))
                .has_value());
    const auto back =
        writer.add_data(0, 4999, data_direction::rx, bytes_of({2}));
    REQUIRE(!back.has_value());
    expect_code(back.error(), recording_errc::time_went_back);
    REQUIRE(
        !writer
             .add_line(0, 4999, control_lines{}, lines_set({control_line::dtr}))
             .has_value());
    REQUIRE(!writer.add_fault(0, 1, "x").has_value());
    REQUIRE(!writer.add_dropped(0, 0, 1).has_value());
    // The same time is allowed, and another place has its own order.
    REQUIRE(writer.add_data(0, 5000, data_direction::tx, bytes_of({3}))
                .has_value());
    REQUIRE(
        writer.add_data(1, 10, data_direction::rx, bytes_of({4})).has_value());
    // A refusal does not stop the writer.
    REQUIRE(!writer.failed());
    REQUIRE_EQ(writer.bytes(), std::uint64_t{3});
    REQUIRE(writer.finish().has_value());

    REQUIRE_EQ(lines_of(slurp(folder / "1.jsonl")).size(), std::size_t{2});
    REQUIRE(read_recording(folder).has_value());
}

TEST_CASE("plugin recording: the writer refuses what the format does not hold")
{
    const scratch_directory scratch;
    recording_writer writer = opened(scratch.path() / "rec", one_place());

    const auto refused = [&](const auto& answer)
    {
        REQUIRE(!answer.has_value());
        expect_code(answer.error(), recording_errc::invalid_argument);
    };
    refused(writer.add_data(1, 0, data_direction::rx, bytes_of({1})));
    refused(writer.add_data(0, 9'007'199'254'740'992, data_direction::rx,
                            bytes_of({1})));
    refused(writer.add_line(0, 0, control_lines{}, control_lines{}));
    refused(writer.add_line(0, 0, control_lines{},
                            lines_set({control_line::break_signal})));
    refused(writer.add_line(0, 0, lines_set({control_line::break_signal}),
                            lines_set({control_line::dtr})));
    refused(writer.add_fault(0, 0, ""));
    refused(writer.add_fault(0, 0, std::string(65, 'x')));
    refused(writer.add_fault(0, 0, "tab\there"));
    refused(writer.add_fault(0, 0, "\xff\xfe"));
    refused(writer.add_dropped(0, 0, 0));
    REQUIRE(writer.add_fault(0, 0, std::string(64, 'x')).has_value());
    REQUIRE_EQ(writer.bytes(), std::uint64_t{0});
}

TEST_CASE("plugin recording: options the format does not hold are refused")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    const auto refused = [&](recording_options options)
    {
        std::string detail;
        const auto made =
            recording_writer::open(folder, std::move(options), &detail);
        REQUIRE(!made.has_value());
        expect_code(made.error(), recording_errc::invalid_argument);
        REQUIRE(!detail.empty());
        // Nothing was made.
        REQUIRE(!fs::exists(folder));
    };

    refused(options_with({}));
    refused(options_with({"a", "a"}));
    refused(options_with({""}));
    refused(options_with(std::vector<std::string>(16, "x")));
    {
        recording_options options = options_with({"a"});
        options.seed = 9'007'199'254'740'992;
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.made_by.plugin = "Not An Id";
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.made_by.version = "one";
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.places[0].settings = "bell\a";
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.redactions.push_back({"b", 0, 1, "blanked"});
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.redactions.push_back({"a", 2, 1, "blanked"});
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.redactions.push_back({"a", 0, 1, "erased"});
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.rule_sets.push_back({1, "bell\a"});
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.rule_sets.push_back({1, std::string(2'000'000, 'x')});
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.limits.ring_minutes = 1;
        options.limits.limit_bytes = 5;
        refused(options);
    }
    {
        recording_options options = options_with({"a"});
        options.limits.limit_seconds = 10'000'000;
        refused(options);
    }
}

// -- limits -------------------------------------------------------------------

TEST_CASE("plugin recording: the byte limit is reached exactly")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_options options = options_with({"COM7", "COM8"});
    options.limits.limit_bytes = 10;
    recording_writer writer = opened(folder, options);

    auto outcome =
        writer.add_data(0, 1, data_direction::rx, bytes_of({1, 2, 3, 4}));
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == add_outcome::written);
    REQUIRE(!writer.full());
    // The limit counts the bytes of all places together.
    outcome = writer.add_data(1, 2, data_direction::tx,
                              bytes_of({5, 6, 7, 8, 9, 10}));
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == add_outcome::limit_reached);
    REQUIRE(writer.full());
    REQUIRE_EQ(writer.bytes(), std::uint64_t{10});

    // Nothing more is written, of any kind.
    outcome = writer.add_data(0, 3, data_direction::rx, bytes_of({11}));
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == add_outcome::was_full);
    outcome = writer.add_fault(0, 3, "late");
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == add_outcome::was_full);
    REQUIRE_EQ(writer.bytes(), std::uint64_t{10});

    const auto summary = writer.finish();
    REQUIRE(summary.has_value());
    REQUIRE(summary->limit_reached);
    REQUIRE_EQ(summary->bytes, std::uint64_t{10});
    REQUIRE_EQ(lines_of(slurp(folder / "1.jsonl")).size(), std::size_t{1});
    REQUIRE_EQ(lines_of(slurp(folder / "2.jsonl")).size(), std::size_t{1});
    REQUIRE(read_recording(folder).has_value());
}

TEST_CASE("plugin recording: one byte past the byte limit is not written")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_options options = one_place();
    options.limits.limit_bytes = 10;
    recording_writer writer = opened(folder, options);

    REQUIRE(writer
                .add_data(0, 1, data_direction::rx,
                          bytes_of({1, 2, 3, 4, 5, 6, 7, 8, 9}))
                .has_value());
    REQUIRE(!writer.full());
    // Two bytes where one fits: the one is written, and the writer is full.
    const auto outcome =
        writer.add_data(0, 2, data_direction::rx, bytes_of({10, 11}));
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == add_outcome::limit_reached);
    REQUIRE(writer.full());
    REQUIRE_EQ(writer.bytes(), std::uint64_t{10});
    REQUIRE(writer.finish().has_value());

    const auto lines = lines_of(slurp(folder / "1.jsonl"));
    REQUIRE_EQ(lines.size(), std::size_t{2});
    REQUIRE_EQ(text_of(json::parse(lines[1]).at("hex")), "0a");
}

TEST_CASE("plugin recording: the time limit is reached exactly")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_options options = one_place();
    options.limits.limit_seconds = 2;
    recording_writer writer = opened(folder, options);

    // A line at exactly the limit's time is written.
    auto outcome =
        writer.add_data(0, 2'000'000'000, data_direction::rx, bytes_of({1}));
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == add_outcome::written);
    REQUIRE(!writer.full());
    // One nanosecond past it is not, and ends the recording.
    outcome =
        writer.add_data(0, 2'000'000'001, data_direction::rx, bytes_of({2}));
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == add_outcome::limit_reached);
    REQUIRE(writer.full());
    outcome = writer.add_fault(0, 2'000'000'002, "late");
    REQUIRE(outcome.has_value());
    REQUIRE(*outcome == add_outcome::was_full);
    REQUIRE_EQ(writer.seconds(), std::uint64_t{2});
    REQUIRE_EQ(writer.duration_ns(), std::uint64_t{2'000'000'000});

    const auto summary = writer.finish();
    REQUIRE(summary.has_value());
    REQUIRE(summary->limit_reached);
    REQUIRE_EQ(lines_of(slurp(folder / "1.jsonl")).size(), std::size_t{1});
}

// -- finish and the index -----------------------------------------------------

TEST_CASE("plugin recording: the index is written last and matches the files")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_writer writer = opened(folder, options_with({"COM7", "link-1"}));
    REQUIRE(writer.add_data(0, 10, data_direction::rx, bytes_of({1, 2, 3}))
                .has_value());
    REQUIRE(writer.add_data(1, 20, data_direction::tx, bytes_of({4, 5}))
                .has_value());
    REQUIRE(
        writer.add_data(1, 30, data_direction::rx, bytes_of({6})).has_value());
    REQUIRE(writer.add_fault(1, 40, "framing").has_value());
    REQUIRE(writer.add_dropped(0, 50, 7).has_value());

    // Until `finish` there is no index and no name it could be mistaken for.
    REQUIRE(!fs::exists(folder / "recording.json"));
    REQUIRE(!fs::exists(folder / "recording.json.tmp"));
    expect_code(reading(folder), recording_errc::file_missing);

    const auto summary = writer.finish();
    REQUIRE(summary.has_value());
    REQUIRE_EQ(summary->folder, folder);
    REQUIRE_EQ(summary->bytes, std::uint64_t{6});
    REQUIRE_EQ(summary->dropped, std::uint64_t{7});
    REQUIRE_EQ(summary->duration_ns, std::uint64_t{50});
    REQUIRE(!summary->limit_reached);
    REQUIRE(!fs::exists(folder / "recording.json.tmp"));

    // The size on disk is every file of the folder.
    std::uint64_t on_disk = 0;
    for (const auto& entry : fs::directory_iterator(folder))
        on_disk += fs::file_size(entry.path());
    REQUIRE_EQ(summary->size_on_disk, on_disk);

    // The index, read with a JSON reader of the test's own.
    const json::value index = json::parse(slurp(folder / "recording.json"));
    const json::array& places = index.at("places").as_array();
    REQUIRE_EQ(places.size(), std::size_t{2});
    std::int64_t all_bytes = 0;
    for (std::size_t i = 0; i < places.size(); ++i)
    {
        const std::string file_name = std::to_string(i + 1) + ".jsonl";
        REQUIRE_EQ(text_of(places[i].at("file")), file_name);
        const std::string text = slurp(folder / file_name);
        REQUIRE_EQ(text_of(places[i].at("sha256")), sha256_of(text));
        std::int64_t counted = 0;
        for (const std::string& line : lines_of(text))
        {
            const json::value parsed = json::parse(line);
            if (text_of(parsed.at("kind")) == "data")
                counted += parsed.at("bytes").as_int64();
        }
        REQUIRE_EQ(places[i].at("bytes").as_int64(), counted);
        all_bytes += counted;
    }
    REQUIRE_EQ(all_bytes, 6);
    REQUIRE_EQ(index.at("dropped").as_int64(), 7);
    REQUIRE_EQ(index.at("duration_ns").as_int64(), 50);
    REQUIRE(read_recording(folder).has_value());

    // A writer that is finished takes nothing more.
    const auto late = writer.add_fault(0, 60, "late");
    REQUIRE(!late.has_value());
    expect_code(late.error(), recording_errc::finished);
    expect_code(writer.finish().error(), recording_errc::finished);
}

TEST_CASE("plugin recording: a writer dropped without finish leaves no index")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    {
        recording_writer writer = opened(folder, one_place());
        REQUIRE(writer.add_data(0, 10, data_direction::rx, bytes_of({1}))
                    .has_value());
    }
    REQUIRE(fs::exists(folder / "1.jsonl"));
    REQUIRE(!fs::exists(folder / "recording.json"));
    REQUIRE(!fs::exists(folder / "recording.json.tmp"));
    std::string detail;
    expect_code(reading(folder, &detail), recording_errc::file_missing);
    REQUIRE(!detail.empty());
}

TEST_CASE("plugin recording: a folder that is not empty is not used")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    fs::create_directories(folder);
    spit(folder / "other.txt", "x");

    std::string detail;
    const auto made = recording_writer::open(folder, one_place(), &detail);
    REQUIRE(!made.has_value());
    expect_code(made.error(), recording_errc::folder_not_usable);
    REQUIRE(!detail.empty());
    // What was there is untouched.
    REQUIRE_EQ(slurp(folder / "other.txt"), "x");
    REQUIRE(!fs::exists(folder / "1.jsonl"));

    // A file where the folder should be.
    const auto over_file =
        recording_writer::open(folder / "other.txt", one_place());
    REQUIRE(!over_file.has_value());
    expect_code(over_file.error(), recording_errc::folder_not_usable);

    // An empty folder, and one that does not exist yet, are used.
    fs::remove(folder / "other.txt");
    REQUIRE(recording_writer::open(folder, one_place()).has_value());
    REQUIRE(recording_writer::open(scratch.path() / "a" / "b", one_place())
                .has_value());
}

TEST_CASE("plugin recording: a failed write is an error and stops the writer")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_writer writer = opened(folder, one_place());
    REQUIRE(
        writer.add_data(0, 10, data_direction::rx, bytes_of({1})).has_value());

    // The index cannot be written under its temporary name when a folder has
    // that name.
    fs::create_directory(folder / "recording.json.tmp");
    const auto summary = writer.finish();
    REQUIRE(!summary.has_value());
    expect_code(summary.error(), recording_errc::io_failed);
    REQUIRE(writer.failed());
    REQUIRE(!writer.detail().empty());
    REQUIRE(!fs::exists(folder / "recording.json"));

    // Every later call is refused.
    const auto more = writer.add_data(0, 20, data_direction::rx, bytes_of({2}));
    REQUIRE(!more.has_value());
    expect_code(more.error(), recording_errc::writer_failed);
    expect_code(writer.finish().error(), recording_errc::writer_failed);
}

// -- the reader: a round trip -----------------------------------------------

TEST_CASE("plugin recording: all four kinds on two places read back as written")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_options options = options_with({"COM7", "tunnel \xC3\xA9 \"2\""});
    options.places[1].kind = place_kind::link;
    options.places[1].settings.clear();
    options.seed = 9'007'199'254'740'991;
    options.rule_sets.push_back(
        {3, "when rx {\n\tsend \"ok\"\r\n}\n \xE2\x80\x94 end"});
    options.rule_sets.push_back({4, ""});
    options.redactions.push_back({"COM7", 100, 200, "blanked"});
    recording_writer writer = opened(folder, options);

    byte_buffer big(5000);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<byte>(i);
    REQUIRE(writer.add_data(0, 100, data_direction::rx, bytes_of({0xde, 0xad}))
                .has_value());
    REQUIRE(writer
                .add_line(1, 150, lines_set({control_line::cts}),
                          lines_set({control_line::cts}))
                .has_value());
    REQUIRE(writer.add_data(0, 200, data_direction::tx, big).has_value());
    REQUIRE(writer.add_fault(1, 250, "break").has_value());
    REQUIRE(writer.add_dropped(1, 300, 12).has_value());
    REQUIRE(writer.add_dropped(0, 300, 30).has_value());
    REQUIRE(writer
                .add_line(0, 400,
                          lines_set({control_line::dcd, control_line::dsr}),
                          lines_set({control_line::dcd, control_line::dsr}))
                .has_value());
    REQUIRE_EQ(writer.bytes(), std::uint64_t{5002});
    REQUIRE_EQ(writer.dropped(), std::uint64_t{42});
    REQUIRE(writer.finish().has_value());

    std::string detail;
    const auto found = read_recording(folder, &detail);
    REQUIRE(found.has_value());
    const auto& index = found->index;
    REQUIRE_EQ(index.format, std::uint64_t{1});
    REQUIRE(index.origin_utc == options.origin_utc);
    REQUIRE_EQ(index.duration_ns, std::uint64_t{400});
    REQUIRE_EQ(index.seed, options.seed);
    REQUIRE_EQ(index.dropped, std::uint64_t{42});
    REQUIRE_EQ(index.made_by.plugin, "com.example.recorder");
    REQUIRE_EQ(index.made_by.version, "1.0.0");
    REQUIRE(index.rule_sets == options.rule_sets);
    REQUIRE(index.redactions == options.redactions);
    REQUIRE_EQ(index.places.size(), std::size_t{2});
    REQUIRE_EQ(index.places[0].file, "1.jsonl");
    REQUIRE_EQ(index.places[0].name, "COM7");
    REQUIRE_EQ(index.places[0].settings, "19200 8N1");
    REQUIRE(index.places[0].kind == place_kind::port);
    REQUIRE_EQ(index.places[0].bytes, std::uint64_t{5002});
    REQUIRE_EQ(index.places[1].file, "2.jsonl");
    REQUIRE_EQ(index.places[1].name, options.places[1].name);
    REQUIRE_EQ(index.places[1].settings, "");
    REQUIRE(index.places[1].kind == place_kind::link);
    REQUIRE_EQ(index.places[1].bytes, std::uint64_t{0});

    const auto walk = [&](std::size_t place)
    {
        auto reader = found->open_place(place);
        REQUIRE(reader.has_value());
        std::vector<recording_event> events;
        while (true)
        {
            auto next = reader->next();
            REQUIRE(next.has_value());
            if (!next->has_value())
                break;
            events.push_back(**next);
        }
        // At the end it stays at the end.
        const auto again = reader->next();
        REQUIRE(again.has_value());
        REQUIRE(!again->has_value());
        return events;
    };

    const auto first = walk(0);
    REQUIRE_EQ(first.size(), std::size_t{5});
    REQUIRE(first[0].kind == recording_event_kind::data);
    REQUIRE_EQ(first[0].t_ns, std::uint64_t{100});
    REQUIRE(first[0].direction == data_direction::rx);
    REQUIRE(first[0].bytes == bytes_of({0xde, 0xad}));
    // The 5000 bytes are two lines of 4096 and 904.
    REQUIRE(first[1].direction == data_direction::tx);
    REQUIRE_EQ(first[1].bytes.size(), std::size_t{4096});
    REQUIRE_EQ(first[2].bytes.size(), std::size_t{904});
    REQUIRE_EQ(first[2].t_ns, std::uint64_t{200});
    REQUIRE(first[1].bytes[4095] == big[4095]);
    REQUIRE(first[2].bytes[0] == big[4096]);
    REQUIRE(first[3].kind == recording_event_kind::dropped);
    REQUIRE_EQ(first[3].lost, std::uint64_t{30});
    REQUIRE(first[4].kind == recording_event_kind::line);
    REQUIRE(first[4].lines ==
            lines_set({control_line::dcd, control_line::dsr}));
    REQUIRE(first[4].changed ==
            lines_set({control_line::dcd, control_line::dsr}));

    const auto second = walk(1);
    REQUIRE_EQ(second.size(), std::size_t{3});
    REQUIRE(second[0].kind == recording_event_kind::line);
    REQUIRE(second[0].lines == lines_set({control_line::cts}));
    REQUIRE(second[1].kind == recording_event_kind::fault);
    REQUIRE_EQ(second[1].fault, "break");
    REQUIRE(second[2].kind == recording_event_kind::dropped);
    REQUIRE_EQ(second[2].lost, std::uint64_t{12});

    expect_code(found->open_place(2).error(), recording_errc::invalid_argument);
}

// -- the reader: a place file that is wrong ---------------------------------

namespace
{

constexpr std::string_view good_line =
    "{\"t_ns\":1,\"kind\":\"fault\",\"fault\":\"x\"}"sv;

/// A recording of one place whose file is `content`, with an index that is
/// right about it, so that what is wrong is in the lines.
void write_hand_made(const fs::path& folder, std::string_view content,
                     std::uint64_t duration_ns = 1'000'000'000'000)
{
    fs::create_directories(folder);
    spit(folder / "1.jsonl", content);
    std::string index =
        "{\"recording_format\":1,\"origin_utc\":\"2026-11-13T09:41:07Z\","
        "\"time_unit\":\"ns\",\"duration_ns\":";
    index.append(std::to_string(duration_ns));
    index.append(",\"seed\":7,\"places\":[{\"file\":\"1.jsonl\","
                 "\"kind\":\"port\",\"name\":\"COM7\",\"settings\":\"\","
                 "\"bytes\":0,\"sha256\":\"");
    index.append(sha256_of(content));
    index.append("\"}],\"rule_sets\":[],\"redactions\":[],\"dropped\":0,"
                 "\"made_by\":{\"plugin\":\"com.example.recorder\","
                 "\"version\":\"1.0.0\"}}\n");
    spit(folder / "recording.json", index);
}

std::string with_line_end(std::string_view line)
{
    std::string out(line);
    out.push_back('\n');
    return out;
}

std::string data_line(std::string_view middle)
{
    std::string out = "{\"t_ns\":1,\"kind\":\"data\",";
    out.append(middle);
    out.append("}\n");
    return out;
}

std::string line_change(std::string_view middle)
{
    std::string out = "{\"t_ns\":1,\"kind\":\"line\",";
    out.append(middle);
    out.append("}\n");
    return out;
}

constexpr std::string_view six_off =
    "\"lines\":{\"dtr\":false,\"rts\":false,\"cts\":false,\"dsr\":false,"
    "\"dcd\":false,\"ri\":false}"sv;

std::string with_changed(std::string_view changed)
{
    std::string middle(six_off);
    middle.append(",\"changed\":");
    middle.append(changed);
    return line_change(middle);
}

} // namespace

TEST_CASE("plugin recording: a place file that breaks the format is refused")
{
    const std::string padded_start =
        "{\"t_ns\":1,\"kind\":\"fault\",\"fault\":\"x\",\"pad\":\"";
    const std::string long_line =
        padded_start + std::string(20'000, 'a') + "\"}\n";
    const std::string hex5000(10'000, 'a');

    const std::vector<std::pair<std::string, std::string>> cases{
        {"a kind it does not know",
         with_line_end("{\"t_ns\":1,\"kind\":\"mystery\"}")},
        {"a line of 20000 bytes", long_line},
        {"a line that is an array", "[1,2]\n"},
        {"a line that is a number", "5\n"},
        {"a line that is not JSON", "hello\n"},
        {"a repeated key",
         with_line_end("{\"t_ns\":1,\"t_ns\":2,\"kind\":\"fault\","
                       "\"fault\":\"x\"}")},
        {"a repeated key with another value",
         with_line_end("{\"t_ns\":1,\"kind\":\"fault\",\"fault\":\"x\","
                       "\"fault\":\"y\"}")},
        {"a t_ns lower than the line before",
         with_line_end("{\"t_ns\":5,\"kind\":\"fault\",\"fault\":\"x\"}") +
             with_line_end("{\"t_ns\":4,\"kind\":\"fault\",\"fault\":\"x\"}")},
        {"no final line feed", std::string(good_line)},
        {"a carriage return", std::string(good_line) + "\r\n"},
        {"a carriage return where JSON allows white space",
         std::string(good_line.substr(0, 10)) + "\r" +
             std::string(good_line.substr(10)) + "\n"},
        {"a blank line",
         with_line_end(good_line) + "\n" + with_line_end(good_line)},
        {"a blank line only", "\n"},
        {"a blank line at the end", with_line_end(good_line) + "\n"},
        {"a byte-order mark", "\xEF\xBB\xBF" + with_line_end(good_line)},
        {"no t_ns", with_line_end("{\"kind\":\"fault\",\"fault\":\"x\"}")},
        {"a t_ns below zero",
         with_line_end("{\"t_ns\":-1,\"kind\":\"fault\",\"fault\":\"x\"}")},
        {"a t_ns that is not whole",
         with_line_end("{\"t_ns\":1.5,\"kind\":\"fault\",\"fault\":\"x\"}")},
        {"a t_ns that is a whole number written as a fraction",
         with_line_end("{\"t_ns\":2.0,\"kind\":\"fault\",\"fault\":\"x\"}")},
        {"a t_ns that is text",
         with_line_end("{\"t_ns\":\"1\",\"kind\":\"fault\",\"fault\":\"x\"}")},
        {"a t_ns of 2^53",
         with_line_end("{\"t_ns\":9007199254740992,\"kind\":\"fault\","
                       "\"fault\":\"x\"}")},
        {"a t_ns above the duration in the index",
         with_line_end("{\"t_ns\":1000000000001,\"kind\":\"fault\","
                       "\"fault\":\"x\"}")},
        {"a kind that is not text",
         with_line_end("{\"t_ns\":1,\"kind\":5,\"fault\":\"x\"}")},
        {"no kind", with_line_end("{\"t_ns\":1,\"fault\":\"x\"}")},
        {"a data line of 5000 bytes",
         data_line("\"direction\":\"R\",\"bytes\":5000,\"hex\":\"" + hex5000 +
                   "\"")},
        {"a data line of 4097 bytes",
         data_line("\"direction\":\"R\",\"bytes\":4097,\"hex\":\"" +
                   std::string(8194, 'a') + "\"")},
        {"a data line of no bytes",
         data_line("\"direction\":\"R\",\"bytes\":0,\"hex\":\"\"")},
        {"a hex of odd length",
         data_line("\"direction\":\"R\",\"bytes\":2,\"hex\":\"abc\"")},
        {"a hex of the wrong length",
         data_line("\"direction\":\"R\",\"bytes\":2,\"hex\":\"ab\"")},
        {"upper-case hex",
         data_line("\"direction\":\"R\",\"bytes\":1,\"hex\":\"AB\"")},
        {"a hex with a character that is not a digit",
         data_line("\"direction\":\"R\",\"bytes\":1,\"hex\":\"zz\"")},
        {"a hex with a prefix",
         data_line("\"direction\":\"R\",\"bytes\":1,\"hex\":\"0x\"")},
        {"a hex with a space",
         data_line("\"direction\":\"R\",\"bytes\":1,\"hex\":\"a \"")},
        {"a hex that is not text",
         data_line("\"direction\":\"R\",\"bytes\":1,\"hex\":12")},
        {"a direction in lower case",
         data_line("\"direction\":\"r\",\"bytes\":1,\"hex\":\"ab\"")},
        {"no direction", data_line("\"bytes\":1,\"hex\":\"ab\"")},
        {"bytes that is text",
         data_line("\"direction\":\"R\",\"bytes\":\"1\",\"hex\":\"ab\"")},
        {"a line change with a line missing",
         line_change("\"lines\":{\"dtr\":false,\"rts\":false,\"cts\":false,"
                     "\"dsr\":false,\"dcd\":false},\"changed\":[\"dtr\"]")},
        {"a line change with a seventh line",
         line_change("\"lines\":{\"dtr\":false,\"rts\":false,\"cts\":false,"
                     "\"dsr\":false,\"dcd\":false,\"ri\":false,"
                     "\"brk\":false},\"changed\":[\"dtr\"]")},
        {"a line change with a state that is not true or false",
         line_change("\"lines\":{\"dtr\":1,\"rts\":false,\"cts\":false,"
                     "\"dsr\":false,\"dcd\":false,\"ri\":false},"
                     "\"changed\":[\"dtr\"]")},
        {"a line change with another name for a line",
         line_change("\"lines\":{\"dtr\":false,\"rts\":false,\"cts\":false,"
                     "\"dsr\":false,\"dcd\":false,\"rng\":false},"
                     "\"changed\":[\"dtr\"]")},
        {"a line change with lines that is not an object",
         line_change("\"lines\":[],\"changed\":[\"dtr\"]")},
        {"a line change that changes nothing", with_changed("[]")},
        {"a line change that changes seven",
         with_changed("[\"dtr\",\"rts\",\"cts\",\"dsr\",\"dcd\",\"ri\","
                      "\"dtr\"]")},
        {"a line change that names a line twice",
         with_changed("[\"dtr\",\"dtr\"]")},
        {"a line change that names no line", with_changed("[\"break\"]")},
        {"a line change whose changed is not text", with_changed("[1]")},
        {"a line change without changed", line_change(six_off)},
        {"a fault of no bytes",
         with_line_end("{\"t_ns\":1,\"kind\":\"fault\",\"fault\":\"\"}")},
        {"a fault of 65 bytes",
         with_line_end("{\"t_ns\":1,\"kind\":\"fault\",\"fault\":\"" +
                       std::string(65, 'x') + "\"}")},
        {"a fault that is not text",
         with_line_end("{\"t_ns\":1,\"kind\":\"fault\",\"fault\":7}")},
        {"a fault with a control character",
         with_line_end(
             "{\"t_ns\":1,\"kind\":\"fault\",\"fault\":\"a\\u0001\"}")},
        {"a dropped of no bytes",
         with_line_end("{\"t_ns\":1,\"kind\":\"dropped\",\"bytes\":0}")},
        {"a dropped without bytes",
         with_line_end("{\"t_ns\":1,\"kind\":\"dropped\"}")},
    };

    for (const auto& [label, content] : cases)
    {
        const scratch_directory scratch;
        const fs::path folder = scratch.path() / "rec";
        write_hand_made(folder, content);
        std::string detail;
        const auto found = read_recording(folder, &detail);
        if (found.has_value() ||
            found.error() != make_error_code(recording_errc::place_invalid))
        {
            throw std::runtime_error("not refused as a bad place file: " +
                                     label);
        }
        // The words name the file and the line.
        REQUIRE(detail.find("1.jsonl") == 0);
        REQUIRE(detail.find("line ") != std::string::npos);
    }
}

TEST_CASE("plugin recording: a line of 16384 bytes is read and one more is not")
{
    const std::string start =
        "{\"t_ns\":1,\"kind\":\"fault\",\"fault\":\"x\",\"pad\":\"";
    const auto line_of = [&](std::size_t size)
    {
        std::string line = start;
        line.append(size - start.size() - 2, 'a');
        line.append("\"}");
        REQUIRE_EQ(line.size(), size);
        return with_line_end(line);
    };
    {
        const scratch_directory scratch;
        write_hand_made(scratch.path() / "rec", line_of(16'384));
        REQUIRE(read_recording(scratch.path() / "rec").has_value());
    }
    {
        const scratch_directory scratch;
        write_hand_made(scratch.path() / "rec", line_of(16'385));
        expect_code(reading(scratch.path() / "rec"),
                    recording_errc::place_invalid);
    }
}

TEST_CASE("plugin recording: a line has members the reader does not know")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    write_hand_made(
        folder,
        with_line_end("{\"t_ns\":1,\"kind\":\"fault\",\"fault\":\"x\","
                      "\"later\":{\"a\":[1,2,{\"b\":null}]}}") +
            with_line_end("{\"extra\":true,\"t_ns\":2,\"kind\":\"fault\","
                          "\"fault\":\"y\"}"));
    std::string detail;
    const auto found = read_recording(folder, &detail);
    if (!found.has_value())
        throw std::runtime_error(detail);
}

TEST_CASE("plugin recording: a data line of 4096 bytes is read")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    fs::create_directories(folder);
    const std::string content =
        data_line("\"direction\":\"W\",\"bytes\":4096,\"hex\":\"" +
                  std::string(8192, 'c') + "\"");
    spit(folder / "1.jsonl", content);
    std::string index = golden_index();
    replace_once(index, std::string(golden_sha256), sha256_of(content));
    replace_once(index, "\"bytes\":2,", "\"bytes\":4096,");
    replace_once(index, "\"dropped\":16", "\"dropped\":0");
    spit(folder / "recording.json", index);
    std::string detail;
    const auto found = read_recording(folder, &detail);
    if (!found.has_value())
        throw std::runtime_error(detail);
}

// -- the reader: an index that is wrong -------------------------------------

TEST_CASE("plugin recording: an index that breaks the format is refused")
{
    struct change
    {
        std::string label;
        std::string from;
        std::string to;
        recording_errc wanted = recording_errc::index_invalid;
    };
    const std::string sha(golden_sha256);
    std::string upper_sha = sha;
    for (char& letter : upper_sha)
    {
        if (letter >= 'a' && letter <= 'f')
            letter = static_cast<char>(letter - 32);
    }
    const std::string whole_places =
        "\"places\":[{\"file\":\"1.jsonl\",\"kind\":\"port\",\"name\":"
        "\"COM7\",\"settings\":\"19200 8N1\",\"bytes\":2,\"sha256\":\"" +
        sha + "\"}]";
    const std::vector<change> changes{
        {"a format above 1", "\"recording_format\":1", "\"recording_format\":2",
         recording_errc::format_unsupported},
        {"a format far above 1", "\"recording_format\":1",
         "\"recording_format\":18446744073709551615",
         recording_errc::format_unsupported},
        {"a format of 0", "\"recording_format\":1", "\"recording_format\":0"},
        {"a format below 0", "\"recording_format\":1",
         "\"recording_format\":-1"},
        {"a format as text", "\"recording_format\":1",
         "\"recording_format\":\"1\""},
        {"a format as a fraction", "\"recording_format\":1",
         "\"recording_format\":1.0"},
        {"no format", "\"recording_format\"", "\"recording_formats\""},
        {"no origin", "\"origin_utc\"", "\"origin\""},
        {"no time unit", "\"time_unit\"", "\"unit\""},
        {"no duration", "\"duration_ns\"", "\"duration\""},
        {"no seed", "\"seed\"", "\"sead\""},
        {"no places", "\"places\"", "\"place\""},
        {"no rule sets", "\"rule_sets\"", "\"rules\""},
        {"no redactions", "\"redactions\"", "\"redacted\""},
        {"no dropped", "\"dropped\":16", "\"drop\":16"},
        {"no made_by", "\"made_by\"", "\"made\""},
        {"a seed as text", "\"seed\":7", "\"seed\":\"7\""},
        {"a seed above 2^53", "\"seed\":7", "\"seed\":9007199254740992"},
        {"a duration with a fraction", "\"duration_ns\":4000",
         "\"duration_ns\":4000.5"},
        {"dropped below 0", "\"dropped\":16", "\"dropped\":-1"},
        {"rule sets that are an object", "\"rule_sets\":[]",
         "\"rule_sets\":{}"},
        {"redactions that are text", "\"redactions\":[]",
         "\"redactions\":\"none\""},
        {"places that are an object", "\"places\":[{", "\"places\":{\"a\":{"},
        {"no places in the list", whole_places, "\"places\":[]"},
        {"made_by that is a number",
         "\"made_by\":{\"plugin\":\"com.example.recorder\",\"version\":"
         "\"1.0.0\"}",
         "\"made_by\":5"},
        {"a plugin that is not an id", "\"plugin\":\"com.example.recorder\"",
         "\"plugin\":\"Not An Id\""},
        {"a version that is not a version", "\"version\":\"1.0.0\"",
         "\"version\":\"one\""},
        {"a time unit of ms", "\"time_unit\":\"ns\"", "\"time_unit\":\"ms\""},
        {"an origin with a space", "2026-11-13T09:41:07Z",
         "2026-11-13 09:41:07Z"},
        {"an origin with a zone", "2026-11-13T09:41:07Z",
         "2026-11-13T09:41:07+00:00"},
        {"an origin that is no day", "2026-11-13T09:41:07Z",
         "2026-02-30T09:41:07Z"},
        {"an origin of month 13", "2026-11-13T09:41:07Z",
         "2026-13-13T09:41:07Z"},
        {"an origin of hour 24", "2026-11-13T09:41:07Z",
         "2026-11-13T24:41:07Z"},
        {"an origin of second 60", "2026-11-13T09:41:07Z",
         "2026-11-13T09:41:60Z"},
        {"an origin with a fraction", "2026-11-13T09:41:07Z",
         "2026-11-13T09:41:07.5Z"},
        {"a file with a separator", "\"file\":\"1.jsonl\"",
         "\"file\":\"a/1.jsonl\""},
        {"a file in a parent", "\"file\":\"1.jsonl\"",
         "\"file\":\"../1.jsonl\""},
        {"a file that is ..", "\"file\":\"1.jsonl\"", "\"file\":\"..\""},
        {"a file with a back slash", "\"file\":\"1.jsonl\"",
         "\"file\":\"a\\\\1.jsonl\""},
        {"a file that is a device", "\"file\":\"1.jsonl\"",
         "\"file\":\"nul.jsonl\""},
        {"a file that is a device in mixed case", "\"file\":\"1.jsonl\"",
         "\"file\":\"Com1\""},
        {"a file that ends in a dot", "\"file\":\"1.jsonl\"",
         "\"file\":\"1.jsonl.\""},
        {"a file that ends in a space", "\"file\":\"1.jsonl\"",
         "\"file\":\"1.jsonl \""},
        {"a file with a colon", "\"file\":\"1.jsonl\"",
         "\"file\":\"c:1.jsonl\""},
        {"a file that is empty", "\"file\":\"1.jsonl\"", "\"file\":\"\""},
        {"a file that is the index", "\"file\":\"1.jsonl\"",
         "\"file\":\"recording.json\""},
        {"a file that is the index in upper case", "\"file\":\"1.jsonl\"",
         "\"file\":\"RECORDING.JSON\""},
        {"a file that is not text", "\"file\":\"1.jsonl\"", "\"file\":1"},
        {"a kind of place that is not known", "\"kind\":\"port\"",
         "\"kind\":\"serial\""},
        {"a name that is empty", "\"name\":\"COM7\"", "\"name\":\"\""},
        {"a name with a control character", "\"name\":\"COM7\"",
         "\"name\":\"COM\\u00007\""},
        {"settings that are not text", "\"settings\":\"19200 8N1\"",
         "\"settings\":19200"},
        {"bytes as text", "\"bytes\":2,", "\"bytes\":\"2\","},
        {"bytes below 0", "\"bytes\":2,", "\"bytes\":-2,"},
        {"a hash in upper case", sha, upper_sha},
        {"a hash that is short", sha, sha.substr(0, 63)},
        {"a hash with a character that is not hex", sha, "z" + sha.substr(1)},
        {"a repeated key", "\"seed\":7,", "\"seed\":7,\"seed\":8,"},
        {"a redaction of a place that is not recorded", "\"redactions\":[]",
         "\"redactions\":[{\"place\":\"COM9\",\"from_ns\":0,\"to_ns\":1,"
         "\"how\":\"blanked\"}]"},
        {"a redaction that ends after the recording", "\"redactions\":[]",
         "\"redactions\":[{\"place\":\"COM7\",\"from_ns\":0,\"to_ns\":4001,"
         "\"how\":\"blanked\"}]"},
        {"a redaction that ends before it starts", "\"redactions\":[]",
         "\"redactions\":[{\"place\":\"COM7\",\"from_ns\":9,\"to_ns\":1,"
         "\"how\":\"blanked\"}]"},
        {"a redaction that is another way", "\"redactions\":[]",
         "\"redactions\":[{\"place\":\"COM7\",\"from_ns\":0,\"to_ns\":1,"
         "\"how\":\"erased\"}]"},
        {"a redaction without a span", "\"redactions\":[]",
         "\"redactions\":[{\"place\":\"COM7\",\"how\":\"blanked\"}]"},
        {"a rule set without a generation", "\"rule_sets\":[]",
         "\"rule_sets\":[{\"text\":\"a\"}]"},
        {"a rule set whose text is a number", "\"rule_sets\":[]",
         "\"rule_sets\":[{\"generation\":1,\"text\":5}]"},
        {"a hash that is not the file's", sha, std::string(64, '0'),
         recording_errc::hash_mismatch},
        {"a byte count that is not the file's", "\"bytes\":2,", "\"bytes\":3,",
         recording_errc::size_mismatch},
        {"a dropped count that is not the lines'", "\"dropped\":16",
         "\"dropped\":15", recording_errc::size_mismatch},
        {"a duration below the last time", "\"duration_ns\":4000",
         "\"duration_ns\":3999", recording_errc::place_invalid},
        {"two places of the same name", "\"places\":[{",
         "\"places\":[{\"file\":\"2.jsonl\",\"kind\":\"port\",\"name\":"
         "\"COM7\",\"settings\":\"\",\"bytes\":0,\"sha256\":\"" +
             sha + "\"},{"},
    };

    for (const change& item : changes)
    {
        const scratch_directory scratch;
        const fs::path folder = scratch.path() / "rec";
        write_golden(folder);
        std::string index = golden_index();
        replace_once(index, item.from, item.to);
        spit(folder / "recording.json", index);

        std::string detail;
        const auto found = read_recording(folder, &detail);
        if (found.has_value() || found.error() != make_error_code(item.wanted))
        {
            throw std::runtime_error("wrong answer for: " + item.label);
        }
        if (detail.empty())
            throw std::runtime_error("no words for: " + item.label);
    }
}

TEST_CASE(
    "plugin recording: an index with more than the format asks for is read")
{
    struct change
    {
        std::string label;
        std::string from;
        std::string to;
    };
    const std::vector<change> changes{
        {"an unknown member at the top", "\"seed\":7,",
         "\"seed\":7,\"later\":{\"a\":[1,2,3]},"},
        {"an unknown member in a place", "\"bytes\":2,",
         "\"extra\":\"x\",\"bytes\":2,"},
        {"an unknown member of made_by", "\"version\":\"1.0.0\"",
         "\"version\":\"1.0.0\",\"build\":12"},
        {"the members in another order",
         "\"time_unit\":\"ns\",\"duration_ns\":4000,",
         "\"duration_ns\":4000,\"time_unit\":\"ns\","},
        {"white space", "\"seed\":7,", "\"seed\" : 7 ,\n  "},
        {"a rule set", "\"rule_sets\":[]",
         "\"rule_sets\":[{\"generation\":2,\"text\":\"a\\nb\"}]"},
        {"a redaction", "\"redactions\":[]",
         "\"redactions\":[{\"place\":\"COM7\",\"from_ns\":10,\"to_ns\":4000,"
         "\"how\":\"blanked\",\"note\":1}]"},
        {"a link as the kind", "\"kind\":\"port\"", "\"kind\":\"link\""},
        {"an origin in 1970", "2026-11-13T09:41:07Z", "1970-01-01T00:00:00Z"},
    };
    for (const change& item : changes)
    {
        const scratch_directory scratch;
        const fs::path folder = scratch.path() / "rec";
        write_golden(folder);
        std::string index = golden_index();
        replace_once(index, item.from, item.to);
        spit(folder / "recording.json", index);
        std::string detail;
        if (!read_recording(folder, &detail).has_value())
            throw std::runtime_error("refused: " + item.label + ": " + detail);
    }
}

TEST_CASE("plugin recording: a place file that is missing is refused")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    write_golden(folder);
    fs::remove(folder / "1.jsonl");
    std::string detail;
    expect_code(reading(folder, &detail), recording_errc::file_missing);
    REQUIRE(detail.find("1.jsonl") != std::string::npos);
}

TEST_CASE(
    "plugin recording: an index that is too large or has a mark is refused")
{
    {
        const scratch_directory scratch;
        const fs::path folder = scratch.path() / "rec";
        write_golden(folder);
        std::string index = golden_index();
        replace_once(index, "\"seed\":7,",
                     "\"seed\":7,\"pad\":\"" + std::string(1'100'000, 'a') +
                         "\",");
        spit(folder / "recording.json", index);
        std::string detail;
        expect_code(reading(folder, &detail), recording_errc::index_invalid);
        REQUIRE(!detail.empty());
    }
    {
        const scratch_directory scratch;
        const fs::path folder = scratch.path() / "rec";
        write_golden(folder);
        spit(folder / "recording.json", "\xEF\xBB\xBF" + golden_index());
        expect_code(reading(folder), recording_errc::index_invalid);
    }
    {
        const scratch_directory scratch;
        const fs::path folder = scratch.path() / "rec";
        write_golden(folder);
        spit(folder / "recording.json", golden_index() + "{}");
        expect_code(reading(folder), recording_errc::index_invalid);
    }
    {
        const scratch_directory scratch;
        const fs::path folder = scratch.path() / "rec";
        write_golden(folder);
        spit(folder / "recording.json", "[]\n");
        expect_code(reading(folder), recording_errc::index_invalid);
    }
}

TEST_CASE("plugin recording: a link or a file nobody listed is refused")
{
    const scratch_directory scratch;
    {
        const fs::path folder = scratch.path() / "extra-file";
        write_golden(folder);
        spit(folder / "notes.txt", "x");
        std::string detail;
        expect_code(reading(folder, &detail), recording_errc::layout_invalid);
        REQUIRE(detail.find("notes.txt") != std::string::npos);
    }
    {
        const fs::path folder = scratch.path() / "extra-folder";
        write_golden(folder);
        fs::create_directory(folder / "sub");
        expect_code(reading(folder), recording_errc::layout_invalid);
    }
    {
        // A place file with a second name is a link.
        const fs::path folder = scratch.path() / "hard";
        write_golden(folder);
        std::error_code failure;
        fs::create_hard_link(folder / "1.jsonl", scratch.path() / "other",
                             failure);
        if (!failure)
            expect_code(reading(folder), recording_errc::layout_invalid);
    }
    {
        // A symbolic link may need a privilege, so this one is skipped where
        // the system does not give it.
        const fs::path folder = scratch.path() / "sym";
        write_golden(folder);
        fs::rename(folder / "1.jsonl", scratch.path() / "moved.jsonl");
        std::error_code failure;
        fs::create_symlink(scratch.path() / "moved.jsonl", folder / "1.jsonl",
                           failure);
        if (!failure)
            expect_code(reading(folder), recording_errc::layout_invalid);
    }
    {
        const fs::path folder = scratch.path() / "whole";
        write_golden(folder);
        std::error_code failure;
        fs::create_directory_symlink(folder, scratch.path() / "via", failure);
        if (!failure)
        {
            expect_code(reading(scratch.path() / "via"),
                        recording_errc::layout_invalid);
        }
    }
    expect_code(reading(scratch.path() / "nothing"),
                recording_errc::file_missing);
}

TEST_CASE("plugin recording: a file that changes after the read is found")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    write_golden(folder);
    const auto found = read_recording(folder);
    REQUIRE(found.has_value());

    spit(folder / "1.jsonl",
         std::string(golden_place) +
             with_line_end(
                 "{\"t_ns\":4000,\"kind\":\"fault\",\"fault\":\"x\"}"));
    auto reader = found->open_place(0);
    REQUIRE(reader.has_value());
    boost::system::error_code failed;
    for (int i = 0; i < 10 && !failed; ++i)
    {
        const auto next = reader->next();
        if (!next.has_value())
            failed = next.error();
        else if (!next->has_value())
            break;
    }
    // The extra line is a good line; the SHA-256 is what says it is another
    // file, and the walk stays ended.
    expect_code(failed, recording_errc::hash_mismatch);
    REQUIRE(!reader->detail().empty());
    expect_code(reader->next().error(), recording_errc::hash_mismatch);
}

// -- ring ---------------------------------------------------------------------

TEST_CASE("plugin recording: a ring keeps about the last minutes of each place")
{
    const scratch_directory scratch;
    const fs::path folder = scratch.path() / "rec";
    recording_options options = options_with({"COM7", "COM8"});
    options.limits.ring_minutes = 2;
    recording_writer writer = opened(folder, options);

    constexpr std::uint64_t minute = 60'000'000'000;
    REQUIRE(writer.add_data(1, 0, data_direction::tx, bytes_of({0xee}))
                .has_value());
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        REQUIRE(writer
                    .add_data(0, i * minute, data_direction::rx,
                              bytes_of({static_cast<int>(i)}))
                    .has_value());
        REQUIRE(writer.add_dropped(0, i * minute + 1, 1).has_value());
    }
    // The minutes 0 and 1 of the first place are gone; the second place's
    // only line is still there until the recording ends.
    REQUIRE_EQ(writer.bytes(), std::uint64_t{4});
    REQUIRE_EQ(writer.dropped(), std::uint64_t{3});

    const auto summary = writer.finish();
    REQUIRE(summary.has_value());
    // At the end the second place's minute is older than two minutes before
    // the last, so it goes too.
    REQUIRE_EQ(summary->bytes, std::uint64_t{3});
    REQUIRE_EQ(summary->dropped, std::uint64_t{3});
    REQUIRE_EQ(summary->duration_ns, 4 * minute + 1);

    // The segments were joined into the one file of the format.
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(folder))
        names.push_back(entry.path().filename().string());
    std::ranges::sort(names);
    REQUIRE(names ==
            (std::vector<std::string>{"1.jsonl", "2.jsonl", "recording.json"}));

    const auto lines = lines_of(slurp(folder / "1.jsonl"));
    REQUIRE_EQ(lines.size(), std::size_t{6});
    REQUIRE_EQ(json::parse(lines[0]).at("t_ns").as_int64(),
               static_cast<std::int64_t>(2 * minute));
    REQUIRE_EQ(slurp(folder / "2.jsonl"), "");

    // The hash in the index is the hash of the file that was joined.
    const auto found = read_recording(folder);
    REQUIRE(found.has_value());
    REQUIRE_EQ(found->index.places[0].sha256,
               sha256_of(slurp(folder / "1.jsonl")));
    REQUIRE_EQ(found->index.places[0].bytes, std::uint64_t{3});
    REQUIRE_EQ(found->index.places[1].bytes, std::uint64_t{0});
    REQUIRE_EQ(found->index.dropped, std::uint64_t{3});
}
