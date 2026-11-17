//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/event.hpp>
#include <vss/core/serial_settings.hpp>
#include <vss/core/types.hpp>

#include "test_framework.hpp"

using namespace vss;

TEST_CASE("settings: 115200 8N1 maths")
{
    serial_settings settings;
    REQUIRE_EQ(settings.baud_rate, 115200u);
    REQUIRE_EQ(settings.data_bits, 8u);

    // 10 bits per character: start + 8 data + stop.
    REQUIRE_NEAR(settings.characters_per_second(), 11520.0, 0.001);
    REQUIRE_EQ(settings.character_duration().count(), std::int64_t{86806});
}

TEST_CASE("settings: framing changes the character rate")
{
    serial_settings settings;
    settings.baud_rate = 9600;
    settings.parity = parity_mode::even;
    settings.stop_bits = stop_bits_mode::two;
    // 1 start + 8 data + 1 parity + 2 stop = 12 bits.
    REQUIRE_NEAR(settings.characters_per_second(), 800.0, 0.001);

    serial_settings fast;
    fast.baud_rate = 1'000'000;
    fast.stop_bits = stop_bits_mode::two;
    // 1 start + 8 data + 2 stop = 11 bits.
    REQUIRE_NEAR(fast.characters_per_second(), 90909.09, 0.01);
}

TEST_CASE("settings: zero baud is safe")
{
    serial_settings settings;
    settings.baud_rate = 0;
    REQUIRE_NEAR(settings.characters_per_second(), 0.0, 0.0);
    REQUIRE_EQ(settings.character_duration().count(), std::int64_t{0});
}

TEST_CASE("settings: validation bounds")
{
    serial_settings settings;
    REQUIRE(!validate(settings));

    settings.baud_rate = 49;
    REQUIRE_EQ(validate(settings), make_error_code(errc::invalid_argument));

    settings.baud_rate = 115200;
    settings.data_bits = 4;
    REQUIRE_EQ(validate(settings), make_error_code(errc::invalid_argument));

    settings.data_bits = 9;
    REQUIRE_EQ(validate(settings), make_error_code(errc::invalid_argument));

    settings.data_bits = 5;
    REQUIRE(!validate(settings));

    settings.data_bits = 8;
    settings.read_timeout = std::chrono::milliseconds(-1);
    REQUIRE_EQ(validate(settings), make_error_code(errc::invalid_argument));

    settings.read_timeout = std::chrono::milliseconds(10);
    settings.baud_rate = 12'000'000;
    REQUIRE(!validate(settings));
    settings.baud_rate = 40'000'000;
    REQUIRE(!validate(settings));
    settings.baud_rate = 40'000'001;
    REQUIRE_EQ(validate(settings), make_error_code(errc::invalid_argument));
}

TEST_CASE("settings: enum text roundtrip")
{
    for (parity_mode value : {parity_mode::none, parity_mode::odd,
                              parity_mode::even, parity_mode::mark,
                              parity_mode::space})
    {
        REQUIRE_EQ(*parse_parity(to_string(value)), value);
    }

    for (stop_bits_mode value : {stop_bits_mode::one, stop_bits_mode::one_point_five,
                                 stop_bits_mode::two})
    {
        REQUIRE_EQ(*parse_stop_bits(to_string(value)), value);
    }

    for (flow_control_mode value : {flow_control_mode::none,
                                    flow_control_mode::software,
                                    flow_control_mode::hardware})
    {
        REQUIRE_EQ(*parse_flow_control(to_string(value)), value);
    }
}

TEST_CASE("settings: short aliases and invalid names")
{
    REQUIRE_EQ(*parse_parity("N"), parity_mode::none);
    REQUIRE_EQ(*parse_parity("o"), parity_mode::odd);
    REQUIRE_EQ(*parse_parity("E"), parity_mode::even);
    REQUIRE_EQ(*parse_stop_bits("1.5"), stop_bits_mode::one_point_five);
    REQUIRE_EQ(*parse_stop_bits("2"), stop_bits_mode::two);
    REQUIRE_EQ(*parse_flow_control("rtscts"), flow_control_mode::hardware);
    REQUIRE_EQ(*parse_flow_control("xonxoff"), flow_control_mode::software);

    REQUIRE(!parse_parity("banana"));
    REQUIRE(!parse_stop_bits("3"));
    REQUIRE(!parse_flow_control("magic"));
}

TEST_CASE("settings: every documented alias is accepted")
{
    REQUIRE_EQ(*parse_parity("m"), parity_mode::mark);
    REQUIRE_EQ(*parse_parity("s"), parity_mode::space);
    REQUIRE_EQ(*parse_parity("off"), parity_mode::none);
    REQUIRE_EQ(*parse_stop_bits("one"), stop_bits_mode::one);
    REQUIRE_EQ(*parse_stop_bits("one-point-five"), stop_bits_mode::one_point_five);
    REQUIRE_EQ(*parse_stop_bits("onepointfive"), stop_bits_mode::one_point_five);
    REQUIRE_EQ(*parse_stop_bits("two"), stop_bits_mode::two);
    REQUIRE_EQ(*parse_flow_control("off"), flow_control_mode::none);
    REQUIRE_EQ(*parse_flow_control("sw"), flow_control_mode::software);
    REQUIRE_EQ(*parse_flow_control("hw"), flow_control_mode::hardware);
}

TEST_CASE("settings: label")
{
    serial_settings settings;
    REQUIRE_EQ(to_label(settings), std::string("115200 8N1"));

    settings.baud_rate = 9600;
    settings.parity = parity_mode::even;
    settings.stop_bits = stop_bits_mode::one_point_five;
    REQUIRE_EQ(to_label(settings), std::string("9600 8E1.5"));
}

TEST_CASE("types: enum names")
{
    REQUIRE_EQ(std::string(to_string(endpoint_kind::virtual_port)), std::string("virtual"));
    REQUIRE_EQ(std::string(to_string(endpoint_kind::physical)), std::string("physical"));
    REQUIRE_EQ(std::string(to_string(endpoint_kind::tcp)), std::string("tcp"));
    REQUIRE_EQ(std::string(to_string(endpoint_kind::udp)), std::string("udp"));
    REQUIRE_EQ(std::string(to_string(endpoint_kind::rfc2217)), std::string("rfc2217"));
    REQUIRE_EQ(std::string(to_string(endpoint_kind::mock)), std::string("mock"));
    REQUIRE_EQ(std::string(to_string(endpoint_kind::mock)), std::string("mock"));

    REQUIRE_EQ(std::string(to_string(endpoint_state::created)), std::string("created"));
    REQUIRE_EQ(std::string(to_string(endpoint_state::opening)), std::string("opening"));
    REQUIRE_EQ(std::string(to_string(endpoint_state::open)), std::string("open"));
    REQUIRE_EQ(std::string(to_string(endpoint_state::closed)), std::string("closed"));
    REQUIRE_EQ(std::string(to_string(endpoint_state::failed)), std::string("failed"));

    REQUIRE_EQ(std::string(to_string(link_mode::duplex)), std::string("duplex"));
    REQUIRE_EQ(std::string(to_string(link_mode::one_way)), std::string("one-way"));

    REQUIRE_EQ(std::string(to_string(merge_policy::interleave)), std::string("interleave"));
    REQUIRE_EQ(std::string(to_string(merge_policy::owner)), std::string("owner"));

    REQUIRE_EQ(std::string(to_string(data_direction::rx)), std::string("rx"));
    REQUIRE_EQ(std::string(to_string(data_direction::tx)), std::string("tx"));

    REQUIRE_EQ(std::string(to_string(event_kind::started)), std::string("started"));
    REQUIRE_EQ(std::string(to_string(event_kind::stopped)), std::string("stopped"));
    REQUIRE_EQ(std::string(to_string(event_kind::data)), std::string("data"));
    REQUIRE_EQ(std::string(to_string(event_kind::lines)), std::string("lines"));
    REQUIRE_EQ(std::string(to_string(event_kind::error)), std::string("error"));
    REQUIRE_EQ(std::string(to_string(event_kind::stats)), std::string("stats"));
    REQUIRE_EQ(std::string(to_string(event_kind::log_message)), std::string("log"));
}
