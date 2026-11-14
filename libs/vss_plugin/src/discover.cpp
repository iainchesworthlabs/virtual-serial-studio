//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/discover.hpp>

#include <vss/plugin/measure.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vss::plugin
{

namespace
{

namespace fs = std::filesystem;

/// `text` in quotes for a message to a person.  A name in a plugin folder
/// was chosen by someone else and has not been checked yet when it is first
/// shown, so it is cut short and anything that is not plain ASCII is
/// replaced.
std::string in_quotes(std::string_view text)
{
    constexpr std::size_t longest = 240;
    std::string out;
    out.reserve(std::min(text.size(), longest) + 5);
    out += '"';
    for (const char c : text.substr(0, longest))
    {
        out += c >= 0x20 && c <= 0x7e && c != '"' ? c : '?';
    }
    if (text.size() > longest)
    {
        out += "...";
    }
    out += '"';
    return out;
}

/// A line that says what is wrong with one file or directory.
std::string about(std::string_view kind, std::string_view name,
                  std::string_view problem)
{
    std::string out(kind);
    out += ' ';
    out += in_quotes(name);
    out += ' ';
    out += problem;
    return out;
}

/// The path as UTF-8 text with `/` between its names, whatever the system's
/// own spelling is.
std::string utf8_text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    std::string out;
    out.reserve(text.size());
    for (const char8_t unit : text)
    {
        out += static_cast<char>(unit);
    }
    return out;
}

/// The path without a trailing separator.  A trailing separator makes the
/// system resolve a link in the last name, so a folder is looked at without
/// it.
fs::path without_separator(const fs::path& directory)
{
    fs::path out = directory;
    if (!out.has_filename() && out.has_relative_path())
    {
        out = out.parent_path();
    }
    return out;
}

/// The path of a file with a known name inside `directory`.
fs::path inside(const fs::path& directory, std::string_view name)
{
    return directory / fs::path(name);
}

/// Why an entry of a root that is not a plain file or directory is refused.
std::string_view not_plain(fs::file_type type) noexcept
{
    return type == fs::file_type::symlink
               ? "is a link"
               : "is neither a plain file nor a plain directory";
}

/// One entry of a root, kept with its name until all of them are known.
struct root_entry
{
    /// The name as UTF-8 text: what the entries are sorted by.
    std::string name;
    fs::path path;
};

void report_root(discovery& out, const plugin_root& root, error_code problem,
                 std::string detail)
{
    root_problem item;
    item.directory = root.directory;
    item.scope = root.scope;
    item.problem = problem;
    item.detail = std::move(detail);
    out.roots.push_back(std::move(item));
}

/// Lists the entries of `directory`.  False when the directory or one of its
/// entries cannot be read.
bool list_entries(const fs::path& directory, std::vector<root_entry>& entries)
{
    std::error_code ec;
    fs::directory_iterator iterator(directory, ec);
    if (ec)
    {
        return false;
    }
    while (iterator != fs::directory_iterator{})
    {
        root_entry item;
        item.path = iterator->path();
        item.name = utf8_text(item.path.filename());
        entries.push_back(std::move(item));
        iterator.increment(ec);
        if (ec)
        {
            return false;
        }
    }
    return true;
}

/// Records why a plugin cannot be used.
void fail(found_plugin& plugin, error_code problem, std::string detail)
{
    plugin.problem = problem;
    plugin.detail = std::move(detail);
}

/// A plugin folder that is looked at: its manifest, its files and its
/// signature, stopping at the first problem.
void read_plugin(found_plugin& plugin)
{
    std::string detail;

    result<std::string> text =
        read_plugin_file(inside(plugin.directory, manifest_file_name),
                         max_manifest_size, &detail);
    if (text.has_error())
    {
        fail(plugin, text.error(), std::move(detail));
        return;
    }
    plugin.manifest_bytes = std::move(*text);

    result<manifest> parsed = parse_manifest(plugin.manifest_bytes, &detail);
    if (parsed.has_error())
    {
        fail(plugin, parsed.error(), std::move(detail));
        return;
    }
    plugin.declared = std::move(*parsed);

    result<std::vector<file_entry>> measured =
        measure_directory(plugin.directory, &detail);
    if (measured.has_error())
    {
        fail(plugin, measured.error(), std::move(detail));
        return;
    }
    plugin.files = std::move(*measured);
    plugin.hash = hash_plugin(plugin.files);

    const result<void> checked =
        check_files(*plugin.declared, plugin.files, &detail);
    if (checked.has_error())
    {
        fail(plugin, checked.error(), std::move(detail));
        return;
    }

    const fs::path signature = inside(plugin.directory, signature_file_name);
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(signature, ec);
    if (status.type() == fs::file_type::not_found)
    {
        return;
    }
    result<std::string> bytes =
        read_plugin_file(signature, max_signature_size, &detail);
    if (bytes.has_error())
    {
        fail(plugin, bytes.error(), std::move(detail));
        return;
    }
    plugin.signature_bytes = std::move(*bytes);
}

/// The start of a `found_plugin`: the entry's folder and the root's scope.
found_plugin start_plugin(const plugin_root& root, const root_entry& entry)
{
    found_plugin plugin;
    plugin.directory = entry.path;
    plugin.scope = root.scope;
    return plugin;
}

/// Adds a `found_plugin` for an entry that is not read, and says why.  Once
/// the limit is reached the reason is `too_many` whatever the entry is.
void add_refused(discovery& out, const plugin_root& root,
                 const root_entry& entry, error_code problem,
                 std::string detail)
{
    found_plugin plugin = start_plugin(root, entry);
    if (out.plugins.size() >= max_plugins)
    {
        plugin.problem = make_error_code(plugin_errc::too_many);
    }
    else
    {
        fail(plugin, problem, std::move(detail));
    }
    out.plugins.push_back(std::move(plugin));
}

/// Looks at one entry of a root.  A plain file is passed over, and so is a
/// directory without a `plugin.json`.
void take_entry(const plugin_root& root, const root_entry& entry,
                discovery& out)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(entry.path, ec);
    const fs::file_type type = status.type();
    if (type == fs::file_type::not_found)
    {
        // It was there when the root was listed and is gone now.
        return;
    }
    if (ec)
    {
        add_refused(out, root, entry, make_error_code(errc::io_error),
                    about("plugin folder", entry.name, "cannot be examined"));
        return;
    }
    if (type == fs::file_type::regular)
    {
        return;
    }
    if (type != fs::file_type::directory)
    {
        add_refused(out, root, entry,
                    make_error_code(plugin_errc::layout_invalid),
                    about("plugin folder", entry.name, not_plain(type)));
        return;
    }

    // Anything but "not found" for `plugin.json` makes the folder a plugin: a
    // `plugin.json` that is a link or a directory is then refused when it is
    // read.
    const fs::file_status manifest_status =
        fs::symlink_status(inside(entry.path, manifest_file_name), ec);
    if (manifest_status.type() == fs::file_type::not_found)
    {
        return;
    }

    if (out.plugins.size() >= max_plugins)
    {
        add_refused(out, root, entry, error_code{}, std::string{});
        return;
    }
    found_plugin plugin = start_plugin(root, entry);
    read_plugin(plugin);
    out.plugins.push_back(std::move(plugin));
}

/// Looks through one root.
void look_through(const plugin_root& root, discovery& out)
{
    const fs::path directory = without_separator(root.directory);
    const std::string shown = utf8_text(root.directory);

    std::error_code ec;
    const fs::file_status status = fs::symlink_status(directory, ec);
    if (status.type() == fs::file_type::not_found)
    {
        return;
    }
    if (ec)
    {
        report_root(out, root, make_error_code(errc::io_error),
                    about("folder", shown, "cannot be examined"));
        return;
    }
    if (status.type() != fs::file_type::directory)
    {
        report_root(out, root, make_error_code(plugin_errc::layout_invalid),
                    about("folder", shown,
                          status.type() == fs::file_type::symlink
                              ? "is a link"
                              : "is not a directory"));
        return;
    }

    std::vector<root_entry> entries;
    if (!list_entries(directory, entries))
    {
        report_root(out, root, make_error_code(errc::io_error),
                    about("folder", shown, "cannot be read"));
        return;
    }
    std::ranges::sort(entries, {}, &root_entry::name);

    for (const root_entry& entry : entries)
    {
        take_entry(root, entry, out);
    }
}

/// The line for a plugin that another copy hides.
std::string shadow_detail(const std::string& id, const fs::path& winner)
{
    std::string out = "id ";
    out += in_quotes(id);
    out += " is also used by ";
    out += in_quotes(utf8_text(winner));
    out += ", which was found first";
    return out;
}

/// Marks each plugin whose id an earlier plugin has.  The earlier one counts
/// even when it has a problem of its own.
void mark_shadowed(std::vector<found_plugin>& plugins)
{
    std::unordered_map<std::string_view, std::size_t> first;
    for (std::size_t i = 0; i < plugins.size(); ++i)
    {
        found_plugin& plugin = plugins[i];
        if (!plugin.declared)
        {
            continue;
        }
        const auto [place, added] = first.try_emplace(plugin.declared->id, i);
        if (added)
        {
            continue;
        }
        plugin.shadowed_by = place->second;
        if (!plugin.problem.failed())
        {
            fail(plugin, make_error_code(plugin_errc::shadowed),
                 shadow_detail(plugin.declared->id,
                               plugins[place->second].directory));
        }
    }
}

/// Reports that reading a root failed in a way that is not the root's own
/// doing.  Running out of memory is the only way to get here, so this too
/// may fail, and then there is nothing more to say.
void report_exception(discovery& out, std::span<const plugin_root> roots,
                      std::size_t reading)
{
    if (roots.empty())
    {
        return;
    }
    const plugin_root& root = roots[std::min(reading, roots.size() - 1)];
    try
    {
        report_root(out, root, make_error_code(errc::io_error),
                    about("folder", utf8_text(root.directory),
                          "cannot be looked through"));
    }
    catch (const std::exception&)
    {
        // Best effort: what was found so far is returned without the line.
    }
}

} // namespace

std::string_view to_string(plugin_scope value) noexcept
{
    switch (value)
    {
    case plugin_scope::with_studio:
        return "with_studio";
    case plugin_scope::for_everyone:
        return "for_everyone";
    case plugin_scope::for_user:
        return "for_user";
    }
    return "unknown";
}

discovery discover_plugins(std::span<const plugin_root> roots)
{
    discovery out;
    std::size_t reading = 0;
    // Every call above reports through an error code, so only running out of
    // memory or a name the library cannot convert can throw.
    try
    {
        for (; reading < roots.size(); ++reading)
        {
            look_through(roots[reading], out);
        }
        mark_shadowed(out.plugins);
    }
    catch (const std::exception&)
    {
        report_exception(out, roots, reading);
    }
    return out;
}

} // namespace vss::plugin
