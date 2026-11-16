//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The update feed (docs/updates.md, section 4): the payload reader and its
// every rule, the signed envelope around it, and the keys built in. Signing
// uses the THROWAWAY TEST KEY of RFC 8032 (`vss::license::test_keys`); it
// signs nothing outside tests.
//

#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/license/test_keys.hpp>
#include <vss/update/error.hpp>
#include <vss/update/feed.hpp>

#include "test_framework.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vss::update;
using namespace std::chrono_literals;
namespace sf = vss::license::signed_file;
namespace signing = vss::license::signing;
namespace test_keys = vss::license::test_keys;

namespace
{

/// The version `text` names. A test fails when it names none.
version v(std::string_view text)
{
    const auto parsed = parse_version(text);
    if (!parsed)
        throw vss_test::failure("not a version: " + std::string(text));
    return *parsed;
}

std::string hash_of(char digit)
{
    return std::string(128, digit);
}

/// A feed with every kind of entry: two Studio releases, and two plugins, the
/// second with no summary and no files.
std::string good_payload()
{
    std::string text = R"json({
  "feed_version": 1,
  "serial": 12,
  "issued_at": "2026-12-01T03:40:00Z",
  "studio": [
    {
      "channel": "stable",
      "version": "0.2.0",
      "released": "2026-12-01",
      "summary": "What is new.",
      "notes_url": "https://example.org/notes/0.2.0",
      "files": [
        {
          "kind": "windows-installer",
          "arch": "x86_64",
          "name": "studio-0.2.0-windows-x86_64.exe",
          "url": "https://example.org/dl/studio-0.2.0-windows-x86_64.exe",
          "size": 1000,
          "sha512": "@A@"
        },
        {
          "kind": "linux-deb",
          "arch": "arm64",
          "name": "studio-0.2.0-arm64.deb",
          "url": "https://example.org/dl/studio-0.2.0-arm64.deb",
          "size": 2000,
          "sha512": "@B@"
        }
      ]
    },
    {
      "channel": "preview",
      "version": "0.3.0-rc.1",
      "released": "2026-12-09",
      "notes_url": "https://example.org/notes/0.3.0-rc.1",
      "files": [
        {
          "kind": "macos-dmg",
          "arch": "arm64",
          "name": "studio-0.3.0-rc.1-arm64.dmg",
          "url": "https://example.org/dl/studio-0.3.0-rc.1-arm64.dmg",
          "size": 3000,
          "sha512": "@C@"
        }
      ]
    }
  ],
  "plugins": [
    {
      "id": "com.virtualserialstudio.decoding",
      "version": "1.3.0",
      "released": "2026-12-02",
      "requires_studio": "0.2.0",
      "summary": "Plugin news.",
      "notes_url": "https://example.org/notes/decoding-1.3.0",
      "files": [
        {
          "os": "windows",
          "arch": "x86_64",
          "name": "decoding-1.3.0-windows-x86_64.zip",
          "url": "https://example.org/dl/decoding-1.3.0-windows-x86_64.zip",
          "size": 4000,
          "sha512": "@D@"
        }
      ]
    },
    {
      "id": "com.virtualserialstudio.export",
      "version": "2.0.0-beta.2",
      "released": "2026-12-03",
      "requires_studio": "0.3.0-rc.1",
      "notes_url": "https://example.org/notes/export-2.0.0",
      "files": []
    }
  ]
})json";
    const std::array<std::pair<std::string_view, char>, 4> hashes{
        {{"@A@", 'a'}, {"@B@", 'b'}, {"@C@", 'c'}, {"@D@", 'd'}}};
    for (const auto& [token, digit] : hashes)
    {
        const std::size_t at = text.find(token);
        text.replace(at, token.size(), hash_of(digit));
    }
    return text;
}

/// `text` with the first `from` replaced by `to`. The edit must find it.
std::string edit(std::string text, std::string_view from, std::string_view to)
{
    const std::size_t at = text.find(from);
    if (at == std::string::npos)
        throw vss_test::failure("the payload has no " + std::string(from));
    text.replace(at, from.size(), to);
    return text;
}

/// `text` without the member `fragment` (`"key": value`, no comma) and the
/// comma that separated it from its neighbour.
std::string drop(std::string text, std::string_view fragment)
{
    const std::size_t at = text.find(fragment);
    if (at == std::string::npos)
        throw vss_test::failure("the payload has no " + std::string(fragment));
    std::size_t end = at + fragment.size();
    std::size_t next = end;
    while (next < text.size() && (text[next] == ' ' || text[next] == '\n'))
        ++next;
    if (next < text.size() && text[next] == ',')
    {
        text.erase(at, next + 1 - at);
        return text;
    }
    // The last member: the comma before it goes instead.
    std::size_t before = at;
    while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == '\n'))
        --before;
    if (before == 0 || text[before - 1] != ',')
        throw vss_test::failure("no comma beside " + std::string(fragment));
    text.erase(before - 1, end - (before - 1));
    return text;
}

std::string member(std::string_view key, std::string_view value)
{
    return "\"" + std::string(key) + "\": " + std::string(value);
}

std::string sha_member(char digit)
{
    return member("sha512", "\"" + hash_of(digit) + "\"");
}

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

std::string code_name(const result<feed>& outcome)
{
    if (outcome)
        return "accepted";
    return std::string(
        to_string(static_cast<update_errc>(outcome.error().value())));
}

bool accepted(const std::string& payload)
{
    return parse_feed_payload(payload).has_value();
}

feed must_parse(const std::string& payload)
{
    std::string detail;
    auto parsed = parse_feed_payload(payload, &detail);
    if (!parsed)
        throw vss_test::failure("refused: " + detail);
    return std::move(*parsed);
}

struct bad_case
{
    std::string name;
    std::string payload;
    /// Part of the line for a person, or empty when it is not checked.
    std::string detail;
    update_errc code = update_errc::feed_malformed;
};

std::vector<bad_case>& cases()
{
    static std::vector<bad_case> list;
    return list;
}

/// A payload that must be refused as `feed_malformed`.
void bad(std::string name, std::string payload, std::string detail = {})
{
    cases().push_back({std::move(name), std::move(payload), std::move(detail),
                       update_errc::feed_malformed});
}

/// `count` plugins with distinct ids and nothing else in the feed.
std::string many_plugins(std::size_t count)
{
    std::string text =
        R"({"feed_version": 1, "serial": 1, "issued_at": "2026-12-01T00:00:00Z", "studio": [], "plugins": [)";
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            text += ", ";
        text +=
            R"({"id": "com.virtualserialstudio.p)" + std::to_string(i) +
            R"(", "version": "1.0.0", "released": "2026-12-01", )" +
            R"("requires_studio": "0.2.0", "notes_url": "https://example.org/n", "files": []})";
    }
    return text + "]}";
}

/// One studio file object, repeated `count` times in a feed.
std::string many_studio_files(std::size_t count)
{
    std::string files;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            files += ", ";
        files +=
            R"({"kind": "linux-rpm", "arch": "arm64", "name": "a.rpm", )"
            R"("url": "https://example.org/a.rpm", "size": 1, "sha512": ")" +
            hash_of('e') + "\"}";
    }
    return R"({"feed_version": 1, "serial": 1, "issued_at": "2026-12-01T00:00:00Z", )"
           R"("studio": [{"channel": "stable", "version": "1.0.0", )"
           R"("released": "2026-12-01", "notes_url": "https://example.org/n", )"
           R"("files": [)" +
           files + "]}]}";
}

/// The feed with its one studio file's `url` replaced.
std::string with_url(std::string_view url)
{
    return edit(good_payload(),
                "\"https://example.org/dl/studio-0.2.0-windows-x86_64.exe\"",
                "\"" + std::string(url) + "\"");
}

std::string with_name(std::string_view name)
{
    return edit(good_payload(), "\"studio-0.2.0-windows-x86_64.exe\"",
                "\"" + std::string(name) + "\"");
}

std::string with_summary(std::string_view json_text)
{
    return edit(good_payload(), "\"What is new.\"",
                "\"" + std::string(json_text) + "\"");
}

} // namespace

// -- the words --------------------------------------------------------------

TEST_CASE("update feed: a device name is the whole part before the first dot")
{
    for (const std::string_view name :
         {"console.exe", "com.exe", "com10.exe", "lpt.zip", "nulls", "a.con",
          "studio-con.exe", "a.b"})
    {
        std::string detail;
        const auto read = parse_feed_payload(with_name(name), &detail);
        REQUIRE(detail.empty());
        REQUIRE(read.has_value());
    }
}

TEST_CASE("update feed: the enumerators read and write the feed's words")
{
    for (const auto value : {channel::stable, channel::preview})
        REQUIRE(channel_from(to_string(value)) == value);
    for (const auto value :
         {file_kind::windows_installer, file_kind::windows_zip,
          file_kind::macos_dmg, file_kind::macos_archive, file_kind::linux_deb,
          file_kind::linux_rpm, file_kind::linux_archive})
        REQUIRE(file_kind_from(to_string(value)) == value);
    for (const auto value : {architecture::x86_64, architecture::arm64})
        REQUIRE(architecture_from(to_string(value)) == value);
    for (const auto value : {operating_system::windows, operating_system::macos,
                             operating_system::linux})
        REQUIRE(operating_system_from(to_string(value)) == value);

    REQUIRE_EQ(to_string(channel::stable), std::string_view("stable"));
    REQUIRE_EQ(to_string(file_kind::windows_installer),
               std::string_view("windows-installer"));
    REQUIRE_EQ(to_string(file_kind::windows_zip),
               std::string_view("windows-zip"));
    REQUIRE_EQ(to_string(file_kind::macos_dmg), std::string_view("macos-dmg"));
    REQUIRE_EQ(to_string(file_kind::macos_archive),
               std::string_view("macos-archive"));
    REQUIRE_EQ(to_string(file_kind::linux_deb), std::string_view("linux-deb"));
    REQUIRE_EQ(to_string(file_kind::linux_rpm), std::string_view("linux-rpm"));
    REQUIRE_EQ(to_string(file_kind::linux_archive),
               std::string_view("linux-archive"));
    REQUIRE_EQ(to_string(architecture::x86_64), std::string_view("x86_64"));
    REQUIRE_EQ(to_string(architecture::arm64), std::string_view("arm64"));
    REQUIRE_EQ(to_string(operating_system::macos), std::string_view("macos"));

    REQUIRE(!channel_from("Stable"));
    REQUIRE(!channel_from("beta"));
    REQUIRE(!channel_from(""));
    REQUIRE(!file_kind_from("windows_installer"));
    REQUIRE(!file_kind_from("linux-snap"));
    REQUIRE(!architecture_from("amd64"));
    REQUIRE(!architecture_from("x86-64"));
    REQUIRE(!operating_system_from("freebsd"));
    REQUIRE(!operating_system_from("Windows"));
}

TEST_CASE("update feed: the error codes have names and messages")
{
    REQUIRE_EQ(std::string(update_category().name()),
               std::string("vss.update"));
    for (const auto code :
         {update_errc::no_update_keys, update_errc::signature_invalid,
          update_errc::feed_malformed, update_errc::feed_version_unsupported,
          update_errc::feed_rolled_back})
    {
        const boost::system::error_code ec = code;
        REQUIRE(ec == code);
        REQUIRE(ec.failed());
        REQUIRE(&ec.category() == &update_category());
        REQUIRE(!ec.message().empty());
        REQUIRE(!to_string(code).empty());
        REQUIRE_NE(to_string(code), std::string_view("unknown"));
    }
    REQUIRE_EQ(to_string(update_errc::no_update_keys),
               std::string_view("no_update_keys"));
    REQUIRE_EQ(to_string(update_errc::signature_invalid),
               std::string_view("signature_invalid"));
    REQUIRE_EQ(to_string(update_errc::feed_malformed),
               std::string_view("feed_malformed"));
    REQUIRE_EQ(to_string(update_errc::feed_version_unsupported),
               std::string_view("feed_version_unsupported"));
    REQUIRE_EQ(to_string(update_errc::feed_rolled_back),
               std::string_view("feed_rolled_back"));
    REQUIRE(!boost::system::error_code(update_errc::success));
}

// -- the payload ------------------------------------------------------------

TEST_CASE("update feed: a good payload is read in full")
{
    std::string detail = "left over";
    const auto parsed = parse_feed_payload(good_payload(), &detail);
    REQUIRE(parsed.has_value());
    // A success clears the line for a person.
    REQUIRE(detail.empty());

    REQUIRE_EQ(parsed->serial, 12);
    REQUIRE(parsed->issued_at ==
            std::chrono::sys_days{2026y / 12 / 1} + 3h + 40min);

    REQUIRE_EQ(parsed->studio.size(), 2);
    const studio_release& stable = parsed->studio[0];
    REQUIRE(stable.channel == channel::stable);
    REQUIRE(stable.version == v("0.2.0"));
    REQUIRE(stable.released == 2026y / 12 / 1);
    REQUIRE_EQ(stable.summary, std::string("What is new."));
    REQUIRE_EQ(stable.notes_url,
               std::string("https://example.org/notes/0.2.0"));
    REQUIRE_EQ(stable.files.size(), 2);
    REQUIRE(stable.files[0].kind == file_kind::windows_installer);
    REQUIRE(stable.files[0].arch == architecture::x86_64);
    REQUIRE_EQ(stable.files[0].name,
               std::string("studio-0.2.0-windows-x86_64.exe"));
    REQUIRE_EQ(stable.files[0].url,
               std::string("https://example.org/dl/"
                           "studio-0.2.0-windows-x86_64.exe"));
    REQUIRE_EQ(stable.files[0].size, 1000);
    REQUIRE_EQ(stable.files[0].sha512[0], 0xaa);
    REQUIRE_EQ(stable.files[0].sha512[63], 0xaa);
    REQUIRE(stable.files[1].kind == file_kind::linux_deb);
    REQUIRE(stable.files[1].arch == architecture::arm64);
    REQUIRE_EQ(stable.files[1].sha512[10], 0xbb);

    const studio_release& preview = parsed->studio[1];
    REQUIRE(preview.channel == channel::preview);
    REQUIRE(preview.version == v("0.3.0-rc.1"));
    REQUIRE_EQ(preview.summary, std::string());
    REQUIRE_EQ(preview.files.size(), 1);
    REQUIRE(preview.files[0].kind == file_kind::macos_dmg);

    REQUIRE_EQ(parsed->plugins.size(), 2);
    const plugin_release& decoding = parsed->plugins[0];
    REQUIRE_EQ(decoding.id, std::string("com.virtualserialstudio.decoding"));
    REQUIRE(decoding.version == v("1.3.0"));
    REQUIRE(decoding.released == 2026y / 12 / 2);
    REQUIRE(decoding.requires_studio == v("0.2.0"));
    REQUIRE_EQ(decoding.summary, std::string("Plugin news."));
    REQUIRE_EQ(decoding.files.size(), 1);
    REQUIRE(decoding.files[0].os == operating_system::windows);
    REQUIRE(decoding.files[0].arch == architecture::x86_64);
    REQUIRE_EQ(decoding.files[0].size, 4000);
    REQUIRE_EQ(decoding.files[0].sha512[5], 0xdd);

    const plugin_release& exporter = parsed->plugins[1];
    REQUIRE(exporter.version == v("2.0.0-beta.2"));
    REQUIRE(exporter.requires_studio == v("0.3.0-rc.1"));
    REQUIRE_EQ(exporter.summary, std::string());
    REQUIRE(exporter.files.empty());
}

TEST_CASE("update feed: a missing line for a person is allowed")
{
    REQUIRE(parse_feed_payload(good_payload(), nullptr).has_value());
    REQUIRE(!parse_feed_payload("nope", nullptr).has_value());
}

TEST_CASE("update feed: what may be left out")
{
    // `plugins` may be absent.
    std::string no_plugins = good_payload();
    no_plugins.erase(no_plugins.find(",\n  \"plugins\": ["));
    no_plugins += "\n}";
    const auto without = must_parse(no_plugins);
    REQUIRE_EQ(without.studio.size(), 2);
    REQUIRE(without.plugins.empty());
    const auto minimal = parse_feed_payload(
        R"({"feed_version": 1, "serial": 1, "issued_at": "2026-12-01T03:40:00Z", "studio": []})");
    REQUIRE(minimal.has_value());
    REQUIRE(minimal->studio.empty());
    REQUIRE(minimal->plugins.empty());
    REQUIRE_EQ(minimal->serial, 1);

    // `summary` may be absent in a studio entry and in a plugin entry.
    const auto no_summary = must_parse(
        drop(drop(good_payload(), member("summary", "\"What is new.\"")),
             member("summary", "\"Plugin news.\"")));
    REQUIRE(no_summary.studio[0].summary.empty());
    REQUIRE(no_summary.plugins[0].summary.empty());

    // A plugin with no file for any system is a list the Studio can name.
    REQUIRE(must_parse(good_payload()).plugins[1].files.empty());

    // An empty summary is the same as none.
    REQUIRE(must_parse(with_summary("")).studio[0].summary.empty());
}

TEST_CASE("update feed: values at their limits are accepted")
{
    // The serial runs from 1 to 2^53.
    REQUIRE_EQ(must_parse(edit(good_payload(), "\"serial\": 12",
                               "\"serial\": 9007199254740992"))
                   .serial,
               9007199254740992ULL);
    REQUIRE_EQ(
        must_parse(edit(good_payload(), "\"serial\": 12", "\"serial\": 1"))
            .serial,
        1);

    // A file may be 4 GiB and no larger.
    REQUIRE_EQ(must_parse(edit(good_payload(), "\"size\": 1000",
                               "\"size\": 4294967296"))
                   .studio[0]
                   .files[0]
                   .size,
               4294967296ULL);
    REQUIRE_EQ(must_parse(edit(good_payload(), "\"size\": 1000", "\"size\": 1"))
                   .studio[0]
                   .files[0]
                   .size,
               1);

    // A name of 255 characters, a summary of 2000 bytes, an address of 2048.
    const std::string name = std::string(251, 'n') + ".exe";
    REQUIRE_EQ(name.size(), 255);
    REQUIRE_EQ(must_parse(with_name(name)).studio[0].files[0].name, name);
    REQUIRE_EQ(must_parse(with_summary(std::string(2000, 's')))
                   .studio[0]
                   .summary.size(),
               2000);
    const std::string url =
        "https://example.org/" + std::string(2048 - 20, 'u');
    REQUIRE_EQ(url.size(), 2048);
    REQUIRE_EQ(must_parse(with_url(url)).studio[0].files[0].url, url);

    // Fourteen files: every kind for each architecture.
    std::string files;
    for (const char* kind :
         {"windows-installer", "windows-zip", "macos-dmg", "macos-archive",
          "linux-deb", "linux-rpm", "linux-archive"})
    {
        for (const char* arch : {"x86_64", "arm64"})
        {
            if (!files.empty())
                files += ", ";
            files += std::string(R"({"kind": ")") + kind + R"(", "arch": ")" +
                     arch +
                     R"(", "name": "f", "url": "https://example.org/f", )"
                     R"("size": 1, "sha512": ")" +
                     hash_of('f') + "\"}";
        }
    }
    const auto fourteen = must_parse(
        R"({"feed_version": 1, "serial": 1, "issued_at": "2026-12-01T00:00:00Z", )"
        R"("studio": [{"channel": "stable", "version": "1.0.0", )"
        R"("released": "2026-12-01", "notes_url": "https://example.org/n", )"
        R"("files": [)" +
        files + "]}]}");
    REQUIRE_EQ(fourteen.studio[0].files.size(), 14);

    // Sixty-four plugins.
    REQUIRE_EQ(must_parse(many_plugins(64)).plugins.size(), 64);
    REQUIRE_EQ(must_parse(many_plugins(0)).plugins.size(), 0);

    // A summary has any text but controls: new lines, accents, other scripts.
    REQUIRE_EQ(must_parse(with_summary(R"(one\ntwo)")).studio[0].summary,
               std::string("one\ntwo"));
    REQUIRE_EQ(must_parse(with_summary("caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac"))
                   .studio[0]
                   .summary,
               std::string("caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac"));
    REQUIRE_EQ(must_parse(with_summary(R"(café)")).studio[0].summary,
               std::string("caf\xc3\xa9"));
    // The 2000 bytes are bytes, so 1000 two-byte characters fit exactly.
    std::string accents;
    for (int i = 0; i < 1000; ++i)
        accents += "\xc3\xa9";
    REQUIRE_EQ(must_parse(with_summary(accents)).studio[0].summary.size(),
               2000);

    // A plugin version may be a pre-release; only a developer version is not
    // accepted anywhere.
    REQUIRE(must_parse(good_payload()).plugins[1].version.is_prerelease());

    // A stable release may be 0.0.0, and the numbers go to 999999.
    REQUIRE(accepted(edit(good_payload(), "\"version\": \"0.2.0\"",
                          "\"version\": \"999999.999999.999999\"")));

    // A host with a port, a query or a fragment, and a bare address.
    REQUIRE(accepted(with_url("https://example.org:8443/a?b=c#d")));
    REQUIRE(accepted(with_url("https://example.org")));
    REQUIRE(accepted(with_url("https://127.0.0.1/a")));
    REQUIRE(accepted(with_url("https://[::1]:8443/a")));

    // A plugin id of 128 characters.
    REQUIRE(accepted(edit(
        good_payload(), "\"com.virtualserialstudio.decoding\"",
        "\"" + std::string(official_plugin_prefix) +
            std::string(128 - official_plugin_prefix.size(), 'a') + "\"")));
}

TEST_CASE("update feed: a name may hold the characters of a file name")
{
    for (const std::string_view name :
         {"a", "A.b-c_d+e~f", "setup-1.2.3.exe", "x..y", "a.b", "-", "_", "+",
          "~", "0", "virtual-serial-studio_0.2.0-1_amd64.deb"})
    {
        const auto parsed = parse_feed_payload(with_name(name));
        if (!parsed)
            throw vss_test::failure("refused name " + std::string(name));
    }
}

TEST_CASE("update feed: the feed's version is read before anything else")
{
    for (const std::string_view value :
         {"2", "0", "-1", "100", "18446744073709551615"})
    {
        std::string detail;
        const auto parsed =
            parse_feed_payload(edit(good_payload(), "\"feed_version\": 1",
                                    "\"feed_version\": " + std::string(value)),
                               &detail);
        REQUIRE_EQ(code_name(parsed) + " for " + std::string(value),
                   "feed_version_unsupported for " + std::string(value));
        REQUIRE(contains(detail, "feed_version"));
    }

    // A later format may have other keys, and is still told apart.
    REQUIRE_EQ(code_name(parse_feed_payload(
                   R"({"feed_version": 2, "novel": [1, 2, 3]})")),
               std::string("feed_version_unsupported"));
    REQUIRE_EQ(code_name(parse_feed_payload(R"({"feed_version": 2})")),
               std::string("feed_version_unsupported"));
    REQUIRE_EQ(
        code_name(parse_feed_payload(R"({"feed_version": 1, "novel": true})")),
        std::string("feed_malformed"));

    // Not a whole number is not a version.
    for (const std::string_view value :
         {"\"1\"", "1.0", "1.5", "null", "true", "[1]", "{}",
          "99999999999999999999999"})
    {
        const auto parsed =
            parse_feed_payload(edit(good_payload(), "\"feed_version\": 1",
                                    "\"feed_version\": " + std::string(value)));
        REQUIRE_EQ(code_name(parsed) + " for " + std::string(value),
                   "feed_malformed for " + std::string(value));
    }
    REQUIRE_EQ(code_name(parse_feed_payload("{}")),
               std::string("feed_malformed"));
}

TEST_CASE("update feed: every rule refuses the whole feed")
{
    // The text and the shape.
    bad("empty text", "", "not JSON");
    bad("not JSON", "nope", "not JSON");
    bad("an array", "[]", "must be an object");
    bad("a number", "1", "must be an object");
    bad("a string", "\"feed\"", "must be an object");
    bad("null", "null", "must be an object");
    bad("text after the document", good_payload() + " x", "not JSON");
    bad("a second document", good_payload() + good_payload(), "not JSON");
    bad("a comment",
        edit(good_payload(), "\"serial\": 12,", "\"serial\": 12, /* c */"),
        "not JSON");
    bad("a trailing comma",
        edit(good_payload(), "\"serial\": 12,", "\"serial\": 12,,"),
        "not JSON");
    bad("a byte that is not UTF-8",
        edit(good_payload(), "What is new.", "What \xff new."), "not JSON");
    bad("a lone surrogate",
        edit(good_payload(), "What is new.", R"(What \ud800 new.)"), "");
    bad("nesting too deep",
        edit(good_payload(), "\"serial\": 12,",
             "\"serial\": 12, \"studio\": [[[[[[[[1]]]]]]]],"),
        "");
    bad("nesting far too deep", std::string(2000, '[') + std::string(2000, ']'),
        "not JSON");

    // Keys given twice, at the top and further down.
    bad("a key twice at the top",
        edit(good_payload(), "\"serial\": 12,",
             "\"serial\": 12, \"serial\": 13,"),
        "twice");
    bad("a key twice in a Studio entry",
        edit(good_payload(), "\"channel\": \"stable\",",
             "\"channel\": \"stable\", \"channel\": \"stable\","),
        "twice");
    bad("a key twice in a file",
        edit(good_payload(), "\"size\": 1000,",
             "\"size\": 1000, \"size\": 1000,"),
        "twice");
    bad("a key twice in a plugin file",
        edit(good_payload(), "\"size\": 4000,",
             "\"size\": 4000, \"size\": 4000,"),
        "twice");
    bad("a key twice in a plugin",
        edit(good_payload(), "\"id\": \"com.virtualserialstudio.export\",",
             "\"id\": \"com.virtualserialstudio.export\", \"id\": \"x\","),
        "twice");

    // Keys this version does not read.
    bad("an unknown key at the top",
        edit(good_payload(), "\"serial\": 12,",
             "\"serial\": 12, \"extra\": 1,"),
        "extra");
    bad("an unknown key in a Studio entry",
        edit(good_payload(), "\"released\": \"2026-12-01\",",
             "\"released\": \"2026-12-01\", \"extra\": 1,"),
        "extra");
    bad("an unknown key in a Studio file",
        edit(good_payload(), "\"size\": 1000,",
             "\"size\": 1000, \"extra\": 1,"),
        "extra");
    bad("an unknown key in a plugin",
        edit(good_payload(), "\"released\": \"2026-12-02\",",
             "\"released\": \"2026-12-02\", \"extra\": 1,"),
        "extra");
    bad("an unknown key in a plugin file",
        edit(good_payload(), "\"size\": 4000,",
             "\"size\": 4000, \"extra\": 1,"),
        "extra");
    bad("a key of a Studio file in a plugin file",
        edit(good_payload(), "\"os\": \"windows\",",
             "\"kind\": \"linux-deb\","),
        "kind");
    bad("an upper-case key",
        edit(good_payload(), "\"serial\": 12", "\"Serial\": 12"), "Serial");

    // Keys that must be there.
    bad("no serial", drop(good_payload(), member("serial", "12")), "serial");
    bad("no issued_at",
        drop(good_payload(), member("issued_at", "\"2026-12-01T03:40:00Z\"")),
        "issued_at");
    bad("no studio list",
        edit(good_payload(), "\"studio\": [", "\"studios\": ["), "studios");
    bad("no channel", drop(good_payload(), member("channel", "\"stable\"")),
        "channel");
    bad("no Studio version",
        drop(good_payload(), member("version", "\"0.2.0\"")), "version");
    bad("no Studio released",
        drop(good_payload(), member("released", "\"2026-12-01\"")), "released");
    bad("no Studio notes_url",
        drop(good_payload(),
             member("notes_url", "\"https://example.org/notes/0.2.0\"")),
        "notes_url");
    bad("no Studio files",
        edit(good_payload(),
             "\"files\": [\n        {\n          \"kind\": "
             "\"windows-installer\"",
             "\"filez\": [\n        {\n          \"kind\": "
             "\"windows-installer\""),
        "");
    bad("no kind",
        drop(good_payload(), member("kind", "\"windows-installer\"")), "kind");
    bad("no arch", drop(good_payload(), member("arch", "\"x86_64\"")), "arch");
    bad("no name",
        drop(good_payload(),
             member("name", "\"studio-0.2.0-windows-x86_64.exe\"")),
        "name");
    bad("no url",
        drop(good_payload(),
             member(
                 "url",
                 "\"https://example.org/dl/studio-0.2.0-windows-x86_64.exe\"")),
        "url");
    bad("no size", drop(good_payload(), member("size", "1000")), "size");
    bad("no sha512", drop(good_payload(), sha_member('a')), "sha512");
    bad("no plugin id",
        drop(good_payload(),
             member("id", "\"com.virtualserialstudio.decoding\"")),
        "id");
    bad("no plugin version",
        drop(good_payload(), member("version", "\"1.3.0\"")), "version");
    bad("no plugin released",
        drop(good_payload(), member("released", "\"2026-12-02\"")), "released");
    bad("no requires_studio",
        drop(good_payload(), member("requires_studio", "\"0.2.0\"")),
        "requires_studio");
    bad("no plugin notes_url",
        drop(good_payload(),
             member("notes_url",
                    "\"https://example.org/notes/decoding-1.3.0\"")),
        "notes_url");
    bad("no plugin files", drop(good_payload(), member("files", "[]")),
        "files");
    bad("no os", drop(good_payload(), member("os", "\"windows\"")), "os");
    bad("no plugin sha512", drop(good_payload(), sha_member('d')), "sha512");

    // The format and the serial.
    bad("feed_version missing",
        drop(good_payload(), member("feed_version", "1")), "feed_version");
    bad("serial 0", edit(good_payload(), "\"serial\": 12", "\"serial\": 0"),
        "serial");
    bad("serial negative",
        edit(good_payload(), "\"serial\": 12", "\"serial\": -1"), "serial");
    bad("serial above 2^53",
        edit(good_payload(), "\"serial\": 12", "\"serial\": 9007199254740993"),
        "serial");
    bad("serial huge",
        edit(good_payload(), "\"serial\": 12",
             "\"serial\": 18446744073709551615"),
        "serial");
    bad("serial a fraction",
        edit(good_payload(), "\"serial\": 12", "\"serial\": 12.5"), "serial");
    bad("serial with an exponent",
        edit(good_payload(), "\"serial\": 12", "\"serial\": 1.2e1"), "serial");
    bad("serial a string",
        edit(good_payload(), "\"serial\": 12", "\"serial\": \"12\""), "serial");
    bad("serial null",
        edit(good_payload(), "\"serial\": 12", "\"serial\": null"), "serial");

    // Dates and times.
    const std::string_view issued = "\"issued_at\": \"2026-12-01T03:40:00Z\"";
    for (const std::string_view value : {"\"2026-12-01\"",
                                         "\"2026-12-01T03:40:00\"",
                                         "\"2026-12-01t03:40:00Z\"",
                                         "\"2026-12-01 03:40:00Z\"",
                                         "\"2026-12-01T03:40:00z\"",
                                         "\"2026-12-01T03:40Z\"",
                                         "\"2026-12-01T03:40:00+00:00\"",
                                         "\"2026-12-01T03:40:00.5Z\"",
                                         "\"2026-12-01T03:40:00 Z\"",
                                         "\" 2026-12-01T03:40:00Z\"",
                                         "\"2026-13-01T03:40:00Z\"",
                                         "\"2026-00-01T03:40:00Z\"",
                                         "\"2026-12-00T03:40:00Z\"",
                                         "\"2026-12-32T03:40:00Z\"",
                                         "\"2026-02-29T03:40:00Z\"",
                                         "\"2026-04-31T03:40:00Z\"",
                                         "\"2026-12-01T24:00:00Z\"",
                                         "\"2026-12-01T03:60:00Z\"",
                                         "\"2026-12-01T03:40:60Z\"",
                                         "\"26-12-01T03:40:00Z\"",
                                         "\"2026-1-1T3:40:00Z\"",
                                         "\"2026/12/01T03:40:00Z\"",
                                         "\"2026-12-01T03:40:0aZ\"",
                                         "\"-026-12-01T03:40:00Z\"",
                                         "\"+026-12-01T03:40:00Z\"",
                                         "1796096400",
                                         "null",
                                         "true"})
    {
        bad("issued_at " + std::string(value),
            edit(good_payload(), issued,
                 "\"issued_at\": " + std::string(value)),
            "issued_at");
    }
    const std::string_view released = "\"released\": \"2026-12-01\"";
    for (const std::string_view value :
         {"\"2026-12-1\"", "\"2026-1-01\"", "\"26-12-01\"", "\"2026/12/01\"",
          "\"20261201\"", "\"2026-12-01T00:00:00Z\"", "\"2026-12-01 \"",
          "\"2026-02-29\"", "\"2026-13-01\"", "\"2026-12-32\"",
          "\"2026-00-10\"", "\"2026-12-00\"", "\"0000-12-01\"", "\"\"",
          "20261201", "null"})
    {
        bad("released " + std::string(value),
            edit(good_payload(), released,
                 "\"released\": " + std::string(value)),
            "released");
    }
    bad("plugin released not a date",
        edit(good_payload(), "\"released\": \"2026-12-02\"",
             "\"released\": \"2026-12-2\""),
        "released");
    // 2028 is a leap year, so the 29th is real there.
    REQUIRE(accepted(edit(good_payload(), "\"released\": \"2026-12-01\"",
                          "\"released\": \"2028-02-29\"")));

    // The Studio list.
    bad("studio null",
        "{\"feed_version\": 1, \"serial\": 1, \"issued_at\": "
        "\"2026-12-01T00:00:00Z\", \"studio\": null}",
        "studio");
    bad("studio an object",
        "{\"feed_version\": 1, \"serial\": 1, \"issued_at\": "
        "\"2026-12-01T00:00:00Z\", \"studio\": {}}",
        "studio");
    bad("studio entry not an object",
        "{\"feed_version\": 1, \"serial\": 1, \"issued_at\": "
        "\"2026-12-01T00:00:00Z\", \"studio\": [1]}",
        "studio[0]");
    bad("three Studio entries",
        edit(good_payload(), "\"studio\": [", "\"studio\": [{}, "), "");
    bad("the same channel twice",
        edit(good_payload(),
             "\"channel\": \"preview\",\n      \"version\": "
             "\"0.3.0-rc.1\"",
             "\"channel\": \"stable\",\n      \"version\": \"0.3.0\""),
        "channel");
    bad("a channel nobody knows",
        edit(good_payload(), "\"channel\": \"preview\"",
             "\"channel\": \"beta\""),
        "channel");
    bad("a channel in another case",
        edit(good_payload(), "\"channel\": \"stable\"",
             "\"channel\": \"Stable\""),
        "channel");
    bad("a channel that is not text",
        edit(good_payload(), "\"channel\": \"stable\"", "\"channel\": 1"),
        "channel");
    bad("a pre-release in the stable channel",
        edit(good_payload(), "\"version\": \"0.2.0\"",
             "\"version\": \"0.2.0-rc.1\""),
        "stable");
    bad("a final release in the preview channel",
        edit(good_payload(), "\"version\": \"0.3.0-rc.1\"",
             "\"version\": \"0.3.0\""),
        "pre-release");
    bad("a developer version in the stable channel",
        edit(good_payload(), "\"version\": \"0.2.0\"",
             "\"version\": \"0.2.0-dev\""),
        "developer");
    bad("a developer version in the preview channel",
        edit(good_payload(), "\"version\": \"0.3.0-rc.1\"",
             "\"version\": \"0.3.0-dev\""),
        "developer");
    bad("a developer version of a plugin",
        edit(good_payload(), "\"version\": \"1.3.0\"",
             "\"version\": \"1.3.0-dev\""),
        "developer");
    bad("a developer version in requires_studio",
        edit(good_payload(), "\"requires_studio\": \"0.2.0\"",
             "\"requires_studio\": \"0.2.0-dev\""),
        "developer");

    // Versions.
    for (const std::string_view value :
         {"\"\"", "\"v0.2.0\"", "\"0.2\"", "\"0.2.0.1\"", "\"0.2.0+build\"",
          "\"00.2.0\"", "\"1000000.0.0\"", "\"0.2.0-\"", "\"0.2.0-rc..1\"",
          "\"0.2.0-rc.01\"", "\" 0.2.0\"", "\"0.2.0 \"", "0", "null",
          "[\"0.2.0\"]"})
    {
        bad("Studio version " + std::string(value),
            edit(good_payload(), "\"version\": \"0.2.0\"",
                 "\"version\": " + std::string(value)),
            "version");
        bad("plugin version " + std::string(value),
            edit(good_payload(), "\"version\": \"1.3.0\"",
                 "\"version\": " + std::string(value)),
            "version");
        bad("requires_studio " + std::string(value),
            edit(good_payload(), "\"requires_studio\": \"0.2.0\"",
                 "\"requires_studio\": " + std::string(value)),
            "requires_studio");
    }

    // Files of a Studio release.
    bad("an empty list of files in a Studio release",
        "{\"feed_version\": 1, \"serial\": 1, \"issued_at\": "
        "\"2026-12-01T00:00:00Z\", \"studio\": [{\"channel\": \"stable\", "
        "\"version\": \"1.0.0\", \"released\": \"2026-12-01\", \"notes_url\": "
        "\"https://example.org/n\", \"files\": []}]}",
        "files");
    bad("seventeen files in a Studio release", many_studio_files(17),
        "more than 16");
    bad("a file list that is not a list",
        "{\"feed_version\": 1, \"serial\": 1, \"issued_at\": "
        "\"2026-12-01T00:00:00Z\", \"studio\": [{\"channel\": \"stable\", "
        "\"version\": \"1.0.0\", \"released\": \"2026-12-01\", \"notes_url\": "
        "\"https://example.org/n\", \"files\": {}}]}",
        "files");
    bad("two files of one kind and architecture",
        edit(edit(good_payload(), "\"kind\": \"linux-deb\"",
                  "\"kind\": \"windows-installer\""),
             "\"arch\": \"arm64\"", "\"arch\": \"x86_64\""),
        "repeats");
    bad("two plugin files of one system and architecture",
        edit(
            good_payload(),
            "\"files\": [\n        {\n          \"os\": \"windows\"",
            "\"files\": [{\"os\": \"windows\", \"arch\": \"x86_64\", "
            "\"name\": \"a\", \"url\": \"https://example.org/a\", \"size\": 1, "
            "\"sha512\": \"" +
                hash_of('a') +
                "\"},\n        {\n          \"os\": \"windows\""),
        "repeats");
    bad("a file that is not an object",
        "{\"feed_version\": 1, \"serial\": 1, \"issued_at\": "
        "\"2026-12-01T00:00:00Z\", \"studio\": [{\"channel\": \"stable\", "
        "\"version\": \"1.0.0\", \"released\": \"2026-12-01\", \"notes_url\": "
        "\"https://example.org/n\", \"files\": [\"a.exe\"]}]}",
        "files[0]");
    bad("an unknown kind",
        edit(good_payload(), "\"kind\": \"windows-installer\"",
             "\"kind\": \"windows-msi\""),
        "kind");
    bad("a kind in another spelling",
        edit(good_payload(), "\"kind\": \"windows-installer\"",
             "\"kind\": \"windows_installer\""),
        "kind");
    bad("a kind that is not text",
        edit(good_payload(), "\"kind\": \"windows-installer\"", "\"kind\": 0"),
        "kind");
    bad("an unknown arch",
        edit(good_payload(), "\"arch\": \"x86_64\"", "\"arch\": \"x86\""),
        "arch");
    bad("an arch in another spelling",
        edit(good_payload(), "\"arch\": \"x86_64\"", "\"arch\": \"amd64\""),
        "arch");
    bad("an unknown os",
        edit(good_payload(), "\"os\": \"windows\"", "\"os\": \"freebsd\""),
        "os");
    bad("an os in another case",
        edit(good_payload(), "\"os\": \"windows\"", "\"os\": \"Windows\""),
        "os");

    // Names: a file name and nothing that can be a path.
    for (const std::string_view name :
         {"",          ".hidden", ".",       "..",          "a/b",
          "/a",        "a/",      "..\\\\a", "a\\\\b",      "C:a",
          "a:b",       "a b",     " a",      "a ",          "a*",
          "a?",        "a\\\"",   "a<b",     "a|b",         "a;b",
          "a$b",       "a%2fb",   "a&b",     "a(b)",        "a#b",
          "a@b",       "a,b",     "a=b",     "caf\xc3\xa9", "\xe6\x97\xa5",
          "a\\u0000b", "a\\tb",   "a\\nb",   "a\\u007fb",   "a\\u00e9"})
    {
        bad("name " + std::string(name), with_name(name), "name");
    }
    // A dot at the end, which Windows drops, and the device names of Windows.
    for (const std::string_view name :
         {"a.", "a.exe.", "con", "CON", "Nul.exe", "aux.tar.gz", "prn.zip",
          "COM1", "com9.exe", "LPT0.deb"})
    {
        bad("name " + std::string(name), with_name(name), "name");
    }
    bad("a name of 256 characters", with_name(std::string(256, 'n')), "name");
    bad("a name that is a number",
        edit(good_payload(), "\"studio-0.2.0-windows-x86_64.exe\"", "5"),
        "name");

    // Addresses.
    for (const std::string_view url : {"",
                                       "http://example.org/a",
                                       "ftp://example.org/a",
                                       "file:///etc/passwd",
                                       "HTTPS://example.org/a",
                                       "Https://example.org/a",
                                       "//example.org/a",
                                       "example.org/a",
                                       "/a",
                                       "https:/example.org/a",
                                       "https:example.org/a",
                                       "https://",
                                       "https:///a",
                                       "https://:443/a",
                                       "https://?x",
                                       "https://#x",
                                       "https://user@example.org/a",
                                       "https://user:password@example.org/a",
                                       "https://@example.org/a",
                                       "https://a@b@example.org/a",
                                       "https://example.org@evil.example/a",
                                       "https://example.org?x@y",
                                       "https://example.org\\\\a",
                                       "https://example.org/a\\\\b",
                                       "https://exa mple.org/a",
                                       "https://example.org/a b",
                                       "https://example.org/a\\tb",
                                       "https://example.org/a\\nb",
                                       "https://example.org/a\\u0000",
                                       "https://example.org/a\\u007f",
                                       "https://h\xc3\xb3st.example/a",
                                       "https://example.org/\xc3\xa9",
                                       "https://example.org/\\u00e9",
                                       " https://example.org/a",
                                       "https://example.org/a "})
    {
        bad("url " + std::string(url), with_url(url), "url");
    }
    bad("an address of 2049 characters",
        with_url("https://example.org/" + std::string(2049 - 20, 'u')), "url");
    bad("an address that is not text",
        edit(good_payload(),
             "\"https://example.org/dl/studio-0.2.0-windows-x86_64.exe\"", "1"),
        "url");
    for (const std::string_view url :
         {"", "http://example.org/n", "https://u@example.org/n",
          "https://example.org\\\\n", "https:///n"})
    {
        bad("Studio notes_url " + std::string(url),
            edit(good_payload(), "\"https://example.org/notes/0.2.0\"",
                 "\"" + std::string(url) + "\""),
            "notes_url");
        bad("plugin notes_url " + std::string(url),
            edit(good_payload(), "\"https://example.org/notes/decoding-1.3.0\"",
                 "\"" + std::string(url) + "\""),
            "notes_url");
        bad("plugin file url " + std::string(url),
            edit(good_payload(),
                 "\"https://example.org/dl/decoding-1.3.0-windows-x86_64.zip\"",
                 "\"" + std::string(url) + "\""),
            "url");
    }

    // Sizes.
    for (const std::string_view value :
         {"0", "-1", "4294967297", "9007199254740993", "18446744073709551615",
          "1.5", "1e3", "\"1000\"", "null", "true"})
    {
        bad("size " + std::string(value),
            edit(good_payload(), "\"size\": 1000",
                 "\"size\": " + std::string(value)),
            "size");
        bad("plugin size " + std::string(value),
            edit(good_payload(), "\"size\": 4000",
                 "\"size\": " + std::string(value)),
            "size");
    }

    // Hashes.
    for (const std::string& value :
         {std::string(), std::string(127, 'a'), std::string(129, 'a'),
          std::string(64, 'a'), std::string(128, 'A'),
          std::string(127, 'a') + "g", std::string(127, 'a') + "G",
          std::string(127, 'a') + " ", std::string(127, 'a') + "-",
          std::string(126, 'a') + "0x", " " + std::string(128, 'a'),
          std::string(128, 'a') + " "})
    {
        bad("sha512 of " + std::to_string(value.size()) + " characters",
            edit(good_payload(), sha_member('a'),
                 member("sha512", "\"" + value + "\"")),
            "sha512");
    }
    bad("sha512 with an upper-case digit",
        edit(good_payload(), sha_member('d'),
             member("sha512", "\"" + std::string(127, 'd') + "D\"")),
        "sha512");
    bad("sha512 a number",
        edit(good_payload(), sha_member('a'), member("sha512", "1")), "sha512");
    bad("sha512 null",
        edit(good_payload(), sha_member('a'), member("sha512", "null")),
        "sha512");

    // Summaries: plain text.
    bad("a summary of 2001 bytes", with_summary(std::string(2001, 's')),
        "summary");
    {
        // One byte too many through a two-byte character.
        std::string accents;
        for (int i = 0; i < 1000; ++i)
            accents += "\xc3\xa9";
        bad("a summary of 2002 bytes", with_summary(accents + "\xc3\xa9"),
            "summary");
        bad("a summary of 2001 bytes with accents", with_summary(accents + "a"),
            "summary");
    }
    for (const std::string_view text :
         {R"(a\u0000b)", R"(a\u0001b)", R"(a\tb)", R"(a\rb)", R"(a\u000bb)",
          R"(a\u000cb)", R"(a\u001bb)", R"(a\u001fb)", R"(a\u007fb)",
          R"(a\u0080b)", R"(a\u0085b)", R"(a\u009fb)",
          // U+0085 and U+009F as UTF-8. Octal, because a hex escape would
          // take the `b` that follows as one of its digits.
          "a\302\205b", "a\302\237b"})
    {
        bad("summary " + std::string(text), with_summary(text), "summary");
    }
    bad("a summary that is a number",
        edit(good_payload(), "\"What is new.\"", "5"), "summary");
    bad("a summary that is null",
        edit(good_payload(), "\"What is new.\"", "null"), "summary");
    bad("a summary that is a list",
        edit(good_payload(), "\"What is new.\"", "[\"a\"]"), "summary");
    bad("a plugin summary of 2001 bytes",
        edit(good_payload(), "\"Plugin news.\"",
             "\"" + std::string(2001, 'p') + "\""),
        "summary");

    // The plugin list.
    bad("plugins an object",
        "{\"feed_version\": 1, \"serial\": 1, \"issued_at\": "
        "\"2026-12-01T00:00:00Z\", \"studio\": [], \"plugins\": {}}",
        "plugins");
    bad("plugins null",
        "{\"feed_version\": 1, \"serial\": 1, \"issued_at\": "
        "\"2026-12-01T00:00:00Z\", \"studio\": [], \"plugins\": null}",
        "plugins");
    bad("sixty-five plugins", many_plugins(65), "more than 64");
    bad("a plugin that is not an object",
        "{\"feed_version\": 1, \"serial\": 1, \"issued_at\": "
        "\"2026-12-01T00:00:00Z\", \"studio\": [], \"plugins\": [\"a\"]}",
        "plugins[0]");
    bad("the same plugin twice",
        edit(good_payload(), "\"id\": \"com.virtualserialstudio.export\"",
             "\"id\": \"com.virtualserialstudio.decoding\""),
        "repeats");
    for (const std::string_view id :
         {"", "decoding", "com.virtualserialstudio", "com.virtualserialstudio.",
          "com.virtualserialstudio", "org.example.decoding",
          "com.example.com.virtualserialstudio.x", "Com.virtualserialstudio.x",
          "com.virtualserialstudio.Decoding", "com.virtualserialstudio.a_b",
          "com.virtualserialstudio.a b", "com.virtualserialstudio.a/b",
          "com.virtualserialstudio.\xc3\xa9",
          "com.virtualserialstudio.a\\u0000", " com.virtualserialstudio.x",
          "com.virtualserialstudio.x "})
    {
        bad("plugin id " + std::string(id),
            edit(good_payload(), "\"com.virtualserialstudio.decoding\"",
                 "\"" + std::string(id) + "\""),
            "id");
    }
    bad("a plugin id of 129 characters",
        edit(good_payload(), "\"com.virtualserialstudio.decoding\"",
             "\"" + std::string(official_plugin_prefix) +
                 std::string(129 - official_plugin_prefix.size(), 'a') + "\""),
        "id");
    bad("a plugin id that is a number",
        edit(good_payload(), "\"com.virtualserialstudio.decoding\"", "7"),
        "id");
    bad("seventeen plugin files",
        edit(
            good_payload(), "\"files\": []",
            "\"files\": [" +
                []
                {
                    std::string files;
                    for (int i = 0; i < 17; ++i)
                        files +=
                            std::string(i == 0 ? "" : ", ") +
                            R"({"os": "linux", "arch": "arm64", "name": "a", )"
                            R"("url": "https://example.org/a", "size": 1, )"
                            R"("sha512": ")" +
                            hash_of('a') + "\"}";
                    return files;
                }() +
                "]"),
        "more than 16");

    for (const bad_case& item : cases())
    {
        std::string detail = "stale";
        const auto parsed = parse_feed_payload(item.payload, &detail);
        const std::string outcome = item.name + " -> " + code_name(parsed);
        REQUIRE_EQ(outcome,
                   item.name + " -> " + std::string(to_string(item.code)));
        // Every refusal says why, in one line for a person.
        REQUIRE_EQ(item.name + (detail.empty() ? " has no detail" : ""),
                   item.name);
        REQUIRE_EQ(item.name + (detail.find('\n') == std::string::npos
                                    ? ""
                                    : " has a detail of several lines"),
                   item.name);
        if (!item.detail.empty() && !contains(detail, item.detail))
            throw vss_test::failure(item.name + ": the detail \"" + detail +
                                    "\" does not mention \"" + item.detail +
                                    "\"");
    }
    // Enough cases that no rule was forgotten.
    REQUIRE(cases().size() > 200);
}

TEST_CASE("update feed: the Studio list has at most two entries")
{
    std::string detail;
    const auto parsed =
        parse_feed_payload(edit(good_payload(), "\"studio\": [",
                                "\"studio\": [{\"channel\": \"stable\"}, "),
                           &detail);
    REQUIRE_EQ(code_name(parsed), std::string("feed_malformed"));
    REQUIRE(contains(detail, "more than 2"));
}

TEST_CASE("update feed: the first fault is the one reported")
{
    std::string detail;
    const auto parsed = parse_feed_payload(
        edit(edit(good_payload(), "\"size\": 1000", "\"size\": 0"),
             "\"size\": 2000", "\"size\": 0"),
        &detail);
    REQUIRE_EQ(code_name(parsed), std::string("feed_malformed"));
    REQUIRE(contains(detail, "studio[0].files[0].size"));

    const auto second = parse_feed_payload(
        edit(good_payload(), "\"size\": 2000", "\"size\": 0"), &detail);
    REQUIRE(!second);
    REQUIRE(contains(detail, "studio[0].files[1].size"));

    const auto plugin = parse_feed_payload(
        edit(good_payload(), "\"size\": 4000", "\"size\": 0"), &detail);
    REQUIRE(!plugin);
    REQUIRE(contains(detail, "plugins[0].files[0].size"));
}

// -- the signed file --------------------------------------------------------

namespace
{

sf::trusted_key feed_key(std::string_view key_id = test_keys::key_id)
{
    sf::trusted_key key;
    key.key_id = std::string(key_id);
    key.purpose = std::string(feed_purpose);
    key.key = signing::public_key_of(test_keys::private_key());
    return key;
}

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    std::vector<std::uint8_t> bytes;
    for (const char c : text)
        bytes.push_back(static_cast<std::uint8_t>(c));
    return bytes;
}

std::string signed_text(std::string_view payload,
                        std::string_view purpose = feed_purpose,
                        std::string_view key_id = test_keys::key_id)
{
    return sf::write(signing::sign(purpose, key_id, bytes_of(payload),
                                   test_keys::private_key()));
}

std::string code_of(const result<feed>& outcome)
{
    return code_name(outcome);
}

} // namespace

TEST_CASE("update feed: a signed feed is read")
{
    const std::vector<sf::trusted_key> keys{feed_key()};
    std::string detail = "stale";
    const auto parsed =
        read_feed(signed_text(good_payload()), keys, 0, &detail);
    REQUIRE(parsed.has_value());
    REQUIRE(detail.empty());
    REQUIRE_EQ(parsed->serial, 12);
    REQUIRE_EQ(parsed->studio.size(), 2);
    REQUIRE_EQ(parsed->plugins.size(), 2);

    // The same feed with no line for a person.
    REQUIRE(read_feed(signed_text(good_payload()), keys, 0).has_value());
    // Any one of several keys may have signed it.
    const std::vector<sf::trusted_key> several{feed_key("test-other"),
                                               feed_key(), feed_key("test-a")};
    REQUIRE(read_feed(signed_text(good_payload()), several, 0).has_value());
}

TEST_CASE("update feed: with no keys nothing is read")
{
    std::string detail;
    const std::span<const sf::trusted_key> none;
    REQUIRE_EQ(
        code_of(read_feed(signed_text(good_payload()), none, 0, &detail)),
        std::string("no_update_keys"));
    REQUIRE(!detail.empty());
    // Whatever the text is: the check on the keys comes first.
    REQUIRE_EQ(code_of(read_feed("not a file", none, 0)),
               std::string("no_update_keys"));
    REQUIRE_EQ(code_of(read_feed("", none, 0)), std::string("no_update_keys"));
    REQUIRE_EQ(code_of(read_feed(signed_text("{}"), none, 100)),
               std::string("no_update_keys"));
}

TEST_CASE("update feed: text that is not a signed file is refused")
{
    const std::vector<sf::trusted_key> keys{feed_key()};
    for (const std::string_view text :
         {"", "not a file", "{}", "[]", "null",
          R"({"format": 1, "purpose": "update-feed"})"})
    {
        std::string detail;
        REQUIRE_EQ(std::string(text) + " -> " +
                       code_of(read_feed(text, keys, 0, &detail)),
                   std::string(text) + " -> signature_invalid");
        REQUIRE(!detail.empty());
    }
    // The bare payload is not a feed as it travels.
    REQUIRE_EQ(code_of(read_feed(good_payload(), keys, 0)),
               std::string("signature_invalid"));
    // A file over the envelope's limit.
    REQUIRE_EQ(
        code_of(read_feed(std::string(sf::max_file_size + 1, ' '), keys, 0)),
        std::string("signature_invalid"));
}

TEST_CASE("update feed: a file made for another purpose is refused")
{
    // The key signed this payload for something else, and is trusted for
    // that too; the purpose inside the signature keeps it from passing.
    std::vector<sf::trusted_key> keys{feed_key()};
    sf::trusted_key licence = feed_key();
    licence.purpose = "licence";
    keys.push_back(licence);
    sf::trusted_key revocations = feed_key();
    revocations.purpose = "revocations";
    keys.push_back(revocations);

    for (const std::string_view purpose :
         {"licence", "revocations", "plugin-manifest", "official-keys",
          "update-feeds", "Update-Feed"})
    {
        std::string detail;
        REQUIRE_EQ(std::string(purpose) + " -> " +
                       code_of(read_feed(signed_text(good_payload(), purpose),
                                         keys, 0, &detail)),
                   std::string(purpose) + " -> signature_invalid");
        REQUIRE(!detail.empty());
    }
    // The right purpose is read with the same keys.
    REQUIRE(read_feed(signed_text(good_payload(), "update-feed"), keys, 0)
                .has_value());
}

TEST_CASE("update feed: a key that is not trusted for feeds is refused")
{
    // A key id nobody trusts.
    const std::vector<sf::trusted_key> keys{feed_key()};
    REQUIRE_EQ(code_of(read_feed(
                   signed_text(good_payload(), feed_purpose, "test-unknown"),
                   keys, 0)),
               std::string("signature_invalid"));

    // The right id, trusted for another purpose only.
    sf::trusted_key licence_only = feed_key();
    licence_only.purpose = "licence";
    const std::vector<sf::trusted_key> others{licence_only};
    REQUIRE_EQ(code_of(read_feed(signed_text(good_payload()), others, 0)),
               std::string("signature_invalid"));
    // The licence tests' own key list holds no feed key.
    REQUIRE_EQ(code_of(read_feed(signed_text(good_payload()),
                                 test_keys::trusted(), 0)),
               std::string("signature_invalid"));

    // A key that is trusted, with another public half.
    sf::trusted_key stranger = feed_key();
    stranger.key[0] = static_cast<std::uint8_t>(stranger.key[0] ^ 0x01);
    const std::vector<sf::trusted_key> strangers{stranger};
    REQUIRE_EQ(code_of(read_feed(signed_text(good_payload()), strangers, 0)),
               std::string("signature_invalid"));

    // A key revoked outright.
    sf::trusted_key revoked = feed_key();
    revoked.revoked_from = sf::time_point::min();
    const std::vector<sf::trusted_key> revoked_keys{revoked};
    REQUIRE_EQ(code_of(read_feed(signed_text(good_payload()), revoked_keys, 0)),
               std::string("signature_invalid"));
}

TEST_CASE("update feed: the payload is not read before the signature "
          "verified")
{
    const std::vector<sf::trusted_key> keys{feed_key()};
    // A payload that would be refused as malformed, with a signature that
    // does not verify: the answer is about the signature.
    for (const std::string_view payload :
         {"this is not JSON", "{}", "{\"feed_version\": 2}", ""})
    {
        auto file = signing::sign(feed_purpose, test_keys::key_id,
                                  bytes_of(payload), test_keys::private_key());
        file.sig[0] = static_cast<std::uint8_t>(file.sig[0] ^ 0x01);
        std::string detail;
        REQUIRE_EQ(std::string(payload) + " -> " +
                       code_of(read_feed(sf::write(file), keys, 0, &detail)),
                   std::string(payload) + " -> signature_invalid");
        REQUIRE(!detail.empty());
    }

    // A good feed with a changed payload: the signature is for the old one.
    auto file =
        signing::sign(feed_purpose, test_keys::key_id, bytes_of(good_payload()),
                      test_keys::private_key());
    file.payload =
        bytes_of(edit(good_payload(), "\"serial\": 12", "\"serial\": 13"));
    REQUIRE_EQ(code_of(read_feed(sf::write(file), keys, 0)),
               std::string("signature_invalid"));

    // A newer format and a rollback are not looked at either.
    auto newer = signing::sign(feed_purpose, test_keys::key_id,
                               bytes_of("{\"feed_version\": 2}"),
                               test_keys::private_key());
    newer.sig[63] = static_cast<std::uint8_t>(newer.sig[63] ^ 0x80);
    REQUIRE_EQ(code_of(read_feed(sf::write(newer), keys, 0)),
               std::string("signature_invalid"));
}

TEST_CASE("update feed: a signed payload that is not a feed is refused with "
          "its reason")
{
    const std::vector<sf::trusted_key> keys{feed_key()};
    std::string detail;
    REQUIRE_EQ(
        code_of(read_feed(signed_text("this is not JSON"), keys, 0, &detail)),
        std::string("feed_malformed"));
    REQUIRE(contains(detail, "not JSON"));

    REQUIRE_EQ(
        code_of(read_feed(
            signed_text(edit(good_payload(), "\"size\": 1000", "\"size\": 0")),
            keys, 0, &detail)),
        std::string("feed_malformed"));
    REQUIRE(contains(detail, "studio[0].files[0].size"));

    REQUIRE_EQ(code_of(read_feed(signed_text("{\"feed_version\": 2}"), keys, 0,
                                 &detail)),
               std::string("feed_version_unsupported"));
    REQUIRE(contains(detail, "feed_version"));
}

TEST_CASE("update feed: the key's window is checked against issued_at")
{
    const std::string text = signed_text(good_payload());
    // The feed was issued at 2026-12-01 03:40:00 UTC.
    const std::chrono::sys_seconds issued =
        std::chrono::sys_days{2026y / 12 / 1} + 3h + 40min;

    sf::trusted_key key = feed_key();
    REQUIRE(read_feed(text, std::vector<sf::trusted_key>{key}, 0).has_value());

    // Inside the window, and on both of its edges.
    key.not_before = issued - 1h;
    key.not_after = issued + 1h;
    REQUIRE(read_feed(text, std::vector<sf::trusted_key>{key}, 0).has_value());
    key.not_before = issued;
    key.not_after = issued;
    REQUIRE(read_feed(text, std::vector<sf::trusted_key>{key}, 0).has_value());

    // Before the window opens, and after it closes.
    key = feed_key();
    key.not_before = issued + 1s;
    std::string detail;
    REQUIRE_EQ(
        code_of(read_feed(text, std::vector<sf::trusted_key>{key}, 0, &detail)),
        std::string("signature_invalid"));
    REQUIRE(!detail.empty());
    key = feed_key();
    key.not_after = issued - 1s;
    REQUIRE_EQ(code_of(read_feed(text, std::vector<sf::trusted_key>{key}, 0)),
               std::string("signature_invalid"));

    // A key that leaked later signed this feed before, and a key that leaked
    // at or before it did not.
    key = feed_key();
    key.revoked_from = issued + 1s;
    REQUIRE(read_feed(text, std::vector<sf::trusted_key>{key}, 0).has_value());
    key.revoked_from = issued;
    REQUIRE_EQ(code_of(read_feed(text, std::vector<sf::trusted_key>{key}, 0)),
               std::string("signature_invalid"));
    key.revoked_from = issued - 1s;
    REQUIRE_EQ(code_of(read_feed(text, std::vector<sf::trusted_key>{key}, 0)),
               std::string("signature_invalid"));
}

TEST_CASE("update feed: the checks come in the documented order")
{
    sf::trusted_key outside = feed_key();
    outside.not_after = sf::time_point::min() + 1s;
    const std::vector<sf::trusted_key> outside_keys{outside};
    const std::vector<sf::trusted_key> keys{feed_key()};

    // The payload is parsed before the window: a malformed payload under a
    // key that does not cover it is a malformed payload.
    REQUIRE_EQ(code_of(read_feed(signed_text("{}"), outside_keys, 0)),
               std::string("feed_malformed"));
    REQUIRE_EQ(code_of(read_feed(signed_text("{\"feed_version\": 2}"),
                                 outside_keys, 0)),
               std::string("feed_version_unsupported"));

    // The window is checked before the serial.
    REQUIRE_EQ(
        code_of(read_feed(signed_text(good_payload()), outside_keys, 1000)),
        std::string("signature_invalid"));

    // The serial is checked last, and only for a feed that is otherwise good.
    REQUIRE_EQ(code_of(read_feed(signed_text("{}"), keys, 1000)),
               std::string("feed_malformed"));
    REQUIRE_EQ(code_of(read_feed(signed_text(good_payload()), keys, 1000)),
               std::string("feed_rolled_back"));

    // The envelope comes before the keys' purposes, and an empty list of keys
    // before everything.
    REQUIRE_EQ(code_of(read_feed("x", keys, 1000)),
               std::string("signature_invalid"));
    REQUIRE_EQ(
        code_of(read_feed("x", std::span<const sf::trusted_key>(), 1000)),
        std::string("no_update_keys"));
}

TEST_CASE("update feed: an older serial is a rollback and the same one is not")
{
    const std::vector<sf::trusted_key> keys{feed_key()};
    const std::string text = signed_text(good_payload());

    REQUIRE(read_feed(text, keys, 0).has_value());
    REQUIRE(read_feed(text, keys, 1).has_value());
    REQUIRE(read_feed(text, keys, 11).has_value());
    // The same feed fetched again.
    REQUIRE(read_feed(text, keys, 12).has_value());

    std::string detail;
    REQUIRE_EQ(code_of(read_feed(text, keys, 13, &detail)),
               std::string("feed_rolled_back"));
    REQUIRE(contains(detail, "12"));
    REQUIRE(contains(detail, "13"));
    REQUIRE_EQ(code_of(read_feed(text, keys, 9007199254740992ULL)),
               std::string("feed_rolled_back"));
    REQUIRE_EQ(code_of(read_feed(text, keys, 18446744073709551615ULL)),
               std::string("feed_rolled_back"));
}

// -- the keys built in ------------------------------------------------------

TEST_CASE("update feed: this build has no update keys yet")
{
    REQUIRE(builtin_feed_keys().empty());
    REQUIRE(product_feed_keys().empty());
    // No key from the product list can be a test key, and none is.
    for (const sf::trusted_key& key : product_feed_keys())
    {
        REQUIRE(!key.key_id.starts_with(sf::test_key_prefix));
        REQUIRE_EQ(key.purpose, std::string(feed_purpose));
    }
    for (const sf::trusted_key& key : builtin_feed_keys())
    {
        REQUIRE(!key.key_id.starts_with(sf::test_key_prefix));
        REQUIRE_EQ(key.purpose, std::string(feed_purpose));
    }
    // So the product list reads nothing: a build without the owner's keys
    // cannot check for updates, and the test key does not stand in for them.
    const auto keys = product_feed_keys();
    REQUIRE_EQ(code_of(read_feed(signed_text(good_payload()), keys, 0)),
               std::string("no_update_keys"));
}

TEST_CASE("update feed: the constants are the ones the design gives")
{
    REQUIRE_EQ(feed_purpose, std::string_view("update-feed"));
    REQUIRE_EQ(feed_format_version, 1);
    REQUIRE_EQ(max_studio_releases, 2);
    REQUIRE_EQ(max_files, 16);
    REQUIRE_EQ(max_plugins, 64);
    REQUIRE_EQ(max_summary_length, 2000);
    REQUIRE_EQ(max_url_length, 2048);
    REQUIRE_EQ(max_name_length, 255);
    REQUIRE_EQ(max_file_size, 4294967296ULL);
    REQUIRE_EQ(official_plugin_prefix,
               std::string_view("com.virtualserialstudio."));
    // The envelope holds 64 KiB, and the feed is read from nothing else.
    REQUIRE_EQ(sf::max_file_size, 65536);
}
