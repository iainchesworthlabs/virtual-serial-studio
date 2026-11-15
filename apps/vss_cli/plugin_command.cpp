//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_cli plugin - which plugins are installed on this computer, and what
// each one is.
//
//   list    read-only: the plugins found in the plugin folders, for each one
//           the problem it has, if any, and what the checks before a run say
//   verify, install, remove
//           in plugin_install_command.cpp
//
// It runs no plugin.  Text that came
// from a plugin is shown as plain text, so that a plugin cannot move the
// cursor or change the colours of the terminal.  docs/plugins.md is the
// design.
//

#include "plugin_command.hpp"

#include "plugin_install_command.hpp"
#include "plugin_support.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/access.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/locations.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/policy.hpp>
#include <vss/plugin/verify.hpp>

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

using plugin_support::path_text;
using plugin_support::plain;
using plugin_support::text_value;

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

    /// What this computer says about plugins, and the verdict on each plugin
    /// of `found`, in the same order.  A plugin whose manifest was not read
    /// has none.
    plugin_support::computer computer;
    std::vector<std::optional<plug::inspection>> verdicts;
};

std::string failure_line(const std::string& detail, const vss::error_code& why)
{
    return detail.empty() ? why.message() : detail;
}

report gather(const list_options& options)
{
    report out;
    const fs::path programs = plugin_support::program_directory();

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

    // The policy file is read for the verdicts only when the usual folders
    // are used, as for the report.
    out.computer = plugin_support::this_computer(options.roots.empty());
    for (const plug::found_plugin& each : out.found.plugins)
    {
        if (each.declared.has_value())
        {
            out.verdicts.emplace_back(
                plug::inspect_plugin(each.directory, out.computer.context));
        }
        else
        {
            out.verdicts.emplace_back(std::nullopt);
        }
    }
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
                const plug::discovery& found, const plug::inspection* verdict,
                const plug::inspect_context& context)
{
    out += heading_of(each);
    out += '\n';
    add_line(out, "Folder:       ", plain(path_text(each.directory)));
    add_line(out, "Installed for: ", installed_for(each.scope));
    const std::optional<std::string> print = fingerprint(each);
    add_line(out, "Fingerprint:  ",
             print.has_value() ? plain(print.value()) : "not worked out");
    if (verdict == nullptr)
    {
        add_line(out, "Signature:    ",
                 has_signature(each) ? "present, not checked"
                                     : "not signed");
    }
    std::string state = state_text(each.problem);
    if (!each.detail.empty())
    {
        state += ": ";
        state += each.detail;
    }
    add_line(out, "State:        ", plain(state));
    if (verdict != nullptr)
        plugin_support::add_verdict_lines(out, *verdict, context);

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

    for (std::size_t index = 0; index < result.found.plugins.size(); ++index)
    {
        const std::optional<plug::inspection>& verdict =
            result.verdicts[index];
        add_plugin(out, result.found.plugins[index], result.found,
                   verdict.has_value() ? &*verdict : nullptr,
                   result.computer.context);
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

    for (const std::string& note : result.computer.notes)
    {
        out += "Note: ";
        out += plain(note);
        out += '\n';
    }
    if (!result.computer.notes.empty())
        out += '\n';

    add_summary(out, result.found.plugins.size());
    return out;
}

// -- the JSON ---------------------------------------------------------------

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

json::value plugin_json(const plug::found_plugin& each,
                        const plug::inspection* verdict,
                        const plug::inspect_context& context)
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
    if (verdict != nullptr)
        plugin_support::add_verdict_json(item, *verdict, context);
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
    for (std::size_t index = 0; index < result.found.plugins.size(); ++index)
    {
        const std::optional<plug::inspection>& verdict =
            result.verdicts[index];
        plugins.push_back(plugin_json(result.found.plugins[index],
                                      verdict.has_value() ? &*verdict : nullptr,
                                      result.computer.context));
    }
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
    static const std::string text = []
    {
        std::string out =
            "  vss_cli plugin list [--json] [--root <folder>]...\n"
            "      Show the plugins found in the plugin folders of this "
            "computer,\n"
            "      the problem of each one that has a problem, and for each "
            "the\n"
            "      checks made before a run: signature, trust, whether it "
            "may run\n"
            "      and whether the licence covers it.  It runs no plugin and "
            "changes\n"
            "      nothing.  Each --root replaces the usual folders with the "
            "folders\n"
            "      given; the policy file is then not read.  Exit code 0 "
            "means it\n"
            "      ran, whatever it found.\n"
            "\n";
        out += plugin_install_usage();
        return out;
    }();
    return text.c_str();
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
    if (verb == "verify")
        return command_plugin_verify(arguments);
    if (verb == "install")
        return command_plugin_install(arguments);
    if (verb == "remove")
        return command_plugin_remove(arguments);
    std::fprintf(stderr, "plugin: unknown step %s\n", verb.c_str());
    std::fputs(plugin_usage(), stderr);
    return exit_usage;
}

} // namespace vss_cli
