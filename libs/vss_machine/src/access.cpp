//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/machine/access.hpp>

#include "platform/machine_platform.hpp"

#include <boost/system/errc.hpp>

#include <cstddef>
#include <exception>
#include <system_error>
#include <utility>

namespace vss::machine
{

namespace
{

namespace fs = std::filesystem;

boost::system::error_code not_found_code()
{
    return boost::system::errc::make_error_code(
        boost::system::errc::no_such_file_or_directory);
}

boost::system::error_code io_error_code()
{
    return boost::system::errc::make_error_code(boost::system::errc::io_error);
}

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

boost::system::result<write_access> examine_all(const fs::path& path,
                                                std::string* explain)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found)
    {
        set_detail(explain, shown(path) + " is not there");
        return not_found_code();
    }
    // Links are resolved here, once, so that what is examined is what a
    // program that opens the path would get.
    const fs::path resolved = fs::canonical(path, ec);
    if (ec)
    {
        set_detail(explain, shown(path) + " cannot be examined");
        return io_error_code();
    }

    // From the root down: the root, then each folder, then the path itself.
    fs::path part = resolved.root_path();
    const fs::path below = resolved.relative_path();
    auto name = below.begin();
    while (true)
    {
        const bool last = name == below.end();
        const boost::system::result<platform::access_finding> found =
            platform::examine(part, last);
        if (found.has_error())
        {
            set_detail(explain, shown(part) + " cannot be examined");
            return io_error_code();
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

} // namespace

std::string_view to_string(write_access value) noexcept
{
    return value == write_access::administrators_only ? "administrators_only"
                                                      : "others_too";
}

boost::system::result<write_access> who_can_write(const fs::path& path,
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
        try
        {
            set_detail(explain, "the path cannot be examined");
        }
        catch (const std::exception&)
        {
            // Out of memory again: the answer is still the error, with
            // no detail to go with it.
            if (explain != nullptr)
                explain->clear();
        }
        return io_error_code();
    }
}

} // namespace vss::machine
