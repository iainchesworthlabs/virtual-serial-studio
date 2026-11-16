//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/capture_point.hpp>

#include <vss/core/control_lines.hpp>
#include <vss/core/hub.hpp>
#include <vss/core/stats.hpp>
#include <vss/core/types.hpp>
#include <vss/plugin/api_capture.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/recording.hpp>
#include <vss/plugin/host/traffic_tap.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss::plugin::host
{

namespace
{

/// The folder of a plugin's recordings, inside its data directory.
constexpr std::string_view recordings_folder = "recordings";

/// How much is taken from the tap at a time.
constexpr std::size_t take_items = 256;
constexpr std::size_t take_bytes = std::size_t{1} * 1024 * 1024;

/// The six lines a recording writes.
constexpr unsigned recorded_lines = 0x3fU;

/// How many names the host tries for a recording it names itself.
constexpr int most_names = 10000;

std::uint64_t nanoseconds_between(time_point from, time_point to) noexcept
{
    if (to <= from)
        return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(to - from)
            .count());
}

class capture_point final : public extension_handler
{
public:
    capture_point(extension_context context, hub& source, place_finder find)
        : context_(std::move(context)), hub_(&source), find_(std::move(find)),
          alive_(std::make_shared<bool>(true))
    {
    }

    ~capture_point() override
    {
        *alive_ = false;
        try
        {
            end_recording();
        }
        catch (...)
        {
            // A recording that cannot be finished is left without its
            // index, which is how a reader knows it is not whole.
            current_.reset();
        }
    }

    bool take(const api_message& message) override
    {
        if (message.op == capture_start_message::op())
        {
            start(message);
            return true;
        }
        if (message.op == capture_stop_message::op())
        {
            stop(message);
            return true;
        }
        if (message.op == capture_status_message::op())
        {
            status(message);
            return true;
        }
        return false;
    }

    void plugin_gone() override
    {
        end_recording();
        current_.reset();
    }

private:
    /// One recording, while it runs and after.
    struct recording_run
    {
        std::unique_ptr<recording_writer> writer;
        std::unique_ptr<traffic_tap> tap;
        std::vector<std::string> names;
        std::vector<control_lines> lines;
        std::vector<std::uint64_t> last_ns;
        time_point started{};
        bool ring = false;
        /// The longest the recording runs, in seconds; 0 for a ring.
        std::uint64_t limit_seconds = 0;
        /// The recording's folder, relative to the data directory.
        std::string place;
        /// Set once the recording is finished.
        std::optional<recording_summary> summary;
        /// The last figures of a recording that could not be finished.
        std::uint64_t bytes = 0;
        std::uint64_t seconds = 0;
        std::uint64_t dropped = 0;
        bool over = false;
        /// True when it ended because it could not be written.
        bool failed = false;
    };

    void answer(std::uint64_t id, api_message message)
    {
        if (id != 0)
            context_.link.answer(id, std::move(message));
    }

    void refuse(std::uint64_t id, api_errc code, const std::string& text)
    {
        answer(id, error_answer(code, text));
    }

    bool running() const noexcept
    {
        return current_.has_value() && !current_->over;
    }

    void start(const api_message& message)
    {
        const auto asked = decode_as<capture_start_message>(message);
        if (!asked)
        {
            refuse(message.id, api_errc::bad_request,
                   "the recording was not asked for as it must be");
            return;
        }
        if (!context_.may(permission::traffic_tap))
        {
            refuse(message.id, api_errc::not_granted,
                   "the plugin may not see traffic");
            return;
        }
        if (running())
        {
            refuse(message.id, api_errc::busy, "a recording is running");
            return;
        }
        if (asked->snapshot_on.has_value() || asked->before_s != 0 ||
            asked->after_s != 0)
        {
            // A ring that is cut out around a trigger needs the decoded
            // stream, which this host does not record from yet.
            refuse(message.id, api_errc::unsupported,
                   "a snapshot on a trigger is not supported");
            return;
        }

        recording_options options;
        std::vector<tap_place> watched;
        std::vector<control_lines> lines;
        for (const std::string& name : asked->places)
        {
            auto found = find_ ? find_(name) : std::nullopt;
            if (!found)
            {
                refuse(message.id, api_errc::refused,
                       "one of the places does not exist");
                return;
            }
            found->tap.name = name;
            control_lines known;
            known.bits = found->lines.bits & recorded_lines;
            lines.push_back(known);
            options.places.push_back(
                place_spec{name, found->kind, std::move(found->settings)});
            watched.push_back(std::move(found->tap));
        }
        // The origin is a whole second, so the times of the recording
        // are counted from that second and not from this moment.
        const auto now_utc = std::chrono::system_clock::now();
        options.origin_utc = std::chrono::floor<std::chrono::seconds>(now_utc);
        const time_point started =
            clock_type::now() -
            std::chrono::duration_cast<clock_type::duration>(
                now_utc - options.origin_utc);
        const std::uint64_t limit_seconds =
            asked->ring > 0 ? 0 : asked->limit_seconds;
        options.made_by.plugin = context_.plugin_id;
        options.made_by.version = context_.plugin_version;
        const bool ring = asked->ring > 0;
        if (ring)
        {
            options.limits.ring_minutes =
                static_cast<std::uint32_t>(asked->ring);
        }
        else
        {
            options.limits.limit_bytes = asked->limit_bytes;
            options.limits.limit_seconds = asked->limit_seconds;
        }

        const auto folder = folder_for(asked->name);
        if (!folder)
        {
            refuse(message.id, api_errc::refused,
                   "there is a recording of that name already");
            return;
        }
        auto writer = recording_writer::open(context_.data_directory /
                                                 std::filesystem::path(*folder),
                                             std::move(options));
        if (!writer)
        {
            refuse(message.id, api_errc::refused,
                   "the recording could not be started");
            return;
        }

        recording_run run;
        run.writer = std::make_unique<recording_writer>(std::move(*writer));
        run.names = asked->places;
        run.lines = std::move(lines);
        run.last_ns.resize(run.names.size(), 0);
        run.started = started;
        run.limit_seconds = limit_seconds;
        run.ring = ring;
        run.place = *folder;
        run.tap = std::make_unique<traffic_tap>(*hub_, std::move(watched));
        current_.emplace(std::move(run));
        current_->tap->set_notify(
            [alive = alive_, this]()
            {
                if (*alive)
                    drain();
            },
            context_.executor);
        answer(message.id, ok_answer());
    }

    /// The folder of a new recording, relative to the data directory:
    /// the plugin's name for it, or one the host makes.  Nothing when the
    /// name is taken.
    std::optional<std::string> folder_for(const std::string& name)
    {
        const std::filesystem::path base =
            context_.data_directory / std::filesystem::path(recordings_folder);
        std::error_code ignored;
        std::filesystem::create_directories(base, ignored);
        const auto taken = [&](const std::string& candidate)
        {
            std::error_code failed;
            return std::filesystem::exists(
                std::filesystem::symlink_status(base / candidate, failed));
        };
        std::string chosen = name;
        if (chosen.empty())
        {
            for (int number = 1; number <= most_names; ++number)
            {
                std::string candidate = "recording-";
                candidate.append(std::to_string(number));
                if (!taken(candidate))
                {
                    chosen = std::move(candidate);
                    break;
                }
            }
            if (chosen.empty())
                return std::nullopt;
        }
        else if (taken(chosen))
        {
            return std::nullopt;
        }
        std::string relative(recordings_folder);
        relative.push_back('/');
        relative.append(chosen);
        return relative;
    }

    /// Writes what the tap holds into the recording.
    void drain()
    {
        if (!current_.has_value() || current_->over || !current_->tap)
            return;
        recording_run& run = *current_;
        const tap_batch batch = run.tap->take(take_items, take_bytes);
        for (const tap_item& item : batch.items)
        {
            const auto found = std::ranges::find(run.names, item.place);
            if (found == run.names.end())
                continue;
            const auto index =
                static_cast<std::size_t>(found - run.names.begin());
            // Times never go back within a place, whatever the order two
            // threads handed their events over in.
            const std::uint64_t when =
                std::max(nanoseconds_between(run.started, item.when),
                         run.last_ns[index]);
            run.last_ns[index] = when;
            if (!write(run, index, when, item))
                break;
        }
        // What the tap could not hold is written for the place it was
        // lost at, at the moment it was noticed.
        for (const tap_drop& lost : batch.dropped)
        {
            if (!running() || lost.bytes == 0)
                continue;
            const auto found = std::ranges::find(run.names, lost.place);
            if (found == run.names.end())
                continue;
            const auto index =
                static_cast<std::size_t>(found - run.names.begin());
            const std::uint64_t when =
                std::max(nanoseconds_between(run.started, clock_type::now()),
                         run.last_ns[index]);
            run.last_ns[index] = when;
            took(run, run.writer->add_dropped(index, when, lost.bytes));
        }
        if (running() && run.tap->queued_items() > 0)
        {
            boost::asio::post(context_.executor,
                              [alive = alive_, this]()
                              {
                                  if (*alive)
                                      drain();
                              });
        }
    }

    /// Writes one item.  False when the recording is over.
    bool write(recording_run& run, std::size_t index, std::uint64_t when,
               const tap_item& item)
    {
        switch (item.kind)
        {
        case tap_item_kind::data:
            if (item.bytes == nullptr || item.bytes->empty())
                return true;
            return took(run, run.writer->add_data(index, when, item.direction,
                                                  *item.bytes));
        case tap_item_kind::lines:
        {
            // The break signal has no place in a recording.
            control_lines now;
            now.bits = item.lines.bits & recorded_lines;
            control_lines changed;
            changed.bits = now.bits ^ run.lines[index].bits;
            run.lines[index] = now;
            if (changed.bits == 0)
                return true;
            return took(run, run.writer->add_line(index, when, now, changed));
        }
        case tap_item_kind::error:
            return took(run, run.writer->add_fault(index, when, "error"));
        case tap_item_kind::bus_fault:
            return took(
                run, run.writer->add_fault(index, when, to_string(item.fault)));
        }
        return true;
    }

    /// What the writer said of one line.  False when the recording is
    /// over: a limit was reached, or the disk failed.
    bool took(recording_run& run, const result<add_outcome>& outcome)
    {
        if (outcome && *outcome == add_outcome::written)
            return true;
        // A limit is the recording's ordinary end; anything else is not.
        if (!outcome)
            run.failed = true;
        // The tap is let go at once: nothing more is wanted from the hub.
        run.over = true;
        run.tap.reset();
        return false;
    }

    /// The bytes of the files of a recording that is being written.
    std::uint64_t size_of_folder(const std::string& place) const
    {
        std::uint64_t total = 0;
        std::error_code failed;
        const std::filesystem::path folder =
            context_.data_directory / std::filesystem::path(place);
        for (std::filesystem::directory_iterator next(folder, failed), end;
             !failed && next != end; next.increment(failed))
        {
            std::error_code ignored;
            if (!next->is_regular_file(ignored) || ignored)
                continue;
            const std::uintmax_t size = next->file_size(ignored);
            if (!ignored)
                total += static_cast<std::uint64_t>(size);
        }
        return total;
    }

    /// Finishes the recording that runs, when one does.
    void end_recording()
    {
        if (!current_.has_value() || current_->summary.has_value() ||
            current_->writer == nullptr)
            return;
        recording_run& run = *current_;
        if (run.tap)
        {
            // What the tap still holds belongs to the recording.
            while (!run.over && run.tap && run.tap->queued_items() > 0)
                drain();
            run.tap.reset();
        }
        run.over = true;
        run.bytes = run.writer->bytes();
        run.seconds = run.writer->seconds();
        run.dropped = run.writer->dropped();
        auto finished = run.writer->finish();
        run.writer.reset();
        if (finished)
            run.summary = std::move(*finished);
        else
            run.failed = true;
    }

    void stop(const api_message& message)
    {
        const auto asked = decode_as<capture_stop_message>(message);
        if (!asked)
        {
            refuse(message.id, api_errc::bad_request,
                   "the stop was not asked for as it must be");
            return;
        }
        const bool had = current_.has_value() && !current_->summary.has_value();
        end_recording();
        if (had && current_.has_value() && !current_->summary.has_value())
        {
            refuse(message.id, api_errc::refused,
                   "the recording could not be finished");
            return;
        }
        // Stopping what does not run is not an error.
        answer(message.id, ok_answer());
    }

    void status(const api_message& message)
    {
        const auto asked = decode_as<capture_status_message>(message);
        if (!asked)
        {
            refuse(message.id, api_errc::bad_request,
                   "the status was not asked for as it must be");
            return;
        }
        if (!current_.has_value())
        {
            refuse(message.id, api_errc::refused, "there is no recording");
            return;
        }
        // A recording that reached a limit is finished when it is asked
        // about, so that the plugin learns its size.
        // So is one whose time is up while nothing happens at its places.
        recording_run& run = *current_;
        if (!run.over && run.limit_seconds != 0 &&
            nanoseconds_between(run.started, clock_type::now()) /
                    1'000'000'000ULL >=
                run.limit_seconds)
        {
            run.over = true;
        }
        if (run.over)
            end_recording();
        capture_result_message answer_body;
        answer_body.running = running();
        answer_body.ring = run.ring;
        answer_body.recording = run.place;
        answer_body.failed = run.failed;
        if (run.summary.has_value())
        {
            answer_body.bytes = run.summary->bytes;
            answer_body.seconds = run.summary->duration_ns / 1'000'000'000ULL;
            answer_body.dropped = run.summary->dropped;
            answer_body.size = run.summary->size_on_disk;
        }
        else if (run.writer != nullptr)
        {
            answer_body.bytes = run.writer->bytes();
            answer_body.seconds = run.writer->seconds();
            answer_body.dropped = run.writer->dropped();
            answer_body.size = size_of_folder(run.place);
        }
        else
        {
            answer_body.bytes = run.bytes;
            answer_body.seconds = run.seconds;
            answer_body.dropped = run.dropped;
        }
        auto encoded = encode_as(answer_body, message.id);
        if (!encoded)
        {
            refuse(message.id, api_errc::refused,
                   "the status could not be written");
            return;
        }
        answer(message.id, std::move(*encoded));
    }

    extension_context context_;
    hub* hub_;
    place_finder find_;
    /// False once this object has gone, for what was posted and runs
    /// later.  Read and written on the service's strand only.
    std::shared_ptr<bool> alive_;
    std::optional<recording_run> current_;
};

} // namespace

std::unique_ptr<extension_handler>
make_capture_point(const extension_context& context, hub& source,
                   place_finder find)
{
    return std::make_unique<capture_point>(context, source, std::move(find));
}

} // namespace vss::plugin::host
