//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/bundle_sections.hpp>

#include <vss/plugin/hash.hpp>
#include <vss/plugin/host/decode_store.hpp>
#include <vss/plugin/manifest.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <fstream>
#include <ios>
#include <set>
#include <stdexcept>
#include <span>
#include <system_error>
#include <vector>

namespace vss::plugin::host
{

namespace
{

namespace fs = std::filesystem;

constexpr std::size_t digest_hex_length = 128;
constexpr std::size_t read_chunk = std::size_t{64} * 1024;

bundle_failure failure(bundle_problem problem, const std::string& section,
                       const std::string& path, std::string detail)
{
    bundle_failure out;
    out.problem = problem;
    out.section = section;
    out.path = path;
    out.detail = std::move(detail);
    return out;
}

bool is_token(std::string_view text) noexcept
{
    return !text.empty() && text.size() <= max_section_id &&
           std::ranges::all_of(text, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-' || c == '_';
           });
}

bool is_lower_hex(std::string_view text) noexcept
{
    return text.size() == digest_hex_length &&
           std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool id_allowed(std::string_view id, const section_rules& rules)
{
    if (std::ranges::find(rules.reserved_ids, id) != rules.reserved_ids.end())
    {
        return true;
    }
    if (rules.plugin_id.empty() || id.size() <= rules.plugin_id.size() + 1)
    {
        return false;
    }
    return id.starts_with(rules.plugin_id) && id[rules.plugin_id.size()] == '.';
}

std::optional<bundle_failure> check_one(const offered_section& section,
                                        const section_rules& rules)
{
    const std::string& id = section.id;
    if (!is_token(id))
    {
        return failure(bundle_problem::id_bad, id, {},
                       "the id is not a token of 1 to 128 bytes");
    }
    if (!id_allowed(id, rules))
    {
        return failure(bundle_problem::id_not_allowed, id, {},
                       "the id is not the plugin's own and not reserved");
    }
    const text_check title = check_text(section.title);
    if (section.title.empty() || section.title.size() > max_section_title ||
        !title.valid_utf8 || title.marks.control || title.marks.bidi)
    {
        return failure(bundle_problem::title_bad, id, {},
                       "the title is empty, too long, not UTF-8 or has a "
                       "control or direction character");
    }
    if (section.files.size() > max_section_files)
    {
        return failure(bundle_problem::too_many_files, id, {},
                       "the section has more files than the limit");
    }

    std::set<std::string> folded;
    std::uint64_t total = 0;
    for (const offered_file& file : section.files)
    {
        if (const auto why = check_relative_path(file.path))
        {
            std::string detail = "the path ";
            detail += *why;
            return failure(bundle_problem::path_bad, id, file.path,
                           std::move(detail));
        }
        if (!folded.insert(fold_path(file.path)).second)
        {
            return failure(bundle_problem::path_twice, id, file.path,
                           "the path is listed twice, or twice when case is "
                           "ignored");
        }
        if (file.size > max_section_bytes)
        {
            return failure(bundle_problem::size_over_limit, id, file.path,
                           "the file is larger than the limit of a section");
        }
        total += file.size;
        if (total > max_section_bytes)
        {
            return failure(bundle_problem::section_over_limit, id, file.path,
                           "the files together are larger than the limit");
        }
        if (!is_lower_hex(file.sha512))
        {
            return failure(bundle_problem::hash_bad, id, file.path,
                           "the hash is not 128 lower-case hex characters");
        }
    }
    return std::nullopt;
}

/// The names of `path`, which `check_relative_path` has passed.
std::vector<std::string> names_of(std::string_view path)
{
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t end = std::min(path.find('/', start), path.size());
        names.emplace_back(path.substr(start, end - start));
        start = end + 1;
    }
    return names;
}

/// Walks `path` below `directory`, name by name, and refuses a link or
/// anything that is not a plain directory on the way and a plain file at the
/// end.  Nothing is followed.
std::optional<bundle_failure> walk(const fs::path& directory,
                                   const std::string& section,
                                   const std::string& path, fs::path& full)
{
    full = directory;
    const std::vector<std::string> names = names_of(path);
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        full /= fs::path(names[i]);
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(full, ec);
        if (status.type() == fs::file_type::not_found)
        {
            return failure(bundle_problem::file_missing, section, path,
                           "the file is not there");
        }
        if (ec)
        {
            return failure(bundle_problem::file_unreadable, section, path,
                           "the file cannot be examined");
        }
        if (status.type() == fs::file_type::symlink)
        {
            return failure(bundle_problem::file_is_link, section, path,
                           "a part of the path is a link");
        }
        const bool last = i + 1 == names.size();
        const fs::file_type wanted =
            last ? fs::file_type::regular : fs::file_type::directory;
        if (status.type() != wanted)
        {
            return failure(bundle_problem::file_not_plain, section, path,
                           last ? "the path is not a plain file"
                                : "a part of the path is not a plain folder");
        }
    }
    return std::nullopt;
}

/// Opens `full` once and reads its size and hash from that handle.
std::optional<bundle_failure> read_file(const fs::path& full,
                                        const offered_file& file,
                                        const std::string& section,
                                        verified_file& out)
{
    std::ifstream stream(full, std::ios::binary);
    if (!stream)
    {
        return failure(bundle_problem::file_unreadable, section, file.path,
                       "the file cannot be opened");
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff end = stream.tellg();
    if (!stream || end < 0)
    {
        return failure(bundle_problem::file_unreadable, section, file.path,
                       "the size cannot be read from the open file");
    }
    if (static_cast<std::uint64_t>(end) != file.size)
    {
        return failure(bundle_problem::size_changed, section, file.path,
                       "the file's size is not the listed size");
    }
    stream.seekg(0, std::ios::beg);

    // Never more than the listed size and one byte: a file that grows is
    // found, and not read to its end.
    sha512_hasher hasher;
    std::vector<char> buffer(read_chunk);
    std::uint64_t seen = 0;
    while (seen <= file.size)
    {
        const std::uint64_t room = file.size + 1 - seen;
        const auto want = static_cast<std::streamsize>(
            std::min<std::uint64_t>(room, buffer.size()));
        stream.read(buffer.data(), want);
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (count > 0)
        {
            hasher.update(std::as_bytes(
                std::span<const char>(buffer.data(), count)));
            seen += count;
        }
        if (stream.bad())
        {
            return failure(bundle_problem::file_unreadable, section, file.path,
                           "the file cannot be read");
        }
        if (stream.eof())
        {
            break;
        }
        if (stream.fail())
        {
            return failure(bundle_problem::file_unreadable, section, file.path,
                           "the file cannot be read");
        }
    }
    if (seen != file.size)
    {
        return failure(bundle_problem::size_changed, section, file.path,
                       "the bytes read are not the listed size");
    }
    out.sha512 = to_hex(hasher.finish());
    // A file listed without a hash is being measured, and is not compared.
    if (!file.sha512.empty() && out.sha512 != file.sha512)
    {
        return failure(bundle_problem::hash_changed, section, file.path,
                       "the file's hash is not the listed hash");
    }
    out.size = seen;
    return std::nullopt;
}

} // namespace

std::string_view to_string(bundle_problem problem) noexcept
{
    switch (problem)
    {
    case bundle_problem::too_many_sections:
        return "too_many_sections";
    case bundle_problem::id_bad:
        return "id_bad";
    case bundle_problem::id_not_allowed:
        return "id_not_allowed";
    case bundle_problem::id_twice:
        return "id_twice";
    case bundle_problem::title_bad:
        return "title_bad";
    case bundle_problem::too_many_files:
        return "too_many_files";
    case bundle_problem::path_bad:
        return "path_bad";
    case bundle_problem::path_twice:
        return "path_twice";
    case bundle_problem::size_over_limit:
        return "size_over_limit";
    case bundle_problem::section_over_limit:
        return "section_over_limit";
    case bundle_problem::hash_bad:
        return "hash_bad";
    case bundle_problem::directory_bad:
        return "directory_bad";
    case bundle_problem::file_missing:
        return "file_missing";
    case bundle_problem::file_is_link:
        return "file_is_link";
    case bundle_problem::file_not_plain:
        return "file_not_plain";
    case bundle_problem::file_unreadable:
        return "file_unreadable";
    case bundle_problem::size_changed:
        return "size_changed";
    case bundle_problem::hash_changed:
        return "hash_changed";
    }
    return "unknown";
}

void throw_exception_from_error(const bundle_failure& failure,
                                const boost::source_location&)
{
    throw std::runtime_error(std::string(to_string(failure.problem)));
}

std::optional<bundle_failure>
check_sections(std::span<const offered_section> sections,
               const section_rules& rules)
{
    if (sections.size() > max_offered_sections)
    {
        return failure(bundle_problem::too_many_sections, {}, {},
                       "the plugin offers more sections than the limit");
    }
    std::set<std::string> seen;
    for (const offered_section& section : sections)
    {
        if (auto found = check_one(section, rules))
        {
            return found;
        }
        if (!seen.insert(section.id).second)
        {
            return failure(bundle_problem::id_twice, section.id, {},
                           "the id is used by two sections");
        }
    }
    return std::nullopt;
}

boost::system::result<std::vector<verified_section>, bundle_failure>
verify_sections(const std::filesystem::path& data_directory,
                std::span<const offered_section> sections,
                const section_rules& rules)
{
    using outcome =
        boost::system::result<std::vector<verified_section>, bundle_failure>;
    try
    {
        if (auto found = check_sections(sections, rules))
        {
            return outcome(boost::system::in_place_error, std::move(*found));
        }

        std::error_code ec;
        const fs::file_status root = fs::symlink_status(data_directory, ec);
        if (ec || root.type() != fs::file_type::directory)
        {
            return outcome(
                boost::system::in_place_error,
                failure(bundle_problem::directory_bad, {}, {},
                        "the data directory is missing, a link or not a "
                        "folder"));
        }

        std::vector<verified_section> verified;
        for (const offered_section& section : sections)
        {
            verified_section done;
            done.id = section.id;
            done.title = section.title;
            done.sensitive = section.sensitive;
            for (const offered_file& file : section.files)
            {
                verified_file checked;
                checked.path = file.path;
                if (auto found = walk(data_directory, section.id, file.path,
                                      checked.full_path))
                {
                    return outcome(boost::system::in_place_error,
                                   std::move(*found));
                }
                if (auto found =
                        read_file(checked.full_path, file, section.id, checked))
                {
                    return outcome(boost::system::in_place_error,
                                   std::move(*found));
                }
                // The name is looked at again after the read: a link put in
                // its place while the file was read is found.
                fs::path again;
                if (auto found =
                        walk(data_directory, section.id, file.path, again))
                {
                    return outcome(boost::system::in_place_error,
                                   std::move(*found));
                }
                done.bytes += checked.size;
                done.files.push_back(std::move(checked));
            }
            verified.push_back(std::move(done));
        }
        return verified;
    }
    catch (const std::exception&)
    {
        return outcome(boost::system::in_place_error,
                       failure(bundle_problem::file_unreadable, {}, {},
                               "the files could not be checked"));
    }
}

boost::system::result<verified_file, bundle_failure>
measure_file(const std::filesystem::path& data_directory,
             const std::string& section, const std::string& path,
             std::uint64_t listed_size)
{
    using outcome = boost::system::result<verified_file, bundle_failure>;
    try
    {
        if (const auto why = check_relative_path(path))
        {
            std::string detail = "the path ";
            detail += *why;
            return outcome(boost::system::in_place_error,
                           failure(bundle_problem::path_bad, section, path,
                                   std::move(detail)));
        }
        std::error_code ec;
        const fs::file_status root = fs::symlink_status(data_directory, ec);
        if (ec || root.type() != fs::file_type::directory)
        {
            return outcome(boost::system::in_place_error,
                           failure(bundle_problem::directory_bad, {}, {},
                                   "the data directory is missing, a link or "
                                   "not a folder"));
        }
        verified_file checked;
        checked.path = path;
        if (auto found = walk(data_directory, section, path,
                              checked.full_path))
        {
            return outcome(boost::system::in_place_error, std::move(*found));
        }
        offered_file listed;
        listed.path = path;
        listed.size = listed_size;
        if (auto found = read_file(checked.full_path, listed, section, checked))
        {
            return outcome(boost::system::in_place_error, std::move(*found));
        }
        fs::path again;
        if (auto found = walk(data_directory, section, path, again))
        {
            return outcome(boost::system::in_place_error, std::move(*found));
        }
        return checked;
    }
    catch (const std::exception&)
    {
        return outcome(boost::system::in_place_error,
                       failure(bundle_problem::file_unreadable, section, path,
                               "the file could not be checked"));
    }
}

} // namespace vss::plugin::host
