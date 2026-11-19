//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pieces the plugin backends share: what a reason code leads to, the
// checks on a public key file, and the fake backend that records requests.
// The real backend is the plugin service's adapter; its mapping and its run
// against a real service are in test_service_mapping.cpp and
// test_service_backend.cpp.
//

#include "backend/engine_studio_backend.hpp"
#include "backend/fake_plugin_backend.hpp"
#include "backend/fake_studio_backend.hpp"
#include "backend/plugin_mapping.hpp"
#include "backend/plugin_places.hpp"

#include <vss/license/sign.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/studio/studio.hpp>

#include "test_framework.hpp"

#include <QTemporaryDir>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace vss::gui;
namespace plug = vss::plugin;
namespace fs = std::filesystem;
namespace signing = vss::license::signing;

namespace
{

fs::path path_of(const QTemporaryDir& dir)
{
    return fs::path(dir.path().toStdWString());
}

void write_file(const fs::path& path, std::string_view content)
{
    std::error_code ignored;
    fs::create_directories(path.parent_path(), ignored);
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

signing::seed test_seed(std::uint8_t fill)
{
    signing::seed seed{};
    seed.fill(fill);
    return seed;
}

std::string hex_of(const plug::key_bytes& key)
{
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : key)
    {
        out += kDigits[byte >> 4];
        out += kDigits[byte & 0x0f];
    }
    return out;
}

constexpr std::uint8_t kAuthorFill = 0x04;

} // namespace

TEST_CASE("plugin mapping: every reason code leads to one group")
{
    using code = plug::plugin_errc;
    using state = plugin_state;
    struct row
    {
        code reason;
        state expected;
    };
    const row table[] = {
        {code::manifest_invalid, state::cannot_be_used},
        {code::manifest_too_large, state::cannot_be_used},
        {code::manifest_unsupported_version, state::needs_newer_studio},
        {code::too_many, state::cannot_be_used},
        {code::shadowed, state::not_used},
        {code::layout_invalid, state::cannot_be_used},
        {code::file_missing, state::cannot_be_used},
        {code::unlisted_file, state::cannot_be_used},
        {code::signature_missing, state::needs_decision},
        {code::signature_invalid, state::cannot_be_used},
        {code::key_unknown, state::needs_decision},
        {code::changed, state::needs_decision},
        {code::revoked, state::cannot_be_used},
        {code::expired, state::cannot_be_used},
        {code::policy, state::blocked},
        {code::not_licensed, state::needs_licence},
        {code::api_incompatible, state::needs_newer_studio},
        {code::capability_missing, state::needs_newer_studio},
        {code::reserved_id, state::not_used},
        {code::unresponsive, state::stopped},
        {code::misbehaving, state::stopped},
        {code::crashed, state::stopped},
        {code::exited, state::stopped},
        {code::start_failed, state::stopped},
        {code::quarantined, state::stopped},
    };
    for (const row& each : table)
        REQUIRE(state_for_reason(each.reason, false).state == each.expected);
    // The isolation reason depends on whether the user may override it.
    REQUIRE(state_for_reason(code::sandbox_insufficient, true).state ==
            state::needs_isolation);
    REQUIRE(state_for_reason(code::sandbox_insufficient, false).state ==
            state::blocked);
}

TEST_CASE("plugin mapping: a key file is read, checked and compared")
{
    QTemporaryDir dir;
    const fs::path folder = path_of(dir) / "plugins" / "tool";
    fs::create_directories(folder);
    key_check_context context;
    context.plugin_folders = {folder};
    plug::trust_store store;

    const std::string public_key =
        hex_of(signing::public_key_of(test_seed(kAuthorFill)));
    const auto key_json = [&](std::string_view id, std::string_view name)
    {
        return std::string(R"({ "key_id": ")") + std::string(id) +
               R"(", "public_key": ")" + public_key + R"(", "name": ")" +
               std::string(name) + R"(" })";
    };

    const fs::path good = path_of(dir) / "author.vsskey.json";
    write_file(good, key_json("author-1", "Author Name"));
    plug::approved_key approved;
    plugin_key_file_view view =
        read_key_file_view(good, context, store, &approved);
    REQUIRE(view.failure == plugin_key_file_view::problem::none);
    REQUIRE_EQ(view.author, QStringLiteral("Author Name"));
    REQUIRE_EQ(view.key_id, QStringLiteral("author-1"));
    REQUIRE_EQ(view.fingerprint.size(), 16);
    REQUIRE(!view.inside_plugin_folder);
    REQUIRE_EQ(approved.key_id, std::string("author-1"));

    // Inside a plugin's own folder: the page must say so.
    const fs::path inside = folder / "author.vsskey.json";
    write_file(inside, key_json("author-1", "Author Name"));
    view = read_key_file_view(inside, context, store, nullptr);
    REQUIRE(view.failure == plugin_key_file_view::problem::none);
    REQUIRE(view.inside_plugin_folder);

    // Not a key file.
    const fs::path junk = path_of(dir) / "junk.vsskey.json";
    write_file(junk, "not json at all");
    view = read_key_file_view(junk, context, store, nullptr);
    REQUIRE(view.failure == plugin_key_file_view::problem::not_a_key_file);

    // A name with a direction override is refused.
    const fs::path rough = path_of(dir) / "rough.vsskey.json";
    write_file(rough, key_json("author-1", "Author\\u202eName"));
    view = read_key_file_view(rough, context, store, nullptr);
    REQUIRE(view.failure == plugin_key_file_view::problem::text_not_plain);

    // The id of an approved key never changes its key.
    plug::approved_key taken;
    taken.key_id = "author-1";
    taken.public_key = signing::public_key_of(test_seed(0x09));
    taken.name = "Somebody else";
    taken.approved_on = std::chrono::year_month_day{
        std::chrono::year{2026}, std::chrono::month{11}, std::chrono::day{14}};
    REQUIRE(store.approve_key(taken).has_value());
    view = read_key_file_view(good, context, store, nullptr);
    REQUIRE(view.failure == plugin_key_file_view::problem::id_in_use);
}

TEST_CASE("plugin backend: a burst of changes is announced once, and every "
          "request is recorded")
{
    FakePluginBackend backend;
    int announcements = 0;
    QObject::connect(&backend, &PluginBackend::snapshot_changed, &backend,
                     [&announcements] { ++announcements; });
    for (int index = 0; index < 10; ++index)
        backend.set_plugins(fake_plugin_samples());
    REQUIRE_EQ(announcements, 0);
    backend.flush();
    REQUIRE_EQ(announcements, 1);
    REQUIRE_EQ(backend.snapshot()->plugins.size(), fake_plugin_samples().size());

    backend.rescan();
    backend.set_enabled(QStringLiteral("a"), QStringLiteral("b"), true);
    REQUIRE_EQ(backend.requests().size(), std::size_t{2});
    REQUIRE(backend.requests()[1].kind == plugin_request_kind::set_enabled);
    REQUIRE(backend.requests()[1].flag);
}

TEST_CASE("plugin places: ports by name, links by the names of their ends")
{
    FakeStudioBackend studio;
    const auto first = studio.create_virtual_port(
        QStringLiteral("Alpha"), vss::virtual_port_kind::loopback, false);
    const auto second = studio.create_virtual_port(
        QStringLiteral("Beta"), vss::virtual_port_kind::loopback, false);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(studio.connect_ports(*first, *second, {}).has_value());
    const auto table = make_plugin_place_table(studio);
    REQUIRE(table != nullptr);
    const std::vector<port_view> ports = studio.ports();
    REQUIRE(!ports.empty());
    for (const port_view& port : ports)
    {
        const auto found = table->find(port.name.toStdString());
        REQUIRE(found != table->end());
        REQUIRE(found->second.kind == vss::plugin::host::place_kind::port);
        REQUIRE_EQ(found->second.tap.endpoints.size(), std::size_t{1});
        REQUIRE_EQ(found->second.tap.endpoints[0], port.id);
        REQUIRE_EQ(found->second.settings, vss::to_label(port.serial));
    }
    int links = 0;
    for (const link_view& link : studio.links())
    {
        if (link.sources.isEmpty() || link.targets.isEmpty())
            continue;
        ++links;
        bool found_link = false;
        for (const auto& each : *table)
        {
            if (each.second.kind == vss::plugin::host::place_kind::link &&
                each.second.tap.link == link.id)
            {
                found_link = true;
                REQUIRE(each.first.find(" - ") != std::string::npos);
                REQUIRE_EQ(each.second.tap.endpoints.size(),
                           static_cast<std::size_t>(link.sources.size() +
                                                    link.targets.size()));
            }
        }
        REQUIRE(found_link);
    }
    REQUIRE(links > 0);
    // A name nobody has is not there.
    REQUIRE(table->find(std::string("no such place")) == table->end());
}

TEST_CASE("plugin places: the ports a plugin may write to are the hub's own "
          "endpoints, by name")
{
    vss::studio_options options;
    vss::studio studio(options);
    EngineStudioBackend backend(studio);
    const auto first = backend.create_virtual_port(
        QStringLiteral("Alpha"), vss::virtual_port_kind::loopback, false);
    const auto second = backend.create_virtual_port(
        QStringLiteral("Beta"), vss::virtual_port_kind::loopback, false);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const auto table = make_plugin_endpoint_table(backend, studio.engine());
    REQUIRE(table != nullptr);
    REQUIRE_EQ(table->size(), std::size_t{2});
    const auto alpha = table->find(std::string("Alpha"));
    REQUIRE(alpha != table->end());
    REQUIRE(alpha->second == studio.engine().find_endpoint(*first));
    REQUIRE(table->find(std::string("Beta"))->second ==
            studio.engine().find_endpoint(*second));
    REQUIRE(table->find(std::string("Gamma")) == table->end());
}
