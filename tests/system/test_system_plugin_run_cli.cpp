//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli plugin run` as a separate process, the way a script uses it.  A
// test writes an unsigned plugin folder into a scratch directory: the test
// plugin program (VSS_TEST_PLUGIN_PROGRAM) copied into it, and a manifest that
// lists its files and runs `vss_test_plugin --serve`.  A trust file that holds
// a decision for that exact copy stands in for the user's answer in Studio,
// which the command never gives.  Where this computer cannot confine a
// plugin as far as the minimum, the decision says to run it without isolation,
// as the service tests do.
//

#include <vss/plugin/discover.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include <boost/json.hpp>

#include "child_process.hpp"
#include "native_io.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"
#include "test_plugin_files.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using vss::plugin::permission;
using vss::plugin::plugin_decision;
using vss::plugin::plugin_root;
using vss::plugin::plugin_scope;
using vss::plugin::sandbox_level;
using vss::plugin::sha512_of;
using vss::plugin::to_hex;
using vss::plugin::trust_level;
using vss::plugin::host::available_sandbox;
using vss_test_support::child_process;
using vss_test_support::copy_test_plugin;
using vss_test_support::scratch_directory;
using vss_test_support::test_plugin_files;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;
namespace io = vss_test_support;
namespace json = boost::json;

using steady = std::chrono::steady_clock;

/// How long any wait lasts before the test gives up.
constexpr std::chrono::milliseconds long_limit{120000};

constexpr std::string_view plugin_id = "com.example.runcli";

// -- files ------------------------------------------------------------------

void write_file(const fs::path& path, std::string_view content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
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

/// What one file of every test plugin is: the test plugin as it was built.
struct program_facts
{
    std::string name;
    std::uint64_t size = 0;
    std::string sha512;
};

/// The files every test plugin has from the build, the program first.  In a
/// build that needs a runtime library beside the program, that is listed too.
const std::vector<program_facts>& test_plugin_facts()
{
    static const std::vector<program_facts> facts = []
    {
        std::vector<program_facts> all;
        for (const fs::path& source : test_plugin_files(
                 fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM)),
                 VSS_TEST_PLUGIN_RUNTIME))
        {
            const std::string bytes = read_file(source);
            program_facts out;
            out.name = source.filename().string();
            out.size = bytes.size();
            out.sha512 = to_hex(sha512_of(bytes));
            all.push_back(std::move(out));
        }
        REQUIRE(!all.empty());
        return all;
    }();
    return facts;
}

/// What a test plugin is made of.
struct plugin_spec
{
    std::string id{plugin_id};
    /// The name, as UTF-8.
    std::string name = "Run command plugin";
    /// The arguments the manifest gives the program before `--serve`, which
    /// the program needs to be last.
    std::vector<std::string> arguments{};
    /// The extension points of the manifest, each as the text of a JSON
    /// object.  The manifest declares the generator `pulse` when there are
    /// none.
    std::vector<std::string> points{};
};

/// `text` as a JSON string, quotes included.
std::string json_text(std::string_view text)
{
    return json::serialize(
        json::value(json::string(text.data(), text.size())));
}

/// The `plugin.json` of a test plugin.  The plugin asks for one permission it
/// must have and one it may do without, and lists every file but the one the
/// manifest never lists.  Its program says the id and version of this
/// manifest in "hello".
std::string manifest_text(const plugin_spec& spec)
{
    std::string out;
    out.append("{\n  \"manifest_version\": 1,\n  \"id\": ");
    out.append(json_text(spec.id));
    out.append(",\n  \"name\": ");
    out.append(json_text(spec.name));
    out.append(",\n  \"version\": \"1.2.0\",\n");
    out.append("  \"author\": { \"name\": \"Example Ltd\" },\n");
    out.append("  \"requires\": { \"api\": { \"min\": \"1.0\", ");
    out.append("\"max\": \"1.x\" } },\n");
    out.append("  \"run\": { \"executable\": ");
    out.append(json_text(test_plugin_facts().front().name));
    out.append(", \"args\": [ ");
    out.append(json_text("--hello-id=" + spec.id));
    out.append(", \"--hello-version=1.2.0\"");
    for (const std::string& argument : spec.arguments)
    {
        out.append(", ");
        out.append(json_text(argument));
    }
    out.append(", \"--serve\" ],\n");
    out.append("    \"platforms\": [ \"windows-x64\", \"windows-arm64\", ");
    out.append("\"linux-x64\", \"linux-arm64\", \"macos-x64\", ");
    out.append("\"macos-arm64\" ] },\n");
    out.append("  \"permissions\": [\n");
    out.append("    { \"name\": \"ports.read\", \"required\": true, ");
    out.append("\"reason\": \"To read the ports.\" },\n");
    out.append("    { \"name\": \"ports.write\", \"required\": false, ");
    out.append("\"reason\": \"To write to the ports.\" }\n  ],\n");
    out.append("  \"extension_points\": [\n");
    if (spec.points.empty())
    {
        out.append("    { \"kind\": \"generator\", \"id\": \"pulse\", ");
        out.append("\"name\": \"Pulse\" }");
    }
    for (std::size_t index = 0; index < spec.points.size(); ++index)
    {
        out.append(index == 0 ? "    " : ",\n    ");
        out.append(spec.points[index]);
    }
    out.append("\n  ],\n");
    out.append("  \"files\": [ ");
    bool first = true;
    for (const program_facts& file : test_plugin_facts())
    {
        if (!first)
            out.append(", ");
        first = false;
        out.append("{ \"path\": ");
        out.append(json_text(file.name));
        out.append(", \"size\": ");
        out.append(std::to_string(file.size));
        out.append(", \"sha512\": ");
        out.append(json_text(file.sha512));
        out.append(" }");
    }
    out.append(" ]\n}\n");
    return out;
}

/// A plugin folder `root/<id>`: the program and its runtime library if any,
/// and `plugin.json`.  Nothing is signed.
fs::path make_plugin(const fs::path& root, const plugin_spec& spec)
{
    const fs::path directory = root / spec.id;
    std::error_code ec;
    fs::create_directories(directory, ec);
    REQUIRE(!ec);
    const std::vector<fs::path> copies =
        copy_test_plugin(fs::absolute(fs::path(VSS_TEST_PLUGIN_PROGRAM)),
                         VSS_TEST_PLUGIN_RUNTIME, directory, ec);
    REQUIRE(!ec);
    REQUIRE_EQ(copies.size(), test_plugin_facts().size());
    write_file(directory / "plugin.json", manifest_text(spec));
    return directory;
}

/// Writes the trust file with the user's decision about the copy of the
/// plugin in `directory`: trusted, with the permission it needs, and on or
/// off.
void decide_about(const fs::path& trust_file, const fs::path& directory,
                  const std::string& id, bool enabled)
{
    const std::vector<plugin_root> roots{
        plugin_root{directory.parent_path(), plugin_scope::for_user}};
    const vss::plugin::discovery found = vss::plugin::discover_plugins(roots);
    REQUIRE_EQ(found.plugins.size(), std::size_t{1});
    const vss::plugin::found_plugin& plugin = found.plugins.front();
    REQUIRE(!plugin.problem.failed());

    plugin_decision decision;
    decision.plugin_id = id;
    decision.hash = plugin.hash;
    decision.level = trust_level::unsigned_plugin;
    decision.enabled = enabled;
    decision.granted = {permission::ports_read};
    // A computer that cannot confine a plugin as far as the minimum refuses
    // one that is not official, as it must; the user's choice to run it
    // without isolation, which a copy of Studio the user controls allows,
    // makes it run.
    decision.run_without_sandbox =
        available_sandbox().level() < sandbox_level::partial;
    decision.decided_on = std::chrono::year{2026} / 11 / 14;

    vss::plugin::trust_store store;
    REQUIRE(store.decide(decision).has_value());
    REQUIRE(vss::plugin::save_trust_store(trust_file, store).has_value());
}

/// A scratch directory with the folders a run uses.
class place
{
public:
    fs::path plugins() const { return scratch_.path() / "plugins"; }
    fs::path trust() const { return scratch_.path() / "trust.json"; }
    fs::path data() const { return scratch_.path() / "data"; }

    /// The words that name the folders of this place, after the id.
    std::vector<std::string> folders() const
    {
        return {"--root",  plugins().generic_string(),
                "--trust", trust().generic_string(),
                "--data",  data().generic_string()};
    }

private:
    scratch_directory scratch_;
};

/// A plugin folder in `where` with the user's answer "yes" for it.
fs::path trusted_plugin(const place& where, const plugin_spec& spec)
{
    const fs::path directory = make_plugin(where.plugins(), spec);
    decide_about(where.trust(), directory, spec.id, true);
    return directory;
}

// -- the command ------------------------------------------------------------

/// How the command ended.
struct cli_result
{
    std::optional<int> code;
    std::string output;
};

std::vector<std::string> run_words(const std::vector<std::string>& words)
{
    std::vector<std::string> arguments{"plugin", "run"};
    arguments.insert(arguments.end(), words.begin(), words.end());
    return arguments;
}

/// The words of a run of `id` in `where`, and then `more`.
std::vector<std::string> words_for(const place& where, const std::string& id,
                                   const std::vector<std::string>& more)
{
    std::vector<std::string> words{id};
    const std::vector<std::string> folders = where.folders();
    words.insert(words.end(), folders.begin(), folders.end());
    words.insert(words.end(), more.begin(), more.end());
    return words;
}

cli_result run_cli(const std::vector<std::string>& words)
{
    child_process child(VSS_CLI_PROGRAM, run_words(words));
    cli_result result;
    result.output = child.read_all(long_limit);
    result.code = child.wait(long_limit);
    return result;
}

/// Fails the test, with what the command printed, unless it ended with the
/// exit code.
void require_exit(const cli_result& result, int expected)
{
    if (result.code.has_value() && result.code.value() == expected)
        return;
    std::string text = "vss_cli plugin run should end with the exit code ";
    text.append(std::to_string(expected));
    text.append(", and it ");
    if (result.code.has_value())
    {
        text.append("ended with ");
        text.append(std::to_string(result.code.value()));
    }
    else
    {
        text.append("did not end");
    }
    text.append(".  It printed:\n");
    text.append(result.output);
    throw vss_test::failure(text);
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

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

/// The index of the first line that has `part`, or the number of lines.
std::size_t index_of_line_with(const std::vector<std::string>& lines,
                               std::string_view part, std::size_t from = 0)
{
    for (std::size_t index = from; index < lines.size(); ++index)
    {
        if (contains(lines[index], part))
            return index;
    }
    return lines.size();
}

/// The process id after "; process " in a state line.
std::optional<std::uint64_t> process_in(std::string_view line)
{
    constexpr std::string_view marker = "; process ";
    const std::size_t at = line.find(marker);
    if (at == std::string_view::npos)
        return std::nullopt;
    const std::string_view digits = line.substr(at + marker.size());
    std::uint64_t value = 0;
    const auto result =
        std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{})
        return std::nullopt;
    return value;
}

/// The first process id any state line of the output names.
std::optional<std::uint64_t>
first_process(const std::vector<std::string>& lines)
{
    for (const std::string& line : lines)
    {
        if (line.rfind("state: ", 0) != 0)
            continue;
        if (const auto process = process_in(line))
            return process;
    }
    return std::nullopt;
}

/// Waits until no process has the number, up to the long limit.
bool wait_until_gone(std::uint64_t pid)
{
    const auto deadline = steady::now() + long_limit;
    while (io::process_exists(pid))
    {
        if (steady::now() >= deadline)
            return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

/// Whether the text has a control character that would move a cursor: any
/// byte below a space but the line feed, and a carriage return that does not
/// come just before one (a text-mode stream writes both).
bool has_control_character(const std::string& text)
{
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte == '\n' || byte >= 0x20)
            continue;
        if (byte == '\r' && index + 1 < text.size() && text[index + 1] == '\n')
            continue;
        return true;
    }
    return false;
}

bool has_byte_above_ascii(const std::string& text)
{
    return std::ranges::any_of(
        text, [](char c) { return static_cast<unsigned char>(c) >= 0x7f; });
}

std::string hex_of(std::string_view bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const char c : bytes)
    {
        const auto byte = static_cast<unsigned char>(c);
        out += digits[static_cast<std::size_t>(byte >> 4U)];
        out += digits[static_cast<std::size_t>(byte & 0x0fU)];
    }
    return out;
}

/// What a hostile plugin prints: a colour change, a carriage return, a bell,
/// a byte that is not UTF-8.
std::string hostile_line()
{
    return "\x1b[31mhostile\r\x07\xff"
           "end";
}

json::object object_of(const std::string& line)
{
    try
    {
        const json::value value = json::parse(line);
        REQUIRE(value.is_object());
        return value.as_object();
    }
    catch (const std::exception& error)
    {
        throw vss_test::failure(std::string("a line is not JSON: ") +
                                error.what() + "\n" + line);
    }
}

std::string string_at(const json::object& object, std::string_view key)
{
    const json::value* const found = object.if_contains(key);
    REQUIRE(found != nullptr);
    REQUIRE(found->is_string());
    return std::string(found->as_string().data(), found->as_string().size());
}

} // namespace

TEST_CASE("plugin run: a trusted plugin runs for --seconds and is stopped")
{
    const place where;
    plugin_spec spec;
    spec.arguments = {"--stdout=a line of output", "--status=Ready to go",
                      "--log=a log line"};
    static_cast<void>(trusted_plugin(where, spec));
    const std::string trust_before = read_file(where.trust());

    const cli_result result =
        run_cli(words_for(where, spec.id, {"--seconds", "1"}));
    require_exit(result, 0);

    const std::vector<std::string> lines = lines_of(result.output);
    const std::size_t running = index_of_line_with(lines, "state: running; ");
    REQUIRE(running < lines.size());
    const std::optional<std::uint64_t> pid = process_in(lines[running]);
    REQUIRE(pid.has_value());
    REQUIRE(contains(lines[running], "; sandbox "));

    // The status and the lines come with a state line or a line of their own.
    REQUIRE(index_of_line_with(lines, "status warning \"Ready to go\"") <
            lines.size());
    REQUIRE(index_of_line_with(lines, "output: a line of output") <
            lines.size());
    REQUIRE(index_of_line_with(lines, "log info: a log line") < lines.size());

    // It was stopped in order, and last.
    const std::size_t stopped = index_of_line_with(lines, "state: stopped");
    REQUIRE(stopped < lines.size());
    REQUIRE(stopped > running);
    REQUIRE(wait_until_gone(pid.value()));

    // The command made no decision of its own, and the plugin has its data
    // folder.
    REQUIRE_EQ(read_file(where.trust()), trust_before);
    std::error_code ec;
    REQUIRE(fs::is_directory(where.data() / spec.id / "data", ec));
}

TEST_CASE("plugin run: --json prints one JSON object on every line")
{
    const place where;
    plugin_spec spec;
    spec.arguments = {"--stdout=a line of output", "--status=Ready to go",
                      "--log=a log line"};
    static_cast<void>(trusted_plugin(where, spec));

    const cli_result result =
        run_cli(words_for(where, spec.id, {"--seconds", "1", "--json"}));
    require_exit(result, 0);

    const std::vector<std::string> lines = lines_of(result.output);
    REQUIRE(!lines.empty());
    bool running = false;
    bool stopped = false;
    bool output_seen = false;
    bool log_seen = false;
    bool status_seen = false;
    std::optional<std::uint64_t> pid;
    for (const std::string& line : lines)
    {
        const json::object object = object_of(line);
        const std::string type = string_at(object, "type");
        REQUIRE_EQ(string_at(object, "plugin"), spec.id);
        if (type == "log")
        {
            const std::string source = string_at(object, "source");
            const std::string text = string_at(object, "text");
            output_seen = output_seen ||
                          (source == "output" && text == "a line of output");
            log_seen = log_seen || (source == "log" && text == "a log line" &&
                                    string_at(object, "level") == "info");
            continue;
        }
        REQUIRE_EQ(type, std::string("state"));
        const std::string state = string_at(object, "state");
        if (state == "running")
        {
            running = true;
            const json::value* const process =
                object.if_contains("process_id");
            REQUIRE(process != nullptr);
            REQUIRE(process->is_uint64() || process->is_int64());
            pid = process->is_uint64()
                      ? process->as_uint64()
                      : static_cast<std::uint64_t>(process->as_int64());
            const json::value* const sandbox = object.if_contains("sandbox");
            REQUIRE(sandbox != nullptr);
            REQUIRE(sandbox->is_object());
            REQUIRE(sandbox->as_object().if_contains("level") != nullptr);
            const json::value* const missing =
                sandbox->as_object().if_contains("not_applied");
            REQUIRE(missing != nullptr);
            REQUIRE(missing->is_array());
        }
        if (const json::value* const status = object.if_contains("status");
            status != nullptr && status->is_object())
        {
            const json::object& about = status->as_object();
            status_seen = status_seen ||
                          (string_at(about, "text") == "Ready to go" &&
                           string_at(about, "level") == "warning");
        }
        stopped = state == "stopped";
    }
    REQUIRE(running);
    REQUIRE(stopped);
    REQUIRE(output_seen);
    REQUIRE(log_seen);
    REQUIRE(status_seen);
    REQUIRE(pid.has_value());
    REQUIRE(wait_until_gone(pid.value()));
}

TEST_CASE("plugin run: a plugin that nobody decided about is not run")
{
    const place where;
    const plugin_spec spec;
    static_cast<void>(make_plugin(where.plugins(), spec));

    const cli_result result =
        run_cli(words_for(where, spec.id, {"--seconds", "1"}));
    require_exit(result, 1);
    REQUIRE(contains(result.output, "state: needs your decision; reason "
                                    "signature_missing"));
    REQUIRE(contains(result.output, "decide in Studio"));
    REQUIRE(contains(result.output, where.trust().generic_string()));
    REQUIRE(contains(result.output, "holds no decision for this copy"));
    REQUIRE(!contains(result.output, "state: running"));
    REQUIRE(!fs::exists(where.trust()));
}

TEST_CASE("plugin run: a plugin that the user switched off is not run")
{
    const place where;
    const plugin_spec spec;
    const fs::path directory = make_plugin(where.plugins(), spec);
    decide_about(where.trust(), directory, spec.id, false);

    const cli_result result =
        run_cli(words_for(where, spec.id, {"--seconds", "1"}));
    require_exit(result, 1);
    REQUIRE(contains(result.output, "state: off"));
    REQUIRE(contains(result.output, "switched off"));
    REQUIRE(!contains(result.output, "state: running"));
}

TEST_CASE("plugin run: an id that is not installed exits 2")
{
    const place where;
    const plugin_spec spec;
    static_cast<void>(trusted_plugin(where, spec));

    const cli_result result =
        run_cli(words_for(where, "com.example.nothere", {"--seconds", "1"}));
    require_exit(result, 2);
    REQUIRE(contains(result.output,
                     "no plugin with the id com.example.nothere"));
    REQUIRE(!contains(result.output, "state: running"));
}

TEST_CASE("plugin run: a command line that is wrong exits 2")
{
    const std::vector<std::vector<std::string>> wrong{
        {},
        {"--json"},
        {"com.example.a", "--bogus"},
        {"com.example.a", "com.example.b"},
        {"com.example.a", "--seconds", "soon"},
        {"com.example.a", "--seconds", "-1"},
        {"com.example.a", "--seconds"},
        {"com.example.a", "--root"},
        {"com.example.a", "--trust="},
    };
    for (const std::vector<std::string>& words : wrong)
    {
        const cli_result result = run_cli(words);
        require_exit(result, 2);
        REQUIRE(contains(result.output, "plugin run: "));
    }
}

TEST_CASE("plugin run: a plugin that never says hello is stopped for failing")
{
    const place where;
    plugin_spec spec;
    spec.arguments = {"--no-hello"};
    static_cast<void>(trusted_plugin(where, spec));

    child_process child(VSS_CLI_PROGRAM,
                        run_words(words_for(where, spec.id, {})));
    std::string output;
    const auto deadline = steady::now() + long_limit;
    for (;;)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - steady::now());
        REQUIRE(left.count() > 0);
        const std::optional<std::string> line = child.read_line(left);
        REQUIRE(line.has_value());
        output.append(line.value());
        output.push_back('\n');
        if (contains(line.value(), "state: waiting to start again"))
            break;
    }

    // The default waits run to about forty seconds before the plugin counts
    // as failed for good, so the command is ended once it has said that it
    // waits.
    if (!child.request_stop())
    {
        child.kill();
        VSS_SKIP("the stop signal could not be delivered; on Windows this "
                 "test needs a console to send CTRL_BREAK to");
    }
    output.append(child.read_all(long_limit));
    const std::optional<int> code = child.wait(long_limit);
    cli_result result;
    result.code = code;
    result.output = output;
    require_exit(result, 1);

    const std::vector<std::string> lines = lines_of(output);
    const std::size_t waiting =
        index_of_line_with(lines, "state: waiting to start again in ");
    REQUIRE(waiting < lines.size());
    REQUIRE(contains(lines[waiting], "(failure 1 of 5)"));
    REQUIRE(contains(lines[waiting], "; reason no_hello"));
    REQUIRE(contains(output, "did not reach running"));
    REQUIRE(!contains(output, "state: running"));
    if (const auto pid = first_process(lines))
        REQUIRE(wait_until_gone(pid.value()));
}

TEST_CASE("plugin run: text from the plugin cannot move the cursor")
{
    const place where;
    plugin_spec spec;
    // A name with letters that are not ASCII, and what a plugin prints that
    // would change the screen.
    spec.name = "Caf\xc3\xa9 \"Run\" plugin \xf0\x9f\x98\x80";
    spec.arguments = {"--stdout-hex=" + hex_of(hostile_line()),
                      "--status=say \"hello\" <b>now</b>"};
    static_cast<void>(trusted_plugin(where, spec));

    const cli_result result =
        run_cli(words_for(where, spec.id, {"--seconds", "1"}));
    require_exit(result, 0);
    REQUIRE(!has_control_character(result.output));
    REQUIRE(!has_byte_above_ascii(result.output));
    // Each byte that was not plain became a question mark.
    REQUIRE(contains(result.output, "output: ?[31mhostile???end"));
    REQUIRE(contains(result.output,
                     "status warning \"say \"hello\" <b>now</b>\""));
}

TEST_CASE("plugin run: --json escapes text from the plugin")
{
    const place where;
    plugin_spec spec;
    spec.name = "Caf\xc3\xa9 \"Run\" plugin \xf0\x9f\x98\x80";
    spec.arguments = {"--stdout-hex=" + hex_of(hostile_line())};
    static_cast<void>(trusted_plugin(where, spec));

    const cli_result result =
        run_cli(words_for(where, spec.id, {"--seconds", "1", "--json"}));
    require_exit(result, 0);
    REQUIRE(!has_control_character(result.output));
    REQUIRE(!has_byte_above_ascii(result.output));
    REQUIRE(contains(result.output, "\\u001b"));

    // Read back, the line is what the plugin printed, but for the byte that
    // is not UTF-8, which cannot be in JSON.
    bool seen = false;
    for (const std::string& line : lines_of(result.output))
    {
        const json::object object = object_of(line);
        if (string_at(object, "type") == "log" &&
            string_at(object, "source") == "output")
        {
            seen = seen || string_at(object, "text") ==
                               "\x1b[31mhostile\r\x07?end";
        }
    }
    REQUIRE(seen);
}

// -- instances ----------------------------------------------------------------

namespace
{

/// A plugin whose manifest declares the adapter `recorder`, and whose program
/// answers what `modifiers` say.
plugin_spec recorder_plugin(std::vector<std::string> modifiers)
{
    plugin_spec spec;
    spec.points = {R"({ "kind": "adapter", "id": "recorder", )"
                   R"("name": "Recorder" })"};
    spec.arguments = std::move(modifiers);
    return spec;
}

/// The three messages of an instance, answered with "ok".
std::vector<std::string> echo_instance_messages()
{
    return {"--echo-op=instance.create", "--echo-op=params.changed",
            "--echo-op=instance.stop"};
}

/// The lines of the output that are JSON objects of an instance.
std::vector<json::object> instance_objects(const std::string& output)
{
    std::vector<json::object> found;
    for (const std::string& line : lines_of(output))
    {
        if (!line.starts_with('{'))
            continue;
        json::object object = object_of(line);
        if (string_at(object, "type") == "instance")
            found.push_back(std::move(object));
    }
    return found;
}

std::int64_t number_at(const json::object& object, std::string_view key)
{
    const json::value* const found = object.if_contains(key);
    REQUIRE(found != nullptr);
    REQUIRE(found->is_int64() || found->is_uint64());
    return found->is_int64() ? found->as_int64()
                             : static_cast<std::int64_t>(found->as_uint64());
}

bool is_null_at(const json::object& object, std::string_view key)
{
    const json::value* const found = object.if_contains(key);
    REQUIRE(found != nullptr);
    return found->is_null();
}

} // namespace

TEST_CASE("plugin run: --instance makes an instance and prints it running")
{
    const place where;
    const plugin_spec spec = recorder_plugin(echo_instance_messages());
    static_cast<void>(trusted_plugin(where, spec));

    const cli_result result = run_cli(words_for(
        where, spec.id, {"--seconds", "1", "--instance", "adapter:recorder"}));
    require_exit(result, 0);

    const std::vector<std::string> lines = lines_of(result.output);
    const std::size_t running =
        index_of_line_with(lines, "instance 1 adapter:recorder: running");
    REQUIRE(running < lines.size());
    REQUIRE(index_of_line_with(lines, "got instance.create") < lines.size());
    REQUIRE(index_of_line_with(lines, ": refused") == lines.size());
    // The plugin was running before it had the instance.
    REQUIRE(index_of_line_with(lines, "state: running") < running);
}

TEST_CASE("plugin run: an instance the plugin refuses ends with exit code 1")
{
    const place where;
    const plugin_spec spec = recorder_plugin({"--refuse-op=instance.create"});
    static_cast<void>(trusted_plugin(where, spec));

    const cli_result result = run_cli(
        words_for(where, spec.id,
                  {"--seconds", "1", "--instance=adapter:recorder"}));
    require_exit(result, 1);
    REQUIRE(contains(result.output,
                     "instance 1 adapter:recorder: refused; problem "
                     "misbehaving; reason \"refused by the test plugin\""));
    REQUIRE(contains(result.output,
                     "plugin run: instance 1 adapter:recorder did not reach "
                     "running: misbehaving"));
    REQUIRE(!contains(result.output, "adapter:recorder: running"));
}

TEST_CASE("plugin run: an --instance that is wrong exits 2")
{
    const std::vector<std::vector<std::string>> wrong{
        {"com.example.a", "--instance", "bogus"},
        {"com.example.a", "--instance", "adapter:recorder=[1]"},
        {"com.example.a", "--instance", "adapter:recorder=not json"},
        {"com.example.a", "--instance", "adapter:recorder={} {}"},
        {"com.example.a", "--instance", "widget:recorder"},
        {"com.example.a", "--instance", "adapter:"},
        {"com.example.a", "--instance", ":recorder"},
        {"com.example.a", "--instance"},
        {"com.example.a", "--instance="},
    };
    for (const std::vector<std::string>& words : wrong)
    {
        const cli_result result = run_cli(words);
        require_exit(result, 2);
        REQUIRE(contains(result.output, "plugin run: "));
    }

    // One line says what is wrong, before the usage.
    const cli_result result =
        run_cli({"com.example.a", "--instance", "adapter:recorder=[1]"});
    require_exit(result, 2);
    REQUIRE(lines_of(result.output).front() ==
            "plugin run: --instance adapter:recorder: the parameters are not "
            "a JSON object");
}

TEST_CASE("plugin run: --json prints each change of an instance")
{
    const place where;
    const plugin_spec spec = recorder_plugin(echo_instance_messages());
    static_cast<void>(trusted_plugin(where, spec));

    // The second instance is of a type the manifest does not declare.
    const cli_result result =
        run_cli(words_for(where, spec.id,
                          {"--seconds", "1", "--json", "--instance",
                           "adapter:recorder={}", "--instance",
                           "adapter:nothing"}));
    require_exit(result, 1);
    REQUIRE(contains(result.output,
                     "plugin run: instance 2 adapter:nothing did not reach "
                     "running: capability_missing"));

    const std::vector<json::object> found = instance_objects(result.output);
    REQUIRE(!found.empty());
    bool recorder_running = false;
    bool nothing_refused = false;
    for (const json::object& object : found)
    {
        REQUIRE_EQ(string_at(object, "plugin"), spec.id);
        REQUIRE_EQ(string_at(object, "kind"), "adapter");
        const json::value* const pending =
            object.if_contains("params_pending");
        REQUIRE(pending != nullptr);
        REQUIRE(pending->is_bool());
        const std::int64_t number = number_at(object, "instance");
        const std::string state = string_at(object, "state");
        if (number == 1)
        {
            REQUIRE_EQ(string_at(object, "instance_type"), "recorder");
            REQUIRE(is_null_at(object, "problem"));
            REQUIRE(is_null_at(object, "reason"));
            recorder_running = recorder_running || state == "running";
        }
        else
        {
            REQUIRE_EQ(number, std::int64_t{2});
            REQUIRE_EQ(string_at(object, "instance_type"), "nothing");
            if (state == "refused")
            {
                REQUIRE_EQ(string_at(object, "problem"),
                           "capability_missing");
                REQUIRE(is_null_at(object, "reason"));
                nothing_refused = true;
            }
        }
    }
    REQUIRE(recorder_running);
    REQUIRE(nothing_refused);
}

TEST_CASE("plugin run: --json gives the plugin's reason for a refusal")
{
    const place where;
    const plugin_spec spec = recorder_plugin({"--refuse-op=instance.create"});
    static_cast<void>(trusted_plugin(where, spec));

    const cli_result result = run_cli(
        words_for(where, spec.id,
                  {"--seconds", "1", "--json", "--instance",
                   "adapter:recorder"}));
    require_exit(result, 1);

    bool refused = false;
    for (const json::object& object : instance_objects(result.output))
    {
        if (string_at(object, "state") != "refused")
            continue;
        REQUIRE_EQ(string_at(object, "problem"), "misbehaving");
        REQUIRE_EQ(string_at(object, "reason"), "refused by the test plugin");
        refused = true;
    }
    REQUIRE(refused);
}
