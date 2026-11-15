//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The values a user gives to a plugin: checked against the manifest's
// declarations, and read and written as the text of a setup file's plugin
// binding and of the plugin API messages.
//

#include <vss/plugin/manifest.hpp>
#include <vss/plugin/parameters.hpp>

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace vss;
using namespace vss::plugin;

namespace
{

namespace fs = std::filesystem;

// -- declarations from the real manifest reader -----------------------------------

/// A complete manifest whose one extension point has these parameters.
std::string manifest_text(std::string_view parameters)
{
    std::string out = R"j({
  "manifest_version": 1,
  "id": "com.example.fixture",
  "name": "Example fixture",
  "version": "1.2.3",
  "requires": { "api": { "min": "1.0", "max": "1.x" } },
  "run": { "executable": "bin/fixture", "platforms": ["linux-x64"] },
  "permissions": [ { "name": "ports.read", "reason": "Needs its port." } ],
  "extension_points": [
    { "kind": "fixture", "id": "slave", "name": "Slave",
      "timing": { "class": "rules" },
      "parameters": [)j";
    out.append(parameters);
    out.append("] } ]\n}");
    return out;
}

std::vector<parameter> declare(std::string_view parameters)
{
    std::string detail;
    const auto read = parse_manifest(manifest_text(parameters), &detail);
    if (!read.has_value())
        vss_test::raise(detail.c_str(), __FILE__, __LINE__);
    return read->extension_points.front().parameters;
}

/// One parameter `p` of this kind: `type`, then the other members.
std::vector<parameter> one(std::string_view body)
{
    std::string text = R"j({ "id": "p", "label": "P", )j";
    text.append(body);
    text.append(" }");
    return declare(text);
}

raw_parameters given(std::string_view id, raw_value value)
{
    raw_parameters out;
    out.push_back({std::string(id), std::move(value)});
    return out;
}

/// The one value `p` is given, checked.
validation check(std::string_view body, raw_value value)
{
    return validate(one(body), given("p", std::move(value)));
}

std::string text_of(const validation& result)
{
    return to_json(result.values, value_source::user);
}

bool refused(const validation& result, parameter_errc code)
{
    return !result.ok() && result.errors.size() == 1 &&
           result.errors.front().code == code &&
           result.errors.front().id == "p" &&
           !result.errors.front().line.empty() && result.values.empty();
}

/// The value of the single checked parameter.
template <class T>
T value_of(const validation& result)
{
    REQUIRE(result.ok());
    REQUIRE_EQ(result.values.size(), std::size_t{1});
    const T* const value = std::get_if<T>(&result.values.front().value);
    REQUIRE(value != nullptr);
    return *value;
}

/// The scope of a checked path value, or an empty one when it has none.
const path_scope& scope_of(const checked_parameter& item)
{
    static const path_scope none;
    return item.scope ? *item.scope : none;
}

/// U+202E, built from bytes so the source holds no direction mark.
std::string bidi_override()
{
    return {static_cast<char>(0xe2), static_cast<char>(0x80),
            static_cast<char>(0xae)};
}

std::string repeat(char c, std::size_t count)
{
    return std::string(count, c);
}

raw_value text(std::string_view value)
{
    return raw_value{std::string(value)};
}

constexpr std::int64_t whole(std::int64_t value)
{
    return value;
}

} // namespace

// -- bool -----------------------------------------------------------------------

TEST_CASE("plugin parameters: a boolean is true or false and nothing else")
{
    const std::string body = R"j("type": "bool", "default": false)j";
    REQUIRE(value_of<bool>(check(body, true)));
    REQUIRE(!value_of<bool>(check(body, false)));
    REQUIRE(refused(check(body, whole(1)), parameter_errc::wrong_type));
    REQUIRE(refused(check(body, text("true")), parameter_errc::wrong_type));
    REQUIRE(refused(check(body, 1.0), parameter_errc::wrong_type));
}

// -- int ------------------------------------------------------------------------

TEST_CASE("plugin parameters: an integer is checked at the edges of min and max")
{
    const std::string body =
        R"j("type": "int", "min": 1, "max": 247, "default": 5)j";
    REQUIRE_EQ(value_of<std::int64_t>(check(body, whole(1))), 1);
    REQUIRE_EQ(value_of<std::int64_t>(check(body, whole(247))), 247);
    REQUIRE(refused(check(body, whole(0)), parameter_errc::below_min));
    REQUIRE(refused(check(body, whole(248)), parameter_errc::above_max));
    REQUIRE(refused(check(body, whole(-5)), parameter_errc::below_min));
    REQUIRE(refused(check(body, 5.0), parameter_errc::wrong_type));
    REQUIRE(refused(check(body, text("5")), parameter_errc::wrong_type));
    REQUIRE(refused(check(body, true), parameter_errc::wrong_type));
}

TEST_CASE("plugin parameters: an integer without limits takes any 64 bit value")
{
    const std::string body = R"j("type": "int", "default": 0)j";
    constexpr auto least = std::numeric_limits<std::int64_t>::min();
    constexpr auto most = std::numeric_limits<std::int64_t>::max();
    REQUIRE_EQ(value_of<std::int64_t>(check(body, least)), least);
    REQUIRE_EQ(value_of<std::int64_t>(check(body, most)), most);
}

TEST_CASE("plugin parameters: the line for an integer names the limit")
{
    const auto result =
        check(R"j("type": "int", "min": 1, "max": 247, "default": 5)j",
              whole(248));
    REQUIRE_EQ(result.errors.front().line,
               std::string("\"p\" must be at most 247."));
    REQUIRE_EQ(std::string(to_string(result.errors.front().code)),
               std::string("above_max"));
}

// -- number ---------------------------------------------------------------------

TEST_CASE("plugin parameters: a number takes a whole number or a real one inside its limits")
{
    const std::string body =
        R"j("type": "number", "min": -1.5, "max": 2.5, "default": 0)j";
    REQUIRE_EQ(value_of<double>(check(body, -1.5)), -1.5);
    REQUIRE_EQ(value_of<double>(check(body, 2.5)), 2.5);
    REQUIRE_EQ(value_of<double>(check(body, whole(2))), 2.0);
    REQUIRE(refused(check(body, 2.5000001), parameter_errc::above_max));
    REQUIRE(refused(check(body, -1.6), parameter_errc::below_min));
    REQUIRE(refused(check(body, whole(3)), parameter_errc::above_max));
    REQUIRE(refused(check(body, text("1.0")), parameter_errc::wrong_type));
}

TEST_CASE("plugin parameters: a number that is not finite is refused")
{
    const std::string body = R"j("type": "number", "default": 0)j";
    REQUIRE(refused(check(body, std::numeric_limits<double>::quiet_NaN()),
                    parameter_errc::bad_format));
    REQUIRE(refused(check(body, std::numeric_limits<double>::infinity()),
                    parameter_errc::bad_format));
    REQUIRE(refused(check(body, -std::numeric_limits<double>::infinity()),
                    parameter_errc::bad_format));
}

TEST_CASE("plugin parameters: a whole number a number cannot hold exactly is refused")
{
    const std::string body = R"j("type": "number", "default": 0)j";
    REQUIRE_EQ(value_of<double>(check(body, whole(9007199254740992))),
               9007199254740992.0);
    REQUIRE(refused(check(body, whole(9007199254740993)),
                    parameter_errc::bad_format));
}

// -- duration -------------------------------------------------------------------

TEST_CASE("plugin parameters: a duration is whole units, inside its limits")
{
    const std::string body =
        R"j("type": "duration", "min": "10us", "max": "1s", "default": "3ms")j";
    REQUIRE_EQ(value_of<std::int64_t>(check(body, text("10us"))), 10'000);
    REQUIRE_EQ(value_of<std::int64_t>(check(body, text("1s"))),
               1'000'000'000);
    REQUIRE_EQ(value_of<std::int64_t>(check(body, text("1000ms"))),
               1'000'000'000);
    REQUIRE_EQ(value_of<std::int64_t>(check(body, text("10000ns"))), 10'000);
    REQUIRE(refused(check(body, text("9999ns")), parameter_errc::below_min));
    REQUIRE(refused(check(body, text("1001ms")), parameter_errc::above_max));
    REQUIRE(refused(check(body, text("2s")), parameter_errc::above_max));
}

TEST_CASE("plugin parameters: a duration with a wrong spelling is refused")
{
    const std::string body = R"j("type": "duration", "default": "3ms")j";
    for (const char* bad :
         {"", "3", "ms", "3 ms", "3m", "3h", "3MS", "-3ms", "1.5ms", "+3ms",
          "3ms ", " 3ms", "0x3ms", "3msms", "99999999999999999999s"})
    {
        REQUIRE(refused(check(body, text(bad)), parameter_errc::bad_format));
    }
    REQUIRE(refused(check(body, whole(3)), parameter_errc::wrong_type));
    REQUIRE(refused(check(body, 3.0), parameter_errc::wrong_type));
}

TEST_CASE("plugin parameters: a duration may be as long as one day and no longer")
{
    const std::string body = R"j("type": "duration", "default": "3ms")j";
    REQUIRE_EQ(value_of<std::int64_t>(check(body, text("86400s"))),
               max_parameter_duration_ns);
    REQUIRE_EQ(value_of<std::int64_t>(check(body, text("86400000000000ns"))),
               max_parameter_duration_ns);
    REQUIRE(refused(check(body, text("86401s")), parameter_errc::bad_format));
    REQUIRE(refused(check(body, text("86400000000001ns")),
                    parameter_errc::bad_format));
    REQUIRE_EQ(value_of<std::int64_t>(check(body, text("0ns"))), 0);
}

TEST_CASE("plugin parameters: a duration is written in the largest unit that holds it exactly")
{
    const std::string body = R"j("type": "duration", "default": "3ms")j";
    const auto write = [&](std::string_view spelling) {
        return text_of(check(body, text(spelling)));
    };
    REQUIRE_EQ(write("3000us"), std::string(R"({"p":"3ms"})"));
    REQUIRE_EQ(write("1750us"), std::string(R"({"p":"1750us"})"));
    REQUIRE_EQ(write("2000ms"), std::string(R"({"p":"2s"})"));
    REQUIRE_EQ(write("1500ms"), std::string(R"({"p":"1500ms"})"));
    REQUIRE_EQ(write("0us"), std::string(R"({"p":"0s"})"));
    REQUIRE_EQ(write("1001ns"), std::string(R"({"p":"1001ns"})"));
}

// -- enum -----------------------------------------------------------------------

TEST_CASE("plugin parameters: an enumeration takes the value of one option")
{
    const std::string body =
        R"j("type": "enum", "default": "even", "options": [
            { "value": "even", "label": "Even" },
            { "value": "odd", "label": "Odd" }])j";
    REQUIRE_EQ(value_of<std::string>(check(body, text("odd"))),
               std::string("odd"));
    REQUIRE(refused(check(body, text("Odd")), parameter_errc::not_an_option));
    REQUIRE(refused(check(body, text("")), parameter_errc::not_an_option));
    REQUIRE(refused(check(body, text("odd ")), parameter_errc::not_an_option));
    REQUIRE(refused(check(body, whole(1)), parameter_errc::wrong_type));
}

// -- string ---------------------------------------------------------------------

TEST_CASE("plugin parameters: a string is plain text of at most max_length bytes")
{
    const std::string body =
        R"j("type": "string", "max_length": 8, "default": "x")j";
    REQUIRE_EQ(value_of<std::string>(check(body, text(""))), std::string());
    REQUIRE_EQ(value_of<std::string>(check(body, text(repeat('a', 8)))),
               repeat('a', 8));
    REQUIRE(refused(check(body, text(repeat('a', 9))),
                    parameter_errc::too_long));
    // "e" with an acute accent is two bytes: four of them fit, five do not.
    const std::string accent = "\xc3\xa9";
    std::string four;
    std::string five;
    for (int i = 0; i < 4; ++i)
        four.append(accent);
    five = four + accent;
    REQUIRE_EQ(value_of<std::string>(check(body, text(four))), four);
    REQUIRE(refused(check(body, text(five)), parameter_errc::too_long));
    REQUIRE(refused(check(body, whole(1)), parameter_errc::wrong_type));
}

TEST_CASE("plugin parameters: a string with a control character or a direction mark is refused")
{
    const std::string body =
        R"j("type": "string", "max_length": 64, "default": "x")j";
    for (const std::string& bad :
         {std::string("a\nb"), std::string("a\tb"), std::string("a\x01" "b"),
          std::string("a\x7f"), bidi_override() + "flip",
          std::string("\xe2\x80\x8f"), std::string("\xff\xfe"),
          std::string("\xc3")})
    {
        REQUIRE(refused(check(body, text(bad)), parameter_errc::bad_format));
    }
}

TEST_CASE("plugin parameters: a string without max_length takes 256 bytes")
{
    const std::string body = R"j("type": "string", "default": "x")j";
    REQUIRE_EQ(value_of<std::string>(check(body, text(repeat('x', 256)))),
               repeat('x', 256));
    REQUIRE(refused(check(body, text(repeat('x', 257))),
                    parameter_errc::too_long));
}

TEST_CASE("plugin parameters: the longest string the manifest allows is taken")
{
    const std::string body =
        R"j("type": "string", "max_length": 4096, "default": "x")j";
    REQUIRE_EQ(value_of<std::string>(check(body, text(repeat('z', 4096))))
                   .size(),
               std::size_t{4096});
    REQUIRE(refused(check(body, text(repeat('z', 4097))),
                    parameter_errc::too_long));
}

// -- bytes ----------------------------------------------------------------------

TEST_CASE("plugin parameters: bytes are hex, with or without a space between pairs")
{
    const std::string body =
        R"j("type": "bytes", "max_length": 4, "default": "02 31 03")j";
    const std::string expected = "\x02\x31\x03";
    REQUIRE_EQ(value_of<std::string>(check(body, text("02 31 03"))),
               expected);
    REQUIRE_EQ(value_of<std::string>(check(body, text("023103"))), expected);
    REQUIRE_EQ(value_of<std::string>(check(body, text("0A fF"))),
               std::string("\x0a\xff", 2));
    REQUIRE_EQ(value_of<std::string>(check(body, text(""))), std::string());
    REQUIRE_EQ(text_of(check(body, text("02 31 03"))),
               std::string(R"({"p":"023103"})"));
}

TEST_CASE("plugin parameters: bytes are checked at max_length and for their spelling")
{
    const std::string body =
        R"j("type": "bytes", "max_length": 4, "default": "")j";
    REQUIRE_EQ(value_of<std::string>(check(body, text("01020304"))).size(),
               std::size_t{4});
    REQUIRE(refused(check(body, text("0102030405")), parameter_errc::too_long));
    for (const char* bad : {"0", "012", "0g", "02  31", "02 ", " 02", "0x02",
                            "02,31", "02-31", "0 2"})
    {
        REQUIRE(refused(check(body, text(bad)), parameter_errc::bad_format));
    }
    REQUIRE(refused(check(body, whole(2)), parameter_errc::wrong_type));
    REQUIRE(refused(check(body, text(repeat('0', 20000))),
                    parameter_errc::too_long));
}

// -- port and link ----------------------------------------------------------------

TEST_CASE("plugin parameters: a port or a link is a name of 1 to 100 bytes of plain text")
{
    for (const char* type : {"port", "link"})
    {
        const std::string body = std::string(R"j("type": ")j") + type + "\"";
        REQUIRE_EQ(value_of<std::string>(check(body, text("COM3"))),
                   std::string("COM3"));
        REQUIRE_EQ(value_of<std::string>(check(body, text(repeat('n', 100))))
                       .size(),
                   std::size_t{100});
        REQUIRE(refused(check(body, text("")), parameter_errc::too_long));
        REQUIRE(refused(check(body, text(repeat('n', 101))),
                        parameter_errc::too_long));
        REQUIRE(refused(check(body, text("a\nb")), parameter_errc::bad_format));
        REQUIRE(refused(check(body, whole(3)), parameter_errc::wrong_type));
    }
}

// -- secret ---------------------------------------------------------------------

TEST_CASE("plugin parameters: a secret is text a user gives and is required")
{
    const std::string body = R"j("type": "secret", "max_length": 8)j";
    REQUIRE_EQ(value_of<std::string>(check(body, text("hunter2"))),
               std::string("hunter2"));
    REQUIRE(refused(check(body, text(repeat('s', 9))),
                    parameter_errc::too_long));
    const validation missing = validate(one(body), {});
    REQUIRE(refused(missing, parameter_errc::missing_required));
}

TEST_CASE("plugin parameters: a setup never holds a secret")
{
    const std::string body = R"j("type": "secret", "max_length": 8)j";
    const validation found =
        validate(one(body), given("p", text("x")), value_source::setup);
    REQUIRE(refused(found, parameter_errc::stored_secret));
    const validation absent = validate(one(body), {}, value_source::setup);
    REQUIRE(absent.ok());
    REQUIRE(absent.values.empty());
}

TEST_CASE("plugin parameters: a secret is written for a message and left out of a setup")
{
    const std::vector<parameter> declared = declare(R"j(
        { "id": "a", "label": "A", "type": "int", "default": 1 },
        { "id": "s", "label": "S", "type": "secret" })j");
    raw_parameters values;
    values.push_back({"s", text("pw")});
    const validation checked = validate(declared, values);
    REQUIRE(checked.ok());
    REQUIRE_EQ(to_json(checked.values, value_source::user),
               std::string(R"({"a":1,"s":"pw"})"));
    REQUIRE_EQ(to_json(checked.values, value_source::setup),
               std::string(R"({"a":1})"));
    REQUIRE_EQ(to_binding_params(checked.values), std::string(R"({"a":1})"));
    REQUIRE_EQ(stable_text(checked.values), std::string(R"({"a":1})"));
}

// -- defaults, required, unknown, repeated -------------------------------------------

TEST_CASE("plugin parameters: a value left out takes the default and says so")
{
    const std::vector<parameter> declared = declare(R"j(
        { "id": "address", "label": "A", "type": "int", "min": 1,
          "max": 247, "default": 1 },
        { "id": "delay", "label": "D", "type": "duration", "default": "3ms" },
        { "id": "mode", "label": "M", "type": "enum", "default": "b",
          "options": [ { "value": "a", "label": "A" },
                       { "value": "b", "label": "B" } ] },
        { "id": "on", "label": "O", "type": "bool", "default": true },
        { "id": "name", "label": "N", "type": "string", "default": "x" },
        { "id": "gain", "label": "G", "type": "number", "default": 1.5 },
        { "id": "blank", "label": "B", "type": "bytes", "default": "00 ff" }
    )j");
    const validation result = validate(declared, {});
    REQUIRE(result.ok());
    REQUIRE_EQ(result.values.size(), declared.size());
    for (std::size_t i = 0; i < declared.size(); ++i)
    {
        REQUIRE_EQ(result.values[i].id, declared[i].id);
        REQUIRE(result.values[i].defaulted);
        REQUIRE(result.values[i].value == declared[i].default_value);
    }
    REQUIRE_EQ(to_json(result.values, value_source::user),
               std::string(R"({"address":1,"blank":"00ff","delay":"3ms",)"
                           R"("gain":1.5E0,"mode":"b","name":"x","on":true})"));
}

TEST_CASE("plugin parameters: a given value is used in place of the default")
{
    const std::vector<parameter> declared = declare(R"j(
        { "id": "address", "label": "A", "type": "int", "default": 1 },
        { "id": "on", "label": "O", "type": "bool", "default": true })j");
    const validation result = validate(declared, given("on", false));
    REQUIRE(result.ok());
    REQUIRE(result.values[0].defaulted);
    REQUIRE(!result.values[1].defaulted);
    REQUIRE(std::get<bool>(result.values[1].value) == false);
}

TEST_CASE("plugin parameters: a parameter with no default must be given")
{
    for (const char* type :
         {"bool", "int", "number", "duration", "string", "bytes", "port",
          "link"})
    {
        const std::string body = std::string(R"j("type": ")j") + type + "\"";
        REQUIRE(refused(validate(one(body), {}),
                        parameter_errc::missing_required));
    }
    REQUIRE(refused(validate(one(R"j("type": "path")j"), {}),
                    parameter_errc::missing_required));
    REQUIRE(refused(
        validate(one(R"j("type": "enum", "options": [
                { "value": "a", "label": "A" }])j"),
                 {}),
        parameter_errc::missing_required));
}

TEST_CASE("plugin parameters: a name that is not declared is refused with that name")
{
    const std::vector<parameter> declared =
        one(R"j("type": "int", "default": 1)j");
    raw_parameters values;
    values.push_back({"p", whole(2)});
    values.push_back({"q", whole(3)});
    const validation result = validate(declared, values);
    REQUIRE(!result.ok());
    REQUIRE(result.values.empty());
    REQUIRE_EQ(result.errors.size(), std::size_t{1});
    REQUIRE_EQ(result.errors[0].id, std::string("q"));
    REQUIRE(result.errors[0].code == parameter_errc::unknown_name);
    REQUIRE_EQ(result.errors[0].line,
               std::string("\"q\" is not a setting of this plugin."));
}

TEST_CASE("plugin parameters: a name given twice is refused")
{
    const std::vector<parameter> declared =
        one(R"j("type": "int", "default": 1)j");
    raw_parameters values;
    values.push_back({"p", whole(2)});
    values.push_back({"p", whole(3)});
    const validation result = validate(declared, values);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.errors.size(), std::size_t{1});
    REQUIRE(result.errors[0].code == parameter_errc::repeated_name);
}

TEST_CASE("plugin parameters: every refusal is reported, one for each parameter")
{
    const std::vector<parameter> declared = declare(R"j(
        { "id": "a", "label": "A", "type": "int", "min": 0, "max": 5 },
        { "id": "b", "label": "B", "type": "bool" },
        { "id": "c", "label": "C", "type": "int", "default": 1 })j");
    raw_parameters values;
    values.push_back({"a", whole(9)});
    values.push_back({"x", whole(1)});
    const validation result = validate(declared, values);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.errors.size(), std::size_t{3});
    REQUIRE(result.errors[0].code == parameter_errc::unknown_name);
    REQUIRE(result.errors[1].code == parameter_errc::above_max);
    REQUIRE(result.errors[2].code == parameter_errc::missing_required);
    REQUIRE(result.values.empty());
}

TEST_CASE("plugin parameters: an unknown name is shown cut short and in plain ASCII")
{
    raw_parameters values;
    values.push_back({bidi_override() + repeat('k', 400), whole(1)});
    const validation result = validate({}, values);
    REQUIRE(!result.ok());
    REQUIRE(result.errors[0].line.size() < 200);
    for (const char c : result.errors[0].line)
    {
        const auto byte = static_cast<unsigned char>(c);
        REQUIRE(byte >= 0x20 && byte < 0x7f);
    }
}

TEST_CASE("plugin parameters: every error code has a name")
{
    for (const parameter_errc code :
         {parameter_errc::missing_required, parameter_errc::unknown_name,
          parameter_errc::repeated_name, parameter_errc::wrong_type,
          parameter_errc::below_min, parameter_errc::above_max,
          parameter_errc::too_long, parameter_errc::bad_format,
          parameter_errc::not_an_option, parameter_errc::bad_path,
          parameter_errc::stored_secret})
    {
        REQUIRE(!to_string(code).empty());
        REQUIRE(to_string(code) != "unknown");
    }
}

// -- path -----------------------------------------------------------------------

namespace
{

/// Whether a path that starts with a slash is a full path here.  It is on
/// Linux and macOS and is not on Windows, where a full path has a drive.
bool slash_paths_are_full()
{
    return fs::path("/a").is_absolute();
}

/// A full path of this system, and the same inside a folder, written with
/// the system's own separator.
std::string base_path()
{
    return slash_paths_are_full() ? "/tmp/vss-data" : "C:\\vss-data";
}

std::string sep()
{
    return slash_paths_are_full() ? "/" : "\\";
}

std::string root_path()
{
    return slash_paths_are_full() ? "/" : "C:\\";
}

const std::string_view file_body = R"j("type": "path", "kind": "file")j";
const std::string_view dir_body =
    R"j("type": "path", "kind": "directory", "access": "write",
        "must_exist": true)j";

} // namespace

TEST_CASE("plugin parameters: a full path to a folder is taken as it is")
{
    const validation result = check(dir_body, text(base_path()));
    REQUIRE(result.ok());
    REQUIRE_EQ(value_of<std::string>(result), base_path());
    REQUIRE(result.values.front().scope.has_value());
    const path_scope& scope = scope_of(result.values.front());
    REQUIRE_EQ(scope.path, base_path());
    REQUIRE(scope.kind == path_kind::directory);
    REQUIRE(scope.access == path_access::write);
    REQUIRE(scope.must_exist);
}

TEST_CASE("plugin parameters: a path to a file keeps its kind and access in the scope")
{
    const validation result =
        check(file_body, text(base_path() + sep() + "a.txt"));
    REQUIRE(result.ok());
    const path_scope& scope = scope_of(result.values.front());
    REQUIRE_EQ(scope.path, base_path() + sep() + "a.txt");
    REQUIRE(scope.kind == path_kind::file);
    REQUIRE(scope.access == path_access::read);
    REQUIRE(!scope.must_exist);
}

TEST_CASE("plugin parameters: a trailing separator is dropped from a folder")
{
    REQUIRE_EQ(value_of<std::string>(check(dir_body,
                                           text(base_path() + sep()))),
               base_path());
    REQUIRE_EQ(value_of<std::string>(
                   check(dir_body, text(base_path() + sep() + sep()))),
               base_path());
}

TEST_CASE("plugin parameters: a trailing separator on a file is refused")
{
    REQUIRE(refused(check(file_body, text(base_path() + sep())),
                    parameter_errc::bad_path));
}

TEST_CASE("plugin parameters: dots and doubled separators in a path are folded away")
{
    REQUIRE_EQ(value_of<std::string>(check(
                   dir_body,
                   text(base_path() + sep() + "." + sep() + "sub" + sep() +
                        sep() + "x"))),
               base_path() + sep() + "sub" + sep() + "x");
}

TEST_CASE("plugin parameters: a path with two dots in it is refused wherever they are")
{
    for (const std::string tail :
         {"/..", "/../x", "/a/../b", "/a/b/../..", "/a/.."})
    {
        std::string path = base_path();
        for (const char c : tail)
            path.push_back(c == '/' ? sep().front() : c);
        REQUIRE(refused(check(dir_body, text(path)),
                        parameter_errc::bad_path));
        REQUIRE(refused(check(file_body, text(path)),
                        parameter_errc::bad_path));
    }
    // A name with dots in it is only a name.
    REQUIRE(check(dir_body, text(base_path() + sep() + "a..b")).ok());
    REQUIRE(check(dir_body, text(base_path() + sep() + "...")).ok());
}

TEST_CASE("plugin parameters: a root or a drive is refused")
{
    REQUIRE(refused(check(dir_body, text(root_path())),
                    parameter_errc::bad_path));
    REQUIRE(refused(check(dir_body, text(root_path() + sep() + ".")),
                    parameter_errc::bad_path));
    REQUIRE(refused(check(dir_body, text(root_path() + sep() + sep())),
                    parameter_errc::bad_path));
    REQUIRE(refused(check(file_body, text(root_path())),
                    parameter_errc::bad_path));
    if (!slash_paths_are_full())
    {
        for (const char* drive :
             {"C:", "C:\\", "C:/", "c:\\.", "D:\\\\", "Z:/"})
        {
            REQUIRE(refused(check(dir_body, text(drive)),
                            parameter_errc::bad_path));
        }
    }
}

TEST_CASE("plugin parameters: a path that is not full is refused")
{
    for (const char* bad :
         {"", "data", "./data", "sub/dir", "~/data", "data/", "."})
    {
        REQUIRE(!check(dir_body, text(bad)).ok());
        REQUIRE(check(dir_body, text(bad)).errors.front().code ==
                parameter_errc::bad_path);
    }
    REQUIRE(refused(check(dir_body, whole(1)), parameter_errc::wrong_type));
    REQUIRE(refused(check(dir_body, text(repeat('a', 1025))),
                    parameter_errc::bad_path));
}

TEST_CASE("plugin parameters: a path with a control character is refused")
{
    REQUIRE(refused(check(dir_body, text(base_path() + sep() + "a\nb")),
                    parameter_errc::bad_path));
    REQUIRE(refused(check(dir_body, text(base_path() + sep() + "a\x01")),
                    parameter_errc::bad_path));
}

TEST_CASE("plugin parameters: a network or device path is refused")
{
    for (const char* bad :
         {"//server/share", "\\\\server\\share", "//?/C:/x", "\\\\?\\C:\\x",
          "\\\\.\\COM3", "//./pipe/x", "/\\server/share"})
    {
        REQUIRE(refused(check(dir_body, text(bad)),
                        parameter_errc::bad_path));
    }
}

TEST_CASE("plugin parameters: on Windows both separators work and a drive path is full")
{
    if (slash_paths_are_full())
        return;
    REQUIRE_EQ(value_of<std::string>(check(dir_body, text("C:/data\\sub/x"))),
               std::string("C:\\data\\sub\\x"));
    REQUIRE_EQ(value_of<std::string>(check(dir_body, text("c:\\Data\\"))),
               std::string("c:\\Data"));
    // A path with no drive, or a drive with no folder, is not a full path.
    REQUIRE(refused(check(dir_body, text("/data")), parameter_errc::bad_path));
    REQUIRE(refused(check(dir_body, text("\\data")), parameter_errc::bad_path));
    REQUIRE(refused(check(dir_body, text("C:data")), parameter_errc::bad_path));
    // The name of a stream of a file.
    REQUIRE(refused(check(file_body, text("C:\\data\\a.txt:stream")),
                    parameter_errc::bad_path));
}

TEST_CASE("plugin parameters: on Linux and macOS a backslash is a letter of a name")
{
    if (!slash_paths_are_full())
        return;
    REQUIRE_EQ(value_of<std::string>(check(dir_body, text("/tmp/a\\b"))),
               std::string("/tmp/a\\b"));
    REQUIRE(refused(check(dir_body, text("C:\\data")),
                    parameter_errc::bad_path));
    REQUIRE_EQ(value_of<std::string>(check(dir_body, text("/tmp//x/./y/"))),
               std::string("/tmp/x/y"));
    REQUIRE(check(dir_body, text("/mnt/a:b")).ok());
}

TEST_CASE("plugin parameters: the path of a non-ASCII name is kept as it was given")
{
    const std::string name = base_path() + sep() + "caf\xc3\xa9";
    REQUIRE_EQ(value_of<std::string>(check(dir_body, text(name))), name);
}

// -- JSON -----------------------------------------------------------------------

TEST_CASE("plugin parameters: from_json reads an object of plain values in the order of the text")
{
    std::string problem;
    const auto read = from_json(
        R"({"b": 1, "a": "x", "c": true, "d": 1.5, "e": -3})", problem);
    REQUIRE(read.has_value());
    REQUIRE_EQ(read->size(), std::size_t{5});
    REQUIRE_EQ((*read)[0].id, std::string("b"));
    REQUIRE(std::holds_alternative<std::int64_t>((*read)[0].value));
    REQUIRE(std::holds_alternative<std::string>((*read)[1].value));
    REQUIRE(std::holds_alternative<bool>((*read)[2].value));
    REQUIRE(std::holds_alternative<double>((*read)[3].value));
    REQUIRE(std::get<std::int64_t>((*read)[4].value) == -3);
}

TEST_CASE("plugin parameters: from_json refuses what is not an object of plain values")
{
    for (const char* bad :
         {"", "   ", "[]", "[1]", "1", "\"x\"", "null", "{", "{}x", "{} {}",
          R"({"a":null})", R"({"a":[1]})", R"({"a":{"b":1}})",
          R"({"a":1,"a":2})", R"({"":1})", R"({"a":1 "b":2})",
          R"({"a":18446744073709551615})", R"({"a":1e999})",
          R"({"a\n":1})", R"({"a":"\u0001"}x)"})
    {
        std::string problem;
        const auto read = from_json(bad, problem);
        if (read.has_value())
            vss_test::raise((std::string("was accepted: ") + bad).c_str(),
                            __FILE__, __LINE__);
        REQUIRE(!problem.empty());
    }
}

TEST_CASE("plugin parameters: from_json refuses a key given twice and names it")
{
    std::string problem;
    REQUIRE(!from_json(R"({"a":1,"b":2,"a":3})", problem).has_value());
    REQUIRE(problem.find("\"a\"") != std::string::npos);
    REQUIRE(problem.find("twice") != std::string::npos);
}

TEST_CASE("plugin parameters: from_json refuses too many members, a long text and a deep one")
{
    std::string problem;
    std::string many = "{";
    for (std::size_t i = 0; i < max_parameters; ++i)
    {
        many.append(i == 0 ? "\"k" : ",\"k");
        many.append(std::to_string(i));
        many.append("\":1");
    }
    REQUIRE(from_json(many + "}", problem).has_value());
    many.append(",\"over\":1}");
    REQUIRE(!from_json(many, problem).has_value());

    const std::string longest = "{\"a\":\"" +
                                repeat('x', max_parameter_text_size) + "\"}";
    REQUIRE(from_json(longest, problem).has_value());
    const std::string too_long = "{\"a\":\"" +
                                 repeat('x', max_parameter_text_size + 1) +
                                 "\"}";
    REQUIRE(!from_json(too_long, problem).has_value());

    const std::string big = "{\"a\":\"" +
                            repeat('x', max_parameters_json_size) + "\"}";
    REQUIRE(!from_json(big, problem).has_value());
    REQUIRE(problem.find("64 KiB") != std::string::npos);

    std::string deep;
    for (int i = 0; i < 100000; ++i)
        deep.push_back('[');
    REQUIRE(!from_json(deep, problem).has_value());
    REQUIRE(!from_json("{\"a\":" + deep, problem).has_value());
    std::string nest = "{\"a\":";
    for (int i = 0; i < 50; ++i)
        nest.append("{\"a\":");
    nest.append("1");
    for (int i = 0; i < 51; ++i)
        nest.push_back('}');
    REQUIRE(!from_json(nest, problem).has_value());
}

TEST_CASE("plugin parameters: the problem of from_json never repeats the text it was given")
{
    std::string problem;
    REQUIRE(!from_json(R"({"secret-looking-key":[1]})", problem).has_value());
    REQUIRE(problem.find("secret-looking-key") == std::string::npos);
}

TEST_CASE("plugin parameters: an empty binding text is the empty set")
{
    std::string problem;
    for (const char* empty : {"", " ", "\n\t "})
    {
        const auto read = from_binding_params(empty, problem);
        REQUIRE(read.has_value());
        REQUIRE(read->empty());
    }
    REQUIRE(from_binding_params("{}", problem).has_value());
    REQUIRE(!from_binding_params("{", problem).has_value());
    REQUIRE_EQ(to_binding_params({}), std::string());
}

TEST_CASE("plugin parameters: values written for the setup read back to the same values")
{
    const std::vector<parameter> declared = declare(R"j(
        { "id": "address", "label": "A", "type": "int", "min": 1,
          "max": 247, "default": 1 },
        { "id": "delay", "label": "D", "type": "duration", "default": "3ms" },
        { "id": "mode", "label": "M", "type": "enum", "default": "b",
          "options": [ { "value": "a", "label": "A" },
                       { "value": "b", "label": "B" } ] },
        { "id": "on", "label": "O", "type": "bool", "default": true },
        { "id": "name", "label": "N", "type": "string", "default": "x" },
        { "id": "gain", "label": "G", "type": "number", "default": 1.5 },
        { "id": "blank", "label": "B", "type": "bytes", "default": "00 ff" },
        { "id": "port", "label": "P", "type": "port" },
        { "id": "link", "label": "L", "type": "link" },
        { "id": "file", "label": "F", "type": "path", "kind": "file" },
        { "id": "dir", "label": "D", "type": "path", "kind": "directory" }
    )j");
    raw_parameters values;
    values.push_back({"address", whole(17)});
    values.push_back({"delay", text("1750us")});
    values.push_back({"gain", 0.1});
    values.push_back({"blank", text("DE AD be ef")});
    values.push_back({"name", text("caf\xc3\xa9 \"quoted\" \\ back")});
    values.push_back({"port", text("Meter 1")});
    values.push_back({"link", text("A to B")});
    values.push_back({"file", text(base_path() + sep() + "f.txt")});
    values.push_back({"dir", text(base_path() + sep())});
    const validation first = validate(declared, values, value_source::setup);
    REQUIRE(first.ok());

    const std::string binding = to_binding_params(first.values);
    std::string problem;
    const auto read = from_binding_params(binding, problem);
    REQUIRE(read.has_value());
    const validation second = validate(declared, *read, value_source::setup);
    REQUIRE(second.ok());
    // What was written is now given, where some values were defaults.
    REQUIRE_EQ(second.values.size(), first.values.size());
    for (std::size_t i = 0; i < first.values.size(); ++i)
    {
        checked_parameter expected = first.values[i];
        expected.defaulted = false;
        REQUIRE(second.values[i] == expected);
    }
    REQUIRE_EQ(to_binding_params(second.values), binding);

    // The message form is the same text for a set with no secret.
    REQUIRE_EQ(to_json(first.values, value_source::user), binding);
}

TEST_CASE("plugin parameters: the text is the same for the same values, whatever the order")
{
    const std::vector<parameter> declared = declare(R"j(
        { "id": "b", "label": "B", "type": "int", "default": 2 },
        { "id": "a", "label": "A", "type": "int", "default": 1 })j");
    raw_parameters forward;
    forward.push_back({"a", whole(7)});
    forward.push_back({"b", whole(8)});
    raw_parameters backward;
    backward.push_back({"b", whole(8)});
    backward.push_back({"a", whole(7)});
    const validation first = validate(declared, forward);
    const validation second = validate(declared, backward);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE_EQ(stable_text(first.values), std::string(R"({"a":7,"b":8})"));
    REQUIRE_EQ(stable_text(first.values), stable_text(second.values));
    // A different value is a different text.
    raw_parameters other;
    other.push_back({"a", whole(7)});
    REQUIRE(stable_text(validate(declared, other).values) !=
            stable_text(first.values));
}

TEST_CASE("plugin parameters: a number that is whole keeps its type through the text")
{
    const std::vector<parameter> declared =
        one(R"j("type": "number", "default": 0)j");
    const validation first = validate(declared, given("p", 5.0));
    REQUIRE(first.ok());
    const std::string written = to_json(first.values, value_source::user);
    std::string problem;
    const auto read = from_json(written, problem);
    REQUIRE(read.has_value());
    const validation second = validate(declared, *read);
    REQUIRE(second.ok());
    REQUIRE(second.values == first.values);
    REQUIRE_EQ(value_of<double>(second), 5.0);
}

// -- the worked manifests of docs/plugins.md -----------------------------------------

TEST_CASE("plugin parameters: section 13: the Modbus fixture's settings")
{
    const std::vector<parameter> declared = declare(R"j(
        { "id": "address", "type": "int", "min": 1, "max": 247,
          "default": 1, "label": { "en": "Slave address" } },
        { "id": "delay", "type": "duration", "min": "0us", "max": "1s",
          "default": "3ms", "label": { "en": "Reply delay" } })j");
    const validation defaults = validate(declared, {});
    REQUIRE(defaults.ok());
    REQUIRE_EQ(stable_text(defaults.values),
               std::string(R"({"address":1,"delay":"3ms"})"));

    // The example binding of section 7.8: { "address": 17 }.
    std::string problem;
    const auto read = from_binding_params(R"({ "address": 17 })", problem);
    REQUIRE(read.has_value());
    const validation result = validate(declared, *read, value_source::setup);
    REQUIRE(result.ok());
    REQUIRE_EQ(to_binding_params(result.values),
               std::string(R"({"address":17,"delay":"3ms"})"));

    raw_parameters values;
    values.push_back({"address", whole(248)});
    values.push_back({"delay", text("1001ms")});
    const validation bad = validate(declared, values);
    REQUIRE_EQ(bad.errors.size(), std::size_t{2});
    REQUIRE(bad.errors[0].code == parameter_errc::above_max);
    REQUIRE(bad.errors[1].code == parameter_errc::above_max);
}

TEST_CASE("plugin parameters: section 19: the settings of the bundle sections")
{
    const std::vector<parameter> declared = declare(R"j(
        { "id": "ports", "label": "Ports", "type": "port" },
        { "id": "blank", "label": "Blank", "type": "bytes" },
        { "id": "text", "label": "Text", "type": "string",
          "max_length": 4096 })j");
    raw_parameters values;
    values.push_back({"ports", text("COM3")});
    values.push_back({"blank", text("00")});
    values.push_back({"text", text("Notes about the run")});
    const validation result = validate(declared, values);
    REQUIRE(result.ok());
    REQUIRE_EQ(to_json(result.values, value_source::user),
               std::string(R"({"blank":"00","ports":"COM3",)"
                           R"("text":"Notes about the run"})"));
    REQUIRE_EQ(validate(declared, {}).errors.size(), std::size_t{3});

    // A file to read, as the examples of sections 19 and 20 ask for.
    const std::vector<parameter> file = declare(R"j(
        { "id": "description", "label": "Protocol description",
          "type": "path", "kind": "file", "access": "read",
          "must_exist": true })j");
    raw_parameters chosen;
    chosen.push_back({"description", text(base_path() + sep() + "proto.json")});
    const validation granted = validate(file, chosen);
    REQUIRE(granted.ok());
    REQUIRE(scope_of(granted.values.front()).access == path_access::read);
    REQUIRE(scope_of(granted.values.front()).must_exist);
}

TEST_CASE("plugin parameters: section 20: the decoder's silence setting")
{
    const std::vector<parameter> declared = declare(R"j(
        { "id": "silence", "label": "Silence", "type": "duration",
          "default": "1750us" })j");
    const validation defaults = validate(declared, {});
    REQUIRE(defaults.ok());
    REQUIRE_EQ(value_of<std::int64_t>(defaults), 1'750'000);
    REQUIRE_EQ(stable_text(defaults.values),
               std::string(R"({"silence":"1750us"})"));
    const validation longer = validate(declared, given("silence", text("2ms")));
    REQUIRE_EQ(value_of<std::int64_t>(longer), 2'000'000);
}
