//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/access.hpp>

#include <vss/machine/access.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/measure.hpp>

#include <boost/system/errc.hpp>

#include <exception>
#include <utility>

namespace vss::plugin
{

namespace
{

namespace fs = std::filesystem;

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

result<write_access> who_can_write(const std::filesystem::path& path,
                                   std::string* explain)
{
    const boost::system::result<machine::write_access> access =
        machine::who_can_write(path, explain);
    if (access.has_value())
        return *access;
    if (access.error() == boost::system::errc::no_such_file_or_directory)
        return make_error_code(errc::not_found);
    return make_error_code(errc::io_error);
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
