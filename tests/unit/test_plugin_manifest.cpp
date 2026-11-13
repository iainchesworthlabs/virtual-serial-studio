//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/error.hpp>
#include <vss/plugin/manifest.hpp>

#include "test_framework.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace vss;
using namespace vss::plugin;

namespace
{

// -- failing with the place of the caller -------------------------------------

[[noreturn]] void
fail_at(const std::string& message,
        std::source_location where = std::source_location::current())
{
    vss_test::raise(message.c_str(), where.file_name(),
                    static_cast<int>(where.line()));
}

// -- text ---------------------------------------------------------------------

std::string repeat(std::string_view unit, std::size_t count)
{
    std::string out;
    for (std::size_t index = 0; index < count; ++index)
        out.append(unit);
    return out;
}

/// `text` with the one occurrence of `from` changed to `to`.  A text that does
/// not hold `from` exactly once fails the test.
std::string
replaced(std::string text, std::string_view from, std::string_view to,
         std::source_location where = std::source_location::current())
{
    const std::size_t at = text.find(from);
    if (at == std::string::npos ||
        text.find(from, at + 1) != std::string::npos)
        fail_at("the text is not there exactly once: " + std::string(from),
                where);
    text.replace(at, from.size(), to);
    return text;
}

/// `["p0", "p1", ...]` with `count` names.
std::string name_list(std::string_view prefix, std::size_t count)
{
    std::string out = "[";
    for (std::size_t index = 0; index < count; ++index)
    {
        if (index != 0)
            out += ", ";
        out += "\"" + std::string(prefix) + std::to_string(index) + "\"";
    }
    out += ']';
    return out;
}

std::vector<std::string> strings(std::initializer_list<std::string_view> items)
{
    return std::vector<std::string>(items.begin(), items.end());
}

localized_text
texts(std::initializer_list<std::pair<std::string, std::string>> entries)
{
    localized_text out;
    out.entries.assign(entries.begin(), entries.end());
    return out;
}

std::chrono::year_month_day date(int y, int m, int d)
{
    return std::chrono::year_month_day{
        std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(m)},
        std::chrono::day{static_cast<unsigned>(d)}};
}

api_version api_at(int major, int minor)
{
    return api_version{static_cast<std::uint16_t>(major),
                       static_cast<std::uint16_t>(minor)};
}

bool all_bytes_are(const sha512_digest& digest, std::uint8_t expected)
{
    for (const std::uint8_t byte : digest)
    {
        if (byte != expected)
            return false;
    }
    return true;
}

// -- manifests ----------------------------------------------------------------

/// A complete, valid manifest: one fixture with one extension point of the
/// class "rules", two parameters, two permissions and no "files".  The
/// parameters are given, so that a test can put its own in their place.
std::string fixture_text(std::string_view parameters)
{
    std::string out = R"j({
  "manifest_version": 1,
  "id": "com.example.fixture",
  "name": "Example fixture",
  "version": "1.2.3",
  "requires": { "api": { "min": "1.0", "max": "1.x" } },
  "run": { "executable": "bin/fixture", "platforms": ["linux-x64"] },
  "permissions": [
    { "name": "ports.read", "reason": "Needs to see its port." },
    { "name": "faults", "required": false }
  ],
  "extension_points": [
    {
      "kind": "fixture",
      "id": "slave",
      "name": "Slave",
      "timing": { "class": "rules" },
      "parameters": [)j";
    out.append(parameters);
    out += R"j(]
    }
  ]
})j";
    return out;
}

constexpr std::string_view default_parameters = R"j(
        { "id": "address", "type": "int", "min": 1, "max": 247,
          "default": 5, "label": "Address" },
        { "id": "enabled", "type": "bool", "default": true,
          "label": "Enabled" }
      )j";

std::string minimal_text()
{
    return fixture_text(default_parameters);
}

// Placeholders @D1@, @D2@ and @D3@ stand for the digests of the files.
constexpr std::string_view full_template = R"j({
  "manifest_version": 1,
  "id": "com.example.full-plugin",
  "name": "Full plugin",
  "version": "2.10.3-rc.1",
  "author": { "name": "Example Ltd", "url": "https://example.com/about" },
  "description": "Does everything a manifest can say.",
  "license": "Apache-2.0",
  "not_after": "2027-12-31",
  "released": "2026-11-13",
  "requires": {
    "api": { "min": "1.2", "max": "1.4" },
    "capabilities": ["engine_rules", "engine_faults"],
    "edition": "pro",
    "runtime": ["python3", "dotnet-8.0"]
  },
  "run": {
    "executable": "bin/full",
    "args": ["--stdio-channel", "--verbose"],
    "platforms": ["windows-x64", "linux-x64", "macos-arm64"]
  },
  "permissions": [
    { "name": "ports.read", "reason": "To see its port." },
    { "name": "modifier.inline", "required": true,
      "reason": "To change bytes on a link." },
    { "name": "net.listen", "reason": "Serves a status page.",
      "scope": ["127.0.0.1:8080", "[::1]:8080"] },
    { "name": "bundle.section", "required": false }
  ],
  "extension_points": [
    {
      "kind": "modifier",
      "id": "byte-editor",
      "name": { "en": "Byte editor", "de": "Byte-Editor" },
      "description": "Changes bytes on a link.",
      "timing": { "class": "inline", "budget_us": 500,
                  "on_timeout": "fail_closed" },
      "max_instances": 4,
      "parameters": [
        { "id": "invert", "type": "bool", "default": false,
          "label": "Invert" },
        { "id": "offset", "type": "int", "min": -100, "max": 100,
          "step": 5, "default": -10, "unit": "ms", "label": "Offset",
          "help": "Shifts the reply." },
        { "id": "gain", "type": "number", "min": 0.5, "max": 2.5,
          "step": 0.25, "default": 1.5, "unit": "x",
          "label": { "en": "Gain", "de": "Verstaerkung" } },
        { "id": "mode", "type": "enum", "default": "slow", "label": "Mode",
          "options": [ { "value": "fast", "label": "Fast" },
                       { "value": "slow", "label": "Slow" } ] },
        { "id": "prefix", "type": "string", "max_length": 8,
          "default": "abc", "label": "Prefix" },
        { "id": "frame", "type": "bytes", "max_length": 16,
          "default": "02 31 03", "label": "Frame" },
        { "id": "delay", "type": "duration", "min": "0us", "max": "1s",
          "default": "3ms", "label": "Delay" },
        { "id": "serial", "type": "port", "label": "Serial port" },
        { "id": "wire", "type": "link", "label": "Link" },
        { "id": "folder", "type": "path", "kind": "directory",
          "access": "write", "must_exist": true, "label": "Log folder" },
        { "id": "token", "type": "secret", "max_length": 64,
          "label": "Token" }
      ]
    },
    { "kind": "generator", "id": "pulse", "name": "Pulse" },
    { "kind": "adapter", "id": "bridge", "name": "Bridge",
      "timing": { "class": "async" } }
  ],
  "contributes": {
    "bundle_sections": [
      { "id": "state", "title": "Plugin state", "sensitive": false,
        "parameters": [
          { "id": "depth", "type": "int", "default": 2, "label": "Depth" }
        ] },
      { "id": "traces", "title": { "en": "Traces", "de": "Spuren" } }
    ]
  },
  "files": [
    { "path": "bin/full", "size": 1234, "sha512": "@D1@" },
    { "path": "lib/data.bin", "size": 0, "sha512": "@D2@" },
    { "path": "README.txt", "size": 42, "sha512": "@D3@" }
  ]
})j";

/// 128 hexadecimal digits that read as 64 bytes of 0xab.
std::string digest_ab()
{
    return repeat("ab", 64);
}

/// The same digit pairs in capitals, so that both cases are read.
std::string digest_upper_ab()
{
    return repeat("AB", 64);
}

/// The bytes 01 23 45 67 89 ab cd ef, eight times over.
std::string digest_pattern()
{
    return repeat("0123456789abcdef", 8);
}

std::string full_text()
{
    std::string text = replaced(std::string(full_template), "@D1@",
                                digest_upper_ab());
    text = replaced(text, "@D2@", digest_pattern());
    return replaced(text, "@D3@", digest_ab());
}

/// Only what a manifest must have, and no extension point.  `extra` is more
/// members, each starting with a comma.
std::string bare_text(std::string_view extra)
{
    std::string out = R"j({
  "manifest_version": 1,
  "id": "com.example.bare",
  "name": "Bare",
  "version": "1.0.0",
  "requires": { "api": { "min": "1.0", "max": "1.x" } },
  "run": { "executable": "bin/bare", "platforms": ["linux-x64"] })j";
    out.append(extra);
    out += "\n}";
    return out;
}

/// A manifest that only adds a support bundle section.
std::string sections_text(std::string_view sections)
{
    return bare_text(R"j(, "permissions": [ { "name": "bundle.section" } ],
  "contributes": { "bundle_sections": [)j" +
                     std::string(sections) + "] }");
}

// -- changing the minimal manifest --------------------------------------------

/// The minimal manifest with the string member `"key": "from"` changed to
/// `"key": "to"`; `to` is JSON text, so it may hold escapes.
std::string with_string(std::string_view key, std::string_view from,
                        std::string_view to)
{
    const std::string head = "\"" + std::string(key) + "\": \"";
    return replaced(minimal_text(), head + std::string(from) + "\"",
                    head + std::string(to) + "\"");
}

/// The minimal manifest with one more top-level member, such as
/// `"not_after": "2027-12-31"`.
std::string with_top(std::string_view member)
{
    return replaced(minimal_text(), "\"manifest_version\": 1,",
                    "\"manifest_version\": 1, " + std::string(member) + ",");
}

std::string with_date(std::string_view key, std::string_view date_text)
{
    return with_top("\"" + std::string(key) + "\": \"" +
                    std::string(date_text) + "\"");
}

constexpr std::string_view api_members = R"j("min": "1.0", "max": "1.x")j";

std::string with_api(std::string_view min, std::string_view max)
{
    return replaced(minimal_text(), api_members,
                    "\"min\": \"" + std::string(min) + "\", \"max\": \"" +
                        std::string(max) + "\"");
}

/// More members of `requires`, such as `"edition": "pro"`.
std::string with_requires(std::string_view members)
{
    return replaced(minimal_text(), R"j("max": "1.x" } })j",
                    R"j("max": "1.x" }, )j" + std::string(members) + " }");
}

constexpr std::string_view run_members =
    R"j("executable": "bin/fixture", "platforms": ["linux-x64"])j";

/// The minimal manifest with these members in `run`.
std::string with_run(std::string_view members)
{
    return replaced(minimal_text(), run_members, members);
}

std::string with_executable(std::string_view path)
{
    return with_run("\"executable\": \"" + std::string(path) +
                    "\", \"platforms\": [\"linux-x64\"]");
}

constexpr std::string_view faults_json =
    R"j({ "name": "faults", "required": false })j";

/// The minimal manifest with the second permission replaced.
std::string with_permission(std::string_view permission_json)
{
    return replaced(minimal_text(), faults_json, permission_json);
}

/// The minimal manifest with one more extension point in front.
std::string with_first_point(std::string_view point_json)
{
    return replaced(minimal_text(), "\"extension_points\": [",
                    "\"extension_points\": [ " + std::string(point_json) +
                        ", ");
}

/// The minimal manifest whose extension point is of `kind` and has the timing
/// object `timing`, or none when `timing` is empty.
std::string point_text(std::string_view kind, std::string_view timing)
{
    const std::string text =
        replaced(minimal_text(), "\"kind\": \"fixture\"",
                 "\"kind\": \"" + std::string(kind) + "\"");
    const std::string member =
        timing.empty() ? std::string()
                       : "\"timing\": " + std::string(timing) + ",";
    return replaced(text, R"j("timing": { "class": "rules" },)j", member);
}

/// `text` with the permission "modifier.inline" in front of the others;
/// `fields` are more members of it, each after a comma.
std::string with_inline_permission(const std::string& text,
                                   std::string_view fields = {})
{
    return replaced(text, "\"permissions\": [",
                    "\"permissions\": [ { \"name\": \"modifier.inline\"" +
                        std::string(fields) + " }, ");
}

/// A modifier of the timing object `timing` that holds the permission it
/// needs for the class "inline".
std::string inline_text(std::string_view timing)
{
    return with_inline_permission(point_text("modifier", timing));
}

std::string with_instances(std::string_view count)
{
    return replaced(minimal_text(), "\"id\": \"slave\",",
                    "\"id\": \"slave\", \"max_instances\": " +
                        std::string(count) + ",");
}

/// The minimal manifest whose only parameter is `{ "id": "p", "label": "P",
/// <body> }`.
std::string param_text(std::string_view body)
{
    return fixture_text("{ \"id\": \"p\", \"label\": \"P\", " +
                        std::string(body) + " }");
}

std::string at_point(std::string_view tail)
{
    return "extension_points[0]" + std::string(tail);
}

std::string at_param(std::string_view tail)
{
    return "extension_points[0].parameters[0]" + std::string(tail);
}

// -- files --------------------------------------------------------------------

std::string file_json(std::string_view path, std::string_view size,
                      std::string_view digest)
{
    return "{ \"path\": \"" + std::string(path) + "\", \"size\": " +
           std::string(size) + ", \"sha512\": \"" + std::string(digest) +
           "\" }";
}

std::string good_file(std::string_view path)
{
    return file_json(path, "10", digest_ab());
}

/// The minimal manifest with this list of files, written as the text of the
/// entries.
std::string with_files(std::string_view entries)
{
    return with_top("\"files\": [" + std::string(entries) + "]");
}

// -- parsing ------------------------------------------------------------------

/// The manifest `text` holds; the test fails when it is refused.
manifest parse_ok(const std::string& text,
                  std::source_location where = std::source_location::current())
{
    std::string detail;
    auto parsed = parse_manifest(text, &detail);
    if (parsed.has_error())
        fail_at("the manifest was refused: " + detail, where);
    if (!detail.empty())
        fail_at("an accepted manifest left a detail: " + detail, where);
    return std::move(*parsed);
}

/// The test fails unless `text` is refused with `code` and a detail that has
/// `detail_part` in it.  `label` says which case of a table this is.
void expect_error(const std::string& text, plugin_errc code,
                  std::string_view detail_part, std::string_view label = {},
                  std::source_location where = std::source_location::current())
{
    std::string detail;
    const auto parsed = parse_manifest(text, &detail);
    const std::string wanted = std::string(to_string(code)) + " with \"" +
                               std::string(detail_part) + "\" (" +
                               std::string(label) + ")";
    if (parsed.has_value())
        fail_at("accepted, but wanted " + wanted, where);
    if (parsed.error() != make_error_code(code))
        fail_at("refused with " + parsed.error().message() + ": " + detail +
                    "; wanted " + wanted,
                where);
    if (detail.find(detail_part) == std::string::npos)
        fail_at("the detail is \"" + detail + "\"; wanted " + wanted, where);
}

void expect_invalid(const std::string& text, std::string_view detail_part,
                    std::string_view label = {},
                    std::source_location where =
                        std::source_location::current())
{
    expect_error(text, plugin_errc::manifest_invalid, detail_part, label,
                 where);
}

void expect_layout(const std::string& text, std::string_view detail_part,
                   std::string_view label = {},
                   std::source_location where =
                       std::source_location::current())
{
    expect_error(text, plugin_errc::layout_invalid, detail_part, label, where);
}

const parameter& parameter_named(const extension_point& point,
                                 std::string_view id)
{
    for (const parameter& item : point.parameters)
    {
        if (item.id == id)
            return item;
    }
    fail_at("the extension point has no parameter " + std::string(id));
}

} // namespace

// -- valid manifests ----------------------------------------------------------

TEST_CASE("plugin manifest: the minimal manifest is read")
{
    const manifest value = parse_ok(minimal_text());

    REQUIRE_EQ(value.id, "com.example.fixture");
    REQUIRE_EQ(value.name, "Example fixture");
    const plugin_version version{1, 2, 3, std::string()};
    REQUIRE(value.version == version);
    REQUIRE_EQ(to_string(value.version), "1.2.3");
    REQUIRE(value.author.name.empty());
    REQUIRE(value.author.url.empty());
    REQUIRE(value.description.empty());
    REQUIRE(value.license.empty());
    REQUIRE(!value.not_after.has_value());
    REQUIRE(!value.released.has_value());

    REQUIRE_EQ(value.needs.api.min.major, 1);
    REQUIRE_EQ(value.needs.api.min.minor, 0);
    REQUIRE_EQ(value.needs.api.max_major, 1);
    REQUIRE(!value.needs.api.max_minor.has_value());
    REQUIRE(value.needs.capabilities.empty());
    REQUIRE(value.needs.edition.empty());
    REQUIRE(value.needs.runtime.empty());

    REQUIRE_EQ(value.run.executable, "bin/fixture");
    REQUIRE(value.run.args.empty());
    REQUIRE(value.run.platforms == strings({"linux-x64"}));

    REQUIRE_EQ(value.permissions.size(), 2);
    const permission_request& reads = value.permissions[0];
    REQUIRE(reads.name == permission::ports_read);
    REQUIRE(reads.required);
    REQUIRE_EQ(reads.reason, "Needs to see its port.");
    REQUIRE(reads.scope.empty());
    const permission_request& faults_request = value.permissions[1];
    REQUIRE(faults_request.name == permission::faults);
    REQUIRE(!faults_request.required);
    REQUIRE(faults_request.reason.empty());
    REQUIRE(value.find(permission::faults) == &faults_request);
    REQUIRE(value.find(permission::net_listen) == nullptr);

    REQUIRE(value.bundle_sections.empty());
    REQUIRE(value.files.empty());
}

TEST_CASE("plugin manifest: minimal extension point and parameters")
{
    const manifest value = parse_ok(minimal_text());

    REQUIRE_EQ(value.extension_points.size(), 1);
    const extension_point& point = value.extension_points[0];
    REQUIRE(point.kind == extension_kind::fixture);
    REQUIRE_EQ(point.id, "slave");
    REQUIRE_EQ(point.name.entries.size(), 1);
    REQUIRE(point.name.entries[0].first.empty());
    REQUIRE_EQ(point.name.entries[0].second, "Slave");
    REQUIRE(point.description.empty());
    REQUIRE(point.timing.type == timing_class::rules);
    REQUIRE_EQ(point.timing.budget_us, 0);
    REQUIRE_EQ(point.max_instances, 16);
    REQUIRE_EQ(point.max_instances, default_instances);

    REQUIRE_EQ(point.parameters.size(), 2);
    const parameter& address = point.parameters[0];
    REQUIRE_EQ(address.id, "address");
    REQUIRE(address.type == parameter_type::integer);
    REQUIRE_EQ(address.label.best("en"), "Address");
    REQUIRE_EQ(std::get<std::int64_t>(address.min), 1);
    REQUIRE_EQ(std::get<std::int64_t>(address.max), 247);
    REQUIRE_EQ(std::get<std::int64_t>(address.default_value), 5);
    REQUIRE(std::holds_alternative<std::monostate>(address.step));
    REQUIRE(address.unit.empty());
    REQUIRE(address.choices.empty());

    const parameter& enabled = point.parameters[1];
    REQUIRE_EQ(enabled.id, "enabled");
    REQUIRE(enabled.type == parameter_type::boolean);
    REQUIRE(std::get<bool>(enabled.default_value));
    REQUIRE(std::holds_alternative<std::monostate>(enabled.min));
    REQUIRE(std::holds_alternative<std::monostate>(enabled.max));
}

TEST_CASE("plugin manifest: what is left out takes its default")
{
    const manifest value = parse_ok(with_first_point(
        R"j({ "kind": "generator", "id": "pulse", "name": "Pulse" })j"));

    REQUIRE_EQ(value.extension_points.size(), 2);
    const extension_point& generator = value.extension_points[0];
    REQUIRE(generator.kind == extension_kind::generator);
    REQUIRE(generator.timing.type == timing_class::async);
    REQUIRE_EQ(generator.timing.budget_us, 0);
    REQUIRE(generator.timing.on_timeout == timeout_action::fail_open);
    REQUIRE_EQ(generator.max_instances, 16);
    REQUIRE(generator.parameters.empty());

    // A permission that does not say is required.
    REQUIRE(value.permissions[0].required);

    const manifest fixture_without_timing =
        parse_ok(point_text("fixture", ""));
    REQUIRE(fixture_without_timing.extension_points[0].timing.type ==
            timing_class::async);
    const manifest adapter_without_timing =
        parse_ok(point_text("adapter", ""));
    REQUIRE(adapter_without_timing.extension_points[0].timing.type ==
            timing_class::async);
}

TEST_CASE("plugin manifest: the full manifest, header and needs")
{
    const manifest value = parse_ok(full_text());

    REQUIRE_EQ(value.id, "com.example.full-plugin");
    REQUIRE_EQ(value.name, "Full plugin");
    const plugin_version version{2, 10, 3, "rc.1"};
    REQUIRE(value.version == version);
    REQUIRE_EQ(to_string(value.version), "2.10.3-rc.1");
    REQUIRE_EQ(value.author.name, "Example Ltd");
    REQUIRE_EQ(value.author.url, "https://example.com/about");
    REQUIRE_EQ(value.description, "Does everything a manifest can say.");
    REQUIRE_EQ(value.license, "Apache-2.0");
    REQUIRE(value.not_after.has_value());
    REQUIRE(*value.not_after == date(2027, 12, 31));
    REQUIRE(value.released.has_value());
    REQUIRE(*value.released == date(2026, 11, 13));

    REQUIRE_EQ(value.needs.api.min.major, 1);
    REQUIRE_EQ(value.needs.api.min.minor, 2);
    REQUIRE_EQ(value.needs.api.max_major, 1);
    REQUIRE(value.needs.api.max_minor.has_value());
    REQUIRE_EQ(*value.needs.api.max_minor, 4);
    REQUIRE(value.needs.capabilities ==
            strings({"engine_rules", "engine_faults"}));
    REQUIRE_EQ(value.needs.edition, "pro");
    REQUIRE(value.needs.runtime == strings({"python3", "dotnet-8.0"}));
}

TEST_CASE("plugin manifest: the full manifest, run and permissions")
{
    const manifest value = parse_ok(full_text());

    REQUIRE_EQ(value.run.executable, "bin/full");
    REQUIRE(value.run.args == strings({"--stdio-channel", "--verbose"}));
    REQUIRE(value.run.platforms ==
            strings({"windows-x64", "linux-x64", "macos-arm64"}));

    REQUIRE_EQ(value.permissions.size(), 4);
    REQUIRE(value.permissions[0].name == permission::ports_read);
    REQUIRE(value.permissions[0].required);
    REQUIRE_EQ(value.permissions[0].reason, "To see its port.");

    const permission_request* const edit =
        value.find(permission::modifier_inline);
    REQUIRE(edit != nullptr);
    REQUIRE(edit->required);
    REQUIRE_EQ(edit->reason, "To change bytes on a link.");
    REQUIRE(edit->scope.empty());

    const permission_request* const listen =
        value.find(permission::net_listen);
    REQUIRE(listen != nullptr);
    REQUIRE(listen->required);
    REQUIRE(listen->scope == strings({"127.0.0.1:8080", "[::1]:8080"}));

    const permission_request* const section =
        value.find(permission::bundle_section);
    REQUIRE(section != nullptr);
    REQUIRE(!section->required);
    REQUIRE(section->reason.empty());
}

TEST_CASE("plugin manifest: the full manifest, extension points")
{
    const manifest value = parse_ok(full_text());

    REQUIRE_EQ(value.extension_points.size(), 3);
    const extension_point& modifier = value.extension_points[0];
    REQUIRE(modifier.kind == extension_kind::modifier);
    REQUIRE_EQ(modifier.id, "byte-editor");
    REQUIRE_EQ(modifier.name.entries.size(), 2);
    REQUIRE_EQ(modifier.name.best("de"), "Byte-Editor");
    REQUIRE_EQ(modifier.name.best("fr"), "Byte editor");
    REQUIRE_EQ(modifier.description.best("en"), "Changes bytes on a link.");
    REQUIRE(modifier.timing.type == timing_class::in_line);
    REQUIRE_EQ(modifier.timing.budget_us, 500);
    REQUIRE(modifier.timing.on_timeout == timeout_action::fail_closed);
    REQUIRE_EQ(modifier.max_instances, 4);
    REQUIRE_EQ(modifier.parameters.size(), 11);

    const extension_point& generator = value.extension_points[1];
    REQUIRE(generator.kind == extension_kind::generator);
    REQUIRE_EQ(generator.id, "pulse");
    REQUIRE(generator.timing.type == timing_class::async);
    REQUIRE_EQ(generator.max_instances, 16);

    const extension_point& adapter = value.extension_points[2];
    REQUIRE(adapter.kind == extension_kind::adapter);
    REQUIRE_EQ(adapter.id, "bridge");
    REQUIRE(adapter.timing.type == timing_class::async);
}

TEST_CASE("plugin manifest: the full manifest, parameters of every type")
{
    const manifest value = parse_ok(full_text());
    const extension_point& point = value.extension_points[0];

    const parameter& invert = parameter_named(point, "invert");
    REQUIRE(invert.type == parameter_type::boolean);
    REQUIRE(!std::get<bool>(invert.default_value));

    const parameter& offset = parameter_named(point, "offset");
    REQUIRE(offset.type == parameter_type::integer);
    REQUIRE_EQ(std::get<std::int64_t>(offset.min), -100);
    REQUIRE_EQ(std::get<std::int64_t>(offset.max), 100);
    REQUIRE_EQ(std::get<std::int64_t>(offset.step), 5);
    REQUIRE_EQ(std::get<std::int64_t>(offset.default_value), -10);
    REQUIRE_EQ(offset.unit, "ms");
    REQUIRE_EQ(offset.help.best("en"), "Shifts the reply.");

    const parameter& gain = parameter_named(point, "gain");
    REQUIRE(gain.type == parameter_type::number);
    REQUIRE_NEAR(std::get<double>(gain.min), 0.5, 1e-12);
    REQUIRE_NEAR(std::get<double>(gain.max), 2.5, 1e-12);
    REQUIRE_NEAR(std::get<double>(gain.step), 0.25, 1e-12);
    REQUIRE_NEAR(std::get<double>(gain.default_value), 1.5, 1e-12);
    REQUIRE_EQ(gain.unit, "x");
    REQUIRE_EQ(gain.label.entries.size(), 2);
    REQUIRE_EQ(gain.label.best("de"), "Verstaerkung");
    REQUIRE_EQ(gain.label.best("fr"), "Gain");

    const parameter& mode = parameter_named(point, "mode");
    REQUIRE(mode.type == parameter_type::choice);
    REQUIRE_EQ(mode.choices.size(), 2);
    REQUIRE_EQ(mode.choices[0].value, "fast");
    REQUIRE_EQ(mode.choices[0].label.best("en"), "Fast");
    REQUIRE_EQ(mode.choices[1].value, "slow");
    REQUIRE_EQ(std::get<std::string>(mode.default_value), "slow");

    const parameter& prefix = parameter_named(point, "prefix");
    REQUIRE(prefix.type == parameter_type::string);
    REQUIRE_EQ(prefix.max_length, 8);
    REQUIRE_EQ(std::get<std::string>(prefix.default_value), "abc");

    const parameter& frame = parameter_named(point, "frame");
    REQUIRE(frame.type == parameter_type::bytes);
    REQUIRE_EQ(frame.max_length, 16);
    const std::string frame_bytes{'\x02', '1', '\x03'};
    REQUIRE_EQ(std::get<std::string>(frame.default_value), frame_bytes);

    const parameter& delay = parameter_named(point, "delay");
    REQUIRE(delay.type == parameter_type::duration);
    REQUIRE_EQ(std::get<std::int64_t>(delay.min), 0);
    REQUIRE_EQ(std::get<std::int64_t>(delay.max), 1'000'000'000);
    REQUIRE_EQ(std::get<std::int64_t>(delay.default_value), 3'000'000);

    const parameter& serial = parameter_named(point, "serial");
    REQUIRE(serial.type == parameter_type::port);
    REQUIRE(std::holds_alternative<std::monostate>(serial.default_value));
    const parameter& wire = parameter_named(point, "wire");
    REQUIRE(wire.type == parameter_type::link);
    REQUIRE(std::holds_alternative<std::monostate>(wire.default_value));

    const parameter& folder = parameter_named(point, "folder");
    REQUIRE(folder.type == parameter_type::path);
    REQUIRE(folder.kind == path_kind::directory);
    REQUIRE(folder.access == path_access::write);
    REQUIRE(folder.must_exist);

    const parameter& token = parameter_named(point, "token");
    REQUIRE(token.type == parameter_type::secret);
    REQUIRE_EQ(token.max_length, 64);
    REQUIRE(std::holds_alternative<std::monostate>(token.default_value));
}

TEST_CASE("plugin manifest: the full manifest, bundle sections")
{
    const manifest value = parse_ok(full_text());

    REQUIRE_EQ(value.bundle_sections.size(), 2);
    const bundle_section& state = value.bundle_sections[0];
    REQUIRE_EQ(state.id, "state");
    REQUIRE_EQ(state.title.best("en"), "Plugin state");
    REQUIRE(!state.sensitive);
    REQUIRE_EQ(state.parameters.size(), 1);
    REQUIRE_EQ(state.parameters[0].id, "depth");
    REQUIRE_EQ(std::get<std::int64_t>(state.parameters[0].default_value), 2);

    const bundle_section& traces = value.bundle_sections[1];
    REQUIRE_EQ(traces.id, "traces");
    REQUIRE_EQ(traces.title.best("de"), "Spuren");
    REQUIRE(traces.sensitive);
    REQUIRE(traces.parameters.empty());
}

TEST_CASE("plugin manifest: the full manifest, files")
{
    const manifest value = parse_ok(full_text());

    REQUIRE_EQ(value.files.size(), 3);
    REQUIRE_EQ(value.files[0].path, "bin/full");
    REQUIRE_EQ(value.files[0].size, 1234);
    REQUIRE(all_bytes_are(value.files[0].sha512, 0xab));

    REQUIRE_EQ(value.files[1].path, "lib/data.bin");
    REQUIRE_EQ(value.files[1].size, 0);
    const sha512_digest& pattern = value.files[1].sha512;
    REQUIRE_EQ(pattern[0], 0x01);
    REQUIRE_EQ(pattern[1], 0x23);
    REQUIRE_EQ(pattern[2], 0x45);
    REQUIRE_EQ(pattern[3], 0x67);
    REQUIRE_EQ(pattern[4], 0x89);
    REQUIRE_EQ(pattern[5], 0xab);
    REQUIRE_EQ(pattern[6], 0xcd);
    REQUIRE_EQ(pattern[7], 0xef);
    REQUIRE_EQ(pattern[8], 0x01);
    REQUIRE_EQ(pattern[63], 0xef);

    REQUIRE_EQ(value.files[2].path, "README.txt");
    REQUIRE_EQ(value.files[2].size, 42);
    REQUIRE(all_bytes_are(value.files[2].sha512, 0xab));
}

TEST_CASE("plugin manifest: api ranges")
{
    // 1.2 to 1.4.
    const manifest pinned_manifest = parse_ok(full_text());
    const api_range& pinned = pinned_manifest.needs.api;
    REQUIRE(pinned.contains(api_at(1, 2)));
    REQUIRE(pinned.contains(api_at(1, 3)));
    REQUIRE(pinned.contains(api_at(1, 4)));
    REQUIRE(!pinned.contains(api_at(1, 1)));
    REQUIRE(!pinned.contains(api_at(1, 5)));
    REQUIRE(!pinned.contains(api_at(0, 9)));
    REQUIRE(!pinned.contains(api_at(2, 0)));

    // 1.0 to 1.x.
    const manifest open_manifest = parse_ok(minimal_text());
    const api_range& any_minor = open_manifest.needs.api;
    REQUIRE(any_minor.contains(api_at(1, 0)));
    REQUIRE(any_minor.contains(api_at(1, 4)));
    REQUIRE(any_minor.contains(api_at(1, 65535)));
    REQUIRE(!any_minor.contains(api_at(2, 0)));
    REQUIRE(!any_minor.contains(api_at(0, 99)));

    // A range of one version.
    const manifest one = parse_ok(with_api("1.4", "1.4"));
    REQUIRE(one.needs.api.max_minor.has_value());
    REQUIRE(one.needs.api.contains(api_at(1, 4)));
    REQUIRE(!one.needs.api.contains(api_at(1, 3)));
    REQUIRE(!one.needs.api.contains(api_at(1, 5)));

    // A range over several majors.
    const manifest wide = parse_ok(with_api("1.7", "3.x"));
    REQUIRE(wide.needs.api.contains(api_at(1, 7)));
    REQUIRE(wide.needs.api.contains(api_at(2, 0)));
    REQUIRE(wide.needs.api.contains(api_at(3, 900)));
    REQUIRE(!wide.needs.api.contains(api_at(1, 6)));
    REQUIRE(!wide.needs.api.contains(api_at(4, 0)));

    REQUIRE(api_at(1, 2) < api_at(1, 3));
    REQUIRE(api_at(1, 9) < api_at(2, 0));
}

TEST_CASE("plugin manifest: parsing is repeatable")
{
    for (const std::string& text : {minimal_text(), full_text()})
    {
        const manifest first = parse_ok(text);
        const manifest second = parse_ok(text);
        REQUIRE(first == second);
        const auto without_detail = parse_manifest(text);
        REQUIRE(without_detail.has_value());
        REQUIRE(*without_detail == first);
    }
    REQUIRE(!(parse_ok(minimal_text()) == parse_ok(full_text())));

    // The detail of a manifest that is read is empty, whatever it held.
    std::string detail = "left over";
    const auto parsed = parse_manifest(minimal_text(), &detail);
    REQUIRE(parsed.has_value());
    REQUIRE(detail.empty());
}

// -- the document -------------------------------------------------------------

TEST_CASE("plugin manifest: a text over the size limit is too large")
{
    const std::string too_big(max_manifest_size + 1, ' ');
    expect_error(too_big, plugin_errc::manifest_too_large, "is larger than");

    std::string padded = minimal_text();
    padded.resize(max_manifest_size, ' ');
    REQUIRE(parse_manifest(padded).has_value());
    padded.push_back(' ');
    expect_error(padded, plugin_errc::manifest_too_large, "is larger than");

    // The limit is on the size, not on what the text says.
    const std::string at_the_limit(max_manifest_size, 'x');
    expect_invalid(at_the_limit, "is not JSON");
}

TEST_CASE("plugin manifest: text that is not JSON")
{
    expect_invalid("this is not json", "is not JSON");
    expect_invalid("", "is not JSON");
    expect_invalid("   \n", "is not JSON");
    expect_invalid("{", "is not JSON");
    expect_invalid("{\"manifest_version\": 1,}", "is not JSON");
    expect_invalid("{\"manifest_version\": 1", "is not JSON");
    expect_invalid(minimal_text().substr(0, 100), "is not JSON");
}

TEST_CASE("plugin manifest: a document that is not an object")
{
    expect_invalid("[]", "must be an object");
    expect_invalid("[{}]", "must be an object");
    expect_invalid("null", "must be an object");
    expect_invalid("42", "must be an object");
    expect_invalid("\"text\"", "must be an object");
    expect_invalid("{}", "manifest_version: is required");
}

TEST_CASE("plugin manifest: text after the document")
{
    expect_invalid(minimal_text() + " {}", "");
    expect_invalid(minimal_text() + "x", "");
    expect_invalid("{} {}", "");

    // White space after it is not text.
    REQUIRE(parse_manifest(minimal_text() + "\n\t \r\n").has_value());
}

TEST_CASE("plugin manifest: nesting deeper than the limit")
{
    const std::size_t levels = max_manifest_depth + 4;
    const std::string arrays = "{\"manifest_version\": 1, \"x\": " +
                               std::string(levels, '[') +
                               std::string(levels, ']') + "}";
    expect_invalid(arrays, "is not JSON");

    const std::string objects = "{\"manifest_version\": 1, \"x\": " +
                                repeat("{\"a\": ", levels) + "1" +
                                std::string(levels, '}') + "}";
    expect_invalid(objects, "is not JSON");

    expect_invalid(std::string(levels, '[') + std::string(levels, ']'),
                   "is not JSON");
}

TEST_CASE("plugin manifest: a key given twice")
{
    const std::string at_top = replaced(
        minimal_text(), "\"version\": \"1.2.3\",",
        "\"version\": \"1.2.3\", \"version\": \"1.2.4\",");
    expect_invalid(at_top, "the key \"version\" is given twice");

    const std::string nested =
        replaced(minimal_text(), "\"kind\": \"fixture\",",
                 "\"kind\": \"fixture\", \"kind\": \"fixture\",");
    expect_invalid(nested, "the key \"kind\" is given twice");

    const std::string in_parameter = replaced(
        minimal_text(), "\"label\": \"Address\"",
        "\"label\": \"Address\", \"label\": \"Other\"");
    expect_invalid(in_parameter, "the key \"label\" is given twice");

    const std::string in_language_map = replaced(
        minimal_text(), "\"label\": \"Address\"",
        "\"label\": { \"en\": \"A\", \"en\": \"B\" }");
    expect_invalid(in_language_map, "the key \"en\" is given twice");
}

TEST_CASE("plugin manifest: a key repeated through an escape")
{
    const std::string text = replaced(
        minimal_text(), "\"id\": \"com.example.fixture\",",
        "\"id\": \"com.example.fixture\", "
        "\"\\u0069d\": \"com.example.other\",");
    expect_invalid(text, "the key \"id\" is given twice");

    const std::string spelt_out = replaced(
        minimal_text(), "\"name\": \"Example fixture\",",
        "\"name\": \"Example fixture\", "
        "\"\\u006e\\u0061\\u006de\": \"Other\",");
    expect_invalid(spelt_out, "the key \"name\" is given twice");
}

TEST_CASE("plugin manifest: an unknown key is refused")
{
    expect_invalid(with_top("\"bogus\": 1"), "unknown key \"bogus\"");
    expect_invalid(with_top("\"bogus\": 1"), "manifest:");

    expect_invalid(with_run(R"j("executable": "bin/fixture", "bogus": true,
                             "platforms": ["linux-x64"])j"),
                   "run: unknown key \"bogus\"");

    expect_invalid(with_requires("\"bogus\": 1"),
                   "requires: unknown key \"bogus\"");

    expect_invalid(replaced(minimal_text(), "\"label\": \"Address\"",
                            "\"label\": \"Address\", \"bogus\": 1"),
                   at_param(": unknown key \"bogus\""));

    expect_invalid(replaced(minimal_text(), "\"class\": \"rules\"",
                            "\"class\": \"rules\", \"bogus\": 1"),
                   at_point(".timing: unknown key \"bogus\""));

    expect_invalid(with_permission(
                       R"j({ "name": "faults", "why": "because" })j"),
                   "permissions[1]: unknown key \"why\"");
}

TEST_CASE("plugin manifest: manifest_version")
{
    const std::string head = "\"manifest_version\": 1";
    expect_invalid(replaced(minimal_text(), head + ",", ""),
                   "manifest_version: is required");
    expect_invalid(replaced(minimal_text(), head, "\"manifest_version\": 0"),
                   "manifest_version: must be from 1 to");
    expect_invalid(replaced(minimal_text(), head,
                            "\"manifest_version\": \"1\""),
                   "manifest_version: must be a whole number");
    expect_invalid(replaced(minimal_text(), head, "\"manifest_version\": -1"),
                   "manifest_version: must be a whole number");
    expect_invalid(replaced(minimal_text(), head, "\"manifest_version\": 1.0"),
                   "manifest_version: must be a whole number");
    expect_invalid(replaced(minimal_text(), head,
                            "\"manifest_version\": 4294967296"),
                   "manifest_version: must be from 1 to");
    expect_invalid(replaced(minimal_text(), head, "\"manifest_version\": null"),
                   "manifest_version:");
}

TEST_CASE("plugin manifest: a newer manifest_version is unsupported")
{
    const std::string head = "\"manifest_version\": 1";
    expect_error(replaced(minimal_text(), head, "\"manifest_version\": 2"),
                 plugin_errc::manifest_unsupported_version,
                 "manifest_version: this version reads format 1 and the "
                 "manifest is format 2");
    expect_error(replaced(minimal_text(), head,
                          "\"manifest_version\": 4294967295"),
                 plugin_errc::manifest_unsupported_version,
                 "format 4294967295");

    // A newer format may have keys this reader does not know.  The answer is
    // still that it needs a newer version.
    expect_error(replaced(minimal_text(), head + ",",
                          "\"manifest_version\": 2, \"bogus\": 1,"),
                 plugin_errc::manifest_unsupported_version, "format 2");
    expect_error(R"j({ "manifest_version": 2, "id": 7, "extra": [] })j",
                 plugin_errc::manifest_unsupported_version, "format 2");
    expect_error("{\"manifest_version\": 2}",
                 plugin_errc::manifest_unsupported_version, "format 2");
}

// -- id, version and texts ----------------------------------------------------

TEST_CASE("plugin manifest: good plugin ids")
{
    const std::vector<std::string> good = {
        "com.example.fixture",
        "a.b",
        "com.example.my-plugin",
        "x1.y2-z3",
        "9.9",
        "com.example.a-b.c",
        std::string(max_id_length - 2, 'a') + ".b"};
    for (const std::string& id : good)
    {
        const manifest value =
            parse_ok(with_string("id", "com.example.fixture", id));
        REQUIRE_EQ(value.id, id);
    }
}

TEST_CASE("plugin manifest: bad plugin ids")
{
    const std::vector<std::string> bad = {
        "plugin",
        "Com.example.plugin",
        "com.Example.plugin",
        ".com.example",
        "com.example.",
        "-com.example",
        "com.example-",
        "com..example",
        "com.-example",
        "com.example-.x",
        "com.example--x",
        "a.",
        ".a",
        "ab",
        "",
        "com_example.x",
        "com.exa mple",
        "com.example.caf\\u00e9",
        std::string(max_id_length, 'a') + ".b"};
    for (const std::string& id : bad)
    {
        expect_invalid(with_string("id", "com.example.fixture", id),
                       "is not a plugin id", id);
    }
    expect_invalid(replaced(minimal_text(), "\"id\": \"com.example.fixture\"",
                            "\"id\": 7"),
                   "id: must be a string");
}

TEST_CASE("plugin manifest: good versions")
{
    const std::vector<std::string> good = {
        "1.2.3",
        "0.0.1",
        "1.0.0-rc.1",
        "10.20.30",
        "999999.999999.999999",
        "1.2.3-alpha-1",
        "1.2.3-0",
        "1.2.3-" + std::string(32, 'a')};
    for (const std::string& text : good)
    {
        const manifest value =
            parse_ok(with_string("version", "1.2.3", text));
        REQUIRE_EQ(to_string(value.version), text);
    }

    const manifest release = parse_ok(with_string("version", "1.2.3",
                                                  "4.5.6-beta.2"));
    const plugin_version expected{4, 5, 6, "beta.2"};
    REQUIRE(release.version == expected);
}

TEST_CASE("plugin manifest: bad versions")
{
    const std::vector<std::string> bad = {
        "1.2",         "1.2.3.4",     "01.2.3",       "1.02.3",
        "1.2.03",      "1.2.3-",      "v1.2.3",       "1.2.3+build",
        "1.2.3-rc.1+b", "1000000.0.0", "",             "1..3",
        "1.2.x",       "-1.2.3",      " 1.2.3",       "1.2.3 ",
        "1.2.3-a..b",  "1.2.3-.a",    "1.2.3-a.",     "1.2.3-rc_1",
        "1.2.3-" + std::string(33, 'a'), "1.2.3-caf\\u00e9"};
    for (const std::string& text : bad)
    {
        expect_invalid(with_string("version", "1.2.3", text),
                       "version: must be a version such as", text);
    }
    expect_invalid(replaced(minimal_text(), "\"version\": \"1.2.3\"",
                            "\"version\": 123"),
                   "version: must be a string");
}

TEST_CASE("plugin manifest: names with characters that mislead are refused")
{
    const std::vector<std::string> refused = {
        "bell\\u0007",  "nul\\u0000",   "tab\\t",       "newline\\n",
        "del\\u007f",   "next\\u0085",  "arabic\\u061c", "lrm\\u200e",
        "rlm\\u200f",   "lre\\u202a",   "rle\\u202b",   "pdf\\u202c",
        "lro\\u202d",   "rlo\\u202e",   "lri\\u2066",   "rli\\u2067",
        "fsi\\u2068",   "pdi\\u2069",   "line\\u2028",  "para\\u2029",
        "bom\\ufeff"};
    for (const std::string& name : refused)
    {
        expect_invalid(with_string("name", "Example fixture", name),
                       "name: has a control character or a direction mark",
                       name);
    }
}

TEST_CASE("plugin manifest: names that are empty, padded or too long")
{
    expect_invalid(with_string("name", "Example fixture", ""),
                   "name: must not be empty");
    expect_invalid(with_string("name", "Example fixture", " Example"),
                   "name: must not begin or end with a space");
    expect_invalid(with_string("name", "Example fixture", "Example "),
                   "name: must not begin or end with a space");
    expect_invalid(with_string("name", "Example fixture",
                               std::string(max_name_length + 1, 'a')),
                   "name: is longer than 100 bytes");
    expect_invalid(replaced(minimal_text(), "\"name\": \"Example fixture\"",
                            "\"name\": 5"),
                   "name: must be a string");

    // The limit is in bytes: fifty letters of two bytes fit, fifty-one do not.
    const manifest fits = parse_ok(
        with_string("name", "Example fixture", repeat("\\u00fc", 50)));
    REQUIRE_EQ(fits.name.size(), max_name_length);
    expect_invalid(with_string("name", "Example fixture",
                               repeat("\\u00fc", 51)),
                   "name: is longer than 100 bytes");
}

TEST_CASE("plugin manifest: names with ordinary text are accepted")
{
    const std::string exact(max_name_length, 'a');
    REQUIRE_EQ(parse_ok(with_string("name", "Example fixture", exact)).name,
               exact);

    const std::string umlauts = "Pr\xc3\xbc" "fger\xc3\xa4" "t";
    const manifest escaped = parse_ok(
        with_string("name", "Example fixture", "Pr\\u00fcfger\\u00e4t"));
    REQUIRE_EQ(escaped.name, umlauts);

    // The same name as raw UTF-8.
    const manifest raw = parse_ok(with_string("name", "Example fixture",
                                              umlauts));
    REQUIRE_EQ(raw.name, umlauts);

    // A letter outside the basic plane, written as a surrogate pair.
    const manifest pair = parse_ok(
        with_string("name", "Example fixture", "Smile \\ud83d\\ude00"));
    const std::string smile = "Smile \xf0\x9f\x98\x80";
    REQUIRE_EQ(pair.name, smile);

    const manifest punctuated = parse_ok(with_string(
        "name", "Example fixture", "Modbus (RTU) - slave: v2, \\\"quoted\\\""));
    REQUIRE_EQ(punctuated.name, "Modbus (RTU) - slave: v2, \"quoted\"");
}

TEST_CASE("plugin manifest: description, license and author")
{
    const manifest value = parse_ok(
        with_top(R"j("description": "Reads things.", "license": "MIT",
                 "author": { "name": "Someone" })j"));
    REQUIRE_EQ(value.description, "Reads things.");
    REQUIRE_EQ(value.license, "MIT");
    REQUIRE_EQ(value.author.name, "Someone");
    REQUIRE(value.author.url.empty());

    expect_invalid(with_top("\"description\": \"" +
                            std::string(max_description_length + 1, 'a') +
                            "\""),
                   "description: is longer than 1000 bytes");
    expect_invalid(with_top("\"description\": \"bad\\u202e\""),
                   "description: has a control character");
    expect_invalid(with_top("\"license\": \"" +
                            std::string(max_name_length + 1, 'a') + "\""),
                   "license: is longer than 100 bytes");
    expect_invalid(with_top("\"license\": 7"), "license: must be a string");

    expect_invalid(with_top("\"author\": \"Someone\""),
                   "author: must be an object");
    expect_invalid(with_top("\"author\": { }"), "author.name: is required");
    expect_invalid(with_top(R"j("author": { "name": "A", "mail": "x" })j"),
                   "author: unknown key \"mail\"");
}

TEST_CASE("plugin manifest: the author's address")
{
    const manifest value = parse_ok(with_top(
        R"j("author": { "name": "A", "url": "https://example.com/a?b=c" })j"));
    REQUIRE_EQ(value.author.url, "https://example.com/a?b=c");

    const std::vector<std::string> bad = {
        "http://example.com", "https://", "example.com",
        "https://exa mple.com", "ftp://example.com",
        "https://example.com/" + std::string(max_url_length, 'a'),
        "https://caf\\u00e9.example"};
    for (const std::string& url : bad)
    {
        expect_invalid(with_top("\"author\": { \"name\": \"A\", \"url\": \"" +
                                url + "\" }"),
                       "author.url: must be an https address", url);
    }
}

TEST_CASE("plugin manifest: dates")
{
    const manifest value = parse_ok(with_date("not_after", "2028-02-29"));
    REQUIRE(value.not_after.has_value());
    REQUIRE(*value.not_after == date(2028, 2, 29));
    REQUIRE(!value.released.has_value());

    const manifest released = parse_ok(with_date("released", "2000-01-01"));
    REQUIRE(released.released.has_value());
    REQUIRE(*released.released == date(2000, 1, 1));
    REQUIRE(!released.not_after.has_value());

    const std::vector<std::string> bad = {
        "2027/12/31", "27-12-31",  "2027-1-31", "2027-02-30", "2027-02-29",
        "2027-13-01", "2027-00-10", "2027-12-32", "2027-12-00", "1999-12-31",
        "0000-01-01", "abcd-ef-gh", "",           "2027-12-31 ", "2027-12-3a",
        "20271231",   "2027-12-311"};
    for (const std::string& text : bad)
    {
        expect_invalid(with_date("not_after", text),
                       "not_after: must be a date such as", text);
        expect_invalid(with_date("released", text),
                       "released: must be a date such as", text);
    }
    expect_invalid(with_top("\"not_after\": 20271231"),
                   "not_after: must be a string");
}

// -- requires -----------------------------------------------------------------

TEST_CASE("plugin manifest: requires and requires.api are needed")
{
    expect_invalid(replaced(minimal_text(),
                            R"j("requires": { "api": { "min": "1.0", )j"
                            R"j("max": "1.x" } },)j",
                            ""),
                   "requires: is required");
    expect_invalid(replaced(minimal_text(),
                            R"j("requires": { "api": { "min": "1.0", )j"
                            R"j("max": "1.x" } },)j",
                            "\"requires\": { },"),
                   "requires.api: is required");
    expect_invalid(replaced(minimal_text(), api_members, "\"min\": \"1.0\""),
                   "requires.api.max: is required");
    expect_invalid(replaced(minimal_text(), api_members, "\"max\": \"1.x\""),
                   "requires.api.min: is required");
    expect_invalid(replaced(minimal_text(), api_members, "\"min\": \"1.0\", "
                            "\"max\": \"1.x\", \"extra\": 1"),
                   "requires.api: unknown key \"extra\"");
}

TEST_CASE("plugin manifest: api minimum that is not a version")
{
    const std::vector<std::string> bad = {
        "1", "1.0.0", "1.x", "a.b", "01.0", "65536.0", "1.65536",
        "",  "-1.0",  "1.-1", "1.", ".1"};
    for (const std::string& text : bad)
    {
        expect_invalid(with_api(text, "1.x"),
                       "requires.api.min: must be a version such as", text);
    }
    const manifest edge = parse_ok(with_api("65535.65535", "65535.65535"));
    REQUIRE_EQ(edge.needs.api.min.major, 65535);
    REQUIRE_EQ(edge.needs.api.min.minor, 65535);
}

TEST_CASE("plugin manifest: api maximum that is not a version")
{
    const std::vector<std::string> bad = {
        "1", "x.x", "1.y", "1.4.0", "1.X", "", ".x", "1.", "65536.x"};
    for (const std::string& text : bad)
    {
        expect_invalid(with_api("1.0", text),
                       "requires.api.max: must be a version such as", text);
    }
}

TEST_CASE("plugin manifest: api maximum below the minimum")
{
    expect_invalid(with_api("1.4", "1.2"), "requires.api.max: is below min");
    expect_invalid(with_api("2.0", "1.x"), "requires.api.max: is below min");
    expect_invalid(with_api("1.0", "0.9"), "requires.api.max: is below min");
    expect_invalid(with_api("1.1", "0.x"), "requires.api.max: is below min");

    // Equal or open ends are fine.
    REQUIRE(parse_manifest(with_api("1.4", "1.4")).has_value());
    REQUIRE(parse_manifest(with_api("1.4", "1.x")).has_value());
    REQUIRE(parse_manifest(with_api("2.0", "2.x")).has_value());
    REQUIRE(parse_manifest(with_api("1.9", "2.0")).has_value());
}

TEST_CASE("plugin manifest: api maximum of the form 1.x")
{
    const manifest value = parse_ok(with_api("1.2", "7.x"));
    REQUIRE_EQ(value.needs.api.max_major, 7);
    REQUIRE(!value.needs.api.max_minor.has_value());

    const manifest closed = parse_ok(with_api("1.2", "7.3"));
    REQUIRE_EQ(closed.needs.api.max_major, 7);
    REQUIRE(closed.needs.api.max_minor.has_value());
    REQUIRE_EQ(*closed.needs.api.max_minor, 3);
}

TEST_CASE("plugin manifest: capabilities, edition and runtime")
{
    const manifest value = parse_ok(with_requires(
        R"j("capabilities": ["a", "b_1"], "edition": "enterprise",
          "runtime": ["python3", "node", "g++"])j"));
    REQUIRE(value.needs.capabilities == strings({"a", "b_1"}));
    REQUIRE_EQ(value.needs.edition, "enterprise");
    REQUIRE(value.needs.runtime == strings({"python3", "node", "g++"}));

    expect_invalid(with_requires("\"edition\": \"free\""),
                   "requires.edition: must be \"pro\" or \"enterprise\"");
    expect_invalid(with_requires("\"edition\": \"Pro\""),
                   "requires.edition: must be");
    expect_invalid(with_requires("\"edition\": 1"),
                   "requires.edition: must be a string");
    expect_invalid(with_requires("\"capabilities\": [\"Engine-Rules\"]"),
                   "requires.capabilities[0]: \"Engine-Rules\" is not the "
                   "name of a capability");
    expect_invalid(with_requires("\"capabilities\": [\"a\", \"a\"]"),
                   "requires.capabilities[1]: \"a\" is given twice");
    expect_invalid(with_requires("\"capabilities\": \"a\""),
                   "requires.capabilities: must be a list");
    expect_invalid(with_requires("\"capabilities\": [1]"),
                   "requires.capabilities[0]: must be a string");
    expect_invalid(with_requires("\"capabilities\": " + name_list("c", 33)),
                   "requires.capabilities: has more than 32 items");
    expect_invalid(with_requires("\"runtime\": [\"Python\"]"),
                   "requires.runtime[0]: \"Python\" is not the name of a "
                   "program");
    expect_invalid(with_requires("\"runtime\": " + name_list("r", 9)),
                   "requires.runtime: has more than 8 items");
    REQUIRE(parse_manifest(with_requires("\"runtime\": " + name_list("r", 8)))
                .has_value());
}

// -- run ----------------------------------------------------------------------

TEST_CASE("plugin manifest: platforms")
{
    expect_invalid(with_run("\"executable\": \"bin/fixture\""),
                   "run.platforms: is required");
    expect_invalid(with_run(R"j("executable": "bin/fixture",
                             "platforms": [])j"),
                   "run.platforms: must not be empty");
    expect_invalid(with_run(R"j("executable": "bin/fixture",
                             "platforms": ["freebsd-x64"])j"),
                   "run.platforms[0]: \"freebsd-x64\" is not a platform this "
                   "version knows");
    expect_invalid(with_run(R"j("executable": "bin/fixture",
                             "platforms": ["Linux-x64"])j"),
                   "is not a platform this version knows");
    expect_invalid(with_run(R"j("executable": "bin/fixture",
                             "platforms": ["linux-x64", "linux-x64"])j"),
                   "run.platforms[1]: \"linux-x64\" is given twice");
    expect_invalid(with_run(R"j("executable": "bin/fixture",
                             "platforms": "linux-x64")j"),
                   "run.platforms: must be a list");

    const manifest all = parse_ok(with_run(
        R"j("executable": "bin/fixture",
            "platforms": ["windows-x64", "windows-arm64", "linux-x64",
                          "linux-arm64", "macos-x64", "macos-arm64"])j"));
    REQUIRE_EQ(all.run.platforms.size(), 6);
}

TEST_CASE("plugin manifest: the executable is needed")
{
    expect_invalid(with_run(R"j("platforms": ["linux-x64"])j"),
                   "run.executable: is required");
    expect_invalid(with_run(R"j("executable": 5,
                             "platforms": ["linux-x64"])j"),
                   "run.executable: must be a string");
    expect_invalid(replaced(minimal_text(), run_members, "\"bogus\": 1"),
                   "run.executable: is required");
}

TEST_CASE("plugin manifest: executable paths that are accepted")
{
    const std::vector<std::string> good = {
        "bin/fixture", "a",          "a/b/c.d",    ".hidden/run",
        "con1",        "COM10",      "lpt",        "console.exe",
        "x y/z",       "nulx.txt",   "aux2",       "a..b",
        "Run-Me_1.exe", std::string(max_path_length, 'a')};
    for (const std::string& path : good)
    {
        const manifest value = parse_ok(with_executable(path));
        REQUIRE_EQ(value.run.executable, path);
    }
}

TEST_CASE("plugin manifest: executable paths that are refused")
{
    // Each is the text of a JSON string, so "a\\\\b" is a path with one
    // backslash.
    const std::vector<std::string> refused = {
        "../x",       "/abs",       "a\\\\b",      "C:/x",
        "a//b",       "a/./b",      "NUL",         "nul.txt",
        "a/com1.dll", "trailing.",  "x ",          "",
        "a/..",       "./x",        "sub/",        "bin/con",
        "aux.tar.gz", "LPT9",       "a/ b",        "a*b",
        "a?b",        "a<b",        "a>b",         "a|b",
        "a\\u0022b",  "caf\\u00e9", "a\\u0001b",   "nul .txt",
        "CONIN$",     "con.",       "..",          ".",
        "Prn",        "COM0",       "a/CONOUT$.x", "/",
        std::string(max_path_length + 1, 'a')};
    for (const std::string& path : refused)
    {
        expect_layout(with_executable(path), "run.executable", path);
    }
}

TEST_CASE("plugin manifest: arguments")
{
    const manifest value = parse_ok(with_run(
        R"j("executable": "bin/fixture", "args": ["-v", "--port=5"],
          "platforms": ["linux-x64"])j"));
    REQUIRE(value.run.args == strings({"-v", "--port=5"}));

    const auto with_args = [](const std::string& args) {
        return with_run("\"executable\": \"bin/fixture\", \"args\": " + args +
                        ", \"platforms\": [\"linux-x64\"]");
    };
    REQUIRE_EQ(parse_ok(with_args(name_list("a", 32))).run.args.size(), 32);
    expect_invalid(with_args(name_list("a", 33)),
                   "run.args: has more than 32 items");
    expect_invalid(with_args("[\"\"]"), "run.args[0]: must not be empty");
    expect_invalid(with_args("[\"" + std::string(max_argument_length + 1, 'a') +
                             "\"]"),
                   "run.args[0]: is longer than 256 bytes");
    REQUIRE(parse_manifest(with_args("[\"" +
                                     std::string(max_argument_length, 'a') +
                                     "\"]"))
                .has_value());
    expect_invalid(with_args("[\"ok\", \"bad\\u0001\"]"),
                   "run.args[1]: has a control character");
    expect_invalid(with_args("[5]"), "run.args[0]: must be a string");
    expect_invalid(with_args("\"-v\""), "run.args: must be a list");
}

// -- permissions --------------------------------------------------------------

TEST_CASE("plugin manifest: unknown or repeated permissions")
{
    expect_invalid(with_permission(R"j({ "name": "teleport" })j"),
                   "permissions[1].name: unknown permission \"teleport\"");
    expect_invalid(with_permission(R"j({ "name": "Ports.Read" })j"),
                   "unknown permission");
    expect_invalid(with_permission(R"j({ "name": "ports.read" })j"),
                   "permissions[1].name: \"ports.read\" is given twice");
    expect_invalid(with_permission(R"j({ "required": true })j"),
                   "permissions[1].name: is required");
    expect_invalid(with_permission("7"), "permissions[1]: must be an object");
    expect_invalid(with_permission(R"j({ "name": 7 })j"),
                   "permissions[1].name: must be a string");
}

TEST_CASE("plugin manifest: the scope of a permission")
{
    expect_invalid(with_permission(R"j({ "name": "faults", "scope": ["x"] })j"),
                   "permissions[1].scope: is only for net.listen and "
                   "net.outbound");
    expect_invalid(with_permission(R"j({ "name": "net.listen" })j"),
                   "permissions[1].scope: is required");
    expect_invalid(with_permission(R"j({ "name": "net.outbound" })j"),
                   "permissions[1].scope: is required");
    expect_invalid(with_permission(
                       R"j({ "name": "net.listen", "scope": [] })j"),
                   "permissions[1].scope: must not be empty");
    expect_invalid(with_permission(
                       R"j({ "name": "net.listen", "scope": ["a b"] })j"),
                   "permissions[1].scope[0]: \"a b\" is not an address");
    expect_invalid(with_permission(
                       R"j({ "name": "net.listen", "scope": [""] })j"),
                   "permissions[1].scope[0]: \"\" is not an address");
    expect_invalid(with_permission(
                       R"j({ "name": "net.listen", "scope": ["x", "x"] })j"),
                   "permissions[1].scope[1]: \"x\" is given twice");
    expect_invalid(with_permission(
                       R"j({ "name": "net.listen", "scope": "x" })j"),
                   "permissions[1].scope: must be a list");
    const std::string long_address(max_scope_length + 1, 'a');
    expect_invalid(with_permission("{ \"name\": \"net.listen\", "
                                   "\"scope\": [\"" + long_address + "\"] }"),
                   "is not an address");
    expect_invalid(with_permission("{ \"name\": \"net.listen\", \"scope\": " +
                                   name_list("h", 17) + " }"),
                   "permissions[1].scope: has more than 16 items");

    const manifest value = parse_ok(with_permission(
        R"j({ "name": "net.outbound", "scope": ["api.example.com:443"] })j"));
    const permission_request* const request =
        value.find(permission::net_outbound);
    REQUIRE(request != nullptr);
    REQUIRE(request->required);
    REQUIRE(request->scope == strings({"api.example.com:443"}));
}

TEST_CASE("plugin manifest: required and reason of a permission")
{
    expect_invalid(replaced(minimal_text(), "\"required\": false",
                            "\"required\": \"no\""),
                   "permissions[1].required: must be true or false");
    expect_invalid(replaced(minimal_text(), "\"required\": false",
                            "\"required\": 0"),
                   "permissions[1].required: must be true or false");
    expect_invalid(replaced(minimal_text(), "\"Needs to see its port.\"",
                            "\"" + std::string(max_reason_length + 1, 'a') +
                                "\""),
                   "permissions[0].reason: is longer than 200 bytes");
    expect_invalid(replaced(minimal_text(), "\"Needs to see its port.\"",
                            "\"\""),
                   "permissions[0].reason: must not be empty");
    expect_invalid(replaced(minimal_text(), "\"Needs to see its port.\"",
                            "\"Bad\\u202etext\""),
                   "permissions[0].reason: has a control character");

    const std::string many = repeat(R"j({ "name": "ports.read" }, )j", 32);
    expect_invalid(with_permission(many + std::string(faults_json)),
                   "permissions: has more than 32 items");
}

TEST_CASE("plugin manifest: every permission is known by its name")
{
    // The first permission of the minimal manifest is ports.read, so that
    // one is left out: it would be given twice.
    for (const permission name :
         {permission::traffic_tap, permission::ports_write,
          permission::topology_modify, permission::modifier_inline,
          permission::faults, permission::serial_devices,
          permission::fs_read, permission::fs_write,
          permission::data_private, permission::process_spawn,
          permission::bundle_section})
    {
        const std::string member =
            "{ \"name\": \"" + std::string(to_string(name)) + "\" }";
        const manifest value = parse_ok(with_permission(member));
        const permission_request* const request = value.find(name);
        REQUIRE(request != nullptr);
        REQUIRE(request->required);
    }
}

// -- extension points ---------------------------------------------------------

TEST_CASE("plugin manifest: extension point kind, id and name")
{
    expect_invalid(replaced(minimal_text(), "\"kind\": \"fixture\"",
                            "\"kind\": \"widget\""),
                   at_point(".kind: unknown kind \"widget\""));
    expect_invalid(replaced(minimal_text(), "\"kind\": \"fixture\"",
                            "\"kind\": \"Fixture\""),
                   at_point(".kind: unknown kind"));
    expect_invalid(replaced(minimal_text(), "\"kind\": \"fixture\",", ""),
                   at_point(".kind: is required"));
    expect_invalid(replaced(minimal_text(), "\"id\": \"slave\",", ""),
                   at_point(".id: is required"));
    expect_invalid(replaced(minimal_text(), "\"name\": \"Slave\",", ""),
                   at_point(".name: is required"));
    expect_invalid(replaced(minimal_text(), "\"extension_points\": [",
                            "\"extension_points\": [ 5, "),
                   "extension_points[0]: must be an object");
}

TEST_CASE("plugin manifest: extension point ids")
{
    const std::vector<std::string> bad = {
        "Slave", "1x", "a b", "", "_x", "-x", "x.y", std::string(65, 'a')};
    for (const std::string& id : bad)
    {
        expect_invalid(replaced(minimal_text(), "\"id\": \"slave\"",
                                "\"id\": \"" + id + "\""),
                       at_point(".id: "), id);
    }
    const std::vector<std::string> good = {
        "a", "x-1_2", "slave", std::string(64, 'a')};
    for (const std::string& id : good)
    {
        const manifest value = parse_ok(replaced(
            minimal_text(), "\"id\": \"slave\"", "\"id\": \"" + id + "\""));
        REQUIRE_EQ(value.extension_points[0].id, id);
    }
}

TEST_CASE("plugin manifest: an extension point id given twice")
{
    expect_invalid(with_first_point(
                       R"j({ "kind": "generator", "id": "slave",
                           "name": "Other" })j"),
                   "extension_points[1].id: \"slave\" is given twice");

    // Another id is fine.
    const std::string other = with_first_point(
        R"j({ "kind": "generator", "id": "other", "name": "Other" })j");
    REQUIRE(parse_manifest(other).has_value());
}

TEST_CASE("plugin manifest: too many extension points")
{
    const std::string points = "[" + repeat("{}, ", 32) + "{}]";
    expect_invalid(bare_text(", \"extension_points\": " + points),
                   "extension_points: has more than 32 items");
    expect_invalid(bare_text(", \"extension_points\": 5"),
                   "extension_points: must be a list");
}

TEST_CASE("plugin manifest: a plugin must add something")
{
    expect_invalid(bare_text(""),
                   "extension_points: the plugin must add at least one "
                   "thing");
    expect_invalid(bare_text(", \"extension_points\": []"),
                   "the plugin must add at least one thing");
    expect_invalid(bare_text(", \"contributes\": {}"),
                   "the plugin must add at least one thing");
    expect_invalid(bare_text(", \"contributes\": { \"bundle_sections\": [] }"),
                   "the plugin must add at least one thing");
}

TEST_CASE("plugin manifest: which timing classes a kind may have")
{
    expect_invalid(point_text("modifier", ""),
                   at_point(".timing: is required for a modifier"));
    expect_invalid(point_text("modifier", R"j({ "class": "rules" })j"),
                   at_point(".timing.class: \"rules\" is not allowed for "
                            "the kind modifier"));
    expect_invalid(point_text("generator",
                              R"j({ "class": "inline", "budget_us": 100 })j"),
                   at_point(".timing.class: \"inline\" is not allowed for "
                            "the kind generator"));
    expect_invalid(point_text("fixture",
                              R"j({ "class": "inline", "budget_us": 100 })j"),
                   at_point(".timing.class: \"inline\" is not allowed for "
                            "the kind fixture"));
    expect_invalid(point_text("adapter", R"j({ "class": "rules" })j"),
                   at_point(".timing.class: \"rules\" is not allowed for"));
    expect_invalid(point_text("adapter",
                              R"j({ "class": "inline", "budget_us": 100 })j"),
                   at_point(".timing.class: \"inline\" is not allowed for"));

    // What is allowed.
    const std::string rules = R"j({ "class": "rules" })j";
    const std::string async = R"j({ "class": "async" })j";
    REQUIRE(parse_manifest(point_text("generator", rules)).has_value());
    REQUIRE(parse_manifest(point_text("generator", async)).has_value());
    REQUIRE(parse_manifest(point_text("fixture", rules)).has_value());
    REQUIRE(parse_manifest(point_text("fixture", async)).has_value());
    REQUIRE(parse_manifest(point_text("adapter", async)).has_value());
    REQUIRE(parse_manifest(point_text("modifier", async)).has_value());
    REQUIRE(parse_manifest(inline_text(
                               R"j({ "class": "inline", "budget_us": 100 })j"))
                .has_value());
}

TEST_CASE("plugin manifest: the timing object")
{
    expect_invalid(point_text("fixture", R"j({ "class": "fast" })j"),
                   at_point(".timing.class: unknown value \"fast\""));
    expect_invalid(point_text("fixture", R"j({ "class": 1 })j"),
                   at_point(".timing.class: must be a string"));
    expect_invalid(point_text("fixture", "{}"),
                   at_point(".timing.class: is required"));
    expect_invalid(point_text("fixture", "\"rules\""),
                   at_point(".timing: must be an object"));
    expect_invalid(point_text("fixture", R"j({ "class": "rules",
                                            "budget_us": 100 })j"),
                   at_point(".timing.budget_us: is only for the class "
                            "\"inline\""));
    expect_invalid(point_text("modifier", R"j({ "class": "async",
                                             "budget_us": 100 })j"),
                   at_point(".timing.budget_us: is only for the class "
                            "\"inline\""));
    expect_invalid(point_text("modifier", R"j({ "class": "async",
                                             "on_timeout": "fail_open" })j"),
                   at_point(".timing.on_timeout: is only for the class "
                            "\"inline\""));
}

TEST_CASE("plugin manifest: the budget of an inline extension point")
{
    expect_invalid(inline_text(R"j({ "class": "inline" })j"),
                   at_point(".timing.budget_us: is required"));
    for (const char* const budget : {"0", "49", "1000001", "99999999"})
    {
        expect_invalid(inline_text(std::string(R"j({ "class": "inline", )j") +
                                   "\"budget_us\": " + budget + " }"),
                       at_point(".timing.budget_us: must be from 50 to "
                                "1000000"),
                       budget);
    }
    for (const char* const budget : {"-5", "1.5", "\"100\"", "null"})
    {
        expect_invalid(inline_text(std::string(R"j({ "class": "inline", )j") +
                                   "\"budget_us\": " + budget + " }"),
                       at_point(".timing.budget_us: must be a whole number "
                                "that is not negative"),
                       budget);
    }

    const manifest lowest = parse_ok(inline_text(
        R"j({ "class": "inline", "budget_us": 50 })j"));
    REQUIRE_EQ(lowest.extension_points[0].timing.budget_us, min_budget_us);
    REQUIRE(lowest.extension_points[0].timing.type == timing_class::in_line);
    const manifest highest = parse_ok(inline_text(
        R"j({ "class": "inline", "budget_us": 1000000 })j"));
    REQUIRE_EQ(highest.extension_points[0].timing.budget_us, max_budget_us);
}

TEST_CASE("plugin manifest: what happens when the budget passes")
{
    const manifest implicit_open = parse_ok(inline_text(
        R"j({ "class": "inline", "budget_us": 100 })j"));
    REQUIRE(implicit_open.extension_points[0].timing.on_timeout ==
            timeout_action::fail_open);

    const manifest explicit_open = parse_ok(inline_text(
        R"j({ "class": "inline", "budget_us": 100,
            "on_timeout": "fail_open" })j"));
    REQUIRE(explicit_open.extension_points[0].timing.on_timeout ==
            timeout_action::fail_open);

    const manifest closed = parse_ok(inline_text(
        R"j({ "class": "inline", "budget_us": 100,
            "on_timeout": "fail_closed" })j"));
    REQUIRE(closed.extension_points[0].timing.on_timeout ==
            timeout_action::fail_closed);

    expect_invalid(inline_text(R"j({ "class": "inline", "budget_us": 100,
                                    "on_timeout": "fail_sideways" })j"),
                   at_point(".timing.on_timeout: must be \"fail_open\" or "
                            "\"fail_closed\""));
}

TEST_CASE("plugin manifest: inline needs the permission modifier.inline")
{
    const std::string timing = R"j({ "class": "inline", "budget_us": 100 })j";
    const std::string wanted = at_point(
        ".timing.class: needs the permission \"modifier.inline\", as "
        "required");

    // Not asked for at all.
    expect_invalid(point_text("modifier", timing), wanted);
    // Asked for, but not as a requirement.
    expect_invalid(with_inline_permission(point_text("modifier", timing),
                                          ", \"required\": false"),
                   wanted);
    // Another permission is not enough.
    expect_invalid(replaced(point_text("modifier", timing), faults_json,
                            R"j({ "name": "traffic.tap" })j"),
                   wanted);

    REQUIRE(parse_manifest(inline_text(timing)).has_value());
}

TEST_CASE("plugin manifest: the number of instances")
{
    for (const char* const count : {"0", "257", "1000", "-1"})
    {
        expect_invalid(with_instances(count), at_point(".max_instances: "),
                       count);
    }
    expect_invalid(with_instances("0"),
                   at_point(".max_instances: must be from 1 to 256"));
    expect_invalid(with_instances("257"),
                   at_point(".max_instances: must be from 1 to 256"));
    expect_invalid(with_instances("\"4\""),
                   at_point(".max_instances: must be a whole number that is "
                            "not negative"));
    expect_invalid(with_instances("4.5"),
                   at_point(".max_instances: must be a whole number"));

    const manifest one = parse_ok(with_instances("1"));
    REQUIRE_EQ(one.extension_points[0].max_instances, 1);
    const manifest most = parse_ok(with_instances("256"));
    REQUIRE_EQ(most.extension_points[0].max_instances, 256);
    REQUIRE_EQ(most.extension_points[0].max_instances, max_instances);
}

TEST_CASE("plugin manifest: description of an extension point")
{
    const manifest value = parse_ok(replaced(
        minimal_text(), "\"name\": \"Slave\",",
        R"j("name": "Slave", "description": { "en": "A slave.",
                                              "de": "Ein Knecht." },)j"));
    const extension_point& point = value.extension_points[0];
    REQUIRE_EQ(point.description.best("de"), "Ein Knecht.");
    REQUIRE_EQ(point.description.best("en-AU"), "A slave.");

    expect_invalid(replaced(minimal_text(), "\"name\": \"Slave\",",
                            "\"name\": \"Slave\", \"description\": \"" +
                                std::string(max_description_length + 1, 'a') +
                                "\","),
                   at_point(".description: is longer than 1000 bytes"));
}

// -- parameters ---------------------------------------------------------------

TEST_CASE("plugin manifest: parameter type, id and label")
{
    expect_invalid(param_text("\"type\": \"float\""),
                   at_param(".type: unknown type \"float\""));
    expect_invalid(param_text("\"type\": \"Int\""),
                   at_param(".type: unknown type"));
    expect_invalid(param_text("\"type\": 5"),
                   at_param(".type: must be a string"));
    expect_invalid(fixture_text(R"j({ "id": "p", "label": "P" })j"),
                   at_param(".type: is required"));
    expect_invalid(fixture_text(R"j({ "id": "p", "type": "bool" })j"),
                   at_param(".label: is required"));
    expect_invalid(fixture_text(R"j({ "label": "P", "type": "bool" })j"),
                   at_param(".id: is required"));
    expect_invalid(fixture_text(
                       R"j({ "id": "P", "label": "P", "type": "bool" })j"),
                   at_param(".id: \"P\" is not an id"));
    expect_invalid(fixture_text("5"), at_param(": must be an object"));
    expect_invalid(fixture_text(
                       R"j({ "id": "p", "label": "P", "type": "bool" },
                          { "id": "p", "label": "Q", "type": "bool" })j"),
                   "extension_points[0].parameters[1].id: \"p\" is given "
                   "twice");
    expect_invalid(fixture_text(repeat("{}, ", 64) + "{}"),
                   "extension_points[0].parameters: has more than 64 items");
}

TEST_CASE("plugin manifest: labels and help in several languages")
{
    const std::vector<std::string> good = {
        "en", "de", "en-AU", "zh-Hant", "es-419", "fil"};
    for (const std::string& tag : good)
    {
        const manifest value = parse_ok(fixture_text(
            "{ \"id\": \"p\", \"type\": \"bool\", \"label\": { \"" + tag +
            "\": \"x\" } }"));
        const parameter& item = value.extension_points[0].parameters[0];
        REQUIRE_EQ(item.label.entries.size(), 1);
        REQUIRE_EQ(item.label.best(tag), "x");
    }

    const std::vector<std::string> bad = {
        "e", "english", "en_AU", "e1", "en-", "en--AU", "EN us", "", "-en"};
    for (const std::string& tag : bad)
    {
        expect_invalid(fixture_text(
                           "{ \"id\": \"p\", \"type\": \"bool\", \"label\": "
                           "{ \"" + tag + "\": \"x\" } }"),
                       at_param(".label: \"" + tag +
                                "\" is not a language tag"),
                       tag);
    }
    expect_invalid(fixture_text(
                       R"j({ "id": "p", "type": "bool", "label": {} })j"),
                   at_param(".label: must not be empty"));
    expect_invalid(fixture_text(
                       R"j({ "id": "p", "type": "bool", "label": 5 })j"),
                   at_param(".label: must be a string, or an object"));
    expect_invalid(fixture_text(
                       R"j({ "id": "p", "type": "bool",
                           "label": { "en": 5 } })j"),
                   at_param(".label.en: must be a string"));
    expect_invalid(param_text("\"type\": \"bool\", \"help\": \"" +
                              std::string(max_help_length + 1, 'a') + "\""),
                   at_param(".help: is longer than 500 bytes"));
    REQUIRE(parse_manifest(param_text("\"type\": \"bool\", \"help\": \"" +
                                      std::string(max_help_length, 'a') +
                                      "\""))
                .has_value());
}

TEST_CASE("plugin manifest: boolean parameters")
{
    const manifest value = parse_ok(param_text("\"type\": \"bool\""));
    const parameter& item = value.extension_points[0].parameters[0];
    REQUIRE(std::holds_alternative<std::monostate>(item.default_value));

    expect_invalid(param_text("\"type\": \"bool\", \"default\": \"yes\""),
                   at_param(".default: must be true or false"));
    expect_invalid(param_text("\"type\": \"bool\", \"default\": 1"),
                   at_param(".default: must be true or false"));
    expect_invalid(param_text("\"type\": \"bool\", \"unit\": \"ms\""),
                   at_param(": unknown key \"unit\""));
    expect_invalid(param_text("\"type\": \"bool\", \"min\": 0"),
                   at_param(": unknown key \"min\""));
}

TEST_CASE("plugin manifest: integer parameters")
{
    const std::string head = "\"type\": \"int\", ";
    expect_invalid(param_text(head + "\"min\": 10, \"max\": 20, "
                                     "\"default\": 5"),
                   at_param(".default: is below min"));
    expect_invalid(param_text(head + "\"min\": 10, \"max\": 20, "
                                     "\"default\": 25"),
                   at_param(".default: is above max"));
    expect_invalid(param_text(head + "\"min\": 20, \"max\": 10"),
                   at_param(".min: is above max"));
    expect_invalid(param_text(head + "\"step\": 0"),
                   at_param(".step: must be above zero"));
    expect_invalid(param_text(head + "\"step\": -3"),
                   at_param(".step: must be above zero"));
    expect_invalid(param_text(head + "\"default\": 1.5"),
                   at_param(".default: must be a whole number"));
    expect_invalid(param_text(head + "\"default\": \"5\""),
                   at_param(".default: must be a whole number"));
    expect_invalid(param_text(head + "\"min\": 1e3"),
                   at_param(".min: must be a whole number"));
    expect_invalid(param_text(head + "\"max\": 9007199254740993"),
                   at_param(".max: is too large"));
    expect_invalid(param_text(head + "\"min\": -9007199254740993"),
                   at_param(".min: is too large"));
    expect_invalid(param_text(head + "\"max\": 18446744073709551615"),
                   at_param(".max: must be a whole number"));
    expect_invalid(param_text(head + "\"unit\": \"" + std::string(17, 'u') +
                              "\""),
                   at_param(".unit: is longer than 16 bytes"));
    expect_invalid(param_text(head + "\"max_length\": 5"),
                   at_param(": unknown key \"max_length\""));

    // The bounds are inclusive, and the extremes are held exactly.
    const manifest edge = parse_ok(
        param_text(head + "\"min\": 5, \"max\": 5, \"default\": 5"));
    const parameter& same = edge.extension_points[0].parameters[0];
    REQUIRE_EQ(std::get<std::int64_t>(same.min), 5);
    REQUIRE_EQ(std::get<std::int64_t>(same.default_value), 5);

    const manifest wide = parse_ok(
        param_text(head + "\"min\": -9007199254740992, "
                          "\"max\": 9007199254740992, \"unit\": \"ms\""));
    const parameter& range = wide.extension_points[0].parameters[0];
    REQUIRE_EQ(std::get<std::int64_t>(range.min), -9'007'199'254'740'992);
    REQUIRE_EQ(std::get<std::int64_t>(range.max), 9'007'199'254'740'992);
    REQUIRE_EQ(range.unit, "ms");
}

TEST_CASE("plugin manifest: number parameters")
{
    const std::string head = "\"type\": \"number\", ";
    expect_invalid(param_text(head + "\"default\": \"1.5\""),
                   at_param(".default: must be a number"));
    expect_invalid(param_text(head + "\"min\": 2.5, \"max\": 1.5"),
                   at_param(".min: is above max"));
    expect_invalid(param_text(head + "\"min\": 1.0, \"default\": 0.5"),
                   at_param(".default: is below min"));
    expect_invalid(param_text(head + "\"max\": 1.0, \"default\": 1.5"),
                   at_param(".default: is above max"));
    expect_invalid(param_text(head + "\"step\": 0.0"),
                   at_param(".step: must be above zero"));
    expect_invalid(param_text(head + "\"step\": -1.5"),
                   at_param(".step: must be above zero"));
    expect_invalid(param_text(head + "\"max_length\": 3"),
                   at_param(": unknown key \"max_length\""));

    // A whole number is a number, and is held as a double.
    const manifest value = parse_ok(
        param_text(head + "\"min\": 0, \"max\": 10, \"default\": 2, "
                          "\"step\": 1, \"unit\": \"V\""));
    const parameter& item = value.extension_points[0].parameters[0];
    REQUIRE_NEAR(std::get<double>(item.default_value), 2.0, 1e-12);
    REQUIRE_NEAR(std::get<double>(item.min), 0.0, 1e-12);
    REQUIRE_NEAR(std::get<double>(item.max), 10.0, 1e-12);
    REQUIRE_NEAR(std::get<double>(item.step), 1.0, 1e-12);
    REQUIRE_EQ(item.unit, "V");
}

TEST_CASE("plugin manifest: enum parameters")
{
    const std::string options =
        R"j("options": [ { "value": "a", "label": "A" }, )j"
        R"j({ "value": "b", "label": "B" } ])j";
    const std::string head = "\"type\": \"enum\", ";

    expect_invalid(param_text(head.substr(0, head.size() - 2)),
                   at_param(".options: is required"));
    expect_invalid(param_text(head + "\"options\": []"),
                   at_param(".options: must not be empty"));
    expect_invalid(param_text(head + "\"options\": 5"),
                   at_param(".options: must be a list"));
    expect_invalid(param_text(head + "\"options\": [ 5 ]"),
                   at_param(".options[0]: must be an object"));
    expect_invalid(param_text(head + R"j("options": [ { "value": "a" } ])j"),
                   at_param(".options[0].label: is required"));
    expect_invalid(param_text(head + R"j("options": [ { "label": "A" } ])j"),
                   at_param(".options[0].value: is required"));
    expect_invalid(param_text(head + R"j("options": [ { "value": "a",
                                       "label": "A", "extra": 1 } ])j"),
                   at_param(".options[0]: unknown key \"extra\""));
    expect_invalid(param_text(head + options + ", \"default\": \"c\""),
                   at_param(".default: is not one of the options"));
    expect_invalid(param_text(head + options + ", \"default\": 1"),
                   at_param(".default: must be a string"));
    expect_invalid(param_text(head + R"j("options": [
                                { "value": "a", "label": "A" },
                                { "value": "a", "label": "B" } ])j"),
                   at_param(".options[1].value: \"a\" is given twice"));

    std::string many = "[";
    for (int index = 0; index < 65; ++index)
    {
        if (index != 0)
            many += ", ";
        many += "{ \"value\": \"v" + std::to_string(index) +
                "\", \"label\": \"L\" }";
    }
    many += ']';
    expect_invalid(param_text(head + "\"options\": " + many),
                   at_param(".options: has more than 64 items"));

    const manifest value =
        parse_ok(param_text(head + options + ", \"default\": \"b\""));
    const parameter& item = value.extension_points[0].parameters[0];
    REQUIRE_EQ(item.choices.size(), 2);
    REQUIRE_EQ(item.choices[0].value, "a");
    REQUIRE_EQ(item.choices[1].value, "b");
    REQUIRE_EQ(item.choices[1].label.best("en"), "B");
    REQUIRE_EQ(std::get<std::string>(item.default_value), "b");

    // Without a default there is none held.
    const manifest bare = parse_ok(param_text(head + options));
    REQUIRE(std::holds_alternative<std::monostate>(
        bare.extension_points[0].parameters[0].default_value));
}

TEST_CASE("plugin manifest: string parameters")
{
    const std::string head = "\"type\": \"string\", ";
    expect_invalid(param_text(head + "\"max_length\": 3, \"default\": "
                                     "\"abcd\""),
                   at_param(".default: is longer than 3 bytes"));
    expect_invalid(param_text(head + "\"default\": \"" +
                              std::string(257, 'a') + "\""),
                   at_param(".default: is longer than 256 bytes"));
    expect_invalid(param_text(head + "\"max_length\": 0"),
                   at_param(".max_length: must be from 1 to 4096"));
    expect_invalid(param_text(head + "\"max_length\": 4097"),
                   at_param(".max_length: must be from 1 to 4096"));
    expect_invalid(param_text(head + "\"max_length\": \"8\""),
                   at_param(".max_length: must be a whole number"));
    expect_invalid(param_text(head + "\"default\": 5"),
                   at_param(".default: must be a string"));
    expect_invalid(param_text(head + "\"default\": \"a\\u0001\""),
                   at_param(".default: has a control character"));

    const manifest implicit = parse_ok(param_text(head + "\"default\": \"x\""));
    REQUIRE_EQ(implicit.extension_points[0].parameters[0].max_length, 256);
    const manifest longest = parse_ok(
        param_text(head + "\"max_length\": 4096, \"default\": \"abc\""));
    REQUIRE_EQ(longest.extension_points[0].parameters[0].max_length, 4096);
    const manifest exact = parse_ok(
        param_text(head + "\"max_length\": 3, \"default\": \"abc\""));
    REQUIRE_EQ(std::get<std::string>(
                   exact.extension_points[0].parameters[0].default_value),
               "abc");
}

TEST_CASE("plugin manifest: bytes parameters")
{
    const std::string head = "\"type\": \"bytes\", ";
    const std::vector<std::string> bad = {
        "023", "zz", "02  31", "02 ", " 02", "0x02", "02-31", "2", "02 3",
        "02 31 0"};
    for (const std::string& text : bad)
    {
        std::string member = head;
        member += "\"default\": \"";
        member += text;
        member += '"';
        expect_invalid(param_text(member),
                       at_param(".default: must be hexadecimal bytes"), text);
    }
    expect_invalid(param_text(head + "\"max_length\": 2, "
                                     "\"default\": \"01 02 03\""),
                   at_param(".default: is longer than max_length"));
    expect_invalid(param_text(head + "\"default\": 2"),
                   at_param(".default: must be a string"));
    expect_invalid(param_text(head + "\"max_length\": 0"),
                   at_param(".max_length: must be from 1 to 4096"));

    const auto bytes_of = [&](const std::string& text) {
        const manifest value =
            parse_ok(param_text(head + "\"default\": \"" + text + "\""));
        return std::get<std::string>(
            value.extension_points[0].parameters[0].default_value);
    };
    const std::string expected{'\x02', '1', '\x03'};
    REQUIRE_EQ(bytes_of("02 31 03"), expected);
    REQUIRE_EQ(bytes_of("023103"), expected);
    REQUIRE_EQ(bytes_of("0231 03"), expected);
    const std::string mixed{'\x0a', static_cast<char>(0xff)};
    REQUIRE_EQ(bytes_of("0aFF"), mixed);
    REQUIRE_EQ(bytes_of("0A ff"), mixed);

    const manifest exact = parse_ok(
        param_text(head + "\"max_length\": 3, \"default\": \"01 02 03\""));
    REQUIRE_EQ(exact.extension_points[0].parameters[0].max_length, 3);
}

TEST_CASE("plugin manifest: duration parameters")
{
    const std::string head = "\"type\": \"duration\", ";
    const std::vector<std::string> bad = {
        "5m", "5", "ms", "-5ms", "1.5s", "5 ms", "5MS", "", "5h", "86401s",
        "86400001ms", "86400000001us", "86400000000001ns",
        "1000000000000000000000s"};
    for (const std::string& text : bad)
    {
        std::string member = head;
        member += "\"default\": \"";
        member += text;
        member += '"';
        expect_invalid(param_text(member),
                       at_param(".default: must be a whole number and a unit"),
                       text);
    }
    expect_invalid(param_text(head + "\"default\": 5"),
                   at_param(".default: must be a string"));
    expect_invalid(param_text(head + "\"min\": \"10ms\", \"max\": \"5ms\""),
                   at_param(".min: is above max"));
    expect_invalid(param_text(head + "\"min\": \"10ms\", \"default\": \"5ms\""),
                   at_param(".default: is below min"));
    expect_invalid(param_text(head + "\"max\": \"5ms\", \"default\": \"10ms\""),
                   at_param(".default: is above max"));
    expect_invalid(param_text(head + "\"unit\": \"ms\""),
                   at_param(": unknown key \"unit\""));

    const auto nanoseconds = [&](const std::string& text) {
        const manifest value =
            parse_ok(param_text(head + "\"default\": \"" + text + "\""));
        return std::get<std::int64_t>(
            value.extension_points[0].parameters[0].default_value);
    };
    REQUIRE_EQ(nanoseconds("0ms"), 0);
    REQUIRE_EQ(nanoseconds("1ns"), 1);
    REQUIRE_EQ(nanoseconds("250us"), 250'000);
    REQUIRE_EQ(nanoseconds("3ms"), 3'000'000);
    REQUIRE_EQ(nanoseconds("2s"), 2'000'000'000);
    REQUIRE_EQ(nanoseconds("86400s"), 86'400'000'000'000);
    REQUIRE_EQ(nanoseconds("86400000ms"), 86'400'000'000'000);
    REQUIRE_EQ(nanoseconds("86400000000us"), 86'400'000'000'000);
    REQUIRE_EQ(nanoseconds("86400000000000ns"), 86'400'000'000'000);
}

TEST_CASE("plugin manifest: path parameters")
{
    const std::string head = "\"type\": \"path\", ";
    const manifest plain =
        parse_ok(param_text(head.substr(0, head.size() - 2)));
    const parameter& item = plain.extension_points[0].parameters[0];
    REQUIRE(item.kind == path_kind::file);
    REQUIRE(item.access == path_access::read);
    REQUIRE(!item.must_exist);

    const manifest named = parse_ok(param_text(
        head + "\"kind\": \"file\", \"access\": \"read\", "
               "\"must_exist\": false"));
    REQUIRE(named.extension_points[0].parameters[0].kind == path_kind::file);

    expect_invalid(param_text(head + "\"kind\": \"folder\""),
                   at_param(".kind: must be \"file\" or \"directory\""));
    expect_invalid(param_text(head + "\"kind\": 5"),
                   at_param(".kind: must be a string"));
    expect_invalid(param_text(head + "\"access\": \"append\""),
                   at_param(".access: must be \"read\" or \"write\""));
    expect_invalid(param_text(head + "\"must_exist\": \"yes\""),
                   at_param(".must_exist: must be true or false"));
    expect_invalid(param_text(head + "\"default\": \"/tmp\""),
                   at_param(": unknown key \"default\""));
}

TEST_CASE("plugin manifest: port, link and secret parameters")
{
    for (const char* const type : {"port", "link"})
    {
        const std::string head = std::string("\"type\": \"") + type + "\"";
        const manifest value = parse_ok(param_text(head));
        const parameter& item = value.extension_points[0].parameters[0];
        REQUIRE(std::holds_alternative<std::monostate>(item.default_value));
        expect_invalid(param_text(head + ", \"default\": \"x\""),
                       at_param(": unknown key \"default\""), type);
        expect_invalid(param_text(head + ", \"max_length\": 5"),
                       at_param(": unknown key \"max_length\""), type);
    }

    // A manifest is no place for a secret.
    expect_invalid(param_text("\"type\": \"secret\", \"default\": \"x\""),
                   at_param(": unknown key \"default\""));
    expect_invalid(param_text("\"type\": \"secret\", \"max_length\": 0"),
                   at_param(".max_length: must be from 1 to 4096"));

    const manifest secret = parse_ok(param_text("\"type\": \"secret\""));
    const parameter& item = secret.extension_points[0].parameters[0];
    REQUIRE(item.type == parameter_type::secret);
    REQUIRE_EQ(item.max_length, 256);
}

// -- bundle sections ----------------------------------------------------------

TEST_CASE("plugin manifest: a plugin that only adds a bundle section")
{
    const manifest value = parse_ok(
        sections_text(R"j({ "id": "state", "title": "State" })j"));
    REQUIRE(value.extension_points.empty());
    REQUIRE_EQ(value.bundle_sections.size(), 1);
    const bundle_section& section = value.bundle_sections[0];
    REQUIRE_EQ(section.id, "state");
    REQUIRE_EQ(section.title.best("en"), "State");
    REQUIRE(section.sensitive);
    REQUIRE(section.parameters.empty());
}

TEST_CASE("plugin manifest: bundle sections need their permission")
{
    const std::string section =
        R"j("contributes": { "bundle_sections": [ { "id": "state",
            "title": "State" } ] })j";
    expect_invalid(with_top(section),
                   "contributes.bundle_sections: needs the permission "
                   "\"bundle.section\"");

    // With the permission, in any form, it is read.
    const manifest value = parse_ok(replaced(
        with_top(section), faults_json,
        R"j({ "name": "bundle.section", "required": false })j"));
    REQUIRE_EQ(value.bundle_sections.size(), 1);
    REQUIRE_EQ(value.extension_points.size(), 1);
}

TEST_CASE("plugin manifest: bundle section ids, titles and limits")
{
    expect_invalid(sections_text(R"j({ "id": "s", "title": "A" },
                                    { "id": "s", "title": "B" })j"),
                   "contributes.bundle_sections[1].id: \"s\" is given twice");
    expect_invalid(sections_text(R"j({ "id": "s" })j"),
                   "contributes.bundle_sections[0].title: is required");
    expect_invalid(sections_text(R"j({ "title": "A" })j"),
                   "contributes.bundle_sections[0].id: is required");
    expect_invalid(sections_text(R"j({ "id": "S", "title": "A" })j"),
                   "contributes.bundle_sections[0].id: \"S\" is not an id");
    expect_invalid(sections_text(
                       R"j({ "id": "s", "title": "A", "sensitive": 1 })j"),
                   "contributes.bundle_sections[0].sensitive: must be true or "
                   "false");
    expect_invalid(sections_text(
                       R"j({ "id": "s", "title": "A", "extra": 1 })j"),
                   "contributes.bundle_sections[0]: unknown key \"extra\"");
    expect_invalid(sections_text(repeat("{}, ", 8) + "{}"),
                   "contributes.bundle_sections: has more than 8 items");
    expect_invalid(bare_text(", \"contributes\": { \"parts\": [] }"),
                   "contributes: unknown key \"parts\"");
    expect_invalid(bare_text(", \"contributes\": 5"),
                   "contributes: must be an object");

    const manifest value = parse_ok(sections_text(
        R"j({ "id": "s", "title": "A", "sensitive": false,
            "parameters": [ { "id": "p", "type": "bool",
                              "label": "P" } ] })j"));
    REQUIRE(!value.bundle_sections[0].sensitive);
    REQUIRE_EQ(value.bundle_sections[0].parameters.size(), 1);

    expect_invalid(sections_text(
                       R"j({ "id": "s", "title": "A", "parameters": [
                            { "id": "p", "type": "nope",
                              "label": "P" } ] })j"),
                   "contributes.bundle_sections[0].parameters[0].type: "
                   "unknown type \"nope\"");
}

// -- files --------------------------------------------------------------------

TEST_CASE("plugin manifest: a plugin that lists its files")
{
    const manifest value = parse_ok(with_files(good_file("bin/fixture")));
    REQUIRE_EQ(value.files.size(), 1);
    REQUIRE_EQ(value.files[0].path, "bin/fixture");
    REQUIRE_EQ(value.files[0].size, 10);
    REQUIRE(all_bytes_are(value.files[0].sha512, 0xab));

    const manifest several = parse_ok(
        with_files(good_file("bin/fixture") + ", " + good_file("lib/a.dll") +
                   ", " + good_file("data/b.txt")));
    REQUIRE_EQ(several.files.size(), 3);

    // Capitals in the digest are read.
    const manifest upper = parse_ok(with_files(
        file_json("bin/fixture", "10", digest_upper_ab())));
    REQUIRE(all_bytes_are(upper.files[0].sha512, 0xab));

    // The largest size, and the smallest.
    REQUIRE(parse_manifest(with_files(file_json("bin/fixture", "4294967296",
                                                digest_ab())))
                .has_value());
    REQUIRE(parse_manifest(with_files(file_json("bin/fixture", "0",
                                                digest_ab())))
                .has_value());
}

TEST_CASE("plugin manifest: a digest that is not 128 hexadecimal digits")
{
    const std::vector<std::string> bad = {
        "",
        "ab",
        repeat("a", 127),
        repeat("a", 129),
        repeat("ab", 128),
        repeat("a", 127) + "g",
        repeat("a", 127) + "G",
        repeat("a", 127) + " ",
        repeat("a", 127) + "-",
        "0x" + repeat("a", 126)};
    for (const std::string& digest : bad)
    {
        expect_invalid(with_files(file_json("bin/fixture", "10", digest)),
                       "files[0].sha512: must be 128 hexadecimal digits",
                       digest);
    }
    expect_invalid(with_files(R"j({ "path": "bin/fixture", "size": 10,
                                 "sha512": 5 })j"),
                   "files[0].sha512: must be a string");
}

TEST_CASE("plugin manifest: the parts of a file entry")
{
    expect_invalid(with_files(R"j({ "path": "bin/fixture", "size": 1 })j"),
                   "files[0].sha512: is required");
    expect_invalid(with_files("{ \"path\": \"bin/fixture\", \"sha512\": \"" +
                              digest_ab() + "\" }"),
                   "files[0].size: is required");
    expect_invalid(with_files("{ \"size\": 1, \"sha512\": \"" + digest_ab() +
                              "\" }"),
                   "files[0].path: is required");
    expect_invalid(with_files(file_json("bin/fixture", "4294967297",
                                        digest_ab())),
                   "files[0].size: must be from 0 to 4294967296");
    expect_invalid(with_files(file_json("bin/fixture", "-1", digest_ab())),
                   "files[0].size: must be a whole number that is not "
                   "negative");
    expect_invalid(with_files(file_json("bin/fixture", "\"10\"",
                                        digest_ab())),
                   "files[0].size: must be a whole number");
    expect_invalid(with_files("{ \"path\": \"bin/fixture\", \"size\": 1, "
                              "\"sha512\": \"" + digest_ab() +
                              "\", \"mode\": 7 }"),
                   "files[0]: unknown key \"mode\"");
    expect_invalid(with_top("\"files\": []"), "files: must not be empty");
    expect_invalid(with_top("\"files\": 5"), "files: must be a list");
    expect_invalid(with_top("\"files\": [" + repeat("{}, ", 4096) + "{}]"),
                   "files: has more than 4096 items");
}

TEST_CASE("plugin manifest: a file listed twice")
{
    expect_layout(with_files(good_file("bin/fixture") + ", " +
                             good_file("bin/fixture")),
                  "files[0].path: \"bin/fixture\" is listed twice");
    expect_layout(with_files(good_file("bin/fixture") + ", " +
                             good_file("lib/a") + ", " + good_file("lib/a")),
                  "is listed twice");
}

TEST_CASE("plugin manifest: two files that differ only in case")
{
    expect_layout(with_files(good_file("bin/fixture") + ", " +
                             good_file("BIN/Fixture")),
                  "differs from another path only in case");
    expect_layout(with_files(good_file("bin/fixture") + ", " +
                             good_file("lib/Data") + ", " +
                             good_file("lib/data")),
                  "only in case");
}

TEST_CASE("plugin manifest: a file inside another file")
{
    expect_layout(with_files(good_file("bin/fixture") + ", " +
                             good_file("bin/fixture/sub")),
                  "files[1].path: \"bin/fixture/sub\" is inside a path that "
                  "is listed as a file");
    expect_layout(with_files(good_file("bin/fixture") + ", " + good_file("a") +
                             ", " + good_file("a/b")),
                  "is inside a path that is listed as a file");
    expect_layout(with_files(good_file("bin/fixture") + ", " +
                             good_file("a/b/c") + ", " + good_file("A/B")),
                  "is inside a path that is listed as a file");

    // A folder is not a file: these are fine.
    REQUIRE(parse_manifest(with_files(good_file("bin/fixture") + ", " +
                                      good_file("a/b") + ", " +
                                      good_file("a/c")))
                .has_value());
}

TEST_CASE("plugin manifest: the manifest and its signature are not listed")
{
    for (const char* const path :
         {"plugin.json", "PLUGIN.JSON", "Plugin.Json", "plugin.sig",
          "PLUGIN.sig"})
    {
        expect_layout(with_files(good_file("bin/fixture") + ", " +
                                 good_file(path)),
                      "files[1].path: the manifest and its signature are not "
                      "listed",
                      path);
    }

    // In a folder they are ordinary names.
    REQUIRE(parse_manifest(with_files(good_file("bin/fixture") + ", " +
                                      good_file("docs/plugin.json")))
                .has_value());
}

TEST_CASE("plugin manifest: the executable must be a listed file")
{
    expect_layout(with_files(good_file("lib/other")),
                  "run.executable: is not one of the listed files");
    expect_layout(with_files(good_file("bin/fixture2")),
                  "run.executable: is not one of the listed files");
    expect_layout(with_files(good_file("bin")),
                  "run.executable: is not one of the listed files");
}

TEST_CASE("plugin manifest: a listed path that breaks the layout rules")
{
    const std::vector<std::string> bad = {
        "../x", "/abs", "a\\\\b", "C:/x", "a//b", "con.txt", "a/nul",
        "trailing.", ""};
    for (const std::string& path : bad)
    {
        expect_layout(with_files(good_file("bin/fixture") + ", " +
                                 good_file(path)),
                      "files[1].path", path);
    }
    expect_invalid(with_files("{ \"path\": 5, \"size\": 1, \"sha512\": \"" +
                              digest_ab() + "\" }"),
                   "files[0].path: must be a string");
}

// -- paths --------------------------------------------------------------------

TEST_CASE("plugin manifest: relative paths that are accepted")
{
    const std::vector<std::string> good = {
        "a",           "bin/fixture", "a/b/c.d",    "a b/c",
        ".hidden",     "a..b",        "con1",       "COM10",
        "lpt",         "nul1.txt",    "x.nul",      "console",
        "aux2",        "a-b_c~1",     "A/B/C",      "1/2/3",
        "a.b.c",       "a/.hidden/b", "Run (1).exe",
        std::string(max_path_length, 'a')};
    for (const std::string& path : good)
    {
        const auto problem = check_relative_path(path);
        if (problem.has_value())
            fail_at("\"" + path + "\" was refused: " + *problem);
    }
}

TEST_CASE("plugin manifest: relative paths that are refused")
{
    const std::vector<std::string> refused = {
        "",          "/",          "/abs",       "//",        "a//b",
        "a/",        "/a",         ".",          "..",        "./x",
        "../x",      "a/./b",      "a/..",       "a/../b",    "a\\b",
        "a:b",       "C:/x",       "a*b",        "a?b",       "a\"b",
        "a<b",       "a>b",        "a|b",        "caf\xc3\xa9", "a\tb",
        "a\x7f",     std::string("a\0b", 3),     " a",        "a ",
        "a/ b",      "a /b",       "a./b",       "trailing.", "a/b.",
        "CON",       "con",        "Prn",        "AUX",       "NUL",
        "COM1",      "lpt9",       "com0",       "CONIN$",    "CONOUT$.txt",
        "a/nul",     "a/NUL.tar.gz", "nul.txt",  "nul .txt",  "aux.tar.gz",
        "bin/con",   "con.d/x",    "COM9.dll",
        std::string(max_path_length + 1, 'a')};
    for (const std::string& path : refused)
    {
        const auto problem = check_relative_path(path);
        if (!problem.has_value())
            fail_at("\"" + path + "\" was accepted");
        REQUIRE(!problem->empty());
    }
}

TEST_CASE("plugin manifest: what is wrong with a refused path")
{
    const auto said = [](std::string_view path) {
        const auto problem = check_relative_path(path);
        REQUIRE(problem.has_value());
        return *problem;
    };
    REQUIRE_EQ(said(""), "is empty");
    REQUIRE_EQ(said("/x"), "must be relative");
    REQUIRE_EQ(said("a//b"), "has an empty name");
    REQUIRE_EQ(said("a/"), "has an empty name");
    REQUIRE_EQ(said("a/../b"), "has the name ..");
    REQUIRE_EQ(said("./a"), "has the name .");
    REQUIRE_EQ(said("a\\b"), "has the character \\");
    REQUIRE_EQ(said("a:b"), "has the character :");
    REQUIRE_EQ(said("nul.txt"), "has the reserved name nul");
    REQUIRE_EQ(said("a/COM1"), "has the reserved name COM1");
    REQUIRE_EQ(said("caf\xc3\xa9"), "has a character that is not plain ASCII");
    REQUIRE_EQ(said(std::string(max_path_length + 1, 'a')),
               "is longer than 240 characters");
    REQUIRE(!check_relative_path("a/b").has_value());
}

TEST_CASE("plugin manifest: fold_path lower-cases ASCII only")
{
    REQUIRE_EQ(fold_path("Bin/FIXTURE.Txt"), "bin/fixture.txt");
    REQUIRE_EQ(fold_path("already/lower"), "already/lower");
    REQUIRE_EQ(fold_path(""), "");
    REQUIRE_EQ(fold_path("A1-_./Z9"), "a1-_./z9");

    // Bytes of other characters are left alone, so the length is the same.
    const std::string umlaut = "\xc3\x84";
    REQUIRE_EQ(fold_path(umlaut), umlaut);
    REQUIRE_EQ(fold_path("\xc3\x84X"), "\xc3\x84x");
    REQUIRE_EQ(fold_path(umlaut + "ABC").size(), 5);
}

// -- localized text -----------------------------------------------------------

TEST_CASE("plugin manifest: localized text picks the language asked for")
{
    const localized_text text = texts({{"de", "Hallo"},
                                       {"en", "Hello"},
                                       {"fr-CA", "Salut"},
                                       {"en-AU", "G'day"}});
    REQUIRE_EQ(text.best("de"), "Hallo");
    REQUIRE_EQ(text.best("en"), "Hello");
    REQUIRE_EQ(text.best("fr-CA"), "Salut");
    // The exact tag wins over its base language.
    REQUIRE_EQ(text.best("en-AU"), "G'day");
}

TEST_CASE("plugin manifest: localized text falls back to the base language")
{
    const localized_text text = texts({{"en", "Hello"}, {"de", "Hallo"}});
    REQUIRE_EQ(text.best("en-AU"), "Hello");
    // The base language comes before English.
    REQUIRE_EQ(text.best("de-AT"), "Hallo");
    REQUIRE_EQ(text.best("de-CH-1996"), "Hallo");
}

TEST_CASE("plugin manifest: localized text falls back to English")
{
    const localized_text text = texts({{"de", "Hallo"}, {"en", "Hello"}});
    REQUIRE_EQ(text.best("ja"), "Hello");
    REQUIRE_EQ(text.best("fr-CA"), "Hello");
    REQUIRE_EQ(text.best(""), "Hello");
}

TEST_CASE("plugin manifest: localized text falls back to the plain string")
{
    const localized_text text = texts({{"de", "Hallo"}, {"", "Plain"}});
    REQUIRE_EQ(text.best("ja"), "Plain");
    REQUIRE_EQ(text.best("de"), "Hallo");
    REQUIRE_EQ(text.best(""), "Plain");

    const manifest value = parse_ok(minimal_text());
    REQUIRE_EQ(value.extension_points[0].name.best("ja"), "Slave");
    REQUIRE_EQ(value.extension_points[0].name.best("en"), "Slave");
}

TEST_CASE("plugin manifest: localized text falls back to the first entry")
{
    const localized_text text = texts({{"de", "Hallo"}, {"fr", "Salut"}});
    REQUIRE_EQ(text.best("ja"), "Hallo");
    REQUIRE_EQ(text.best(""), "Hallo");
    REQUIRE_EQ(text.best("fr"), "Salut");
}

TEST_CASE("plugin manifest: empty localized text")
{
    const localized_text none;
    REQUIRE(none.empty());
    REQUIRE(none.best("en").empty());
    REQUIRE(none.best("").empty());
    REQUIRE(none.best("de-AT").empty());

    const localized_text some = texts({{"en", "Hello"}});
    REQUIRE(!some.empty());
    REQUIRE(some == texts({{"en", "Hello"}}));
    REQUIRE(!(some == none));
}

// -- names --------------------------------------------------------------------

TEST_CASE("plugin manifest: every permission has a name that parses back")
{
    struct named_permission
    {
        permission value;
        std::string_view name;
    };
    const std::array<named_permission, 14> all{{
        {permission::ports_read, "ports.read"},
        {permission::traffic_tap, "traffic.tap"},
        {permission::ports_write, "ports.write"},
        {permission::topology_modify, "topology.modify"},
        {permission::modifier_inline, "modifier.inline"},
        {permission::faults, "faults"},
        {permission::serial_devices, "serial.devices"},
        {permission::fs_read, "fs.read"},
        {permission::fs_write, "fs.write"},
        {permission::data_private, "data.private"},
        {permission::net_listen, "net.listen"},
        {permission::net_outbound, "net.outbound"},
        {permission::process_spawn, "process.spawn"},
        {permission::bundle_section, "bundle.section"},
    }};
    for (const named_permission& item : all)
    {
        REQUIRE_EQ(to_string(item.value), item.name);
        const std::optional<permission> parsed = parse_permission(item.name);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == item.value);
    }

    REQUIRE(!parse_permission("").has_value());
    REQUIRE(!parse_permission("Ports.Read").has_value());
    REQUIRE(!parse_permission("ports_read").has_value());
    REQUIRE(!parse_permission("ports.read ").has_value());
    REQUIRE(!parse_permission("teleport").has_value());
}

TEST_CASE("plugin manifest: names of kinds, classes and types")
{
    REQUIRE_EQ(to_string(extension_kind::generator), "generator");
    REQUIRE_EQ(to_string(extension_kind::fixture), "fixture");
    REQUIRE_EQ(to_string(extension_kind::modifier), "modifier");
    REQUIRE_EQ(to_string(extension_kind::adapter), "adapter");

    REQUIRE_EQ(to_string(timing_class::rules), "rules");
    REQUIRE_EQ(to_string(timing_class::in_line), "inline");
    REQUIRE_EQ(to_string(timing_class::async), "async");

    REQUIRE_EQ(to_string(parameter_type::boolean), "bool");
    REQUIRE_EQ(to_string(parameter_type::integer), "int");
    REQUIRE_EQ(to_string(parameter_type::number), "number");
    REQUIRE_EQ(to_string(parameter_type::choice), "enum");
    REQUIRE_EQ(to_string(parameter_type::string), "string");
    REQUIRE_EQ(to_string(parameter_type::bytes), "bytes");
    REQUIRE_EQ(to_string(parameter_type::duration), "duration");
    REQUIRE_EQ(to_string(parameter_type::port), "port");
    REQUIRE_EQ(to_string(parameter_type::link), "link");
    REQUIRE_EQ(to_string(parameter_type::path), "path");
    REQUIRE_EQ(to_string(parameter_type::secret), "secret");
}

TEST_CASE("plugin manifest: versions are written as they were read")
{
    const plugin_version plain{3, 14, 159, std::string()};
    REQUIRE_EQ(to_string(plain), "3.14.159");
    const plugin_version pre{1, 0, 0, "rc.1"};
    REQUIRE_EQ(to_string(pre), "1.0.0-rc.1");
    REQUIRE(!(plain == pre));
}

// -- errors -------------------------------------------------------------------

TEST_CASE("plugin manifest: plugin_errc has names and messages")
{
    REQUIRE_EQ(to_string(plugin_errc::success), "success");
    REQUIRE_EQ(to_string(plugin_errc::manifest_invalid), "manifest_invalid");
    REQUIRE_EQ(to_string(plugin_errc::manifest_too_large),
               "manifest_too_large");
    REQUIRE_EQ(to_string(plugin_errc::manifest_unsupported_version),
               "manifest_unsupported_version");
    REQUIRE_EQ(to_string(plugin_errc::layout_invalid), "layout_invalid");
    REQUIRE_EQ(to_string(plugin_errc::signature_invalid),
               "signature_invalid");
    REQUIRE_EQ(to_string(plugin_errc::quarantined), "quarantined");
    REQUIRE_EQ(to_string(plugin_errc::reserved_id), "reserved_id");

    for (int value = static_cast<int>(plugin_errc::success);
         value <= static_cast<int>(plugin_errc::reserved_id); ++value)
    {
        const plugin_errc code = static_cast<plugin_errc>(value);
        REQUIRE(!plugin_category().message(value).empty());
        REQUIRE(to_string(code) != "unknown");
        REQUIRE(!to_string(code).empty());
    }
}

TEST_CASE("plugin manifest: the category of plugin_errc")
{
    REQUIRE_EQ(std::string(plugin_category().name()), "vss.plugin");
    const error_code code = make_error_code(plugin_errc::layout_invalid);
    REQUIRE(code.category() == plugin_category());
    REQUIRE(code == plugin_errc::layout_invalid);
    REQUIRE(code != plugin_errc::manifest_invalid);
    REQUIRE(code.failed());
    REQUIRE(!make_error_code(plugin_errc::success).failed());
    REQUIRE(code.message() == plugin_category().message(code.value()));
}

TEST_CASE("plugin manifest: the error of a refused manifest has the category")
{
    std::string detail;
    const auto refused = parse_manifest("{}", &detail);
    REQUIRE(refused.has_error());
    REQUIRE(refused.error().category() == plugin_category());
    REQUIRE(refused.error() == plugin_errc::manifest_invalid);
    REQUIRE(!detail.empty());

    // The detail is optional.
    const auto without_detail = parse_manifest("{}");
    REQUIRE(without_detail.has_error());
    REQUIRE(without_detail.error() == refused.error());
}
