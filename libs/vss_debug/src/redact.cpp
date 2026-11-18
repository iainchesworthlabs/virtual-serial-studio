//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/debug/redact.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <regex>

namespace vss::debug
{
namespace
{

constexpr std::size_t shortest_literal = 3;

char lower(char letter) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
}

/// The path separators are alike for this comparison: a Windows path may
/// have been written with either.
bool same_letter(char a, char b) noexcept
{
    if ((a == '\\' || a == '/') && (b == '\\' || b == '/'))
        return true;
    return lower(a) == lower(b);
}

std::string replace_literal(std::string text, const std::string& needle,
                            const std::string& replacement)
{
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size())
    {
        if (at + needle.size() <= text.size() &&
            std::equal(needle.begin(), needle.end(), text.begin() + at,
                       same_letter))
        {
            out += replacement;
            at += needle.size();
        }
        else
        {
            out += text[at];
            ++at;
        }
    }
    return out;
}

const std::regex& url_password()
{
    static const std::regex pattern(
        R"(([A-Za-z][A-Za-z0-9+.\-]*://)[^/\s:@]+:[^/\s@]+@)");
    return pattern;
}

const std::regex& named_secret()
{
    static const std::regex pattern(
        R"(((?:password|passwd|token|secret|api[_-]?key|apikey)\s*[=:]\s*)[^\s,;"'&]+)",
        std::regex::icase);
    return pattern;
}

const std::regex& bearer()
{
    static const std::regex pattern(R"((bearer\s+)[A-Za-z0-9._~+/\-]+=*)",
                                    std::regex::icase);
    return pattern;
}

/// A device serial number: `SER=` or `SERIAL=` and the like in a hardware
/// path, and the part after the last `\` of a USB instance id.
const std::regex& serial_number()
{
    static const std::regex pattern(
        R"(\b((?:serial(?:number)?|ser)\s*[=:]\s*)[A-Za-z0-9]+)",
        std::regex::icase);
    return pattern;
}

} // namespace

void redactor::add_literal(std::string needle, std::string replacement)
{
    if (needle.size() < shortest_literal)
        return;
    literals_.push_back({std::move(needle), std::move(replacement)});
    // Longest first, so a folder is replaced before a name inside it.
    std::ranges::stable_sort(literals_, [](const literal& a, const literal& b)
                             { return a.needle.size() > b.needle.size(); });
}

redactor redactor::for_user(std::string_view home, std::string_view user,
                            std::string_view computer)
{
    redactor out;
    out.add_literal(std::string(home), "<home>");
    out.add_literal(std::string(user), "<user>");
    out.add_literal(std::string(computer), "<computer>");
    return out;
}

std::string redactor::apply(std::string_view text) const
{
    std::string out(text);
    for (const literal& item : literals_)
        out = replace_literal(std::move(out), item.needle, item.replacement);
    out = std::regex_replace(out, url_password(), "$1<redacted>@");
    out = std::regex_replace(out, named_secret(), "$1<redacted>");
    out = std::regex_replace(out, bearer(), "$1<redacted>");
    out = std::regex_replace(out, serial_number(), "$1<redacted>");
    return out;
}

} // namespace vss::debug
