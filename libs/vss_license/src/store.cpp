//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/license/store.hpp>

#include "strict_json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss::license
{

namespace
{

namespace fs = std::filesystem;
namespace json = boost::json;

/// The longest file name stem a licence id gives.
constexpr std::size_t max_name_length = 64;

/// A path as UTF-8 text, the same on every system.
std::string utf8_of(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

/// A file's bytes, or nothing when it cannot be read or is larger than a
/// signed file may be. The size is checked before the file is read, and the
/// read stops one byte past the limit in case the file grew meanwhile.
std::optional<std::string> read_signed_file(const fs::path& path)
{
    std::error_code failed;
    const std::uintmax_t size = fs::file_size(path, failed);
    if (failed || size > signed_file::max_file_size)
        return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size) + 1, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (file.bad())
        return std::nullopt;
    const auto count = static_cast<std::size_t>(file.gcount());
    if (count > signed_file::max_file_size)
        return std::nullopt;
    bytes.resize(count);
    return bytes;
}

/// Writes `bytes` to a file beside `target` and renames it to `target`, so
/// that no reader sees half a file. An existing `target` is replaced.
bool write_whole_file(const fs::path& requested, std::string_view bytes)
{
    // The path is resolved to its canonical form first, so the file that is
    // written is the one the path really leads to.
    std::error_code failed;
    const fs::path target = fs::weakly_canonical(requested, failed);
    if (failed)
        return false;
    fs::path temporary = target;
    temporary += ".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out)
        {
            out.close();
            fs::remove(temporary, failed);
            return false;
        }
    }
    fs::rename(temporary, target, failed);
    if (failed)
    {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool lower_equal(char left, char right)
{
    const auto lower = [](char c)
    { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return lower(left) == lower(right);
}

/// Whether `name` ends in a dot and `suffix`, in any ASCII case.
bool has_suffix(const std::string& name, std::string_view suffix)
{
    if (name.size() < suffix.size() + 1 ||
        name[name.size() - suffix.size() - 1] != '.')
        return false;
    return std::equal(suffix.begin(), suffix.end(),
                      name.end() - static_cast<std::ptrdiff_t>(suffix.size()),
                      lower_equal);
}

/// The regular files with `suffix` in `directory`, by name, at most
/// `max_files_per_folder`. A folder that is not there or cannot be listed has
/// none.
std::vector<fs::path> files_in(const fs::path& directory,
                               std::string_view suffix)
{
    std::vector<fs::path> found;
    if (directory.empty())
        return found;
    std::error_code failed;
    for (fs::directory_iterator entry(directory, failed);
         !failed && entry != fs::directory_iterator{}; entry.increment(failed))
    {
        std::error_code kind_failed;
        if (entry->is_regular_file(kind_failed) && !kind_failed &&
            has_suffix(utf8_of(entry->path().filename()), suffix))
            found.push_back(entry->path());
    }
    std::ranges::sort(
        found, [](const fs::path& left, const fs::path& right)
        { return utf8_of(left.filename()) < utf8_of(right.filename()); });
    if (found.size() > max_files_per_folder)
        found.resize(max_files_per_folder);
    return found;
}

/// A file name stem for a licence id: its letters, digits and hyphens. A
/// licence id is text from a file, so nothing else of it reaches the file
/// system.
std::string safe_name(const std::string& licence_id)
{
    std::string name;
    for (const char c : licence_id)
    {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-';
        if (keep && name.size() < max_name_length)
            name += c;
    }
    return name.empty() ? std::string("licence") : name;
}

bool same(const status& left, const status& right)
{
    return left.state == right.state && left.reason == right.reason &&
           left.value == right.value && left.key_id == right.key_id &&
           left.days_left == right.days_left;
}

bool same(const store_view& left, const store_view& right)
{
    if (!same(left.status, right.status) ||
        left.computer_name != right.computer_name ||
        left.files.size() != right.files.size())
        return false;
    for (std::size_t index = 0; index < left.files.size(); ++index)
    {
        const found_licence& a = left.files[index];
        const found_licence& b = right.files[index];
        if (!same(a.status, b.status) || a.file_name != b.file_name ||
            a.machine_wide != b.machine_wide || a.in_use != b.in_use)
            return false;
    }
    return true;
}

time_point system_now()
{
    return std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
}

} // namespace

store_options default_store_options()
{
    store_options options;
    options.user_directory = user_licence_directory();
    if (!options.user_directory.empty())
        options.clock_file =
            options.user_directory.parent_path() / "licence-clock.json";
    options.machine_directory = machine_licence_directory();
    options.keys = signed_file::product_trust_list();
    options.computer_name = computer_name();
    return options;
}

store::store(store_options options) : options_(std::move(options))
{
    if (!options_.clock)
        options_.clock = system_now;
    view_.computer_name = options_.computer_name;
    load_clock();
}

context store::judging_context(time_point now) const
{
    context where;
    where.now = now;
    where.latest_seen = clock_.latest_seen;
    where.build_time = options_.build_time;
    where.computer_name = options_.computer_name;
    where.keys = options_.keys;
    where.revocations = &revocations_;
    return where;
}

void store::load_clock()
{
    if (options_.clock_file.empty())
        return;
    const auto bytes = read_signed_file(options_.clock_file);
    if (!bytes)
        return;
    // A clock file that cannot be read is the same as none.
    const auto document = detail::parse_strict(*bytes);
    if (!document || !document->is_object())
        return;
    const json::object& object = document->as_object();
    if (const json::value* seen = object.if_contains("latest_seen");
        seen != nullptr && seen->is_string())
    {
        const auto time = parse_time(std::string_view(seen->as_string()));
        if (time)
            clock_.latest_seen = *time;
    }
    if (const json::value* resets = object.if_contains("applied_resets");
        resets != nullptr && resets->is_array())
    {
        for (const json::value& token : resets->as_array())
        {
            if (token.is_string())
                clock_.applied_resets.emplace_back(token.as_string());
        }
    }
}

void store::save_clock() const
{
    if (options_.clock_file.empty() || clock_.latest_seen == time_point::min())
        return;
    json::array resets;
    for (const std::string& token : clock_.applied_resets)
        resets.emplace_back(token);
    const std::string seen = format_time(clock_.latest_seen);
    json::object object;
    object["latest_seen"] = std::string_view(seen);
    object["applied_resets"] = std::move(resets);

    if (options_.clock_file.has_parent_path())
    {
        std::error_code failed;
        fs::create_directories(options_.clock_file.parent_path(), failed);
    }
    write_whole_file(options_.clock_file, json::serialize(object));
}

void store::load_revocations()
{
    // The list with the highest sequence number wins, wherever it is. The
    // one built into this release is the starting point; a signed list in a
    // licence folder replaces it only when its number is higher.
    revocations_ = builtin_revocations();
    for (const fs::path& directory :
         {options_.machine_directory, options_.user_directory})
    {
        for (const fs::path& path : files_in(directory, revocations_suffix))
        {
            const auto bytes = read_signed_file(path);
            if (!bytes)
                continue;
            const auto list = read_revocations(*bytes, options_.keys);
            if (!list)
                continue;
            revocations_ = newer(revocations_, *list);
        }
    }
}

bool store::refresh()
{
    const time_point now = options_.clock();
    load_revocations();
    const context where = judging_context(now);

    store_view found;
    found.computer_name = options_.computer_name;
    std::vector<license::status> statuses;
    const auto read_folder = [&](const fs::path& directory, bool machine_wide)
    {
        for (const fs::path& path : files_in(directory, licence_suffix))
        {
            const auto bytes = read_signed_file(path);
            found_licence file;
            file.file_name = utf8_of(path.filename());
            file.machine_wide = machine_wide;
            if (bytes)
            {
                file.status = evaluate(*bytes, where);
            }
            else
            {
                file.status.state = state::invalid;
                file.status.reason = reason::malformed;
            }
            statuses.push_back(file.status);
            found.files.push_back(std::move(file));
        }
    };
    read_folder(options_.machine_directory, true);
    read_folder(options_.user_directory, false);

    found.status = best(statuses);
    // Mark the file in use and put it first. `best` returns a copy, so the
    // first file with the same licence and state is the one.
    const auto used =
        std::ranges::find_if(found.files, [&](const found_licence& file)
                             { return same(file.status, found.status); });
    if (used != found.files.end())
    {
        used->in_use = true;
        std::rotate(found.files.begin(), used, used + 1);
    }

    // The time only moves forward here, and only a clock that is not behind
    // is written down.
    const clock_state seen = observe(clock_, now);
    if (seen != clock_)
    {
        clock_ = seen;
        save_clock();
    }

    const bool changed = !same(found, view_);
    view_ = std::move(found);
    return changed;
}

import_result store::import_file(const fs::path& path)
{
    import_result result;
    // A file too large to be a licence is refused as not valid, unread.
    std::error_code failed;
    if (const std::uintmax_t size = fs::file_size(path, failed);
        !failed && size > signed_file::max_file_size)
    {
        result.outcome = import_outcome::not_valid;
        result.status.state = state::invalid;
        result.status.reason = reason::malformed;
        result.detail = make_error_code(errc::too_large).message();
        return result;
    }
    const auto bytes = read_signed_file(path);
    if (!bytes)
    {
        result.outcome = import_outcome::unreadable;
        return result;
    }

    load_revocations();
    const time_point now = options_.clock();
    result.status = evaluate(*bytes, judging_context(now));

    // Only a file whose signature verified has a value, and of those a
    // withdrawn licence or one for other computers is refused as well.
    if (!result.status.value || result.status.state == state::invalid)
    {
        result.outcome = import_outcome::not_valid;
        const auto envelope = signed_file::parse(*bytes);
        if (!envelope)
        {
            result.detail = envelope.error().message();
        }
        else
        {
            const auto key =
                signed_file::verify(*envelope, licence_purpose, options_.keys);
            if (!key)
                result.detail = key.error().message();
        }
        return result;
    }

    // The same bytes in either folder: nothing to do.
    for (const fs::path& directory :
         {options_.machine_directory, options_.user_directory})
    {
        for (const fs::path& installed : files_in(directory, licence_suffix))
        {
            const auto other = read_signed_file(installed);
            if (other && *other == *bytes)
            {
                result.outcome = import_outcome::already_installed;
                return result;
            }
        }
    }

    if (options_.user_directory.empty())
    {
        result.outcome = import_outcome::unreadable;
        return result;
    }
    fs::create_directories(options_.user_directory, failed);
    if (failed || !fs::is_directory(options_.user_directory, failed))
    {
        result.outcome = import_outcome::unreadable;
        return result;
    }

    // A new name each time: importing never replaces an older file. A link
    // that points nowhere counts as a file here, so it is not written through.
    const std::string base = safe_name(result.status.value->licence_id);
    fs::path target;
    for (std::size_t number = 1; number <= max_files_per_folder; ++number)
    {
        std::string name = base;
        if (number > 1)
            name += '-' + std::to_string(number);
        name += '.';
        name += licence_suffix;
        const fs::path candidate = options_.user_directory / name;
        if (!fs::exists(fs::symlink_status(candidate, failed)))
        {
            target = candidate;
            break;
        }
    }
    if (target.empty() || !write_whole_file(target, *bytes))
    {
        result.outcome = import_outcome::unreadable;
        return result;
    }

    // A verified licence may carry a token that sets the stored time back,
    // once, for a computer whose clock ran ahead.
    const clock_state reset = apply_reset(clock_, *result.status.value);
    if (reset != clock_)
    {
        clock_ = reset;
        save_clock();
        result.status = evaluate(*bytes, judging_context(now));
    }

    result.outcome = import_outcome::imported;
    refresh();
    return result;
}

bool store::remove(const std::string& file_name)
{
    // Only a file this store listed in the user's folder: the name is
    // matched against the folder's own listing and never used as a path.
    bool removed = false;
    for (const fs::path& path :
         files_in(options_.user_directory, licence_suffix))
    {
        if (utf8_of(path.filename()) != file_name)
            continue;
        std::error_code failed;
        if (fs::remove(path, failed))
            removed = true;
    }
    if (removed)
        refresh();
    return removed;
}

} // namespace vss::license
