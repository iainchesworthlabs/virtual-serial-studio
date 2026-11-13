//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/junit.hpp>

#include <array>
#include <cstddef>

namespace vss::control
{
namespace
{

/// The code point of the UTF-8 sequence at `text[index]` and its length, or
/// a length of 0 when the bytes there are not one.
struct decoded
{
    std::uint32_t point = 0;
    std::size_t length = 0;
};

decoded decode_utf8(std::string_view text, std::size_t index) noexcept
{
    const auto at = [&](std::size_t offset)
    { return static_cast<unsigned char>(text[index + offset]); };
    const unsigned char lead = at(0);
    if (lead < 0x80)
        return {lead, 1};
    decoded out;
    std::uint32_t least = 0;
    if ((lead & 0xe0) == 0xc0)
    {
        out.length = 2;
        out.point = lead & 0x1fu;
        least = 0x80;
    }
    else if ((lead & 0xf0) == 0xe0)
    {
        out.length = 3;
        out.point = lead & 0x0fu;
        least = 0x800;
    }
    else if ((lead & 0xf8) == 0xf0)
    {
        out.length = 4;
        out.point = lead & 0x07u;
        least = 0x10000;
    }
    else
    {
        return {};
    }
    if (text.size() - index < out.length)
        return {};
    for (std::size_t offset = 1; offset < out.length; ++offset)
    {
        const unsigned char next = at(offset);
        if ((next & 0xc0) != 0x80)
            return {};
        out.point = (out.point << 6) | (next & 0x3fu);
    }
    if (out.point < least || out.point > 0x10ffff ||
        (out.point >= 0xd800 && out.point <= 0xdfff))
        return {};
    return out;
}

/// The characters XML 1.0 allows in a document.
bool allowed_in_xml(std::uint32_t point) noexcept
{
    return point == 0x9 || point == 0xa || point == 0xd ||
           (point >= 0x20 && point <= 0xd7ff) ||
           (point >= 0xe000 && point <= 0xfffd) ||
           (point >= 0x10000 && point <= 0x10ffff);
}

void append_number(std::string& out, std::uint64_t value)
{
    out += std::to_string(value);
}

/// Milliseconds as seconds with three decimals: `1.250`.
std::string seconds_text(std::uint64_t milliseconds)
{
    std::string out = std::to_string(milliseconds / 1000);
    const std::string part = std::to_string(milliseconds % 1000);
    out.push_back('.');
    out.append(3 - part.size(), '0');
    out += part;
    return out;
}

void append_two(std::string& out, std::uint64_t value)
{
    out.push_back(static_cast<char>('0' + (value / 10) % 10));
    out.push_back(static_cast<char>('0' + value % 10));
}

void append_attribute(std::string& out, std::string_view name,
                      std::string_view value)
{
    out.push_back(' ');
    out += name;
    out += "=\"";
    out += xml_text(value);
    out.push_back('"');
}

} // namespace

std::string_view outcome_name(case_outcome value) noexcept
{
    switch (value)
    {
    case case_outcome::passed:
        return "passed";
    case case_outcome::failed:
        return "failed";
    case case_outcome::error:
        return "error";
    case case_outcome::skipped:
        return "skipped";
    }
    return "error";
}

std::string xml_text(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size())
    {
        const decoded next = decode_utf8(text, index);
        if (next.length == 0)
        {
            out.push_back('?');
            ++index;
            continue;
        }
        const std::string_view piece = text.substr(index, next.length);
        index += next.length;
        if (!allowed_in_xml(next.point))
            continue;
        switch (next.point)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        // A line end or a tab in an attribute would be read back as a
        // space; as a reference it is read back as itself.
        case 0x9:
            out += "&#9;";
            break;
        case 0xa:
            out += "&#10;";
            break;
        case 0xd:
            out += "&#13;";
            break;
        default:
            out += piece;
            break;
        }
    }
    return out;
}

std::string iso_time(std::uint64_t milliseconds_since_1970)
{
    const std::uint64_t seconds = milliseconds_since_1970 / 1000;
    const std::uint64_t in_day = seconds % 86400;
    // Days to a date of the Gregorian calendar, without the C library's
    // time functions, which differ between the systems this builds on.
    const std::uint64_t z = seconds / 86400 + 719468;
    const std::uint64_t era = z / 146097;
    const std::uint64_t day_of_era = z - era * 146097;
    const std::uint64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
         day_of_era / 146096) /
        365;
    const std::uint64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::uint64_t shifted_month = (5 * day_of_year + 2) / 153;
    const std::uint64_t day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    const std::uint64_t month =
        shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    const std::uint64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);

    std::string out;
    const std::string digits = std::to_string(year);
    if (digits.size() < 4)
        out.append(4 - digits.size(), '0');
    out += digits;
    out.push_back('-');
    append_two(out, month);
    out.push_back('-');
    append_two(out, day);
    out.push_back('T');
    append_two(out, in_day / 3600);
    out.push_back(':');
    append_two(out, (in_day / 60) % 60);
    out.push_back(':');
    append_two(out, in_day % 60);
    out.push_back('Z');
    return out;
}

std::string write_junit(const junit_suite& suite)
{
    std::array<std::uint64_t, 4> counts{};
    for (const junit_case& one : suite.cases)
        ++counts[static_cast<std::size_t>(one.outcome)];
    const auto count_of = [&](case_outcome which)
    { return counts[static_cast<std::size_t>(which)]; };

    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<testsuites>\n  <testsuite";
    append_attribute(out, "name", suite.name);
    out += " tests=\"";
    append_number(out, suite.cases.size());
    out += "\" failures=\"";
    append_number(out, count_of(case_outcome::failed));
    out += "\" errors=\"";
    append_number(out, count_of(case_outcome::error));
    out += "\" skipped=\"";
    append_number(out, count_of(case_outcome::skipped));
    out.push_back('"');
    append_attribute(out, "time", seconds_text(suite.duration_ms));
    append_attribute(out, "timestamp", iso_time(suite.started_ms));
    out += ">\n";
    for (const junit_case& one : suite.cases)
    {
        out += "    <testcase";
        append_attribute(out, "name", one.name);
        append_attribute(out, "classname", suite.name);
        append_attribute(out, "time", seconds_text(one.duration_ms));
        if (one.outcome == case_outcome::passed)
        {
            out += "/>\n";
            continue;
        }
        out += ">\n      <";
        const std::string_view element =
            one.outcome == case_outcome::failed  ? "failure"
            : one.outcome == case_outcome::error ? "error"
                                                 : "skipped";
        out += element;
        append_attribute(out, "message", one.message);
        out += "/>\n    </testcase>\n";
    }
    out += "  </testsuite>\n</testsuites>\n";
    return out;
}

} // namespace vss::control
