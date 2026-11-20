//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real plugin service behind the page's backend, against a scratch folder
// that holds one plugin nobody signed: the test plugin program of the system
// tests, copied beside a manifest that lists it. The plugin waits for the
// user's decision, runs after it, and is off after "Disable".
//

#include "backend/service_plugin_backend.hpp"

#include <vss/plugin/hash.hpp>

#include "qt_wait.hpp"
#include "test_framework.hpp"
#include "test_plugin_files.hpp"

#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace vss::gui;
namespace plug = vss::plugin;
namespace fs = std::filesystem;

namespace
{

constexpr const char* kPluginId = "org.example.served";

std::string read_file(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/// A plugin folder `root/<id>`: the test plugin program, the runtime library
/// a build with the address sanitiser of MSVC needs beside it, and a
/// `plugin.json` that lists them, as the system tests lay one out.
fs::path make_plugin(const fs::path& root, const std::string& more_args = {},
                     const std::string& more_permissions = {},
                     const std::string& point_extra = {},
                     const std::string& version = "1.2.0")
{
    const fs::path program = fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM));
    const fs::path directory = root / kPluginId;
    std::error_code ec;
    fs::create_directories(directory, ec);
    REQUIRE(!ec);
    const std::vector<fs::path> copies = vss_test_support::copy_test_plugin(
        program, VSS_TEST_PLUGIN_RUNTIME, directory, ec);
    REQUIRE(!ec);
    REQUIRE(!copies.empty());
    const std::string name = copies.front().filename().string();
    std::string manifest;
    manifest += "{\n  \"manifest_version\": 1,\n  \"id\": \"";
    manifest += kPluginId;
    manifest += "\",\n  \"name\": \"Served\",\n  \"version\": \"" + version +
                "\",\n";
    manifest += "  \"author\": { \"name\": \"Example Ltd\" },\n";
    manifest += "  \"requires\": { \"api\": { \"min\": \"1.0\", \"max\": "
                "\"1.x\" } },\n";
    manifest += "  \"run\": { \"executable\": \"" + name +
                "\", \"args\": [ "
                "\"--hello-id=" +
                std::string(kPluginId) + "\", \"--hello-version=" + version +
                "\"" + more_args + ", \"--serve\" ],\n";
    manifest += "    \"platforms\": [ \"windows-x64\", \"windows-arm64\", "
                "\"linux-x64\", \"linux-arm64\", \"macos-x64\", "
                "\"macos-arm64\" ] },\n";
    manifest += "  \"permissions\": [ { \"name\": \"ports.read\", "
                "\"required\": true, \"reason\": \"To read the ports.\" }" +
                more_permissions + " ],\n";
    manifest += "  \"extension_points\": [ { \"kind\": \"generator\", "
                "\"id\": \"pulse\", \"name\": \"Pulse\"" +
                point_extra + " } ],\n";
    manifest += "  \"files\": [";
    for (const fs::path& copy : copies)
    {
        const std::string bytes = read_file(copy);
        manifest += copy == copies.front() ? " " : ", ";
        manifest += "{ \"path\": \"" + copy.filename().string() +
                    "\", \"size\": " + std::to_string(bytes.size()) +
                    ", \"sha512\": \"" + plug::to_hex(plug::sha512_of(bytes)) +
                    "\" }";
    }
    manifest += " ]\n}\n";
    write_file(directory / "plugin.json", manifest);
    return directory;
}

const plugin_view* find_view(const plugin_snapshot_ptr& snapshot,
                             const QString& id)
{
    for (const plugin_view& each : snapshot->plugins)
    {
        if (each.id == id)
            return &each;
    }
    return nullptr;
}

} // namespace

TEST_CASE("service backend: the real service runs an unsigned plugin after a "
          "decision, and it is Off after Disable")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    make_plugin(plugins);

    service_plugin_options options;
    plug::plugin_root user;
    user.directory = plugins;
    user.scope = plug::plugin_scope::for_user;
    options.roots.push_back(user);
    options.program_directory =
        fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM)).parent_path();
    options.trust_file = root / "settings" / "trust.json";
    options.data_root = root / "data";
    options.user_root = plugins;
    options.seed = 1;
    ServicePluginBackend backend(std::move(options));

    int changes = 0;
    QObject::connect(&backend, &PluginBackend::snapshot_changed, &backend,
                     [&changes] { ++changes; });
    const QString id = QString::fromLatin1(kPluginId);
    const auto state_is = [&](plugin_state wanted)
    {
        const plugin_view* view = find_view(backend.snapshot(), id);
        return view != nullptr && view->state == wanted;
    };

    // Found, and waiting for the person: nobody decided about it.
    REQUIRE(vss_test::wait_until(
        [&] { return state_is(plugin_state::needs_decision); }, 30000));
    // The page is told through a queued signal, once for a burst.
    REQUIRE(vss_test::wait_until([&] { return changes >= 1; }, 30000));
    // The manifest is read a moment after the plugin is found.
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const plugin_view* found = find_view(backend.snapshot(), id);
            return found != nullptr && !found->adds.empty();
        },
        30000));
    plugin_view view = *find_view(backend.snapshot(), id);
    REQUIRE(view.trust == plugin_trust::unsigned_plugin);
    REQUIRE_EQ(view.reason_code, QStringLiteral("signature_missing"));
    REQUIRE_EQ(view.fingerprint.size(), 16);
    REQUIRE_EQ(view.adds.size(), std::size_t{1});
    REQUIRE_EQ(view.adds[0].type_name, QStringLiteral("Pulse"));
    REQUIRE(backend.snapshot()->can_run_plugins);

    // The decision, with the hash the question was asked for: the plugin
    // runs.
    plugin_decision_request request;
    request.plugin_id = view.id;
    request.hash = view.hash;
    request.key_id = view.key_id;
    request.granted = {QStringLiteral("ports.read")};
    request.switch_on = true;
    backend.decide(request);
    // A computer that cannot isolate a plugin does not run an unsigned one
    // on the decision alone: it waits until the person lets this plugin run
    // without isolation.
    REQUIRE(vss_test::wait_until(
        [&]
        {
            return state_is(plugin_state::available) ||
                   state_is(plugin_state::needs_isolation);
        },
        60000));
    if (state_is(plugin_state::needs_isolation))
    {
        const plugin_view waiting = *find_view(backend.snapshot(), id);
        REQUIRE(waiting.isolation == plugin_isolation::none);
        REQUIRE(waiting.note == plugin_note::isolation_override);
        REQUIRE(waiting.override_allowed);
        REQUIRE(!waiting.override_active);
        backend.set_no_isolation(view.id, view.hash, true);
    }
    const bool running = vss_test::wait_until(
        [&] { return state_is(plugin_state::available); }, 60000);
    if (!running)
    {
        // Say what the page saw, to find out why.
        const plugin_view* now = find_view(backend.snapshot(), id);
        if (now != nullptr)
            std::fprintf(stderr, "state %d note %d reason %s detail %s\n",
                         static_cast<int>(now->state),
                         static_cast<int>(now->note),
                         now->reason_code.toUtf8().constData(),
                         now->detail.toUtf8().constData());
    }
    REQUIRE(running);
    view = *find_view(backend.snapshot(), id);
    REQUIRE(view.wished_on);
    REQUIRE(view.note == plugin_note::none);

    // A decision for a version that is not there stores nothing and runs
    // nothing.
    plugin_decision_request stale = request;
    stale.hash = QString(128, QLatin1Char('0'));
    backend.decide(stale);

    // Disable: it stops, and the page shows it Off.
    backend.set_enabled(view.id, view.hash, false);
    REQUIRE(vss_test::wait_until([&] { return state_is(plugin_state::off); },
                                 60000));
    REQUIRE(!find_view(backend.snapshot(), id)->wished_on);

    // Enable again, and it runs again.
    backend.set_enabled(view.id, view.hash, true);
    REQUIRE(vss_test::wait_until(
        [&] { return state_is(plugin_state::available); }, 60000));

    // Remove it, with its saved data: it is stopped first, then the folder
    // goes, then the data.
    const fs::path saved = root / "data" / kPluginId;
    {
        std::error_code ignored;
        fs::create_directories(saved / "data", ignored);
        write_file(saved / "data" / "note.txt", "kept until removed");
    }
    backend.remove_plugin(view.id, view.hash, true);
    REQUIRE(vss_test::wait_until(
        [&] { return find_view(backend.snapshot(), id) == nullptr; }, 60000));
    REQUIRE(vss_test::wait_until([&] { return !fs::exists(plugins / kPluginId); },
                                 60000));
    REQUIRE(vss_test::wait_until([&] { return !fs::exists(saved); }, 60000));

    // The end of the program: every plugin is stopped, and the call returns.
    backend.shut_down();
}

namespace
{

service_plugin_options options_for(const fs::path& root,
                                   const fs::path& plugins)
{
    service_plugin_options options;
    plug::plugin_root user;
    user.directory = plugins;
    user.scope = plug::plugin_scope::for_user;
    options.roots.push_back(user);
    options.program_directory =
        fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM)).parent_path();
    options.trust_file = root / "settings" / "trust.json";
    options.data_root = root / "data";
    options.user_root = plugins;
    options.seed = 1;
    return options;
}

/// The arguments that make the test plugin ask Studio to show a code as soon
/// as it is ready.
std::string ask_for_code()
{
    return R"(, "--send-header={\"op\":\"secret.show\",\"label\":\"Pairing code\",)"
           R"(\"text\":\"123456\",\"expires_s\":60}")";
}

/// After a decision, a computer that cannot isolate a plugin keeps an
/// unsigned one waiting until the person lets it run without isolation (Linux
/// and macOS give none, and so does a Windows without the isolation the
/// plugin needs). Says that for the test plugin, as the person would.
void allow_without_isolation(ServicePluginBackend& backend,
                             const plugin_view& view)
{
    const QString id = view.id;
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const plugin_view* now = find_view(backend.snapshot(), id);
            return now != nullptr &&
                   (now->state == plugin_state::available ||
                    now->state == plugin_state::needs_isolation);
        },
        60000));
    const plugin_view* now = find_view(backend.snapshot(), id);
    REQUIRE(now != nullptr);
    if (now->state == plugin_state::needs_isolation)
        backend.set_no_isolation(view.id, view.hash, true);
}

/// Decides about the plugin on the page's behalf and waits for it to run.
void run_the_plugin(ServicePluginBackend& backend, const QString& id)
{
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const plugin_view* view = find_view(backend.snapshot(), id);
            return view != nullptr &&
                   view->state == plugin_state::needs_decision &&
                   !view->adds.empty();
        },
        30000));
    const plugin_view view = *find_view(backend.snapshot(), id);
    plugin_decision_request request;
    request.plugin_id = view.id;
    request.hash = view.hash;
    request.key_id = view.key_id;
    request.granted = {QStringLiteral("ports.read")};
    request.switch_on = true;
    backend.decide(request);
    allow_without_isolation(backend, view);
}

} // namespace

TEST_CASE("service backend: a code a plugin asks to show reaches the page and "
          "is answered once")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    make_plugin(plugins, ask_for_code());
    ServicePluginBackend backend(options_for(root, plugins));

    std::vector<plugin_secret_request> asked;
    QObject::connect(&backend, &PluginBackend::secret_requested, &backend,
                     [&asked](const plugin_secret_request& request)
                     { asked.push_back(request); });
    run_the_plugin(backend, QString::fromLatin1(kPluginId));
    const bool came =
        vss_test::wait_until([&] { return !asked.empty(); }, 60000);
    if (!came)
    {
        const plugin_view* now =
            find_view(backend.snapshot(), QString::fromLatin1(kPluginId));
        if (now != nullptr)
        {
            std::fprintf(stderr, "SECRET state %d reason %s\n",
                         static_cast<int>(now->state),
                         now->reason_code.toUtf8().constData());
            for (const plugin_log_line& line : now->log)
                std::fprintf(stderr, "SECRET log: %s\n",
                             line.text.toUtf8().constData());
        }
    }
    REQUIRE(came);
    REQUIRE_EQ(asked.size(), std::size_t{1});
    REQUIRE_EQ(asked[0].plugin_id, QString::fromLatin1(kPluginId));
    REQUIRE_EQ(asked[0].plugin_name, QStringLiteral("Served"));
    REQUIRE_EQ(asked[0].label, QStringLiteral("Pairing code"));
    REQUIRE_EQ(asked[0].text, QStringLiteral("123456"));
    REQUIRE_EQ(asked[0].seconds, 60);
    REQUIRE(asked[0].id != 0);
    // The answer goes to the plugin; a second answer for it is ignored.
    backend.answer_secret(asked[0].id, true);
    backend.answer_secret(asked[0].id, false);
    // The code is in no snapshot and no log line the page can read.
    const plugin_view* view =
        find_view(backend.snapshot(), QString::fromLatin1(kPluginId));
    REQUIRE(view != nullptr);
    for (const plugin_log_line& line : view->log)
        REQUIRE(!line.text.contains(QStringLiteral("123456")));
    REQUIRE(!view->status_text.contains(QStringLiteral("123456")));
    backend.shut_down();
}

TEST_CASE("service backend: with nobody to show a code to, the plugin is told "
          "no, and shutting down does not wait on a dialog")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    make_plugin(plugins, ask_for_code());
    ServicePluginBackend backend(options_for(root, plugins));
    run_the_plugin(backend, QString::fromLatin1(kPluginId));
    // No dialog is connected: the request is answered no, and the plugin
    // carries on running.
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const plugin_view* view =
                find_view(backend.snapshot(), QString::fromLatin1(kPluginId));
            return view != nullptr && view->state == plugin_state::available;
        },
        60000));
    // Ending the program does not wait on a plugin or on a dialog.
    const auto started = std::chrono::steady_clock::now();
    backend.shut_down();
    REQUIRE(std::chrono::steady_clock::now() - started <
            std::chrono::seconds(20));
}

TEST_CASE("service backend: a control session a plugin asks for reaches the "
          "page with the plugin's words, and nothing is remembered")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    make_plugin(
        plugins,
        R"(, "--send-header={\"op\":\"control.session.request\",)"
        R"(\"scopes\":[\"monitor\",\"control.data\"],\"peer\":\"Alex\",)"
        R"(\"reason\":\"To run a test.\"}")");
    ServicePluginBackend backend(options_for(root, plugins));

    std::vector<plugin_control_request> asked;
    QObject::connect(&backend, &PluginBackend::control_requested, &backend,
                     [&asked](const plugin_control_request& request)
                     { asked.push_back(request); });
    run_the_plugin(backend, QString::fromLatin1(kPluginId));
    const bool arrived =
        vss_test::wait_until([&] { return !asked.empty(); }, 60000);
    if (!arrived)
    {
        const plugin_view* now =
            find_view(backend.snapshot(), QString::fromLatin1(kPluginId));
        if (now != nullptr)
        {
            std::fprintf(stderr, "state %d reason %s\n",
                         static_cast<int>(now->state),
                         now->reason_code.toUtf8().constData());
            for (const plugin_log_line& line : now->log)
                std::fprintf(stderr, "log: %s\n",
                             line.text.toUtf8().constData());
        }
    }
    REQUIRE(arrived);
    REQUIRE_EQ(asked[0].plugin_name, QStringLiteral("Served"));
    REQUIRE_EQ(asked[0].peer, QStringLiteral("Alex"));
    REQUIRE_EQ(asked[0].reason, QStringLiteral("To run a test."));
    REQUIRE_EQ(asked[0].scopes.size(), 2);
    REQUIRE_EQ(asked[0].life_seconds, 24 * 3600);
    backend.answer_control(asked[0].id, false);
    backend.shut_down();
}

TEST_CASE(
    "service backend: instances are asked for, kept, and come back at the "
    "next start")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    make_plugin(plugins);
    const QString id = QString::fromLatin1(kPluginId);
    {
        service_plugin_options options = options_for(root, plugins);
        options.instances_file = root / "settings" / "instances.json";
        ServicePluginBackend backend(std::move(options));
        run_the_plugin(backend, id);
        REQUIRE(vss_test::wait_until(
            [&]
            {
                const plugin_view* view = find_view(backend.snapshot(), id);
                return view != nullptr &&
                       view->state == plugin_state::available;
            },
            60000));
        backend.add_instance(id, QStringLiteral("pulse"), {});
        REQUIRE(vss_test::wait_until(
            [&]
            {
                const plugin_view* view = find_view(backend.snapshot(), id);
                return view != nullptr && view->instances.size() == 1;
            },
            60000));
        const plugin_instance_view made =
            find_view(backend.snapshot(), id)->instances.front();
        REQUIRE_EQ(made.number, quint64{1});
        REQUIRE_EQ(made.point_id, QStringLiteral("pulse"));
        REQUIRE_EQ(made.point_name, QStringLiteral("Pulse"));
        // A point the plugin does not declare is not made either.
        backend.add_instance(id, QStringLiteral("nothing"), {});
        backend.shut_down();
    }
    REQUIRE(fs::exists(root / "settings" / "instances.json"));
    {
        // The next start: the instance is there before the plugin runs.
        service_plugin_options options = options_for(root, plugins);
        options.instances_file = root / "settings" / "instances.json";
        ServicePluginBackend backend(std::move(options));
        const bool back = vss_test::wait_until(
            [&]
            {
                const plugin_view* view = find_view(backend.snapshot(), id);
                return view != nullptr && view->instances.size() == 1;
            },
            60000);
        if (!back)
        {
            const plugin_view* now = find_view(backend.snapshot(), id);
            if (now != nullptr)
                std::fprintf(stderr, "state %d instances %d\n",
                             static_cast<int>(now->state),
                             static_cast<int>(now->instances.size()));
        }
        REQUIRE(back);
        const plugin_view* view = find_view(backend.snapshot(), id);
        REQUIRE(view != nullptr);
        REQUIRE_EQ(view->instances.size(), std::size_t{1});
        // The plugin is not running, so the instance is not either.
        REQUIRE(view->instances.front().state ==
                plugin_instance_state::not_running);
        // Taking it away is kept too.
        backend.remove_instance(id, view->instances.front().number);
        REQUIRE(vss_test::wait_until(
            [&]
            {
                const plugin_view* now = find_view(backend.snapshot(), id);
                return now != nullptr && now->instances.empty();
            },
            60000));
        backend.shut_down();
    }
}

namespace
{

/// Whether a log line of the plugin has `text`.
bool log_has(ServicePluginBackend& backend, const QString& id,
             const QString& text)
{
    const plugin_view* view = find_view(backend.snapshot(), id);
    if (view == nullptr)
        return false;
    for (const plugin_log_line& line : view->log)
    {
        if (line.text.contains(text))
            return true;
    }
    return false;
}

void show_log(ServicePluginBackend& backend, const QString& id)
{
    const plugin_view* view = find_view(backend.snapshot(), id);
    if (view == nullptr)
        return;
    for (const plugin_log_line& line : view->log)
        std::fprintf(stderr, "log: %s\n", line.text.toUtf8().constData());
}

/// The test plugin asks for the licence file as soon as it is ready, and asks
/// for `licence.read` as well as the permission every test plugin has.
void ask_for_licence(const fs::path& plugins)
{
    make_plugin(plugins, R"(, "--send-header={\"op\":\"licence.get\"}")",
                R"(, { "name": "licence.read", "required": false, )"
                R"("reason": "To check it." })");
}

void decide_with(ServicePluginBackend& backend, const QString& id,
                 const QStringList& granted)
{
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const plugin_view* view = find_view(backend.snapshot(), id);
            return view != nullptr &&
                   view->state == plugin_state::needs_decision &&
                   !view->adds.empty();
        },
        30000));
    const plugin_view view = *find_view(backend.snapshot(), id);
    plugin_decision_request request;
    request.plugin_id = view.id;
    request.hash = view.hash;
    request.key_id = view.key_id;
    request.granted = granted;
    request.switch_on = true;
    backend.decide(request);
    allow_without_isolation(backend, view);
}

vss::license::status a_valid_licence()
{
    vss::license::status status;
    status.state = vss::license::state::valid;
    status.reason = vss::license::reason::ok;
    return status;
}

} // namespace

TEST_CASE("service backend: a plugin allowed to read the licence is handed the "
          "file Studio uses, and one that is not is refused")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    const fs::path licences = root / "licences";
    std::error_code ignored;
    fs::create_directories(licences, ignored);
    write_file(licences / "studio.vsslic", "the bytes of a licence file");
    ask_for_licence(plugins);
    const QString id = QString::fromLatin1(kPluginId);

    {
        service_plugin_options options = options_for(root, plugins);
        options.licence_user_directory = licences;
        ServicePluginBackend backend(std::move(options));
        backend.set_licence(a_valid_licence(),
                            licence_file_ref{"studio.vsslic", false});
        decide_with(
            backend, id,
            {QStringLiteral("ports.read"), QStringLiteral("licence.read")});
        const bool answered = vss_test::wait_until(
            [&] { return log_has(backend, id, QStringLiteral("answer ")); },
            60000);
        if (!answered || log_has(backend, id, QStringLiteral("answer error")))
            show_log(backend, id);
        REQUIRE(answered);
        // The answer is the file, not a refusal.
        REQUIRE(!log_has(backend, id, QStringLiteral("answer error")));
        backend.shut_down();
    }
    {
        // Not allowed to read it, and not official: refused.
        fs::remove_all(root / "settings", ignored);
        service_plugin_options options = options_for(root, plugins);
        options.licence_user_directory = licences;
        ServicePluginBackend backend(std::move(options));
        backend.set_licence(a_valid_licence(),
                            licence_file_ref{"studio.vsslic", false});
        decide_with(backend, id, {QStringLiteral("ports.read")});
        const bool answered = vss_test::wait_until(
            [&]
            { return log_has(backend, id, QStringLiteral("answer error")); },
            60000);
        if (!answered)
            show_log(backend, id);
        REQUIRE(answered);
        backend.shut_down();
    }
}

TEST_CASE("service backend: the licence is the same one until it changes")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    make_plugin(plugins);
    ServicePluginBackend backend(options_for(root, plugins));
    // Telling the plugins of the same licence again, and of none, does not
    // stop anything or fail.
    backend.set_licence(a_valid_licence(), licence_file_ref{"a.vsslic", false});
    backend.set_licence(a_valid_licence(), licence_file_ref{"a.vsslic", false});
    backend.set_licence(vss::license::no_licence(), std::nullopt);
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const plugin_view* view =
                find_view(backend.snapshot(), QString::fromLatin1(kPluginId));
            return view != nullptr &&
                   view->state == plugin_state::needs_decision;
        },
        30000));
    backend.shut_down();
}

TEST_CASE("service backend: what a plugin reports of its licence check reaches "
          "the page")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    make_plugin(plugins, R"(, "--notify-header={\"op\":\"licence.report\",)"
                         R"(\"reason\":\"expired\"}")");
    ServicePluginBackend backend(options_for(root, plugins));
    const QString id = QString::fromLatin1(kPluginId);
    run_the_plugin(backend, id);
    const bool reported = vss_test::wait_until(
        [&]
        {
            const plugin_view* view = find_view(backend.snapshot(), id);
            return view != nullptr && view->has_licence_report;
        },
        60000);
    REQUIRE(reported);
    const plugin_view* view = find_view(backend.snapshot(), id);
    REQUIRE(view != nullptr);
    REQUIRE(view->licence_report_reason == vss::license::reason::expired);
    backend.shut_down();
}

TEST_CASE(
    "service backend: an instance of a point with a required port is made "
    "only when the port is given")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    make_plugin(plugins, {}, {},
                R"(, "parameters": [ { "id": "port", "type": "port", )"
                R"("label": "Port" } ])");
    const QString id = QString::fromLatin1(kPluginId);
    service_plugin_options options = options_for(root, plugins);
    options.instances_file = root / "settings" / "instances.json";
    ServicePluginBackend backend(std::move(options));
    run_the_plugin(backend, id);
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const plugin_view* view = find_view(backend.snapshot(), id);
            return view != nullptr && view->state == plugin_state::available &&
                   !view->adds.empty();
        },
        60000));
    // The page is told what to ask for.
    const plugin_view view = *find_view(backend.snapshot(), id);
    REQUIRE_EQ(view.adds.front().asks.size(), std::size_t{1});
    REQUIRE_EQ(view.adds.front().asks.front().id, QStringLiteral("port"));
    REQUIRE(view.adds.front().asks.front().place_name);
    REQUIRE(!view.adds.front().needs_settings);

    // Without the port nothing is made; with it, one is.
    backend.add_instance(id, QStringLiteral("pulse"), {});
    backend.add_instance(id, QStringLiteral("pulse"),
                         {{QStringLiteral("port"), QStringLiteral("Alpha")}});
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const plugin_view* now = find_view(backend.snapshot(), id);
            return now != nullptr && now->instances.size() == 1;
        },
        60000));
    backend.shut_down();
    const std::string kept = read_file(root / "settings" / "instances.json");
    REQUIRE(kept.find("Alpha") != std::string::npos);
}

TEST_CASE("service backend: the same plugin installed for everyone and for the "
          "user shows both copies, names what an add refuses, and removing "
          "the user's copy leaves the other one")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path everyone = root / "everyone";
    const fs::path plugins = root / "plugins";
    const fs::path incoming = root / "incoming";
    make_plugin(everyone);
    make_plugin(plugins);
    const fs::path source = make_plugin(incoming);

    service_plugin_options options = options_for(root, plugins);
    // Read in the order of the real computer: for everyone, then the user.
    plug::plugin_root machine;
    machine.directory = everyone;
    machine.scope = plug::plugin_scope::for_everyone;
    options.roots.insert(options.roots.begin(), machine);
    ServicePluginBackend backend(std::move(options));
    const QString id = QString::fromLatin1(kPluginId);

    std::vector<plugin_request_result> finished;
    QObject::connect(&backend, &PluginBackend::request_finished, &backend,
                     [&finished](const plugin_request_result& result)
                     { finished.push_back(result); });
    std::vector<plugin_inspection_view> inspected;
    QObject::connect(&backend, &PluginBackend::plugin_inspected, &backend,
                     [&inspected](quint64, const plugin_inspection_view& found)
                     { inspected.push_back(found); });

    const auto copies_of = [&]
    {
        std::vector<plugin_view> found;
        for (const plugin_view& each : backend.snapshot()->plugins)
        {
            if (each.id == id)
                found.push_back(each);
        }
        return found;
    };
    REQUIRE(vss_test::wait_until([&] { return copies_of().size() == 2; },
                                 30000));

    // Both are listed. The copy for everyone is read first, so it is the one
    // that runs, and the user's copy says so and where the other one is.
    std::vector<plugin_view> both = copies_of();
    const plugin_view& used = both[0].installed_for ==
                                      plugin_installed_for::everyone
                                  ? both[0]
                                  : both[1];
    const plugin_view& unused = both[0].installed_for ==
                                        plugin_installed_for::everyone
                                    ? both[1]
                                    : both[0];
    REQUIRE(used.state != plugin_state::not_used);
    REQUIRE(unused.installed_for == plugin_installed_for::you_only);
    REQUIRE(unused.state == plugin_state::not_used);
    REQUIRE(unused.has_used_copy);
    REQUIRE(unused.used_copy_installed_for == plugin_installed_for::everyone);
    REQUIRE_EQ(unused.used_copy_id, id);
    // The two are identical, which is the case that Remove must not mix up.
    REQUIRE_EQ(used.hash, unused.hash);

    // Adding the same plugin: the refusal is about the copy for everyone and
    // has the plugin's name.
    backend.inspect_plugin(QString::fromStdWString(source.wstring()));
    REQUIRE(vss_test::wait_until([&] { return !inspected.empty(); }, 30000));
    REQUIRE(inspected[0].failure ==
            plugin_inspection_view::problem::already_installed);
    backend.add_plugin(QString::fromStdWString(source.wstring()), false);
    REQUIRE(vss_test::wait_until([&] { return !finished.empty(); }, 30000));
    REQUIRE(finished[0].kind == plugin_request_kind::add_plugin);
    REQUIRE(finished[0].error == plugin_request_error::already_installed);
    REQUIRE_EQ(finished[0].plugin_name, QStringLiteral("Served"));
    finished.clear();

    // Remove the user's copy: its folder goes, the copy for everyone stays,
    // is listed and is not marked unused. Its saved data stays too.
    const fs::path saved = root / "data" / kPluginId;
    {
        std::error_code ignored;
        fs::create_directories(saved, ignored);
        write_file(saved / "note.txt", "used by the copy for everyone");
    }
    backend.remove_plugin(unused.id, unused.hash, true);
    REQUIRE(vss_test::wait_until(
        [&] { return !fs::exists(plugins / kPluginId); }, 60000));
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const std::vector<plugin_view> left = copies_of();
            return left.size() == 1 &&
                   left[0].installed_for == plugin_installed_for::everyone &&
                   left[0].state != plugin_state::not_used;
        },
        60000));
    REQUIRE(fs::exists(everyone / kPluginId));
    REQUIRE(fs::exists(saved / "note.txt"));

    // With only the copy for everyone left, adding the plugin for the user
    // is refused, and the refusal says which place has it.
    REQUIRE(vss_test::wait_until(
        [&]
        {
            return std::any_of(finished.begin(), finished.end(),
                               [](const plugin_request_result& each)
                               {
                                   return each.kind ==
                                          plugin_request_kind::remove_plugin;
                               });
        },
        30000));
    const std::size_t finished_before = finished.size();
    backend.add_plugin(QString::fromStdWString(source.wstring()), false);
    plugin_request_result refusal;
    REQUIRE(vss_test::wait_until(
        [&]
        {
            if (finished.size() <= finished_before)
                return false;
            refusal = finished.back();
            return true;
        },
        30000));
    REQUIRE(refusal.kind == plugin_request_kind::add_plugin);
    REQUIRE(refusal.error == plugin_request_error::installed_for_everyone);
    REQUIRE_EQ(refusal.plugin_name, QStringLiteral("Served"));
    REQUIRE(!fs::exists(plugins / kPluginId));
    const std::size_t inspected_before = inspected.size();
    backend.inspect_plugin(QString::fromStdWString(source.wstring()));
    REQUIRE(vss_test::wait_until(
        [&] { return inspected.size() > inspected_before; }, 30000));
    const auto inspection = inspected.back();
    REQUIRE(inspection.failure ==
            plugin_inspection_view::problem::installed_for_everyone);
    REQUIRE(!inspection.can_install);
    backend.shut_down();
}

TEST_CASE("service backend: another version of a plugin the user has replaces "
          "it once the program has stopped, and the saved data stays")
{
    QTemporaryDir dir;
    const fs::path root = fs::path(dir.path().toStdWString());
    const fs::path plugins = root / "plugins";
    const fs::path incoming = root / "incoming";
    make_plugin(plugins);
    const fs::path newer = make_plugin(incoming, {}, {}, {}, "1.3.0");
    const QString newer_text = QString::fromStdWString(newer.wstring());

    ServicePluginBackend backend(options_for(root, plugins));
    const QString id = QString::fromLatin1(kPluginId);
    // Only the answers to adding: deciding and running the plugin finish
    // requests of their own.
    std::vector<plugin_request_result> finished;
    QObject::connect(&backend, &PluginBackend::request_finished, &backend,
                     [&finished](const plugin_request_result& result)
                     {
                         if (result.kind == plugin_request_kind::add_plugin)
                             finished.push_back(result);
                     });
    std::vector<plugin_inspection_view> inspected;
    QObject::connect(&backend, &PluginBackend::plugin_inspected, &backend,
                     [&inspected](quint64, const plugin_inspection_view& found)
                     { inspected.push_back(found); });
    const auto version_is = [&](const char* wanted)
    {
        const plugin_view* view = find_view(backend.snapshot(), id);
        return view != nullptr && view->version == QLatin1String(wanted);
    };

    // The user's copy runs, and has saved data.
    run_the_plugin(backend, id);
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const plugin_view* view = find_view(backend.snapshot(), id);
            return view != nullptr && view->state == plugin_state::available;
        },
        60000));
    const fs::path saved = root / "data" / kPluginId;
    {
        std::error_code ignored;
        fs::create_directories(saved, ignored);
        write_file(saved / "note.txt", "kept through the upgrade");
    }

    // The folder is another version of that plugin: it replaces the copy.
    backend.inspect_plugin(newer_text);
    REQUIRE(vss_test::wait_until([&] { return !inspected.empty(); }, 30000));
    REQUIRE(inspected[0].failure == plugin_inspection_view::problem::none);
    REQUIRE(inspected[0].can_install);
    REQUIRE(inspected[0].replaces == plugin_replacement::upgrade);
    REQUIRE_EQ(inspected[0].installed_version, QStringLiteral("1.2.0"));
    REQUIRE_EQ(inspected[0].plugin.version, QStringLiteral("1.3.0"));

    // A request that was not asked to replace changes nothing.
    backend.add_plugin(newer_text, false);
    REQUIRE(vss_test::wait_until([&] { return !finished.empty(); }, 30000));
    REQUIRE(finished[0].kind == plugin_request_kind::add_plugin);
    REQUIRE(finished[0].error == plugin_request_error::already_installed);
    REQUIRE(read_file(plugins / kPluginId / "plugin.json")
                .find("\"version\": \"1.2.0\"") != std::string::npos);
    REQUIRE(version_is("1.2.0"));
    finished.clear();

    // Asked to replace: the program is stopped first, then the folder is
    // replaced, and the new version waits for a decision.
    backend.add_plugin(newer_text, true);
    plugin_request_result upgraded;
    REQUIRE(vss_test::wait_until(
        [&]
        {
            if (finished.empty())
                return false;
            upgraded = finished.back();
            return true;
        },
        60000));
    REQUIRE(upgraded.kind == plugin_request_kind::add_plugin);
    REQUIRE(upgraded.error == plugin_request_error::none);
    REQUIRE(upgraded.replaced == plugin_replacement::upgrade);
    REQUIRE_EQ(upgraded.plugin_name, QStringLiteral("Served"));
    REQUIRE(read_file(plugins / kPluginId / "plugin.json")
                .find("\"version\": \"1.3.0\"") != std::string::npos);
    REQUIRE(!fs::exists(plugins / ".vss-work"));
    REQUIRE(vss_test::wait_until(
        [&]
        {
            // Hold the snapshot while the view is read: a second call of
            // snapshot() can replace it and free what the view points into.
            const plugin_snapshot_ptr shot = backend.snapshot();
            const plugin_view* view = find_view(shot, id);
            return view != nullptr && view->version == QLatin1String("1.3.0") &&
                   view->state == plugin_state::needs_decision;
        },
        60000));
    REQUIRE(fs::exists(saved / "note.txt"));

    // The same files again are installed already.
    const std::size_t looked = inspected.size();
    backend.inspect_plugin(newer_text);
    REQUIRE(vss_test::wait_until([&] { return inspected.size() > looked; },
                                 30000));
    REQUIRE(inspected.back().failure ==
            plugin_inspection_view::problem::already_installed);
    REQUIRE(!inspected.back().can_install);
    REQUIRE(inspected.back().replaces == plugin_replacement::none);
    backend.shut_down();
}
