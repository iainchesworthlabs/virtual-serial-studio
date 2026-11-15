//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_cli plugin run - one installed plugin, run under the real plugin host,
// so that a person, a script or the tests of another repository can see it
// start, say hello, be confined and stop.
//
// The command makes a `plugin_service` with the plugin folders, trust file
// and data folder it is given, asks it to scan, and then only waits.  The
// service runs on an `io_context` of its own, one thread; its slow work runs
// on a pool of one thread.  The service's change handler does no more than
// keep the latest snapshot and wake the main thread, which prints, so that
// nothing on the service's strand waits for the terminal.
//
// It makes no trust decision: a plugin that needs one is reported as such and
// the command ends.  No licence is read, so a plugin that needs one does not
// run.  Text that came from a plugin is shown as plain text, or as an escaped
// JSON string with --json.  docs/plugins.md is the design.
//

#include "plugin_run_command.hpp"

#include "plugin_support.hpp"
#include "stop_signals.hpp"

#include <vss/core/error.hpp>
#include <vss/machine/folders.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/host/runner.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/host/service.hpp>
#include <vss/plugin/lifecycle.hpp>
#include <vss/plugin/locations.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace vss_cli
{
namespace
{

namespace fs = std::filesystem;
namespace host = vss::plugin::host;
namespace json = boost::json;
namespace plug = vss::plugin;
namespace sup = plugin_support;

using namespace std::chrono_literals;

constexpr int exit_ok = 0;
constexpr int exit_failed = 1;
constexpr int exit_usage = 2;

/// The longest `--seconds` takes: 31 days.
constexpr long long most_seconds = 2678400;

/// How often the main thread looks for a signal and for the end of `--seconds`
/// when nothing else wakes it.
constexpr std::chrono::milliseconds poll_interval{100};

/// How long the context may take to finish after the service has gone.
constexpr std::chrono::seconds finish_limit{10};

// -- the command line -------------------------------------------------------

struct options
{
    bool json = false;
    std::string id;
    std::vector<fs::path> roots;
    std::optional<fs::path> data;
    std::optional<fs::path> trust;
    long long seconds = 0;
};

enum class taken
{
    no,
    yes,
    bad
};

/// Reads the value of `name`, written as `--name value` or `--name=value`.
taken take_value(const std::vector<std::string>& arguments, std::size_t& index,
                 std::string_view name, std::string_view what,
                 std::string& value)
{
    const std::string& argument = arguments[index];
    const bool exact = argument == name;
    const bool joined = argument.size() > name.size() &&
                        argument.starts_with(name) &&
                        argument[name.size()] == '=';
    if (!exact && !joined)
        return taken::no;
    if (joined)
    {
        value = argument.substr(name.size() + 1);
    }
    else if (index + 1 < arguments.size())
    {
        ++index;
        value = arguments[index];
    }
    else
    {
        value.clear();
    }
    if (value.empty())
    {
        std::fprintf(stderr, "plugin run: %.*s needs %.*s\n",
                     static_cast<int>(name.size()), name.data(),
                     static_cast<int>(what.size()), what.data());
        return taken::bad;
    }
    return taken::yes;
}

bool parse_seconds(const std::string& text, long long& out)
{
    long long number = 0;
    const char* const end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end || number < 0 ||
        number > most_seconds)
    {
        std::fprintf(stderr,
                     "plugin run: --seconds needs a whole number of seconds, "
                     "0 to %lld\n",
                     most_seconds);
        return false;
    }
    out = number;
    return true;
}

/// Reads the words after `plugin run`.  When a word is wrong, says so and
/// returns false.
bool parse(const std::vector<std::string>& arguments, options& out)
{
    for (std::size_t index = 2; index < arguments.size(); ++index)
    {
        const std::string& argument = arguments[index];
        if (argument == "--json")
        {
            out.json = true;
            continue;
        }
        std::string value;
        taken got = take_value(arguments, index, "--root", "a folder", value);
        if (got == taken::yes)
        {
            out.roots.emplace_back(value);
            continue;
        }
        if (got == taken::no)
        {
            got = take_value(arguments, index, "--data", "a folder", value);
            if (got == taken::yes)
            {
                out.data = fs::path(value);
                continue;
            }
        }
        if (got == taken::no)
        {
            got = take_value(arguments, index, "--trust", "a file", value);
            if (got == taken::yes)
            {
                out.trust = fs::path(value);
                continue;
            }
        }
        if (got == taken::no)
        {
            got = take_value(arguments, index, "--seconds",
                             "a number of seconds", value);
            if (got == taken::yes)
            {
                if (!parse_seconds(value, out.seconds))
                    return false;
                continue;
            }
        }
        if (got == taken::bad)
            return false;
        if (out.id.empty() && argument.rfind("--", 0) != 0)
        {
            out.id = argument;
            continue;
        }
        std::fprintf(stderr, "plugin run: unknown argument %s\n",
                     argument.c_str());
        return false;
    }
    if (out.id.empty())
    {
        std::fputs("plugin run: the id of a plugin is needed\n", stderr);
        return false;
    }
    return true;
}

/// Where this command looks, and keeps what it needs.
struct places
{
    std::vector<plug::plugin_root> roots;
    fs::path programs;
    fs::path policy_file;
    fs::path trust_file;
    fs::path data_root;
};

/// The folders for these options, or nothing, with the reason on the error
/// stream.
std::optional<places> places_for(const options& given)
{
    places out;
    out.programs = sup::program_directory();
    if (given.roots.empty())
    {
        const plug::plugin_locations usual =
            plug::plugin_locations_for(out.programs);
        out.roots = usual.roots;
        out.policy_file = usual.policy_file;
    }
    else
    {
        for (const fs::path& folder : given.roots)
        {
            plug::plugin_root root;
            root.directory = folder;
            root.scope = plug::plugin_scope::for_user;
            out.roots.push_back(std::move(root));
        }
    }

    const fs::path own = vss::machine::user_folder();
    if (given.trust.has_value())
    {
        out.trust_file = given.trust.value();
    }
    else if (!own.empty())
    {
        out.trust_file = own / "trust.json";
    }
    else
    {
        std::fputs("plugin run: cannot work out your Virtual Serial Studio "
                   "folder on this computer; give the trust file with "
                   "--trust\n",
                   stderr);
        return std::nullopt;
    }
    if (given.data.has_value())
    {
        out.data_root = given.data.value();
    }
    else if (!own.empty())
    {
        out.data_root = own / "plugin-data";
    }
    else
    {
        std::fputs("plugin run: cannot work out your Virtual Serial Studio "
                   "folder on this computer; give the data folder with "
                   "--data\n",
                   stderr);
        return std::nullopt;
    }
    return out;
}

// -- what is shown ----------------------------------------------------------

/// What a plugin is doing, in the groups a person is told about.
enum class group
{
    starting,
    running,
    stopping,
    stopped,
    waiting,
    failed,
    off,
    decision,
    blocked,
    licence,
    unusable
};

struct group_names
{
    /// For the text.
    std::string_view words;
    /// For the JSON.
    std::string_view key;
};

group_names names_of(group which) noexcept
{
    switch (which)
    {
    case group::starting:
        return {"starting", "starting"};
    case group::running:
        return {"running", "running"};
    case group::stopping:
        return {"stopping", "stopping"};
    case group::stopped:
        return {"stopped", "stopped"};
    case group::waiting:
        return {"waiting to start again", "waiting_to_start_again"};
    case group::failed:
        return {"stopped after failing", "stopped_after_failing"};
    case group::off:
        return {"off", "off"};
    case group::decision:
        return {"needs your decision", "needs_decision"};
    case group::blocked:
        return {"blocked by the administrator", "blocked_by_administrator"};
    case group::licence:
        return {"needs a licence", "needs_licence"};
    case group::unusable:
        return {"cannot be used", "cannot_be_used"};
    }
    return {"unknown", "unknown"};
}

/// A state the plugin does not leave without the user: the command ends.
bool cannot_go_on(group which) noexcept
{
    switch (which)
    {
    case group::failed:
    case group::off:
    case group::decision:
    case group::blocked:
    case group::licence:
    case group::unusable:
        return true;
    case group::starting:
    case group::running:
    case group::stopping:
    case group::stopped:
    case group::waiting:
        break;
    }
    return false;
}

struct sandbox_view
{
    std::string level;
    std::vector<std::string> not_applied;
    std::string failure;
};

struct status_view
{
    std::string level;
    std::string text;
    std::string code;
};

/// One moment of the plugin, as the command shows it.
struct view
{
    group state = group::starting;
    std::string reason;
    std::string detail;
    std::optional<std::uint64_t> process;
    /// How the process last ended: its exit code, and whether it ended
    /// abnormally.
    std::optional<std::int64_t> exit_code;
    bool exit_abnormal = false;
    std::optional<sandbox_view> sandbox;
    std::optional<status_view> status;
    std::size_t failures = 0;
    std::size_t failure_limit = 0;
    /// When the next start is due, for the line.  It runs down by itself, so
    /// it is not part of what a change is.
    std::optional<std::chrono::milliseconds> wait;
};

bool same_sandbox(const std::optional<sandbox_view>& left,
                  const std::optional<sandbox_view>& right)
{
    if (left.has_value() != right.has_value())
        return false;
    if (!left.has_value())
        return true;
    return left.value().level == right.value().level &&
           left.value().not_applied == right.value().not_applied &&
           left.value().failure == right.value().failure;
}

bool same_status(const std::optional<status_view>& left,
                 const std::optional<status_view>& right)
{
    if (left.has_value() != right.has_value())
        return false;
    if (!left.has_value())
        return true;
    return left.value().level == right.value().level &&
           left.value().text == right.value().text &&
           left.value().code == right.value().code;
}

/// Whether the two say the same, which is not whether the wait is.
bool same_state(const view& left, const view& right)
{
    return left.state == right.state && left.reason == right.reason &&
           left.detail == right.detail && left.process == right.process &&
           same_sandbox(left.sandbox, right.sandbox) &&
           same_status(left.status, right.status) &&
           left.exit_code == right.exit_code &&
           left.failures == right.failures &&
           left.failure_limit == right.failure_limit;
}

group group_of_problem(const vss::error_code& problem)
{
    if (problem == plug::plugin_errc::signature_missing ||
        problem == plug::plugin_errc::key_unknown)
        return group::decision;
    if (problem == plug::plugin_errc::policy)
        return group::blocked;
    if (problem == plug::plugin_errc::not_licensed)
        return group::licence;
    return group::unusable;
}

sandbox_view sandbox_of(const host::sandbox_report& report)
{
    sandbox_view out;
    out.level = std::string(plug::to_string(report.level()));
    for (const host::sandbox_part part : report.not_applied())
        out.not_applied.emplace_back(host::to_string(part));
    if (report.failure.failed())
        out.failure = sup::code_text(report.failure);
    return out;
}

/// The plugin as the service's snapshot shows it.  `asked_to_stop` says that
/// a runner that has stopped is stopped, and not yet started.
view view_of(const host::service_plugin& entry, bool asked_to_stop)
{
    view out;
    if (entry.problem.failed())
    {
        out.state = group_of_problem(entry.problem);
        out.reason = sup::code_text(entry.problem);
        out.detail = entry.detail;
        return out;
    }
    if (!entry.enabled)
    {
        out.state = group::off;
        return out;
    }
    const std::shared_ptr<const host::runner_snapshot>& run = entry.run;
    if (run == nullptr)
    {
        out.state = group::starting;
        return out;
    }

    switch (run->state)
    {
    case plug::run_state::stopped:
        out.state = asked_to_stop ? group::stopped : group::starting;
        break;
    case plug::run_state::starting:
        out.state = group::starting;
        break;
    case plug::run_state::ready:
        out.state = group::running;
        break;
    case plug::run_state::stopping:
        out.state = group::stopping;
        break;
    case plug::run_state::backing_off:
        out.state = group::waiting;
        break;
    case plug::run_state::quarantined:
        out.state = group::failed;
        break;
    }
    if (out.state == group::waiting || out.state == group::failed)
    {
        out.reason = sup::code_text(run->last_failure);
        out.detail = run->detail;
        out.failures = run->failures;
        out.failure_limit = run->failure_limit;
    }
    if (out.state == group::waiting && run->next_start.has_value())
    {
        const auto left =
            run->next_start.value() - std::chrono::steady_clock::now();
        out.wait = std::max(
            std::chrono::duration_cast<std::chrono::milliseconds>(left),
            std::chrono::milliseconds{0});
    }
    if (run->process_id != 0)
        out.process = run->process_id;
    if (run->last_exit.has_value() && run->process_id == 0)
    {
        out.exit_code = run->last_exit.value().code;
        out.exit_abnormal = run->last_exit.value().abnormal;
    }
    if (run->process_id != 0 || !run->sandbox.applied.empty())
        out.sandbox = sandbox_of(run->sandbox);
    if (run->status.has_value())
    {
        status_view status;
        status.level = std::string(plug::to_string(run->status.value().level));
        status.text = run->status.value().text;
        status.code = run->status.value().code;
        out.status = std::move(status);
    }
    return out;
}

/// The wait as seconds and tenths, such as `1.2`.
std::string seconds_text(std::chrono::milliseconds wait)
{
    const long long tenths = (wait.count() + 50) / 100;
    std::string out = std::to_string(tenths / 10);
    out += '.';
    out += std::to_string(tenths % 10);
    return out;
}

std::string words_of(const view& now)
{
    std::string out(names_of(now.state).words);
    if (now.state == group::waiting)
    {
        if (now.wait.has_value())
        {
            out += " in ";
            out += seconds_text(now.wait.value());
            out += " s";
        }
        out += " (failure ";
        out += std::to_string(now.failures);
        out += " of ";
        out += std::to_string(now.failure_limit);
        out += ')';
    }
    return out;
}

std::string sandbox_text(const sandbox_view& sandbox)
{
    std::string out = "sandbox ";
    out += sup::plain(sandbox.level);
    if (!sandbox.not_applied.empty())
    {
        out += " (not applied: ";
        for (std::size_t index = 0; index < sandbox.not_applied.size(); ++index)
        {
            if (index != 0)
                out += ", ";
            out += sup::plain(sandbox.not_applied[index]);
        }
        out += ')';
    }
    return out;
}

/// `label` and `text`, as plain text, and a line feed.
std::string labelled(std::string_view label, std::string_view text)
{
    std::string out(label);
    out += sup::plain(text);
    out += '\n';
    return out;
}

std::string state_text(const view& now)
{
    std::string out = "state: ";
    out += words_of(now);
    if (!now.reason.empty())
    {
        out += "; reason ";
        out += sup::plain(now.reason);
    }
    if (!now.detail.empty())
    {
        out += "; detail \"";
        out += sup::plain(now.detail);
        out += '"';
    }
    if (now.process.has_value())
    {
        out += "; process ";
        out += std::to_string(now.process.value());
    }
    if (now.exit_code.has_value())
    {
        out += now.exit_abnormal ? "; ended abnormally with code "
                                 : "; ended with code ";
        out += std::to_string(now.exit_code.value());
    }
    if (now.sandbox.has_value())
    {
        out += "; ";
        out += sandbox_text(now.sandbox.value());
    }
    if (now.status.has_value())
    {
        const status_view& status = now.status.value();
        out += "; status ";
        out += sup::plain(status.level);
        out += " \"";
        out += sup::plain(status.text);
        out += '"';
        if (!status.code.empty())
        {
            out += " (code ";
            out += sup::plain(status.code);
            out += ')';
        }
    }
    out += '\n';
    return out;
}

json::value optional_text(const std::string& text)
{
    if (text.empty())
        return json::value(nullptr);
    return sup::text_value(text);
}

std::string json_line(json::object value)
{
    std::string out = json::serialize(value);
    out += '\n';
    return out;
}

std::string state_json(const std::string& id, const view& now)
{
    json::object out;
    out["type"] = "state";
    out["plugin"] = sup::text_value(id);
    out["state"] = std::string(names_of(now.state).key);
    out["reason"] = optional_text(now.reason);
    out["detail"] = optional_text(now.detail);
    if (now.process.has_value())
        out["process_id"] = now.process.value();
    else
        out["process_id"] = nullptr;
    if (now.exit_code.has_value())
    {
        out["exit_code"] = now.exit_code.value();
        out["exit_abnormal"] = now.exit_abnormal;
    }
    if (now.sandbox.has_value())
    {
        const sandbox_view& sandbox = now.sandbox.value();
        json::object object;
        object["level"] = sup::text_value(sandbox.level);
        json::array missing;
        for (const std::string& part : sandbox.not_applied)
            missing.push_back(sup::text_value(part));
        object["not_applied"] = std::move(missing);
        object["failure"] = optional_text(sandbox.failure);
        out["sandbox"] = std::move(object);
    }
    else
    {
        out["sandbox"] = nullptr;
    }
    if (now.status.has_value())
    {
        const status_view& status = now.status.value();
        json::object object;
        object["level"] = sup::text_value(status.level);
        object["text"] = sup::text_value(status.text);
        object["code"] = optional_text(status.code);
        out["status"] = std::move(object);
    }
    else
    {
        out["status"] = nullptr;
    }
    if (now.wait.has_value())
        out["wait_ms"] = static_cast<std::int64_t>(now.wait.value().count());
    else
        out["wait_ms"] = nullptr;
    out["failures"] = static_cast<std::uint64_t>(now.failures);
    out["failure_limit"] = static_cast<std::uint64_t>(now.failure_limit);
    return json_line(std::move(out));
}

std::string log_text(const host::runner_log_line& line)
{
    std::string label = line.from_output ? "output: " : "log ";
    if (!line.from_output)
    {
        label += plug::to_string(line.level);
        label += ": ";
    }
    return labelled(label, line.text);
}

std::string log_json(const std::string& id, const host::runner_log_line& line)
{
    json::object out;
    out["type"] = "log";
    out["plugin"] = sup::text_value(id);
    out["source"] = line.from_output ? "output" : "log";
    out["level"] = std::string(plug::to_string(line.level));
    out["text"] = sup::text_value(line.text);
    return json_line(std::move(out));
}

std::string note_json(const std::string& text)
{
    json::object out;
    out["type"] = "note";
    out["text"] = sup::text_value(text);
    return json_line(std::move(out));
}

void print(const std::string& text)
{
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

bool same_line(const host::runner_log_line& left,
               const host::runner_log_line& right)
{
    return left.from_output == right.from_output && left.level == right.level &&
           left.text == right.text;
}

/// The place in `now` where the lines that `before` has not shown start.  A
/// snapshot holds the latest lines, so what `before` still holds is the start
/// of `now` once the oldest of `before` have dropped off the front.  A plugin
/// that fills the whole log with one line over and over looks as if it wrote
/// nothing; the snapshot has nothing to tell them apart.
std::size_t first_new_line(const std::vector<host::runner_log_line>& before,
                           const std::vector<host::runner_log_line>& now)
{
    for (std::size_t dropped = 0; dropped < before.size(); ++dropped)
    {
        const std::size_t kept = before.size() - dropped;
        if (kept > now.size())
            continue;
        bool same = true;
        for (std::size_t index = 0; index < kept && same; ++index)
            same = same_line(before[dropped + index], now[index]);
        if (same)
            return kept;
    }
    return 0;
}

/// The plugin with this id, or null.  A copy with a level counts before one
/// that is only a name.
const host::service_plugin* find_entry(const host::service_snapshot& snapshot,
                                       const std::string& id)
{
    const host::service_plugin* first = nullptr;
    for (const host::service_plugin& plugin : snapshot.plugins)
    {
        if (plugin.id != id)
            continue;
        if (plugin.level.has_value())
            return &plugin;
        if (first == nullptr)
            first = &plugin;
    }
    return first;
}

/// What the command prints as the snapshots come, and what it concludes.
/// Used by the main thread only.
class reporter
{
public:
    reporter(const options& given, const places& where)
        : json_(given.json), id_(given.id),
          trust_file_(sup::path_text(where.trust_file))
    {
    }

    void observe(const host::service_snapshot& snapshot)
    {
        note(snapshot.notes);
        if (snapshot.scanning)
            saw_scanning_ = true;
        else if (saw_scanning_)
            scanned_ = true;
        if (!scanned_ || finished())
            return;

        const host::service_plugin* const entry = find_entry(snapshot, id_);
        if (entry == nullptr)
        {
            missing_ = true;
            return;
        }
        // After the service has shut down it holds no run for a plugin.
        if (stopping_ && entry->run == nullptr && entry->enabled &&
            !entry->problem.failed())
            return;

        const view now = view_of(*entry, stopping_);
        show(now);
        if (entry->run != nullptr)
            show_log(entry->run->log);
        if (now.state == group::running)
            reached_running_ = true;
        if (cannot_go_on(now.state))
        {
            ended_ = true;
            final_ = now;
        }
    }

    /// The command has asked the service to stop.
    void asked_to_stop() noexcept { stopping_ = true; }

    /// The service has shut down: whatever ran has gone.
    void closed()
    {
        if (finished() || !last_.has_value())
            return;
        if (last_.value().state == group::stopped)
            return;
        view gone;
        gone.state = group::stopped;
        show(gone);
    }

    /// There is no reason to wait: the plugin cannot run, or is not there.
    bool finished() const noexcept { return ended_ || missing_; }

    bool reached_running() const noexcept { return reached_running_; }

    /// Says on the error stream how it ended, when it did not end well, and
    /// gives the exit code.
    int conclude() const
    {
        if (missing_)
        {
            std::string text = "plugin run: no plugin with the id ";
            text += sup::plain(id_);
            text += " is installed in the plugin folders\n";
            std::fputs(text.c_str(), stderr);
            return exit_usage;
        }
        if (ended_)
        {
            std::fputs(ended_message().c_str(), stderr);
            return exit_failed;
        }
        if (!reached_running_)
        {
            std::string text = "plugin run: ";
            text += sup::plain(id_);
            text += " did not reach running before it was stopped\n";
            std::fputs(text.c_str(), stderr);
            return exit_failed;
        }
        return exit_ok;
    }

private:
    void emit(const std::string& text) const { print(text); }

    void note(const std::vector<std::string>& notes)
    {
        for (const std::string& text : notes)
        {
            if (std::ranges::find(noted_, text) != noted_.end())
                continue;
            noted_.push_back(text);
            emit(json_ ? note_json(text) : labelled("note: ", text));
        }
    }

    void show(const view& now)
    {
        if (!last_.has_value() || !same_state(last_.value(), now))
            emit(json_ ? state_json(id_, now) : state_text(now));
        last_ = now;
    }

    void show_log(const std::vector<host::runner_log_line>& log)
    {
        for (std::size_t index = first_new_line(shown_log_, log);
             index < log.size(); ++index)
            emit(json_ ? log_json(id_, log[index]) : log_text(log[index]));
        shown_log_ = log;
    }

    std::string ended_message() const
    {
        const view& end = final_;
        std::string text = "plugin run: ";
        text += sup::plain(id_);
        switch (end.state)
        {
        case group::decision:
            text += " needs your decision and this command makes none: "
                    "decide in Studio.  The trust file ";
            text += sup::plain(trust_file_);
            text += " holds no decision for this copy of the plugin";
            break;
        case group::off:
            text += " is switched off in the trust file ";
            text += sup::plain(trust_file_);
            break;
        case group::blocked:
            text += " is blocked by the administrator";
            break;
        case group::licence:
            text += " needs a licence, and this command reads no licence "
                    "files";
            break;
        case group::failed:
            text += " failed too often and was stopped";
            break;
        case group::starting:
        case group::running:
        case group::stopping:
        case group::stopped:
        case group::waiting:
        case group::unusable:
            text += " cannot be used";
            break;
        }
        if (!end.reason.empty())
        {
            text += ": ";
            text += sup::plain(end.reason);
        }
        if (!end.detail.empty())
        {
            text += ": ";
            text += sup::plain(end.detail);
        }
        text += '\n';
        return text;
    }

    bool json_ = false;
    std::string id_;
    std::string trust_file_;

    bool saw_scanning_ = false;
    bool scanned_ = false;
    bool missing_ = false;
    bool ended_ = false;
    bool stopping_ = false;
    bool reached_running_ = false;
    view final_;
    std::optional<view> last_;
    std::vector<host::runner_log_line> shown_log_;
    std::vector<std::string> noted_;
};

// -- the threads ------------------------------------------------------------

std::atomic<bool> g_stop_requested{false};

/// Called from a signal, or from a thread Windows starts: only sets a flag.
void request_stop(int)
{
    g_stop_requested.store(true);
}

/// The threads the service runs on: an `io_context` run by one thread, and a
/// pool of one for the slow work.  The destructor lets both run dry, so the
/// service must have shut down and be gone first.
class host_threads
{
public:
    host_threads()
        : guard_(boost::asio::make_work_guard(context_)),
          finished_future_(finished_.get_future()),
          thread_([this] { run_context(); })
    {
    }

    ~host_threads()
    {
        try
        {
            slow_.join();
            guard_.reset();
            if (finished_future_.wait_for(finish_limit) !=
                std::future_status::ready)
                context_.stop();
            if (thread_.joinable())
                thread_.join();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // A destructor does not throw, and nothing can be done about it.
        }
    }

    host_threads(const host_threads&) = delete;
    host_threads& operator=(const host_threads&) = delete;

    boost::asio::any_io_executor context_executor()
    {
        return context_.get_executor();
    }

    boost::asio::any_io_executor slow_executor()
    {
        return slow_.get_executor();
    }

private:
    void run_context()
    {
        try
        {
            context_.run();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // A handler that throws ends the run; the command then ends.
        }
        finished_.set_value();
    }

    boost::asio::io_context context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        guard_;
    boost::asio::thread_pool slow_{1};
    std::promise<void> finished_;
    std::future<void> finished_future_;
    std::thread thread_;
};

/// What the service's strand hands to the main thread.
struct shared_state
{
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<const host::service_snapshot>> snapshots;
    bool shut_down = false;
};

/// The snapshots that came since the last call, in order, and whether the
/// service has shut down.  Waits until there is one or the other, or for the
/// poll interval.
struct arrivals
{
    std::vector<std::shared_ptr<const host::service_snapshot>> snapshots;
    bool shut_down = false;
};

arrivals wait_for_arrivals(shared_state& state)
{
    arrivals out;
    std::unique_lock lock(state.mutex);
    state.changed.wait_for(
        lock, poll_interval,
        [&state] { return !state.snapshots.empty() || state.shut_down; });
    out.snapshots.assign(state.snapshots.begin(), state.snapshots.end());
    state.snapshots.clear();
    out.shut_down = state.shut_down;
    return out;
}

int run_plugin(const options& given, const places& where)
{
    // Before anything is printed, so that a signal that comes at once is
    // not the end of the process with the plugin still running.
    g_stop_requested.store(false);
    platform::catch_stop_signals(request_stop);

    host_threads threads;
    host::service_options setup;
    setup.roots = where.roots;
    setup.computer.program_directory = where.programs;
    setup.computer.policy_file = where.policy_file;
    setup.computer.trust_file = where.trust_file;
    setup.plan.data_root = where.data_root;
    // No capabilities, no licence and no hook for tests: what runs is what
    // this computer would run.

    const auto state = std::make_shared<shared_state>();
    const auto service = host::plugin_service::create(
        threads.context_executor(), threads.slow_executor(), std::move(setup));
    service->on_change(
        [weak = std::weak_ptr<host::plugin_service>(service), state]()
        {
            const auto locked = weak.lock();
            if (!locked)
                return;
            auto latest = locked->snapshot();
            const std::lock_guard lock(state->mutex);
            state->snapshots.push_back(std::move(latest));
            state->changed.notify_all();
        });
    service->rescan();

    reporter report(given, where);
    bool shutting_down = false;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    for (;;)
    {
        const arrivals arrived = wait_for_arrivals(*state);
        for (const auto& snapshot : arrived.snapshots)
            report.observe(*snapshot);
        if (arrived.shut_down)
            break;

        const auto now = std::chrono::steady_clock::now();
        if (!deadline.has_value() && given.seconds > 0 &&
            report.reached_running())
            deadline = now + std::chrono::seconds(given.seconds);
        const bool time_is_up = deadline.has_value() && now >= deadline.value();
        if (shutting_down ||
            !(g_stop_requested.load() || report.finished() || time_is_up))
            continue;
        shutting_down = true;
        report.asked_to_stop();
        service->shut_down(
            [state]
            {
                const std::lock_guard lock(state->mutex);
                state->shut_down = true;
                state->changed.notify_all();
            });
    }
    report.closed();
    return report.conclude();
}

} // namespace

const char* plugin_run_usage() noexcept
{
    return "  vss_cli plugin run <id> [--root <folder>]... [--data <folder>]\n"
           "                      [--trust <file>] [--seconds <n>] [--json]\n"
           "      Run one installed plugin under the plugin host and print "
           "each\n"
           "      change of its state: starting, running, waiting to start "
           "again,\n"
           "      stopped after failing, off, needs your decision, blocked by "
           "the\n"
           "      administrator, needs a licence, or cannot be used and why.  "
           "Each\n"
           "      line also has the process id, how far the plugin is "
           "confined and\n"
           "      which parts of the sandbox were not applied, and its status. "
           " "
           "Each\n"
           "      line of its output and log is printed as it comes.  Text "
           "from the\n"
           "      plugin is shown as plain text.  --json prints one JSON "
           "object on\n"
           "      each line instead.\n"
           "      The plugin runs until you press Ctrl-C or it cannot run, "
           "or, with\n"
           "      --seconds, for that long once it is running.  It is then "
           "stopped\n"
           "      in order.  Each --root replaces the usual plugin folders "
           "with the\n"
           "      folders given; the policy file is then not read.  --trust "
           "names\n"
           "      the file that holds your decisions about plugins; the "
           "default is\n"
           "      trust.json in your Virtual Serial Studio folder, the one "
           "that\n"
           "      holds your plugin folder.  The command never changes it: a "
           "plugin\n"
           "      that needs your decision is not run, so decide in Studio.  "
           "--data\n"
           "      names the folder where plugins keep their data; the default "
           "is\n"
           "      plugin-data in your Virtual Serial Studio folder.  No "
           "licence is\n"
           "      read, so a plugin that needs one does not run.\n"
           "      Exit code 0 means the plugin was running and was stopped, "
           "1 that\n"
           "      it never ran or failed too often, and 2 that the command "
           "line is\n"
           "      wrong or no plugin has the id.\n"
           "\n";
}

int command_plugin_run(const std::vector<std::string>& arguments)
{
    options given;
    if (!parse(arguments, given))
    {
        std::fputs(plugin_run_usage(), stderr);
        return exit_usage;
    }
    const std::optional<places> where = places_for(given);
    if (!where.has_value())
        return exit_usage;
    return run_plugin(given, where.value());
}

} // namespace vss_cli
