//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Looking at a plugin folder, installing it and removing it, on plugin
// folders that each test makes in a scratch directory.  Every key in this file
// is a THROWAWAY TEST KEY: a private key made of one repeated byte, written
// down here for anyone to read.  None of them signs anything outside these
// tests.
//

#include <vss/core/error.hpp>
#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/machine/folders.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/measure.hpp>
#include <vss/plugin/signature.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace vss;
using namespace vss::plugin;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;
namespace signing = vss::license::signing;
namespace signed_file = vss::license::signed_file;

using vss_test_support::scratch_directory;

constexpr std::chrono::sys_days today{2026y / 11 / 14};

// The test keys, by the byte each is made of.
constexpr std::uint8_t plugins_fill = 0x02;
constexpr std::uint8_t revocations_fill = 0x03;
constexpr std::uint8_t author_fill = 0x04;
constexpr std::uint8_t stranger_fill = 0x05;

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
        out += digits[byte >> 4];
        out += digits[byte & 0x0f];
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

/// What the program file of every test plugin holds.
constexpr std::string_view program_text = "program bytes";

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

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// What a test plugin is made of.
struct spec
{
    std::string id = "com.example.modbus";
    /// A `not_after` date, or none.
    std::string not_after;
    /// The edition the plugin asks for, or none.
    std::string edition;
    std::string program{program_text};
};

std::string manifest_text(const spec& what)
{
    std::string out = "{\n  \"manifest_version\": 1,\n  \"id\": \"";
    out += what.id;
    out += "\",\n  \"name\": \"Test\",\n  \"version\": \"1.0.0\",\n";
    out += "  \"author\": { \"name\": \"Example Ltd\" },\n";
    if (!what.not_after.empty())
        out += "  \"not_after\": \"" + what.not_after + "\",\n";
    out += "  \"requires\": { \"api\": { \"min\": \"1.0\", \"max\": \"1.x\" }";
    if (!what.edition.empty())
        out += ", \"edition\": \"" + what.edition + "\"";
    out += " },\n";
    out += "  \"run\": { \"executable\": \"bin/prog\", ";
    out += "\"platforms\": [\"linux-x64\"] },\n";
    out += "  \"extension_points\": [\n";
    out += "    { \"kind\": \"generator\", \"id\": \"pulse\", ";
    out += "\"name\": \"Pulse\" }\n  ],\n";
    out += "  \"files\": [ { \"path\": \"bin/prog\", \"size\": ";
    out += std::to_string(what.program.size());
    out += ", \"sha512\": \"";
    out += to_hex(sha512_of(what.program));
    out += "\" } ]\n}\n";
    return out;
}

/// A plugin folder `parent/folder` that lists its program, not signed.
fs::path make_plugin(const fs::path& parent, std::string_view folder,
                     const spec& what = {})
{
    const fs::path directory = parent / std::string(folder);
    write_file(directory / "plugin.json", manifest_text(what));
    write_file(directory / "bin" / "prog", what.program);
    return directory;
}

/// Signs the plugin in `directory` as the key `key_id`, made of `fill`.
void sign_plugin(const fs::path& directory, std::string_view key_id,
                 std::uint8_t fill, const std::string& id = "com.example.modbus")
{
    const std::string manifest = read_file(directory / "plugin.json");
    std::string claim = R"({ "plugin_id": ")";
    claim += id;
    claim += R"(", "version": "1.0.0", "manifest_sha512": ")";
    claim += to_hex(sha512_of(manifest));
    claim += R"(" })";
    write_file(directory / "plugin.sig",
               signed_text(plugin_purpose, key_id, claim, fill));
}

/// A plugin signed with the official test key.  Official plugins need an
/// end date.
fs::path make_official(const fs::path& parent, std::string_view folder,
                       spec what = {})
{
    what.id = "com.virtualserialstudio.protocol";
    what.not_after = "2027-06-01";
    const fs::path directory = make_plugin(parent, folder, what);
    sign_plugin(directory, "plugins-2026", plugins_fill, what.id);
    return directory;
}

/// A plugin signed with the key `author-1`, which the user approved.
fs::path make_third_party(const fs::path& parent, std::string_view folder,
                          const spec& what = {})
{
    const fs::path directory = make_plugin(parent, folder, what);
    sign_plugin(directory, "author-1", author_fill, what.id);
    return directory;
}

inspect_context make_context()
{
    inspect_context out;
    out.today = today;
    out.official = official_keys();
    out.user_keys.push_back(approved_signer{"author-1", public_of(author_fill)});
    out.sandbox = sandbox_level::full;
    out.install = install_kind::for_one_user;
    return out;
}

install_options make_options(const inspect_context& context)
{
    install_options out;
    out.context = context;
    return out;
}

/// The names in a folder, sorted; none when it is not there.
std::vector<std::string> names_in(const fs::path& directory)
{
    std::vector<std::string> names;
    std::error_code ec;
    if (!fs::is_directory(directory, ec))
        return names;
    for (const fs::directory_entry& each : fs::directory_iterator(directory))
        names.push_back(each.path().filename().string());
    std::ranges::sort(names);
    return names;
}

bool make_file_link(const fs::path& target, const fs::path& link_path)
{
    std::error_code ec;
    fs::create_symlink(target, link_path, ec);
    return !ec;
}

/// The trust level of an inspection that has one.
trust_level level_of(const inspection& found)
{
    REQUIRE(found.level.has_value());
    return found.level.value_or(trust_level::unsigned_plugin);
}

plugin_hash hash_of_folder(const fs::path& directory)
{
    const auto files = measure_directory(directory);
    REQUIRE(files.has_value());
    return hash_plugin(files.value());
}

/// Installs and requires that it was refused with `expected`, and that the
/// target folder holds nothing at all.
void require_refused(const fs::path& source, const fs::path& root,
                     const install_options& options, plugin_errc expected)
{
    std::string detail;
    const auto done = install_plugin(source, root, options, &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == expected);
    REQUIRE(!detail.empty());
    REQUIRE(names_in(root).empty());
}

} // namespace

// -- inspect ----------------------------------------------------------------

TEST_CASE("plugin install: an official plugin is looked at in one result")
{
    const scratch_directory scratch;
    const fs::path directory = make_official(scratch.path(), "official");
    const inspection found = inspect_plugin(directory, make_context());

    REQUIRE(!found.problem.failed());
    REQUIRE(found.declared.has_value());
    REQUIRE_EQ(found.declared.value().id,
               std::string("com.virtualserialstudio.protocol"));
    REQUIRE(found.signature.state == signature_state::valid);
    REQUIRE(found.signature.origin == key_origin::official);
    REQUIRE_EQ(found.signature.key_id, std::string("plugins-2026"));
    REQUIRE(found.level.has_value());
    REQUIRE(level_of(found) == trust_level::official);
    REQUIRE_EQ(found.key_id, std::string("plugins-2026"));
    REQUIRE(!found.run.failed());
    REQUIRE(found.hash == hash_of_folder(directory));
    REQUIRE(!found.licence_checked);
    REQUIRE(!blocks_install(found));
}

TEST_CASE("plugin install: a plugin signed with an approved key is "
          "third-party")
{
    const scratch_directory scratch;
    const fs::path directory = make_third_party(scratch.path(), "third");
    const inspection found = inspect_plugin(directory, make_context());
    REQUIRE(!found.problem.failed());
    REQUIRE(found.signature.state == signature_state::valid);
    REQUIRE(found.signature.origin == key_origin::user);
    REQUIRE(level_of(found) == trust_level::third_party_signed);
    REQUIRE(!found.run.failed());
    REQUIRE(!blocks_install(found));
}

TEST_CASE("plugin install: a plugin with no signature is unsigned")
{
    const scratch_directory scratch;
    const fs::path directory = make_plugin(scratch.path(), "plain");
    const inspection found = inspect_plugin(directory, make_context());
    REQUIRE(!found.problem.failed());
    REQUIRE(found.signature.state == signature_state::absent);
    REQUIRE(level_of(found) == trust_level::unsigned_plugin);
    REQUIRE(!blocks_install(found));
}

TEST_CASE("plugin install: a key nobody has leaves the plugin signed with an "
          "unknown key")
{
    const scratch_directory scratch;
    const fs::path directory = make_plugin(scratch.path(), "unknown");
    sign_plugin(directory, "nobody-1", stranger_fill);
    const inspection found = inspect_plugin(directory, make_context());
    REQUIRE(!found.problem.failed());
    REQUIRE(found.signature.state == signature_state::unknown_key);
    REQUIRE_EQ(found.signature.key_id, std::string("nobody-1"));
    REQUIRE(level_of(found) == trust_level::signed_unknown_key);
    REQUIRE(!blocks_install(found));
}

TEST_CASE("plugin install: a flipped byte, an extra file and a missing file "
          "are found")
{
    const scratch_directory scratch;

    const fs::path flipped = make_official(scratch.path(), "flipped");
    write_file(flipped / "bin" / "prog", "program bytez");
    inspection found = inspect_plugin(flipped, make_context());
    REQUIRE(found.problem == plugin_errc::signature_invalid);
    REQUIRE(found.detail.find("bin/prog") != std::string::npos);
    REQUIRE(!found.level.has_value());
    REQUIRE(blocks_install(found));

    const fs::path extra = make_official(scratch.path(), "extra");
    write_file(extra / "bin" / "more", "more");
    found = inspect_plugin(extra, make_context());
    REQUIRE(found.problem == plugin_errc::unlisted_file);
    REQUIRE(blocks_install(found));

    const fs::path missing = make_official(scratch.path(), "missing");
    std::error_code ec;
    fs::remove(missing / "bin" / "prog", ec);
    found = inspect_plugin(missing, make_context());
    REQUIRE(found.problem == plugin_errc::file_missing);
    REQUIRE(blocks_install(found));
}

TEST_CASE("plugin install: a signature made with the wrong key does not hold")
{
    const scratch_directory scratch;
    // It names the approved key `author-1` but is made with another key.
    const fs::path directory = make_plugin(scratch.path(), "wrong");
    sign_plugin(directory, "author-1", stranger_fill);
    const inspection found = inspect_plugin(directory, make_context());
    REQUIRE(found.signature.state == signature_state::invalid);
    REQUIRE(found.problem == plugin_errc::signature_invalid);
    REQUIRE(!found.level.has_value());
    REQUIRE(blocks_install(found));
}

TEST_CASE("plugin install: an expired plugin is reported and still "
          "installable; a revoked one is not")
{
    const scratch_directory scratch;
    spec old;
    old.not_after = "2026-01-01";
    const fs::path expired = make_third_party(scratch.path(), "expired", old);
    inspection found = inspect_plugin(expired, make_context());
    REQUIRE(!found.problem.failed());
    REQUIRE(found.run == plugin_errc::expired);
    REQUIRE(level_of(found) == trust_level::third_party_signed);
    REQUIRE(!blocks_install(found));

    const fs::path revoked = make_third_party(scratch.path(), "revoked");
    inspect_context context = make_context();
    revocation_list list;
    list.serial = 1;
    list.key_ids.push_back("author-1");
    context.revocations = list;
    found = inspect_plugin(revoked, context);
    REQUIRE(found.run == plugin_errc::revoked);
    REQUIRE(blocks_install(found));
}

TEST_CASE("plugin install: no sandbox does not stop an install")
{
    const scratch_directory scratch;
    const fs::path directory = make_third_party(scratch.path(), "third");
    inspect_context context = make_context();
    context.sandbox = sandbox_level::none;
    const inspection found = inspect_plugin(directory, context);
    REQUIRE(found.run == plugin_errc::sandbox_insufficient);
    REQUIRE(!blocks_install(found));
}

TEST_CASE("plugin install: the licence is asked only for an official plugin "
          "that names an edition")
{
    const scratch_directory scratch;
    spec pro;
    pro.edition = "pro";
    const fs::path directory = make_official(scratch.path(), "pro", pro);

    // The licence was not read: not asked.
    inspection found = inspect_plugin(directory, make_context());
    REQUIRE(!found.licence_checked);

    inspect_context context = make_context();
    context.licence_known = true;
    found = inspect_plugin(directory, context);
    REQUIRE(found.licence_checked);
    REQUIRE(found.licence_problem == plugin_errc::not_licensed);
    REQUIRE(!found.licence.granted);
    // A licence that does not cover it does not stop an install.
    REQUIRE(!blocks_install(found));

    // A third-party plugin is never asked.
    spec other;
    other.edition = "pro";
    const fs::path third = make_third_party(scratch.path(), "third", other);
    found = inspect_plugin(third, context);
    REQUIRE(!found.licence_checked);
}

TEST_CASE("plugin install: a folder that is not a plugin gives a reason")
{
    const scratch_directory scratch;
    inspection found = inspect_plugin(scratch.path() / "nothing", make_context());
    REQUIRE(found.problem == plugin_errc::file_missing);
    REQUIRE(blocks_install(found));

    write_file(scratch.path() / "empty" / "other.txt", "x");
    found = inspect_plugin(scratch.path() / "empty", make_context());
    REQUIRE(found.problem == plugin_errc::file_missing);

    write_file(scratch.path() / "bad" / "plugin.json", "{ not JSON");
    found = inspect_plugin(scratch.path() / "bad", make_context());
    REQUIRE(found.problem == plugin_errc::manifest_invalid);
    REQUIRE(!found.declared.has_value());
}

// -- install ----------------------------------------------------------------

TEST_CASE("plugin install: a good official plugin is copied, and the copy has "
          "the same hash")
{
    const scratch_directory scratch;
    const fs::path source = make_official(scratch.path(), "source");
    const fs::path root = scratch.path() / "installed";

    const auto done =
        install_plugin(source, root, make_options(make_context()));
    REQUIRE(done.has_value());
    const install_report& report = done.value();
    REQUIRE(report.directory == root / "com.virtualserialstudio.protocol");
    REQUIRE(!report.replaced);
    REQUIRE(report.warning.empty());
    REQUIRE(level_of(report.found) == trust_level::official);
    REQUIRE(hash_of_folder(report.directory) == hash_of_folder(source));
    REQUIRE_EQ(read_file(report.directory / "bin" / "prog"),
               std::string(program_text));
    // Nothing is left beside it.
    const std::vector<std::string> names = names_in(root);
    REQUIRE_EQ(names.size(), std::size_t{1});
    REQUIRE_EQ(names[0], std::string("com.virtualserialstudio.protocol"));

    // The copy inspects the same as the source.
    const inspection again = inspect_plugin(report.directory, make_context());
    REQUIRE(!again.problem.failed());
    REQUIRE(level_of(again) == trust_level::official);
}

TEST_CASE("plugin install: a third-party plugin signed with an approved key "
          "is copied")
{
    const scratch_directory scratch;
    const fs::path source = make_third_party(scratch.path(), "source");
    const fs::path root = scratch.path() / "installed";
    const auto done =
        install_plugin(source, root, make_options(make_context()));
    REQUIRE(done.has_value());
    REQUIRE(level_of(done.value().found) ==
            trust_level::third_party_signed);
    REQUIRE(hash_of_folder(done.value().directory) == hash_of_folder(source));
}

TEST_CASE("plugin install: an unsigned plugin installs only with "
          "allow_unsigned")
{
    const scratch_directory scratch;
    const fs::path source = make_plugin(scratch.path(), "source");
    const fs::path root = scratch.path() / "installed";

    install_options options = make_options(make_context());
    require_refused(source, root, options, plugin_errc::signature_missing);

    options.allow_unsigned = true;
    const auto done = install_plugin(source, root, options);
    REQUIRE(done.has_value());
    REQUIRE(level_of(done.value().found) == trust_level::unsigned_plugin);
    REQUIRE(hash_of_folder(done.value().directory) == hash_of_folder(source));
}

TEST_CASE("plugin install: a signed plugin with a key nobody has is treated "
          "as unsigned and installs only with allow_unsigned")
{
    const scratch_directory scratch;
    const fs::path source = make_plugin(scratch.path(), "source");
    sign_plugin(source, "nobody-1", stranger_fill);
    const fs::path root = scratch.path() / "installed";
    install_options options = make_options(make_context());

    std::string detail;
    const inspection found = inspect_plugin(source, options.context);
    REQUIRE(install_refusal(found, false, &detail) ==
            plugin_errc::key_unknown);
    REQUIRE(!detail.empty());
    REQUIRE(!install_refusal(found, true).failed());
    require_refused(source, root, options, plugin_errc::key_unknown);

    options.allow_unsigned = true;
    const auto done = install_plugin(source, root, options);
    REQUIRE(done.has_value());
    REQUIRE(level_of(done.value().found) ==
            trust_level::signed_unknown_key);
}

TEST_CASE("plugin install: a forged signature under an approved key id does "
          "not hold, and a key the user approved is known")
{
    const scratch_directory scratch;
    const fs::path source = make_plugin(scratch.path(), "source");
    // Signed by a stranger but named as the approved key.
    sign_plugin(source, "author-1", stranger_fill);
    install_options options = make_options(make_context());
    require_refused(source, scratch.path() / "installed", options,
                    plugin_errc::signature_invalid);

    // The same key id, signed for real, is known.
    sign_plugin(source, "author-1", author_fill);
    const inspection found = inspect_plugin(source, options.context);
    REQUIRE(found.signature.state == signature_state::valid);
    REQUIRE(!install_refusal(found, false).failed());
}

TEST_CASE("plugin install: an id that cannot be a folder name is refused by "
          "install_refusal")
{
    const scratch_directory scratch;
    spec device;
    device.id = "con.example";
    const fs::path source = make_plugin(scratch.path(), "source", device);
    const inspection found = inspect_plugin(source, make_context());
    REQUIRE(!found.problem.failed());
    std::string detail;
    REQUIRE(install_refusal(found, true, &detail) ==
            plugin_errc::layout_invalid);
    REQUIRE(!detail.empty());
}

TEST_CASE("plugin install: a plugin that does not hold is refused with its "
          "reason and nothing is written")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    install_options options = make_options(make_context());
    // The unsigned check must not hide the real reason.
    options.allow_unsigned = true;

    const fs::path flipped = make_official(scratch.path(), "flipped");
    write_file(flipped / "bin" / "prog", "program bytez");
    require_refused(flipped, root, options, plugin_errc::signature_invalid);

    const fs::path extra = make_official(scratch.path(), "extra");
    write_file(extra / "bin" / "more", "more");
    require_refused(extra, root, options, plugin_errc::unlisted_file);

    const fs::path missing = make_official(scratch.path(), "missing");
    std::error_code ec;
    fs::remove(missing / "bin" / "prog", ec);
    require_refused(missing, root, options, plugin_errc::file_missing);

    const fs::path wrong = make_plugin(scratch.path(), "wrong");
    sign_plugin(wrong, "author-1", stranger_fill);
    require_refused(wrong, root, options, plugin_errc::signature_invalid);

    write_file(scratch.path() / "badjson" / "plugin.json", "{ not JSON");
    require_refused(scratch.path() / "badjson", root, options,
                    plugin_errc::manifest_invalid);

    // A plugin that lists its program by a path that leaves the folder.
    std::string leaving = manifest_text({});
    for (std::size_t at = leaving.find("bin/prog"); at != std::string::npos;
         at = leaving.find("bin/prog"))
        leaving.replace(at, 8, "../prog");
    write_file(scratch.path() / "layout" / "plugin.json", leaving);
    const auto done =
        install_plugin(scratch.path() / "layout", root, options);
    REQUIRE(done.has_error());
    REQUIRE(names_in(root).empty());
}

TEST_CASE("plugin install: a revoked plugin is refused")
{
    const scratch_directory scratch;
    const fs::path source = make_third_party(scratch.path(), "source");
    inspect_context context = make_context();
    revocation_list list;
    list.serial = 1;
    list.plugin_hashes.push_back(hash_of_folder(source));
    context.revocations = list;
    require_refused(source, scratch.path() / "installed",
                    make_options(context), plugin_errc::revoked);
}

TEST_CASE("plugin install: an expired plugin installs and the report says "
          "it will not run")
{
    const scratch_directory scratch;
    spec old;
    old.not_after = "2026-01-01";
    const fs::path source = make_third_party(scratch.path(), "source", old);
    const auto done = install_plugin(source, scratch.path() / "installed",
                                     make_options(make_context()));
    REQUIRE(done.has_value());
    REQUIRE(done.value().found.run == plugin_errc::expired);
}

TEST_CASE("plugin install: a link in the source is refused")
{
    const scratch_directory scratch;
    const fs::path source = make_official(scratch.path(), "source");
    write_file(scratch.path() / "outside.txt", "secret");
    if (!make_file_link(scratch.path() / "outside.txt",
                        source / "bin" / "link"))
        VSS_SKIP("symbolic links cannot be made here");
    require_refused(source, scratch.path() / "installed",
                    make_options(make_context()), plugin_errc::layout_invalid);
}

TEST_CASE("plugin install: a source folder that is a link is refused")
{
    const scratch_directory scratch;
    const fs::path real = make_official(scratch.path(), "real");
    std::error_code ec;
    fs::create_directory_symlink(real, scratch.path() / "link", ec);
    if (ec)
        VSS_SKIP("symbolic links cannot be made here");
    require_refused(scratch.path() / "link", scratch.path() / "installed",
                    make_options(make_context()), plugin_errc::layout_invalid);
}

TEST_CASE("plugin install: the same id is refused unless replace is set")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    spec first;
    first.program = "old program";
    const fs::path one = make_third_party(scratch.path(), "one", first);
    spec second;
    second.program = "new program";
    const fs::path two = make_third_party(scratch.path(), "two", second);

    install_options options = make_options(make_context());
    REQUIRE(install_plugin(one, root, options).has_value());

    std::string detail;
    const auto clash = install_plugin(two, root, options, &detail);
    REQUIRE(clash.has_error());
    REQUIRE(clash.error() == errc::already_exists);
    REQUIRE(!detail.empty());
    REQUIRE_EQ(read_file(root / "com.example.modbus" / "bin" / "prog"),
               std::string("old program"));
    REQUIRE_EQ(names_in(root).size(), std::size_t{1});

    options.replace = true;
    const auto done = install_plugin(two, root, options);
    REQUIRE(done.has_value());
    REQUIRE(done.value().replaced);
    REQUIRE_EQ(read_file(root / "com.example.modbus" / "bin" / "prog"),
               std::string("new program"));
    // The old copy is gone, and nothing else is left.
    REQUIRE_EQ(names_in(root).size(), std::size_t{1});
    REQUIRE(hash_of_folder(root / "com.example.modbus") ==
            hash_of_folder(two));
}

namespace
{

/// File operations that fail on the call number given (counting both
/// renames, from 1) and otherwise do the real thing.
struct failing_operations
{
    int fail_rename_at = 0;
    bool fail_remove = false;
    /// The failing rename is the program dying: from then on nothing is
    /// renamed or removed, so the files are as they were at that moment.
    bool crash = false;
    bool crashed = false;
    int renames = 0;
    file_operations real = default_file_operations();

    file_operations operations()
    {
        file_operations out;
        out.rename = [this](const fs::path& from, const fs::path& to)
        {
            ++renames;
            if (crashed)
                return make_error_code(errc::io_error);
            if (renames == fail_rename_at)
            {
                crashed = crash;
                return make_error_code(errc::io_error);
            }
            return real.rename(from, to);
        };
        out.remove_all = [this](const fs::path& path)
        {
            if (fail_remove || crashed)
                return make_error_code(errc::io_error);
            return real.remove_all(path);
        };
        return out;
    }
};

/// What discovery finds in `root`: the folder names of the plugins, with
/// nothing wrong with them.
std::vector<std::string> found_in(const fs::path& root)
{
    plugin_root entry;
    entry.directory = root;
    entry.scope = plugin_scope::for_user;
    const discovery found = discover_plugins(std::span(&entry, 1));
    std::vector<std::string> out;
    for (const found_plugin& each : found.plugins)
    {
        REQUIRE(!each.problem.failed());
        REQUIRE(!each.shadowed_by.has_value());
        REQUIRE(each.directory.filename().string().front() != '.');
        out.push_back(each.directory.filename().string() + " " +
                      to_hex(sha512_of(read_file(each.directory / "bin" /
                                                 "prog"))).substr(0, 8));
    }
    return out;
}

std::string id_and(std::string_view program)
{
    return "com.example.modbus " +
           to_hex(sha512_of(program)).substr(0, 8);
}

} // namespace

TEST_CASE("plugin install: a failure between the two renames of a replace "
          "puts the old plugin back")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    spec first;
    first.program = "old program";
    const fs::path one = make_third_party(scratch.path(), "one", first);
    spec second;
    second.program = "new program";
    const fs::path two = make_third_party(scratch.path(), "two", second);
    install_options options = make_options(make_context());
    REQUIRE(install_plugin(one, root, options).has_value());
    const plugin_hash before = hash_of_folder(root / "com.example.modbus");

    // Renames: the old one aside (1), the new one in (2), then the old one
    // back (3).
    failing_operations fails;
    fails.fail_rename_at = 2;
    const file_operations operations = fails.operations();
    options.operations = &operations;
    options.replace = true;
    std::string detail;
    const auto done = install_plugin(two, root, options, &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == errc::io_error);
    REQUIRE(detail.find("old copy is as it was") != std::string::npos);
    REQUIRE_EQ(fails.renames, 3);
    REQUIRE(hash_of_folder(root / "com.example.modbus") == before);
    REQUIRE_EQ(read_file(root / "com.example.modbus" / "bin" / "prog"),
               std::string("old program"));
    REQUIRE_EQ(names_in(root).size(), std::size_t{1});
}

TEST_CASE("plugin install: a failure of the first rename of a replace leaves "
          "the old plugin and no temporary folder")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    const fs::path one = make_third_party(scratch.path(), "one");
    spec second;
    second.program = "new program";
    const fs::path two = make_third_party(scratch.path(), "two", second);
    install_options options = make_options(make_context());
    REQUIRE(install_plugin(one, root, options).has_value());

    failing_operations fails;
    fails.fail_rename_at = 1;
    const file_operations operations = fails.operations();
    options.operations = &operations;
    options.replace = true;
    const auto done = install_plugin(two, root, options);
    REQUIRE(done.has_error());
    REQUIRE_EQ(read_file(root / "com.example.modbus" / "bin" / "prog"),
               std::string(program_text));
    REQUIRE_EQ(names_in(root).size(), std::size_t{1});
}

TEST_CASE("plugin install: a failed rename of a new install leaves nothing")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    const fs::path source = make_third_party(scratch.path(), "source");

    failing_operations fails;
    fails.fail_rename_at = 1;
    const file_operations operations = fails.operations();
    install_options options = make_options(make_context());
    options.operations = &operations;
    const auto done = install_plugin(source, root, options);
    REQUIRE(done.has_error());
    REQUIRE(names_in(root).empty());
}

TEST_CASE("plugin install: an old copy that cannot be removed is an error "
          "that names what is left, and discovery finds only the new plugin")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    const fs::path one = make_third_party(scratch.path(), "one");
    spec second;
    second.program = "new program";
    const fs::path two = make_third_party(scratch.path(), "two", second);
    install_options options = make_options(make_context());
    REQUIRE(install_plugin(one, root, options).has_value());

    failing_operations fails;
    fails.fail_remove = true;
    const file_operations operations = fails.operations();
    options.operations = &operations;
    options.replace = true;
    std::string detail;
    const auto done = install_plugin(two, root, options, &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == errc::io_error);
    REQUIRE(detail.find(".vss-work") != std::string::npos);
    REQUIRE(detail.find("old copy") != std::string::npos);
    REQUIRE_EQ(read_file(root / "com.example.modbus" / "bin" / "prog"),
               std::string("new program"));
    REQUIRE(fs::exists(root / ".vss-work"));
    REQUIRE_EQ(found_in(root), std::vector<std::string>{id_and("new program")});

    // The next install clears what was left.
    options.operations = nullptr;
    const auto again = install_plugin(two, root, options);
    REQUIRE(again.has_value());
    REQUIRE_EQ(names_in(root), std::vector<std::string>{"com.example.modbus"});
}

TEST_CASE("plugin install: after a crash at each step discovery finds the "
          "right version or none, and the next install clears the leftovers")
{
    spec second;
    second.program = "new program";

    struct step
    {
        std::string name;
        int fail_rename_at = 0;
        bool fail_remove = false;
        bool replace = false;
        std::vector<std::string> found;
    };
    const std::vector<step> steps{
        // A new install cut off after the copy: a whole plugin sits in the
        // work folder and is not found.
        {"new install, before the rename", 1, false, false, {}},
        // A replace cut off before the old plugin is moved: the old one.
        {"replace, before the old copy is moved",
         1,
         false,
         true,
         {id_and(program_text)}},
        // Cut off with the old plugin aside and the new one not in place:
        // neither is found, and neither is lost.
        {"replace, between the two renames", 2, false, true, {}},
        // Cut off after both renames: the new plugin, whose old copy is
        // still in the work folder.
        {"replace, before the old copy is removed",
         0,
         true,
         true,
         {id_and("new program")}},
    };
    for (const step& at : steps)
    {
        const scratch_directory scratch;
        const fs::path root = scratch.path() / "installed";
        const fs::path one = make_third_party(scratch.path(), "one");
        const fs::path two = make_third_party(scratch.path(), "two", second);
        install_options options = make_options(make_context());
        if (at.replace)
            REQUIRE(install_plugin(one, root, options).has_value());

        failing_operations fails;
        fails.fail_rename_at = at.fail_rename_at;
        fails.fail_remove = at.fail_remove;
        fails.crash = true;
        const file_operations operations = fails.operations();
        options.operations = &operations;
        options.replace = at.replace;
        REQUIRE(install_plugin(two, root, options).has_error());
        REQUIRE_EQ(found_in(root), at.found);

        options.operations = nullptr;
        options.replace = true;
        const auto again = install_plugin(two, root, options);
        REQUIRE(again.has_value());
        REQUIRE_EQ(names_in(root),
                   std::vector<std::string>{"com.example.modbus"});
        REQUIRE_EQ(found_in(root),
                   std::vector<std::string>{id_and("new program")});
    }
}

TEST_CASE("plugin discovery: an entry whose name starts with a dot is never a "
          "plugin")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    const fs::path source = make_third_party(scratch.path(), "source");
    fs::create_directories(root);
    fs::copy(source, root / ".hidden", fs::copy_options::recursive);
    fs::create_directories(root / ".vss-work" / "i-1");
    fs::copy(source, root / ".vss-work" / "i-1" / "new",
             fs::copy_options::recursive);
    REQUIRE(found_in(root).empty());
}

TEST_CASE("plugin install: the source is changed after it was looked at and "
          "the copy is refused as changed")
{
    for (const std::string_view file :
         {"plugin.json", "plugin.sig", "bin/prog"})
    {
        const scratch_directory scratch;
        const fs::path source = make_third_party(scratch.path(), "source");
        const fs::path root = scratch.path() / "installed";
        file_operations operations = default_file_operations();
        operations.inspected = [&](const fs::path& folder)
        {
            // Same size, other bytes, for the program; another text for the
            // others.
            std::string text = read_file(folder / std::string(file));
            text.back() = text.back() == 'x' ? 'y' : 'x';
            write_file(folder / std::string(file), text);
        };
        install_options options = make_options(make_context());
        options.operations = &operations;
        std::string detail;
        const auto done = install_plugin(source, root, options, &detail);
        REQUIRE(done.has_error());
        REQUIRE(done.error() == plugin_errc::changed);
        REQUIRE(!detail.empty());
        REQUIRE(names_in(root).empty());
    }
}

TEST_CASE("plugin install: the id is used as a folder name only when it "
          "cannot reach outside the folder or name a device")
{
    // The manifest reader already refuses ids with path characters.
    for (const std::string_view text :
         {std::string_view{".."}, std::string_view{"../evil.plugin"},
          std::string_view{"a/b.c"}, std::string_view{"a\\b.c"},
          std::string_view{"/abs.plugin"}, std::string_view{"c:evil.plugin"},
          std::string_view{"com.example/../../x"},
          std::string_view{".hidden.plugin"}, std::string_view{"a..b"},
          std::string_view{"com.example."},
          std::string_view{"con.example name"}})
    {
        // The id is put into the JSON text with its backslashes doubled, so
        // that it is the id that is refused and not the JSON.
        std::string escaped;
        for (const char c : text)
        {
            if (c == '\\')
                escaped += '\\';
            escaped += c;
        }
        spec what;
        what.id = escaped;
        const auto parsed = parse_manifest(manifest_text(what));
        REQUIRE(parsed.has_error());
        REQUIRE(!usable_as_folder_name(text));
    }

    // Names of devices on Windows are refused on every system.
    for (const char* text :
         {"con.example", "nul.example", "prn.example", "aux.example",
          "com1.example", "lpt9.example", "COM1.example"})
    {
        REQUIRE(!usable_as_folder_name(text));
    }
    REQUIRE(usable_as_folder_name("com.example.modbus"));
    REQUIRE(usable_as_folder_name("con-sulting.example"));
    REQUIRE(usable_as_folder_name("com10.example"));

    // A plugin whose id passes the manifest reader but names a device is
    // refused by the install, and nothing is written.
    const scratch_directory scratch;
    spec device;
    device.id = "nul.example";
    const fs::path source = make_plugin(scratch.path(), "source", device);
    install_options options = make_options(make_context());
    options.allow_unsigned = true;
    require_refused(source, scratch.path() / "installed", options,
                    plugin_errc::layout_invalid);
}

TEST_CASE("plugin install: the machine folder must not be writable by "
          "others")
{
    // A folder of the scratch directory can be changed by the user who made
    // it, who is not an administrator, on every system.
    const scratch_directory scratch;
    const fs::path source = make_third_party(scratch.path(), "source");
    install_options options = make_options(make_context());
    options.for_everyone = true;
    std::string detail;
    const auto done =
        install_plugin(source, scratch.path() / "machine", options, &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == errc::permission_denied);
    REQUIRE(!detail.empty());
    // Not even the folder is made.
    REQUIRE(!fs::exists(scratch.path() / "machine"));
}

// -- remove -----------------------------------------------------------------

TEST_CASE("plugin remove: removes the plugin's folder and nothing else")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    const fs::path one = make_third_party(scratch.path(), "one");
    spec other;
    other.id = "com.example.other";
    const fs::path two = make_third_party(scratch.path(), "two", other);
    const install_options options = make_options(make_context());
    REQUIRE(install_plugin(one, root, options).has_value());
    REQUIRE(install_plugin(two, root, options).has_value());
    write_file(root / "data" / "state.bin", "kept");

    std::string detail;
    const auto done = remove_plugin(root, "com.example.modbus", {},
                                    &detail);
    REQUIRE(done.has_value());
    REQUIRE(!fs::exists(root / "com.example.modbus"));
    REQUIRE(fs::exists(root / "com.example.other" / "plugin.json"));
    REQUIRE_EQ(read_file(root / "data" / "state.bin"), std::string("kept"));

    const auto again = remove_plugin(root, "com.example.modbus", {},
                                     &detail);
    REQUIRE(again.has_error());
    REQUIRE(again.error() == errc::not_found);
}

TEST_CASE("plugin remove: the machine folder must not be writable by others")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    const fs::path source = make_third_party(scratch.path(), "source");
    REQUIRE(install_plugin(source, root, make_options(make_context()))
                .has_value());
    remove_options options;
    options.for_everyone = true;
    std::string detail;
    const auto done =
        remove_plugin(root, "com.example.modbus", options, &detail);
    REQUIRE(done.has_error());
    REQUIRE(done.error() == errc::permission_denied);
    REQUIRE(!detail.empty());
    REQUIRE(fs::exists(root / "com.example.modbus" / "plugin.json"));
    REQUIRE(remove_plugin(scratch.path() / "missing", "com.example.modbus",
                          options)
                .error() == errc::permission_denied);
    REQUIRE(!fs::exists(scratch.path() / "missing"));
}

TEST_CASE("plugin remove: an id that is not a plugin id, or a folder that is "
          "not a plugin, is refused")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    write_file(root / "keep" / "file.txt", "kept");
    write_file(root / "com.example.notplugin" / "file.txt", "kept");
    write_file(scratch.path() / "outside" / "plugin.json", "{}");

    for (const char* id : {"", "..", "keep", "../outside", "a/b.c", "a\\b.c",
                           "nul.example", "COM1.example", "."})
    {
        const auto done = remove_plugin(root, id);
        REQUIRE(done.has_error());
        REQUIRE(done.error() == errc::invalid_argument);
    }
    REQUIRE(fs::exists(root / "keep" / "file.txt"));
    REQUIRE(fs::exists(scratch.path() / "outside" / "plugin.json"));

    std::string detail;
    const auto not_plugin =
        remove_plugin(root, "com.example.notplugin", {}, &detail);
    REQUIRE(not_plugin.has_error());
    REQUIRE(not_plugin.error() == plugin_errc::layout_invalid);
    REQUIRE(!detail.empty());
    REQUIRE(fs::exists(root / "com.example.notplugin" / "file.txt"));
}

TEST_CASE("plugin remove: a folder that is a link is not followed")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    const fs::path real = make_third_party(scratch.path(), "real");
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::create_directory_symlink(real, root / "com.example.modbus", ec);
    if (ec)
        VSS_SKIP("symbolic links cannot be made here");
    const auto done = remove_plugin(root, "com.example.modbus");
    REQUIRE(done.has_error());
    REQUIRE(done.error() == plugin_errc::layout_invalid);
    REQUIRE(fs::exists(real / "plugin.json"));
}

TEST_CASE("plugin install: the official key list built in is accepted")
{
    std::string detail;
    const auto list = builtin_key_list(&detail);
    REQUIRE(list.has_value());
    REQUIRE(list.value().find("plugins-2026", plugin_purpose) != nullptr);
}

TEST_CASE("plugin install: a context for this computer has the built-in list "
          "and no approved key")
{
    std::vector<std::string> notes;
    computer_options options;
    options.today = today;
    const inspect_context context = context_for_this_computer(options, &notes);
    REQUIRE(context.official.has_value());
    REQUIRE(context.today == today);
    REQUIRE(context.user_keys.empty());
    REQUIRE(context.administrator_keys.empty());
    REQUIRE(context.sandbox == sandbox_level::none);
    REQUIRE(!context.licence_known);
    REQUIRE(!context.rules.has_value());
    REQUIRE(notes.empty());
}

TEST_CASE("plugin install: a context for this computer reads the trust store "
          "and the policy")
{
    const scratch_directory scratch;
    trust_store store;
    approved_key key;
    key.key_id = "author-1";
    key.public_key = public_of(author_fill);
    key.name = "Example Ltd";
    key.approved_on = 2026y / 11 / 10;
    REQUIRE(store.approve_key(key).has_value());
    write_file(scratch.path() / "trust.json", to_json(store));
    // A policy file that cannot be used: it is not a policy.
    write_file(scratch.path() / "policy.json", "{ not JSON");

    std::vector<std::string> notes;
    computer_options options;
    options.today = today;
    options.trust_file = scratch.path() / "trust.json";
    options.policy_file = scratch.path() / "policy.json";
    const inspect_context context = context_for_this_computer(options, &notes);
    REQUIRE_EQ(context.user_keys.size(), std::size_t{1});
    REQUIRE_EQ(context.user_keys[0].key_id, std::string("author-1"));
    // A policy that cannot be used leaves the official-only policy.
    REQUIRE(context.rules.has_value());
    REQUIRE(context.rules.value() == official_only_policy());
    REQUIRE(!notes.empty());

    // So a third-party plugin does not run under it.
    const fs::path directory = make_third_party(scratch.path(), "third");
    const inspection found = inspect_plugin(directory, context);
    REQUIRE(level_of(found) == trust_level::third_party_signed);
    REQUIRE(found.run == plugin_errc::policy);
}

TEST_CASE("plugin install: the trust file is trust.json in the user's folder")
{
    // Studio and every command read one file.
    const fs::path folder = vss::machine::user_folder();
    if (folder.empty())
        REQUIRE(trust_file_of_this_user().empty());
    else
        REQUIRE(trust_file_of_this_user() == folder / "trust.json");
}
