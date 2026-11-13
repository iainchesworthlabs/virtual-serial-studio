//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace vss;
using namespace vss::plugin;

namespace
{

/// A file as a test measures it: the digest is of the given content.
file_entry file(std::string path, std::string_view content)
{
    file_entry entry;
    entry.path = std::move(path);
    entry.size = content.size();
    entry.sha512 = sha512_of(content);
    return entry;
}

/// A manifest that runs `bin/plugin` and lists `listed`.
manifest declared(std::vector<file_entry> listed)
{
    manifest value;
    value.id = "com.example.plugin";
    value.run.executable = "bin/plugin";
    value.files = std::move(listed);
    return value;
}

std::vector<file_entry> good_directory()
{
    return {file("plugin.json", "{}"), file("plugin.sig", "signature"),
            file("bin/plugin", "program"), file("data/table.bin", "table")};
}

manifest good_manifest()
{
    return declared(
        {file("bin/plugin", "program"), file("data/table.bin", "table")});
}

void require_refused(const manifest& value,
                     const std::vector<file_entry>& measured,
                     plugin_errc expected, std::string_view part)
{
    std::string detail;
    const auto checked = check_files(value, measured, &detail);
    REQUIRE(checked.has_error());
    REQUIRE(checked.error() == expected);
    REQUIRE(detail.find(part) != std::string::npos);
}

} // namespace

TEST_CASE("plugin hash: SHA-512 gives the published values")
{
    // FIPS 180-4 examples: the empty message and "abc".
    REQUIRE_EQ(to_hex(sha512_of(std::string_view{})),
               "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9"
               "ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927"
               "da3e");
    REQUIRE_EQ(to_hex(sha512_of(std::string_view("abc"))),
               "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d3"
               "9a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54c"
               "a49f");
}

TEST_CASE("plugin hash: bytes in parts hash as the whole")
{
    const std::string text(1000, 'x');
    sha512_hasher hasher;
    hasher.update(std::string_view(text).substr(0, 1));
    hasher.update(std::string_view(text).substr(1, 127));
    hasher.update(std::as_bytes(std::span(text)).subspan(128));
    hasher.update(std::string_view{});
    REQUIRE(hasher.finish() == sha512_of(text));
    REQUIRE(sha512_of(std::as_bytes(std::span(text))) == sha512_of(text));
}

TEST_CASE("plugin hash: the hash does not depend on the order of the files")
{
    std::vector<file_entry> files = good_directory();
    const plugin_hash first = hash_plugin(files);
    std::ranges::reverse(files);
    REQUIRE(hash_plugin(files) == first);
    std::ranges::rotate(files, files.begin() + 1);
    REQUIRE(hash_plugin(files) == first);
    REQUIRE_EQ(first.short_text().size(), std::size_t{16});
    REQUIRE_EQ(first.short_text(), to_hex(first.bytes).substr(0, 16));
}

TEST_CASE("plugin hash: the hash follows its definition")
{
    const std::vector<file_entry> files = {file("b", "two"), file("a", "1")};
    std::string lines;
    lines += std::string("a") + '\0' + "1" + '\0' +
             to_hex(sha512_of(std::string_view("1"))) + "\n";
    lines += std::string("b") + '\0' + "3" + '\0' +
             to_hex(sha512_of(std::string_view("two"))) + "\n";
    REQUIRE(hash_plugin(files).bytes == sha512_of(lines));
}

TEST_CASE("plugin hash: any change to the files changes the hash")
{
    const std::vector<file_entry> base = good_directory();
    const plugin_hash hash = hash_plugin(base);

    std::vector<file_entry> content = base;
    content[2] = file("bin/plugin", "progrAm");
    REQUIRE(hash_plugin(content) != hash);

    std::vector<file_entry> renamed = base;
    renamed[3].path = "data/table2.bin";
    REQUIRE(hash_plugin(renamed) != hash);

    std::vector<file_entry> added = base;
    added.push_back(file("extra", ""));
    REQUIRE(hash_plugin(added) != hash);

    std::vector<file_entry> removed = base;
    removed.pop_back();
    REQUIRE(hash_plugin(removed) != hash);

    std::vector<file_entry> resized = base;
    resized[3].size += 1;
    REQUIRE(hash_plugin(resized) != hash);

    std::vector<file_entry> manifest_changed = base;
    manifest_changed[0] = file("plugin.json", "{ }");
    REQUIRE(hash_plugin(manifest_changed) != hash);

    std::vector<file_entry> resigned = base;
    resigned[1] = file("plugin.sig", "another");
    REQUIRE(hash_plugin(resigned) != hash);
}

TEST_CASE("plugin hash: a path and a size cannot be read two ways")
{
    // "a" with size 12 and "a1" with size 2 must not hash alike.
    file_entry one = file("a", "");
    one.size = 12;
    file_entry two = file("a1", "");
    two.size = 2;
    REQUIRE(hash_plugin(std::vector{one}) != hash_plugin(std::vector{two}));
    REQUIRE(hash_plugin(std::vector<file_entry>{}).bytes ==
            sha512_of(std::string_view{}));
}

TEST_CASE("plugin hash: files that match the manifest are accepted")
{
    std::string detail = "old";
    REQUIRE(
        check_files(good_manifest(), good_directory(), &detail).has_value());
    REQUIRE(detail.empty());
    REQUIRE(check_files(good_manifest(), good_directory()).has_value());

    // Without a signature file, and in another order.
    std::vector<file_entry> files = good_directory();
    files.erase(files.begin() + 1);
    std::ranges::reverse(files);
    REQUIRE(check_files(good_manifest(), files).has_value());
}

TEST_CASE("plugin hash: a manifest with no list accepts other files")
{
    std::vector<file_entry> files = good_directory();
    files.push_back(file("notes/readme.txt", "hello"));
    REQUIRE(check_files(declared({}), files).has_value());
}

TEST_CASE("plugin hash: the program must be there even with no list")
{
    const std::vector<file_entry> files = {file("plugin.json", "{}")};
    require_refused(declared({}), files, plugin_errc::file_missing,
                    "\"bin/plugin\"");

    // A name that differs only in case is another file.
    const std::vector<file_entry> other = {file("plugin.json", "{}"),
                                           file("bin/Plugin", "program")};
    require_refused(declared({}), other, plugin_errc::file_missing,
                    "\"bin/plugin\"");
}

TEST_CASE("plugin hash: a plugin without its manifest file is refused")
{
    std::vector<file_entry> files = good_directory();
    files.erase(files.begin());
    require_refused(good_manifest(), files, plugin_errc::layout_invalid,
                    "plugin.json");

    files.push_back(file("Plugin.json", "{}"));
    require_refused(good_manifest(), files, plugin_errc::layout_invalid,
                    "plugin.json");
}

TEST_CASE("plugin hash: a listed file that is not there is refused")
{
    std::vector<file_entry> files = good_directory();
    files.pop_back();
    require_refused(good_manifest(), files, plugin_errc::file_missing,
                    "\"data/table.bin\"");
}

TEST_CASE("plugin hash: a changed file is refused")
{
    std::vector<file_entry> content = good_directory();
    content[3] = file("data/table.bin", "tablE");
    require_refused(good_manifest(), content, plugin_errc::signature_invalid,
                    "\"data/table.bin\"");

    std::vector<file_entry> longer = good_directory();
    longer[2] = file("bin/plugin", "program and more");
    require_refused(good_manifest(), longer, plugin_errc::signature_invalid,
                    "16 bytes and the manifest says 7");

    // The right digest with the wrong size is still refused.
    std::vector<file_entry> size_only = good_directory();
    size_only[2].size = 8;
    require_refused(good_manifest(), size_only,
                    plugin_errc::signature_invalid, "\"bin/plugin\"");
}

TEST_CASE("plugin hash: a file the manifest does not list is refused")
{
    std::vector<file_entry> files = good_directory();
    files.push_back(file("bin/helper", "more code"));
    require_refused(good_manifest(), files, plugin_errc::unlisted_file,
                    "\"bin/helper\"");

    // The two own files are exact names: another spelling is a file.
    std::vector<file_entry> spelled = good_directory();
    spelled[1].path = "PLUGIN.SIG";
    require_refused(good_manifest(), spelled, plugin_errc::unlisted_file,
                    "\"PLUGIN.SIG\"");

    std::vector<file_entry> nested = good_directory();
    nested.push_back(file("data/plugin.json", "{}"));
    require_refused(good_manifest(), nested, plugin_errc::unlisted_file,
                    "\"data/plugin.json\"");
}

TEST_CASE("plugin hash: a path that leaves the plugin is refused")
{
    for (const std::string_view bad :
         {"../outside", "/etc/passwd", "bin\\plugin2", "C:/x", "a//b", "NUL",
          "bin/", ""})
    {
        std::vector<file_entry> files = good_directory();
        files.push_back(file(std::string(bad), "x"));
        std::string detail;
        const auto checked = check_files(good_manifest(), files, &detail);
        REQUIRE(checked.has_error());
        REQUIRE(checked.error() == plugin_errc::layout_invalid);
        REQUIRE(!detail.empty());
    }
}

TEST_CASE("plugin hash: the same file twice, or a case clash, is refused")
{
    std::vector<file_entry> twice = good_directory();
    twice.push_back(file("bin/plugin", "program"));
    require_refused(good_manifest(), twice, plugin_errc::layout_invalid,
                    "is there twice");

    std::vector<file_entry> clash = good_directory();
    clash.push_back(file("Data/table.bin", "table"));
    require_refused(good_manifest(), clash, plugin_errc::layout_invalid,
                    "differ only in case");
    require_refused(declared({}), clash, plugin_errc::layout_invalid,
                    "differ only in case");
}

TEST_CASE("plugin hash: more files than the limit are refused")
{
    std::vector<file_entry> files = good_directory();
    const file_entry blank = file("x", "");
    for (std::size_t i = files.size(); i < max_files + 3; ++i)
    {
        file_entry next = blank;
        next.path = "f/" + std::to_string(i);
        files.push_back(std::move(next));
    }
    require_refused(declared({}), files, plugin_errc::layout_invalid,
                    "more files than the limit");
    files.pop_back();
    REQUIRE(check_files(declared({}), files).has_value());
}

TEST_CASE("plugin hash: only the official prefix is a reserved id")
{
    REQUIRE(is_reserved_id("com.virtualserialstudio.diagnostics"));
    REQUIRE(is_reserved_id("com.virtualserialstudio."));
    REQUIRE(!is_reserved_id("com.virtualserialstudio"));
    REQUIRE(!is_reserved_id("com.virtualserialstudios.x"));
    REQUIRE(!is_reserved_id("org.example.com.virtualserialstudio.x"));
    REQUIRE(!is_reserved_id(""));
}
