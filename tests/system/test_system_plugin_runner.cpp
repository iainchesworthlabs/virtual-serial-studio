//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss::plugin::host::plugin_runner` keeping a real program running: the test
// plugin (VSS_TEST_PLUGIN_PROGRAM) in its serving mode, which behaves as a
// plugin or breaks one rule of the conversation as its arguments say.  The
// runner runs on an io_context of its own with one thread; a test starts it
// and then waits, with a limit, for a condition on its snapshots.  Nothing
// here sleeps for a fixed time to let something happen.
//

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/host/process.hpp>
#include <vss/plugin/host/runner.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/lifecycle.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/protocol.hpp>
#include <vss/plugin/session.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include "native_io.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"
#include "test_plugin_files.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
#include <variant>
#include <vector>

using vss::error_code;
using vss::plugin::api_errc;
using vss::plugin::api_message;
using vss::plugin::api_range;
using vss::plugin::api_version;
using vss::plugin::error_message;
using vss::plugin::log_level;
using vss::plugin::ok_message;
using vss::plugin::other_message;
using vss::plugin::permission;
using vss::plugin::plugin_errc;
using vss::plugin::run_state;
using vss::plugin::sandbox_level;
using vss::plugin::session_options;
using vss::plugin::status_level;
using vss::plugin::host::available_sandbox;
using vss::plugin::host::plugin_environment;
using vss::plugin::host::plugin_runner;
using vss::plugin::host::process_request;
using vss::plugin::host::runner_log_line;
using vss::plugin::host::runner_options;
using vss::plugin::host::runner_snapshot;
using vss::plugin::host::sandbox_profile;
using vss_test_support::copy_test_plugin;
using vss_test_support::scratch_directory;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;
namespace io = vss_test_support;

using steady = std::chrono::steady_clock;
using snapshot_ptr = std::shared_ptr<const runner_snapshot>;

/// How long any wait lasts before the test gives up.
constexpr std::chrono::milliseconds long_limit{20000};

constexpr std::string_view plugin_id = "com.example.test";
constexpr std::string_view plugin_version = "1.2.0";

// -- what a snapshot says -----------------------------------------------------

std::string_view state_name(run_state state)
{
    switch (state)
    {
    case run_state::stopped:
        return "stopped";
    case run_state::starting:
        return "starting";
    case run_state::ready:
        return "ready";
    case run_state::stopping:
        return "stopping";
    case run_state::backing_off:
        return "backing_off";
    case run_state::quarantined:
        return "quarantined";
    }
    return "unknown";
}

std::string describe(const runner_snapshot& snapshot)
{
    std::string text = "state ";
    text.append(state_name(snapshot.state));
    text.append(", failures ");
    text.append(std::to_string(snapshot.failures));
    text.append(", last failure ");
    if (snapshot.last_failure)
        text.append(snapshot.last_failure.message());
    else
        text.append("none");
    text.append(", process ");
    text.append(std::to_string(snapshot.process_id));
    text.append(", detail [");
    text.append(snapshot.detail);
    text.push_back(']');
    return text;
}

bool is_code(const error_code& code, plugin_errc expected)
{
    return code == make_error_code(expected);
}

bool is_code(const error_code& code, api_errc expected)
{
    return code == make_error_code(expected);
}

/// The first log line that comes from standard output and error, or from
/// "log" messages, and says exactly `text`.
const runner_log_line* find_line(const runner_snapshot& snapshot,
                                 bool from_output, std::string_view text)
{
    for (const runner_log_line& line : snapshot.log)
    {
        if (line.from_output == from_output && line.text == text)
            return &line;
    }
    return nullptr;
}

auto in_state(run_state wanted)
{
    return [wanted](const runner_snapshot& snapshot)
    { return snapshot.state == wanted; };
}

/// Waits until `predicate(snapshot)` holds for the latest snapshot, polling
/// every 10 ms, and returns that snapshot.  Fails the test, saying what was
/// waited for and what the last snapshot was, when the limit passes first.
template <class Predicate>
snapshot_ptr wait_for(const std::shared_ptr<plugin_runner>& runner,
                      std::string_view what, Predicate predicate,
                      std::chrono::milliseconds limit = long_limit)
{
    const auto deadline = steady::now() + limit;
    for (;;)
    {
        snapshot_ptr snapshot = runner->snapshot();
        REQUIRE(snapshot != nullptr);
        if (predicate(*snapshot))
            return snapshot;
        if (steady::now() >= deadline)
        {
            std::string text = "gave up waiting for ";
            text.append(what);
            text.append(": ");
            text.append(describe(*snapshot));
            throw vss_test::failure(text);
        }
        std::this_thread::sleep_for(10ms);
    }
}

/// Waits until no process has the number, up to the long limit.
bool wait_until_gone(std::uint64_t pid)
{
    const auto deadline = steady::now() + long_limit;
    while (io::process_exists(pid))
    {
        if (steady::now() >= deadline)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

void require_failure(const runner_snapshot& snapshot, plugin_errc expected)
{
    if (is_code(snapshot.last_failure, expected))
        return;
    std::string text = "the last failure should be ";
    text.append(to_string(expected));
    text.append(": ");
    text.append(describe(snapshot));
    throw vss_test::failure(text);
}

// -- the runner and what it runs on -------------------------------------------

/// The options for the test plugin with `arguments`, in `scratch` and with
/// the environment of a plugin.  There is no sandbox profile and none is
/// required.  The times are short so that a test is quick: a plugin that
/// fails is started again after 50 ms, and the third failure quarantines it.
runner_options runner_options_for(const scratch_directory& scratch,
                                  std::vector<std::string> arguments)
{
    runner_options options;
    options.process.program = fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM));
    options.process.arguments = std::move(arguments);
    options.process.working_directory = scratch.path();
    options.process.environment = plugin_environment(plugin_id, scratch.path());
    options.process.least = sandbox_level::none;

    session_options& session = options.session;
    session.plugin_id = std::string(plugin_id);
    session.version = std::string(plugin_version);
    session.accepted = api_range{api_version{1, 0}, 1, std::nullopt};
    session.ack.granted = {permission::ports_read};
    session.ack.capabilities = {"engine_rules"};
    session.ack.data_directory = scratch.path().generic_string();
    session.hello_timeout = 1500ms;
    session.ping_after = 300ms;
    session.pong_timeout = 300ms;
    session.missed_pongs = 2;
    session.shutdown_timeout = 800ms;
    session.request_timeout = 1000ms;

    options.restart.waits = {50ms, 50ms};
    options.restart.jitter_percent = 0;
    options.end_grace = 300ms;
    return options;
}

/// A scratch directory, an io_context run by one thread, and the runner
/// that a test makes on it.  The destructor ends the plugin at once, lets
/// the io_context finish and joins the thread.
class runner_fixture
{
public:
    runner_fixture()
        : guard_(boost::asio::make_work_guard(context_)),
          finished_future_(finished_.get_future()),
          thread_([this] { run_context(); })
    {
    }

    ~runner_fixture()
    {
        try
        {
            if (runner_)
                runner_->abandon();
            guard_.reset();
            if (finished_future_.wait_for(10s) != std::future_status::ready)
                context_.stop();
            thread_.join();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // Nothing can be done about it here, and a destructor does not
            // throw.
        }
    }

    runner_fixture(const runner_fixture&) = delete;
    runner_fixture& operator=(const runner_fixture&) = delete;

    const scratch_directory& scratch() const noexcept { return scratch_; }

    runner_options options_for(std::vector<std::string> arguments) const
    {
        return runner_options_for(scratch_, std::move(arguments));
    }

    std::shared_ptr<plugin_runner> make_runner_with(runner_options options)
    {
        runner_ =
            plugin_runner::create(context_.get_executor(), std::move(options));
        return runner_;
    }

    std::shared_ptr<plugin_runner>
    make_runner(std::vector<std::string> arguments)
    {
        return make_runner_with(options_for(std::move(arguments)));
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
            // A handler that throws ends the run; the test then times out.
        }
        finished_.set_value();
    }

    scratch_directory scratch_;
    boost::asio::io_context context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        guard_;
    std::promise<void> finished_;
    std::future<void> finished_future_;
    std::shared_ptr<plugin_runner> runner_;
    std::thread thread_;
};

/// Starts the runner and waits until the plugin is ready.
snapshot_ptr start_until_ready(const std::shared_ptr<plugin_runner>& runner)
{
    runner->start();
    return wait_for(runner, "the plugin to be ready",
                    in_state(run_state::ready));
}

/// Stops the runner and waits until it has stopped.
snapshot_ptr stop_until_stopped(const std::shared_ptr<plugin_runner>& runner)
{
    runner->stop("test");
    return wait_for(runner, "the plugin to stop", in_state(run_state::stopped));
}

/// A plugin that is started with `arguments` and fails the way `expected`
/// says.  It is waited for the first time the snapshot says so.
snapshot_ptr expect_failure(std::vector<std::string> arguments,
                            plugin_errc expected)
{
    runner_fixture fixture;
    const auto runner = fixture.make_runner(std::move(arguments));
    runner->start();
    const snapshot_ptr snapshot = wait_for(
        runner, to_string(expected), [expected](const runner_snapshot& latest)
        { return is_code(latest.last_failure, expected); });
    REQUIRE(snapshot->failures >= std::size_t{1});
    REQUIRE(!snapshot->detail.empty());
    return snapshot;
}

/// What a request of the test is answered with, for the test's thread.
class answer_waiter
{
public:
    plugin_runner::answer_handler handler() const
    {
        return [shared = shared_](error_code code, const api_message& answer)
        {
            const std::lock_guard lock(shared->mutex);
            if (shared->calls == 0)
            {
                shared->code = code;
                shared->answer = answer;
            }
            ++shared->calls;
            shared->changed.notify_all();
        };
    }

    /// False when no answer came within `limit`.
    bool wait(std::chrono::milliseconds limit) const
    {
        std::unique_lock lock(shared_->mutex);
        return shared_->changed.wait_for(lock, limit,
                                         [this] { return shared_->calls > 0; });
    }

    std::size_t calls() const
    {
        const std::lock_guard lock(shared_->mutex);
        return shared_->calls;
    }

    error_code code() const
    {
        const std::lock_guard lock(shared_->mutex);
        return shared_->code;
    }

    api_message answer() const
    {
        const std::lock_guard lock(shared_->mutex);
        return shared_->answer;
    }

private:
    struct shared_state
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::size_t calls = 0;
        error_code code;
        api_message answer;
    };

    std::shared_ptr<shared_state> shared_ = std::make_shared<shared_state>();
};

/// A request of an extension point, which the runner sends as it is: an "op"
/// that the API does not know and an empty body.
api_message request_with_op(std::string_view op)
{
    std::string header = R"({"op":")";
    header.append(op);
    header.append(R"("})");
    api_message message;
    message.op = std::string(op);
    message.body = other_message{std::move(header)};
    return message;
}

/// The messages of the plugin that reached the handler of `on_message`, as
/// the test's thread sees them.
class seen_messages
{
public:
    void add(std::string op, std::uint64_t id)
    {
        const std::lock_guard lock(mutex_);
        seen_.emplace_back(std::move(op), id);
    }

    /// The id of the first message with the op, when one came.
    std::optional<std::uint64_t> id_of(std::string_view op) const
    {
        const std::lock_guard lock(mutex_);
        for (const auto& entry : seen_)
        {
            if (entry.first == op)
                return entry.second;
        }
        return std::nullopt;
    }

    std::size_t count() const
    {
        const std::lock_guard lock(mutex_);
        return seen_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, std::uint64_t>> seen_;
};

bool same_log(const std::vector<runner_log_line>& left,
              const std::vector<runner_log_line>& right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (left[index].from_output != right[index].from_output ||
            left[index].level != right[index].level ||
            left[index].text != right[index].text)
            return false;
    }
    return true;
}

bool same_snapshot(const runner_snapshot& left, const runner_snapshot& right)
{
    return left.state == right.state &&
           left.last_failure == right.last_failure &&
           left.detail == right.detail && left.failures == right.failures &&
           left.failure_limit == right.failure_limit &&
           left.next_start == right.next_start &&
           left.process_id == right.process_id &&
           left.sandbox.applied == right.sandbox.applied &&
           left.sandbox.failure == right.sandbox.failure &&
           left.api == right.api && left.status == right.status &&
           same_log(left.log, right.log) &&
           left.output_bytes_dropped == right.output_bytes_dropped &&
           left.revision == right.revision;
}

} // namespace

// -- a plugin that behaves ----------------------------------------------------

TEST_CASE("plugin runner: a plugin that behaves becomes ready and then stops")
{
    runner_fixture fixture;
    const auto runner = fixture.make_runner({"--serve"});

    const snapshot_ptr before = runner->snapshot();
    REQUIRE(before != nullptr);
    REQUIRE(before->state == run_state::stopped);
    REQUIRE_EQ(before->process_id, std::uint64_t{0});

    const snapshot_ptr ready = start_until_ready(runner);
    REQUIRE(ready->process_id != 0);
    REQUIRE(ready->api == vss::plugin::plugin_api_version);
    REQUIRE_EQ(ready->failures, std::size_t{0});
    REQUIRE(!ready->last_failure);
    const std::uint64_t pid = ready->process_id;
    REQUIRE(io::process_exists(pid));

    const snapshot_ptr stopped = stop_until_stopped(runner);
    REQUIRE(!stopped->last_failure);
    REQUIRE_EQ(stopped->process_id, std::uint64_t{0});
    REQUIRE_EQ(stopped->failures, std::size_t{0});
    REQUIRE(wait_until_gone(pid));
}

TEST_CASE("plugin runner: its status, a log message and its error output are "
          "in the snapshot")
{
    runner_fixture fixture;
    const auto runner =
        fixture.make_runner({"--status=careful", "--log=a-log-line",
                             "--stderr-each=a-line-of-error", "--serve"});
    runner->start();

    const snapshot_ptr snapshot = wait_for(
        runner, "the status and both kinds of line",
        [](const runner_snapshot& latest)
        {
            return latest.status.has_value() &&
                   find_line(latest, false, "a-log-line") != nullptr &&
                   find_line(latest, true, "a-line-of-error") != nullptr;
        });

    REQUIRE(snapshot->status.has_value());
    REQUIRE(snapshot->status->level == status_level::warning);
    REQUIRE_EQ(snapshot->status->text, "careful");

    const runner_log_line* const message =
        find_line(*snapshot, false, "a-log-line");
    REQUIRE(message != nullptr);
    REQUIRE(message->level == log_level::info);
    REQUIRE(!message->from_output);

    const runner_log_line* const output =
        find_line(*snapshot, true, "a-line-of-error");
    REQUIRE(output != nullptr);
    REQUIRE(output->from_output);

    // What the plugin wrote is in one kind of line only.
    REQUIRE(find_line(*snapshot, true, "a-log-line") == nullptr);
    REQUIRE(find_line(*snapshot, false, "a-line-of-error") == nullptr);
    REQUIRE(snapshot->log.size() <= runner_options{}.kept_log_lines);
}

// -- plugins that fail --------------------------------------------------------

TEST_CASE("plugin runner: a plugin that never says hello is quarantined, and "
          "started afresh when asked")
{
    runner_fixture fixture;
    runner_options options = fixture.options_for({"--no-hello", "--serve"});
    options.session.hello_timeout = 400ms;
    const auto runner = fixture.make_runner_with(std::move(options));
    runner->start();

    const auto quarantined = [](const runner_snapshot& latest)
    { return latest.state == run_state::quarantined && latest.failures == 3; };

    const snapshot_ptr first =
        wait_for(runner, "the plugin to be quarantined", quarantined);
    require_failure(*first, plugin_errc::no_hello);
    REQUIRE_EQ(first->failure_limit, std::size_t{3});
    REQUIRE_EQ(first->process_id, std::uint64_t{0});

    // Starting a quarantined plugin counts no failures to begin with.
    runner->start();
    const snapshot_ptr again =
        wait_for(runner, "the plugin to leave quarantine",
                 [](const runner_snapshot& latest)
                 { return latest.state != run_state::quarantined; });
    REQUIRE(again->failures < again->failure_limit);

    const snapshot_ptr last =
        wait_for(runner, "the plugin to be quarantined again", quarantined);
    require_failure(*last, plugin_errc::no_hello);
    REQUIRE(last->revision > first->revision);
}

TEST_CASE("plugin runner: a hello that is not the manifest's fails")
{
    static_cast<void>(expect_failure(
        {"--hello-id=com.example.other", "--serve"}, plugin_errc::bad_hello));
    static_cast<void>(expect_failure({"--hello-version=9.9.9", "--serve"},
                                     plugin_errc::bad_hello));
}

TEST_CASE("plugin runner: a plugin that exits after it is ready is started "
          "again")
{
    runner_fixture fixture;
    const auto runner =
        fixture.make_runner({"--exit-after-ready=0", "--serve"});
    runner->start();

    const snapshot_ptr snapshot =
        wait_for(runner, "the plugin to be started again",
                 [](const runner_snapshot& latest)
                 {
                     const bool again =
                         latest.state == run_state::backing_off ||
                         latest.state == run_state::starting ||
                         latest.state == run_state::quarantined;
                     return again && latest.failures >= 1 &&
                            is_code(latest.last_failure, plugin_errc::exited);
                 });
    require_failure(*snapshot, plugin_errc::exited);
    if (snapshot->state == run_state::backing_off)
        REQUIRE(snapshot->next_start.has_value());
    else
        REQUIRE(!snapshot->next_start.has_value());
}

TEST_CASE("plugin runner: a plugin that crashes after it is ready is reported "
          "as crashed")
{
    runner_fixture fixture;
    const auto runner = fixture.make_runner({"--crash-after-ready", "--serve"});
    runner->start();

    // The first failure of any kind is waited for, so that the one that is
    // reported is the one that is judged.
    const snapshot_ptr snapshot =
        wait_for(runner, "the plugin to fail", [](const runner_snapshot& latest)
                 { return static_cast<bool>(latest.last_failure); });
    require_failure(*snapshot, plugin_errc::crashed);
    REQUIRE(snapshot->failures >= std::size_t{1});
}

TEST_CASE("plugin runner: a plugin that stops answering ping fails")
{
    static_cast<void>(expect_failure({"--ignore-ping", "--serve"},
                                     plugin_errc::unresponsive));
}

TEST_CASE("plugin runner: bytes that are not a frame fail as misbehaving")
{
    // The session closes a plugin that is ready, and sends bytes that are
    // not a frame, as `misbehaving`.
    static_cast<void>(expect_failure({"--garbage-after-ready", "--serve"},
                                     plugin_errc::misbehaving));

    // So does a second hello.
    static_cast<void>(
        expect_failure({"--hello-twice", "--serve"}, plugin_errc::misbehaving));
}

// -- ending a plugin ----------------------------------------------------------

TEST_CASE("plugin runner: a plugin that ignores shutdown is ended anyway")
{
    runner_fixture fixture;
    const auto runner = fixture.make_runner({"--ignore-shutdown", "--serve"});
    const snapshot_ptr ready = start_until_ready(runner);
    const std::uint64_t pid = ready->process_id;
    REQUIRE(pid != 0);

    // The session reports the missing "bye" as a failure while it stops;
    // what counts here is that the runner ends up stopped and the process
    // is gone.
    const snapshot_ptr stopped = stop_until_stopped(runner);
    REQUIRE_EQ(stopped->process_id, std::uint64_t{0});
    REQUIRE(wait_until_gone(pid));
}

TEST_CASE("plugin runner: abandon ends a running plugin at once")
{
    runner_fixture fixture;
    const auto runner = fixture.make_runner({"--serve"});
    const snapshot_ptr ready = start_until_ready(runner);
    const std::uint64_t pid = ready->process_id;
    REQUIRE(pid != 0);
    REQUIRE(io::process_exists(pid));

    runner->abandon();
    const snapshot_ptr stopped = wait_for(
        runner, "the runner to stop",
        [](const runner_snapshot& latest)
        {
            return latest.state == run_state::stopped && latest.process_id == 0;
        });
    REQUIRE(stopped->state == run_state::stopped);
    REQUIRE(wait_until_gone(pid));
}

// -- requests and messages ----------------------------------------------------

TEST_CASE("plugin runner: a request is answered, and one that cannot be is "
          "refused")
{
    runner_fixture fixture;
    const auto runner = fixture.make_runner({"--echo-op=test.echo", "--serve"});

    // Nothing runs yet: the request is not made.
    {
        const answer_waiter unavailable;
        runner->request(request_with_op("test.echo"), unavailable.handler());
        REQUIRE(unavailable.wait(long_limit));
        REQUIRE_EQ(unavailable.calls(), std::size_t{1});
        REQUIRE(is_code(unavailable.code(), api_errc::plugin_unavailable));
        REQUIRE(unavailable.answer().op.empty());
    }

    static_cast<void>(start_until_ready(runner));

    // An op the plugin echoes is answered with "ok".
    const answer_waiter echoed;
    runner->request(request_with_op("test.echo"), echoed.handler());
    REQUIRE(echoed.wait(long_limit));
    REQUIRE(!echoed.code());
    const api_message echo = echoed.answer();
    REQUIRE_EQ(echo.op, "ok");
    REQUIRE(echo.re != 0);
    REQUIRE(std::holds_alternative<ok_message>(echo.body));

    // An op it does not know is answered with its own "error"; the request
    // itself did not fail.
    const answer_waiter unknown;
    runner->request(request_with_op("test.other"), unknown.handler());
    REQUIRE(unknown.wait(long_limit));
    REQUIRE(!unknown.code());
    const api_message refusal = unknown.answer();
    REQUIRE_EQ(refusal.op, "error");
    REQUIRE(refusal.re != 0);
    REQUIRE(refusal.re != echo.re);
    const auto* const refusal_body = std::get_if<error_message>(&refusal.body);
    REQUIRE(refusal_body != nullptr);
    REQUIRE_EQ(refusal_body->category, "vss.plugin.api");
    REQUIRE_EQ(refusal_body->value, "unknown_op");

    // Once the plugin has stopped there is nobody to ask.
    static_cast<void>(stop_until_stopped(runner));
    const answer_waiter after;
    runner->request(request_with_op("test.echo"), after.handler());
    REQUIRE(after.wait(long_limit));
    REQUIRE(is_code(after.code(), api_errc::plugin_unavailable));
    REQUIRE(after.answer().op.empty());

    // Each handler was called once, and none was called again when the
    // plugin stopped.
    REQUIRE_EQ(echoed.calls(), std::size_t{1});
    REQUIRE_EQ(unknown.calls(), std::size_t{1});
    REQUIRE_EQ(after.calls(), std::size_t{1});
}

TEST_CASE("plugin runner: a message of the plugin reaches the handler and is "
          "answered")
{
    runner_fixture fixture;
    const auto runner =
        fixture.make_runner({"--send-op=test.hello", "--serve"});

    const auto seen = std::make_shared<seen_messages>();
    // The runner is held by the fixture, so a plain pointer to it is safe
    // for as long as the handler can run.
    plugin_runner* const raw = runner.get();
    runner->on_message(
        [seen, raw](const api_message& message)
        {
            seen->add(message.op, message.id);
            if (message.id == 0)
                return;
            api_message answer;
            answer.op = "ok";
            answer.body = ok_message{};
            raw->answer(message.id, std::move(answer));
        });
    runner->start();

    const snapshot_ptr snapshot =
        wait_for(runner, "the plugin to print the answer",
                 [](const runner_snapshot& latest)
                 { return find_line(latest, true, "answer ok") != nullptr; });
    REQUIRE(snapshot->state == run_state::ready);
    const std::optional<std::uint64_t> id = seen->id_of("test.hello");
    REQUIRE(id.has_value());
    REQUIRE_EQ(*id, std::uint64_t{2});
    REQUIRE_EQ(seen->count(), std::size_t{1});
}

TEST_CASE("plugin runner: a message nobody handles is answered with "
          "unknown_op")
{
    runner_fixture fixture;
    const auto runner =
        fixture.make_runner({"--send-op=test.hello", "--serve"});
    runner->start();

    const snapshot_ptr snapshot = wait_for(
        runner, "the plugin to print the refusal",
        [](const runner_snapshot& latest)
        {
            return find_line(latest, true, "answer error") != nullptr &&
                   find_line(latest, true, "answer-value unknown_op") !=
                       nullptr;
        });
    // A plugin that is refused once is not ended for it.
    REQUIRE(snapshot->state == run_state::ready);
}

// -- limits ------------------------------------------------------------

TEST_CASE("plugin runner: a flood of log messages is bounded and the runner "
          "stays responsive")
{
    constexpr std::size_t kept = 50;
    runner_fixture fixture;
    runner_options options =
        fixture.options_for({"--flood-after-ready=20000", "--serve"});
    options.kept_log_lines = kept;
    const auto runner = fixture.make_runner_with(std::move(options));
    runner->start();

    // The session lets some of the messages through, so the log fills.  It
    // never holds more than it is allowed to, however many were sent.
    std::size_t most = 0;
    const snapshot_ptr filled =
        wait_for(runner, "the log to fill",
                 [&most](const runner_snapshot& latest)
                 {
                     most = std::max(most, latest.log.size());
                     return latest.log.size() >= kept;
                 });
    REQUIRE(filled->log.size() <= kept);

    // The session may have ended the plugin for the flood as `misbehaving`,
    // or for not answering a "ping" while it was held up writing, or not
    // yet.  Either way the runner still stops it.
    if (filled->last_failure)
    {
        REQUIRE(is_code(filled->last_failure, plugin_errc::misbehaving) ||
                is_code(filled->last_failure, plugin_errc::unresponsive));
    }
    runner->stop("test");
    const snapshot_ptr stopped =
        wait_for(runner, "the plugin to stop",
                 [&most](const runner_snapshot& latest)
                 {
                     most = std::max(most, latest.log.size());
                     return latest.state == run_state::stopped;
                 });
    REQUIRE(most <= kept);
    REQUIRE(stopped->log.size() <= kept);
    REQUIRE_EQ(stopped->process_id, std::uint64_t{0});
}

// -- snapshots ------------------------------------------------------------

TEST_CASE("plugin runner: the revision goes up and a snapshot never changes")
{
    runner_fixture fixture;
    const auto runner =
        fixture.make_runner({"--status=one", "--log=two", "--serve"});

    const snapshot_ptr first = runner->snapshot();
    REQUIRE(first != nullptr);
    // A copy on purpose: it is compared with the snapshot later.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    runner_snapshot first_copy = *first;

    std::uint64_t latest_revision = first->revision;
    const auto never_down = [&latest_revision](const runner_snapshot& latest)
    {
        REQUIRE(latest.revision >= latest_revision);
        latest_revision = latest.revision;
        return true;
    };
    static_cast<void>(never_down(*first));

    runner->start();
    const snapshot_ptr middle =
        wait_for(runner, "the status and the log line",
                 [&never_down](const runner_snapshot& latest)
                 {
                     return never_down(latest) &&
                            latest.state == run_state::ready &&
                            latest.status.has_value() &&
                            find_line(latest, false, "two") != nullptr;
                 });
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    runner_snapshot middle_copy = *middle;
    REQUIRE(middle->revision > first->revision);

    runner->stop("test");
    const snapshot_ptr last = wait_for(
        runner, "the plugin to stop",
        [&never_down](const runner_snapshot& latest)
        { return never_down(latest) && latest.state == run_state::stopped; });
    REQUIRE(last->revision > middle->revision);

    // The snapshots taken earlier say what they said then.
    REQUIRE(same_snapshot(*first, first_copy));
    REQUIRE(same_snapshot(*middle, middle_copy));
    REQUIRE(first->state == run_state::stopped);
    REQUIRE(first->log.empty());
    REQUIRE(!first->status.has_value());
    REQUIRE(middle->state == run_state::ready);
    REQUIRE(middle->process_id != 0);
}

// -- a sandbox ------------------------------------------------------------

namespace
{

/// A request for the test plugin as an installed plugin is laid out: the
/// program copied into a directory of its own, a data directory beside it,
/// and a profile that names the two.
process_request confined_request(const scratch_directory& scratch,
                                 std::vector<std::string> arguments)
{
    sandbox_profile profile;
    profile.name = std::string(plugin_id);
    profile.plugin_directory = scratch.path() / "plugin";
    profile.data_directory = scratch.path() / "data";

    std::error_code ec;
    fs::create_directories(profile.plugin_directory, ec);
    fs::create_directories(profile.data_directory, ec);
    const std::vector<fs::path> copies =
        copy_test_plugin(VSS_TEST_PLUGIN_PROGRAM, VSS_TEST_PLUGIN_RUNTIME,
                         profile.plugin_directory, ec);
    REQUIRE(!ec);
    REQUIRE(!copies.empty());
    const fs::path& program = copies.front();

    process_request request;
    request.program = program;
    request.arguments = std::move(arguments);
    request.working_directory = profile.data_directory;
    request.environment = plugin_environment(plugin_id, profile.data_directory);
    request.sandbox = profile;
    return request;
}

} // namespace

TEST_CASE("plugin runner: a plugin runs confined where the computer can")
{
    if (available_sandbox().level() != sandbox_level::full)
        VSS_SKIP("this computer cannot confine a plugin fully");
    runner_fixture fixture;
    runner_options options = fixture.options_for({});
    options.process = confined_request(fixture.scratch(), {"--serve"});
    options.process.least = sandbox_level::full;
    const auto runner = fixture.make_runner_with(std::move(options));

    const snapshot_ptr ready = start_until_ready(runner);
    REQUIRE(ready->sandbox.level() == sandbox_level::full);
    REQUIRE(ready->process_id != 0);
    REQUIRE_EQ(ready->failures, std::size_t{0});

    static_cast<void>(stop_until_stopped(runner));
}

TEST_CASE("plugin runner: a plugin that cannot be confined as far as asked is "
          "not started")
{
    if (available_sandbox().level() == sandbox_level::full)
        VSS_SKIP("this computer can confine a plugin fully");
    runner_fixture fixture;
    runner_options options = fixture.options_for({});
    options.process = confined_request(fixture.scratch(), {"--serve"});
    options.process.least = sandbox_level::full;
    const auto runner = fixture.make_runner_with(std::move(options));
    runner->start();

    // The state is `stopped` before the start as well, so what is waited for
    // is the failure.
    const snapshot_ptr snapshot =
        wait_for(runner, "the start to be refused",
                 [](const runner_snapshot& latest)
                 {
                     return latest.state == run_state::stopped &&
                            static_cast<bool>(latest.last_failure);
                 });
    require_failure(*snapshot, plugin_errc::sandbox_insufficient);
    REQUIRE_EQ(snapshot->process_id, std::uint64_t{0});
}
