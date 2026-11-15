//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the plugin library's findings become the groups the Plugins page shows,
// for plugins made in a scratch directory, and the real plugin backend's
// requests on its worker thread. The signing keys here are throwaway test
// keys; the program never trusts them.
//

#include "backend/fake_plugin_backend.hpp"
#include "backend/library_plugin_backend.hpp"
#include "backend/plugin_mapping.hpp"

#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/signature.hpp>
#include <vss/plugin/trust_store.hpp>

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include <QTemporaryDir>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace vss::gui;
namespace plug = vss::plugin;
namespace fs = std::filesystem;
namespace signing = vss::license::signing;
namespace signed_file = vss::license::signed_file;
using namespace std::chrono_literals;

namespace
{

constexpr std::chrono::sys_days kToday{2026y / 11 / 14};

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

std::string listed(std::string_view path, std::string_view content)
{
    std::string out = "{ \"path\": \"";
    out += path;
    out += "\", \"size\": ";
    out += std::to_string(content.size());
    out += ", \"sha512\": \"";
    out += plug::to_hex(plug::sha512_of(content));
    out += "\" }";
    return out;
}

constexpr std::string_view kProgramText = "program bytes";

struct manifest_parts
{
    std::string id = "org.example.tool";
    std::string name = "Example tool";
    std::string version = "1.0.0";
    int manifest_version = 1;
    std::string api_min = "1.0";
    std::string extra_requires;
    std::string not_after;
    std::string permissions;
    bool list_files = false;
    std::string program = std::string(kProgramText);
};

std::string manifest_text(const manifest_parts& parts)
{
    std::string out = "{\n  \"manifest_version\": ";
    out += std::to_string(parts.manifest_version);
    out += ",\n  \"id\": \"" + parts.id + "\",\n";
    out += "  \"name\": \"" + parts.name + "\",\n";
    out += "  \"version\": \"" + parts.version + "\",\n";
    out += "  \"author\": { \"name\": \"Somebody\" },\n";
    if (!parts.not_after.empty())
        out += "  \"not_after\": \"" + parts.not_after + "\",\n";
    out += "  \"requires\": { \"api\": { \"min\": \"" + parts.api_min +
           "\", \"max\": \"2.x\" }" + parts.extra_requires + " },\n";
    out += "  \"run\": { \"executable\": \"bin/prog\", \"platforms\": "
           "[\"windows-x64\", \"linux-x64\", \"macos-arm64\", "
           "\"linux-arm64\", \"macos-x64\", \"windows-arm64\"] },\n";
    if (!parts.permissions.empty())
        out += "  \"permissions\": [ " + parts.permissions + " ],\n";
    out += "  \"extension_points\": [ { \"kind\": \"generator\", \"id\": "
           "\"pulse\", \"name\": \"Pulse\" } ]";
    if (parts.list_files)
        out += ",\n  \"files\": [ " + listed("bin/prog", parts.program) + " ]";
    out += "\n}\n";
    return out;
}

fs::path make_plugin(const fs::path& root, std::string_view folder,
                     const manifest_parts& parts)
{
    const fs::path directory = root / std::string(folder);
    write_file(directory / "plugin.json", manifest_text(parts));
    write_file(directory / "bin" / "prog", parts.program);
    return directory;
}

plug::plugin_root root_of(const fs::path& directory, plug::plugin_scope scope)
{
    plug::plugin_root out;
    out.directory = directory;
    out.scope = scope;
    return out;
}

plugin_gather_options
options_for(const fs::path& root,
            plug::plugin_scope scope = plug::plugin_scope::for_user,
            plug::install_kind install = plug::install_kind::for_one_user)
{
    plugin_gather_options options;
    options.roots.push_back(root_of(root, scope));
    options.install = install;
    options.today = kToday;
    options.studio_version = "0.1.0";
    return options;
}

const plugin_view* find_view(const plugin_gathered& found, std::string_view id)
{
    for (const plugin_view& each : found.snapshot.plugins)
    {
        if (each.id.toStdString() == id)
            return &each;
    }
    return nullptr;
}

plug::plugin_hash hash_of_view(const plugin_view& view)
{
    const auto parsed = parse_hash_text(view.hash);
    REQUIRE(parsed.has_value());
    return *parsed;
}

plug::plugin_decision decision_for(const plugin_view& view,
                                   bool enabled = true)
{
    plug::plugin_decision decision;
    decision.plugin_id = view.id.toStdString();
    decision.hash = hash_of_view(view);
    decision.level = plug::trust_level::unsigned_plugin;
    decision.enabled = enabled;
    decision.decided_on = std::chrono::year_month_day{2026y / 11 / 14};
    return decision;
}

// -- signing, with throwaway keys ---------------------------------------------

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

std::string signed_text(std::string_view purpose, std::string_view key_id,
                        std::string_view payload, std::uint8_t fill)
{
    std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    return signed_file::write(
        signing::sign(purpose, key_id, bytes, test_seed(fill)));
}

constexpr std::uint8_t kRootFill = 0x01;
constexpr std::uint8_t kPluginsFill = 0x02;
constexpr std::uint8_t kAuthorFill = 0x04;

std::string key_list_payload()
{
    return std::string(R"({ "key_list_version": 1, "serial": 4, )"
                       R"("issued": "2026-11-13", "keys": [ )") +
           R"({ "key_id": "plugins-2026", "public_key": ")" +
           hex_of(signing::public_key_of(test_seed(kPluginsFill))) +
           R"(", "purpose": "plugin", "name": "Virtual Serial Studio", )"
           R"("valid_from": "2026-11-01", "valid_to": "2028-11-01" } ], )"
           R"("revoked": [] })";
}

/// Signs the plugin in `directory` with the test key `fill` under `key_id`.
void sign_plugin(const fs::path& directory, const manifest_parts& parts,
                 std::string_view key_id, std::uint8_t fill)
{
    const std::string manifest = manifest_text(parts);
    const std::string claim = R"({ "plugin_id": ")" + parts.id +
                              R"(", "version": ")" + parts.version +
                              R"(", "manifest_sha512": ")" +
                              plug::to_hex(plug::sha512_of(manifest)) +
                              R"(" })";
    write_file(directory / "plugin.sig",
               signed_text(plug::plugin_purpose, key_id, claim, fill));
}

} // namespace

TEST_CASE("plugin mapping: an undecided plugin needs a decision, and a changed "
          "one says so")
{
    QTemporaryDir dir;
    const fs::path root = path_of(dir) / "plugins";
    manifest_parts parts;
    parts.permissions = R"({ "name": "ports.read", "required": true },)"
                        R"({ "name": "fs.write", "required": false, )"
                        R"("reason": "To save." })";
    const fs::path folder = make_plugin(root, "tool", parts);

    plugin_gather_options options = options_for(root);
    options.sandbox = plug::sandbox_level::full;
    plug::trust_store store;

    plugin_gathered found = gather_plugins(options, store);
    const plugin_view* view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    REQUIRE(view->state == plugin_state::needs_decision);
    REQUIRE(view->note == plugin_note::decision_first);
    REQUIRE_EQ(view->reason_code, QStringLiteral("untrusted"));
    REQUIRE(view->trust == plugin_trust::unsigned_plugin);
    REQUIRE_EQ(view->fingerprint.size(), 16);
    REQUIRE_EQ(view->hash.size(), 128);
    REQUIRE(view->installed_for == plugin_installed_for::you_only);
    REQUIRE_EQ(view->permissions.size(), std::size_t{2});
    REQUIRE(!found.snapshot.can_run_plugins);

    // After the user's decision the plugin may run, and this build cannot run
    // it: it is Off, and the wish is kept.
    REQUIRE(store.decide(decision_for(*view)).has_value());
    found = gather_plugins(options, store);
    view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    REQUIRE(view->state == plugin_state::off);
    REQUIRE(view->wished_on);
    // An optional permission the user did not grant is shown as turned down.
    bool turned_down = false;
    for (const plugin_permission_view& each : view->permissions)
        turned_down = turned_down || (each.id == QStringLiteral("fs.write") &&
                                      each.turned_down);
    REQUIRE(turned_down);

    // The plugin changes: a new hash asks again, and says it changed.
    write_file(folder / "bin" / "prog", "other program bytes");
    found = gather_plugins(options, store);
    view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    REQUIRE(view->state == plugin_state::needs_decision);
    REQUIRE(view->note == plugin_note::decision_changed);
    REQUIRE_EQ(view->reason_code, QStringLiteral("changed"));
    REQUIRE(view->differs_from_approved);
}

TEST_CASE("plugin mapping: with no isolation on this computer a decided plugin "
          "is Not isolated or Blocked")
{
    QTemporaryDir dir;
    const fs::path root = path_of(dir) / "plugins";
    const manifest_parts parts;
    make_plugin(root, "tool", parts);

    // Decide it first, so that the isolation is the only thing left.
    plugin_gather_options options = options_for(root);
    options.sandbox = plug::sandbox_level::full;
    plug::trust_store store;
    plugin_gathered found = gather_plugins(options, store);
    const plugin_view* view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    const plugin_view decided = *view;
    REQUIRE(store.decide(decision_for(decided)).has_value());

    // Studio installed for one user: the user may override.
    options.sandbox = plug::sandbox_level::none;
    found = gather_plugins(options, store);
    view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    REQUIRE(view->state == plugin_state::needs_isolation);
    REQUIRE(view->note == plugin_note::isolation_override);
    REQUIRE(view->override_allowed);
    REQUIRE(view->isolation == plugin_isolation::none);

    // The user's override for this version makes it allowed, and Off.
    plug::plugin_decision overridden = decision_for(decided);
    overridden.run_without_sandbox = true;
    REQUIRE(store.decide(overridden).has_value());
    found = gather_plugins(options, store);
    view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    REQUIRE(view->state == plugin_state::off);
    REQUIRE(view->override_active);

    // Studio installed for everyone and no policy: only an administrator can
    // allow it, so it is Blocked.
    options.install = plug::install_kind::for_everyone;
    plug::trust_store fresh;
    REQUIRE(fresh.decide(decision_for(decided)).has_value());
    found = gather_plugins(options, fresh);
    view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    REQUIRE(view->state == plugin_state::blocked);
    REQUIRE(view->note == plugin_note::isolation_no_override);
    REQUIRE(!view->override_allowed);
}

TEST_CASE("plugin mapping: files that do not match the manifest, and a "
          "manifest that is not valid, cannot be used")
{
    QTemporaryDir dir;
    const fs::path root = path_of(dir) / "plugins";
    manifest_parts listed_parts;
    listed_parts.id = "org.example.listed";
    listed_parts.list_files = true;
    const fs::path folder = make_plugin(root, "listed", listed_parts);
    // The program is not the one the manifest lists.
    write_file(folder / "bin" / "prog", "changed after listing");
    write_file(root / "broken" / "plugin.json", "{ not json");

    plug::trust_store store;
    const plugin_gathered found = gather_plugins(options_for(root), store);
    const plugin_view* mismatch = find_view(found, "org.example.listed");
    REQUIRE(mismatch != nullptr);
    REQUIRE(mismatch->state == plugin_state::cannot_be_used);
    REQUIRE(mismatch->note == plugin_note::invalid_signature);
    REQUIRE_EQ(mismatch->reason_code, QStringLiteral("signature_invalid"));

    // No name could be read, so the folder's name stands in for it.
    const plugin_view* broken = find_view(found, "broken");
    REQUIRE(broken != nullptr);
    REQUIRE(broken->state == plugin_state::cannot_be_used);
    REQUIRE(broken->note == plugin_note::invalid_manifest);
    REQUIRE_EQ(broken->name, QStringLiteral("broken"));
}

TEST_CASE("plugin mapping: a newer manifest, API or capability needs a newer "
          "Studio")
{
    QTemporaryDir dir;
    const fs::path root = path_of(dir) / "plugins";
    manifest_parts manifest_newer;
    manifest_newer.id = "org.example.manifest";
    manifest_newer.manifest_version = 99;
    make_plugin(root, "manifest", manifest_newer);
    manifest_parts api_newer;
    api_newer.id = "org.example.api";
    api_newer.api_min = "2.0";
    make_plugin(root, "api", api_newer);
    manifest_parts capability;
    capability.id = "org.example.capability";
    capability.extra_requires = R"(, "capabilities": ["engine_rules"])";
    make_plugin(root, "capability", capability);

    plug::trust_store store;
    plugin_gather_options options = options_for(root);
    options.sandbox = plug::sandbox_level::full;
    const plugin_gathered found = gather_plugins(options, store);

    const plugin_view* a = find_view(found, "org.example.api");
    REQUIRE(a != nullptr);
    REQUIRE(a->state == plugin_state::needs_newer_studio);
    REQUIRE(a->note == plugin_note::newer_studio_version);
    REQUIRE_EQ(a->studio_version, QStringLiteral("0.1.0"));

    const plugin_view* c = find_view(found, "org.example.capability");
    REQUIRE(c != nullptr);
    REQUIRE(c->state == plugin_state::needs_newer_studio);
    REQUIRE(c->note == plugin_note::newer_studio_capability);
    REQUIRE_EQ(c->capability_name, QStringLiteral("engine_rules"));

    // A manifest this reader is too old for has no manifest to read, so the
    // plugin is found by its folder.
    const plugin_view* m = find_view(found, "manifest");
    REQUIRE(m != nullptr);
    REQUIRE(m->state == plugin_state::needs_newer_studio);
}

TEST_CASE("plugin mapping: a copy that is shadowed is not used and names the "
          "copy that is")
{
    QTemporaryDir dir;
    const fs::path studio = path_of(dir) / "studio";
    const fs::path user = path_of(dir) / "user";
    manifest_parts parts;
    parts.id = "org.example.same";
    make_plugin(studio, "same", parts);
    parts.version = "2.0.0";
    make_plugin(user, "same", parts);

    plugin_gather_options options;
    options.roots.push_back(root_of(studio, plug::plugin_scope::for_everyone));
    options.roots.push_back(root_of(user, plug::plugin_scope::for_user));
    options.install = plug::install_kind::for_one_user;
    options.today = kToday;
    plug::trust_store store;
    const plugin_gathered found = gather_plugins(options, store);
    REQUIRE_EQ(found.snapshot.plugins.size(), std::size_t{2});

    const plugin_view* unused = nullptr;
    for (const plugin_view& each : found.snapshot.plugins)
    {
        if (each.state == plugin_state::not_used)
            unused = &each;
    }
    REQUIRE(unused != nullptr);
    REQUIRE(unused->installed_for == plugin_installed_for::you_only);
    REQUIRE(unused->note == plugin_note::not_used_same);
    REQUIRE(unused->newer_than_used_copy);
    REQUIRE(!unused->used_copy_folder.isEmpty());
    REQUIRE_EQ(unused->reason_code, QStringLiteral("shadowed"));
}

TEST_CASE("plugin mapping: an expired plugin cannot be used, and an unofficial "
          "one may run until Studio closes")
{
    QTemporaryDir dir;
    const fs::path root = path_of(dir) / "plugins";
    manifest_parts parts;
    parts.not_after = "2026-06-30";
    make_plugin(root, "tool", parts);

    plug::trust_store store;
    plugin_gather_options options = options_for(root);
    options.sandbox = plug::sandbox_level::full;
    plugin_gathered found = gather_plugins(options, store);
    const plugin_view* view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    REQUIRE(view->state == plugin_state::cannot_be_used);
    REQUIRE(view->note == plugin_note::expired);
    REQUIRE(view->can_run_expired);
    REQUIRE_EQ(view->expires, QDate(2026, 6, 30));

    // After "Run until Studio closes" the plugin goes on to its decision.
    options.run_expired.insert(parts.id + "|" + view->hash.toStdString());
    found = gather_plugins(options, store);
    view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    REQUIRE(view->state == plugin_state::needs_decision);
}

TEST_CASE("plugin mapping: a policy that allows only official plugins blocks "
          "an unsigned one")
{
    QTemporaryDir dir;
    const fs::path root = path_of(dir) / "plugins";
    const manifest_parts parts;
    make_plugin(root, "tool", parts);
    const fs::path policy = path_of(dir) / "policy.json";
    write_file(policy, R"({ "policy_version": 1, "allow": "official_only" })");

    // Installed for everyone, so that a policy that turns the user's own
    // folder off, as an unusable one does, does not hide it.
    plugin_gather_options options =
        options_for(root, plug::plugin_scope::for_everyone,
                    plug::install_kind::for_everyone);
    options.policy_file = policy;
    options.sandbox = plug::sandbox_level::full;
    plug::trust_store store;
    const plugin_gathered found = gather_plugins(options, store);
    const plugin_view* view = find_view(found, parts.id);
    REQUIRE(view != nullptr);
    REQUIRE(view->state == plugin_state::blocked);
    REQUIRE(view->note == plugin_note::blocked_official_only);
    // Whether the file could be trusted depends on who may write to it on
    // this computer; either way the page says something about it.
    REQUIRE(found.snapshot.banner == plugin_banner::policy_untrusted ||
            found.snapshot.banner == plugin_banner::official_only);
}

TEST_CASE("plugin mapping: signed plugins are official, third-party or "
          "unknown, and an official one without a licence needs one")
{
    QTemporaryDir dir;
    const fs::path root = path_of(dir) / "plugins";
    const fs::path keys = path_of(dir) / "official-keys.json";
    write_file(keys, signed_text(plug::key_list_purpose, "root-2026",
                                 key_list_payload(), kRootFill));

    manifest_parts official;
    official.id = "com.virtualserialstudio.decode";
    official.name = "Decoding";
    official.list_files = true;
    official.extra_requires = R"(, "edition": "pro")";
    official.not_after = "2027-06-30";
    const fs::path official_folder = make_plugin(root, "official", official);
    sign_plugin(official_folder, official, "plugins-2026", kPluginsFill);

    manifest_parts author;
    author.id = "org.author.tool";
    author.name = "Author tool";
    author.list_files = true;
    const fs::path author_folder = make_plugin(root, "author", author);
    sign_plugin(author_folder, author, "author-1", kAuthorFill);

    plugin_gather_options options = options_for(root);
    options.official_keys_file = keys;
    options.root_keys = {plug::root_key{
        "root-2026", signing::public_key_of(test_seed(kRootFill))}};
    options.sandbox = plug::sandbox_level::full;
    plug::trust_store store;

    plugin_gathered found = gather_plugins(options, store);
    const plugin_view* decode = find_view(found, official.id);
    REQUIRE(decode != nullptr);
    REQUIRE(decode->trust == plugin_trust::official);
    // The official plugin has no decision yet.
    REQUIRE(decode->state == plugin_state::needs_decision);
    REQUIRE(found.snapshot.banner == plugin_banner::no_licence_for_official);
    // A key nobody approved: signed, and the signature could not be checked.
    const plugin_view* tool = find_view(found, author.id);
    REQUIRE(tool != nullptr);
    REQUIRE(tool->trust == plugin_trust::unknown_key);
    REQUIRE_EQ(tool->key_id, QStringLiteral("author-1"));

    // The user decides the official one: with no licence it needs one.
    plug::plugin_decision official_decision = decision_for(*decode);
    official_decision.level = plug::trust_level::official;
    official_decision.key_id = "plugins-2026";
    REQUIRE(store.decide(official_decision).has_value());
    found = gather_plugins(options, store);
    decode = find_view(found, official.id);
    REQUIRE(decode != nullptr);
    REQUIRE(decode->state == plugin_state::needs_licence);
    REQUIRE(decode->licence_reason == vss::license::reason::no_licence);

    // Approving the author's key makes the plugin third-party signed, under
    // the name the user saw.
    plug::approved_key key;
    key.key_id = "author-1";
    key.public_key = signing::public_key_of(test_seed(kAuthorFill));
    key.name = "Author Name";
    key.approved_on = std::chrono::year_month_day{2026y / 11 / 14};
    REQUIRE(store.approve_key(key).has_value());
    found = gather_plugins(options, store);
    tool = find_view(found, author.id);
    REQUIRE(tool != nullptr);
    REQUIRE(tool->trust == plugin_trust::third_party);
    REQUIRE_EQ(tool->key_name, QStringLiteral("Author Name"));
    REQUIRE(tool->key_approved_on.isValid());
    // Approving a key never switches a plugin on: it still needs its decision.
    REQUIRE(tool->state == plugin_state::needs_decision);

    // Taking the key back moves the plugin back.
    REQUIRE(store.forget_key("author-1"));
    found = gather_plugins(options, store);
    tool = find_view(found, author.id);
    REQUIRE(tool != nullptr);
    REQUIRE(tool->trust == plugin_trust::unknown_key);
}

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
    const fs::path root = path_of(dir) / "plugins";
    const manifest_parts parts;
    const fs::path folder = make_plugin(root, "tool", parts);
    plug::trust_store store;
    const plugin_gathered found = gather_plugins(options_for(root), store);

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
        read_key_file_view(good, found, store, &approved);
    REQUIRE(view.failure == plugin_key_file_view::problem::none);
    REQUIRE_EQ(view.author, QStringLiteral("Author Name"));
    REQUIRE_EQ(view.key_id, QStringLiteral("author-1"));
    REQUIRE_EQ(view.fingerprint.size(), 16);
    REQUIRE(!view.inside_plugin_folder);
    REQUIRE_EQ(approved.key_id, std::string("author-1"));

    // Inside a plugin's own folder: the page must say so.
    const fs::path inside = folder / "author.vsskey.json";
    write_file(inside, key_json("author-1", "Author Name"));
    view = read_key_file_view(inside, found, store, nullptr);
    REQUIRE(view.failure == plugin_key_file_view::problem::none);
    REQUIRE(view.inside_plugin_folder);

    // Not a key file.
    const fs::path junk = path_of(dir) / "junk.vsskey.json";
    write_file(junk, "not json at all");
    view = read_key_file_view(junk, found, store, nullptr);
    REQUIRE(view.failure == plugin_key_file_view::problem::not_a_key_file);

    // A name with a direction override is refused.
    const fs::path rough = path_of(dir) / "rough.vsskey.json";
    write_file(rough, key_json("author-1", "Author\\u202eName"));
    view = read_key_file_view(rough, found, store, nullptr);
    REQUIRE(view.failure == plugin_key_file_view::problem::text_not_plain);

    // The id of an approved key never changes its key.
    plug::approved_key taken;
    taken.key_id = "author-1";
    taken.public_key = signing::public_key_of(test_seed(0x09));
    taken.name = "Somebody else";
    taken.approved_on = std::chrono::year_month_day{2026y / 11 / 14};
    REQUIRE(store.approve_key(taken).has_value());
    view = read_key_file_view(good, found, store, nullptr);
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

TEST_CASE("plugin backend: the real backend stores decisions and keys on its "
          "own thread and tells the page")
{
    QTemporaryDir dir;
    const fs::path root = path_of(dir) / "plugins";
    manifest_parts parts;
    parts.permissions = R"({ "name": "ports.read", "required": true })";
    make_plugin(root, "tool", parts);

    plugin_gather_options options = options_for(root);
    options.sandbox = plug::sandbox_level::full;
    options.trust_store_file = path_of(dir) / "data" / "trust.json";

    LibraryPluginBackend backend(options);
    int changes = 0;
    QObject::connect(&backend, &PluginBackend::snapshot_changed, &backend,
                     [&changes] { ++changes; });
    REQUIRE(vss_test::wait_until(
        [&] { return !backend.snapshot()->plugins.empty(); }));
    plugin_view view = backend.snapshot()->plugins.front();
    REQUIRE(view.state == plugin_state::needs_decision);
    REQUIRE(!backend.snapshot()->can_run_plugins);

    // The decision is stored for the plugin's hash, and the plugin becomes
    // Off with the wish kept: nothing is started.
    plugin_decision_request request;
    request.plugin_id = view.id;
    request.hash = view.hash;
    request.granted = {QStringLiteral("ports.read")};
    request.switch_on = true;
    backend.decide(request);
    REQUIRE(vss_test::wait_until(
        [&]
        {
            return backend.snapshot()->plugins.front().state ==
                   plugin_state::off;
        }));
    view = backend.snapshot()->plugins.front();
    REQUIRE(view.wished_on);
    const auto stored = plug::load_trust_store(options.trust_store_file);
    REQUIRE(stored.has_value());
    REQUIRE_EQ(stored->decisions().size(), std::size_t{1});
    REQUIRE_EQ(QString::fromStdString(
                   plug::to_hex(stored->decisions()[0].hash.bytes)),
               view.hash);
    REQUIRE(stored->decisions()[0].enabled);

    // A decision for a hash that no plugin has changes nothing.
    request.hash = QString(128, QLatin1Char('0'));
    backend.decide(request);
    backend.set_enabled(view.id, view.hash, false);
    REQUIRE(vss_test::wait_until(
        [&] { return !backend.snapshot()->plugins.front().wished_on; }));

    // A key: the fingerprint that was compared must be the file's.
    const std::string public_key =
        hex_of(signing::public_key_of(test_seed(kAuthorFill)));
    const fs::path key_file = path_of(dir) / "author.vsskey.json";
    write_file(key_file,
               std::string(R"({ "key_id": "author-1", "public_key": ")") +
                   public_key + R"(", "name": "Author Name" })");
    std::vector<std::pair<quint64, plugin_key_file_view>> key_reads;
    QObject::connect(&backend, &PluginBackend::key_file_read, &backend,
                     [&key_reads](quint64 request,
                                  const plugin_key_file_view& found)
                     { key_reads.emplace_back(request, found); });
    const quint64 number =
        backend.read_key_file(QString::fromStdWString(key_file.wstring()));
    REQUIRE(vss_test::wait_until([&] { return key_reads.size() == 1; }));
    REQUIRE_EQ(key_reads[0].first, number);
    const plugin_key_file_view key = key_reads[0].second;
    REQUIRE(key.failure == plugin_key_file_view::problem::none);

    std::vector<plugin_request_result> done;
    QObject::connect(&backend, &PluginBackend::request_finished, &backend,
                     [&done](const plugin_request_result& result)
                     { done.push_back(result); });
    backend.approve_key(key.path, QStringLiteral("0000000000000000"));
    REQUIRE(vss_test::wait_until([&] { return done.size() >= 1; }));
    auto result = done.back();
    REQUIRE(result.kind == plugin_request_kind::approve_key);
    REQUIRE(result.error == plugin_request_error::fingerprint_changed);
    REQUIRE(plug::load_trust_store(options.trust_store_file)->keys().empty());

    backend.approve_key(key.path, key.fingerprint);
    REQUIRE(vss_test::wait_until([&] { return done.size() >= 2; }));
    result = done.back();
    REQUIRE(result.error == plugin_request_error::none);
    REQUIRE_EQ(result.author, QStringLiteral("Author Name"));
    REQUIRE_EQ(plug::load_trust_store(options.trust_store_file)->keys().size(),
               std::size_t{1});

    backend.remove_key(QStringLiteral("author-1"));
    REQUIRE(vss_test::wait_until(
        [&]
        {
            const auto now = plug::load_trust_store(options.trust_store_file);
            return now.has_value() && now->keys().empty();
        }));
    REQUIRE(changes >= 1);
}

TEST_CASE("plugin backend: add a plugin to the user's folder")
{
    QTemporaryDir dir;
    const fs::path user = path_of(dir) / "user";
    fs::create_directories(user);
    const manifest_parts parts;
    const fs::path source =
        make_plugin(path_of(dir) / "elsewhere", "tool", parts);
    fs::create_directories(path_of(dir) / "empty");

    plugin_gather_options options = options_for(user);
    options.sandbox = plug::sandbox_level::full;
    LibraryPluginBackend backend(options);
    std::vector<plugin_request_result> done;
    QObject::connect(&backend, &PluginBackend::request_finished, &backend,
                     [&done](const plugin_request_result& result)
                     { done.push_back(result); });
    // The first look at the folders has finished.
    REQUIRE(vss_test::wait_until([&] { return done.size() >= 1; }));

    // A folder without plugin.json is not a plugin.
    backend.add_plugin(
        QString::fromStdWString((path_of(dir) / "empty").wstring()));
    REQUIRE(vss_test::wait_until([&] { return done.size() >= 2; }));
    REQUIRE(done.back().error ==
            plugin_request_error::not_a_plugin);

    backend.add_plugin(QString::fromStdWString(source.wstring()));
    REQUIRE(vss_test::wait_until(
        [&] { return !backend.snapshot()->plugins.empty(); }));
    // Nothing runs: it asks for a decision.
    REQUIRE(backend.snapshot()->plugins.front().state ==
            plugin_state::needs_decision);

    // The same plugin again is refused.
    const std::size_t before = done.size();
    backend.add_plugin(QString::fromStdWString(source.wstring()));
    REQUIRE(vss_test::wait_until([&] { return done.size() > before; }));
    REQUIRE(done.back().error ==
            plugin_request_error::already_installed);
}
