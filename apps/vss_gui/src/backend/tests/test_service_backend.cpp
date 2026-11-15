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

#include <QTemporaryDir>

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

/// A plugin folder `root/<id>`: the test plugin program and a `plugin.json`
/// that lists it, as the system tests lay one out.
fs::path make_plugin(const fs::path& root)
{
    const fs::path program = fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM));
    const fs::path directory = root / kPluginId;
    std::error_code ec;
    fs::create_directories(directory, ec);
    REQUIRE(!ec);
    const fs::path copy = directory / program.filename();
    fs::copy_file(program, copy, fs::copy_options::overwrite_existing, ec);
    REQUIRE(!ec);
    const std::string bytes = read_file(copy);
    const std::string name = copy.filename().string();
    std::string manifest;
    manifest += "{\n  \"manifest_version\": 1,\n  \"id\": \"";
    manifest += kPluginId;
    manifest += "\",\n  \"name\": \"Served\",\n  \"version\": \"1.2.0\",\n";
    manifest += "  \"author\": { \"name\": \"Example Ltd\" },\n";
    manifest += "  \"requires\": { \"api\": { \"min\": \"1.0\", \"max\": "
                "\"1.x\" } },\n";
    manifest += "  \"run\": { \"executable\": \"" + name +
                "\", \"args\": [ "
                "\"--hello-id=" +
                std::string(kPluginId) +
                "\", \"--hello-version=1.2.0\", \"--serve\" ],\n";
    manifest += "    \"platforms\": [ \"windows-x64\", \"windows-arm64\", "
                "\"linux-x64\", \"linux-arm64\", \"macos-x64\", "
                "\"macos-arm64\" ] },\n";
    manifest += "  \"permissions\": [ { \"name\": \"ports.read\", "
                "\"required\": true, \"reason\": \"To read the ports.\" } ],\n";
    manifest += "  \"extension_points\": [ { \"kind\": \"generator\", "
                "\"id\": \"pulse\", \"name\": \"Pulse\" } ],\n";
    manifest += "  \"files\": [ { \"path\": \"" + name +
                "\", \"size\": " + std::to_string(bytes.size()) +
                ", \"sha512\": \"" + plug::to_hex(plug::sha512_of(bytes)) +
                "\" } ]\n}\n";
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
