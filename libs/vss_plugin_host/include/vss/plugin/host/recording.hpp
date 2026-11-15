//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The host's writer and reader of a traffic recording, the open format of
// docs/plugins.md, section 19.6.
//
// A recording is a folder.  It holds an index, `recording.json`, and one file
// for each recorded place (a port or a link).  A place file is UTF-8 JSON
// Lines: one JSON object for each line, each line ended by one line feed.
// Every line has `t_ns`, whole nanoseconds since the recording's
// `origin_utc`, and `kind`, which says what else the line holds:
//
//   data     `direction` ("R" or "W"), `bytes` (1 to 4096) and `hex` (the
//            bytes as 2 x `bytes` lower-case hexadecimal digits)
//   line     `lines`, the state of the six control lines after a change, and
//            `changed`, which of them changed
//   fault    `fault`, text of 1 to 64 bytes
//   dropped  `bytes`, how many bytes of traffic were lost at this point
//
// The writer keeps each place file open, hashes what it writes, and writes
// the index last, so a folder without an index is never taken for a whole
// recording.  The reader checks everything the format says and refuses
// whatever breaks it.  Neither starts a thread or reads a clock: times are
// given.  A writer is used from one thread.
//

#pragma once

#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/error.hpp>
#include <vss/core/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vss::plugin::host
{

/// Why a recording could not be written or was refused.
enum class recording_errc
{
    success = 0,
    /// The writer was given a value the format does not allow.
    invalid_argument,
    /// The folder to write exists and is not an empty folder.
    folder_not_usable,
    /// The operating system failed a write, a read or a rename.
    io_failed,
    /// An earlier write failed, so the writer refuses all others.
    writer_failed,
    /// `finish` was called, so the writer takes nothing more.
    finished,
    /// `t_ns` is lower than the last one written to the place.
    time_went_back,
    /// This part of the writer is not built.
    not_built,
    /// The index breaks the format.
    index_invalid,
    /// `recording_format` is higher than this reader reads.
    format_unsupported,
    /// A line of a place file breaks the format.
    place_invalid,
    /// The folder holds a link, a file the index does not list, or a file
    /// name the rules do not allow.
    layout_invalid,
    /// A file the index lists is not there.
    file_missing,
    /// A place file's `bytes` are not the index's.
    size_mismatch,
    /// A place file's SHA-256 is not the index's.
    hash_mismatch
};

/// The category of `recording_errc`, named `vss.recording`.
const boost::system::error_category& recording_category() noexcept;
boost::system::error_code make_error_code(recording_errc value) noexcept;

std::string_view to_string(recording_errc value) noexcept;

/// The index file's name.
inline constexpr std::string_view recording_index_name = "recording.json";

/// The highest `recording_format` this code reads and the one it writes.
inline constexpr std::uint64_t recording_format_number = 1;

/// The highest time, count or seed the format holds: 2^53 - 1, the largest
/// whole number every JSON reader keeps exactly.
inline constexpr std::uint64_t recording_max_number =
    (std::uint64_t{1} << 53) - 1;

/// A line of a place file is at most this many bytes, its line feed apart.
inline constexpr std::size_t recording_max_line_bytes = 16'384;

/// A `data` line holds at most this many bytes of traffic.  A longer chunk is
/// written as several lines with the same `t_ns`.
inline constexpr std::size_t recording_max_chunk_bytes = 4096;

/// The index is at most this many bytes.
inline constexpr std::size_t recording_max_index_bytes = std::size_t{1} << 20;

/// The most places a recording has: the 16 files of a bundle section, less
/// the index.
inline constexpr std::size_t recording_max_places = 15;

enum class place_kind
{
    port,
    link
};

std::string_view to_string(place_kind value) noexcept;

/// One place to record.
struct place_spec
{
    /// The name of the port or link, 1 to 256 bytes of plain text, different
    /// for every place of a recording.
    std::string name;
    place_kind kind = place_kind::port;
    /// What the place was set to, as text, such as `19200 8N1`.  At most 256
    /// bytes of plain text.
    std::string settings;
};

struct recording_limits
{
    /// The most bytes of traffic, the `bytes` of every `data` line added
    /// together over all places.  Zero is no limit.
    std::uint64_t limit_bytes = 0;
    /// The longest time.  A line later than this many seconds after the
    /// origin is not written.  Zero is no limit.
    std::uint64_t limit_seconds = 0;
    /// More than zero keeps about the last this many minutes of each place
    /// and drops older traffic as the recording goes on.  It is a recording
    /// that is left running, so it takes neither of the limits above.
    std::uint32_t ring_minutes = 0;
};

/// The `made_by` member of the index.
struct recording_made_by
{
    /// The plugin's id, as in its manifest.
    std::string plugin;
    /// The plugin's version, as in its manifest.
    std::string version;
};

/// A rule set that was installed while the recording ran.
struct recording_rule_set
{
    std::uint64_t generation = 0;
    /// The rule set's text.  Plain text in which line feeds, carriage
    /// returns and tabs are allowed.
    std::string text;

    friend bool operator==(const recording_rule_set&,
                           const recording_rule_set&) = default;
};

/// A span of one place's traffic that was replaced with a filler.
struct recording_redaction
{
    /// The `name` of a place of the recording.
    std::string place;
    std::uint64_t from_ns = 0;
    std::uint64_t to_ns = 0;
    /// The only way there is: "blanked".
    std::string how = "blanked";

    friend bool operator==(const recording_redaction&,
                           const recording_redaction&) = default;
};

/// What a recording starts with.
struct recording_options
{
    /// The moment `t_ns` of 0 is, to the second.
    std::chrono::sys_seconds origin_utc{};
    /// The seed of the run, 0 to `recording_max_number`.
    std::uint64_t seed = 0;
    /// 1 to `recording_max_places` places.  A place is named by its position
    /// in this list when lines are added.
    std::vector<place_spec> places;
    recording_limits limits;
    recording_made_by made_by;
    std::vector<recording_rule_set> rule_sets;
    std::vector<recording_redaction> redactions;
};

/// What `add_*` did.
enum class add_outcome
{
    /// Everything was written.
    written,
    /// A limit was reached by this call.  What fitted before it was written;
    /// `full()` is now true and nothing more is written.
    limit_reached,
    /// The writer was full already; nothing was written.
    was_full
};

/// What `finish` made.
struct recording_summary
{
    std::filesystem::path folder;
    /// The size of the index and every place file together.
    std::uint64_t size_on_disk = 0;
    /// The bytes of traffic in the recording.
    std::uint64_t bytes = 0;
    /// The bytes the recording lost.
    std::uint64_t dropped = 0;
    std::uint64_t duration_ns = 0;
    /// True when the recording stopped at a limit.
    bool limit_reached = false;
};

class recording_writer
{
public:
    /// Makes the folder, which must not exist or must be empty, and opens a
    /// file for each place.  The index is not written until `finish`.
    /// `detail`, when given, receives the words for a refusal.
    static result<recording_writer> open(const std::filesystem::path& folder,
                                         recording_options options,
                                         std::string* detail = nullptr);

    ~recording_writer();
    recording_writer(recording_writer&&) noexcept;
    recording_writer& operator=(recording_writer&&) noexcept;
    recording_writer(const recording_writer&) = delete;
    recording_writer& operator=(const recording_writer&) = delete;

    /// Each of the four adds one event to a place, `place` being the position
    /// of the place in `recording_options::places`.  `t_ns` is whole
    /// nanoseconds since the origin, at most `recording_max_number`, and not
    /// lower than the last one given for the same place.
    ///
    /// Not written, and an error: a value the format does not allow
    /// (`invalid_argument`), a time that goes back (`time_went_back`), a
    /// failed write (`io_failed`, and `writer_failed` for every call after
    /// it).  A limit is not an error: it is the outcome.
    ///
    /// `add_data`: `direction` is `rx` for `R` and `tx` for `W`, as the
    /// capture of `vss_studio` has them: `R` is bytes the port received,
    /// bytes that came in from whatever is attached to it, and `W` is bytes
    /// written to the port, bytes the hub sent out to that attachment.  A
    /// chunk of more than `recording_max_chunk_bytes` is written as several
    /// lines with the same `t_ns`.  An empty chunk writes nothing.
    result<add_outcome> add_data(std::size_t place, std::uint64_t t_ns,
                                 data_direction direction, bytes_view bytes);

    /// `add_line`: `lines` is the state of the six lines after the change and
    /// `changed` the ones that changed, at least one.  The break signal has
    /// no place in the format, so a set bit for it is refused.
    result<add_outcome> add_line(std::size_t place, std::uint64_t t_ns,
                                 control_lines lines, control_lines changed);

    /// `add_fault`: `fault` is 1 to 64 bytes of plain text.
    result<add_outcome> add_fault(std::size_t place, std::uint64_t t_ns,
                                  std::string_view fault);

    /// `add_dropped`: `bytes`, one or more, is how much traffic was lost at
    /// this point.
    result<add_outcome> add_dropped(std::size_t place, std::uint64_t t_ns,
                                    std::uint64_t bytes);

    /// True once a limit was reached.
    bool full() const noexcept;
    /// True once a write failed.
    bool failed() const noexcept;
    /// Bytes of traffic written, the traffic of the places added together.
    /// With a ring, what is kept so far.
    std::uint64_t bytes() const noexcept;
    /// Bytes reported lost by `add_dropped`.  With a ring, what is kept.
    std::uint64_t dropped() const noexcept;
    /// The highest `t_ns` written, in nanoseconds and in whole seconds.
    std::uint64_t duration_ns() const noexcept;
    std::uint64_t seconds() const noexcept;
    /// The words for the last error, for a person: no traffic in it.
    std::string_view detail() const noexcept;

    /// Flushes and closes every place file, writes the index to a temporary
    /// name in the folder and renames it to `recording.json`, last.  A writer
    /// that is destroyed without it leaves no index.
    result<recording_summary> finish();

private:
    struct state;
    explicit recording_writer(std::unique_ptr<state> impl) noexcept;

    std::unique_ptr<state> state_;
};

// -- reading ------------------------------------------------------------------

/// One place as the index lists it.
struct recording_place
{
    /// A plain file name in the recording's folder.
    std::string file;
    place_kind kind = place_kind::port;
    std::string name;
    std::string settings;
    /// The `bytes` of all the `data` lines of the file.
    std::uint64_t bytes = 0;
    /// The SHA-256 of the file as 64 lower-case hex digits.
    std::string sha256;
};

/// The index of a recording.
struct recording_index
{
    std::uint64_t format = recording_format_number;
    std::chrono::sys_seconds origin_utc{};
    std::uint64_t duration_ns = 0;
    std::uint64_t seed = 0;
    std::vector<recording_place> places;
    std::vector<recording_rule_set> rule_sets;
    std::vector<recording_redaction> redactions;
    std::uint64_t dropped = 0;
    recording_made_by made_by;
};

enum class recording_event_kind
{
    data,
    line,
    fault,
    dropped
};

/// One line of a place file.  Which members count depends on `kind`.
struct recording_event
{
    recording_event_kind kind = recording_event_kind::data;
    std::uint64_t t_ns = 0;
    /// `data`.
    data_direction direction = data_direction::rx;
    /// `data`: the bytes of the line, at most `recording_max_chunk_bytes`.
    byte_buffer bytes;
    /// `line`: the state of the lines after the change, and which changed.
    control_lines lines;
    control_lines changed;
    /// `fault`.
    std::string fault;
    /// `dropped`.
    std::uint64_t lost = 0;
};

/// Walks the lines of one place file, one at a time, in memory of a line's
/// size.  It checks every line as `read_recording` does, and at the end of the
/// file it checks the file's `bytes` and SHA-256 against the index again, so a
/// file that changed since `read_recording` is found.
class place_reader
{
public:
    ~place_reader();
    place_reader(place_reader&&) noexcept;
    place_reader& operator=(place_reader&&) noexcept;
    place_reader(const place_reader&) = delete;
    place_reader& operator=(const place_reader&) = delete;

    /// The next line, or nothing at the end of the file.  An error
    /// (`place_invalid`, `size_mismatch`, `hash_mismatch`, `io_failed`) ends
    /// the walk.
    result<std::optional<recording_event>> next();

    /// The words for the last error, for a person: no traffic in it.
    std::string_view detail() const noexcept;

private:
    friend struct recording;
    struct state;
    explicit place_reader(std::unique_ptr<state> impl) noexcept;

    std::unique_ptr<state> state_;
};

/// A recording that `read_recording` found whole.
struct recording
{
    std::filesystem::path folder;
    recording_index index;

    /// Opens the file of `index.places[place]` to walk it.
    result<place_reader> open_place(std::size_t place) const;
};

/// Reads the index and every place file of a recording and checks them against
/// the format, in memory of one line's size.  `detail`, when given, receives
/// one line that names the file and the line.
///
/// Refused: a folder that is a link or holds a link, a subfolder or a file
/// the index does not list (`layout_invalid`); an index over
/// `recording_max_index_bytes`, or one that breaks the format
/// (`index_invalid`); a `recording_format` above `recording_format_number`
/// (`format_unsupported`); a listed file that is not there (`file_missing`);
/// a line that breaks the format (`place_invalid`); a file whose traffic or
/// SHA-256 is not the index's (`size_mismatch`, `hash_mismatch`).
result<recording> read_recording(const std::filesystem::path& folder,
                                 std::string* detail = nullptr);

} // namespace vss::plugin::host

namespace boost::system
{
template <>
struct is_error_code_enum<vss::plugin::host::recording_errc> : std::true_type
{
};
} // namespace boost::system
