//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/wire_time.hpp>

namespace vss
{

std::string to_text(const wire_framing& framing)
{
    std::string out;
    out.push_back(static_cast<char>('0' + (framing.data_bits % 10)));
    switch (framing.parity)
    {
    case parity_mode::none: out.push_back('N'); break;
    case parity_mode::odd: out.push_back('O'); break;
    case parity_mode::even: out.push_back('E'); break;
    case parity_mode::mark: out.push_back('M'); break;
    case parity_mode::space: out.push_back('S'); break;
    }
    switch (framing.stop_bits)
    {
    case stop_bits_mode::one: out.push_back('1'); break;
    case stop_bits_mode::one_point_five: out.append("1.5"); break;
    case stop_bits_mode::two: out.push_back('2'); break;
    }
    return out;
}

std::optional<wire_framing> parse_framing(std::string_view text)
{
    if (text.size() < 3)
        return std::nullopt;
    wire_framing out;
    if (text[0] < '5' || text[0] > '8')
        return std::nullopt;
    out.data_bits = static_cast<unsigned>(text[0] - '0');
    switch (text[1])
    {
    case 'N': case 'n': out.parity = parity_mode::none; break;
    case 'O': case 'o': out.parity = parity_mode::odd; break;
    case 'E': case 'e': out.parity = parity_mode::even; break;
    case 'M': case 'm': out.parity = parity_mode::mark; break;
    case 'S': case 's': out.parity = parity_mode::space; break;
    default: return std::nullopt;
    }
    const std::string_view stop = text.substr(2);
    if (stop == "1")
        out.stop_bits = stop_bits_mode::one;
    else if (stop == "1.5")
        out.stop_bits = stop_bits_mode::one_point_five;
    else if (stop == "2")
        out.stop_bits = stop_bits_mode::two;
    else
        return std::nullopt;
    return out;
}

} // namespace vss
