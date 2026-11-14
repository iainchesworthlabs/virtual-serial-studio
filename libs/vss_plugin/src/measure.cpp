//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/measure.hpp>

#include <vss/plugin/hash.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss::plugin
{

namespace
{

namespace fs = std::filesystem;

/// How many bytes are read from a file at a time.
constexpr std::size_t read_chunk = std::size_t{64} * 1024;

/// What a walk of the directory has found so far.
struct measurement
{
    std::vector<file_entry> files;
    std::vector<char> buffer = std::vector<char>(read_chunk);
    std::uintmax_t total = 0;
};

void set_detail(std::string* detail, std::string text)
{
    if (detail != nullptr)
    {
        *detail = std::move(text);
    }
}

boost::system::error_code refuse(plugin_errc code, std::string* detail,
                                 std::string text)
{
    set_detail(detail, std::move(text));
    return make_error_code(code);
}

boost::system::error_code refuse(errc code, std::string* detail,
                                 std::string text)
{
    set_detail(detail, std::move(text));
    return make_error_code(code);
}

/// `text` in quotes for a message to a person.  A name in a plugin's
/// directory was chosen by someone else and has not been checked yet when
/// it is first shown, so it is cut short and anything that is not plain
/// ASCII is replaced.
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

std::string join_names(const std::string& parent, const fs::path& name)
{
    std::string out = parent;
    if (!out.empty())
    {
        out += '/';
    }
    out += utf8_text(name);
    return out;
}

/// Why an entry that is not a plain file or directory is refused.
std::string_view not_plain(fs::file_type type) noexcept
{
    return type == fs::file_type::symlink
               ? "is a link"
               : "is neither a plain file nor a plain directory";
}

std::string over_the_bytes_limit()
{
    std::string text = "takes the plugin over the limit of ";
    text += std::to_string(max_plugin_bytes);
    text += " bytes";
    return text;
}

/// Hashes one file in chunks and adds it to `found`.
result<void> add_file(const fs::path& path, const std::string& relative,
                      measurement& found, std::string* detail)
{
    if (found.files.size() >= max_measured_files)
    {
        return refuse(plugin_errc::layout_invalid, detail,
                      "the plugin has more files than the limit of " +
                          std::to_string(max_measured_files));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return refuse(errc::io_error, detail,
                      about("file", relative, "cannot be opened"));
    }

    sha512_hasher hasher;
    std::uint64_t size = 0;
    while (true)
    {
        stream.read(found.buffer.data(),
                    static_cast<std::streamsize>(found.buffer.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (count > 0)
        {
            size += count;
            found.total += count;
            if (found.total > max_plugin_bytes)
            {
                return refuse(plugin_errc::layout_invalid, detail,
                              about("file", relative, over_the_bytes_limit()));
            }
            hasher.update(std::as_bytes(
                std::span<const char>(found.buffer.data(), count)));
        }
        if (stream.bad())
        {
            return refuse(errc::io_error, detail,
                          about("file", relative, "cannot be read"));
        }
        if (stream.eof())
        {
            break;
        }
        if (stream.fail())
        {
            return refuse(errc::io_error, detail,
                          about("file", relative, "cannot be read"));
        }
    }

    file_entry entry;
    entry.path = relative;
    entry.size = size;
    entry.sha512 = hasher.finish();
    found.files.push_back(std::move(entry));
    return {};
}

result<void> walk(const fs::path& directory, const std::string& shown,
                  const std::string& relative, std::size_t depth,
                  measurement& found, std::string* detail);

/// Examines one entry of a directory: a plain file is measured, a plain
/// directory is walked and anything else is refused.  Nothing is followed:
/// `symlink_status` looks at the entry itself.
result<void> visit(const fs::path& entry, const std::string& parent,
                   std::size_t depth, measurement& found, std::string* detail)
{
    const std::string relative = join_names(parent, entry.filename());

    std::error_code ec;
    const fs::file_status status = fs::symlink_status(entry, ec);
    if (ec)
    {
        return refuse(errc::io_error, detail,
                      about("file", relative, "cannot be examined"));
    }
    const fs::file_type type = status.type();
    if (type != fs::file_type::regular && type != fs::file_type::directory)
    {
        return refuse(plugin_errc::layout_invalid, detail,
                      about("file", relative, not_plain(type)));
    }
    if (const std::optional<std::string> wrong = check_relative_path(relative))
    {
        return refuse(plugin_errc::layout_invalid, detail,
                      "file " + in_quotes(relative) + ": " + *wrong);
    }
    if (depth > max_plugin_depth)
    {
        return refuse(plugin_errc::layout_invalid, detail,
                      about("file", relative,
                            "lies deeper than " +
                                std::to_string(max_plugin_depth) +
                                " names"));
    }
    if (type == fs::file_type::directory)
    {
        return walk(entry, relative, relative, depth, found, detail);
    }
    return add_file(entry, relative, found, detail);
}

/// Measures what is in `directory`, which lies at `depth` (the plugin's own
/// directory is at 0).  `shown` is how it is named in a message and
/// `relative` is its path inside the plugin.
result<void> walk(const fs::path& directory, const std::string& shown,
                  const std::string& relative, std::size_t depth,
                  measurement& found, std::string* detail)
{
    std::error_code ec;
    fs::directory_iterator entries(directory, ec);
    if (ec)
    {
        return refuse(errc::io_error, detail,
                      about("directory", shown, "cannot be read"));
    }
    while (entries != fs::directory_iterator{})
    {
        const result<void> done =
            visit(entries->path(), relative, depth + 1, found, detail);
        if (done.has_error())
        {
            return done;
        }
        entries.increment(ec);
        if (ec)
        {
            return refuse(errc::io_error, detail,
                          about("directory", shown, "cannot be read"));
        }
    }
    return {};
}

result<std::vector<file_entry>> measure(const fs::path& directory,
                                        std::string* detail)
{
    // A trailing separator makes the system resolve a link in the last name,
    // so the plugin's directory is looked at without it.
    fs::path root = directory;
    if (!root.has_filename() && root.has_relative_path())
    {
        root = root.parent_path();
    }
    const std::string shown = utf8_text(directory);

    std::error_code ec;
    const fs::file_status status = fs::symlink_status(root, ec);
    if (status.type() == fs::file_type::not_found)
    {
        return refuse(errc::not_found, detail,
                      about("directory", shown, "is not there"));
    }
    if (ec)
    {
        return refuse(errc::io_error, detail,
                      about("directory", shown, "cannot be examined"));
    }
    if (status.type() != fs::file_type::directory)
    {
        return refuse(plugin_errc::layout_invalid, detail,
                      about("directory", shown,
                            status.type() == fs::file_type::symlink
                                ? "is a link"
                                : "is not a directory"));
    }

    measurement found;
    const result<void> done = walk(root, shown, std::string{}, 0, found,
                                   detail);
    if (done.has_error())
    {
        return done.error();
    }
    std::ranges::sort(found.files, {}, &file_entry::path);
    return std::move(found.files);
}

} // namespace

result<std::vector<file_entry>>
measure_directory(const std::filesystem::path& directory, std::string* detail)
{
    set_detail(detail, std::string{});
    // Every call above reports through an error code, so only running out of
    // memory or a name the library cannot convert can throw.
    try
    {
        return measure(directory, detail);
    }
    catch (const std::exception&)
    {
        return refuse(errc::io_error, detail,
                      "the plugin's directory cannot be measured");
    }
}

result<std::string> read_plugin_file(const std::filesystem::path& file,
                                     std::size_t max_size, std::string* detail)
{
    set_detail(detail, std::string{});
    try
    {
        const std::string shown = utf8_text(file);

        std::error_code ec;
        const fs::file_status status = fs::symlink_status(file, ec);
        if (status.type() == fs::file_type::not_found)
        {
            return refuse(plugin_errc::file_missing, detail,
                          about("file", shown, "is not there"));
        }
        if (ec)
        {
            return refuse(errc::io_error, detail,
                          about("file", shown, "cannot be examined"));
        }
        if (status.type() != fs::file_type::regular)
        {
            return refuse(plugin_errc::layout_invalid, detail,
                          about("file", shown,
                                status.type() == fs::file_type::symlink
                                    ? "is a link"
                                    : "is not a plain file"));
        }

        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            return refuse(errc::io_error, detail,
                          about("file", shown, "cannot be opened"));
        }

        // One byte more than `max_size` is enough to know it is too large,
        // and the buffer is never larger than a chunk.
        std::string text;
        std::vector<char> buffer(read_chunk);
        while (text.size() <= max_size)
        {
            const std::size_t room = max_size - text.size();
            const std::size_t want = room < buffer.size() ? room + 1
                                                          : buffer.size();
            stream.read(buffer.data(), static_cast<std::streamsize>(want));
            text.append(buffer.data(),
                        static_cast<std::size_t>(stream.gcount()));
            if (stream.bad())
            {
                return refuse(errc::io_error, detail,
                              about("file", shown, "cannot be read"));
            }
            if (stream.eof())
            {
                break;
            }
            if (stream.fail())
            {
                return refuse(errc::io_error, detail,
                              about("file", shown, "cannot be read"));
            }
        }
        if (text.size() > max_size)
        {
            return refuse(plugin_errc::manifest_too_large, detail,
                          about("file", shown,
                                "has more than " + std::to_string(max_size) +
                                    " bytes"));
        }
        return text;
    }
    catch (const std::exception&)
    {
        return refuse(errc::io_error, detail, "the file cannot be read");
    }
}

} // namespace vss::plugin
