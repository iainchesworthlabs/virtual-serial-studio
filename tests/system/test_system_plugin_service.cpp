//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss::plugin::host::plugin_service` finding, judging, running and stopping
// real plugins.  A test writes plugin folders into a scratch directory: the
// test plugin program (VSS_TEST_PLUGIN_PROGRAM) copied into each, a manifest
// that lists its files, and for an official plugin a signature made with a
// THROWAWAY TEST KEY, a private key made of one repeated byte that is written
// down here for anyone to read.  The service is told through its test hook to
// take that key as the official one; no program that ships does.
//
// The service runs on an io_context of its own with one thread, and its slow
// work on a pool of one thread.  A test calls the service and then waits, with
// a limit, for a condition on its snapshots.  Nothing here sleeps for a fixed
// time to let something happen.
//

#include <vss/core/error.hpp>
#include <vss/license/license.hpp>
#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/host/runner.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/host/service.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/lifecycle.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/signature.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

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
#include <fstream>
#include <functional>
#include <future>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using vss::error_code;
using vss::plugin::inspect_context;
using vss::plugin::key_bytes;
using vss::plugin::key_list;
using vss::plugin::load_trust_store;
using vss::plugin::parse_key_list;
using vss::plugin::extension_kind;
using vss::plugin::permission;
using vss::plugin::plugin_decision;
using vss::plugin::plugin_errc;
using vss::plugin::plugin_purpose;
using vss::plugin::plugin_root;
using vss::plugin::plugin_scope;
using vss::plugin::run_state;
using vss::plugin::sandbox_level;
using vss::plugin::sha512_of;
using vss::plugin::to_hex;
using vss::plugin::trust_level;
using vss::plugin::host::available_sandbox;
using vss::plugin::host::instance_spec;
using vss::plugin::host::instance_state;
using vss::plugin::host::instance_view;
using vss::plugin::host::runner_log_line;
using vss::plugin::host::plugin_service;
using vss::plugin::host::service_options;
using vss::plugin::host::service_plugin;
using vss::plugin::host::service_snapshot;
using vss_test_support::copy_test_plugin;
using vss_test_support::scratch_directory;
using vss_test_support::test_plugin_files;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;
namespace io = vss_test_support;
namespace lic = vss::license;
namespace signing = vss::license::signing;
namespace signed_file = vss::license::signed_file;

using steady = std::chrono::steady_clock;
using snapshot_ptr = std::shared_ptr<const service_snapshot>;

/// How long any wait lasts before the test gives up.
constexpr std::chrono::milliseconds long_limit{60000};

/// The day every scan is made on, so that the end date of an official plugin
/// and the validity of the test key do not depend on the clock.
constexpr std::chrono::sys_days today{2026y / 11 / 14};
constexpr std::chrono::year_month_day decided_day = 2026y / 11 / 14;

constexpr std::string_view official_id = "com.virtualserialstudio.testplugin";
constexpr std::string_view second_official_id =
    "com.virtualserialstudio.testsecond";
constexpr std::string_view licensed_id = "com.virtualserialstudio.testlicensed";
constexpr std::string_view third_party_id = "com.example.testplugin";

// -- the test keys ----------------------------------------------------------

constexpr std::uint8_t plugins_fill = 0x02;
constexpr std::uint8_t revocations_fill = 0x03;

signing::seed test_seed(std::uint8_t fill)
{
    signing::seed seed{};
    seed.fill(fill);
    return seed;
}

key_bytes public_of(std::uint8_t fill)
{
    return signing::public_key_of(test_seed(fill));
}

std::string hex_of(const key_bytes& key)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : key)
    {
        out += digits[static_cast<std::size_t>(byte >> 4U)];
        out += digits[static_cast<std::size_t>(byte & 0x0fU)];
    }
    return out;
}

std::string signed_text(std::string_view purpose, std::string_view key_id,
                        std::string_view payload, std::uint8_t fill)
{
    std::vector<std::uint8_t> bytes;
    for (const char c : payload)
        bytes.push_back(static_cast<std::uint8_t>(c));
    return signed_file::write(
        signing::sign(purpose, key_id, bytes, test_seed(fill)));
}

/// The official key list of the tests, as the hook gives it to the service.
key_list official_keys()
{
    std::string text = R"({ "key_list_version": 1, "serial": 4,
        "issued": "2026-11-13", "keys": [ { "key_id": "plugins-2026",
        "public_key": ")";
    text += hex_of(public_of(plugins_fill));
    text += R"(", "purpose": "plugin", "name": "Virtual Serial Studio",
        "valid_from": "2026-11-01", "valid_to": "2028-11-01" },
        { "key_id": "revocations-2026", "public_key": ")";
    text += hex_of(public_of(revocations_fill));
    text += R"(", "purpose": "plugin-revocations",
        "name": "Virtual Serial Studio", "valid_from": "2026-11-01",
        "valid_to": "2028-11-01" } ], "revoked": [] })";
    const auto parsed = parse_key_list(text);
    REQUIRE(parsed.has_value());
    return parsed.value();
}

// -- files ------------------------------------------------------------------

void write_file(const fs::path& path, std::string_view content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    REQUIRE(!out.fail());
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// What one file of every test plugin is: the test plugin as it was built.
struct program_facts
{
    std::string name;
    std::uint64_t size = 0;
    std::string sha512;
};

/// The files every test plugin has from the build, the program first.  In a
/// build that needs a runtime library beside the program, that is listed too.
const std::vector<program_facts>& test_plugin_facts()
{
    static const std::vector<program_facts> facts = []
    {
        std::vector<program_facts> all;
        for (const fs::path& source :
             test_plugin_files(fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM)),
                               VSS_TEST_PLUGIN_RUNTIME))
        {
            const std::string bytes = read_file(source);
            program_facts out;
            out.name = source.filename().string();
            out.size = bytes.size();
            out.sha512 = to_hex(sha512_of(bytes));
            all.push_back(std::move(out));
        }
        REQUIRE(!all.empty());
        return all;
    }();
    return facts;
}

/// The program of every test plugin.
const program_facts& test_program()
{
    return test_plugin_facts().front();
}

/// What a test plugin is made of.
struct plugin_spec
{
    std::string id{third_party_id};
    std::string name = "Test plugin";
    /// Signed with the official test key, and so with an end date.
    bool official = false;
    /// The edition the plugin asks for, or none.
    std::string edition;
    /// What the plugin's second file, `data.txt`, holds.  The plugin has no
    /// such file when this is empty.
    std::string data;
    /// What the manifest gives the program after the id and the version and
    /// before `--serve`: the modifiers of the test plugin.
    std::vector<std::string> arguments{};
    /// The extension points of the manifest, each as the text of a JSON
    /// object.  The manifest declares the generator `pulse` when there are
    /// none.
    std::vector<std::string> points{};
};

plugin_spec official_plugin(std::string_view id)
{
    plugin_spec spec;
    spec.id = std::string(id);
    spec.official = true;
    return spec;
}

void append_file_entry(std::string& out, std::string_view path,
                       std::uint64_t size, std::string_view sha512)
{
    out.append("{ \"path\": \"");
    out.append(path);
    out.append("\", \"size\": ");
    out.append(std::to_string(size));
    out.append(", \"sha512\": \"");
    out.append(sha512);
    out.append("\" }");
}

/// The `plugin.json` of a test plugin.  The plugin asks for one permission it
/// must have and one it may do without, and lists every file but the two the
/// manifest never lists.  Its program says the id and version of this
/// manifest in "hello"; the modifiers come before `--serve`, which the test
/// plugin needs.
std::string manifest_text(const plugin_spec& spec)
{
    const program_facts& program = test_program();
    std::string out;
    out.append("{\n  \"manifest_version\": 1,\n  \"id\": \"");
    out.append(spec.id);
    out.append("\",\n  \"name\": \"");
    out.append(spec.name);
    out.append("\",\n  \"version\": \"1.2.0\",\n");
    out.append("  \"author\": { \"name\": \"Example Ltd\" },\n");
    if (spec.official)
        out.append("  \"not_after\": \"2027-06-01\",\n");
    out.append("  \"requires\": { \"api\": { \"min\": \"1.0\", ");
    out.append("\"max\": \"1.x\" }");
    if (!spec.edition.empty())
    {
        out.append(", \"edition\": \"");
        out.append(spec.edition);
        out.append("\"");
    }
    out.append(" },\n  \"run\": { \"executable\": \"");
    out.append(program.name);
    out.append("\", \"args\": [ \"--hello-id=");
    out.append(spec.id);
    out.append("\", \"--hello-version=1.2.0\"");
    for (const std::string& argument : spec.arguments)
    {
        out.append(", \"");
        out.append(argument);
        out.append("\"");
    }
    out.append(", \"--serve\" ],\n");
    out.append("    \"platforms\": [ \"windows-x64\", \"windows-arm64\", ");
    out.append("\"linux-x64\", \"linux-arm64\", \"macos-x64\", ");
    out.append("\"macos-arm64\" ] },\n");
    out.append("  \"permissions\": [\n");
    out.append("    { \"name\": \"ports.read\", \"required\": true, ");
    out.append("\"reason\": \"To read the ports.\" },\n");
    out.append("    { \"name\": \"ports.write\", \"required\": false, ");
    out.append("\"reason\": \"To write to the ports.\" }\n  ],\n");
    out.append("  \"extension_points\": [\n");
    if (spec.points.empty())
    {
        out.append("    { \"kind\": \"generator\", \"id\": \"pulse\", ");
        out.append("\"name\": \"Pulse\" }");
    }
    for (std::size_t index = 0; index < spec.points.size(); ++index)
    {
        out.append(index == 0 ? "    " : ",\n    ");
        out.append(spec.points[index]);
    }
    out.append("\n  ],\n");
    out.append("  \"files\": [ ");
    bool first = true;
    for (const program_facts& file : test_plugin_facts())
    {
        if (!first)
            out.append(", ");
        first = false;
        append_file_entry(out, file.name, file.size, file.sha512);
    }
    if (!spec.data.empty())
    {
        out.append(", ");
        append_file_entry(out, "data.txt", spec.data.size(),
                          to_hex(sha512_of(spec.data)));
    }
    out.append(" ]\n}\n");
    return out;
}

/// Signs the plugin in `directory` with the official test key.
void sign_plugin(const fs::path& directory, std::string_view id,
                 std::string_view manifest)
{
    std::string claim = R"({ "plugin_id": ")";
    claim.append(id);
    claim.append(R"(", "version": "1.2.0", "manifest_sha512": ")");
    claim.append(to_hex(sha512_of(manifest)));
    claim.append(R"(" })");
    write_file(
        directory / "plugin.sig",
        signed_text(plugin_purpose, "plugins-2026", claim, plugins_fill));
}

/// A plugin folder `root/<id>`: the program and its runtime library if any,
/// `data.txt` when there is one, `plugin.json` and, for an official plugin,
/// `plugin.sig`.
fs::path make_plugin(const fs::path& root, const plugin_spec& spec)
{
    const fs::path directory = root / spec.id;
    std::error_code ec;
    fs::create_directories(directory, ec);
    REQUIRE(!ec);
    const std::vector<fs::path> copies =
        copy_test_plugin(fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM)),
                         VSS_TEST_PLUGIN_RUNTIME, directory, ec);
    REQUIRE(!ec);
    REQUIRE_EQ(copies.size(), test_plugin_facts().size());
    if (!spec.data.empty())
        write_file(directory / "data.txt", spec.data);
    const std::string manifest = manifest_text(spec);
    write_file(directory / "plugin.json", manifest);
    if (spec.official)
        sign_plugin(directory, spec.id, manifest);
    return directory;
}

/// Removes a folder, trying again for a while: a program that has just ended
/// can keep its file open for a moment on Windows.
bool remove_folder(const fs::path& directory)
{
    const auto deadline = steady::now() + long_limit;
    for (;;)
    {
        std::error_code ec;
        fs::remove_all(directory, ec);
        std::error_code looked;
        if (!ec && !fs::exists(directory, looked))
            return true;
        if (steady::now() >= deadline)
            return false;
        std::this_thread::sleep_for(20ms);
    }
}

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

std::string describe(const service_snapshot& snapshot)
{
    std::string text = "revision ";
    text.append(std::to_string(snapshot.revision));
    text.append(snapshot.scanning ? ", scanning" : ", not scanning");
    for (const service_plugin& plugin : snapshot.plugins)
    {
        text.append("; ");
        text.append(plugin.id);
        text.append(": problem ");
        text.append(plugin.problem.failed() ? plugin.problem.message()
                                            : std::string("none"));
        text.append(" [");
        text.append(plugin.detail);
        text.append("], ");
        text.append(plugin.enabled ? "enabled" : "not enabled");
        text.append(plugin.decided ? ", decided" : ", not decided");
        if (plugin.run != nullptr)
        {
            text.append(", run ");
            text.append(state_name(plugin.run->state));
            text.append(" process ");
            text.append(std::to_string(plugin.run->process_id));
        }
        else
        {
            text.append(", no run");
        }
    }
    for (const std::string& note : snapshot.notes)
    {
        text.append("; note [");
        text.append(note);
        text.push_back(']');
    }
    return text;
}

/// The plugin with this id, or null.  It points into `snapshot`.
const service_plugin* find(const service_snapshot& snapshot,
                           std::string_view id)
{
    for (const service_plugin& plugin : snapshot.plugins)
    {
        if (plugin.id == id)
            return &plugin;
    }
    return nullptr;
}

/// The plugin with this id; the test fails when there is none.
const service_plugin& entry_of(const service_snapshot& snapshot,
                               std::string_view id)
{
    const service_plugin* const plugin = find(snapshot, id);
    if (plugin == nullptr)
    {
        std::string text = "there is no plugin ";
        text.append(id);
        text.append(": ");
        text.append(describe(snapshot));
        throw vss_test::failure(text);
    }
    return *plugin;
}

bool holds(const std::vector<permission>& list, permission wanted)
{
    return std::ranges::find(list, wanted) != list.end();
}

bool has_code(const error_code& code, plugin_errc expected)
{
    return code == make_error_code(expected);
}

/// Fails the test, saying what the problem was, unless it is `expected`.
void require_problem(const service_plugin& entry, plugin_errc expected)
{
    if (has_code(entry.problem, expected))
        return;
    std::string text = "the problem of ";
    text.append(entry.id);
    text.append(" should be ");
    text.append(to_string(expected));
    text.append(", and it is ");
    text.append(entry.problem.failed() ? entry.problem.message()
                                       : std::string("none"));
    text.append(" [");
    text.append(entry.detail);
    text.push_back(']');
    throw vss_test::failure(text);
}

/// The process of a plugin that is running.
std::uint64_t process_of(const service_plugin& entry)
{
    REQUIRE(entry.run != nullptr);
    return entry.run->process_id;
}

// -- waiting ------------------------------------------------------------------

/// Waits until `predicate(snapshot)` holds for the latest snapshot, polling
/// every 10 ms, and returns that snapshot.  Fails the test, saying what was
/// waited for and what the last snapshot was, when the limit passes first.
template <class Predicate>
snapshot_ptr wait_for(const plugin_service& service, std::string_view what,
                      const Predicate& predicate,
                      std::chrono::milliseconds limit = long_limit)
{
    const auto deadline = steady::now() + limit;
    for (;;)
    {
        snapshot_ptr snapshot = service.snapshot();
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

/// The plugin has been found by a scan.
auto is_found(std::string_view id)
{
    return [name = std::string(id)](const service_snapshot& snapshot)
    { return find(snapshot, name) != nullptr; };
}

/// The plugin runs and has said hello.
auto is_ready(std::string_view id)
{
    return [name = std::string(id)](const service_snapshot& snapshot)
    {
        const service_plugin* const entry = find(snapshot, name);
        return entry != nullptr && entry->enabled && entry->run != nullptr &&
               entry->run->state == run_state::ready;
    };
}

/// The plugin may run, is switched off and has no run.
auto is_off(std::string_view id)
{
    return [name = std::string(id)](const service_snapshot& snapshot)
    {
        const service_plugin* const entry = find(snapshot, name);
        return entry != nullptr && !entry->enabled && entry->run == nullptr &&
               !entry->problem.failed();
    };
}

/// The instance of the plugin with this number, or null.  It points into
/// `plugin`.
const instance_view* instance_in(const service_plugin& plugin,
                                 std::uint64_t number)
{
    for (const instance_view& item : plugin.instances)
    {
        if (item.spec.instance == number)
            return &item;
    }
    return nullptr;
}

/// A line of the output or log of the plugin's run is exactly `text`.
bool log_has(const service_plugin& plugin, std::string_view text)
{
    if (plugin.run == nullptr)
        return false;
    return std::ranges::any_of(
        plugin.run->log, [text](const runner_log_line& line)
        { return line.text == text; });
}

/// The plugin has the instance, in this state.
auto instance_is(std::string_view id, std::uint64_t number,
                 instance_state state)
{
    return [name = std::string(id), number, state](
               const service_snapshot& snapshot)
    {
        const service_plugin* const entry = find(snapshot, name);
        if (entry == nullptr)
            return false;
        const instance_view* const item = instance_in(*entry, number);
        return item != nullptr && item->state == state;
    };
}

/// The plugin's run has printed the line.
auto has_line(std::string_view id, std::string_view text)
{
    return [name = std::string(id),
            line = std::string(text)](const service_snapshot& snapshot)
    {
        const service_plugin* const entry = find(snapshot, name);
        return entry != nullptr && log_has(*entry, line);
    };
}

/// Waits until the plugin's run has printed the line.
snapshot_ptr wait_for_line(const plugin_service& service, std::string_view id,
                           std::string_view text)
{
    std::string what = "the plugin to print \"";
    what.append(text);
    what.append("\"");
    return wait_for(service, what, has_line(id, text));
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

// -- the service and what it runs on -----------------------------------------

/// How many times a `done` of `shut_down` was called, for the test's thread.
class done_count
{
public:
    void add()
    {
        const std::lock_guard lock(mutex_);
        ++calls_;
        changed_.notify_all();
    }

    std::size_t calls() const
    {
        const std::lock_guard lock(mutex_);
        return calls_;
    }

    /// False when no call beyond `seen` came within `limit`.
    bool wait_past(std::size_t seen, std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit,
                                 [this, seen] { return calls_ > seen; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t calls_ = 0;
};

/// What the change handler of the service saw.  The handler runs on the
/// service's strand, which is where `runner` may be asked.
struct change_state
{
    std::mutex mutex;
    std::size_t calls = 0;
    /// The ids for which `runner` gave a runner.
    std::set<std::string> with_runner;
    /// A snapshot said a plugin runs and `runner` had none for it.
    bool runner_missing = false;
};

/// A scratch directory, an io_context run by one thread, a pool of one thread
/// for the slow work, and the service that a test makes on them.  The
/// destructor shuts the service down, which ends every plugin, and lets both
/// threads finish.
class service_fixture
{
public:
    service_fixture()
        : keys_(official_keys()),
          guard_(boost::asio::make_work_guard(context_)),
          finished_future_(finished_.get_future()),
          thread_([this] { run_context(); })
    {
    }

    ~service_fixture()
    {
        try
        {
            if (service_ && !closed_)
                static_cast<void>(shut_down_and_wait());
            slow_.join();
            guard_.reset();
            if (finished_future_.wait_for(10s) != std::future_status::ready)
                context_.stop();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // Nothing can be done about it here, and a destructor does not
            // throw.  The thread is stopped below.
        }
        try
        {
            if (thread_.joinable())
            {
                context_.stop();
                thread_.join();
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // The same: a destructor does not throw.
        }
    }

    service_fixture(const service_fixture&) = delete;
    service_fixture& operator=(const service_fixture&) = delete;

    /// The folder the plugins are installed in.
    fs::path plugins_directory() const { return scratch_.path() / "plugins"; }
    fs::path trust_file() const { return scratch_.path() / "trust.json"; }

    /// The options of a service on this fixture: the one plugin folder, a
    /// trust store and a data folder of its own, no policy, no licence, and
    /// the test key as the official one.
    service_options options() const
    {
        service_options out;
        out.roots = {plugin_root{plugins_directory(), plugin_scope::for_user}};
        out.computer.trust_file = trust_file();
        out.computer.today = today;
        out.plan.data_root = scratch_.path() / "data";
        out.change_context_for_tests = [keys = keys_](inspect_context& context)
        { context.official = keys; };
        return out;
    }

    plugin_service& start() { return start(options()); }

    plugin_service& start(service_options given)
    {
        closed_ = false;
        changes_ = std::make_shared<change_state>();
        service_ = plugin_service::create(
            context_.get_executor(), slow_.get_executor(), std::move(given));
        service_->on_change(
            [weak = std::weak_ptr<plugin_service>(service_), state = changes_]()
            {
                const auto locked = weak.lock();
                if (!locked)
                    return;
                const auto snapshot = locked->snapshot();
                const std::lock_guard lock(state->mutex);
                ++state->calls;
                for (const service_plugin& plugin : snapshot->plugins)
                {
                    const bool has_runner =
                        locked->runner(plugin.id) != nullptr;
                    if (has_runner)
                        state->with_runner.insert(plugin.id);
                    if (plugin.run != nullptr && !has_runner)
                        state->runner_missing = true;
                }
            });
        return *service_;
    }

    /// Shuts the service down, waits for its `done`, and makes a new service
    /// with the same options, which finds what the first one stored.
    plugin_service& restart()
    {
        shut_down();
        return start();
    }

    /// Shuts the service down and waits for its `done`.
    void shut_down() { REQUIRE(shut_down_and_wait()); }

    std::size_t done_calls() const { return done_->calls(); }

    /// Whether `runner` gave a runner for the plugin inside the change
    /// handler at least once.
    bool runner_was_seen(const std::string& id) const
    {
        const std::lock_guard lock(changes_->mutex);
        return changes_->with_runner.contains(id);
    }

    /// Whether the handler ever saw a plugin that runs with no runner.
    bool runner_was_missing() const
    {
        const std::lock_guard lock(changes_->mutex);
        return changes_->runner_missing;
    }

private:
    bool shut_down_and_wait()
    {
        if (!service_)
            return true;
        const std::size_t before = done_->calls();
        closed_ = true;
        service_->shut_down([state = done_] { state->add(); });
        return done_->wait_past(before, long_limit);
    }

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
    key_list keys_;
    boost::asio::io_context context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        guard_;
    boost::asio::thread_pool slow_{1};
    std::promise<void> finished_;
    std::future<void> finished_future_;
    std::shared_ptr<change_state> changes_ = std::make_shared<change_state>();
    std::shared_ptr<done_count> done_ = std::make_shared<done_count>();
    std::shared_ptr<plugin_service> service_;
    bool closed_ = false;
    std::thread thread_;
};

/// A person's answer to "may this plugin run": yes, with the permissions it
/// needs and not the one it may do without.  Waits until the plugin is ready
/// and returns that snapshot.
///
/// A computer that cannot confine a plugin as far as the minimum refuses a
/// plugin that is not official, as it must; the answer is then given again
/// with the user's choice to run it without isolation, which a copy of Studio
/// that the user controls allows.
snapshot_ptr trust_and_wait(plugin_service& service, std::string_view id)
{
    const snapshot_ptr found =
        wait_for(service, "the plugin to be found", is_found(id));
    const service_plugin& entry = entry_of(*found, id);

    plugin_decision decision;
    decision.plugin_id = entry.id;
    decision.hash = entry.hash;
    decision.key_id = entry.key_id;
    decision.level = entry.level.value_or(trust_level::unsigned_plugin);
    decision.enabled = true;
    decision.granted = {permission::ports_read};
    decision.decided_on = decided_day;
    service.decide(decision);

    if (available_sandbox().level() < sandbox_level::partial)
    {
        const std::string name(id);
        const snapshot_ptr refused =
            wait_for(service, "the plugin to be refused for want of isolation",
                     [&name](const service_snapshot& latest)
                     {
                         const service_plugin* const plugin =
                             find(latest, name);
                         return plugin != nullptr && plugin->decided &&
                                has_code(plugin->problem,
                                         plugin_errc::sandbox_insufficient);
                     });
        REQUIRE(entry_of(*refused, id).run == nullptr);
        decision.run_without_sandbox = true;
        service.decide(decision);
    }
    return wait_for(service, "the plugin to be ready", is_ready(id));
}

/// A licence that covers every official plugin, as the licence library judges
/// it on a day inside its term.
lic::status licence_that_covers_official_plugins()
{
    const auto at = [](int year, unsigned month, unsigned day)
    {
        return lic::time_point{std::chrono::sys_days{std::chrono::year{year} /
                                                     std::chrono::month{month} /
                                                     std::chrono::day{day}}};
    };
    lic::licence value;
    value.licence_id = "VSS-2026-000123";
    value.licensee_name = "Example Lab Pty Ltd";
    value.issued_at = at(2026, 11, 13);
    value.starts_at = at(2026, 11, 13);
    value.expires_at = at(2027, 11, 13);
    value.grace_days = 28;
    value.features = {"plugins.official"};
    lic::context where;
    where.now = at(2027, 1, 1);
    const lic::status judged = lic::evaluate(value, nullptr, where);
    REQUIRE(judged.active());
    return judged;
}

/// An official plugin that asks for the Pro edition.
plugin_spec licensed_plugin()
{
    plugin_spec spec = official_plugin(licensed_id);
    spec.edition = "pro";
    return spec;
}

/// The licensed plugin, with the licence the service is given (none when the
/// function is empty).  It must not run, and it says why.
void require_not_licensed(std::function<lic::status()> licence)
{
    service_fixture fixture;
    static_cast<void>(
        make_plugin(fixture.plugins_directory(), licensed_plugin()));
    service_options options = fixture.options();
    options.licence = std::move(licence);
    plugin_service& service = fixture.start(std::move(options));
    service.rescan();

    const snapshot_ptr found =
        wait_for(service, "the plugin to be found", is_found(licensed_id));
    const service_plugin& entry = entry_of(*found, licensed_id);
    require_problem(entry, plugin_errc::not_licensed);
    REQUIRE(entry.run == nullptr);
    REQUIRE(!entry.enabled);
    REQUIRE(entry.level.has_value());
    REQUIRE(entry.level == trust_level::official);
}

} // namespace

// -- finding plugins ----------------------------------------------------------

TEST_CASE("plugin service: with no plugins installed a scan finds none")
{
    service_fixture fixture;
    plugin_service& service = fixture.start();

    const snapshot_ptr before = service.snapshot();
    REQUIRE(before != nullptr);
    REQUIRE(before->plugins.empty());
    REQUIRE(!before->scanning);

    service.rescan();
    const snapshot_ptr after = wait_for(
        service, "the scan to finish", [&before](const service_snapshot& latest)
        { return latest.revision > before->revision && !latest.scanning; });
    REQUIRE(after->plugins.empty());
    REQUIRE(!after->scanning);
    REQUIRE(after->revision > before->revision);
}

TEST_CASE("plugin service: an official plugin is found, runs and is ready")
{
    service_fixture fixture;
    static_cast<void>(
        make_plugin(fixture.plugins_directory(), official_plugin(official_id)));
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr ready =
        wait_for(service, "the plugin to be ready", is_ready(official_id));
    const service_plugin& entry = entry_of(*ready, official_id);

    REQUIRE_EQ(entry.id, std::string(official_id));
    REQUIRE_EQ(entry.name, "Test plugin");
    REQUIRE_EQ(entry.version, "1.2.0");
    REQUIRE_EQ(entry.author, "Example Ltd");
    REQUIRE_EQ(entry.directory.filename().string(), std::string(official_id));
    REQUIRE(entry.scope == plugin_scope::for_user);
    REQUIRE(entry.level.has_value());
    REQUIRE(entry.level == trust_level::official);
    REQUIRE_EQ(entry.key_id, "plugins-2026");
    REQUIRE(!entry.problem.failed());
    REQUIRE(entry.enabled);
    // Nobody has decided: the signature is what trusts an official plugin.
    REQUIRE(!entry.decided);

    // It is given what it asked for, and nothing is held back.
    REQUIRE_EQ(entry.asked.size(), std::size_t{2});
    REQUIRE(holds(entry.granted, permission::ports_read));
    REQUIRE(holds(entry.granted, permission::ports_write));
    REQUIRE(entry.withheld.empty());

    REQUIRE(entry.run != nullptr);
    REQUIRE(entry.run->state == run_state::ready);
    const std::uint64_t pid = process_of(entry);
    REQUIRE(pid != 0);
    REQUIRE(io::process_exists(pid));

    // The runner was there for the code that carries an extension point,
    // asked on the service's strand.
    REQUIRE(fixture.runner_was_seen(std::string(official_id)));
    REQUIRE(!fixture.runner_was_missing());
}

// -- plugins that are not official -------------------------------------------

TEST_CASE("plugin service: a plugin that is not signed waits for a decision")
{
    service_fixture fixture;
    static_cast<void>(make_plugin(fixture.plugins_directory(), plugin_spec{}));
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr found =
        wait_for(service, "the plugin to be found", is_found(third_party_id));
    const service_plugin& entry = entry_of(*found, third_party_id);

    require_problem(entry, plugin_errc::signature_missing);
    REQUIRE(!entry.detail.empty());
    REQUIRE(entry.run == nullptr);
    REQUIRE(!entry.decided);
    REQUIRE(!entry.enabled);
    REQUIRE(entry.level.has_value());
    REQUIRE(entry.level == trust_level::unsigned_plugin);
    REQUIRE(entry.key_id.empty());
    REQUIRE(!found->scanning);
}

TEST_CASE("plugin service: a decision lets the plugin run and is kept")
{
    service_fixture fixture;
    static_cast<void>(make_plugin(fixture.plugins_directory(), plugin_spec{}));
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr ready = trust_and_wait(service, third_party_id);
    const service_plugin& entry = entry_of(*ready, third_party_id);

    REQUIRE(entry.decided);
    REQUIRE(entry.enabled);
    REQUIRE(!entry.problem.failed());
    REQUIRE(entry.run != nullptr);
    REQUIRE(entry.run->state == run_state::ready);
    REQUIRE(process_of(entry) != 0);

    // It has what the decision granted, its own data folder, and not the
    // permission it may do without.
    REQUIRE(holds(entry.granted, permission::ports_read));
    REQUIRE(holds(entry.granted, permission::data_private));
    REQUIRE(!holds(entry.granted, permission::ports_write));
    REQUIRE(holds(entry.withheld, permission::ports_write));

    // The decision is in the trust store on disk, for these exact files.
    const auto stored = load_trust_store(fixture.trust_file());
    REQUIRE(stored.has_value());
    const auto& decisions = stored.value().decisions();
    REQUIRE_EQ(decisions.size(), std::size_t{1});
    const plugin_decision& kept = decisions.front();
    REQUIRE_EQ(kept.plugin_id, std::string(third_party_id));
    REQUIRE(kept.hash == entry.hash);
    REQUIRE(kept.key_id.empty());
    REQUIRE(kept.level == trust_level::unsigned_plugin);
    REQUIRE(kept.enabled);
    REQUIRE(holds(kept.granted, permission::ports_read));
}

TEST_CASE("plugin service: switching on a plugin nobody decided about leaves "
          "a note")
{
    service_fixture fixture;
    static_cast<void>(make_plugin(fixture.plugins_directory(), plugin_spec{}));
    plugin_service& service = fixture.start();
    service.rescan();
    static_cast<void>(
        wait_for(service, "the plugin to be found", is_found(third_party_id)));

    service.set_enabled(std::string(third_party_id), true);
    const snapshot_ptr noted = wait_for(
        service, "a note about the decision",
        [](const service_snapshot& latest)
        {
            return !latest.scanning &&
                   std::ranges::any_of(
                       latest.notes, [](const std::string& note)
                       { return note.find("decision") != std::string::npos; });
        });

    // Only a person's decision about this copy lets it run.
    const service_plugin& entry = entry_of(*noted, third_party_id);
    require_problem(entry, plugin_errc::signature_missing);
    REQUIRE(entry.run == nullptr);
    REQUIRE(!entry.decided);
    REQUIRE(!entry.enabled);
}

TEST_CASE("plugin service: forgetting a decision stops the plugin and asks "
          "again")
{
    service_fixture fixture;
    static_cast<void>(make_plugin(fixture.plugins_directory(), plugin_spec{}));
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr ready = trust_and_wait(service, third_party_id);
    const std::uint64_t pid = process_of(entry_of(*ready, third_party_id));
    REQUIRE(pid != 0);
    REQUIRE(entry_of(*ready, third_party_id).decided);

    service.forget(std::string(third_party_id));
    const snapshot_ptr asked = wait_for(
        service, "the plugin to wait for a decision again",
        [](const service_snapshot& latest)
        {
            const service_plugin* const plugin = find(latest, third_party_id);
            return plugin != nullptr && !plugin->decided &&
                   plugin->run == nullptr &&
                   has_code(plugin->problem, plugin_errc::signature_missing);
        });
    REQUIRE(!entry_of(*asked, third_party_id).enabled);
    REQUIRE(wait_until_gone(pid));

    // The store no longer holds a decision for it.
    const auto stored = load_trust_store(fixture.trust_file());
    REQUIRE(stored.has_value());
    REQUIRE(stored.value().decisions().empty());
}

// -- switching a plugin off and on --------------------------------------------

TEST_CASE("plugin service: switching a plugin off stops it, and the choice "
          "is kept")
{
    service_fixture fixture;
    static_cast<void>(
        make_plugin(fixture.plugins_directory(), official_plugin(official_id)));
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr ready =
        wait_for(service, "the plugin to be ready", is_ready(official_id));
    const std::uint64_t first_pid = process_of(entry_of(*ready, official_id));
    REQUIRE(first_pid != 0);

    // Off: the plugin is judged as it was and is not run.
    service.set_enabled(std::string(official_id), false);
    const snapshot_ptr off =
        wait_for(service, "the plugin to be off", is_off(official_id));
    REQUIRE(entry_of(*off, official_id).decided);
    REQUIRE(wait_until_gone(first_pid));
    {
        const auto stored = load_trust_store(fixture.trust_file());
        REQUIRE(stored.has_value());
        REQUIRE_EQ(stored.value().decisions().size(), std::size_t{1});
        REQUIRE(!stored.value().decisions().front().enabled);
    }

    // On again: it is started afresh.
    service.set_enabled(std::string(official_id), true);
    const snapshot_ptr again =
        wait_for(service, "the plugin to run again", is_ready(official_id));
    const std::uint64_t second_pid = process_of(entry_of(*again, official_id));
    REQUIRE(second_pid != 0);
    REQUIRE(io::process_exists(second_pid));

    // Off once more, and then a new service on the same trust store: the
    // plugin stays off.
    service.set_enabled(std::string(official_id), false);
    static_cast<void>(
        wait_for(service, "the plugin to be off again", is_off(official_id)));
    REQUIRE(wait_until_gone(second_pid));

    plugin_service& next = fixture.restart();
    next.rescan();
    const snapshot_ptr kept = wait_for(
        next, "the new service to find the plugin",
        [](const service_snapshot& latest)
        { return !latest.scanning && find(latest, official_id) != nullptr; });
    const service_plugin& entry = entry_of(*kept, official_id);
    REQUIRE(entry.decided);
    REQUIRE(!entry.enabled);
    REQUIRE(!entry.problem.failed());
    REQUIRE(entry.run == nullptr);
}

TEST_CASE("plugin service: a plugin that is stopped does not disturb another")
{
    service_fixture fixture;
    static_cast<void>(
        make_plugin(fixture.plugins_directory(), official_plugin(official_id)));
    static_cast<void>(make_plugin(fixture.plugins_directory(),
                                  official_plugin(second_official_id)));
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr both =
        wait_for(service, "both plugins to be ready",
                 [](const service_snapshot& latest)
                 {
                     return is_ready(official_id)(latest) &&
                            is_ready(second_official_id)(latest);
                 });
    const std::uint64_t first_pid = process_of(entry_of(*both, official_id));
    const std::uint64_t second_pid =
        process_of(entry_of(*both, second_official_id));
    REQUIRE(first_pid != 0);
    REQUIRE(second_pid != 0);
    REQUIRE(first_pid != second_pid);

    service.set_enabled(std::string(official_id), false);
    const snapshot_ptr one_off =
        wait_for(service, "the first plugin to be off", is_off(official_id));
    REQUIRE(wait_until_gone(first_pid));

    // The other was not started again: same process, still ready.
    const service_plugin& other = entry_of(*one_off, second_official_id);
    REQUIRE(other.run != nullptr);
    REQUIRE(other.run->state == run_state::ready);
    REQUIRE_EQ(other.run->process_id, second_pid);
    REQUIRE(io::process_exists(second_pid));
}

// -- files that change --------------------------------------------------------

TEST_CASE("plugin service: a plugin whose file changed is stopped and "
          "refused")
{
    service_fixture fixture;
    plugin_spec spec = official_plugin(official_id);
    spec.data = "the first text";
    const fs::path directory = make_plugin(fixture.plugins_directory(), spec);
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr ready =
        wait_for(service, "the plugin to be ready", is_ready(official_id));
    const std::uint64_t pid = process_of(entry_of(*ready, official_id));
    REQUIRE(pid != 0);

    // The manifest lists `data.txt` with the digest of the first text.
    write_file(directory / "data.txt", "a text that is not the first");
    service.rescan();
    const snapshot_ptr refused =
        wait_for(service, "the plugin to be refused",
                 [](const service_snapshot& latest)
                 {
                     const service_plugin* const plugin =
                         find(latest, official_id);
                     return plugin != nullptr && plugin->run == nullptr &&
                            plugin->problem.failed();
                 });
    const service_plugin& entry = entry_of(*refused, official_id);
    require_problem(entry, plugin_errc::signature_invalid);
    REQUIRE(entry.detail.find("data.txt") != std::string::npos);
    REQUIRE(!entry.enabled);
    REQUIRE(wait_until_gone(pid));

    // Put back as it was, it is the plugin that was checked and runs again.
    write_file(directory / "data.txt", spec.data);
    service.rescan();
    const snapshot_ptr again =
        wait_for(service, "the plugin to run again", is_ready(official_id));
    REQUIRE(!entry_of(*again, official_id).problem.failed());
    REQUIRE(process_of(entry_of(*again, official_id)) != 0);
}

TEST_CASE("plugin service: a plugin folder that is removed goes from the "
          "snapshot")
{
    service_fixture fixture;
    const fs::path directory =
        make_plugin(fixture.plugins_directory(), official_plugin(official_id));
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr ready =
        wait_for(service, "the plugin to be ready", is_ready(official_id));
    const std::uint64_t pid = process_of(entry_of(*ready, official_id));
    REQUIRE(pid != 0);

    // The files of a plugin that runs are in use; stop it first.
    service.set_enabled(std::string(official_id), false);
    static_cast<void>(
        wait_for(service, "the plugin to be off", is_off(official_id)));
    REQUIRE(wait_until_gone(pid));
    REQUIRE(remove_folder(directory));

    service.rescan();
    const snapshot_ptr gone = wait_for(
        service, "the plugin to be gone", [](const service_snapshot& latest)
        { return !latest.scanning && find(latest, official_id) == nullptr; });
    REQUIRE(gone->plugins.empty());
}

// -- licences -----------------------------------------------------------------

TEST_CASE("plugin service: a plugin that needs a licence does not run when "
          "the service is given none")
{
    // The header says that no plugin that needs a licence runs when the
    // service has no function for it.
    require_not_licensed(nullptr);
}

TEST_CASE("plugin service: a plugin that needs a licence does not run with "
          "no licence on the computer")
{
    require_not_licensed([] { return lic::no_licence(); });
}

TEST_CASE("plugin service: a plugin that needs a licence runs when a licence "
          "covers it")
{
    service_fixture fixture;
    static_cast<void>(
        make_plugin(fixture.plugins_directory(), licensed_plugin()));
    service_options options = fixture.options();
    lic::status covered = licence_that_covers_official_plugins();
    options.licence = [covered] { return covered; };
    plugin_service& service = fixture.start(std::move(options));
    service.rescan();

    const snapshot_ptr ready =
        wait_for(service, "the plugin to be ready", is_ready(licensed_id));
    const service_plugin& entry = entry_of(*ready, licensed_id);
    REQUIRE(!entry.problem.failed());
    REQUIRE(entry.run != nullptr);
    REQUIRE(process_of(entry) != 0);
}

// -- shutting down and busy services ------------------------------------------

TEST_CASE("plugin service: shutting down stops every plugin, calls done once "
          "and starts nothing afterwards")
{
    service_fixture fixture;
    static_cast<void>(
        make_plugin(fixture.plugins_directory(), official_plugin(official_id)));
    static_cast<void>(make_plugin(fixture.plugins_directory(),
                                  official_plugin(second_official_id)));
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr both =
        wait_for(service, "both plugins to be ready",
                 [](const service_snapshot& latest)
                 {
                     return is_ready(official_id)(latest) &&
                            is_ready(second_official_id)(latest);
                 });
    const std::uint64_t first_pid = process_of(entry_of(*both, official_id));
    const std::uint64_t second_pid =
        process_of(entry_of(*both, second_official_id));
    REQUIRE(first_pid != 0);
    REQUIRE(second_pid != 0);

    REQUIRE_EQ(fixture.done_calls(), std::size_t{0});
    fixture.shut_down();
    REQUIRE_EQ(fixture.done_calls(), std::size_t{1});
    REQUIRE(wait_until_gone(first_pid));
    REQUIRE(wait_until_gone(second_pid));

    // The snapshot soon shows no run for either.
    static_cast<void>(wait_for(service, "no plugin to run",
                               [](const service_snapshot& latest)
                               {
                                   return std::ranges::none_of(
                                       latest.plugins,
                                       [](const service_plugin& plugin)
                                       { return plugin.run != nullptr; });
                               }));

    // A scan asked for now starts nothing.  The second shutdown goes through
    // the same strand, after the scan was asked for, so that it is known the
    // service has dealt with it; then the snapshot is looked at for a short
    // while, a time to see that nothing happens, not one to wait for it.
    service.rescan();
    fixture.shut_down();
    REQUIRE_EQ(fixture.done_calls(), std::size_t{2});
    const auto until = steady::now() + 200ms;
    while (steady::now() < until)
    {
        const snapshot_ptr latest = service.snapshot();
        REQUIRE(latest != nullptr);
        REQUIRE(!latest->scanning);
        for (const service_plugin& plugin : latest->plugins)
            REQUIRE(plugin.run == nullptr);
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(!io::process_exists(first_pid));
    REQUIRE(!io::process_exists(second_pid));
}

TEST_CASE("plugin service: calls made while a scan runs are not lost")
{
    service_fixture fixture;
    static_cast<void>(
        make_plugin(fixture.plugins_directory(), official_plugin(official_id)));
    plugin_service& service = fixture.start();
    service.rescan();

    const snapshot_ptr ready =
        wait_for(service, "the plugin to be ready", is_ready(official_id));
    const std::uint64_t pid = process_of(entry_of(*ready, official_id));
    REQUIRE(pid != 0);

    // The first scan is under way when the other two calls come.  Between
    // them the service keeps the switch, and one scan serves both.
    service.rescan();
    service.set_enabled(std::string(official_id), false);
    service.rescan();

    const snapshot_ptr last =
        wait_for(service, "the switch to show once the scans are done",
                 [](const service_snapshot& latest)
                 {
                     const service_plugin* const plugin =
                         find(latest, official_id);
                     return !latest.scanning && plugin != nullptr &&
                            !plugin->enabled && plugin->run == nullptr;
                 });
    REQUIRE(entry_of(*last, official_id).decided);
    REQUIRE(wait_until_gone(pid));
}

TEST_CASE("plugin service: an approved key is kept by the service and shown")
{
    service_fixture fixture;
    plugin_service& service = fixture.start(fixture.options());
    service.rescan();
    static_cast<void>(wait_for(service, "the first scan",
                               [](const service_snapshot& now)
                               { return now.scans > 0; }));

    vss::plugin::approved_key key;
    key.key_id = "acme-2026";
    key.name = "Acme";
    key.public_key.fill(0x11);
    service.approve_key(key);
    const snapshot_ptr approved =
        wait_for(service, "the key to be shown", [](const service_snapshot& now)
                 { return now.approved_keys.size() == std::size_t{1}; });
    REQUIRE_EQ(approved->approved_keys.front().key_id, "acme-2026");
    REQUIRE_EQ(approved->approved_keys.front().name, "Acme");

    // It is in the trust store on disk, written by the service.
    const auto stored = load_trust_store(fixture.trust_file());
    REQUIRE(stored.has_value());
    REQUIRE(stored.value().find_key("acme-2026") != nullptr);

    service.forget_key("acme-2026");
    static_cast<void>(wait_for(service, "the key to go",
                               [](const service_snapshot& now)
                               { return now.approved_keys.empty(); }));
    const auto after = load_trust_store(fixture.trust_file());
    REQUIRE(after.has_value());
    REQUIRE(after.value().find_key("acme-2026") == nullptr);
}

// -- instances ----------------------------------------------------------------

namespace
{

/// An official plugin whose manifest declares the adapter `recorder`, and
/// whose program answers what `modifiers` say, as the test plugin does.
plugin_spec recorder_plugin(std::vector<std::string> modifiers)
{
    plugin_spec spec = official_plugin(official_id);
    spec.points = {R"({ "kind": "adapter", "id": "recorder", )"
                   R"("name": "Recorder" })"};
    spec.arguments = std::move(modifiers);
    return spec;
}

/// The three messages of an instance, answered with "ok".
std::vector<std::string> echo_instance_messages()
{
    return {"--echo-op=instance.create", "--echo-op=params.changed",
            "--echo-op=instance.stop"};
}

instance_spec recorder_instance(std::uint64_t number,
                                std::string params = "{}")
{
    instance_spec spec;
    spec.instance = number;
    spec.kind = extension_kind::adapter;
    spec.type = "recorder";
    spec.params = std::move(params);
    return spec;
}

} // namespace

TEST_CASE("plugin service: the instances wanted of a plugin are created and "
          "kept in step")
{
    service_fixture fixture;
    static_cast<void>(make_plugin(fixture.plugins_directory(),
                                  recorder_plugin(echo_instance_messages())));
    plugin_service& service = fixture.start();
    service.set_instances(std::string(official_id), {recorder_instance(1)});
    service.rescan();

    const snapshot_ptr running =
        wait_for(service, "the instance to run",
                 instance_is(official_id, 1, instance_state::running));
    const service_plugin& entry = entry_of(*running, official_id);
    REQUIRE_EQ(entry.instances.size(), std::size_t{1});
    const instance_view& item = entry.instances.front();
    REQUIRE_EQ(item.spec.instance, std::uint64_t{1});
    REQUIRE(item.spec.kind == extension_kind::adapter);
    REQUIRE_EQ(item.spec.type, "recorder");
    REQUIRE(!item.problem.failed());
    REQUIRE(item.reason.empty());
    REQUIRE(!item.params_pending);
    static_cast<void>(
        wait_for_line(service, official_id, "got instance.create"));

    // New parameters are told to the plugin, and the instance goes on.
    service.set_instances(std::string(official_id),
                          {recorder_instance(1, R"({ "channel": 2 })")});
    const snapshot_ptr changed = wait_for(
        service, "the plugin to be told the new parameters",
        [](const service_snapshot& latest)
        {
            const service_plugin* const plugin = find(latest, official_id);
            if (plugin == nullptr)
                return false;
            const instance_view* const found = instance_in(*plugin, 1);
            return found != nullptr &&
                   found->state == instance_state::running &&
                   !found->params_pending &&
                   found->spec.params == R"({ "channel": 2 })" &&
                   log_has(*plugin, "got params.changed");
        });
    const instance_view* const kept =
        instance_in(entry_of(*changed, official_id), 1);
    REQUIRE(kept != nullptr);
    REQUIRE(!kept->problem.failed());

    // An empty list stops the instance, and then it is gone.
    service.set_instances(std::string(official_id), {});
    static_cast<void>(wait_for(
        service, "the instance to be stopped and gone",
        [](const service_snapshot& latest)
        {
            const service_plugin* const plugin = find(latest, official_id);
            return plugin != nullptr && plugin->instances.empty() &&
                   log_has(*plugin, "got instance.stop");
        }));
}

TEST_CASE("plugin service: an instance the plugin refuses is refused")
{
    service_fixture fixture;
    static_cast<void>(
        make_plugin(fixture.plugins_directory(),
                    recorder_plugin({"--refuse-op=instance.create"})));
    plugin_service& service = fixture.start();
    service.set_instances(std::string(official_id), {recorder_instance(1)});
    service.rescan();

    const snapshot_ptr refused =
        wait_for(service, "the instance to be refused",
                 instance_is(official_id, 1, instance_state::refused));
    const service_plugin& entry = entry_of(*refused, official_id);
    const instance_view* const item = instance_in(entry, 1);
    REQUIRE(item != nullptr);
    REQUIRE(has_code(item->problem, plugin_errc::misbehaving));
    REQUIRE_EQ(item->reason, "refused by the test plugin");
    // The plugin itself goes on.
    REQUIRE(entry.run != nullptr);
    REQUIRE(entry.run->state == run_state::ready);
}

TEST_CASE("plugin service: an instance of a type the manifest does not "
          "declare is refused before the plugin is asked")
{
    service_fixture fixture;
    static_cast<void>(make_plugin(fixture.plugins_directory(),
                                  recorder_plugin(echo_instance_messages())));
    plugin_service& service = fixture.start();
    instance_spec unknown = recorder_instance(1);
    unknown.type = "playback";
    service.set_instances(std::string(official_id), {unknown});
    service.rescan();

    static_cast<void>(
        wait_for(service, "the plugin to be ready", is_ready(official_id)));
    const snapshot_ptr refused =
        wait_for(service, "the instance to be refused",
                 instance_is(official_id, 1, instance_state::refused));
    const service_plugin& entry = entry_of(*refused, official_id);
    const instance_view* const item = instance_in(entry, 1);
    REQUIRE(item != nullptr);
    REQUIRE(has_code(item->problem, plugin_errc::capability_missing));
    REQUIRE(item->reason.empty());
    REQUIRE(!log_has(entry, "got instance.create"));
}

TEST_CASE("plugin service: a plugin switched off and on has its instance "
          "created again")
{
    service_fixture fixture;
    static_cast<void>(make_plugin(fixture.plugins_directory(),
                                  recorder_plugin(echo_instance_messages())));
    plugin_service& service = fixture.start();
    service.set_instances(std::string(official_id), {recorder_instance(1)});
    service.rescan();

    const snapshot_ptr first =
        wait_for(service, "the instance to run",
                 instance_is(official_id, 1, instance_state::running));
    const std::uint64_t first_pid = process_of(entry_of(*first, official_id));
    static_cast<void>(
        wait_for_line(service, official_id, "got instance.create"));

    service.set_enabled(std::string(official_id), false);
    static_cast<void>(
        wait_for(service, "the plugin to be off", is_off(official_id)));
    REQUIRE(wait_until_gone(first_pid));

    service.set_enabled(std::string(official_id), true);
    const snapshot_ptr again = wait_for(
        service, "the instance to be created again",
        [first_pid](const service_snapshot& latest)
        {
            const service_plugin* const plugin = find(latest, official_id);
            if (plugin == nullptr || plugin->run == nullptr ||
                plugin->run->process_id == first_pid)
                return false;
            const instance_view* const item = instance_in(*plugin, 1);
            return item != nullptr && item->state == instance_state::running &&
                   log_has(*plugin, "got instance.create");
        });
    REQUIRE(process_of(entry_of(*again, official_id)) != first_pid);
}

TEST_CASE("plugin service: instances wanted of a plugin that is not "
          "installed are kept")
{
    service_fixture fixture;
    plugin_service& service = fixture.start();
    service.set_instances("com.example.nothere", {recorder_instance(1)});
    service.set_instances(std::string(official_id), {recorder_instance(1)});
    service.rescan();

    const snapshot_ptr none =
        wait_for(service, "the scan to finish",
                 [](const service_snapshot& latest)
                 { return latest.scans >= 1 && !latest.scanning; });
    REQUIRE(none->plugins.empty());

    // The plugin is installed afterwards and has the instance that was
    // wanted of it all along.
    static_cast<void>(make_plugin(fixture.plugins_directory(),
                                  recorder_plugin(echo_instance_messages())));
    service.rescan();
    const snapshot_ptr running =
        wait_for(service, "the instance to run",
                 instance_is(official_id, 1, instance_state::running));
    REQUIRE_EQ(running->plugins.size(), std::size_t{1});
}
