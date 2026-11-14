//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli plugin list` as a separate process, the way a script uses it: it
// is pointed at folders a test made, and the exit code, the text and the JSON
// are read back.  It starts no plugin, so the plugin folders hold only text
// and a file that stands for the program.
//

#include <boost/json.hpp>

#include "child_process.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using vss_test_support::child_process;
using vss_test_support::scratch_directory;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr std::chrono::milliseconds long_limit{20000};

/// How the command ended.
struct cli_result
{
    std::optional<int> code;
    std::string output;
};

cli_result run_cli(const std::vector<std::string>& words)
{
    std::vector<std::string> arguments{"plugin"};
    arguments.insert(arguments.end(), words.begin(), words.end());
    child_process child(VSS_CLI_PROGRAM, arguments);
    cli_result result;
    result.output = child.read_all(long_limit);
    result.code = child.wait(long_limit);
    return result;
}

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

/// A `plugin.json` that Studio reads: it runs `bin/prog`.
std::string manifest_text(std::string_view id)
{
    std::string out = "{\n";
    out += "  \"manifest_version\": 1,\n";
    out += "  \"id\": \"";
    out += id;
    out += "\",\n";
    out += "  \"name\": \"Modbus fixture\",\n";
    out += "  \"version\": \"1.2.0\",\n";
    out += "  \"requires\": { \"api\": { \"min\": \"1.0\", \"max\": \"1.x\" } "
           "},\n";
    out += "  \"run\": { \"executable\": \"bin/prog\", ";
    out += "\"platforms\": [\"linux-x64\"] },\n";
    out += "  \"extension_points\": [\n";
    out += "    { \"kind\": \"fixture\", \"id\": \"meter\", ";
    out += "\"name\": \"Meter\" }\n";
    out += "  ]\n}\n";
    return out;
}

/// A plugin folder `parent/folder` that discovery finds no problem with.
fs::path make_plugin(const fs::path& parent, std::string_view folder,
                     std::string_view id)
{
    const fs::path directory = parent / std::string(folder);
    write_file(directory / "plugin.json", manifest_text(id));
    write_file(directory / "bin" / "prog", "program bytes");
    return directory;
}

/// A plugin folder whose `plugin.json` is not JSON.
fs::path make_broken_plugin(const fs::path& parent, const std::string& folder)
{
    const fs::path directory = parent / folder;
    write_file(directory / "plugin.json", "{ this is not JSON");
    return directory;
}

/// The path as the command prints it: `/` between the names.
std::string shown(const fs::path& path)
{
    return path.generic_string();
}

/// The lines of `text`, without their line endings.
std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(current);
            current.clear();
        }
        else if (c != '\r')
        {
            current += c;
        }
    }
    if (!current.empty())
        lines.push_back(current);
    return lines;
}

bool has_line(const std::vector<std::string>& lines, const std::string& line)
{
    for (const std::string& each : lines)
    {
        if (each == line)
            return true;
    }
    return false;
}

/// The first line that starts with `prefix` after line number `from`, or
/// nothing.
std::optional<std::string> line_starting(const std::vector<std::string>& lines,
                                         const std::string& prefix,
                                         std::size_t from)
{
    for (std::size_t index = from; index < lines.size(); ++index)
    {
        if (lines[index].rfind(prefix, 0) == 0)
            return lines[index];
    }
    return std::nullopt;
}

std::size_t index_of(const std::vector<std::string>& lines,
                     const std::string& line)
{
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (lines[index] == line)
            return index;
    }
    throw vss_test::failure("the output has no line: " + line);
}

std::string text_of(const json::value& value)
{
    REQUIRE(value.is_string());
    const json::string& text = value.as_string();
    return std::string(text.data(), text.size());
}

json::value parse_output(const std::string& output)
{
    try
    {
        return json::parse(output);
    }
    catch (const std::exception& error)
    {
        throw vss_test::failure(std::string("the output is not JSON: ") +
                                error.what() + "\n" + output);
    }
}

} // namespace

TEST_CASE("plugin cli: list prints a block for each plugin")
{
    const scratch_directory scratch;
    const fs::path good =
        make_plugin(scratch.path(), "modbus-fixture", "com.example.modbus");
    make_broken_plugin(scratch.path(), "broken");

    const cli_result result =
        run_cli({"list", "--root", scratch.path().string()});
    REQUIRE(result.code.has_value());
    REQUIRE_EQ(result.code.value_or(-1), 0);

    const std::vector<std::string> lines = lines_of(result.output);
    REQUIRE(lines.size() > 10);
    // The first line says what kind of install this is.  Which one depends
    // on the folder the test program is in.
    const bool installed =
        lines[0].rfind("Studio is installed for ", 0) == 0 ||
        lines[0].rfind("Studio's own folder could not be examined: ", 0) == 0;
    REQUIRE(installed);
    REQUIRE_EQ(lines[1], std::string("Policy: not read, because --root was "
                                     "given."));

    // The folders are in the order of their names: "broken" first.
    const std::size_t broken = index_of(lines, "broken");
    const std::size_t found =
        index_of(lines, "com.example.modbus  1.2.0  Modbus fixture");
    REQUIRE(broken < found);

    const std::string folder = shown(good);
    REQUIRE(has_line(lines, "  Folder:       " + folder));
    REQUIRE(has_line(lines, "  Installed for: you only"));
    REQUIRE(has_line(lines, "  Signature:    not signed"));
    REQUIRE(has_line(lines, "  State:        found"));

    const auto print = line_starting(lines, "  Fingerprint:  ", found);
    REQUIRE(print.has_value());
    const std::string digits = print.value().substr(16);
    REQUIRE_EQ(digits.size(), std::size_t{16});
    REQUIRE(digits.find_first_not_of("0123456789abcdef") == std::string::npos);

    // The plugin with the broken manifest is shown by its folder, with the
    // reason code and a line that says what is wrong.
    const auto state = line_starting(lines, "  State:        ", broken);
    REQUIRE(state.has_value());
    REQUIRE(state.value().rfind("  State:        manifest_invalid: ", 0) == 0);
    REQUIRE(state.value().size() > 32);
    const auto missing = line_starting(lines, "  Fingerprint:  ", broken);
    REQUIRE(missing.has_value());
    REQUIRE_EQ(missing.value(), std::string("  Fingerprint:  not worked out"));

    REQUIRE_EQ(lines.back(),
               std::string("2 plugins found. This build does not run "
                           "plugins."));
}

TEST_CASE("plugin cli: list --json prints one JSON object")
{
    const scratch_directory scratch;
    const fs::path good =
        make_plugin(scratch.path(), "modbus-fixture", "com.example.modbus");
    make_broken_plugin(scratch.path(), "broken");

    const cli_result result =
        run_cli({"list", "--json", "--root", scratch.path().string()});
    REQUIRE(result.code.has_value());
    REQUIRE_EQ(result.code.value_or(-1), 0);

    const json::value top = parse_output(result.output);
    REQUIRE(top.is_object());
    const json::object& object = top.as_object();
    REQUIRE_EQ(object.size(), std::size_t{4});

    const json::value& install = object.at("install");
    REQUIRE(install.is_null() ||
            text_of(install) == "for_everyone" ||
            text_of(install) == "for_one_user");

    const json::object& policy = object.at("policy").as_object();
    REQUIRE(policy.at("file").is_null());
    REQUIRE(!policy.at("present").as_bool());
    REQUIRE(!policy.at("used").as_bool());

    const json::array& plugins = object.at("plugins").as_array();
    REQUIRE_EQ(plugins.size(), std::size_t{2});

    const json::object& broken = plugins[0].as_object();
    REQUIRE(broken.at("id").is_null());
    REQUIRE(broken.at("version").is_null());
    REQUIRE(broken.at("name").is_null());
    REQUIRE_EQ(text_of(broken.at("state")), std::string("manifest_invalid"));
    REQUIRE(!text_of(broken.at("detail")).empty());
    REQUIRE_EQ(text_of(broken.at("scope")), std::string("for_user"));
    REQUIRE_EQ(text_of(broken.at("signature")), std::string("none"));
    REQUIRE(broken.at("fingerprint").is_null());
    REQUIRE(broken.at("shadowed_by").is_null());

    const json::object& fine = plugins[1].as_object();
    REQUIRE_EQ(text_of(fine.at("id")), std::string("com.example.modbus"));
    REQUIRE_EQ(text_of(fine.at("version")), std::string("1.2.0"));
    REQUIRE_EQ(text_of(fine.at("name")), std::string("Modbus fixture"));
    REQUIRE_EQ(text_of(fine.at("folder")), shown(good));
    REQUIRE_EQ(text_of(fine.at("state")), std::string("found"));
    REQUIRE_EQ(text_of(fine.at("detail")), std::string());
    REQUIRE_EQ(text_of(fine.at("signature")), std::string("none"));
    REQUIRE_EQ(text_of(fine.at("fingerprint")).size(), std::size_t{16});
    REQUIRE(fine.at("shadowed_by").is_null());

    REQUIRE(object.at("roots_not_read").as_array().empty());
}

TEST_CASE("plugin cli: a signature file is shown and is not checked")
{
    const scratch_directory scratch;
    const fs::path signed_one =
        make_plugin(scratch.path(), "signed", "com.example.signed");
    write_file(signed_one / "plugin.sig", "not a real signature");

    const cli_result text =
        run_cli({"list", "--root", scratch.path().string()});
    REQUIRE_EQ(text.code.value_or(-1), 0);
    REQUIRE(has_line(lines_of(text.output),
                     "  Signature:    present, not checked by this build"));

    const cli_result as_json =
        run_cli({"list", "--json", "--root", scratch.path().string()});
    REQUIRE_EQ(as_json.code.value_or(-1), 0);
    const json::value top = parse_output(as_json.output);
    const json::object& item =
        top.as_object().at("plugins").as_array()[0].as_object();
    REQUIRE_EQ(text_of(item.at("signature")), std::string("present"));
    // The plugin is still found: the command says nothing about the key.
    REQUIRE_EQ(text_of(item.at("state")), std::string("found"));
}

TEST_CASE("plugin cli: the first root wins and the second copy is shadowed")
{
    const scratch_directory scratch;
    const fs::path first = scratch.path() / "first";
    const fs::path second = scratch.path() / "second";
    const fs::path winner = make_plugin(first, "one", "com.example.same");
    make_plugin(second, "two", "com.example.same");

    const cli_result text = run_cli(
        {"list", "--root", first.string(), "--root", second.string()});
    REQUIRE_EQ(text.code.value_or(-1), 0);
    const std::vector<std::string> lines = lines_of(text.output);
    REQUIRE(line_starting(lines, "  State:        shadowed", 0).has_value());
    REQUIRE(has_line(lines, "  Not used:     the copy in " + shown(winner) +
                                " is used"));
    REQUIRE_EQ(lines.back(),
               std::string("2 plugins found. This build does not run "
                           "plugins."));

    const cli_result as_json = run_cli({"list", "--json", "--root",
                                        first.string(), "--root",
                                        second.string()});
    REQUIRE_EQ(as_json.code.value_or(-1), 0);
    const json::value top = parse_output(as_json.output);
    const json::array& plugins = top.as_object().at("plugins").as_array();
    REQUIRE_EQ(plugins.size(), std::size_t{2});
    REQUIRE_EQ(text_of(plugins[0].as_object().at("state")),
               std::string("found"));
    REQUIRE_EQ(text_of(plugins[1].as_object().at("state")),
               std::string("shadowed"));
    REQUIRE_EQ(plugins[1].as_object().at("shadowed_by").as_int64(), 0);
}

TEST_CASE("plugin cli: a folder with no plugins, and one that is not there")
{
    const scratch_directory scratch;
    const fs::path empty = scratch.path() / "empty";
    fs::create_directories(empty);

    const cli_result none = run_cli({"list", "--root", empty.string()});
    REQUIRE_EQ(none.code.value_or(-1), 0);
    REQUIRE_EQ(lines_of(none.output).back(),
               std::string("No plugins found. This build does not run "
                           "plugins."));

    // A folder that is not there is passed over without a word.
    const cli_result absent = run_cli(
        {"list", "--root=" + (scratch.path() / "missing").string()});
    REQUIRE_EQ(absent.code.value_or(-1), 0);
    REQUIRE_EQ(lines_of(absent.output).back(),
               std::string("No plugins found. This build does not run "
                           "plugins."));
}

TEST_CASE("plugin cli: a root that cannot be looked through is reported")
{
    const scratch_directory scratch;
    // A file where a folder is expected.
    const fs::path file = scratch.path() / "not-a-folder";
    write_file(file, "text");
    make_plugin(scratch.path() / "real", "one", "com.example.one");

    const cli_result text = run_cli({"list", "--root", file.string(), "--root",
                                     (scratch.path() / "real").string()});
    REQUIRE_EQ(text.code.value_or(-1), 0);
    const std::vector<std::string> lines = lines_of(text.output);
    const auto problem =
        line_starting(lines, "Folder " + shown(file) + " was not looked "
                             "through: ", 0);
    REQUIRE(problem.has_value());
    REQUIRE_EQ(lines.back(),
               std::string("1 plugin found. This build does not run plugins."));

    const cli_result as_json =
        run_cli({"list", "--json", "--root", file.string(), "--root",
                 (scratch.path() / "real").string()});
    REQUIRE_EQ(as_json.code.value_or(-1), 0);
    const json::value top = parse_output(as_json.output);
    const json::array& roots = top.as_object().at("roots_not_read").as_array();
    REQUIRE_EQ(roots.size(), std::size_t{1});
    REQUIRE_EQ(text_of(roots[0].as_object().at("folder")), shown(file));
    REQUIRE(!text_of(roots[0].as_object().at("detail")).empty());
    REQUIRE_EQ(top.as_object().at("plugins").as_array().size(), std::size_t{1});
}

TEST_CASE("plugin cli: text from a plugin cannot move the cursor")
{
    const scratch_directory scratch;
    // A folder name with an escape character.  Windows does not allow one.
    const std::string name = std::string("bad") + '\x1b' + "[31mname";
    const fs::path directory = scratch.path() / name;
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec)
        VSS_SKIP("this system does not allow a control character in a name");
    write_file(directory / "plugin.json", "{ not JSON");

    const cli_result text =
        run_cli({"list", "--root", scratch.path().string()});
    REQUIRE_EQ(text.code.value_or(-1), 0);
    REQUIRE(text.output.find('\x1b') == std::string::npos);
    REQUIRE(has_line(lines_of(text.output), "bad?[31mname"));

    const cli_result as_json =
        run_cli({"list", "--json", "--root", scratch.path().string()});
    REQUIRE_EQ(as_json.code.value_or(-1), 0);
    // The JSON text has the character as an escape, which is plain text.
    REQUIRE(as_json.output.find('\x1b') == std::string::npos);
    const json::value top = parse_output(as_json.output);
    const json::array& plugins = top.as_object().at("plugins").as_array();
    REQUIRE_EQ(plugins.size(), std::size_t{1});
    REQUIRE_EQ(text_of(plugins[0].as_object().at("folder")), shown(directory));
}

TEST_CASE("plugin cli: a command line it does not understand exits with 2")
{
    const std::vector<std::vector<std::string>> mistakes = {
        {},
        {"frobnicate"},
        {"list", "--bogus"},
        {"list", "stray"},
        {"list", "--root"},
        {"list", "--root="},
    };
    for (const std::vector<std::string>& words : mistakes)
    {
        const cli_result result = run_cli(words);
        REQUIRE(result.code.has_value());
        REQUIRE_EQ(result.code.value_or(-1), 2);
        // The usage is printed, and nothing that looks like a result.
        REQUIRE(result.output.find("vss_cli plugin list") !=
                std::string::npos);
        REQUIRE(result.output.find("This build does not run") ==
                std::string::npos);
    }
}

TEST_CASE("plugin cli: list with no --root reads the usual folders")
{
    // What is in them depends on the computer, and so does the install kind,
    // so only the shape is checked: the command ran, and the JSON has all
    // four members.
    const cli_result result = run_cli({"list", "--json"});
    REQUIRE_EQ(result.code.value_or(-1), 0);
    const json::value top = parse_output(result.output);
    const json::object& object = top.as_object();
    REQUIRE(object.contains("install"));
    REQUIRE(object.contains("policy"));
    REQUIRE(object.at("plugins").is_array());
    REQUIRE(object.at("roots_not_read").is_array());
    const json::object& policy = object.at("policy").as_object();
    REQUIRE(policy.at("present").is_bool());
    REQUIRE(policy.at("used").is_bool());
}
