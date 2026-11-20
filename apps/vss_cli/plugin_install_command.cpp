//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_cli plugin verify, install and remove.
//
//   verify   read-only: what the checks before a run say about a plugin
//            folder, and whether it can be installed
//   install  copies a plugin folder into a plugin folder of this computer
//   remove   removes an installed plugin's folder
//
// None of them runs a plugin.  Text that came from a plugin is shown as plain
// text.  docs/plugins.md is the design.
//

#include "plugin_install_command.hpp"

#include "plugin_support.hpp"

#include <vss/core/error.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/locations.hpp>

#include <boost/json.hpp>

#include <cstddef>
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
namespace sup = plugin_support;

constexpr int exit_ok = 0;
constexpr int exit_refused = 1;
constexpr int exit_usage = 2;

struct options
{
    bool json = false;
    bool for_everyone = false;
    bool replace = false;
    bool allow_unsigned = false;
    std::optional<fs::path> root;
    /// The folder (verify, install) or the id (remove).
    std::string subject;
};

enum class flag
{
    json,
    for_everyone,
    replace,
    allow_unsigned,
    root
};

bool allowed(std::string_view command, flag which) noexcept
{
    if (command == "verify")
        return which == flag::json || which == flag::allow_unsigned;
    if (command == "install")
    {
        return which == flag::for_everyone || which == flag::replace ||
               which == flag::allow_unsigned || which == flag::root;
    }
    return which == flag::for_everyone || which == flag::root;
}

/// Reads the words after `plugin <command>`.  When a word is wrong, says so
/// and returns false.
bool parse(const std::vector<std::string>& arguments, options& out)
{
    const std::string& command = arguments[1];
    const auto complain = [&command](const std::string& argument)
    {
        std::fprintf(stderr, "plugin %s: unknown argument %s\n",
                     command.c_str(), argument.c_str());
        return false;
    };
    std::size_t index = 2;
    while (index < arguments.size())
    {
        const std::string& argument = arguments[index++];
        if (argument == "--json" && allowed(command, flag::json))
        {
            out.json = true;
        }
        else if (argument == "--for-everyone" &&
                 allowed(command, flag::for_everyone))
        {
            out.for_everyone = true;
        }
        else if (argument == "--replace" && allowed(command, flag::replace))
        {
            out.replace = true;
        }
        else if (argument == "--allow-unsigned" &&
                 allowed(command, flag::allow_unsigned))
        {
            out.allow_unsigned = true;
        }
        else if (argument == "--root" && allowed(command, flag::root))
        {
            if (index >= arguments.size() || arguments[index].empty())
            {
                std::fprintf(stderr, "plugin %s: --root needs a folder\n",
                             command.c_str());
                return false;
            }
            out.root = fs::path(arguments[index++]);
        }
        else if (argument.rfind("--root=", 0) == 0 &&
                 allowed(command, flag::root))
        {
            if (argument.size() == 7)
            {
                std::fprintf(stderr, "plugin %s: --root needs a folder\n",
                             command.c_str());
                return false;
            }
            out.root = fs::path(argument.substr(7));
        }
        else if (out.subject.empty() && argument.rfind("--", 0) != 0)
        {
            out.subject = argument;
        }
        else
        {
            return complain(argument);
        }
    }
    if (out.subject.empty())
    {
        std::fprintf(stderr, "plugin %s: %s is needed\n", command.c_str(),
                     command == "remove" ? "the id of a plugin" : "a folder");
        return false;
    }
    return true;
}

void print(const std::string& text)
{
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

void add_line(std::string& out, std::string_view label, std::string_view value)
{
    out += "  ";
    out += label;
    out += value;
    out += '\n';
}

/// Whether the folder would be installed, and what to say about it.
struct decision
{
    bool acceptable = false;
    std::string text;
};

decision install_decision(const plug::inspection& found, bool allow_unsigned)
{
    decision out;
    std::string detail;
    const vss::error_code why =
        plug::install_refusal(found, allow_unsigned, &detail);
    if (!why.failed())
    {
        out.acceptable = true;
        out.text = "can be installed";
        return out;
    }
    if (why == plug::plugin_errc::signature_missing)
    {
        out.text = "cannot be installed unless you give --allow-unsigned, "
                   "because it is not signed";
        return out;
    }
    if (why == plug::plugin_errc::key_unknown)
    {
        out.text = "cannot be installed unless you give --allow-unsigned, "
                   "because it is signed with a key nobody has approved, so "
                   "nothing shows who made it";
        return out;
    }
    out.text = "cannot be installed, " + sup::code_text(why);
    if (!detail.empty())
        out.text += ": " + sup::plain(detail);
    return out;
}

std::string heading_of(const plug::inspection& found, const fs::path& folder)
{
    if (!found.declared.has_value())
        return sup::plain(sup::path_text(folder.filename()));
    return sup::plain(found.declared->id) + "  " +
           sup::plain(plug::to_string(found.declared->version)) + "  " +
           sup::plain(found.declared->name);
}

std::string verify_text(const plug::inspection& found,
                        const plug::inspect_context& context,
                        const decision& verdict)
{
    std::string out = heading_of(found, found.directory);
    out += '\n';
    add_line(out, "Folder:       ", sup::plain(sup::path_text(found.directory)));
    if (!found.files.empty())
        add_line(out, "Fingerprint:  ", found.hash.short_text());
    if (found.problem.failed())
    {
        std::string problem = sup::code_text(found.problem);
        if (!found.detail.empty())
            problem += ": " + found.detail;
        add_line(out, "Problem:      ", sup::plain(problem));
    }
    sup::add_verdict_lines(out, found, context);
    add_line(out, "Install:      ", verdict.text);
    return out;
}

json::value verify_json(const plug::inspection& found,
                        const plug::inspect_context& context,
                        const decision& verdict)
{
    json::object item;
    if (found.declared.has_value())
    {
        item["id"] = sup::text_value(found.declared->id);
        item["version"] =
            sup::text_value(plug::to_string(found.declared->version));
        item["name"] = sup::text_value(found.declared->name);
    }
    else
    {
        item["id"] = nullptr;
        item["version"] = nullptr;
        item["name"] = nullptr;
    }
    item["folder"] = sup::text_value(sup::path_text(found.directory));
    if (found.files.empty())
        item["fingerprint"] = nullptr;
    else
        item["fingerprint"] = sup::text_value(found.hash.short_text());
    item["problem"] = found.problem.failed()
                          ? sup::text_value(sup::code_text(found.problem))
                          : json::value(nullptr);
    item["detail"] = sup::text_value(found.detail);
    sup::add_verdict_json(item, found, context);
    item["installable"] = verdict.acceptable;
    item["install"] = sup::text_value(verdict.text);
    return json::value(std::move(item));
}

/// The plugin folder to install into or remove from, or nothing, with the
/// reason on the error stream.
std::optional<fs::path> target_of(const options& given, std::string_view who)
{
    if (given.root.has_value())
        return given.root;
    const plug::plugin_locations places =
        plug::plugin_locations_for(sup::program_directory());
    const plug::plugin_scope wanted = given.for_everyone
                                          ? plug::plugin_scope::for_everyone
                                          : plug::plugin_scope::for_user;
    for (const plug::plugin_root& root : places.roots)
    {
        if (root.scope == wanted)
            return root.directory;
    }
    std::fprintf(stderr,
                 "plugin %.*s: cannot work out the plugin folder %s on this "
                 "computer; give one with --root\n",
                 static_cast<int>(who.size()), who.data(),
                 given.for_everyone ? "for everyone" : "for you");
    return std::nullopt;
}

} // namespace

const char* plugin_install_usage() noexcept
{
    return "  vss_cli plugin verify <folder> [--allow-unsigned] [--json]\n"
           "      Check a plugin folder the way Studio does before a run and "
           "say\n"
           "      what it found: signature, who stands behind it, the key, "
           "whether\n"
           "      it may run on this computer and why not, whether the "
           "licence\n"
           "      covers it, and whether it can be installed.  It reads only. "
           "Exit\n"
           "      code 0 means it can be installed, 1 that it cannot.  A "
           "plugin\n"
           "      with no signature can be installed only with "
           "--allow-unsigned.\n"
           "\n"
           "  vss_cli plugin install <folder> [--for-everyone] [--replace]\n"
           "                         [--allow-unsigned] [--root <folder>]\n"
           "      Check a plugin folder and copy it into your plugin folder. "
           "With\n"
           "      --for-everyone it goes into the folder of everyone on this\n"
           "      computer, which needs a prompt that may write there; "
           "nothing is\n"
           "      raised for you.  --replace puts it in place of an installed "
           "copy\n"
           "      with the same id.  --root names the plugin folder to use.  "
           "The\n"
           "      plugin is not run.  Exit code 0 means it was installed.\n"
           "\n"
           "  vss_cli plugin remove <id> [--for-everyone] [--root <folder>]\n"
           "      Remove an installed plugin's folder.  Its data is kept.\n"
           "\n";
}

int command_plugin_verify(const std::vector<std::string>& arguments)
{
    options given;
    if (!parse(arguments, given))
    {
        std::fputs(plugin_install_usage(), stderr);
        return exit_usage;
    }
    const sup::computer computer = sup::this_computer(true);
    const plug::inspection found = plug::inspect_plugin(
        fs::path(given.subject), computer.context);
    const decision verdict = install_decision(found, given.allow_unsigned);

    if (given.json)
    {
        std::string text =
            json::serialize(verify_json(found, computer.context, verdict));
        text += '\n';
        print(text);
    }
    else
    {
        std::string text = verify_text(found, computer.context, verdict);
        for (const std::string& note : computer.notes)
            text += "Note: " + sup::plain(note) + "\n";
        print(text);
    }
    return verdict.acceptable ? exit_ok : exit_refused;
}

int command_plugin_install(const std::vector<std::string>& arguments)
{
    options given;
    if (!parse(arguments, given))
    {
        std::fputs(plugin_install_usage(), stderr);
        return exit_usage;
    }
    const std::optional<fs::path> target = target_of(given, "install");
    if (!target.has_value())
        return exit_refused;

    const sup::computer computer = sup::this_computer(true);
    plug::install_options what;
    what.allow_unsigned = given.allow_unsigned;
    what.replace = given.replace;
    what.for_everyone = given.for_everyone;
    what.context = computer.context;

    std::string detail;
    const vss::result<plug::install_report> done = plug::install_plugin(
        fs::path(given.subject), *target, what, &detail);
    if (done.has_error())
    {
        std::string text = "plugin install: not installed, " +
                           sup::code_text(done.error());
        if (!detail.empty())
            text += ": " + sup::plain(detail);
        text += '\n';
        std::fputs(text.c_str(), stderr);
        return exit_refused;
    }

    const plug::install_report& report = done.value();
    const plug::inspection& found = report.found;
    std::string text = report.replaced ? "Replaced " : "Installed ";
    text += heading_of(found, found.directory);
    text += '\n';
    add_line(text, "Folder:       ", sup::plain(sup::path_text(report.directory)));
    add_line(text, "Fingerprint:  ", found.hash.short_text());
    sup::add_verdict_lines(text, found, computer.context);
    if (!report.warning.empty())
        text += "Warning: " + sup::plain(report.warning) + "\n";
    for (const std::string& note : computer.notes)
        text += "Note: " + sup::plain(note) + "\n";
    print(text);
    return exit_ok;
}

int command_plugin_remove(const std::vector<std::string>& arguments)
{
    options given;
    if (!parse(arguments, given))
    {
        std::fputs(plugin_install_usage(), stderr);
        return exit_usage;
    }
    const std::optional<fs::path> target = target_of(given, "remove");
    if (!target.has_value())
        return exit_refused;

    std::string detail;
    plug::remove_options removing;
    removing.for_everyone = given.for_everyone;
    const vss::result<void> done =
        plug::remove_plugin(*target, given.subject, removing, &detail);
    if (done.has_error())
    {
        std::string text = "plugin remove: not removed, " +
                           sup::code_text(done.error());
        if (!detail.empty())
            text += ": " + sup::plain(detail);
        text += '\n';
        std::fputs(text.c_str(), stderr);
        return exit_refused;
    }
    print("Removed " + sup::plain(given.subject) + " from " +
          sup::plain(sup::path_text(*target)) +
          ". Its data directory was not touched.\n");
    return exit_ok;
}

} // namespace vss_cli
