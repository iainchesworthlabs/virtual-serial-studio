//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/access.hpp>

#include <vss/plugin/error.hpp>
#include <vss/plugin/measure.hpp>

#include "platform/access_platform.hpp"

#include <cstddef>
#include <exception>
#include <system_error>
#include <utility>

namespace vss::plugin
{

namespace
{

namespace fs = std::filesystem;

void set_detail(std::string* explain, std::string text)
{
    if (explain != nullptr)
        *explain = std::move(text);
}

/// The path in quotes, for a message to a person: cut short, and with
/// anything that is not plain ASCII replaced.
std::string shown(const fs::path& path)
{
    constexpr std::size_t longest = 240;
    const std::u8string text = path.generic_u8string();
    std::string out = "\"";
    for (std::size_t i = 0; i < text.size() && i < longest; ++i)
    {
        const auto c = static_cast<char>(text[i]);
        out += c >= 0x20 && c <= 0x7e && c != '"' ? c : '?';
    }
    if (text.size() > longest)
        out += "...";
    out += '"';
    return out;
}

result<write_access> examine_all(const fs::path& path, std::string* explain)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found)
    {
        set_detail(explain, shown(path) + " is not there");
        return make_error_code(errc::not_found);
    }
    // Links are resolved here, once, so that what is examined is what a
    // program that opens the path would get.
    const fs::path resolved = fs::canonical(path, ec);
    if (ec)
    {
        set_detail(explain, shown(path) + " cannot be examined");
        return make_error_code(errc::io_error);
    }

    // From the root down: the root, then each folder, then the path itself.
    fs::path part = resolved.root_path();
    const fs::path below = resolved.relative_path();
    auto name = below.begin();
    while (true)
    {
        const bool last = name == below.end();
        const result<platform::access_finding> found =
            platform::examine(part, last);
        if (found.has_error())
        {
            set_detail(explain, shown(part) + " cannot be examined");
            return make_error_code(errc::io_error);
        }
        if (found->others)
        {
            std::string text = shown(part);
            text += ' ';
            text += found->why;
            set_detail(explain, std::move(text));
            return write_access::others_too;
        }
        if (last)
            break;
        part /= *name;
        ++name;
    }
    set_detail(explain, std::string{});
    return write_access::administrators_only;
}

loaded_policy not_used(error_code problem, std::string text)
{
    loaded_policy out;
    out.rules = official_only_policy();
    out.problem = problem;
    out.detail = std::move(text);
    return out;
}

loaded_policy read_policy(const fs::path& file)
{
    std::string text;
    const result<std::string> bytes =
        read_plugin_file(file, max_policy_size, &text);
    if (bytes.has_error())
    {
        if (bytes.error() == plugin_errc::file_missing)
            return loaded_policy{};
        if (bytes.error() == plugin_errc::manifest_too_large)
            return not_used(make_error_code(errc::config_error),
                            std::move(text));
        return not_used(bytes.error(), std::move(text));
    }

    // Who could have written it is asked before what it says.
    const result<write_access> access = who_can_write(file, &text);
    if (access.has_error())
        return not_used(make_error_code(errc::io_error), std::move(text));
    if (*access != write_access::administrators_only)
    {
        std::string why = "the policy file can be changed by someone who is "
                          "not an administrator: ";
        why += text;
        return not_used(make_error_code(errc::permission_denied),
                        std::move(why));
    }

    result<policy> parsed = parse_policy(*bytes, &text);
    if (parsed.has_error())
        return not_used(parsed.error(), std::move(text));
    loaded_policy out;
    out.rules = std::move(*parsed);
    return out;
}

} // namespace

std::string_view to_string(write_access value) noexcept
{
    return value == write_access::administrators_only
               ? "administrators_only"
               : "others_too";
}

result<write_access> who_can_write(const std::filesystem::path& path,
                                   std::string* explain)
{
    // The calls above report through error codes; only running out of
    // memory or a name the library cannot convert can throw.
    try
    {
        return examine_all(path, explain);
    }
    catch (const std::exception&)
    {
        set_detail(explain, "the path cannot be examined");
        return make_error_code(errc::io_error);
    }
}

result<install_kind>
kind_of_install(const std::filesystem::path& program_directory,
                std::string* explain)
{
    const result<write_access> access =
        who_can_write(program_directory, explain);
    if (access.has_error())
        return access.error();
    return *access == write_access::administrators_only
               ? install_kind::for_everyone
               : install_kind::for_one_user;
}

loaded_policy load_policy(const std::filesystem::path& file)
{
    try
    {
        return read_policy(file);
    }
    catch (const std::exception&)
    {
        return not_used(make_error_code(errc::io_error),
                        "the policy file cannot be read");
    }
}

} // namespace vss::plugin
