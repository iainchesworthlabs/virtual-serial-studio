//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/measure.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace vss;
using namespace vss::plugin;

namespace
{

namespace fs = std::filesystem;

using vss_test_support::scratch_directory;

/// What the program file of every test plugin holds.
constexpr std::string_view program_text = "program bytes";

/// Writes a file, and the directories above it when they are not there.
void write_file(const fs::path& path, std::string_view content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    REQUIRE(!out.fail());
}

/// The bytes a file holds now.
std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// A `plugin.json` that `parse_manifest` accepts.  It runs `bin/prog` and
/// lists the files given as JSON objects, when there are any.
std::string manifest_text(std::string_view id, std::string_view files = {})
{
    std::string out = "{\n";
    out += "  \"manifest_version\": 1,\n";
    out += "  \"id\": \"";
    out += id;
    out += "\",\n";
    out += "  \"name\": \"Test\",\n";
    out += "  \"version\": \"1.0.0\",\n";
    out += "  \"requires\": { \"api\": { \"min\": \"1.0\", \"max\": \"1.x\" } "
           "},\n";
    out += "  \"run\": { \"executable\": \"bin/prog\", ";
    out += "\"platforms\": [\"linux-x64\"] },\n";
    out += "  \"extension_points\": [\n";
    out += "    { \"kind\": \"generator\", \"id\": \"pulse\", ";
    out += "\"name\": \"Pulse\" }\n";
    out += "  ]";
    if (!files.empty())
    {
        out += ",\n  \"files\": [ ";
        out += files;
        out += " ]";
    }
    out += "\n}\n";
    return out;
}

/// One entry of the `files` list of a manifest, for a file with `content`.
std::string listed(std::string_view path, std::string_view content)
{
    std::string out = "{ \"path\": \"";
    out += path;
    out += "\", \"size\": ";
    out += std::to_string(content.size());
    out += ", \"sha512\": \"";
    out += to_hex(sha512_of(content));
    out += "\" }";
    return out;
}

/// A plugin folder `parent/folder` that `check_files` accepts: a manifest
/// with the given id and the program it runs.
fs::path make_plugin(const fs::path& parent, std::string_view folder,
                     std::string_view id)
{
    const fs::path directory = parent / std::string(folder);
    write_file(directory / "plugin.json", manifest_text(id));
    write_file(directory / "bin" / "prog", program_text);
    return directory;
}

/// The same, with a manifest that lists its program.
fs::path make_listed_plugin(const fs::path& parent, std::string_view folder,
                            std::string_view id)
{
    const fs::path directory = parent / std::string(folder);
    write_file(directory / "plugin.json",
               manifest_text(id, listed("bin/prog", program_text)));
    write_file(directory / "bin" / "prog", program_text);
    return directory;
}

plugin_root root_of(const fs::path& directory, plugin_scope scope)
{
    plugin_root out;
    out.directory = directory;
    out.scope = scope;
    return out;
}

/// A plugin that was found and has no problem.
void require_good(const found_plugin& found, std::string_view id)
{
    REQUIRE(!found.problem.failed());
    REQUIRE(found.detail.empty());
    REQUIRE(found.declared.has_value());
    REQUIRE_EQ(found.declared.value().id, std::string(id));
    REQUIRE(!found.shadowed_by.has_value());
}

/// A plugin that is refused for `expected`, with a line that says why.
void require_problem(const found_plugin& found, plugin_errc expected)
{
    REQUIRE(found.problem == expected);
    REQUIRE(!found.detail.empty());
}

/// A plugin that is refused before anything in it was read.
void require_nothing_read(const found_plugin& found)
{
    REQUIRE(!found.declared.has_value());
    REQUIRE(found.manifest_bytes.empty());
    REQUIRE(!found.signature_bytes.has_value());
    REQUIRE(found.files.empty());
    REQUIRE(found.hash == plugin_hash{});
    REQUIRE(!found.shadowed_by.has_value());
}

} // namespace

TEST_CASE("plugin discover: scopes have names")
{
    REQUIRE_EQ(to_string(plugin_scope::with_studio),
               std::string_view("with_studio"));
    REQUIRE_EQ(to_string(plugin_scope::for_everyone),
               std::string_view("for_everyone"));
    REQUIRE_EQ(to_string(plugin_scope::for_user), std::string_view("for_user"));
}

TEST_CASE("plugin discover: no roots and a root that is not there find nothing")
{
    const std::vector<plugin_root> none;
    const discovery empty = discover_plugins(none);
    REQUIRE(empty.plugins.empty());
    REQUIRE(empty.roots.empty());

    const scratch_directory scratch;
    const std::vector<plugin_root> missing = {
        root_of(scratch.path() / "not-there", plugin_scope::for_everyone)};
    const discovery found = discover_plugins(missing);
    REQUIRE(found.plugins.empty());
    REQUIRE(found.roots.empty());
}

TEST_CASE("plugin discover: a valid plugin is read whole")
{
    const scratch_directory scratch;
    const fs::path directory =
        make_plugin(scratch.path(), "alpha", "com.example.alpha");
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_everyone)};

    const discovery found = discover_plugins(roots);
    REQUIRE(found.roots.empty());
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    const found_plugin& plugin = found.plugins[0];
    require_good(plugin, "com.example.alpha");
    REQUIRE(plugin.directory == directory);
    REQUIRE(plugin.scope == plugin_scope::for_everyone);
    REQUIRE_EQ(plugin.manifest_bytes, read_file(directory / "plugin.json"));
    REQUIRE(!plugin.signature_bytes.has_value());

    const auto measured = measure_directory(directory);
    REQUIRE(measured.has_value());
    REQUIRE(plugin.files == *measured);
    REQUIRE_EQ(plugin.files.size(), std::size_t{2});
    REQUIRE(plugin.hash == hash_plugin(*measured));
}

TEST_CASE("plugin discover: a plugin.sig is handed over with its bytes")
{
    const scratch_directory scratch;
    const fs::path directory =
        make_plugin(scratch.path(), "alpha", "com.example.alpha");
    const std::string signature("sig\0bytes\n", 10);
    write_file(directory / "plugin.sig", signature);
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    const found_plugin& plugin = found.plugins[0];
    require_good(plugin, "com.example.alpha");
    REQUIRE(plugin.signature_bytes.has_value());
    REQUIRE_EQ(*plugin.signature_bytes, signature);

    // The signature is one of the files that the hash covers.
    const auto measured = measure_directory(directory);
    REQUIRE(measured.has_value());
    REQUIRE_EQ(plugin.files.size(), std::size_t{3});
    REQUIRE(plugin.files == *measured);
    REQUIRE(plugin.hash == hash_plugin(*measured));
}

TEST_CASE("plugin discover: the plugins of a root are sorted by folder name")
{
    const scratch_directory scratch;
    make_plugin(scratch.path(), "beta", "com.example.beta");
    make_plugin(scratch.path(), "alpha", "com.example.alpha");
    // Upper case comes before lower case when the names are compared as bytes.
    make_plugin(scratch.path(), "Zed", "com.example.zed");
    // These two are not plugins and are passed over.
    write_file(scratch.path() / "no-manifest" / "bin" / "prog", program_text);
    write_file(scratch.path() / "0-notes.txt", "a plain file");
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE(found.roots.empty());
    REQUIRE_EQ(found.plugins.size(), std::size_t{3});
    require_good(found.plugins[0], "com.example.zed");
    require_good(found.plugins[1], "com.example.alpha");
    require_good(found.plugins[2], "com.example.beta");
    REQUIRE(found.plugins[0].directory == scratch.path() / "Zed");
    REQUIRE(found.plugins[1].directory == scratch.path() / "alpha");
    REQUIRE(found.plugins[2].directory == scratch.path() / "beta");
}

TEST_CASE("plugin discover: a root may be given with a trailing separator")
{
    const scratch_directory scratch;
    make_plugin(scratch.path(), "alpha", "com.example.alpha");
    const std::vector<plugin_root> roots = {
        root_of(scratch.path() / "", plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE(found.roots.empty());
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    require_good(found.plugins[0], "com.example.alpha");
}

TEST_CASE("plugin discover: roots keep their order and each gives its scope")
{
    const scratch_directory scratch;
    const fs::path studio = scratch.path() / "studio";
    const fs::path everyone = scratch.path() / "everyone";
    const fs::path user = scratch.path() / "user";
    make_plugin(studio, "b", "com.example.studio-b");
    make_plugin(studio, "a", "com.example.studio-a");
    make_plugin(everyone, "m", "com.example.everyone-m");
    make_plugin(user, "z", "com.example.user-z");
    make_plugin(user, "y", "com.example.user-y");
    const std::vector<plugin_root> roots = {
        root_of(studio, plugin_scope::with_studio),
        root_of(everyone, plugin_scope::for_everyone),
        root_of(user, plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE(found.roots.empty());
    REQUIRE_EQ(found.plugins.size(), std::size_t{5});
    const std::vector<std::string_view> ids = {
        "com.example.studio-a", "com.example.studio-b",
        "com.example.everyone-m", "com.example.user-y", "com.example.user-z"};
    const std::vector<plugin_scope> scopes = {
        plugin_scope::with_studio, plugin_scope::with_studio,
        plugin_scope::for_everyone, plugin_scope::for_user,
        plugin_scope::for_user};
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        require_good(found.plugins[i], ids[i]);
        REQUIRE(found.plugins[i].scope == scopes[i]);
    }
}

TEST_CASE("plugin discover: the first copy of an id counts and later ones are "
          "shadowed")
{
    const scratch_directory scratch;
    const fs::path studio = scratch.path() / "studio";
    const fs::path everyone = scratch.path() / "everyone";
    const fs::path user = scratch.path() / "user";
    make_plugin(studio, "one", "com.example.same");
    make_plugin(everyone, "two", "com.example.same");
    make_plugin(user, "three", "com.example.same");
    make_plugin(user, "other", "com.example.other");
    const std::vector<plugin_root> roots = {
        root_of(studio, plugin_scope::with_studio),
        root_of(everyone, plugin_scope::for_everyone),
        root_of(user, plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{4});
    require_good(found.plugins[0], "com.example.same");

    for (const std::size_t at : {std::size_t{1}, std::size_t{3}})
    {
        const found_plugin& copy = found.plugins[at];
        require_problem(copy, plugin_errc::shadowed);
        REQUIRE(copy.shadowed_by.has_value());
        REQUIRE_EQ(*copy.shadowed_by, std::size_t{0});
        REQUIRE(copy.declared.has_value());
        REQUIRE_EQ(copy.declared.value().id, std::string("com.example.same"));
        REQUIRE(!copy.manifest_bytes.empty());
        REQUIRE(!copy.files.empty());
    }
    // Sorted by folder name, "other" comes before "three".
    require_good(found.plugins[2], "com.example.other");
    REQUIRE(found.plugins[2].directory == user / "other");
    REQUIRE(found.plugins[3].directory == user / "three");
}

TEST_CASE("plugin discover: a copy with a problem of its own still shadows")
{
    const scratch_directory scratch;
    const fs::path studio = scratch.path() / "studio";
    const fs::path user = scratch.path() / "user";
    const fs::path broken = make_plugin(studio, "broken", "com.example.same");
    fs::remove(broken / "bin" / "prog");
    make_plugin(user, "good", "com.example.same");
    const std::vector<plugin_root> roots = {
        root_of(studio, plugin_scope::with_studio),
        root_of(user, plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{2});
    require_problem(found.plugins[0], plugin_errc::file_missing);
    REQUIRE(!found.plugins[0].shadowed_by.has_value());
    require_problem(found.plugins[1], plugin_errc::shadowed);
    REQUIRE(found.plugins[1].shadowed_by.has_value());
    REQUIRE_EQ(found.plugins[1].shadowed_by.value_or(99), std::size_t{0});
}

TEST_CASE("plugin discover: a shadowed copy keeps a problem of its own")
{
    const scratch_directory scratch;
    const fs::path studio = scratch.path() / "studio";
    const fs::path user = scratch.path() / "user";
    make_plugin(studio, "good", "com.example.same");
    const fs::path broken = make_plugin(user, "broken", "com.example.same");
    fs::remove(broken / "bin" / "prog");
    const std::vector<plugin_root> roots = {
        root_of(studio, plugin_scope::with_studio),
        root_of(user, plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{2});
    require_good(found.plugins[0], "com.example.same");
    require_problem(found.plugins[1], plugin_errc::file_missing);
    REQUIRE(found.plugins[1].shadowed_by.has_value());
    REQUIRE_EQ(found.plugins[1].shadowed_by.value_or(99), std::size_t{0});
    REQUIRE(found.plugins[1].declared.has_value());
}

TEST_CASE("plugin discover: a manifest that does not parse is refused")
{
    const scratch_directory scratch;
    const std::string text = "{ this is not json";
    write_file(scratch.path() / "bad" / "plugin.json", text);
    write_file(scratch.path() / "bad" / "bin" / "prog", program_text);
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    const found_plugin& plugin = found.plugins[0];
    require_problem(plugin, plugin_errc::manifest_invalid);
    REQUIRE(!plugin.declared.has_value());
    REQUIRE_EQ(plugin.manifest_bytes, text);
    REQUIRE(plugin.files.empty());
    REQUIRE(!plugin.signature_bytes.has_value());
}

TEST_CASE("plugin discover: a manifest of a newer format is refused")
{
    const scratch_directory scratch;
    std::string text = manifest_text("com.example.newer");
    const std::size_t at = text.find("\"manifest_version\": 1");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("\"manifest_version\": 1").size(),
                 "\"manifest_version\": 2");
    write_file(scratch.path() / "newer" / "plugin.json", text);
    write_file(scratch.path() / "newer" / "bin" / "prog", program_text);
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    require_problem(found.plugins[0],
                    plugin_errc::manifest_unsupported_version);
    REQUIRE(!found.plugins[0].declared.has_value());
    REQUIRE_EQ(found.plugins[0].manifest_bytes, text);
}

TEST_CASE("plugin discover: a manifest that is too large is not read")
{
    const scratch_directory scratch;
    write_file(scratch.path() / "huge" / "plugin.json",
               std::string(max_manifest_size + 1, ' '));
    write_file(scratch.path() / "huge" / "bin" / "prog", program_text);
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    require_problem(found.plugins[0], plugin_errc::manifest_too_large);
    require_nothing_read(found.plugins[0]);
}

TEST_CASE("plugin discover: a missing program is reported with the files read")
{
    const scratch_directory scratch;
    const fs::path directory =
        make_plugin(scratch.path(), "alpha", "com.example.alpha");
    fs::remove(directory / "bin" / "prog");
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    const found_plugin& plugin = found.plugins[0];
    require_problem(plugin, plugin_errc::file_missing);
    REQUIRE(plugin.declared.has_value());
    REQUIRE_EQ(plugin.declared.value().id, std::string("com.example.alpha"));
    REQUIRE_EQ(plugin.manifest_bytes, read_file(directory / "plugin.json"));
    REQUIRE_EQ(plugin.files.size(), std::size_t{1});
    REQUIRE_EQ(plugin.files[0].path, std::string("plugin.json"));
    REQUIRE(plugin.hash == hash_plugin(plugin.files));
    REQUIRE(!plugin.signature_bytes.has_value());
}

TEST_CASE("plugin discover: a plugin that lists its files is accepted when "
          "they match")
{
    const scratch_directory scratch;
    make_listed_plugin(scratch.path(), "alpha", "com.example.alpha");
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    const found_plugin& plugin = found.plugins[0];
    require_good(plugin, "com.example.alpha");
    REQUIRE(plugin.declared.has_value());
    REQUIRE_EQ(plugin.declared.value().files.size(), std::size_t{1});
}

TEST_CASE("plugin discover: a listed file that was changed is refused")
{
    const scratch_directory scratch;
    const fs::path directory =
        make_listed_plugin(scratch.path(), "alpha", "com.example.alpha");
    write_file(directory / "bin" / "prog", "program bytes, changed");
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    const found_plugin& plugin = found.plugins[0];
    require_problem(plugin, plugin_errc::signature_invalid);
    REQUIRE(plugin.declared.has_value());
    REQUIRE_EQ(plugin.files.size(), std::size_t{2});
    REQUIRE(plugin.hash == hash_plugin(plugin.files));
}

TEST_CASE("plugin discover: a file the manifest does not list is refused")
{
    const scratch_directory scratch;
    const fs::path directory =
        make_listed_plugin(scratch.path(), "alpha", "com.example.alpha");
    write_file(directory / "bin" / "extra", "not listed");
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    const found_plugin& plugin = found.plugins[0];
    require_problem(plugin, plugin_errc::unlisted_file);
    REQUIRE(plugin.declared.has_value());
    REQUIRE_EQ(plugin.files.size(), std::size_t{3});
}

TEST_CASE("plugin discover: a plugin.sig that is too large is refused")
{
    const scratch_directory scratch;
    const fs::path directory =
        make_plugin(scratch.path(), "alpha", "com.example.alpha");
    write_file(directory / "plugin.sig",
               std::string(max_signature_size + 1, 'x'));
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    const found_plugin& plugin = found.plugins[0];
    require_problem(plugin, plugin_errc::manifest_too_large);
    REQUIRE(plugin.declared.has_value());
    REQUIRE_EQ(plugin.manifest_bytes, read_file(directory / "plugin.json"));
    REQUIRE(!plugin.signature_bytes.has_value());
    REQUIRE_EQ(plugin.files.size(), std::size_t{3});
    REQUIRE(plugin.hash == hash_plugin(plugin.files));
}

TEST_CASE("plugin discover: a plugin.sig of the largest size is accepted")
{
    const scratch_directory scratch;
    const fs::path directory =
        make_plugin(scratch.path(), "alpha", "com.example.alpha");
    const std::string signature(max_signature_size, 'x');
    write_file(directory / "plugin.sig", signature);
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    require_good(found.plugins[0], "com.example.alpha");
    REQUIRE(found.plugins[0].signature_bytes.has_value());
    REQUIRE_EQ(found.plugins[0].signature_bytes.value_or(std::string()),
               signature);
}

TEST_CASE("plugin discover: plugins past the limit are not read")
{
    const scratch_directory scratch;
    const std::size_t total = max_plugins + 2;
    for (std::size_t i = 0; i < total; ++i)
    {
        std::string name = "p";
        if (i < 10)
        {
            name += '0';
        }
        name += std::to_string(i);
        std::string id = "com.example.";
        id += name;
        make_plugin(scratch.path(), name, id);
    }
    const std::vector<plugin_root> roots = {
        root_of(scratch.path(), plugin_scope::for_everyone)};

    const discovery found = discover_plugins(roots);
    REQUIRE(found.roots.empty());
    REQUIRE_EQ(found.plugins.size(), total);
    for (std::size_t i = 0; i < max_plugins; ++i)
    {
        REQUIRE(!found.plugins[i].problem.failed());
        REQUIRE(found.plugins[i].declared.has_value());
    }
    for (std::size_t i = max_plugins; i < total; ++i)
    {
        const found_plugin& plugin = found.plugins[i];
        REQUIRE(plugin.problem == plugin_errc::too_many);
        REQUIRE(plugin.scope == plugin_scope::for_everyone);
        std::string name = "p";
        name += std::to_string(i);
        REQUIRE(plugin.directory == scratch.path() / name);
        REQUIRE(plugin.detail.empty());
        require_nothing_read(plugin);
    }
}

TEST_CASE("plugin discover: a root that is a plain file is reported")
{
    const scratch_directory scratch;
    const fs::path file = scratch.path() / "not-a-folder";
    write_file(file, "a plain file");
    const fs::path user = scratch.path() / "user";
    make_plugin(user, "alpha", "com.example.alpha");
    const std::vector<plugin_root> roots = {
        root_of(file, plugin_scope::with_studio),
        root_of(user, plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE_EQ(found.roots.size(), std::size_t{1});
    REQUIRE(found.roots[0].directory == file);
    REQUIRE(found.roots[0].scope == plugin_scope::with_studio);
    REQUIRE(found.roots[0].problem == plugin_errc::layout_invalid);
    REQUIRE(!found.roots[0].detail.empty());
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    require_good(found.plugins[0], "com.example.alpha");
    REQUIRE(found.plugins[0].scope == plugin_scope::for_user);
}

TEST_CASE("plugin discover: a root that is a link is not followed")
{
    const scratch_directory scratch;
    const fs::path real = scratch.path() / "real";
    make_plugin(real, "alpha", "com.example.alpha");
    const fs::path link = scratch.path() / "link";
    std::error_code ec;
    fs::create_directory_symlink(real, link, ec);
    if (ec)
    {
        VSS_SKIP("symbolic links cannot be made here");
    }

    for (const fs::path& directory : {link, link / ""})
    {
        const std::vector<plugin_root> roots = {
            root_of(directory, plugin_scope::for_everyone)};
        const discovery found = discover_plugins(roots);
        REQUIRE(found.plugins.empty());
        REQUIRE_EQ(found.roots.size(), std::size_t{1});
        REQUIRE(found.roots[0].directory == directory);
        REQUIRE(found.roots[0].scope == plugin_scope::for_everyone);
        REQUIRE(found.roots[0].problem == plugin_errc::layout_invalid);
        REQUIRE(!found.roots[0].detail.empty());
    }
}

TEST_CASE("plugin discover: an entry of a root that is a link is not followed")
{
    const scratch_directory scratch;
    const fs::path outside = scratch.path() / "outside";
    make_plugin(outside, "plug", "com.example.linked");
    const fs::path root = scratch.path() / "root";
    make_plugin(root, "real", "com.example.real");
    std::error_code ec;
    fs::create_directory_symlink(outside / "plug", root / "link", ec);
    if (ec)
    {
        VSS_SKIP("symbolic links cannot be made here");
    }
    const std::vector<plugin_root> roots = {
        root_of(root, plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE(found.roots.empty());
    REQUIRE_EQ(found.plugins.size(), std::size_t{2});
    REQUIRE(found.plugins[0].directory == root / "link");
    REQUIRE(found.plugins[0].scope == plugin_scope::for_user);
    require_problem(found.plugins[0], plugin_errc::layout_invalid);
    require_nothing_read(found.plugins[0]);
    require_good(found.plugins[1], "com.example.real");
}

TEST_CASE("plugin discover: a plugin.json that is a link is refused")
{
    const scratch_directory scratch;
    const fs::path elsewhere = scratch.path() / "elsewhere.json";
    write_file(elsewhere, manifest_text("com.example.linked"));
    const fs::path root = scratch.path() / "root";
    write_file(root / "alpha" / "bin" / "prog", program_text);
    std::error_code ec;
    fs::create_symlink(elsewhere, root / "alpha" / "plugin.json", ec);
    if (ec)
    {
        VSS_SKIP("symbolic links cannot be made here");
    }
    const std::vector<plugin_root> roots = {
        root_of(root, plugin_scope::for_user)};

    const discovery found = discover_plugins(roots);
    REQUIRE(found.roots.empty());
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    REQUIRE(found.plugins[0].directory == root / "alpha");
    require_problem(found.plugins[0], plugin_errc::layout_invalid);
    require_nothing_read(found.plugins[0]);
}
