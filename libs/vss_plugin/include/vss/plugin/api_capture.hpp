//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages of recording, of the sections a plugin adds to a support
// bundle, and of replay from a recording.  docs/plugins.md, sections 19.4 to
// 19.6 and 20.8.
//
// `capture.*` asks the agent to record chosen places into a file of the
// plugin's data directory; the agent writes the file and the plugin never
// sees the traffic.  `traffic.tap` scopes it: the host refuses a place the
// user did not pick for the recording.  `timeline.load` hands the engine a
// recording to play onto a place.  `bundle.prepare` asks the plugin to write
// the files of one section of a bundle and to name them.
//
// Not defined, as the design names no message for them: the delivery of
// tapped traffic (`traffic.tap`, section 7.6), and the progress and the
// cancelling of `bundle.prepare` (section 19.5).
//

#pragma once

#include <vss/plugin/api_message.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin
{

/// The most places one `capture.start` names, and the most in the
/// `places` of its `snapshot_on`.  A choice: the design gives no limit.  A
/// name is at most 128 bytes and a list entry with its quotes and comma 131,
/// so two full lists are 2 x 128 x 131 = 33,536 bytes, and the rest of the
/// header is under 1,000: well inside the 64 KiB of a header.
inline constexpr std::size_t max_capture_places = 128;
/// The longest `name` of a recording, in bytes.
inline constexpr std::size_t max_capture_name = 64;
/// The largest `limit_bytes`: 256 MiB, the limit of a bundle section
/// (section 19.5).
inline constexpr std::uint64_t max_capture_bytes =
    std::uint64_t{256} * 1024 * 1024;
/// The `limit_bytes` of a recording that does not say: 64 MiB (section 19.6).
inline constexpr std::uint64_t default_capture_bytes =
    std::uint64_t{64} * 1024 * 1024;
/// The largest `limit_seconds`.  A choice: one hour.
inline constexpr std::uint64_t max_capture_seconds = 3600;
/// The `limit_seconds` of a recording that does not say: 10 minutes
/// (section 19.6).
inline constexpr std::uint64_t default_capture_seconds = 600;
/// The most minutes a ring keeps.  A choice: one hour.
inline constexpr std::uint64_t max_capture_ring_minutes = 60;
/// The most kinds in one `snapshot_on`: every kind of section 20.7 once.  A
/// choice.
inline constexpr std::size_t max_snapshot_kinds = 16;
/// The most seconds of `before_s` and of `after_s`.  A choice: ten minutes.
inline constexpr std::uint64_t max_snapshot_seconds = 600;

/// The least and the most `time_scale` of `timeline.load`.  A choice: a
/// replay a hundred times slower or faster.
inline constexpr double min_time_scale = 0.01;
inline constexpr double max_time_scale = 100.0;
/// The longest `recording` of `timeline.load`, in bytes.  A choice.
inline constexpr std::size_t max_recording_path = max_path_length;

/// The most files in one bundle section (section 19.5).
inline constexpr std::size_t max_bundle_files = 16;
/// The longest name of a file of a bundle section, in bytes.  A choice.
inline constexpr std::size_t max_bundle_file_name = 128;
/// The most bytes of all the files of one bundle section: 256 MiB
/// (section 19.5).  It is also the most of one file.
inline constexpr std::uint64_t max_bundle_section_bytes =
    std::uint64_t{256} * 1024 * 1024;

/// The kinds of a trigger that `snapshot_on` may name: the closed list of
/// section 20.7.
inline constexpr std::array<std::string_view, 11> snapshot_kinds{
    "framing",    "unknown_type", "unknown_value", "check",
    "length",     "timeout",      "sequence",      "contention",
    "line_error", "decoder_lost", "custom"};

/// When a trigger passes, the agent cuts a span out of the ring into a kept
/// recording (section 20.8 (c)).
struct snapshot_spec
{
    /// 1 to `max_snapshot_kinds` names from `snapshot_kinds`, none twice.
    std::vector<std::string> kinds;
    /// 0 to `max_capture_places` names, each 1 to `max_api_name` bytes of
    /// plain text.  None is every place of the recording.
    std::vector<std::string> places;

    friend bool operator==(const snapshot_spec&,
                           const snapshot_spec&) = default;
};

/// Starts a recording.  The plugin's request; the host answers "ok" or an
/// error (`not_granted` for a place `traffic.tap` does not cover).
struct capture_start_message
{
    static constexpr std::string_view op() noexcept { return "capture.start"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::request; }

    /// 1 to `max_capture_places` names, each 1 to `max_api_name` bytes of
    /// plain text, none twice.
    std::vector<std::string> places;
    /// The stem of the recording's file name, 0 to `max_capture_name` bytes,
    /// by the rules of a manifest path name and with no slash.  Empty: the
    /// host chooses one.  The answer to `capture.status` gives the path.
    std::string name;
    /// 1 to `max_capture_bytes`.  The recording stops when it is reached.
    std::uint64_t limit_bytes = default_capture_bytes;
    /// 1 to `max_capture_seconds`.  The recording stops when it is reached.
    std::uint64_t limit_seconds = default_capture_seconds;
    /// Whole minutes, 0 to `max_capture_ring_minutes`.  0 is not a ring.
    /// A ring keeps only the last this many minutes.
    std::uint64_t ring = 0;
    /// Given only when `ring` is not 0 (section 20.8 (c)).
    std::optional<snapshot_spec> snapshot_on;
    /// Seconds kept before the trigger and after it, 0 to
    /// `max_snapshot_seconds`.  Given only when `ring` is not 0.
    std::uint64_t before_s = 0;
    std::uint64_t after_s = 0;

    friend bool operator==(const capture_start_message&,
                           const capture_start_message&) = default;
};

/// Stops the recording.  The host answers "ok" or an error.
struct capture_stop_message
{
    static constexpr std::string_view op() noexcept { return "capture.stop"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::request; }

    friend bool operator==(const capture_stop_message&,
                           const capture_stop_message&) = default;
};

/// Asks how the recording is going.  The host answers `capture.result`.
struct capture_status_message
{
    static constexpr std::string_view op() noexcept { return "capture.status"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::request; }

    friend bool operator==(const capture_status_message&,
                           const capture_status_message&) = default;
};

/// The answer to `capture.status`.
struct capture_result_message
{
    static constexpr std::string_view op() noexcept { return "capture.result"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::answer; }

    bool running = false;
    /// Bytes recorded so far, 0 to `max_api_number`.
    std::uint64_t bytes = 0;
    /// Seconds recorded so far, 0 to `max_api_number`.
    std::uint64_t seconds = 0;
    /// Bytes the recording lost, 0 to `max_api_number`.  It should be 0.
    std::uint64_t dropped = 0;
    /// Whether the recording is a ring.
    bool ring = false;
    /// The file the host writes, as a path inside the plugin's data
    /// directory, by the rules of `check_relative_path`: 1 to
    /// `max_recording_path` bytes.  `timeline.load` and `bundle.result` use
    /// the same kind of path.
    std::string recording;
    /// The bytes of that file so far, 0 to `max_api_number`.
    std::uint64_t size = 0;
    /// True when the recording ended because it could not be written, such
    /// as a full disk: what it holds may be incomplete.  Left out when
    /// false.
    bool failed = false;

    friend bool operator==(const capture_result_message&,
                           const capture_result_message&) = default;
};

/// Hands the engine a recording to play onto a place, as a timeline.  The
/// plugin's request; the host answers "ok" or an error.
struct timeline_load_message
{
    static constexpr std::string_view op() noexcept { return "timeline.load"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;
    /// The recording, as a path inside the plugin's data directory by the
    /// rules of `check_relative_path`: 1 to `max_recording_path` bytes.
    std::string recording;
    /// The name the hub gives the place it plays onto: 1 to `max_api_name`
    /// bytes.
    std::string place;
    /// `min_time_scale` to `max_time_scale`.  1.0 is the recorded timing.
    double time_scale = 1.0;
    /// The seed of the replay, 0 to `max_api_number`.  None keeps the one
    /// the recording has.
    std::optional<std::uint64_t> seed;

    friend bool operator==(const timeline_load_message&,
                           const timeline_load_message&) = default;
};

/// Asks the plugin to write the files of one section of a support bundle.  The
/// plugin answers `bundle.result`, or an error, within 30 seconds.
struct bundle_prepare_message
{
    static constexpr std::string_view op() noexcept { return "bundle.prepare"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    /// The id of the section: 1 to `max_api_name` lower-case letters, digits,
    /// dots, hyphens and underscores, such as `traffic`, or a plugin id
    /// before a dot.
    std::string section;
    /// What the user chose for the section, as one JSON object.  "{}" for
    /// none.
    std::string settings = "{}";

    friend bool operator==(const bundle_prepare_message&,
                           const bundle_prepare_message&) = default;
};

/// One file of a bundle section, in the plugin's data directory.
struct bundle_file
{
    /// A path inside the folder of the section, by the rules of
    /// `check_relative_path`, of 1 to `max_bundle_file_name` bytes.  No two
    /// paths of one list are the same when case is ignored.
    std::string name;
    /// 0 to `max_bundle_section_bytes`.
    std::uint64_t size = 0;

    friend bool operator==(const bundle_file&, const bundle_file&) = default;
};

/// The answer to `bundle.prepare`.
struct bundle_result_message
{
    static constexpr std::string_view op() noexcept { return "bundle.result"; }
    static constexpr api_side from() noexcept { return api_side::plugin; }
    static constexpr api_role role() noexcept { return api_role::answer; }

    /// 0 to `max_bundle_files` files, no name twice.  The sizes add up to at
    /// most `max_bundle_section_bytes`.
    std::vector<bundle_file> files;

    friend bool operator==(const bundle_result_message&,
                           const bundle_result_message&) = default;
};

template <>
result<capture_start_message>
decode_as<capture_start_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<capture_start_message>(const capture_start_message&, std::uint64_t);
template <>
result<capture_stop_message> decode_as<capture_stop_message>(const api_message&,
                                                             std::string*);
template <>
result<api_message> encode_as<capture_stop_message>(const capture_stop_message&,
                                                    std::uint64_t);
template <>
result<capture_status_message>
decode_as<capture_status_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<capture_status_message>(const capture_status_message&, std::uint64_t);
template <>
result<capture_result_message>
decode_as<capture_result_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<capture_result_message>(const capture_result_message&, std::uint64_t);
template <>
result<timeline_load_message>
decode_as<timeline_load_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<timeline_load_message>(const timeline_load_message&, std::uint64_t);
template <>
result<bundle_prepare_message>
decode_as<bundle_prepare_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<bundle_prepare_message>(const bundle_prepare_message&, std::uint64_t);
template <>
result<bundle_result_message>
decode_as<bundle_result_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<bundle_result_message>(const bundle_result_message&, std::uint64_t);

} // namespace vss::plugin
