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

#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/signature.hpp>

#include <boost/json.hpp>

#include "child_process.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
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
    REQUIRE(has_line(lines,
                     "  Signature:    Not signed. Studio cannot tell who "
                     "made it or whether it was changed."));
    REQUIRE(has_line(lines, "  Trust:        Unsigned"));
    REQUIRE(has_line(lines, "  Key:          none"));
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

TEST_CASE("plugin cli: a signature file that does not hold is shown as such")
{
    const scratch_directory scratch;
    const fs::path signed_one =
        make_plugin(scratch.path(), "signed", "com.example.signed");
    write_file(signed_one / "plugin.sig", "not a real signature");

    const cli_result text =
        run_cli({"list", "--root", scratch.path().string()});
    REQUIRE_EQ(text.code.value_or(-1), 0);
    const auto signature =
        line_starting(lines_of(text.output), "  Signature:    ", 0);
    REQUIRE(signature.has_value());
    REQUIRE(signature.value().rfind(
                "  Signature:    The signature does not hold: ", 0) == 0);

    const cli_result as_json =
        run_cli({"list", "--json", "--root", scratch.path().string()});
    REQUIRE_EQ(as_json.code.value_or(-1), 0);
    const json::value top = parse_output(as_json.output);
    const json::object& item =
        top.as_object().at("plugins").as_array()[0].as_object();
    REQUIRE_EQ(text_of(item.at("signature")), std::string("present"));
    // The plugin is still found, and the verdict says the signature fails.
    REQUIRE_EQ(text_of(item.at("state")), std::string("found"));
    REQUIRE_EQ(text_of(item.at("signature_state")), std::string("invalid"));
    REQUIRE(item.at("trust").is_null());
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

// -- verify, install and remove ---------------------------------------------

namespace
{

/// A key nobody on a test computer knows.  THROWAWAY TEST KEY: a private
/// key made of one repeated byte, written down here for anyone to read.
constexpr std::uint8_t stranger_fill = 0x05;

std::string listed_manifest(std::string_view id, std::string_view program)
{
    std::string out = manifest_text(id);
    // Replace the closing brace with a list of the files.
    const std::size_t close = out.rfind('}');
    out.erase(close);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    out += ",\n  \"files\": [ { \"path\": \"bin/prog\", \"size\": ";
    out += std::to_string(program.size());
    out += ", \"sha512\": \"";
    out += vss::plugin::to_hex(vss::plugin::sha512_of(program));
    out += "\" } ]\n}\n";
    return out;
}

/// A plugin that lists its program and is signed with a key that the test
/// computer does not know, so that it is signed with an unknown key.
fs::path make_signed_plugin(const fs::path& parent, std::string_view folder,
                            std::string_view id,
                            std::string_view program = "program bytes")
{
    const fs::path directory = parent / std::string(folder);
    const std::string manifest = listed_manifest(id, program);
    write_file(directory / "plugin.json", manifest);
    write_file(directory / "bin" / "prog", program);

    std::string claim = "{ \"plugin_id\": \"";
    claim += id;
    claim += "\", \"version\": \"1.2.0\", \"manifest_sha512\": \"";
    claim += vss::plugin::to_hex(vss::plugin::sha512_of(manifest));
    claim += "\" }";
    std::vector<std::uint8_t> payload(claim.begin(), claim.end());
    vss::license::signing::seed seed{};
    seed.fill(stranger_fill);
    write_file(directory / "plugin.sig",
               vss::license::signed_file::write(vss::license::signing::sign(
                   vss::plugin::plugin_purpose, "nobody-1", payload, seed)));
    return directory;
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
    std::sort(names.begin(), names.end());
    return names;
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("plugin cli: verify says what the checks found and exits 0 for a "
          "plugin that can be installed")
{
    const scratch_directory scratch;
    const fs::path folder =
        make_signed_plugin(scratch.path(), "signed", "com.example.signed");

    // A key nobody approved proves nothing about who made the plugin, so the
    // plugin installs only with --allow-unsigned, and the answer says which
    // it is.
    const cli_result refused = run_cli({"verify", folder.string()});
    REQUIRE_EQ(refused.code.value_or(-1), 1);
    const auto why = line_starting(lines_of(refused.output),
                                   "  Install:      ", 0);
    REQUIRE(why.has_value());
    REQUIRE(why.value().find("--allow-unsigned") != std::string::npos);
    REQUIRE(why.value().find("signed with a key nobody has approved") !=
            std::string::npos);

    const cli_result result =
        run_cli({"verify", "--allow-unsigned", folder.string()});
    REQUIRE_EQ(result.code.value_or(-1), 0);
    const std::vector<std::string> lines = lines_of(result.output);
    REQUIRE(has_line(lines, "com.example.signed  1.2.0  Modbus fixture"));
    REQUIRE(has_line(lines, "  Folder:       " + shown(folder)));
    REQUIRE(has_line(lines, "  Trust:        Signed by an author you have not "
                            "approved"));
    REQUIRE(has_line(lines, "  Key:          nobody-1"));
    REQUIRE(has_line(lines, "  Licence:      none needed"));
    REQUIRE(has_line(lines, "  Install:      can be installed"));
    const auto may_run = line_starting(lines, "  May run:      no, ", 0);
    REQUIRE(may_run.has_value());
    REQUIRE_EQ(lines.back(), std::string("This build does not run plugins."));

    const cli_result as_json =
        run_cli({"verify", "--json", "--allow-unsigned", folder.string()});
    REQUIRE_EQ(as_json.code.value_or(-1), 0);
    const json::object item = parse_output(as_json.output).as_object();
    REQUIRE_EQ(text_of(item.at("id")), std::string("com.example.signed"));
    REQUIRE_EQ(text_of(item.at("trust")), std::string("signed_unknown_key"));
    REQUIRE_EQ(text_of(item.at("signature_state")),
               std::string("unknown_key"));
    REQUIRE_EQ(text_of(item.at("key_id")), std::string("nobody-1"));
    REQUIRE(item.at("installable").as_bool());
    REQUIRE(item.at("problem").is_null());
    REQUIRE(!item.at("may_run").as_bool());
    REQUIRE(!text_of(item.at("run_reason")).empty());
    REQUIRE(!item.at("licence").as_object().at("checked").as_bool());
}

TEST_CASE("plugin cli: verify exits 1 for an unsigned plugin unless "
          "--allow-unsigned is given")
{
    const scratch_directory scratch;
    const fs::path folder =
        make_plugin(scratch.path(), "plain", "com.example.plain");

    const cli_result refused = run_cli({"verify", folder.string()});
    REQUIRE_EQ(refused.code.value_or(-1), 1);
    REQUIRE(has_line(lines_of(refused.output), "  Trust:        Unsigned"));
    const auto install = line_starting(lines_of(refused.output),
                                       "  Install:      ", 0);
    REQUIRE(install.has_value());
    REQUIRE(install.value().find("--allow-unsigned") != std::string::npos);

    const cli_result allowed =
        run_cli({"verify", "--allow-unsigned", folder.string()});
    REQUIRE_EQ(allowed.code.value_or(-1), 0);
}

TEST_CASE("plugin cli: verify exits 1 for an id that install would refuse")
{
    const scratch_directory scratch;
    const fs::path folder =
        make_plugin(scratch.path(), "device", "con.example");

    const cli_result result =
        run_cli({"verify", "--allow-unsigned", folder.string()});
    REQUIRE_EQ(result.code.value_or(-1), 1);
    const auto install = line_starting(lines_of(result.output),
                                       "  Install:      ", 0);
    REQUIRE(install.has_value());
    REQUIRE(install.value().find("layout_invalid") != std::string::npos);

    const fs::path root = scratch.path() / "installed";
    const cli_result refused = run_cli({"install", folder.string(),
                                        "--allow-unsigned", "--root",
                                        root.string()});
    REQUIRE_EQ(refused.code.value_or(-1), 1);
    REQUIRE(names_in(root).empty());
}

TEST_CASE("plugin cli: verify exits 1 for a plugin whose file was changed")
{
    const scratch_directory scratch;
    const fs::path folder =
        make_signed_plugin(scratch.path(), "signed", "com.example.signed");
    write_file(folder / "bin" / "prog", "program bytez");

    const cli_result result = run_cli({"verify", "--json", folder.string()});
    REQUIRE_EQ(result.code.value_or(-1), 1);
    const json::object item = parse_output(result.output).as_object();
    REQUIRE_EQ(text_of(item.at("problem")), std::string("signature_invalid"));
    REQUIRE(!item.at("installable").as_bool());
    REQUIRE(item.at("trust").is_null());

    const cli_result text = run_cli({"verify", folder.string()});
    REQUIRE_EQ(text.code.value_or(-1), 1);
    const auto problem =
        line_starting(lines_of(text.output), "  Problem:      ", 0);
    REQUIRE(problem.has_value());
    REQUIRE(problem.value().find("bin/prog") != std::string::npos);
}

TEST_CASE("plugin cli: verify, install and remove need their arguments")
{
    REQUIRE_EQ(run_cli({"verify"}).code.value_or(-1), 2);
    REQUIRE_EQ(run_cli({"install"}).code.value_or(-1), 2);
    REQUIRE_EQ(run_cli({"remove"}).code.value_or(-1), 2);
    REQUIRE_EQ(run_cli({"verify", "a", "b"}).code.value_or(-1), 2);
    REQUIRE_EQ(run_cli({"verify", "--replace", "a"}).code.value_or(-1), 2);
    REQUIRE_EQ(run_cli({"install", "--json", "a"}).code.value_or(-1), 2);
    REQUIRE_EQ(run_cli({"install", "a", "--root"}).code.value_or(-1), 2);
    REQUIRE_EQ(run_cli({"remove", "a", "--replace"}).code.value_or(-1), 2);
}

TEST_CASE("plugin cli: an installed plugin is listed with its verdict")
{
    const scratch_directory scratch;
    const fs::path source =
        make_signed_plugin(scratch.path(), "source", "com.example.signed");
    const fs::path root = scratch.path() / "installed";

    const cli_result without =
        run_cli({"install", source.string(), "--root", root.string()});
    REQUIRE_EQ(without.code.value_or(-1), 1);
    REQUIRE(without.output.find("key_unknown") != std::string::npos);
    REQUIRE(names_in(root).empty());

    const cli_result done = run_cli({"install", source.string(),
                                     "--allow-unsigned", "--root",
                                     root.string()});
    REQUIRE_EQ(done.code.value_or(-1), 0);
    const std::vector<std::string> lines = lines_of(done.output);
    REQUIRE(has_line(lines, "Installed com.example.signed  1.2.0  Modbus "
                            "fixture"));
    REQUIRE(has_line(lines, "  Folder:       " +
                                shown(root / "com.example.signed")));
    REQUIRE(has_line(lines, "  Trust:        Signed by an author you have "
                            "not approved"));
    REQUIRE_EQ(lines.back(), std::string("This build does not run plugins."));
    REQUIRE_EQ(read_file(root / "com.example.signed" / "bin" / "prog"),
               std::string("program bytes"));
    REQUIRE_EQ(names_in(root).size(), std::size_t{1});

    const cli_result listed =
        run_cli({"list", "--json", "--root", root.string()});
    REQUIRE_EQ(listed.code.value_or(-1), 0);
    const json::value listed_top = parse_output(listed.output);
    const json::array& plugins =
        listed_top.as_object().at("plugins").as_array();
    REQUIRE_EQ(plugins.size(), std::size_t{1});
    const json::object& item = plugins[0].as_object();
    REQUIRE_EQ(text_of(item.at("id")), std::string("com.example.signed"));
    REQUIRE_EQ(text_of(item.at("state")), std::string("found"));
    REQUIRE_EQ(text_of(item.at("signature")), std::string("present"));
    REQUIRE_EQ(text_of(item.at("signature_state")),
               std::string("unknown_key"));
    REQUIRE_EQ(text_of(item.at("trust")), std::string("signed_unknown_key"));
    REQUIRE_EQ(text_of(item.at("key_id")), std::string("nobody-1"));
    REQUIRE(!item.at("may_run").as_bool());

    const cli_result text = run_cli({"list", "--root", root.string()});
    const std::vector<std::string> list_lines = lines_of(text.output);
    REQUIRE(has_line(list_lines, "  Trust:        Signed by an author you "
                                 "have not approved"));
    REQUIRE(has_line(list_lines, "  Key:          nobody-1"));
    REQUIRE_EQ(list_lines.back(),
               std::string("1 plugin found. This build does not run "
                           "plugins."));
}

TEST_CASE("plugin cli: install refuses a changed folder and writes nothing")
{
    const scratch_directory scratch;
    const fs::path source =
        make_signed_plugin(scratch.path(), "source", "com.example.signed");
    write_file(source / "bin" / "prog", "program bytez");
    const fs::path root = scratch.path() / "installed";

    const cli_result result = run_cli({"install", source.string(),
                                       "--allow-unsigned", "--root",
                                       root.string()});
    REQUIRE_EQ(result.code.value_or(-1), 1);
    REQUIRE(result.output.find("signature_invalid") != std::string::npos);
    REQUIRE(names_in(root).empty());
}

TEST_CASE("plugin cli: install needs --allow-unsigned for an unsigned plugin, "
          "--replace for a second copy")
{
    const scratch_directory scratch;
    const fs::path source =
        make_plugin(scratch.path(), "source", "com.example.plain");
    const fs::path root = scratch.path() / "installed";

    const cli_result refused =
        run_cli({"install", source.string(), "--root", root.string()});
    REQUIRE_EQ(refused.code.value_or(-1), 1);
    REQUIRE(refused.output.find("signature_missing") != std::string::npos);
    REQUIRE(names_in(root).empty());

    const cli_result done = run_cli({"install", source.string(),
                                     "--allow-unsigned", "--root",
                                     root.string()});
    REQUIRE_EQ(done.code.value_or(-1), 0);
    REQUIRE(has_line(lines_of(done.output), "  Trust:        Unsigned"));

    const cli_result again = run_cli({"install", source.string(),
                                      "--allow-unsigned", "--root",
                                      root.string()});
    REQUIRE_EQ(again.code.value_or(-1), 1);
    REQUIRE(again.output.find("already installed") != std::string::npos);

    const cli_result replaced =
        run_cli({"install", source.string(), "--allow-unsigned", "--replace",
                 "--root", root.string()});
    REQUIRE_EQ(replaced.code.value_or(-1), 0);
    REQUIRE(has_line(lines_of(replaced.output),
                     "Replaced com.example.plain  1.2.0  Modbus fixture"));
    REQUIRE_EQ(names_in(root).size(), std::size_t{1});
}

TEST_CASE("plugin cli: install --for-everyone refuses a folder that others "
          "can write to")
{
    const scratch_directory scratch;
    const fs::path source =
        make_signed_plugin(scratch.path(), "source", "com.example.signed");
    const fs::path root = scratch.path() / "machine";

    const cli_result result =
        run_cli({"install", source.string(), "--allow-unsigned",
                 "--for-everyone", "--root", root.string()});
    REQUIRE_EQ(result.code.value_or(-1), 1);
    REQUIRE(result.output.find("administrator") != std::string::npos);
    // The folder is not made.
    REQUIRE(!fs::exists(root));

    const cli_result removal = run_cli({"remove", "com.example.signed",
                                        "--for-everyone", "--root",
                                        root.string()});
    REQUIRE_EQ(removal.code.value_or(-1), 1);
    REQUIRE(removal.output.find("administrator") != std::string::npos);
    REQUIRE(!fs::exists(root));
}

TEST_CASE("plugin cli: remove takes a plugin out and keeps the others")
{
    const scratch_directory scratch;
    const fs::path root = scratch.path() / "installed";
    const fs::path one =
        make_signed_plugin(scratch.path(), "one", "com.example.one");
    const fs::path two =
        make_signed_plugin(scratch.path(), "two", "com.example.two");
    REQUIRE_EQ(run_cli({"install", one.string(), "--allow-unsigned", "--root",
                        root.string()})
                   .code.value_or(-1),
               0);
    REQUIRE_EQ(run_cli({"install", two.string(), "--allow-unsigned", "--root",
                        root.string()})
                   .code.value_or(-1),
               0);

    const cli_result removed =
        run_cli({"remove", "com.example.one", "--root", root.string()});
    REQUIRE_EQ(removed.code.value_or(-1), 0);
    REQUIRE(removed.output.find("Removed com.example.one") !=
            std::string::npos);
    const std::vector<std::string> names = names_in(root);
    REQUIRE_EQ(names.size(), std::size_t{1});
    REQUIRE_EQ(names[0], std::string("com.example.two"));

    REQUIRE_EQ(
        run_cli({"remove", "com.example.one", "--root", root.string()})
            .code.value_or(-1),
        1);
    REQUIRE_EQ(run_cli({"remove", "../two", "--root", root.string()})
                   .code.value_or(-1),
               1);
    REQUIRE_EQ(names_in(root).size(), std::size_t{1});
}
