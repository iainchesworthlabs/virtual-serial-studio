//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_cli plugin - which plugins are installed on this computer.
//
//   list    read-only: the plugins found in the plugin folders, and for each
//           one the problem it has, if any
//
// It runs no plugin, checks no signature and changes nothing.  Text that came
// from a plugin is shown as plain text, so that a plugin cannot move the
// cursor or change the colours of the terminal.  docs/plugins.md is the
// design.
//

#include "plugin_command.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/access.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/locations.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/verify.hpp>
#include <vss/ports/permissions.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss_cli
{
namespace
{

namespace fs = std::filesystem;
namespace json = boost::json;
namespace plug = vss::plugin;

constexpr int exit_ok = 0;
constexpr int exit_usage = 2;

// -- the command line -------------------------------------------------------

struct list_options
{
    bool json = false;
    /// The folders given with --root.  When there are none, the usual
    /// folders of this computer are used.
    std::vector<fs::path> roots;
};

/// Reads the words after `plugin list`.  When a word is wrong, says so and
/// returns false.
bool parse_list(const std::vector<std::string>& arguments,
                list_options& options)
{
    for (std::size_t index = 2; index < arguments.size(); ++index)
    {
        const std::string& argument = arguments[index];
        if (argument == "--json")
        {
            options.json = true;
        }
        else if (argument == "--root")
        {
            if (index + 1 >= arguments.size() || arguments[index + 1].empty())
            {
                std::fputs("plugin list: --root needs a folder\n", stderr);
                return false;
            }
            ++index;
            options.roots.emplace_back(arguments[index]);
        }
        else if (argument.rfind("--root=", 0) == 0)
        {
            if (argument.size() == 7)
            {
                std::fputs("plugin list: --root needs a folder\n", stderr);
                return false;
            }
            options.roots.emplace_back(argument.substr(7));
        }
        else
        {
            std::fprintf(stderr, "plugin list: unknown argument %s\n",
                         argument.c_str());
            return false;
        }
    }
    return true;
}

// -- text from outside ------------------------------------------------------

/// `text` with every byte outside printable ASCII replaced by `?`.
std::string plain(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        out += byte >= 0x20 && byte <= 0x7e ? c : '?';
    }
    return out;
}

/// How many bytes the UTF-8 character at the start of `text` has, or 0 when
/// the bytes there are not a character: a stray or missing continuation, a
/// longer form than needed, a surrogate or a value above U+10FFFF.
std::size_t utf8_length(std::string_view text)
{
    constexpr std::size_t not_a_character = 0;
    const auto at = [&text](std::size_t index)
    { return static_cast<unsigned>(static_cast<unsigned char>(text[index])); };
    const auto continuation = [&](std::size_t index, unsigned low,
                                  unsigned high)
    {
        return index < text.size() && at(index) >= low && at(index) <= high;
    };

    const unsigned lead = at(0);
    if (lead < 0x80)
        return 1;
    if (lead >= 0xc2 && lead <= 0xdf)
        return continuation(1, 0x80, 0xbf) ? 2 : not_a_character;
    if (lead >= 0xe0 && lead <= 0xef)
    {
        const unsigned low = lead == 0xe0 ? 0xa0 : 0x80;
        const unsigned high = lead == 0xed ? 0x9f : 0xbf;
        if (continuation(1, low, high) && continuation(2, 0x80, 0xbf))
            return 3;
        return not_a_character;
    }
    if (lead >= 0xf0 && lead <= 0xf4)
    {
        const unsigned low = lead == 0xf0 ? 0x90 : 0x80;
        const unsigned high = lead == 0xf4 ? 0x8f : 0xbf;
        if (continuation(1, low, high) && continuation(2, 0x80, 0xbf) &&
            continuation(3, 0x80, 0xbf))
            return 4;
        return not_a_character;
    }
    return not_a_character;
}

/// `text` as valid UTF-8, with each byte that is not part of a character
/// replaced by `?`.  A JSON text must be valid UTF-8.
std::string valid_utf8(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size())
    {
        const std::size_t length = utf8_length(text.substr(index));
        if (length == 0)
        {
            out += '?';
            ++index;
        }
        else
        {
            out.append(text.substr(index, length));
            index += length;
        }
    }
    return out;
}

/// The path with `/` between the names, as UTF-8.
std::string path_text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

/// The folder of the running program, or an empty path when it cannot be
/// found.
fs::path program_directory()
{
    const std::string program = vss::permissions::this_program();
    if (program.empty())
        return {};
    // The text is UTF-8 on every system.
    const std::u8string text(program.begin(), program.end());
    return fs::path(text).parent_path();
}

// -- what was found ---------------------------------------------------------

/// What came of looking for the administrator's policy.
enum class policy_state
{
    /// Not looked for: the folders were given with --root.
    not_read,
    /// There is no policy file.
    none,
    /// There is one and it counts.
    used,
    /// There is one and it cannot be used.
    unusable
};

struct report
{
    /// Nothing when the folder of the programs could not be examined.
    std::optional<plug::install_kind> install;
    /// Why not, as one line.
    std::string install_detail;

    policy_state policy = policy_state::none;
    fs::path policy_file;
    std::string policy_detail;

    plug::discovery found;
};

std::string failure_line(const std::string& detail, const vss::error_code& why)
{
    return detail.empty() ? why.message() : detail;
}

report gather(const list_options& options)
{
    report out;
    const fs::path programs = program_directory();

    std::string detail;
    const auto kind = plug::kind_of_install(programs, &detail);
    if (kind.has_value())
        out.install = kind.value();
    else
        out.install_detail = failure_line(detail, kind.error());

    std::vector<plug::plugin_root> roots;
    if (options.roots.empty())
    {
        const plug::plugin_locations places =
            plug::plugin_locations_for(programs);
        roots = places.roots;
        out.policy_file = places.policy_file;
        if (!places.policy_file.empty())
        {
            const plug::loaded_policy loaded =
                plug::load_policy(places.policy_file);
            if (!loaded.rules.has_value())
                out.policy = policy_state::none;
            else if (loaded.problem.failed())
                out.policy = policy_state::unusable;
            else
                out.policy = policy_state::used;
            out.policy_detail =
                loaded.problem.failed()
                    ? failure_line(loaded.detail, loaded.problem)
                    : loaded.detail;
        }
    }
    else
    {
        out.policy = policy_state::not_read;
        out.policy_detail = "not read, because --root was given";
        for (const fs::path& folder : options.roots)
        {
            plug::plugin_root root;
            root.directory = folder;
            root.scope = plug::plugin_scope::for_user;
            roots.push_back(std::move(root));
        }
    }

    out.found = plug::discover_plugins(roots);
    return out;
}

/// The reason of a plugin as a word: `found`, the code of a plugin reason, or
/// the system's own words for any other error.
std::string state_text(const vss::error_code& problem)
{
    if (!problem.failed())
        return "found";
    if (problem.category() == plug::plugin_category())
    {
        return std::string(
            plug::to_string(static_cast<plug::plugin_errc>(problem.value())));
    }
    return problem.message();
}

/// Whether the plugin has a `plugin.sig`.  A signature that could not be
/// read, because it is a link or too large, is still there.
bool has_signature(const plug::found_plugin& each)
{
    if (each.signature_bytes.has_value())
        return true;
    return std::any_of(each.files.begin(), each.files.end(),
                       [](const plug::file_entry& file)
                       { return file.path == plug::signature_file_name; });
}

/// The fingerprint, or nothing when the files were never measured.
std::optional<std::string> fingerprint(const plug::found_plugin& each)
{
    if (each.files.empty())
        return std::nullopt;
    return each.hash.short_text();
}

/// The copy that counts, for a plugin that is shadowed.
const plug::found_plugin* winner_of(const plug::found_plugin& each,
                                    const plug::discovery& found)
{
    const std::optional<std::size_t>& shadowed_by = each.shadowed_by;
    if (!shadowed_by.has_value())
        return nullptr;
    const std::size_t index = shadowed_by.value();
    return index < found.plugins.size() ? &found.plugins[index] : nullptr;
}

// -- the text ---------------------------------------------------------------

std::string_view installed_for(plug::plugin_scope scope) noexcept
{
    switch (scope)
    {
    case plug::plugin_scope::with_studio:
        return "came with Studio";
    case plug::plugin_scope::for_everyone:
        return "everyone on this computer";
    case plug::plugin_scope::for_user:
        return "you only";
    }
    return "unknown";
}

/// One line of a plugin's block.  `label` carries its own spaces.
void add_line(std::string& out, std::string_view label, std::string_view value)
{
    out += "  ";
    out += label;
    out += value;
    out += '\n';
}

std::string heading_of(const plug::found_plugin& each)
{
    const std::optional<plug::manifest>& declared = each.declared;
    if (!declared.has_value())
        return plain(path_text(each.directory.filename()));
    std::string heading = plain(declared.value().id);
    heading += "  ";
    heading += plain(plug::to_string(declared.value().version));
    heading += "  ";
    heading += plain(declared.value().name);
    return heading;
}

void add_plugin(std::string& out, const plug::found_plugin& each,
                const plug::discovery& found)
{
    out += heading_of(each);
    out += '\n';
    add_line(out, "Folder:       ", plain(path_text(each.directory)));
    add_line(out, "Installed for: ", installed_for(each.scope));
    const std::optional<std::string> print = fingerprint(each);
    add_line(out, "Fingerprint:  ",
             print.has_value() ? plain(print.value()) : "not worked out");
    add_line(out, "Signature:    ",
             has_signature(each) ? "present, not checked by this build"
                                 : "not signed");

    std::string state = state_text(each.problem);
    if (!each.detail.empty())
    {
        state += ": ";
        state += each.detail;
    }
    add_line(out, "State:        ", plain(state));

    if (const plug::found_plugin* winner = winner_of(each, found))
    {
        std::string copy = "the copy in ";
        copy += plain(path_text(winner->directory));
        copy += " is used";
        add_line(out, "Not used:     ", copy);
    }
}

void add_summary(std::string& out, std::size_t count)
{
    if (count == 0)
        out += "No plugins found.";
    else if (count == 1)
        out += "1 plugin found.";
    else
    {
        out += std::to_string(count);
        out += " plugins found.";
    }
    out += " This build does not run plugins.\n";
}

std::string text_of(const report& result)
{
    std::string out;
    const std::optional<plug::install_kind>& install = result.install;
    if (install.has_value())
    {
        out += install.value() == plug::install_kind::for_everyone
                   ? "Studio is installed for everyone on this computer.\n"
                   : "Studio is installed for you only.\n";
    }
    else
    {
        out += "Studio's own folder could not be examined: ";
        out += plain(result.install_detail);
        out += '\n';
    }

    switch (result.policy)
    {
    case policy_state::not_read:
        out += "Policy: not read, because --root was given.\n";
        break;
    case policy_state::none:
        out += "Policy: none.\n";
        break;
    case policy_state::used:
        out += "Policy: ";
        out += plain(path_text(result.policy_file));
        out += ".\n";
        break;
    case policy_state::unusable:
        out += "Policy: ";
        out += plain(path_text(result.policy_file));
        out += " could not be used, so only official plugins are allowed: ";
        out += plain(result.policy_detail);
        out += '\n';
        break;
    }
    out += '\n';

    for (const plug::found_plugin& each : result.found.plugins)
    {
        add_plugin(out, each, result.found);
        out += '\n';
    }

    for (const plug::root_problem& each : result.found.roots)
    {
        out += "Folder ";
        out += plain(path_text(each.directory));
        out += " was not looked through: ";
        out += plain(failure_line(each.detail, each.problem));
        out += '\n';
    }
    if (!result.found.roots.empty())
        out += '\n';

    add_summary(out, result.found.plugins.size());
    return out;
}

// -- the JSON ---------------------------------------------------------------

json::value text_value(std::string_view text)
{
    const std::string cleaned = valid_utf8(text);
    return json::value(json::string(cleaned.data(), cleaned.size()));
}

json::value optional_text_value(const std::optional<std::string>& text)
{
    if (!text.has_value())
        return json::value(nullptr);
    return text_value(text.value());
}

json::value policy_json(const report& result)
{
    json::object policy;
    if (result.policy == policy_state::not_read ||
        result.policy_file.empty())
        policy["file"] = nullptr;
    else
        policy["file"] = text_value(path_text(result.policy_file));
    policy["present"] = result.policy == policy_state::used ||
                        result.policy == policy_state::unusable;
    policy["used"] = result.policy == policy_state::used;
    policy["detail"] = text_value(result.policy_detail);
    return json::value(std::move(policy));
}

json::value plugin_json(const plug::found_plugin& each)
{
    json::object item;
    const std::optional<plug::manifest>& declared = each.declared;
    if (declared.has_value())
    {
        item["id"] = text_value(declared.value().id);
        item["version"] =
            text_value(plug::to_string(declared.value().version));
        item["name"] = text_value(declared.value().name);
    }
    else
    {
        item["id"] = nullptr;
        item["version"] = nullptr;
        item["name"] = nullptr;
    }
    item["folder"] = text_value(path_text(each.directory));
    item["scope"] = text_value(plug::to_string(each.scope));
    item["fingerprint"] = optional_text_value(fingerprint(each));
    item["signature"] = has_signature(each) ? "present" : "none";
    item["state"] = text_value(state_text(each.problem));
    item["detail"] = text_value(each.detail);
    const std::optional<std::size_t>& shadowed_by = each.shadowed_by;
    if (shadowed_by.has_value())
    {
        item["shadowed_by"] = static_cast<std::uint64_t>(shadowed_by.value());
    }
    else
    {
        item["shadowed_by"] = nullptr;
    }
    return json::value(std::move(item));
}

std::string json_of(const report& result)
{
    json::object top;
    const std::optional<plug::install_kind>& install = result.install;
    if (!install.has_value())
        top["install"] = nullptr;
    else if (install.value() == plug::install_kind::for_everyone)
        top["install"] = "for_everyone";
    else
        top["install"] = "for_one_user";
    top["policy"] = policy_json(result);

    json::array plugins;
    for (const plug::found_plugin& each : result.found.plugins)
        plugins.push_back(plugin_json(each));
    top["plugins"] = std::move(plugins);

    json::array roots;
    for (const plug::root_problem& each : result.found.roots)
    {
        json::object item;
        item["folder"] = text_value(path_text(each.directory));
        item["detail"] = text_value(failure_line(each.detail, each.problem));
        roots.push_back(std::move(item));
    }
    top["roots_not_read"] = std::move(roots);

    std::string out = json::serialize(top);
    out += '\n';
    return out;
}

// -- the commands -----------------------------------------------------------

int command_list(const std::vector<std::string>& arguments)
{
    list_options options;
    if (!parse_list(arguments, options))
    {
        std::fputs(plugin_usage(), stderr);
        return exit_usage;
    }

    const report result = gather(options);
    const std::string text = options.json ? json_of(result) : text_of(result);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return exit_ok;
}

} // namespace

const char* plugin_usage() noexcept
{
    return "  vss_cli plugin list [--json] [--root <folder>]...\n"
           "      Show the plugins found in the plugin folders of this "
           "computer,\n"
           "      and the problem of each one that has a problem.  Reads "
           "only: it\n"
           "      runs no plugin and checks no signature.  Each --root "
           "replaces\n"
           "      the usual folders with the folders given; the policy file "
           "is\n"
           "      then not read.  Exit code 0 means it ran, whatever it "
           "found.\n"
           "\n";
}

int command_plugin(const std::vector<std::string>& arguments)
{
    if (arguments.size() < 2)
    {
        std::fputs(plugin_usage(), stderr);
        return exit_usage;
    }
    const std::string& verb = arguments[1];
    if (verb == "list")
        return command_list(arguments);
    std::fprintf(stderr, "plugin: unknown step %s\n", verb.c_str());
    std::fputs(plugin_usage(), stderr);
    return exit_usage;
}

} // namespace vss_cli
