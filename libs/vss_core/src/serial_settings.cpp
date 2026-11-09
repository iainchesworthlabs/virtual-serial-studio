//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/serial_settings.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace vss {
namespace {

std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return out;
}

double stop_bit_count(stop_bits_mode mode) noexcept
{
    switch (mode)
    {
    case stop_bits_mode::one: return 1.0;
    case stop_bits_mode::one_point_five: return 1.5;
    case stop_bits_mode::two: return 2.0;
    }
    return 1.0;
}

char parity_letter(parity_mode mode) noexcept
{
    switch (mode)
    {
    case parity_mode::none: return 'N';
    case parity_mode::odd: return 'O';
    case parity_mode::even: return 'E';
    case parity_mode::mark: return 'M';
    case parity_mode::space: return 'S';
    }
    return 'N';
}

} // namespace

double serial_settings::characters_per_second() const noexcept
{
    if (baud_rate == 0)
        return 0.0;
    const double framing = 1.0 + static_cast<double>(data_bits)
        + (parity == parity_mode::none ? 0.0 : 1.0) + stop_bit_count(stop_bits);
    return static_cast<double>(baud_rate) / framing;
}

std::chrono::nanoseconds serial_settings::character_duration() const noexcept
{
    const double rate = characters_per_second();
    if (rate <= 0.0)
        return std::chrono::nanoseconds{0};
    return std::chrono::nanoseconds(
        static_cast<std::int64_t>(std::llround(1'000'000'000.0 / rate)));
}

error_code validate(const serial_settings& settings) noexcept
{
    if (settings.baud_rate < 50 || settings.baud_rate > 40'000'000)
        return make_error_code(errc::invalid_argument);
    if (settings.data_bits < 5 || settings.data_bits > 8)
        return make_error_code(errc::invalid_argument);
    // Mark and space parity are accepted but rare; the virtual ports model
    // them and physical UARTs may round them.
    if (settings.read_timeout.count() < 0 || settings.write_timeout.count() < 0)
        return make_error_code(errc::invalid_argument);
    return {};
}

const char* to_string(parity_mode value) noexcept
{
    switch (value)
    {
    case parity_mode::none: return "none";
    case parity_mode::odd: return "odd";
    case parity_mode::even: return "even";
    case parity_mode::mark: return "mark";
    case parity_mode::space: return "space";
    }
    return "none";
}

const char* to_string(stop_bits_mode value) noexcept
{
    switch (value)
    {
    case stop_bits_mode::one: return "one";
    case stop_bits_mode::one_point_five: return "one-point-five";
    case stop_bits_mode::two: return "two";
    }
    return "one";
}

const char* to_string(flow_control_mode value) noexcept
{
    switch (value)
    {
    case flow_control_mode::none: return "none";
    case flow_control_mode::software: return "software";
    case flow_control_mode::hardware: return "hardware";
    }
    return "none";
}

result<parity_mode> parse_parity(std::string_view text)
{
    const std::string value = lower(text);
    if (value == "none" || value == "n" || value == "off")
        return parity_mode::none;
    if (value == "odd" || value == "o")
        return parity_mode::odd;
    if (value == "even" || value == "e")
        return parity_mode::even;
    if (value == "mark" || value == "m")
        return parity_mode::mark;
    if (value == "space" || value == "s")
        return parity_mode::space;
    return make_error_code(errc::invalid_argument);
}

result<stop_bits_mode> parse_stop_bits(std::string_view text)
{
    const std::string value = lower(text);
    if (value == "1" || value == "one")
        return stop_bits_mode::one;
    if (value == "1.5" || value == "one-point-five" || value == "onepointfive")
        return stop_bits_mode::one_point_five;
    if (value == "2" || value == "two")
        return stop_bits_mode::two;
    return make_error_code(errc::invalid_argument);
}

result<flow_control_mode> parse_flow_control(std::string_view text)
{
    const std::string value = lower(text);
    if (value == "none" || value == "off")
        return flow_control_mode::none;
    if (value == "software" || value == "xonxoff" || value == "sw")
        return flow_control_mode::software;
    if (value == "hardware" || value == "rtscts" || value == "hw")
        return flow_control_mode::hardware;
    return make_error_code(errc::invalid_argument);
}

std::string to_label(const line_format& format)
{
    std::string out = std::to_string(format.baud_rate);
    if (!format.framing_known)
        return out;
    out.push_back(' ');
    out.push_back(static_cast<char>('0' + (format.data_bits % 10)));
    out.push_back(parity_letter(format.parity));
    switch (format.stop_bits)
    {
    case stop_bits_mode::one: out.push_back('1'); break;
    case stop_bits_mode::one_point_five: out.append("1.5"); break;
    case stop_bits_mode::two: out.push_back('2'); break;
    }
    return out;
}

std::string to_label(const serial_settings& settings)
{
    return to_label(format_of(settings));
}

} // namespace vss
